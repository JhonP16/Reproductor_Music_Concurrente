/*
 * player.c - Máquina de estados del reproductor (hilo controlador).
 *
 *            PLAY/NEXT/PREV/PLAY_ID          PAUSE
 *   STOPPED ─────────────────────▶ PLAYING ◀──────▶ PAUSED
 *      ▲                              │                │
 *      └──────── STOP / fin lista ────┴────────────────┘
 */
#include "player.h"
#include "evlog.h"

#include <string.h>

static void set_state(player_t *p, play_state_t s, uint64_t gen,
                      uint64_t id, size_t index)
{
    pthread_mutex_lock(&p->lock);
    p->state = s;
    p->gen = gen;
    p->cur_id = id;
    p->cur_index = index;
    pthread_mutex_unlock(&p->lock);
}

play_state_t player_state(player_t *p)
{
    pthread_mutex_lock(&p->lock);
    play_state_t s = p->state;
    pthread_mutex_unlock(&p->lock);
    return s;
}

uint64_t player_current_id(player_t *p)
{
    play_status_t st;
    audio_out_get_status(p->out, &st);
    pthread_mutex_lock(&p->lock);
    uint64_t id = (st.active && st.gen == p->gen) ? st.track_id : p->cur_id;
    pthread_mutex_unlock(&p->lock);
    return id;
}

/*
 * Pista de referencia para Next/Prev/Seek: la que REALMENTE suena si la
 * salida ya está en la generación vigente (incluye avances sin hueco de
 * una pista a la siguiente); si no, la última pedida (p. ej. el usuario
 * pulsó Next dos veces antes de que llegara audio).
 */
static void reference_track(player_t *p, uint64_t *id, size_t *index,
                            uint64_t *pos, uint32_t *rate)
{
    play_status_t st;
    audio_out_get_status(p->out, &st);
    pthread_mutex_lock(&p->lock);
    if (st.active && st.gen == p->gen) {
        *id = st.track_id;
        *index = st.track_index;
        *pos = st.pos_frames;
        *rate = st.rate;
    } else {
        *id = p->cur_id;
        *index = p->cur_index;
        *pos = p->cur_seek;
        *rate = p->cur_rate;
    }
    pthread_mutex_unlock(&p->lock);
}

/* Secuencia de cambio de pista: flush -> petición -> reanudar salida. */
static void start_track_ex(player_t *p, const track_info_t *t, size_t index,
                           uint64_t seek_frame, bool paused)
{
    uint64_t gen = ringbuf_flush(p->rb);          /* 1. invalida todo   */
    decoder_request(p->dec, gen, t, index, seek_frame); /* 2. productor */
    audio_out_set_paused(p->out, paused);         /* 3. consumidor      */
    set_state(p, paused ? PS_PAUSED : PS_PLAYING, gen, t->id, index);
    play_status_t st;
    audio_out_get_status(p->out, &st);
    pthread_mutex_lock(&p->lock);
    p->cur_seek = seek_frame;
    if (st.track_id != t->id)
        p->cur_rate = 0;            /* se desconoce hasta que suene */
    else if (st.rate)
        p->cur_rate = st.rate;
    pthread_mutex_unlock(&p->lock);
    evlog_add(EV_SYNC, "Controller: flush -> gen %llu, pista #%llu \"%s\"",
              (unsigned long long)gen, (unsigned long long)t->id, t->title);
}

static void start_track(player_t *p, const track_info_t *t, size_t index,
                        uint64_t seek_frame)
{
    start_track_ex(p, t, index, seek_frame, false);
}

static void do_stop(player_t *p)
{
    uint64_t gen = ringbuf_flush(p->rb);
    audio_out_set_paused(p->out, false);
    pthread_mutex_lock(&p->lock);
    p->state = PS_STOPPED;
    p->gen = gen;
    pthread_mutex_unlock(&p->lock);
    evlog_add(EV_SYNC, "Controller: STOP, flush -> gen %llu",
              (unsigned long long)gen);
}

static void play_current_or_first(player_t *p)
{
    track_info_t t;
    size_t idx = 0;
    uint64_t id;
    pthread_mutex_lock(&p->lock);
    id = p->cur_id;
    pthread_mutex_unlock(&p->lock);
    if ((id && playlist_get(p->pl, id, &t, &idx)) ||
        playlist_get_index(p->pl, idx = 0, &t))
        start_track(p, &t, idx, 0);
    else
        evlog_add(EV_WARN, "Controller: la lista está vacía");
}

