/*
 * fx.c - Síntesis de efectos y productor del ring FX.
 *
 * Los efectos se sintetizan por procedimiento (sin archivos) a la
 * frecuencia de muestreo que tenga el dispositivo en ese momento, así
 * siempre coinciden con la música que se está reproduciendo.
 */
#include "fx.h"
#include "evlog.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU (2.0 * M_PI)

static const struct { const char *name; double dur; } FX_INFO[FX_COUNT] = {
    [FX_APPLAUSE] = { "Aplausos", 2.2 },
    [FX_HORN]     = { "Bocina",   1.3 },
    [FX_BELLS]    = { "Campanas", 1.6 },
    [FX_LASER]    = { "Láser",    0.9 },
    [FX_DRUMS]    = { "Tambor",   1.6 },
};

const char *fx_name(int id)
{
    return id >= 0 && id < FX_COUNT ? FX_INFO[id].name : "?";
}

/* ------------------------------------------------------------------ */
/* Síntesis (solo la ejecuta el hilo FX)                               */
/* ------------------------------------------------------------------ */

static float noise(uint32_t *s)                /* xorshift32 en [-1, 1] */
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return (float)(*s / 4294967296.0 * 2.0 - 1.0);
}

static double fade(double t, double dur, double in, double out)
{
    double a = t < in ? t / in : 1.0;
    double b = dur - t < out ? (dur - t) / out : 1.0;
    return a * (b < 0 ? 0 : b);
}

static double saw(double x) { return 2.0 * (x - floor(x + 0.5)); }

/* Tom/bombo con caída de tono. */
static double tom(double tt, double f_end, double f_add)
{
    if (tt < 0) return 0;
    double ph = f_end * tt + f_add * (1.0 - exp(-20.0 * tt)) / 20.0;
    return sin(TAU * ph) * exp(-tt * 9.0);
}

/* Barrido exponencial de f0 a f1 en `len` segundos. */
static double zap(double tt, double f0, double f1, double len)
{
    if (tt < 0 || tt > len) return 0;
    double k = log(f1 / f0) / len;
    double ph = f0 * (exp(k * tt) - 1.0) / k;
    return tanh(4.0 * sin(TAU * ph)) * exp(-tt * 3.0);
}

