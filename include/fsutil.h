/*
 * fsutil.h - Exploración de directorios (sin estado compartido).
 */
#ifndef FSUTIL_H
#define FSUTIL_H

#include "common.h"

typedef struct {
    char name[256];
    bool is_dir;
} dir_entry_t;

/* Lista `dir`: primero subdirectorios, luego archivos de audio
 * soportados, ambos en orden alfabético. Devuelve un arreglo con malloc
 * (liberar con free) y su tamaño en *n; NULL si falla. */
dir_entry_t *fs_list_dir(const char *dir, size_t *n);

/* Recorre `path` (archivo o directorio, recursivo hasta `depth`) y llama
 * cb(path, ctx) por cada archivo de audio soportado, en orden. Devuelve
 * cuántos encontró. */
size_t fs_collect(const char *path, int depth,
                  void (*cb)(const char *path, void *ctx), void *ctx);

bool fs_is_dir(const char *path);

#endif
