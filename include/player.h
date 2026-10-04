/*
 * player.h - Hilo CONTROLADOR: consume la cola de comandos y coordina
 * playlist, decodificador, ring buffer y salida.
 *
 * Es el único que hace ringbuf_flush() y decoder_request(), de modo que
 * las transiciones de estado están serializadas en un solo hilo: no hay
 * dos "Next" compitiendo entre sí.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include "audio_out.h"
#include "cmdqueue.h"
#include "decoder.h"
#include "fx.h"
#include "playlist.h"
#include "ringbuf.h"

typedef enum { PS_STOPPED = 0, PS_PLAYING, PS_PAUSED } play_state_t;

typedef struct {
    pthread_t       thread;
    cmdqueue_t     *cmdq;
    playlist_t     *pl;
    ringbuf_t      *rb;
    decoder_t      *dec;
    audio_out_t    *out;
    fx_t           *fx;            /* se silencia al detener */

    pthread_mutex_t lock;          /* protege los campos de abajo */
    play_state_t    state;
    uint64_t        gen;           /* generación vigente */
    uint64_t        cur_id;        /* última pista pedida */
    size_t          cur_index;
    uint64_t        cur_seek;      /* frame inicial pedido */
    uint32_t        cur_rate;      /* rate conocido de la pista */

    atomic_int      thread_state;
    atomic_uint_fast64_t cmds_handled;
} player_t;

int          player_start(player_t *p, cmdqueue_t *q, playlist_t *pl,
                          ringbuf_t *rb, decoder_t *dec, audio_out_t *out,
                          fx_t *fx);
play_state_t player_state(player_t *p);
/* Pista "actual" según el controlador (para que la UI la resalte). */
uint64_t     player_current_id(player_t *p);
void         player_join(player_t *p);   /* tras enviar CMD_QUIT */

#endif
