/*
 * source.h - Fuente de audio abstracta (decodificador de formato).
 *
 * Una fuente NO es compartida entre hilos: solo el hilo decodificador la
 * crea, lee y destruye, así que no necesita sincronización propia.
 * Siempre entrega PCM int16 estéreo intercalado.
 */
#ifndef SOURCE_H
#define SOURCE_H

#include "common.h"

typedef struct source source_t;

typedef struct {
    long (*read)(source_t *s, int16_t *out, size_t frames); /* 0=EOF <0=err */
    int  (*seek)(source_t *s, uint64_t frame);
    void (*close)(source_t *s);
} source_ops_t;

struct source {
    const source_ops_t *ops;
    const char *codec;          /* "WAV" / "MP3" */
    uint32_t    rate;
    uint16_t    src_channels;   /* canales del archivo original */
    uint16_t    bits;           /* bits por muestra originales  */
    uint64_t    total_frames;   /* 0 si se desconoce            */
    char        title[MAX_TITLE_LEN];
    char        artist[MAX_TITLE_LEN];
    void       *impl;
};

/* Abre según extensión/cabecera. Devuelve NULL y rellena err si falla. */
source_t *source_open(const char *path, char *err, size_t errlen);
static inline long source_read(source_t *s, int16_t *o, size_t n)
{ return s->ops->read(s, o, n); }
static inline int  source_seek(source_t *s, uint64_t f)
{ return s->ops->seek(s, f); }
void source_close(source_t *s);

/* Implementaciones concretas. */
source_t *wav_open(const char *path, char *err, size_t errlen);
source_t *mp3_open(const char *path, char *err, size_t errlen);
int       mp3_global_init(void);   /* llamar una vez desde main */
void      mp3_global_exit(void);

bool      source_is_supported(const char *path);

#endif
