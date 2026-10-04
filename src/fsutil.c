/*
 * fsutil.c - Utilidades de sistema de archivos.
 */
#include "fsutil.h"
#include "source.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

bool fs_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int cmp_entry(const void *a, const void *b)
{
    const dir_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

dir_entry_t *fs_list_dir(const char *dir, size_t *n)
{
    DIR *d = opendir(dir);
    *n = 0;
    if (!d)
        return NULL;
    size_t cap = 64;
    dir_entry_t *v = malloc(cap * sizeof *v);
    char full[MAX_PATH_LEN];
    struct dirent *e;
    while (v && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;                       /* ocultos, "." y ".." */
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        bool is_dir = fs_is_dir(full);
        if (!is_dir && !source_is_supported(e->d_name))
            continue;
        if (*n == cap) {
            dir_entry_t *nv = realloc(v, (cap *= 2) * sizeof *v);
            if (!nv) { free(v); v = NULL; break; }
            v = nv;
        }
        snprintf(v[*n].name, sizeof v[*n].name, "%s", e->d_name);
        v[*n].is_dir = is_dir;
        (*n)++;
    }
    closedir(d);
    if (v)
        qsort(v, *n, sizeof *v, cmp_entry);
    else
        *n = 0;
    return v;
}

size_t fs_collect(const char *path, int depth,
                  void (*cb)(const char *path, void *ctx), void *ctx)
{
    if (!fs_is_dir(path)) {
        if (source_is_supported(path)) {
            cb(path, ctx);
            return 1;
        }
        return 0;
    }
    if (depth < 0)
        return 0;
    size_t n, found = 0;
    dir_entry_t *v = fs_list_dir(path, &n);
    char full[MAX_PATH_LEN];
    for (size_t i = 0; i < n; i++) {
        size_t plen = strlen(path);
        snprintf(full, sizeof full, "%s%s%s", path,
                 plen && path[plen - 1] == '/' ? "" : "/", v[i].name);
        found += fs_collect(full, depth - 1, cb, ctx);
    }
    free(v);
    return found;
}
