/*
 * decoder.c - Hilo PRODUCTOR: decodifica pistas y llena el ring buffer.
 */
#include "decoder.h"
#include "evlog.h"
#include "source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------- Selección de formato ---------------- */

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    return (dot && (!slash || dot > slash)) ? dot + 1 : "";
}

bool source_is_supported(const char *path)
{
    const char *e = ext_of(path);
    return !strcasecmp(e, "wav") || !strcasecmp(e, "wave") ||
           !strcasecmp(e, "mp3");
}

source_t *source_open(const char *path, char *err, size_t errlen)
{
    const char *e = ext_of(path);
    if (!strcasecmp(e, "wav") || !strcasecmp(e, "wave"))
        return wav_open(path, err, errlen);
    if (!strcasecmp(e, "mp3"))
        return mp3_open(path, err, errlen);
    /* Sin extensión conocida: intentar WAV y luego MP3. */
    source_t *s = wav_open(path, err, errlen);
    return s ? s : mp3_open(path, err, errlen);
}

void source_close(source_t *s)
{
    if (s)
        s->ops->close(s);
}

/* ---------------- Peticiones ---------------- */

void decoder_request(decoder_t *d, uint64_t gen, const track_info_t *t,
                     size_t index, uint64_t seek_frame)
{
    pthread_mutex_lock(&d->lock);
    d->req_gen = gen;
    d->req_track = *t;
    d->req_index = index;
    d->req_seek = seek_frame;
    d->has_req = true;            /* una petición nueva reemplaza a la vieja */
    pthread_cond_signal(&d->req_cond);
    pthread_mutex_unlock(&d->lock);
}

