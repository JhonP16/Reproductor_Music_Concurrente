/*
 * playlist.c - Lista doblemente enlazada protegida con pthread_rwlock_t.
 */
#include "playlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct pl_node {
    uint64_t   id;
    char      *path;
    char      *title;
    pl_node_t *prev;
    pl_node_t *next;
};

/* ------------------------------------------------------------------ */

void title_from_path(const char *path, char *title, size_t len)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(title, len, "%s", base);
    char *dot = strrchr(title, '.');
    if (dot && dot != title)
        *dot = '\0';
    for (char *p = title; *p; p++)
        if (*p == '_')
            *p = ' ';
}

static void copy_info(const pl_node_t *n, track_info_t *out)
{
    out->id = n->id;
    snprintf(out->path, sizeof out->path, "%s", n->path);
    snprintf(out->title, sizeof out->title, "%s", n->title);
}

static void free_node(pl_node_t *n)
{
    free(n->path);
    free(n->title);
    free(n);
}

static void unlink_node(playlist_t *pl, pl_node_t *n)
{
    if (n->prev) n->prev->next = n->next; else pl->head = n->next;
    if (n->next) n->next->prev = n->prev; else pl->tail = n->prev;
    n->prev = n->next = NULL;
    pl->count--;
}

/* Busca por id. Requiere lock (rd o wr). */
static pl_node_t *find(playlist_t *pl, uint64_t id, size_t *index)
{
    size_t i = 0;
    for (pl_node_t *n = pl->head; n; n = n->next, i++) {
        if (n->id == id) {
            if (index) *index = i;
            return n;
        }
    }
    return NULL;
}

static pl_node_t *at(playlist_t *pl, size_t index)
{
    pl_node_t *n = pl->head;
    while (n && index--)
        n = n->next;
    return n;
}

static void bump_version(playlist_t *pl)
{
    atomic_fetch_add(&pl->version, 1);
}

/* ------------------------------------------------------------------ */

