/*
 * playlist.h - Lista de reproducción compartida (Lectores-Escritores).
 *
 * Protegida por un pthread_rwlock_t con preferencia a escritores:
 *   - Lectores (decodificador preparando la siguiente pista, controlador,
 *     UI dibujando la lista) toman rdlock y pueden ejecutarse en paralelo.
 *   - Escritores (usuario: agregar, eliminar, mover, limpiar, mezclar)
 *     toman wrlock en exclusión mutua.
 *
 * Regla de oro anti use-after-free: NINGÚN puntero a nodo sale de este
 * módulo. Toda consulta copia los datos (track_info_t) mientras se tiene
 * el lock; las pistas se identifican fuera por su `id` único y monótono.
 * Así, eliminar un nodo (incluso el que suena) es siempre seguro.
 */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include "common.h"
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    uint64_t id;
    char     path[MAX_PATH_LEN];
    char     title[MAX_TITLE_LEN];
} track_info_t;

typedef struct pl_node pl_node_t;

typedef struct {
    pthread_rwlock_t rw;
    pl_node_t       *head;
    pl_node_t       *tail;
    size_t           count;
    uint64_t         next_id;
    unsigned int     rng;            /* semilla de shuffle (bajo wrlock) */
    atomic_uint_fast64_t version;    /* cambia en cada escritura         */
} playlist_t;

int      playlist_init(playlist_t *pl);
void     playlist_destroy(playlist_t *pl);

/* ---- Escritores (wrlock) ---- */
uint64_t playlist_add(playlist_t *pl, const char *path);      /* 0 = error */
bool     playlist_remove(playlist_t *pl, uint64_t id);
bool     playlist_move(playlist_t *pl, uint64_t id, int delta); /* ±1 */
void     playlist_clear(playlist_t *pl);
void     playlist_shuffle(playlist_t *pl);

/* ---- Lectores (rdlock) ---- */
size_t   playlist_count(playlist_t *pl);
uint64_t playlist_version(playlist_t *pl);
/* Copia la pista `id`. *index recibe su posición. */
bool     playlist_get(playlist_t *pl, uint64_t id, track_info_t *out,
                      size_t *index);
bool     playlist_get_index(playlist_t *pl, size_t index, track_info_t *out);
/*
 * Pista siguiente/anterior a `id`. Si `id` ya no existe (fue eliminada
 * mientras sonaba) se usa `hint_index`, la última posición conocida de esa
 * pista: tras el borrado, el elemento que ocupa esa posición es justamente
 * el que la seguía. *out_index recibe la posición de la pista devuelta.
 */
bool     playlist_next_after(playlist_t *pl, uint64_t id, size_t hint_index,
                             track_info_t *out, size_t *out_index);
bool     playlist_prev_before(playlist_t *pl, uint64_t id, size_t hint_index,
                              track_info_t *out, size_t *out_index);
/* Copia hasta `max` pistas a partir de `offset`. Devuelve cuántas copió;
 * *total recibe el tamaño de la lista en ese instante (consistente). */
size_t   playlist_snapshot(playlist_t *pl, size_t offset, track_info_t *out,
                           size_t max, size_t *total);

/* Utilidad: título legible a partir de la ruta. */
void     title_from_path(const char *path, char *title, size_t len);

#endif