bool decoder_get_meta(decoder_t *d, uint64_t track_id, track_meta_t *out)
{
    bool found = false;
    pthread_mutex_lock(&d->lock);
    for (size_t i = 0; i < META_CACHE; i++) {
        if (d->meta[i].track_id == track_id && track_id) {
            *out = d->meta[i];
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return found;
}

static void publish_meta(decoder_t *d, uint64_t id, const source_t *s)
{
    track_meta_t m = { .track_id = id, .rate = s->rate,
                       .src_channels = s->src_channels, .bits = s->bits,
                       .total_frames = s->total_frames };
    snprintf(m.codec, sizeof m.codec, "%s", s->codec);
    snprintf(m.title, sizeof m.title, "%s", s->title);
    snprintf(m.artist, sizeof m.artist, "%s", s->artist);

    pthread_mutex_lock(&d->lock);
    d->meta[d->meta_next] = m;
    d->meta_next = (d->meta_next + 1) % META_CACHE;
    pthread_mutex_unlock(&d->lock);
}

/* Espera una petición. Devuelve false si hay que terminar. */
static bool wait_request(decoder_t *d, uint64_t *gen, track_info_t *t,
                         size_t *index, uint64_t *seek)
{
    pthread_mutex_lock(&d->lock);
    while (!d->has_req && !d->shutdown) {
        atomic_store(&d->state, TS_WAIT_REQUEST);
        pthread_cond_wait(&d->req_cond, &d->lock);
    }
    bool ok = !d->shutdown;
    if (ok) {
        *gen = d->req_gen;
        *t = d->req_track;
        *index = d->req_index;
        *seek = d->req_seek;
        d->has_req = false;
    }
    pthread_mutex_unlock(&d->lock);
    atomic_store(&d->state, TS_RUNNING);
    return ok;
}

/* Envía un chunk EOS ("no hay más pistas"). */
static rb_result_t push_eos(decoder_t *d, audio_chunk_t *c, uint64_t gen)
{
    memset(c, 0, offsetof(audio_chunk_t, data));
    c->gen = gen;
    c->flags = CHUNK_EOS;
    return ringbuf_push(d->rb, c, &d->state);
}

/*
 * Decodifica `t` y las siguientes de la playlist hasta que no haya más
 * o la generación quede obsoleta.
 */
static void play_from(decoder_t *d, audio_chunk_t *c, uint64_t gen,
                      track_info_t *t, size_t index, uint64_t seek)
{
    char err[160];
    for (;;) {
        if (ringbuf_gen(d->rb) != gen)     /* ya invalidada: no abrir */
            return;
        source_t *src = source_open(t->path, err, sizeof err);
        if (!src) {
            evlog_add(EV_ERROR, "Decoder: '%s': %s", t->title, err);
        } else {
            atomic_fetch_add(&d->tracks_opened, 1);
            publish_meta(d, t->id, src);
            if (seek && source_seek(src, seek) != 0)
                seek = 0;
            evlog_add(EV_INFO, "Decoder: abre #%llu \"%s\" (%s %u Hz)",
                      (unsigned long long)t->id, t->title, src->codec,
                      src->rate);

            uint64_t pos = seek;
            uint16_t flags = CHUNK_TRACK_START;
            for (;;) {
                long n = source_read(src, c->data, CHUNK_FRAMES);
                if (n < 0)
                    evlog_add(EV_WARN, "Decoder: error de lectura en '%s'",
                              t->title);
                if (n <= 0)
                    break;
                c->gen = gen;
                c->track_id = t->id;
                c->track_index = index;
                c->pos_frames = pos;
                c->total_frames = src->total_frames;
                c->rate = src->rate;
                c->channels = 2;
                c->flags = flags;
                c->frames = (uint32_t)n;
                flags = 0;
                pos += (uint64_t)n;

                rb_result_t r = ringbuf_push(d->rb, c, &d->state);
                if (r != RB_OK) {          /* flush (Next/Stop/...) o salir */
                    source_close(src);
                    if (r == RB_STALE)
                        evlog_add(EV_SYNC, "Decoder: gen %llu obsoleta, "
                                  "abandona \"%s\"",
                                  (unsigned long long)gen, t->title);
                    return;
                }
            }
            source_close(src);
        }
        seek = 0;

        /* Preparar la siguiente pista: LECTURA concurrente de la playlist
         * (rdlock) mientras el usuario puede estar modificándola. */
        track_info_t next;
        size_t next_index;
        if (!playlist_next_after(d->pl, t->id, index, &next, &next_index)) {
            evlog_add(EV_INFO, "Decoder: fin de la lista");
            push_eos(d, c, gen);
            return;
        }
        *t = next;
        index = next_index;
    }
}

static void *decoder_main(void *arg)
{
    decoder_t *d = arg;
    audio_chunk_t *c = malloc(sizeof *c);   /* ~8 KiB: fuera de la pila */
    if (!c) {
        evlog_add(EV_ERROR, "Decoder: sin memoria");
        atomic_store(&d->state, TS_EXITED);
        return NULL;
    }
    uint64_t gen, seek;
    track_info_t t;
    size_t index;
    while (wait_request(d, &gen, &t, &index, &seek))
        play_from(d, c, gen, &t, index, seek);
    free(c);
    atomic_store(&d->state, TS_EXITED);
    return NULL;
}

int decoder_start(decoder_t *d, playlist_t *pl, ringbuf_t *rb)
{
    memset(d, 0, sizeof *d);
    d->pl = pl;
    d->rb = rb;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->req_cond, NULL);
    atomic_store(&d->state, TS_IDLE);
    return pthread_create(&d->thread, NULL, decoder_main, d);
}

void decoder_shutdown(decoder_t *d)
{
    pthread_mutex_lock(&d->lock);
    d->shutdown = true;
    pthread_cond_broadcast(&d->req_cond);
    pthread_mutex_unlock(&d->lock);
}

void decoder_join(decoder_t *d)
{
    pthread_join(d->thread, NULL);
    pthread_cond_destroy(&d->req_cond);
    pthread_mutex_destroy(&d->lock);
}
