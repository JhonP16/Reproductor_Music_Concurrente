/*
 * ringbuf.h - Búfer circular acotado de chunks PCM (productor-consumidor).
 *
 * Sincronización:
 *   - Un mutex protege todo el estado (slots, índices, generación).
 *   - not_full : el productor espera aquí cuando count == RING_SLOTS.
 *   - not_empty: el consumidor espera aquí cuando no hay datos suficientes.
 *   Ambas esperas son pthread_cond_wait dentro de un while(predicado):
 *   no hay espera activa.
 *
 * Generaciones: cada flush incrementa `gen`. Un chunk cuyo gen no coincide
 * con el actual se considera obsoleto: push lo rechaza (RB_STALE) y el
 * productor abandona lo que estaba decodificando. Esto implementa
 * Next/Prev/Stop/Seek instantáneos sin destruir hilos.
 *
 * Prebuffer: tras un flush (o si el consumidor vació el búfer) la salida
 * no reanuda hasta tener PREBUFFER_SLOTS chunks o un chunk final (EOS),
 * lo que evita underruns en cadena.
 *
 * Capacidad y prebuffer son configurables (ringbuf_init_ex): el búfer de
 * música usa RING_SLOTS/PREBUFFER_SLOTS; el de efectos es pequeño, sin
 * prebuffer, y su consumidor usa ringbuf_try_pop() (no bloqueante) para que
 * la falta de efectos nunca detenga la música.
 */
#ifndef RINGBUF_H
#define RINGBUF_H

#include "common.h"
#include <pthread.h>
#include <stdatomic.h>

enum {
    CHUNK_TRACK_START = 1u << 0,   /* primer chunk de una pista          */
    CHUNK_EOS         = 1u << 1,   /* fin de la reproducción (no más)    */
};

typedef struct {
    uint64_t gen;          /* generación a la que pertenece           */
    uint64_t track_id;     /* id de la pista en la playlist           */
    uint64_t track_index;  /* posición en la playlist al decodificarla */
    uint64_t pos_frames;   /* frame inicial de este chunk en la pista */
    uint64_t total_frames; /* duración total de la pista (0 = ?)      */
    uint32_t rate;
    uint16_t channels;
    uint16_t flags;
    uint32_t frames;       /* frames válidos en data                  */
    int16_t  data[CHUNK_FRAMES * MAX_CHANNELS];
} audio_chunk_t;

typedef enum { RB_OK = 0, RB_STALE, RB_SHUTDOWN, RB_EMPTY } rb_result_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;

    audio_chunk_t  *slots;         /* `cap` elementos                   */
    size_t          cap;           /* capacidad en chunks               */
    size_t          prebuffer;     /* marca alta tras flush (0 = no)    */
    size_t          head;          /* siguiente a leer                  */
    size_t          tail;          /* siguiente a escribir              */
    size_t          count;
    size_t          eos_count;     /* chunks EOS presentes en el búfer  */
    bool            prebuffering;
    bool            shutdown;
    uint64_t        gen;

    /* Estadísticas (leídas por la UI sin lock: atómicas). */
    atomic_uint_fast64_t waits_full;
    atomic_uint_fast64_t waits_empty;
    atomic_uint_fast64_t pushed;
    atomic_uint_fast64_t popped;
    atomic_uint_fast64_t flushes;
} ringbuf_t;

int         ringbuf_init(ringbuf_t *rb);   /* RING_SLOTS, PREBUFFER_SLOTS */
int         ringbuf_init_ex(ringbuf_t *rb, size_t cap, size_t prebuffer);
void        ringbuf_destroy(ringbuf_t *rb);

/* Bloquea mientras esté lleno. `state` (opcional) se actualiza con el
 * estado del hilo llamador para la UI. Copia el chunk dentro del búfer. */
rb_result_t ringbuf_push(ringbuf_t *rb, const audio_chunk_t *c,
                         atomic_int *state);

/* Bloquea mientras no haya datos (o se esté pre-cargando). Si mientras
 * espera ocurre un flush devuelve RB_STALE, para que la salida pueda
 * descartar de inmediato el audio ya entregado al dispositivo. */
rb_result_t ringbuf_pop(ringbuf_t *rb, audio_chunk_t *out,
                        atomic_int *state);

/* No bloqueante: extrae un chunk si hay alguno, si no devuelve RB_EMPTY.
 * Ignora el prebuffer (pensado para búferes con prebuffer = 0). */
rb_result_t ringbuf_try_pop(ringbuf_t *rb, audio_chunk_t *out);

/* Descarta todo el contenido, incrementa y devuelve la nueva generación,
 * y despierta a todos los hilos bloqueados. */
uint64_t    ringbuf_flush(ringbuf_t *rb);

typedef struct {
    size_t   head, tail, count, cap;
    uint64_t gen;
    bool     prebuffering;
} rb_snapshot_t;

uint64_t    ringbuf_gen(ringbuf_t *rb);
void        ringbuf_snapshot(ringbuf_t *rb, rb_snapshot_t *s);  /* para la UI */
size_t      ringbuf_count(ringbuf_t *rb);

/* Despierta a todos y hace que push/pop devuelvan RB_SHUTDOWN. */
void        ringbuf_shutdown(ringbuf_t *rb);

#endif
