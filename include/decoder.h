/*
 * decoder.h - Hilo decodificador (PRODUCTOR del ring buffer).
 *
 * El controlador le entrega peticiones {generación, pista, posición} a
 * través de un mutex + variable de condición (`req_cond`). El hilo:
 *   1. espera una petición (bloqueado, sin girar);
 *   2. abre y decodifica la pista en chunks, ringbuf_push() de cada uno;
 *   3. al terminar la pista prepara la SIGUIENTE leyendo la playlist
 *      (rdlock) y continúa sin hueco;
 *   4. si push devuelve RB_STALE (hubo flush por Next/Prev/Stop/Seek)
 *      abandona inmediatamente y vuelve a (1).
 */
#ifndef DECODER_H
#define DECODER_H

#include "playlist.h"
#include "ringbuf.h"

/* Metadatos de la pista en decodificación, publicados para la UI. */
typedef struct {
    uint64_t track_id;
    char     codec[8];
    uint32_t rate;
    uint16_t src_channels;
    uint16_t bits;
    uint64_t total_frames;
    char     title[MAX_TITLE_LEN];
    char     artist[MAX_TITLE_LEN];
} track_meta_t;

#define META_CACHE 4

typedef struct {
    pthread_t       thread;
    pthread_mutex_t lock;          /* protege req_* , shutdown y meta[] */
    pthread_cond_t  req_cond;
    bool            has_req;
    bool            shutdown;
    uint64_t        req_gen;
    track_info_t    req_track;
    size_t          req_index;
    uint64_t        req_seek;      /* frame inicial */

    track_meta_t    meta[META_CACHE];
    size_t          meta_next;

    playlist_t     *pl;
    ringbuf_t      *rb;
    atomic_int      state;         /* thread_state_t para la UI */
    atomic_uint_fast64_t tracks_opened;
} decoder_t;

int  decoder_start(decoder_t *d, playlist_t *pl, ringbuf_t *rb);
/* Pide decodificar `t` (posición `index`) desde `seek_frame` con la
 * generación `gen` (devuelta por el ringbuf_flush previo). */
void decoder_request(decoder_t *d, uint64_t gen, const track_info_t *t,
                     size_t index, uint64_t seek_frame);
/* Copia metadatos de `track_id` si están en caché. */
bool decoder_get_meta(decoder_t *d, uint64_t track_id, track_meta_t *out);
void decoder_shutdown(decoder_t *d);   /* señaliza; luego decoder_join */
void decoder_join(decoder_t *d);

#endif
