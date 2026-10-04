/*
 * cmdqueue.c - Cola de comandos con mutex + variable de condición.
 */
#include "cmdqueue.h"

#include <string.h>

void cmdqueue_init(cmdqueue_t *q)
{
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void cmdqueue_destroy(cmdqueue_t *q)
{
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}

bool cmdqueue_push(cmdqueue_t *q, command_t c)
{
    bool ok = false;
    pthread_mutex_lock(&q->lock);
    if (!q->closed && q->count < CMDQ_CAP) {
        q->buf[(q->head + q->count) % CMDQ_CAP] = c;
        q->count++;
        ok = true;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

bool cmdqueue_pop(cmdqueue_t *q, command_t *out)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->lock);
    bool ok = q->count > 0;
    if (ok) {
        *out = q->buf[q->head];
        q->head = (q->head + 1) % CMDQ_CAP;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

void cmdqueue_close(cmdqueue_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

const char *cmd_name(cmd_type_t t)
{
    switch (t) {
    case CMD_PLAY:         return "PLAY";
    case CMD_PAUSE_TOGGLE: return "PAUSE";
    case CMD_STOP:         return "STOP";
    case CMD_NEXT:         return "NEXT";
    case CMD_PREV:         return "PREV";
    case CMD_PLAY_ID:      return "PLAY_ID";
    case CMD_SEEK:         return "SEEK";
    case CMD_TRACK_ENDED:  return "TRACK_ENDED";
    case CMD_QUIT:         return "QUIT";
    default:               return "NONE";
    }
}
