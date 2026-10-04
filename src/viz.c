/*
 * viz.c - FFT iterativa radix-2 (Cooley-Tukey) + agrupación en bandas.
 */
#include "viz.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N VIZ_SAMPLES

static void fft(float *re, float *im)
{
    /* Reordenamiento bit-reverse. */
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < N; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

void viz_spectrum(spectrum_t *sp, const float *window, int nbands,
                  uint32_t rate, bool silent)
{
    static float hann[N];
    static int hann_ready;
    float re[N], im[N], mag[N / 2];

    if (nbands > VIZ_MAX_BANDS) nbands = VIZ_MAX_BANDS;
    if (nbands < 1) nbands = 1;
    if (sp->nbands != nbands) {
        memset(sp, 0, sizeof *sp);
        sp->nbands = nbands;
    }
    if (!hann_ready) {          /* solo lo usa el hilo UI */
        for (int i = 0; i < N; i++)
            hann[i] = 0.5f - 0.5f * (float)cos(2.0 * M_PI * i / (N - 1));
        hann_ready = 1;
    }
    if (!rate) rate = 44100;

    float target[VIZ_MAX_BANDS] = {0};
    if (!silent) {
        for (int i = 0; i < N; i++) {
            re[i] = window[i] * hann[i];
            im[i] = 0.0f;
        }
        fft(re, im);
        for (int i = 0; i < N / 2; i++)
            mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]);

        /* Bandas con espaciado logarítmico entre 40 Hz y 16 kHz. */
        const double fmin = 40.0, fmax = 16000.0;
        double hz_per_bin = (double)rate / N;
        for (int b = 0; b < nbands; b++) {
            double f0 = fmin * pow(fmax / fmin, (double)b / nbands);
            double f1 = fmin * pow(fmax / fmin, (double)(b + 1) / nbands);
            int i0 = (int)(f0 / hz_per_bin), i1 = (int)(f1 / hz_per_bin);
            if (i1 <= i0) i1 = i0 + 1;
            if (i1 > N / 2) i1 = N / 2;
            float m = 0;
            for (int i = i0; i < i1; i++)
                if (mag[i] > m) m = mag[i];
            /* a dB, normalizado: ~-60 dB -> 0, 0 dB -> 1 */
            float db = 20.0f * log10f(m / (N / 4.0f) + 1e-9f);
            float v = (db + 60.0f) / 60.0f;
            /* realce leve de agudos (el espectro musical cae ~3 dB/oct) */
            v += 0.12f * (float)b / nbands;
            target[b] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
    }
    for (int b = 0; b < nbands; b++) {
        /* Ataque rápido, caída suave. */
        float cur = sp->level[b];
        sp->level[b] = target[b] > cur ? cur + (target[b] - cur) * 0.7f
                                       : cur * 0.82f;
        if (sp->level[b] >= sp->peak[b])
            sp->peak[b] = sp->level[b];
        else
            sp->peak[b] -= 0.012f;
        if (sp->peak[b] < 0) sp->peak[b] = 0;
    }
}