static void synth(fx_voice_t *v, float *l, float *r)
{
    double t = v->t, dur = FX_INFO[v->id].dur;
    double sl = 0, sr = 0;

    switch (v->id) {
    case FX_APPLAUSE: {
        /* Tres "grupos" de personas aplaudiendo a ritmos distintos. */
        double e = (exp(-fmod(t * 9.1, 1.0) * 9) +
                    exp(-fmod(t * 11.7 + 0.31, 1.0) * 9) +
                    exp(-fmod(t * 13.3 + 0.67, 1.0) * 9)) / 3.0;
        float nl = noise(&v->seed), nr = noise(&v->seed);
        v->lp[0] += 0.35f * (nl - v->lp[0]);       /* pasa-altos simple */
        v->lp[1] += 0.35f * (nr - v->lp[1]);
        double g = 0.9 * e * fade(t, dur, 0.12, 0.6);
        sl = (nl - v->lp[0]) * g;
        sr = (nr - v->lp[1]) * g;
        break;
    }
    case FX_HORN: {
        static const double f[3] = { 233.08, 293.66, 349.23 };
        double vib = 1.0 + 0.004 * sin(TAU * 5.5 * t);
        double s = 0;
        for (int i = 0; i < 3; i++)
            s += saw(f[i] * vib * t + i * 0.13);
        s = tanh(1.8 * s / 3.0) * 0.5 * fade(t, dur, 0.03, 0.15);
        sl = sr = s;
        break;
    }
    case FX_BELLS: {
        static const double f[4] = { 1046.5, 1318.5, 1568.0, 2093.0 };
        for (int i = 0; i < 4; i++) {
            double tt = t - i * 0.12;
            if (tt < 0) continue;
            double b = exp(-tt * 4.0) *
                       (sin(TAU * f[i] * tt) +
                        0.4 * sin(TAU * f[i] * 2.76 * tt) * exp(-tt * 6.0));
            double pan = i % 2 ? 0.3 : 0.7;      /* alterna izq/der */
            sl += b * pan * 0.35;
            sr += b * (1.0 - pan) * 0.35;
        }
        break;
    }
    case FX_LASER: {
        double a = zap(t, 2200, 180, 0.4) * 0.3;
        double b = zap(t - 0.45, 2600, 220, 0.4) * 0.3;
        sl = a * 0.85 + b * 0.25;               /* efecto ping-pong */
        sr = a * 0.25 + b * 0.85;
        break;
    }
    case FX_DRUMS: {
        static const double hits[] = { 0.0, 0.2, 0.4, 0.5, 0.6, 0.7 };
        static const double pitch[] = { 140, 120, 110, 95, 85, 75 };
        for (size_t i = 0; i < 6; i++) {
            double s = tom(t - hits[i], pitch[i], 120) * 0.5;
            double pan = 0.25 + 0.1 * (double)i;     /* recorre el estéreo */
            sl += s * (1.0 - pan) * 1.4;
            sr += s * pan * 1.4;
        }
        double tc = t - 0.9;                          /* golpe final */
        if (tc >= 0) {
            double k = tom(tc, 55, 160) * 0.6;
            float n = noise(&v->seed);
            v->lp[0] += 0.5f * (n - v->lp[0]);
            double crash = (n - v->lp[0]) * exp(-tc * 2.5) * 0.35;
            sl += k + crash;
            sr += k + crash;
        }
        break;
    }
    }
    *l = (float)sl;
    *r = (float)sr;
}

/* ------------------------------------------------------------------ */
/* Hilo productor                                                      */
/* ------------------------------------------------------------------ */

static unsigned voices_mask(const fx_voice_t *v, int nv)
{
    unsigned m = 0;
    for (int i = 0; i < nv; i++)
        m |= 1u << v[i].id;
    return m;
}

static void *fx_main(void *arg)
{
    fx_t *f = arg;
    audio_chunk_t *c = malloc(sizeof *c);
    fx_voice_t voices[FX_MAX_VOICES];
    int nv = 0;
    uint32_t seed = 0x9e3779b9u;

    while (c) {
        /* ---- Sección crítica: recoger disparos / cancelaciones ---- */
        pthread_mutex_lock(&f->lock);
        while (!f->npending && !f->cancel && nv == 0 && !f->shutdown) {
            atomic_store(&f->state, TS_WAIT_TRIGGER);
            pthread_cond_wait(&f->cond, &f->lock);
        }
        if (f->shutdown) {
            pthread_mutex_unlock(&f->lock);
            break;
        }
        if (f->cancel) {
            nv = 0;
            f->cancel = false;
        }
        for (size_t i = 0; i < f->npending; i++) {
            if (nv == FX_MAX_VOICES) {           /* reemplaza la más vieja */
                memmove(voices, voices + 1, (FX_MAX_VOICES - 1) * sizeof *voices);
                nv--;
            }
            seed = seed * 1664525u + 1013904223u;
            voices[nv++] = (fx_voice_t){ .id = f->pending[i],
                                         .seed = seed | 1u };
        }
        f->npending = 0;
        /* La generación se lee con f->lock tomado (orden fx → ring): un
         * fx_cancel() posterior la invalidará y el push dará RB_STALE. */
        uint64_t gen = ringbuf_gen(f->rb);
        pthread_mutex_unlock(&f->lock);

        atomic_store(&f->active_mask, voices_mask(voices, nv));
        if (nv == 0)
            continue;
        atomic_store(&f->state, TS_RUNNING);

        /* ---- Síntesis fuera de todo lock ---- */
        uint32_t rate = atomic_load(&f->out->device_rate);
        if (!rate) rate = 44100;
        double dt = 1.0 / rate;
        for (uint32_t i = 0; i < FX_CHUNK_FRAMES; i++) {
            float l = 0, r = 0;
            for (int k = 0; k < nv; k++) {
                float vl, vr;
                if (voices[k].t < FX_INFO[voices[k].id].dur) {
                    synth(&voices[k], &vl, &vr);
                    l += vl;
                    r += vr;
                }
                voices[k].t += dt;
            }
            l = l > 1 ? 1 : l < -1 ? -1 : l;
            r = r > 1 ? 1 : r < -1 ? -1 : r;
            c->data[2 * i] = (int16_t)(l * 32767);
            c->data[2 * i + 1] = (int16_t)(r * 32767);
        }
        int k = 0;                                /* retirar terminadas */
        for (int i = 0; i < nv; i++)
            if (voices[i].t < FX_INFO[voices[i].id].dur)
                voices[k++] = voices[i];
        nv = k;

        c->gen = gen;
        c->rate = rate;
        c->channels = 2;
        c->frames = FX_CHUNK_FRAMES;
        c->flags = 0;
        c->track_id = c->track_index = c->pos_frames = c->total_frames = 0;

        rb_result_t r = ringbuf_push(f->rb, c, &f->state);
        if (r == RB_SHUTDOWN)
            break;
        if (r == RB_STALE)                       /* fx_cancel() */
            nv = 0;
        atomic_store(&f->active_mask, voices_mask(voices, nv));
    }
    free(c);
    atomic_store(&f->active_mask, 0);
    atomic_store(&f->state, TS_EXITED);
    return NULL;
}

