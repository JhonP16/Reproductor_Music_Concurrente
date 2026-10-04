/*
 * dec_wav.c - Parser RIFF/WAVE propio. Soporta PCM 8/16/24/32 bits,
 * IEEE float 32 y WAVE_FORMAT_EXTENSIBLE; 1..N canales (se convierte a
 * estéreo int16).
 */
#include "source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAV_FMT_PCM        1
#define WAV_FMT_FLOAT      3
#define WAV_FMT_EXTENSIBLE 0xFFFE

typedef struct {
    FILE    *f;
    uint16_t fmt;
    uint16_t channels;
    uint16_t bits;
    uint16_t block_align;
    long     data_off;
    uint64_t data_frames;
    uint64_t pos;               /* frame actual */
    uint8_t *raw;               /* búfer de lectura cruda */
    size_t   raw_cap;
} wav_t;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Convierte una muestra cruda a int16. */
static int16_t sample_at(const wav_t *w, const uint8_t *p)
{
    switch (w->bits) {
    case 8:  return (int16_t)(((int)p[0] - 128) * 256);
    case 16: return (int16_t)le16(p);
    case 24: return (int16_t)(p[1] | p[2] << 8);
    case 32:
        if (w->fmt == WAV_FMT_FLOAT) {
            float f;
            uint32_t u = le32(p);
            memcpy(&f, &u, sizeof f);
            if (f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            return (int16_t)(f * 32767.0f);
        }
        return (int16_t)(le32(p) >> 16);
    default: return 0;
    }
}

static long wav_read(source_t *s, int16_t *out, size_t frames)
{
    wav_t *w = s->impl;
    uint64_t left = w->data_frames - w->pos;
    if (frames > left)
        frames = (size_t)left;
    if (frames == 0)
        return 0;
    size_t need = frames * w->block_align;
    if (need > w->raw_cap) {
        uint8_t *nr = realloc(w->raw, need);
        if (!nr)
            return -1;
        w->raw = nr;
        w->raw_cap = need;
    }
    size_t got = fread(w->raw, w->block_align, frames, w->f);
    if (got == 0)
        return ferror(w->f) ? -1 : 0;

    size_t bps = w->bits / 8;
    for (size_t i = 0; i < got; i++) {
        const uint8_t *fr = w->raw + i * w->block_align;
        int16_t l = sample_at(w, fr);
        int16_t r = w->channels > 1 ? sample_at(w, fr + bps) : l;
        out[2 * i] = l;
        out[2 * i + 1] = r;
    }
    w->pos += got;
    return (long)got;
}

static int wav_seek(source_t *s, uint64_t frame)
{
    wav_t *w = s->impl;
    if (frame > w->data_frames)
        frame = w->data_frames;
    if (fseek(w->f, w->data_off + (long)(frame * w->block_align), SEEK_SET))
        return -1;
    w->pos = frame;
    return 0;
}

static void wav_close(source_t *s)
{
    wav_t *w = s->impl;
    if (w) {
        if (w->f) fclose(w->f);
        free(w->raw);
        free(w);
    }
    free(s);
}

static const source_ops_t wav_ops = { wav_read, wav_seek, wav_close };

source_t *wav_open(const char *path, char *err, size_t errlen)
{
    source_t *s = calloc(1, sizeof *s);
    wav_t *w = calloc(1, sizeof *w);
    if (!s || !w) {
        free(s); free(w);
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    s->ops = &wav_ops;
    s->impl = w;
    s->codec = "WAV";

    uint8_t hdr[12], ck[8], fmt[40];
    bool have_fmt = false;
    if (!(w->f = fopen(path, "rb"))) {
        snprintf(err, errlen, "no se puede abrir");
        goto fail;
    }
    if (fread(hdr, 1, 12, w->f) != 12 || memcmp(hdr, "RIFF", 4) ||
        memcmp(hdr + 8, "WAVE", 4)) {
        snprintf(err, errlen, "no es RIFF/WAVE");
        goto fail;
    }
    for (;;) {                                  /* recorrer chunks */
        if (fread(ck, 1, 8, w->f) != 8) {
            snprintf(err, errlen, "falta chunk 'data'");
            goto fail;
        }
        uint32_t sz = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            size_t rd = sz < sizeof fmt ? sz : sizeof fmt;
            if (sz < 16 || fread(fmt, 1, rd, w->f) != rd) {
                snprintf(err, errlen, "chunk fmt inválido");
                goto fail;
            }
            if (sz > rd) fseek(w->f, (long)(sz - rd), SEEK_CUR);
            w->fmt = le16(fmt);
            w->channels = le16(fmt + 2);
            s->rate = le32(fmt + 4);
            w->block_align = le16(fmt + 12);
            w->bits = le16(fmt + 14);
            if (w->fmt == WAV_FMT_EXTENSIBLE && rd >= 26)
                w->fmt = le16(fmt + 24);        /* subformato GUID */
            have_fmt = true;
        } else if (!memcmp(ck, "data", 4)) {
            if (!have_fmt) {
                snprintf(err, errlen, "'data' antes de 'fmt '");
                goto fail;
            }
            w->data_off = ftell(w->f);
            w->data_frames = w->block_align ? sz / w->block_align : 0;
            break;
        } else {
            fseek(w->f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    if ((w->fmt != WAV_FMT_PCM && w->fmt != WAV_FMT_FLOAT) ||
        !(w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32) ||
        w->channels == 0 || s->rate == 0 ||
        w->block_align < w->channels * (w->bits / 8)) {
        snprintf(err, errlen, "formato WAV no soportado (fmt=%u bits=%u)",
                 w->fmt, w->bits);
        goto fail;
    }
    s->src_channels = w->channels;
    s->bits = w->bits;
    s->total_frames = w->data_frames;
    return s;

fail:
    wav_close(s);
    return NULL;
}
