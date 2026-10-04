/*
 * evlog.c - Registro de eventos protegido por mutex.
 */
#include "evlog.h"
#include "common.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct {
    pthread_mutex_t lock;
    ev_entry_t      ring[EVLOG_CAP];
    uint64_t        seq;          /* total de eventos escritos */
    int             echo;
} g_log = { .lock = PTHREAD_MUTEX_INITIALIZER };

static struct timespec g_t0;

double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)(ts.tv_sec - g_t0.tv_sec) +
           (double)(ts.tv_nsec - g_t0.tv_nsec) / 1e9;
}

const char *thread_state_name(thread_state_t s)
{
    static const char *names[] = {
        [TS_IDLE]           = "IDLE",
        [TS_RUNNING]        = "RUNNING",
        [TS_WAIT_NOT_FULL]  = "WAIT not_full",
        [TS_WAIT_NOT_EMPTY] = "WAIT not_empty",
        [TS_WAIT_CMD]       = "WAIT cmd",
        [TS_WAIT_REQUEST]   = "WAIT request",
        [TS_WAIT_TRIGGER]   = "WAIT trigger",
        [TS_PAUSED]         = "PAUSED",
        [TS_PREBUFFERING]   = "PREBUFFER",
        [TS_EXITED]         = "EXITED",
    };
    if ((size_t)s < ARRAY_LEN(names) && names[s])
        return names[s];
    return "?";
}

void evlog_init(void)
{
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    pthread_mutex_lock(&g_log.lock);
    g_log.seq = 0;
    pthread_mutex_unlock(&g_log.lock);
}

void evlog_destroy(void)
{
    /* El mutex es estático; no hay memoria dinámica que liberar. */
}

void evlog_set_echo(int on)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.echo = on;
    pthread_mutex_unlock(&g_log.lock);
}

void evlog_add(ev_level_t level, const char *fmt, ...)
{
    char buf[EVLOG_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);   /* formateo fuera del lock */
    va_end(ap);
    double t = now_seconds();

    pthread_mutex_lock(&g_log.lock);
    ev_entry_t *e = &g_log.ring[g_log.seq % EVLOG_CAP];
    e->seq   = g_log.seq++;
    e->t     = t;
    e->level = level;
    memcpy(e->msg, buf, sizeof buf);
    int echo = g_log.echo;
    pthread_mutex_unlock(&g_log.lock);

    if (echo)
        fprintf(stderr, "[%8.3f] %s\n", t, buf);
}

size_t evlog_snapshot(ev_entry_t *out, size_t max)
{
    pthread_mutex_lock(&g_log.lock);
    uint64_t total = g_log.seq;
    size_t n = total < EVLOG_CAP ? (size_t)total : EVLOG_CAP;
    if (n > max)
        n = max;
    for (size_t i = 0; i < n; i++)
        out[i] = g_log.ring[(total - n + i) % EVLOG_CAP];
    pthread_mutex_unlock(&g_log.lock);
    return n;
}

uint64_t evlog_seq(void)
{
    pthread_mutex_lock(&g_log.lock);
    uint64_t s = g_log.seq;
    pthread_mutex_unlock(&g_log.lock);
    return s;
}
