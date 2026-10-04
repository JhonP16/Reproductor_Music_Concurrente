/*
 * cmdqueue.h - Cola FIFO acotada de comandos UI -> Controlador.
 *
 * mutex + variable de condición `not_empty`. El emisor (UI) NUNCA se
 * bloquea: si la cola está llena el comando se descarta y se reporta.
 * El controlador se bloquea en cmdqueue_pop() sin espera activa.
 */
#ifndef CMDQUEUE_H
#define CMDQUEUE_H

#include "common.h"
#include <pthread.h>

typedef enum {
    CMD_NONE = 0,
    CMD_PLAY,          /* reproducir (o reanudar)                    */
    CMD_PAUSE_TOGGLE,
    CMD_STOP,
    CMD_NEXT,
    CMD_PREV,
    CMD_PLAY_ID,       /* arg_u = id de pista                        */
    CMD_SEEK,          /* arg_i = segundos relativos (±)             */
    CMD_TRACK_ENDED,   /* interno: la salida llegó a EOS             */
    CMD_QUIT,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    int64_t    arg_i;
    uint64_t   arg_u;
} command_t;

#define CMDQ_CAP 64

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    command_t       buf[CMDQ_CAP];
    size_t          head, count;
    bool            closed;
} cmdqueue_t;

void cmdqueue_init(cmdqueue_t *q);
void cmdqueue_destroy(cmdqueue_t *q);
/* No bloqueante. Devuelve false si la cola estaba llena o cerrada. */
bool cmdqueue_push(cmdqueue_t *q, command_t c);
/* Bloqueante. Devuelve false si la cola se cerró y está vacía. */
bool cmdqueue_pop(cmdqueue_t *q, command_t *out);
void cmdqueue_close(cmdqueue_t *q);
const char *cmd_name(cmd_type_t t);

#endif
