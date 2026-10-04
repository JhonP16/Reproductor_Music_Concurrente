/*
 * test_ringbuf.c - Estrés del búfer circular: un productor que numera los
 * datos, un consumidor que verifica orden/integridad, y un hilo "usuario"
 * que hace flush aleatorios (como Next/Prev). Verifica:
 *   - dentro de una misma generación, los chunks llegan en orden y sin
 *     corrupción;
 *   - nunca se entrega un chunk de una generación anterior al flush;
 *   - shutdown despierta a todos (sin deadlock).
 */
#include "ringbuf.h"
#include "evlog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static ringbuf_t rb;
static atomic_int stop_flag;
static atomic_uint_fast64_t consumed, stale_seen;

static void fill(audio_chunk_t *c, uint64_t gen, uint64_t seq)
{
    c->gen = gen;
    c->track_id = seq;
    c->frames = CHUNK_FRAMES;
    c->channels = 2;
    c->rate = 44100;
    c->flags = 0;
    for (size_t i = 0; i < CHUNK_FRAMES * 2; i++)
        c->data[i] = (int16_t)(seq * 31 + i);
}

static void *producer(void *arg)
{
    (void)arg;
    audio_chunk_t *c = malloc(sizeof *c);
    uint64_t gen = ringbuf_gen(&rb), seq = 0;
    while (!atomic_load(&stop_flag)) {
        fill(c, gen, seq);
        rb_result_t r = ringbuf_push(&rb, c, NULL);
        if (r == RB_SHUTDOWN)
            break;
        if (r == RB_STALE) {           /* flush: reiniciar en nueva gen */
            gen = ringbuf_gen(&rb);
            seq = 0;
            continue;
        }
        seq++;
    }
    free(c);
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    audio_chunk_t *c = malloc(sizeof *c);
    uint64_t last_gen = 0, expect = 0;
    for (;;) {
        rb_result_t r = ringbuf_pop(&rb, c, NULL);
        if (r == RB_SHUTDOWN)
            break;
        if (r == RB_STALE)            /* flush mientras esperaba */
            continue;
        /* Monotonía de generaciones: nunca retroceder. */
        assert(c->gen >= last_gen);
        if (c->gen != last_gen) {
            last_gen = c->gen;
            expect = 0;
        }
        /* Con flushes intermedios la secuencia de una gen empieza en 0. */
        assert(c->track_id == expect);
        expect++;
        for (size_t i = 0; i < CHUNK_FRAMES * 2; i++)
            assert(c->data[i] == (int16_t)(c->track_id * 31 + i));
        if (c->gen < ringbuf_gen(&rb))
            atomic_fetch_add(&stale_seen, 1); /* posible: flush tras pop */
        atomic_fetch_add(&consumed, 1);
    }
    free(c);
    return NULL;
}

static void *flusher(void *arg)
{
    (void)arg;
    unsigned seed = 1234;
    while (!atomic_load(&stop_flag)) {
        usleep(200 + rand_r(&seed) % 3000);
        ringbuf_flush(&rb);
    }
    return NULL;
}

/* ---- Segundo escenario: búfer de efectos (cap 4, sin prebuffer) con
 * consumidor NO bloqueante (try_pop) y cancelaciones concurrentes. ---- */
static ringbuf_t fxrb;
static atomic_int fx_stop;
static atomic_uint_fast64_t fx_got, fx_empty;

static void *fx_producer(void *arg)
{
    (void)arg;
    audio_chunk_t *c = malloc(sizeof *c);
    uint64_t gen = ringbuf_gen(&fxrb), seq = 0;
    while (!atomic_load(&fx_stop)) {
        fill(c, gen, seq);
        rb_result_t r = ringbuf_push(&fxrb, c, NULL);
        if (r == RB_SHUTDOWN) break;
        if (r == RB_STALE) { gen = ringbuf_gen(&fxrb); seq = 0; continue; }
        seq++;
    }
    free(c);
    return NULL;
}

static void *fx_consumer(void *arg)
{
    (void)arg;
    audio_chunk_t *c = malloc(sizeof *c);
    uint64_t last_gen = 0, expect = 0;
    struct timespec ts = { 0, 50000 };       /* simula el ritmo del audio */
    while (!atomic_load(&fx_stop)) {
        rb_result_t r = ringbuf_try_pop(&fxrb, c);
        if (r == RB_EMPTY) {                 /* nunca bloquea */
            atomic_fetch_add(&fx_empty, 1);
            nanosleep(&ts, NULL);
            continue;
        }
        if (r == RB_SHUTDOWN) break;
        assert(c->gen >= last_gen);
        if (c->gen != last_gen) { last_gen = c->gen; expect = 0; }
        assert(c->track_id == expect);
        expect++;
        assert(c->data[7] == (int16_t)(c->track_id * 31 + 7));
        atomic_fetch_add(&fx_got, 1);
        nanosleep(&ts, NULL);
    }
    free(c);
    return NULL;
}

static void test_fx_ring(void)
{
    assert(ringbuf_init_ex(&fxrb, 4, 0) == 0);
    audio_chunk_t *tmp = malloc(sizeof *tmp);
    assert(ringbuf_try_pop(&fxrb, tmp) == RB_EMPTY);
    pthread_t p, c;
    pthread_create(&p, NULL, fx_producer, NULL);
    pthread_create(&c, NULL, fx_consumer, NULL);
    unsigned seed = 99;
    for (int i = 0; i < 200; i++) {          /* fx_cancel() concurrentes */
        usleep(1000 + rand_r(&seed) % 4000);
        ringbuf_flush(&fxrb);
    }
    atomic_store(&fx_stop, 1);
    ringbuf_shutdown(&fxrb);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    printf("test_fx_ring OK: %lu bloques por try_pop, %lu intentos vacíos "
           "(sin bloquear)\n", (unsigned long)atomic_load(&fx_got),
           (unsigned long)atomic_load(&fx_empty));
    free(tmp);
    ringbuf_destroy(&fxrb);
}

int main(void)
{
    evlog_init();
    assert(ringbuf_init(&rb) == 0);
    pthread_t p, c, f;
    pthread_create(&p, NULL, producer, NULL);
    pthread_create(&c, NULL, consumer, NULL);
    pthread_create(&f, NULL, flusher, NULL);

    sleep(2);
    atomic_store(&stop_flag, 1);
    pthread_join(f, NULL);
    ringbuf_shutdown(&rb);
    pthread_join(p, NULL);
    pthread_join(c, NULL);

    printf("test_ringbuf OK: %lu chunks consumidos, %lu flushes, "
           "waits_full=%lu waits_empty=%lu\n",
           (unsigned long)atomic_load(&consumed),
           (unsigned long)atomic_load(&rb.flushes),
           (unsigned long)atomic_load(&rb.waits_full),
           (unsigned long)atomic_load(&rb.waits_empty));
    ringbuf_destroy(&rb);
    test_fx_ring();
    return 0;
}
