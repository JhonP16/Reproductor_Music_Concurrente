/*
 * audio_out.c - Hilo CONSUMIDOR: ring buffer -> ALSA.
 */
#include "audio_out.h"
#include "evlog.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LATENCY_US 120000          /* búfer del dispositivo ~120 ms */

#define PCM(o) ((snd_pcm_t *)(o)->pcm)

/* ---------------- Dispositivo ---------------- */

/* alsa-lib imprime errores en stderr, lo que corrompería la pantalla
 * ncurses: se redirigen al registro de eventos. */
static void alsa_error_handler(const char *file, int line, const char *func,
                               int err, const char *fmt, ...)
{
    (void)file; (void)line; (void)err;
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    evlog_add(EV_WARN, "ALSA %s: %s", func, msg);
}

static int device_config(audio_out_t *o, uint32_t rate, uint16_t ch)
{
    if (atomic_load(&o->dry_run)) {
        o->cur_rate = rate;
        o->cur_channels = ch;
        atomic_store(&o->device_rate, rate);
        return 0;
    }
    if (o->cur_rate) {
        /* Cambio de formato entre pistas: dejar sonar lo encolado. */
        snd_pcm_drain(PCM(o));
    }
    int rc = snd_pcm_set_params(PCM(o), SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED, ch, rate,
                                1 /* resampleo por software */, LATENCY_US);
    if (rc < 0) {
        evlog_add(EV_ERROR, "AudioOut: set_params %u Hz: %s", rate,
                  snd_strerror(rc));
        return rc;
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    o->can_pause = snd_pcm_hw_params_current(PCM(o), hw) == 0 &&
                   snd_pcm_hw_params_can_pause(hw);
    o->cur_rate = rate;
    o->cur_channels = ch;
    atomic_store(&o->device_rate, rate);
    evlog_add(EV_INFO, "AudioOut: dispositivo a %u Hz, %u canales%s", rate,
              ch, o->can_pause ? "" : " (sin pausa HW)");
    return 0;
}

/* Corta inmediatamente lo que el dispositivo tenía encolado. */
static void device_drop(audio_out_t *o)
{
    if (atomic_load(&o->dry_run) || !o->cur_rate)
        return;
    snd_pcm_drop(PCM(o));
    snd_pcm_prepare(PCM(o));
}

static void device_pause(audio_out_t *o, bool on)
{
    if (atomic_load(&o->dry_run) || !o->cur_rate)
        return;
    if (o->can_pause && snd_pcm_state(PCM(o)) != SND_PCM_STATE_PREPARED) {
        if (snd_pcm_pause(PCM(o), on) == 0)
            return;
    }
    if (on)                      /* sin pausa por hardware: descartar */
        device_drop(o);
    else if (snd_pcm_state(PCM(o)) == SND_PCM_STATE_PAUSED)
        snd_pcm_pause(PCM(o), 0);
}

static void dry_sleep(uint32_t frames, uint32_t rate)
{
    uint64_t ns = (uint64_t)frames * 1000000000ull / (rate ? rate : 44100);
    struct timespec ts = { (time_t)(ns / 1000000000ull),
                           (long)(ns % 1000000000ull) };
    nanosleep(&ts, NULL);        /* reloj simulado: duerme, no gira */
}

static void device_write(audio_out_t *o, const int16_t *data, uint32_t frames)
{
    if (atomic_load(&o->dry_run)) {
        dry_sleep(frames, o->cur_rate);
        return;
    }
    while (frames > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(PCM(o), data, frames);
        if (w == -EAGAIN)
            continue;
        if (w < 0) {
            if (w == -EPIPE) {
                atomic_fetch_add(&o->underruns, 1);
                evlog_add(EV_WARN, "AudioOut: UNDERRUN del dispositivo");
            }
            if (snd_pcm_recover(PCM(o), (int)w, 1) < 0) {
                evlog_add(EV_ERROR, "AudioOut: writei: %s",
                          snd_strerror((int)w));
                return;
            }
            continue;
        }
        data += (size_t)w * o->cur_channels;
        frames -= (uint32_t)w;
    }
}

static uint64_t device_delay(audio_out_t *o)
{
    snd_pcm_sframes_t d = 0;
    if (atomic_load(&o->dry_run) || snd_pcm_delay(PCM(o), &d) < 0 || d < 0)
        return 0;
    return (uint64_t)d;
}

/* ---------------- Pausa ---------------- */

/* Bloquea mientras esté en pausa. Devuelve false si hay que terminar. */
static bool wait_unpaused(audio_out_t *o)
{
    pthread_mutex_lock(&o->ctl_lock);
    if (o->paused && !o->shutdown) {
        pthread_mutex_unlock(&o->ctl_lock);
        device_pause(o, true);            /* E/S siempre fuera del lock */
        evlog_add(EV_SYNC, "AudioOut: duerme en pause_cond");
        pthread_mutex_lock(&o->ctl_lock);
        while (o->paused && !o->shutdown) {
            atomic_store(&o->state, TS_PAUSED);
            pthread_cond_wait(&o->pause_cond, &o->ctl_lock);
        }
        pthread_mutex_unlock(&o->ctl_lock);
        device_pause(o, false);
        evlog_add(EV_SYNC, "AudioOut: despierta (reanudar)");
        pthread_mutex_lock(&o->ctl_lock);
    }
    bool ok = !o->shutdown;
    pthread_mutex_unlock(&o->ctl_lock);
    atomic_store(&o->state, TS_RUNNING);
    return ok;
}

void audio_out_set_paused(audio_out_t *o, bool paused)
{
    pthread_mutex_lock(&o->ctl_lock);
    o->paused = paused;
    pthread_cond_broadcast(&o->pause_cond);
    pthread_mutex_unlock(&o->ctl_lock);
}

/* ---------------- Publicación de estado ---------------- */

static void set_inactive(audio_out_t *o, uint64_t gen)
{
    pthread_mutex_lock(&o->status_lock);
    o->status.active = false;
    o->status.gen = gen;
    pthread_mutex_unlock(&o->status_lock);
}

static void publish(audio_out_t *o, const audio_chunk_t *c, uint64_t delay)
{
    /* Medidas de nivel y ventana mono calculadas fuera del lock. */
    float pk_l = 0, pk_r = 0;
    double sq_l = 0, sq_r = 0;
    uint32_t n = c->frames;
    for (uint32_t i = 0; i < n; i++) {
        float l = c->data[2 * i] / 32768.0f, r = c->data[2 * i + 1] / 32768.0f;
        if (fabsf(l) > pk_l) pk_l = fabsf(l);
        if (fabsf(r) > pk_r) pk_r = fabsf(r);
        sq_l += (double)l * l;
        sq_r += (double)r * r;
    }
    uint64_t end = c->pos_frames + n;
    uint64_t pos = end > delay ? end - delay : 0;
    if (pos < c->pos_frames && (c->flags & CHUNK_TRACK_START))
        pos = c->pos_frames;

    pthread_mutex_lock(&o->status_lock);
    o->status = (play_status_t){
        .gen = c->gen, .track_id = c->track_id,
        .track_index = c->track_index, .pos_frames = pos,
        .total_frames = c->total_frames, .rate = c->rate, .active = true,
    };
    viz_data_t *v = &o->viz;
    uint32_t take = n < VIZ_SAMPLES ? n : VIZ_SAMPLES;
    memmove(v->window, v->window + take,
            (VIZ_SAMPLES - take) * sizeof(float));
    for (uint32_t i = 0; i < take; i++) {
        uint32_t k = n - take + i;
        v->window[VIZ_SAMPLES - take + i] =
            (c->data[2 * k] + c->data[2 * k + 1]) / 65536.0f;
    }
    v->peak_l = pk_l;
    v->peak_r = pk_r;
    v->rms_l = n ? (float)sqrt(sq_l / n) : 0;
    v->rms_r = n ? (float)sqrt(sq_r / n) : 0;
    v->serial++;
    pthread_mutex_unlock(&o->status_lock);
}

void audio_out_get_status(audio_out_t *o, play_status_t *st)
{
    pthread_mutex_lock(&o->status_lock);
    *st = o->status;
    pthread_mutex_unlock(&o->status_lock);
}

void audio_out_get_viz(audio_out_t *o, viz_data_t *v)
{
    pthread_mutex_lock(&o->status_lock);
    *v = o->viz;
    pthread_mutex_unlock(&o->status_lock);
}

/* ---------------- Hilo ---------------- */

/*
 * Suma los efectos pendientes sobre el chunk de música. Usa
 * ringbuf_try_pop(): si el productor de efectos no tiene nada listo, NO se
 * espera (la música nunca se detiene por los efectos).
 */
static void mix_fx(audio_out_t *o, audio_chunk_t *c)
{
    if (!o->fx_rb || !o->fx_chunk)
        return;
    audio_chunk_t *fx = o->fx_chunk;
    /* Un bloque sacado antes de un fx_cancel() ya no vale. */
    if (o->fx_off < fx->frames && fx->gen != ringbuf_gen(o->fx_rb))
        o->fx_off = fx->frames;

    uint32_t i = 0;
    while (i < c->frames) {
        if (o->fx_off >= fx->frames) {
            if (ringbuf_try_pop(o->fx_rb, fx) != RB_OK)
                return;                       /* sin efectos: no esperar */
            o->fx_off = 0;
            atomic_fetch_add(&o->fx_mixed, 1);
        }
        uint32_t n = fx->frames - o->fx_off;
        if (n > c->frames - i)
            n = c->frames - i;
        int16_t *dst = c->data + 2 * i;
        const int16_t *src = fx->data + 2 * o->fx_off;
        for (uint32_t k = 0; k < 2 * n; k++) {
            int32_t v = (int32_t)dst[k] + src[k];   /* mezcla = suma */
            dst[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        i += n;
        o->fx_off += n;
    }
}

static void apply_volume(audio_chunk_t *c, int vol)
{
    if (vol >= 100)
        return;
    int32_t g = vol * vol;                 /* curva perceptual ~cuadrática */
    for (uint32_t i = 0; i < c->frames * 2; i++)
        c->data[i] = (int16_t)((int32_t)c->data[i] * g / 10000);
}

static void *audio_main(void *arg)
{
    audio_out_t *o = arg;
    audio_chunk_t *c = malloc(sizeof *c);
    uint64_t last_gen = ringbuf_gen(o->rb);

    while (c) {
        /* ¿Hubo flush (Next/Prev/Stop/Seek) desde el último chunk? Cortar
         * de inmediato lo que el dispositivo aún tiene encolado. */
        uint64_t g = ringbuf_gen(o->rb);
        if (g != last_gen) {
            device_drop(o);
            set_inactive(o, g);
            last_gen = g;
        }
        if (!wait_unpaused(o))
            break;

        rb_result_t r = ringbuf_pop(o->rb, c, &o->state);
        if (r == RB_SHUTDOWN)
            break;
        if (r == RB_STALE)
            continue;                       /* el bucle hará el drop */

        /* La pausa pudo llegar mientras esperábamos datos. */
        if (!wait_unpaused(o))
            break;
        if (c->gen != ringbuf_gen(o->rb))   /* flush tras el pop */
            continue;
        if (c->gen != last_gen) {
            device_drop(o);
            last_gen = c->gen;
        }

        if (c->flags & CHUNK_EOS) {
            set_inactive(o, c->gen);
            evlog_add(EV_INFO, "AudioOut: EOS gen %llu",
                      (unsigned long long)c->gen);
            cmdqueue_push(o->cmdq, (command_t){ .type = CMD_TRACK_ENDED,
                                                .arg_u = c->gen });
            continue;
        }
        if (c->rate != o->cur_rate || c->channels != o->cur_channels) {
            if (device_config(o, c->rate, c->channels) < 0)
                continue;
        }
        if (c->flags & CHUNK_TRACK_START)
            evlog_add(EV_INFO, "AudioOut: suena pista #%llu",
                      (unsigned long long)c->track_id);

        mix_fx(o, c);
        apply_volume(c, atomic_load(&o->volume));
        device_write(o, c->data, c->frames);
        atomic_fetch_add(&o->frames_played, c->frames);
        publish(o, c, device_delay(o));
    }
    free(c);
    free(o->fx_chunk);
    o->fx_chunk = NULL;
    atomic_store(&o->state, TS_EXITED);
    return NULL;
}

int audio_out_start(audio_out_t *o, ringbuf_t *rb, ringbuf_t *fx_rb,
                    cmdqueue_t *q, const char *device)
{
    memset(o, 0, sizeof *o);
    o->rb = rb;
    o->fx_rb = fx_rb;
    if (fx_rb && !(o->fx_chunk = calloc(1, sizeof *o->fx_chunk)))
        return -1;
    atomic_store(&o->device_rate, 44100);
    o->cmdq = q;
    snprintf(o->device, sizeof o->device, "%s", device ? device : "(null)");
    pthread_mutex_init(&o->ctl_lock, NULL);
    pthread_cond_init(&o->pause_cond, NULL);
    pthread_mutex_init(&o->status_lock, NULL);
    atomic_store(&o->volume, 80);

    snd_lib_error_set_handler(alsa_error_handler);
    snd_pcm_t *pcm = NULL;
    int rc = device ? snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0)
                    : -ENODEV;
    if (!device) {
        evlog_add(EV_INFO, "AudioOut: salida simulada (--null)");
        atomic_store(&o->dry_run, 1);
    } else if (rc < 0) {
        evlog_add(EV_WARN, "AudioOut: no se pudo abrir '%s' (%s): modo "
                  "simulado sin sonido", o->device, snd_strerror(rc));
        atomic_store(&o->dry_run, 1);
    }
    o->pcm = pcm;
    return pthread_create(&o->thread, NULL, audio_main, o);
}

void audio_out_shutdown(audio_out_t *o)
{
    pthread_mutex_lock(&o->ctl_lock);
    o->shutdown = true;
    pthread_cond_broadcast(&o->pause_cond);
    pthread_mutex_unlock(&o->ctl_lock);
}

void audio_out_join(audio_out_t *o)
{
    pthread_join(o->thread, NULL);
    if (o->pcm) {
        snd_pcm_drop(PCM(o));
        snd_pcm_close(PCM(o));
        o->pcm = NULL;
    }
    snd_config_update_free_global();      /* libera caché de config ALSA */
    snd_lib_error_set_handler(NULL);
    pthread_mutex_destroy(&o->status_lock);
    pthread_cond_destroy(&o->pause_cond);
    pthread_mutex_destroy(&o->ctl_lock);
}
