/*
 * stress.h - Hilo de estrés opcional: simula a un usuario frenético que
 * agrega, elimina, reordena y salta pistas mientras se reproduce, para
 * demostrar la robustez de la playlist y del protocolo de flush.
 *
 * Duerme entre operaciones con pthread_cond_timedwait (no gira) y se
 * detiene al instante con stress_stop().
 */
#ifndef STRESS_H
#define STRESS_H

#include "cmdqueue.h"
#include "fx.h"
#include "playlist.h"

typedef struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            running;     /* hilo vivo */
    bool            stop;
    playlist_t     *pl;
    cmdqueue_t     *cmdq;
    fx_t           *fx;
    char          **pool;        /* rutas para agregar */
    size_t          npool;
    atomic_uint_fast64_t ops;
    atomic_int      state;
} stress_t;

void stress_init(stress_t *s, playlist_t *pl, cmdqueue_t *q, fx_t *fx);
int  stress_start(stress_t *s);          /* toma el pool de la playlist */
void stress_stop(stress_t *s);           /* señaliza y hace join */
bool stress_running(stress_t *s);
void stress_destroy(stress_t *s);

#endif
