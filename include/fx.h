/*
 * fx.h - Hilo de EFECTOS: segundo PRODUCTOR que suena encima de la música.
 *
 *   UI ──fx_trigger()──▶ [pending, mutex + cond] ──▶ hilo FX (sintetiza)
 *                                                       │ push (bloqueante)
 *                                                       ▼
 *                     AudioOut ◀──try_pop (NO bloqueante)── ring FX
 *
 * - La UI solo encola el disparo (no bloquea).
 * - El hilo FX duerme en `cond` mientras no haya efectos sonando. Con
 *   efectos activos sintetiza bloques cortos (FX_CHUNK_FRAMES) y los
 *   empuja a su propio ring buffer, durmiendo en not_full cuando se llena.
 * - AudioOut mezcla (suma) esos bloques sobre la música con
 *   ringbuf_try_pop(): si no hay efectos no espera, así un productor nunca
 *   puede detener al otro.
 * - fx_cancel() hace flush del ring FX (generación nueva): los bloques de
 *   efectos cancelados se descartan al instante.
 */
#ifndef FX_H
#define FX_H

#include "audio_out.h"
#include "ringbuf.h"

typedef enum {
    FX_APPLAUSE = 0, FX_HORN, FX_BELLS, FX_LASER, FX_DRUMS, FX_COUNT
} fx_id_t;

#define FX_SLOTS        4      /* capacidad del ring FX                 */
#define FX_CHUNK_FRAMES 512    /* ~12 ms por bloque: baja latencia      */
#define FX_MAX_VOICES   6      /* efectos simultáneos                   */
#define FX_PENDING_MAX  16

typedef struct {
    int      id;
    double   t;                 /* segundos transcurridos   */
    uint32_t seed;              /* ruido (por voz)          */
    float    lp[2];             /* estado de filtro         */
} fx_voice_t;

typedef struct {
    pthread_t       thread;
    pthread_mutex_t lock;       /* protege pending, cancel, shutdown */
    pthread_cond_t  cond;
    int             pending[FX_PENDING_MAX];
    size_t          npending;
    bool            cancel;
    bool            shutdown;
    bool            enabled;    /* solo hay efectos con música sonando */

    ringbuf_t      *rb;
    audio_out_t    *out;        /* solo para leer la frecuencia actual */

    atomic_int      state;
    atomic_uint     active_mask;    /* bit i = efecto i sonando */
    atomic_uint_fast64_t triggered;
    atomic_uint_fast64_t rejected;  /* disparos rechazados (sin música / cola llena) */
} fx_t;

const char *fx_name(int id);
int  fx_start(fx_t *f, ringbuf_t *rb, audio_out_t *out);
/* No bloqueante. Devuelve false si los efectos están deshabilitados
 * (la música no está sonando) o la cola de disparos está llena. */
bool fx_trigger(fx_t *f, int id);
/* El controlador la llama tras cada transición: habilitar solo en
 * PLAYING. Deshabilitar cancela lo pendiente (bajo el mismo lock que
 * fx_trigger, sin ventana de carrera). */
void fx_set_enabled(fx_t *f, bool on);
void fx_cancel(fx_t *f);            /* silencia todos los efectos */
void fx_shutdown(fx_t *f);
void fx_join(fx_t *f);

#endif
