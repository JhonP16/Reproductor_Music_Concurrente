/*
 * audio_out.h - Hilo de salida de audio (CONSUMIDOR del ring buffer).
 *
 * Extrae chunks con ringbuf_pop() (bloqueante) y los escribe en ALSA con
 * snd_pcm_writei() en modo bloqueante: el propio dispositivo marca el
 * ritmo de la frecuencia de muestreo, sin espera activa.
 *
 * Pausa: flag `paused` + variable de condición `pause_cond`; el hilo se
 * duerme en ella y el productor, al llenarse el búfer, se duerme en
 * not_full. Nadie gira.
 *
 * Publica el estado de reproducción (pista que REALMENTE suena, posición)
 * y los datos de visualización bajo `status_lock` (lock hoja).
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include "cmdqueue.h"
#include "ringbuf.h"

#define VIZ_SAMPLES 2048        /* ventana para FFT (potencia de 2) */

typedef struct {
    uint64_t gen;
    uint64_t track_id;          /* 0 = nada sonando */
    uint64_t track_index;
    uint64_t pos_frames;        /* posición audible estimada */
    uint64_t total_frames;
    uint32_t rate;
    bool     active;
} play_status_t;

typedef struct {
    float    window[VIZ_SAMPLES];   /* mono, últimas muestras escritas */
    float    peak_l, peak_r;        /* 0..1 */
    float    rms_l, rms_r;
    uint64_t serial;                /* cambia con cada actualización   */
} viz_data_t;

typedef struct {
    pthread_t       thread;
    ringbuf_t      *rb;
    ringbuf_t      *fx_rb;          /* 2.º productor: efectos (opcional) */
    cmdqueue_t     *cmdq;           /* para notificar fin de lista */

    /* Pausa / apagado */
    pthread_mutex_t ctl_lock;
    pthread_cond_t  pause_cond;
    bool            paused;
    bool            shutdown;

    /* Estado publicado (lock hoja) */
    pthread_mutex_t status_lock;
    play_status_t   status;
    viz_data_t      viz;
    size_t          viz_pos;

    /* Solo los usa el hilo de salida */
    void           *pcm;            /* snd_pcm_t* (opaco aquí) */
    uint32_t        cur_rate;
    uint16_t        cur_channels;
    bool            can_pause;
    char            device[64];

    /* Mezcla de efectos (solo el hilo de salida) */
    audio_chunk_t  *fx_chunk;       /* bloque FX en curso               */
    uint32_t        fx_off;         /* frames ya mezclados de fx_chunk  */

    atomic_uint     device_rate;    /* frecuencia actual (la lee FX)    */
    atomic_uint_fast64_t fx_mixed;  /* bloques FX mezclados             */
    atomic_int      volume;         /* 0..100 */
    atomic_int      state;          /* thread_state_t */
    atomic_uint_fast64_t underruns;
    atomic_uint_fast64_t frames_played;
    atomic_int      dry_run;        /* sin dispositivo: simula el reloj */
} audio_out_t;

/* device == NULL -> salida simulada (reloj por nanosleep, sin sonido). */
int  audio_out_start(audio_out_t *o, ringbuf_t *rb, ringbuf_t *fx_rb,
                     cmdqueue_t *q, const char *device);
void audio_out_set_paused(audio_out_t *o, bool paused);
void audio_out_get_status(audio_out_t *o, play_status_t *st);
void audio_out_get_viz(audio_out_t *o, viz_data_t *v);
void audio_out_shutdown(audio_out_t *o);
void audio_out_join(audio_out_t *o);

#endif
