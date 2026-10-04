/*
 * stress.c - Generador de modificaciones concurrentes aleatorias.
 */
#include "stress.h"
#include "evlog.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void stress_init(stress_t *s, playlist_t *pl, cmdqueue_t *q, fx_t *fx)
{
    memset(s, 0, sizeof *s);
    s->fx = fx;
    s->pl = pl;
    s->cmdq = q;
    pthread_mutex_init(&s->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&s->cond, &ca);
    pthread_condattr_destroy(&ca);
    atomic_store(&s->state, TS_IDLE);
}

static void free_pool(stress_t *s)
{
    for (size_t i = 0; i < s->npool; i++)
        free(s->pool[i]);
    free(s->pool);
    s->pool = NULL;
    s->npool = 0;
}

/* Duerme `ms` o hasta que pidan parar. Devuelve false si hay que parar. */
static bool nap(stress_t *s, long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&s->lock);
    atomic_store(&s->state, TS_IDLE);
    int rc = 0;
    while (!s->stop && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&s->cond, &s->lock, &ts);
    bool go = !s->stop;
    pthread_mutex_unlock(&s->lock);
    atomic_store(&s->state, TS_RUNNING);
    return go;
}

static void *stress_main(void *arg)
{
    stress_t *s = arg;
    unsigned seed = (unsigned)time(NULL);
    track_info_t t;
    evlog_add(EV_WARN, "Stress: hilo iniciado (%zu rutas)", s->npool);

    while (nap(s, 40 + rand_r(&seed) % 160)) {
        size_t n = playlist_count(s->pl);
        int op = rand_r(&seed) % 100;
        if ((op < 35 || n < 3) && s->npool) {
            playlist_add(s->pl, s->pool[(size_t)rand_r(&seed) % s->npool]);
        } else if (op < 60 && n > 3) {
            if (playlist_get_index(s->pl, (size_t)rand_r(&seed) % n, &t))
                playlist_remove(s->pl, t.id);
        } else if (op < 85 && n) {
            if (playlist_get_index(s->pl, (size_t)rand_r(&seed) % n, &t))
                playlist_move(s->pl, t.id, (rand_r(&seed) & 1) ? 1 : -1);
        } else if (op < 88) {
            playlist_shuffle(s->pl);
        } else if (op < 93) {
            cmdqueue_push(s->cmdq, (command_t){ .type = CMD_NEXT });
        } else if (op < 95) {
            cmdqueue_push(s->cmdq, (command_t){ .type = CMD_PREV });
        } else if (op < 98) {
            fx_trigger(s->fx, rand_r(&seed) % FX_COUNT);   /* 2.º productor */
        } else {
            cmdqueue_push(s->cmdq, (command_t){
                .type = CMD_SEEK, .arg_i = (rand_r(&seed) & 1) ? 10 : -5 });
        }
        atomic_fetch_add(&s->ops, 1);
    }
    atomic_store(&s->state, TS_EXITED);
    evlog_add(EV_WARN, "Stress: hilo detenido (%llu ops)",
              (unsigned long long)atomic_load(&s->ops));
    return NULL;
}

int stress_start(stress_t *s)
{
    if (s->running)
        return 0;
    /* Pool de rutas = contenido actual de la playlist (copiado). */
    size_t total = playlist_count(s->pl);
    track_info_t *v = total ? malloc(total * sizeof *v) : NULL;
    size_t n = v ? playlist_snapshot(s->pl, 0, v, total, NULL) : 0;
    free_pool(s);
    s->pool = n ? calloc(n, sizeof *s->pool) : NULL;
    for (size_t i = 0; s->pool && i < n; i++)
        s->pool[s->npool++] = strdup(v[i].path);
    free(v);

    s->stop = false;
    if (pthread_create(&s->thread, NULL, stress_main, s) != 0)
        return -1;
    s->running = true;
    return 0;
}

void stress_stop(stress_t *s)
{
    if (!s->running)
        return;
    pthread_mutex_lock(&s->lock);
    s->stop = true;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->thread, NULL);
    s->running = false;
}

bool stress_running(stress_t *s)
{
    return s->running;        /* solo lo modifica el hilo UI/main */
}

void stress_destroy(stress_t *s)
{
    stress_stop(s);
    free_pool(s);
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
}