int playlist_init(playlist_t *pl)
{
    memset(pl, 0, sizeof *pl);
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    /* Preferencia a escritores: con lectores frecuentes (UI a 30 fps,
     * decodificador) un comando del usuario no debe sufrir inanición. */
    pthread_rwlockattr_setkind_np(&attr,
                                  PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    int rc = pthread_rwlock_init(&pl->rw, &attr);
    pthread_rwlockattr_destroy(&attr);
    pl->next_id = 1;
    pl->rng = (unsigned)time(NULL);
    return rc == 0 ? 0 : -1;
}

void playlist_destroy(playlist_t *pl)
{
    playlist_clear(pl);
    pthread_rwlock_destroy(&pl->rw);
}

uint64_t playlist_add(playlist_t *pl, const char *path)
{
    /* Reservar memoria FUERA de la sección crítica. */
    pl_node_t *n = calloc(1, sizeof *n);
    char title[MAX_TITLE_LEN];
    title_from_path(path, title, sizeof title);
    if (!n || !(n->path = strdup(path)) || !(n->title = strdup(title))) {
        if (n) { free(n->path); free(n); }
        return 0;
    }

    pthread_rwlock_wrlock(&pl->rw);
    n->id = pl->next_id++;
    n->prev = pl->tail;
    if (pl->tail) pl->tail->next = n; else pl->head = n;
    pl->tail = n;
    pl->count++;
    uint64_t id = n->id;
    bump_version(pl);
    pthread_rwlock_unlock(&pl->rw);
    return id;
}

bool playlist_remove(playlist_t *pl, uint64_t id)
{
    pthread_rwlock_wrlock(&pl->rw);
    pl_node_t *n = find(pl, id, NULL);
    if (n) {
        unlink_node(pl, n);
        bump_version(pl);
    }
    pthread_rwlock_unlock(&pl->rw);
    /* Nadie más puede tener un puntero a n: liberar sin lock. */
    if (n)
        free_node(n);
    return n != NULL;
}

bool playlist_move(playlist_t *pl, uint64_t id, int delta)
{
    bool ok = false;
    pthread_rwlock_wrlock(&pl->rw);
    pl_node_t *n = find(pl, id, NULL);
    if (n && delta < 0 && n->prev) {
        pl_node_t *p = n->prev;            /* ... A p n B ... -> A n p B */
        unlink_node(pl, n);
        n->next = p;
        n->prev = p->prev;
        if (p->prev) p->prev->next = n; else pl->head = n;
        p->prev = n;
        pl->count++;
        ok = true;
    } else if (n && delta > 0 && n->next) {
        pl_node_t *q = n->next;            /* ... A n q B ... -> A q n B */
        unlink_node(pl, n);
        n->prev = q;
        n->next = q->next;
        if (q->next) q->next->prev = n; else pl->tail = n;
        q->next = n;
        pl->count++;
        ok = true;
    }
    if (ok)
        bump_version(pl);
    pthread_rwlock_unlock(&pl->rw);
    return ok;
}

void playlist_clear(playlist_t *pl)
{
    pthread_rwlock_wrlock(&pl->rw);
    pl_node_t *list = pl->head;          /* desenganchar toda la lista... */
    pl->head = pl->tail = NULL;
    pl->count = 0;
    bump_version(pl);
    pthread_rwlock_unlock(&pl->rw);

    while (list) {                       /* ...y liberarla fuera del lock */
        pl_node_t *next = list->next;
        free_node(list);
        list = next;
    }
}

void playlist_shuffle(playlist_t *pl)
{
    pthread_rwlock_wrlock(&pl->rw);
    size_t n = pl->count;
    pl_node_t **v = n > 1 ? malloc(n * sizeof *v) : NULL;
    if (v) {
        size_t i = 0;
        for (pl_node_t *x = pl->head; x; x = x->next)
            v[i++] = x;
        for (i = n - 1; i > 0; i--) {     /* Fisher-Yates */
            size_t j = (size_t)rand_r(&pl->rng) % (i + 1);
            pl_node_t *t = v[i]; v[i] = v[j]; v[j] = t;
        }
        for (i = 0; i < n; i++) {
            v[i]->prev = i ? v[i - 1] : NULL;
            v[i]->next = i + 1 < n ? v[i + 1] : NULL;
        }
        pl->head = v[0];
        pl->tail = v[n - 1];
        bump_version(pl);
    }
    pthread_rwlock_unlock(&pl->rw);
    free(v);
}

/* ------------------------------------------------------------------ */

size_t playlist_count(playlist_t *pl)
{
    pthread_rwlock_rdlock(&pl->rw);
    size_t n = pl->count;
    pthread_rwlock_unlock(&pl->rw);
    return n;
}

uint64_t playlist_version(playlist_t *pl)
{
    return atomic_load(&pl->version);
}

bool playlist_get(playlist_t *pl, uint64_t id, track_info_t *out,
                  size_t *index)
{
    pthread_rwlock_rdlock(&pl->rw);
    pl_node_t *n = find(pl, id, index);
    if (n)
        copy_info(n, out);
    pthread_rwlock_unlock(&pl->rw);
    return n != NULL;
}

bool playlist_get_index(playlist_t *pl, size_t index, track_info_t *out)
{
    pthread_rwlock_rdlock(&pl->rw);
    pl_node_t *n = at(pl, index);
    if (n)
        copy_info(n, out);
    pthread_rwlock_unlock(&pl->rw);
    return n != NULL;
}

bool playlist_next_after(playlist_t *pl, uint64_t id, size_t hint_index,
                         track_info_t *out, size_t *out_index)
{
    pthread_rwlock_rdlock(&pl->rw);
    size_t idx = 0;
    pl_node_t *cur = find(pl, id, &idx);
    pl_node_t *n;
    if (cur) {
        n = cur->next;
        idx++;
    } else {                          /* la pista actual fue eliminada */
        idx = hint_index;
        n = at(pl, idx);
    }
    if (n) {
        copy_info(n, out);
        if (out_index) *out_index = idx;
    }
    pthread_rwlock_unlock(&pl->rw);
    return n != NULL;
}

bool playlist_prev_before(playlist_t *pl, uint64_t id, size_t hint_index,
                          track_info_t *out, size_t *out_index)
{
    pthread_rwlock_rdlock(&pl->rw);
    size_t idx = 0;
    pl_node_t *cur = find(pl, id, &idx);
    pl_node_t *n = NULL;
    if (cur) {
        n = cur->prev;
        if (n) idx--;
    } else if (hint_index > 0 && pl->count > 0) {
        idx = hint_index - 1 < pl->count ? hint_index - 1 : pl->count - 1;
        n = at(pl, idx);
    }
    if (n) {
        copy_info(n, out);
        if (out_index) *out_index = idx;
    }
    pthread_rwlock_unlock(&pl->rw);
    return n != NULL;
}

size_t playlist_snapshot(playlist_t *pl, size_t offset, track_info_t *out,
                         size_t max, size_t *total)
{
    pthread_rwlock_rdlock(&pl->rw);
    if (total)
        *total = pl->count;
    size_t k = 0;
    for (pl_node_t *n = at(pl, offset); n && k < max; n = n->next)
        copy_info(n, &out[k++]);
    pthread_rwlock_unlock(&pl->rw);
    return k;
}
