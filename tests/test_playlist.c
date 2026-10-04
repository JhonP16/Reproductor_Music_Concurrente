/*
 * test_playlist.c - Estrés Lectores-Escritores sobre la playlist.
 *   - 4 lectores: recorren la lista con next_after/prev_before/snapshot
 *     (como el decodificador y la UI) y validan consistencia.
 *   - 3 escritores: add/remove/move/clear/shuffle aleatorios.
 * Con ASan/TSan detecta use-after-free, carreras y corrupción.
 */
#include "playlist.h"
#include "evlog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static playlist_t pl;
static atomic_int stop_flag;
static atomic_uint_fast64_t reads, writes;

static void *reader(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    track_info_t t, *snap = malloc(64 * sizeof *snap);
    uint64_t cur = 0;
    size_t idx = 0;
    while (!atomic_load(&stop_flag)) {
        switch (rand_r(&seed) % 3) {
        case 0:
            if (playlist_next_after(&pl, cur, idx, &t, &idx)) {
                assert(t.id > 0 && strncmp(t.path, "/music/", 7) == 0);
                cur = t.id;
            } else {
                cur = 0;
                idx = 0;
                if (playlist_get_index(&pl, 0, &t))
                    cur = t.id;
            }
            break;
        case 1:
            if (playlist_prev_before(&pl, cur, idx, &t, &idx))
                cur = t.id;
            break;
        default: {
            size_t total = 0;
            size_t n = playlist_snapshot(&pl, 0, snap, 64, &total);
            assert(n <= total && n <= 64);
            for (size_t i = 1; i < n; i++)
                assert(snap[i].id != snap[i - 1].id);
        }
        }
        atomic_fetch_add(&reads, 1);
    }
    free(snap);
    return NULL;
}

static void *writer(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    char path[64];
    track_info_t t;
    while (!atomic_load(&stop_flag)) {
        int op = rand_r(&seed) % 100;
        size_t n = playlist_count(&pl);
        if (op < 45 || n == 0) {
            snprintf(path, sizeof path, "/music/song_%u.mp3", rand_r(&seed));
            assert(playlist_add(&pl, path) != 0);
        } else if (op < 75) {
            if (playlist_get_index(&pl, (size_t)rand_r(&seed) % n, &t))
                playlist_remove(&pl, t.id);
        } else if (op < 95) {
            if (playlist_get_index(&pl, (size_t)rand_r(&seed) % n, &t))
                playlist_move(&pl, t.id, (rand_r(&seed) & 1) ? 1 : -1);
        } else if (op < 98) {
            playlist_shuffle(&pl);
        } else if (n > 200) {
            playlist_clear(&pl);
        }
        atomic_fetch_add(&writes, 1);
    }
    return NULL;
}

int main(void)
{
    evlog_init();
    assert(playlist_init(&pl) == 0);

    /* Pruebas deterministas básicas. */
    uint64_t a = playlist_add(&pl, "/music/a.mp3");
    uint64_t b = playlist_add(&pl, "/music/b.wav");
    uint64_t c = playlist_add(&pl, "/music/c.mp3");
    track_info_t t;
    size_t idx;
    assert(playlist_next_after(&pl, a, 0, &t, &idx) && t.id == b && idx == 1);
    assert(playlist_move(&pl, c, -1));                 /* a c b */
    assert(playlist_get_index(&pl, 1, &t) && t.id == c);
    assert(playlist_remove(&pl, c));                   /* a b, c borrada */
    /* La pista que sonaba (c, idx 1) desapareció: la siguiente es b. */
    assert(playlist_next_after(&pl, c, 1, &t, &idx) && t.id == b);
    assert(playlist_prev_before(&pl, c, 1, &t, &idx) && t.id == a);
    assert(strcmp(t.title, "a") == 0);
    playlist_clear(&pl);
    assert(playlist_count(&pl) == 0);

    pthread_t r[4], w[3];
    for (uintptr_t i = 0; i < 4; i++)
        pthread_create(&r[i], NULL, reader, (void *)(i + 1));
    for (uintptr_t i = 0; i < 3; i++)
        pthread_create(&w[i], NULL, writer, (void *)(i + 100));
    sleep(2);
    atomic_store(&stop_flag, 1);
    for (int i = 0; i < 4; i++) pthread_join(r[i], NULL);
    for (int i = 0; i < 3; i++) pthread_join(w[i], NULL);

    printf("test_playlist OK: %lu lecturas, %lu escrituras, %zu pistas al final\n",
           (unsigned long)atomic_load(&reads),
           (unsigned long)atomic_load(&writes), playlist_count(&pl));
    playlist_destroy(&pl);
    return 0;
}
