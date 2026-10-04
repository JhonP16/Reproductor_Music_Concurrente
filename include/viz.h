/*
 * viz.h - Analizador de espectro: FFT radix-2 propia (sin librerías).
 * Se ejecuta en el hilo UI sobre una COPIA de la ventana de muestras, así
 * que no retiene ningún lock mientras calcula.
 */
#ifndef VIZ_H
#define VIZ_H

#include "audio_out.h"

#define VIZ_MAX_BANDS 128

typedef struct {
    float level[VIZ_MAX_BANDS];   /* 0..1 suavizado */
    float peak[VIZ_MAX_BANDS];    /* marcador de pico con caída */
    int   nbands;
} spectrum_t;

/* Actualiza `sp` (nbands bandas logarítmicas) a partir de la ventana. */
void viz_spectrum(spectrum_t *sp, const float *window, int nbands,
                  uint32_t rate, bool silent);

#endif