static void handle(player_t *p, const command_t *c)
{
    play_state_t s = player_state(p);
    track_info_t t;
    size_t idx;
    uint64_t id, pos;
    uint32_t rate;

    switch (c->type) {
    case CMD_PLAY:
        if (s == PS_PAUSED) {
            audio_out_set_paused(p->out, false);
            set_state(p, PS_PLAYING, p->gen, p->cur_id, p->cur_index);
        } else if (s == PS_STOPPED) {
            play_current_or_first(p);
        }
        break;

    case CMD_PAUSE_TOGGLE:
        if (s == PS_PLAYING) {
            audio_out_set_paused(p->out, true);
            pthread_mutex_lock(&p->lock);
            p->state = PS_PAUSED;
            pthread_mutex_unlock(&p->lock);
            evlog_add(EV_SYNC, "Controller: PAUSE (salida bloqueada en cond)");
        } else if (s == PS_PAUSED) {
            audio_out_set_paused(p->out, false);
            pthread_mutex_lock(&p->lock);
            p->state = PS_PLAYING;
            pthread_mutex_unlock(&p->lock);
            evlog_add(EV_SYNC, "Controller: RESUME (signal pause_cond)");
        } else {
            play_current_or_first(p);
        }
        break;

    case CMD_STOP:
        if (s != PS_STOPPED)
            do_stop(p);
        break;

    case CMD_NEXT:
        reference_track(p, &id, &idx, &pos, &rate);
        if (playlist_next_after(p->pl, id, idx, &t, &idx))
            start_track(p, &t, idx, 0);
        else if (playlist_get_index(p->pl, 0, &t))
            start_track(p, &t, 0, 0);            /* volver al inicio */
        else
            do_stop(p);
        break;

    case CMD_PREV:
        reference_track(p, &id, &idx, &pos, &rate);
        /* Comportamiento clásico: si van >3 s, reinicia la pista. */
        if (rate && pos > 3ull * rate && playlist_get(p->pl, id, &t, &idx))
            start_track(p, &t, idx, 0);
        else if (playlist_prev_before(p->pl, id, idx, &t, &idx))
            start_track(p, &t, idx, 0);
        else if (playlist_get(p->pl, id, &t, &idx))
            start_track(p, &t, idx, 0);
        break;

    case CMD_PLAY_ID:
        if (playlist_get(p->pl, c->arg_u, &t, &idx))
            start_track(p, &t, idx, 0);
        break;

    case CMD_SEEK:
        if (s == PS_STOPPED)
            break;
        reference_track(p, &id, &idx, &pos, &rate);
        track_meta_t m;
        if (!rate && decoder_get_meta(p->dec, id, &m))
            rate = m.rate;
        if (rate && playlist_get(p->pl, id, &t, &idx)) {
            int64_t target = (int64_t)pos + c->arg_i * (int64_t)rate;
            /* Seek en pausa: se recoloca y sigue en pausa. */
            start_track_ex(p, &t, idx, target > 0 ? (uint64_t)target : 0,
                           s == PS_PAUSED);
        }
        break;

    case CMD_TRACK_ENDED:
        /* Solo vale si el EOS es de la generación vigente. */
        pthread_mutex_lock(&p->lock);
        if (c->arg_u == p->gen && p->state != PS_STOPPED) {
            p->state = PS_STOPPED;
            evlog_add(EV_INFO, "Controller: fin de la lista -> STOPPED");
        }
        pthread_mutex_unlock(&p->lock);
        break;

    default:
        break;
    }
}

static void *player_main(void *arg)
{
    player_t *p = arg;
    command_t c;
    for (;;) {
        atomic_store(&p->thread_state, TS_WAIT_CMD);
        if (!cmdqueue_pop(p->cmdq, &c))
            break;
        atomic_store(&p->thread_state, TS_RUNNING);
        if (c.type == CMD_QUIT)
            break;
        handle(p, &c);
        /* Los efectos solo se aceptan con música sonando: pausar, detener
         * o llegar al fin de la lista los silencia y cancela. */
        if (p->fx)
            fx_set_enabled(p->fx, player_state(p) == PS_PLAYING);
        atomic_fetch_add(&p->cmds_handled, 1);
    }
    atomic_store(&p->thread_state, TS_EXITED);
    return NULL;
}

int player_start(player_t *p, cmdqueue_t *q, playlist_t *pl, ringbuf_t *rb,
                 decoder_t *dec, audio_out_t *out, fx_t *fx)
{
    memset(p, 0, sizeof *p);
    p->cmdq = q;
    p->pl = pl;
    p->rb = rb;
    p->dec = dec;
    p->out = out;
    p->fx = fx;
    p->gen = ringbuf_gen(rb);
    pthread_mutex_init(&p->lock, NULL);
    return pthread_create(&p->thread, NULL, player_main, p);
}

void player_join(player_t *p)
{
    pthread_join(p->thread, NULL);
    pthread_mutex_destroy(&p->lock);
}
