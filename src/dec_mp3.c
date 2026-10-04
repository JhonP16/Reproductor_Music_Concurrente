/*
 * dec_mp3.c - Fuente MP3 sobre libmpg123 (solo decodificación; no aporta
 * ninguna sincronización). Salida forzada a int16 estéreo.
 */
#include "source.h"

#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    mpg123_handle *mh;
} mp3_t;

int mp3_global_init(void)
{
    return mpg123_init() == MPG123_OK ? 0 : -1;
}

void mp3_global_exit(void)
{
#if MPG123_API_VERSION < 46
    mpg123_exit();
#endif
}

static long mp3_read(source_t *s, int16_t *out, size_t frames)
{
    mp3_t *m = s->impl;
    size_t bytes = frames * 2 * sizeof(int16_t), done = 0;
    for (;;) {
        int rc = mpg123_read(m->mh, out, bytes, &done);
        if (rc == MPG123_OK || rc == MPG123_DONE || done > 0)
            return (long)(done / (2 * sizeof(int16_t)));
        if (rc == MPG123_NEW_FORMAT)
            continue;            /* formato fijado a s16 estéreo: seguir */
        return -1;
    }
}

static int mp3_seek(source_t *s, uint64_t frame)
{
    mp3_t *m = s->impl;
    return mpg123_seek(m->mh, (off_t)frame, SEEK_SET) < 0 ? -1 : 0;
}

static void mp3_close(source_t *s)
{
    mp3_t *m = s->impl;
    if (m) {
        if (m->mh) {
            mpg123_close(m->mh);
            mpg123_delete(m->mh);
        }
        free(m);
    }
    free(s);
}

static const source_ops_t mp3_ops = { mp3_read, mp3_seek, mp3_close };

static void copy_tag(char *dst, size_t len, const mpg123_string *src)
{
    if (src && src->p && src->fill)
        snprintf(dst, len, "%s", src->p);
}

source_t *mp3_open(const char *path, char *err, size_t errlen)
{
    source_t *s = calloc(1, sizeof *s);
    mp3_t *m = calloc(1, sizeof *m);
    int rc;
    if (!s || !m) {
        free(s); free(m);
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    s->ops = &mp3_ops;
    s->impl = m;
    s->codec = "MP3";

    m->mh = mpg123_new(NULL, &rc);
    if (!m->mh) {
        snprintf(err, errlen, "mpg123_new: %s", mpg123_plain_strerror(rc));
        goto fail;
    }
    mpg123_param(m->mh, MPG123_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET, 0);
    mpg123_format_none(m->mh);
    const long *rates;
    size_t nrates;
    mpg123_rates(&rates, &nrates);
    for (size_t i = 0; i < nrates; i++)
        mpg123_format(m->mh, rates[i], MPG123_STEREO, MPG123_ENC_SIGNED_16);

    if (mpg123_open(m->mh, path) != MPG123_OK) {
        snprintf(err, errlen, "%s", mpg123_strerror(m->mh));
        goto fail;
    }
    long rate;
    int ch, enc;
    if (mpg123_getformat(m->mh, &rate, &ch, &enc) != MPG123_OK) {
        snprintf(err, errlen, "formato MP3 inválido");
        goto fail;
    }
    mpg123_scan(m->mh);                 /* duración exacta (sin lock) */
    off_t len = mpg123_length(m->mh);
    s->rate = (uint32_t)rate;
    s->total_frames = len > 0 ? (uint64_t)len : 0;
    s->bits = 16;

    struct mpg123_frameinfo fi;
    s->src_channels = 2;
    if (mpg123_info(m->mh, &fi) == MPG123_OK)
        s->src_channels = fi.mode == MPG123_M_MONO ? 1 : 2;

    mpg123_id3v1 *v1 = NULL;
    mpg123_id3v2 *v2 = NULL;
    if (mpg123_id3(m->mh, &v1, &v2) == MPG123_OK) {
        if (v2) {
            copy_tag(s->title, sizeof s->title, v2->title);
            copy_tag(s->artist, sizeof s->artist, v2->artist);
        }
        if (v1 && !s->title[0])
            snprintf(s->title, sizeof s->title, "%.30s", v1->title);
        if (v1 && !s->artist[0])
            snprintf(s->artist, sizeof s->artist, "%.30s", v1->artist);
    }
    return s;

fail:
    mp3_close(s);
    return NULL;
}
