/*
 * app.h - Agrupa los componentes del reproductor para la UI y main.
 */
#ifndef APP_H
#define APP_H

#include "audio_out.h"
#include "cmdqueue.h"
#include "decoder.h"
#include "fx.h"
#include "player.h"
#include "playlist.h"
#include "ringbuf.h"
#include "stress.h"

typedef struct {
    playlist_t  playlist;
    ringbuf_t   ring;
    ringbuf_t   fx_ring;    /* búfer del 2.º productor (efectos) */
    cmdqueue_t  cmdq;
    decoder_t   decoder;
    audio_out_t out;
    fx_t        fx;
    player_t    player;
    stress_t    stress;
    int         signal_fd;      /* signalfd de SIGINT/SIGTERM (o -1) */
    char        start_dir[MAX_PATH_LEN];
} app_t;

/* Envía un comando al controlador (no bloqueante). */
void app_send(app_t *a, cmd_type_t type, int64_t arg_i, uint64_t arg_u);

/* Bucle de la interfaz ncurses (hilo principal). */
int ui_run(app_t *a);

#endif