/* ------------------------------------------------------------------ */

int fx_start(fx_t *f, ringbuf_t *rb, audio_out_t *out)
{
    memset(f, 0, sizeof *f);
    f->rb = rb;
    f->out = out;
    pthread_mutex_init(&f->lock, NULL);
    pthread_cond_init(&f->cond, NULL);
    atomic_store(&f->state, TS_IDLE);
    return pthread_create(&f->thread, NULL, fx_main, f);
}

bool fx_trigger(fx_t *f, int id)
{
    if (id < 0 || id >= FX_COUNT)
        return false;
    bool ok = false;
    pthread_mutex_lock(&f->lock);
    if (f->enabled && f->npending < FX_PENDING_MAX && !f->shutdown) {
        f->pending[f->npending++] = id;
        ok = true;
        pthread_cond_signal(&f->cond);
    }
    pthread_mutex_unlock(&f->lock);
    if (ok) {
        atomic_fetch_add(&f->triggered, 1);
        evlog_add(EV_SYNC, "FX: disparo '%s' (signal cond)", fx_name(id));
    } else {
        atomic_fetch_add(&f->rejected, 1);
    }
    return ok;
}

/* Requiere f->lock. */
static void cancel_locked(fx_t *f)
{
    f->npending = 0;
    f->cancel = true;
    ringbuf_flush(f->rb);         /* orden de locks: fx.lock → ring.lock */
    pthread_cond_signal(&f->cond);
}

void fx_cancel(fx_t *f)
{
    pthread_mutex_lock(&f->lock);
    cancel_locked(f);
    pthread_mutex_unlock(&f->lock);
}

void fx_set_enabled(fx_t *f, bool on)
{
    pthread_mutex_lock(&f->lock);
    if (f->enabled && !on)
        cancel_locked(f);
    f->enabled = on;
    pthread_mutex_unlock(&f->lock);
}

void fx_shutdown(fx_t *f)
{
    pthread_mutex_lock(&f->lock);
    f->shutdown = true;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);
}

void fx_join(fx_t *f)
{
    pthread_join(f->thread, NULL);
    pthread_cond_destroy(&f->cond);
    pthread_mutex_destroy(&f->lock);
}
