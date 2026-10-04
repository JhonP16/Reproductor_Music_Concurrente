/*
 * ringbuf.c - Implementación del búfer circular productor-consumidor.
 */
#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

static inline void set_state(atomic_int *state, thread_state_t s)
{
    if (state)
        atomic_store(state, (int)s);
}

int ringbuf_init(ringbuf_t *rb)
{
    return ringbuf_init_ex(rb, RING_SLOTS, PREBUFFER_SLOTS);
}

int ringbuf_init_ex(ringbuf_t *rb, size_t cap, size_t prebuffer)
{
    memset(rb, 0, sizeof *rb);
    if (cap == 0 || prebuffer > cap)
        return -1;
    rb->slots = calloc(cap, sizeof *rb->slots);
    if (!rb->slots)
        return -1;
    rb->cap = cap;
    rb->prebuffer = prebuffer;
    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    rb->prebuffering = prebuffer > 0;
    rb->gen = 1;
    return 0;
}

void ringbuf_destroy(ringbuf_t *rb)
{
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
    pthread_mutex_destroy(&rb->lock);
    free(rb->slots);
    rb->slots = NULL;
}

rb_result_t ringbuf_push(ringbuf_t *rb, const audio_chunk_t *c,
                         atomic_int *state)
{
    pthread_mutex_lock(&rb->lock);

    /* Espera (bloqueante) mientras el búfer esté lleno y el chunk siga
     * siendo vigente. Un flush cambia rb->gen y nos despierta. */
    if (rb->count == rb->cap && !rb->shutdown && c->gen == rb->gen)
        atomic_fetch_add(&rb->waits_full, 1);
    while (rb->count == rb->cap && !rb->shutdown && c->gen == rb->gen) {
        set_state(state, TS_WAIT_NOT_FULL);
        pthread_cond_wait(&rb->not_full, &rb->lock);
    }
    set_state(state, TS_RUNNING);

    if (rb->shutdown) {
        pthread_mutex_unlock(&rb->lock);
        return RB_SHUTDOWN;
    }
    if (c->gen != rb->gen) {               /* fue invalidado por un flush */
        pthread_mutex_unlock(&rb->lock);
        return RB_STALE;
    }

    memcpy(&rb->slots[rb->tail], c, sizeof *c);
    rb->tail = (rb->tail + 1) % rb->cap;
    rb->count++;
    if (c->flags & CHUNK_EOS)
        rb->eos_count++;
    atomic_fetch_add(&rb->pushed, 1);

    /* Solo despertar al consumidor si ya puede avanzar. */
    if (!rb->prebuffering || rb->count >= rb->prebuffer || rb->eos_count)
        pthread_cond_signal(&rb->not_empty);

    pthread_mutex_unlock(&rb->lock);
    return RB_OK;
}

/* Predicado del consumidor: ¿puede extraer un chunk ya? */
static bool can_pop(const ringbuf_t *rb)
{
    if (rb->count == 0)
        return false;
    if (!rb->prebuffering)
        return true;
    return rb->count >= rb->prebuffer || rb->eos_count > 0;
}

rb_result_t ringbuf_pop(ringbuf_t *rb, audio_chunk_t *out, atomic_int *state)
{
    pthread_mutex_lock(&rb->lock);

    if (rb->count == 0 && !rb->prebuffering && rb->prebuffer) {
        /* El consumidor alcanzó al productor: re-entrar en prebuffer para
         * no reproducir a trozos (evita chasquidos repetidos). */
        rb->prebuffering = true;
    }
    uint64_t entry_gen = rb->gen;
    if (!can_pop(rb) && !rb->shutdown)
        atomic_fetch_add(&rb->waits_empty, 1);
    while (!can_pop(rb) && !rb->shutdown && rb->gen == entry_gen) {
        set_state(state, rb->prebuffering && rb->count > 0
                             ? TS_PREBUFFERING : TS_WAIT_NOT_EMPTY);
        pthread_cond_wait(&rb->not_empty, &rb->lock);
    }
    set_state(state, TS_RUNNING);

    if (rb->shutdown) {
        pthread_mutex_unlock(&rb->lock);
        return RB_SHUTDOWN;
    }
    if (rb->gen != entry_gen) {          /* hubo flush mientras esperaba */
        pthread_mutex_unlock(&rb->lock);
        return RB_STALE;
    }

    rb->prebuffering = false;
    memcpy(out, &rb->slots[rb->head], sizeof *out);
    rb->head = (rb->head + 1) % rb->cap;
    rb->count--;
    if (out->flags & CHUNK_EOS)
        rb->eos_count--;
    atomic_fetch_add(&rb->popped, 1);

    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->lock);
    return RB_OK;
}

rb_result_t ringbuf_try_pop(ringbuf_t *rb, audio_chunk_t *out)
{
    pthread_mutex_lock(&rb->lock);
    if (rb->shutdown || rb->count == 0) {
        rb_result_t r = rb->shutdown ? RB_SHUTDOWN : RB_EMPTY;
        pthread_mutex_unlock(&rb->lock);
        return r;
    }
    memcpy(out, &rb->slots[rb->head], sizeof *out);
    rb->head = (rb->head + 1) % rb->cap;
    rb->count--;
    if (out->flags & CHUNK_EOS)
        rb->eos_count--;
    atomic_fetch_add(&rb->popped, 1);
    pthread_cond_signal(&rb->not_full);      /* el productor puede seguir */
    pthread_mutex_unlock(&rb->lock);
    return RB_OK;
}

uint64_t ringbuf_flush(ringbuf_t *rb)
{
    pthread_mutex_lock(&rb->lock);
    rb->head = rb->tail = rb->count = 0;
    rb->eos_count = 0;
    rb->prebuffering = rb->prebuffer > 0;
    uint64_t g = ++rb->gen;
    atomic_fetch_add(&rb->flushes, 1);
    /* broadcast: puede haber productor esperando not_full con un chunk
     * ahora obsoleto, y consumidor esperando not_empty. */
    pthread_cond_broadcast(&rb->not_full);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);
    return g;
}

uint64_t ringbuf_gen(ringbuf_t *rb)
{
    pthread_mutex_lock(&rb->lock);
    uint64_t g = rb->gen;
    pthread_mutex_unlock(&rb->lock);
    return g;
}

void ringbuf_snapshot(ringbuf_t *rb, rb_snapshot_t *s)
{
    pthread_mutex_lock(&rb->lock);
    s->head = rb->head;
    s->tail = rb->tail;
    s->count = rb->count;
    s->cap = rb->cap;
    s->gen = rb->gen;
    s->prebuffering = rb->prebuffering;
    pthread_mutex_unlock(&rb->lock);
}

size_t ringbuf_count(ringbuf_t *rb)
{
    pthread_mutex_lock(&rb->lock);
    size_t n = rb->count;
    pthread_mutex_unlock(&rb->lock);
    return n;
}

void ringbuf_shutdown(ringbuf_t *rb)
{
    pthread_mutex_lock(&rb->lock);
    rb->shutdown = true;
    pthread_cond_broadcast(&rb->not_full);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);
}
