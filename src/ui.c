/*
 * ui.c - Interfaz de terminal (ncursesw) del reproductor.
 *
 * Reglas de concurrencia de la UI:
 *   - ncurses NO es thread-safe: solo este hilo (main) lo usa.
 *   - La UI nunca modifica directamente el estado del reproductor: envía
 *     comandos a la cola (no bloqueante) o llama a las operaciones de la
 *     playlist, que toman su propio wrlock.
 *   - Todo lo que dibuja proviene de COPIAS tomadas con los locks de cada
 *     módulo (snapshot), nunca de punteros a estructuras compartidas.
 *   - Espera eventos con poll() sobre stdin + signalfd con timeout de
 *     33 ms (refresco ~30 fps): el hilo duerme en el kernel, no gira.
 */
#define NCURSES_WIDECHAR 1
#include "app.h"
#include "evlog.h"
#include "fsutil.h"
#include "viz.h"

#include <curses.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Colores                                                             */
/* ------------------------------------------------------------------ */

enum {
    C_BORDER = 1, C_TITLE, C_TEXT, C_DIM, C_ACCENT, C_CURRENT, C_CURSOR,
    C_OK, C_WARN, C_ERR, C_INFO, C_SYNC, C_KEY, C_PAUSE, C_BADGE_PLAY,
    C_BADGE_PAUSE, C_BADGE_STOP, C_SLOT_FREE, C_HEAD,
    C_GRAD = 30,          /* 10 pares: gradiente verde -> rojo */
    C_LOGO = 50,          /* NLOGO pares: gradiente del logo   */
};
#define NGRAD 10
#define NLOGO 18

static bool g_256;

static void init_colors(void)
{
    if (!has_colors())
        return;
    start_color();
    use_default_colors();
    g_256 = COLORS >= 256;

    if (g_256) {
        init_pair(C_BORDER, 61, -1);
        init_pair(C_TITLE, 213, -1);
        init_pair(C_TEXT, 252, -1);
        init_pair(C_DIM, 243, -1);
        init_pair(C_ACCENT, 45, -1);
        init_pair(C_CURRENT, 48, -1);
        init_pair(C_CURSOR, 231, 61);
        init_pair(C_OK, 84, -1);
        init_pair(C_WARN, 221, -1);
        init_pair(C_ERR, 203, -1);
        init_pair(C_INFO, 117, -1);
        init_pair(C_SYNC, 177, -1);
        init_pair(C_KEY, 16, 45);
        init_pair(C_PAUSE, 75, -1);
        init_pair(C_BADGE_PLAY, 16, 48);
        init_pair(C_BADGE_PAUSE, 16, 221);
        init_pair(C_BADGE_STOP, 231, 203);
        init_pair(C_SLOT_FREE, 238, -1);
        init_pair(C_HEAD, 231, -1);
        static const short grad[NGRAD] = { 46, 82, 118, 154, 190, 226,
                                           220, 214, 208, 196 };
        for (int i = 0; i < NGRAD; i++)
            init_pair((short)(C_GRAD + i), grad[i], -1);
        static const short logo[NLOGO] = { 51, 45, 39, 33, 27, 63, 99, 135,
                                           171, 207, 206, 205, 170, 134,
                                           98, 62, 69, 75 };
        for (int i = 0; i < NLOGO; i++)
            init_pair((short)(C_LOGO + i), logo[i], -1);
    } else {
        init_pair(C_BORDER, COLOR_BLUE, -1);
        init_pair(C_TITLE, COLOR_MAGENTA, -1);
        init_pair(C_TEXT, COLOR_WHITE, -1);
        init_pair(C_DIM, COLOR_WHITE, -1);
        init_pair(C_ACCENT, COLOR_CYAN, -1);
        init_pair(C_CURRENT, COLOR_GREEN, -1);
        init_pair(C_CURSOR, COLOR_WHITE, COLOR_BLUE);
        init_pair(C_OK, COLOR_GREEN, -1);
        init_pair(C_WARN, COLOR_YELLOW, -1);
        init_pair(C_ERR, COLOR_RED, -1);
        init_pair(C_INFO, COLOR_CYAN, -1);
        init_pair(C_SYNC, COLOR_MAGENTA, -1);
        init_pair(C_KEY, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_PAUSE, COLOR_BLUE, -1);
        init_pair(C_BADGE_PLAY, COLOR_BLACK, COLOR_GREEN);
        init_pair(C_BADGE_PAUSE, COLOR_BLACK, COLOR_YELLOW);
        init_pair(C_BADGE_STOP, COLOR_WHITE, COLOR_RED);
        init_pair(C_SLOT_FREE, COLOR_BLUE, -1);
        init_pair(C_HEAD, COLOR_WHITE, -1);
        for (int i = 0; i < NGRAD; i++)
            init_pair((short)(C_GRAD + i),
                      i < 5 ? COLOR_GREEN : i < 8 ? COLOR_YELLOW : COLOR_RED,
                      -1);
        static const short logo[3] = { COLOR_CYAN, COLOR_BLUE, COLOR_MAGENTA };
        for (int i = 0; i < NLOGO; i++)
            init_pair((short)(C_LOGO + i), logo[i / 6], -1);
    }
}

#define CP(c) COLOR_PAIR(c)

/* ------------------------------------------------------------------ */
/* Estado de la UI (solo lo toca el hilo principal)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    app_t        *a;
    int           W, H;
    bool          running;

    /* Copia local de la playlist (se refresca cuando cambia la versión) */
    track_info_t *pl;
    size_t        pl_n, pl_cap;
    uint64_t      pl_ver;
    size_t        cursor, scroll;
    uint64_t      cursor_id;

    /* Navegador de archivos */
    bool          browsing;
    char          cwd[MAX_PATH_LEN];
    dir_entry_t  *ent;
    size_t        n_ent, bcur, bscroll;

    bool          confirm_clear;
    bool          show_help;

    spectrum_t    spec;
    float         vu[2], vu_peak[2];
    uint64_t      viz_serial;
    int           stale_frames;

    char          flash[160];
    double        flash_until;
    double        fps, fps_t0;
    int           fps_frames;
    uint64_t      frame;
} ui_t;

static void ui_flash(ui_t *u, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void ui_flash(ui_t *u, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->flash, sizeof u->flash, fmt, ap);
    va_end(ap);
    u->flash_until = now_seconds() + 2.5;
}

/* ------------------------------------------------------------------ */
/* Primitivas de dibujo                                                */
/* ------------------------------------------------------------------ */

/* Escribe `s` (UTF-8) recortado a `maxw` columnas. Devuelve el ancho. */
static int put(int y, int x, int maxw, const char *s)
{
    if (maxw <= 0)
        return 0;
    wchar_t wb[600];
    size_t n = mbstowcs(wb, s, ARRAY_LEN(wb) - 1);
    if (n == (size_t)-1) {
        mvaddnstr(y, x, s, maxw);
        return (int)strnlen(s, (size_t)maxw);
    }
    int w = 0;
    size_t i = 0;
    for (; i < n; i++) {
        int cw = wcwidth(wb[i]);
        if (cw < 0) cw = 1;
        if (w + cw > maxw) break;
        w += cw;
    }
    mvaddnwstr(y, x, wb, (int)i);
    return w;
}

static int putf(int y, int x, int maxw, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int putf(int y, int x, int maxw, const char *fmt, ...)
{
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return put(y, x, maxw, buf);
}

static void hline_str(int y, int x, int n, const char *s)
{
    for (int i = 0; i < n; i++)
        mvaddstr(y, x + i, s);
}

/* Caja con esquinas redondeadas y título incrustado. */
static void box_titled(int y, int x, int h, int w, const char *icon,
                       const char *title, const char *right)
{
    if (h < 2 || w < 4)
        return;
    attron(CP(C_BORDER));
    mvaddstr(y, x, "╭");
    hline_str(y, x + 1, w - 2, "─");
    mvaddstr(y, x + w - 1, "╮");
    for (int i = 1; i < h - 1; i++) {
        mvaddstr(y + i, x, "│");
        mvaddstr(y + i, x + w - 1, "│");
    }
    mvaddstr(y + h - 1, x, "╰");
    hline_str(y + h - 1, x + 1, w - 2, "─");
    mvaddstr(y + h - 1, x + w - 1, "╯");
    attroff(CP(C_BORDER));

    if (title) {
        attron(CP(C_BORDER));
        mvaddstr(y, x + 2, "┤");
        attroff(CP(C_BORDER));
        int cx = x + 3;
        attron(CP(C_ACCENT) | A_BOLD);
        cx += putf(y, cx, w - 8, " %s ", icon ? icon : "");
        attroff(CP(C_ACCENT) | A_BOLD);
        attron(CP(C_TITLE) | A_BOLD);
        cx += putf(y, cx, w - (cx - x) - 4, "%s ", title);
        attroff(CP(C_TITLE) | A_BOLD);
        attron(CP(C_BORDER));
        mvaddstr(y, cx, "├");
        attroff(CP(C_BORDER));
    }
    if (right) {
        char buf[128];
        snprintf(buf, sizeof buf, " %s ", right);
        int rw = (int)mbstowcs(NULL, buf, 0);
        if (rw > 0 && rw < w - 20) {
            attron(CP(C_DIM));
            put(y, x + w - 3 - rw, rw, buf);
            attroff(CP(C_DIM));
        }
    }
}

static void fmt_time(char *b, size_t n, uint64_t frames, uint32_t rate)
{
    if (!rate) { snprintf(b, n, "--:--"); return; }
    uint64_t s = frames / rate;
    snprintf(b, n, "%02llu:%02llu", (unsigned long long)(s / 60),
             (unsigned long long)(s % 60));
}

/* ------------------------------------------------------------------ */
/* Logo                                                                */
/* ------------------------------------------------------------------ */

typedef struct { char c; const char *row[3]; } glyph_t;
static const glyph_t FONT[] = {
    { 'C', { "█▀▀", "█  ", "▀▀▀" } }, { 'O', { "█▀█", "█ █", "▀▀▀" } },
    { 'N', { "█▄ █", "█ ▀█", "▀  ▀" } }, { 'U', { "█ █", "█ █", "▀▀▀" } },
    { 'P', { "█▀█", "█▀▀", "▀  " } }, { 'L', { "█  ", "█  ", "▀▀▀" } },
    { 'A', { "▄▀█", "█▀█", "▀ ▀" } }, { 'Y', { "█ █", "▀█▀", " ▀ " } },
    { 'E', { "█▀▀", "█▀▀", "▀▀▀" } }, { 'R', { "█▀█", "█▀▄", "▀ ▀" } },
    { ' ', { " ", " ", " " } },
};

static const glyph_t *glyph(char c)
{
    for (size_t i = 0; i < ARRAY_LEN(FONT); i++)
        if (FONT[i].c == c)
            return &FONT[i];
    return &FONT[ARRAY_LEN(FONT) - 1];
}

/* Dibuja el logo con un degradado animado columna a columna. */
static int draw_logo(ui_t *u, int y, int x)
{
    const char *word = "CONCU PLAYER";
    int col = 0;
    for (const char *p = word; *p; p++) {
        const glyph_t *g = glyph(*p);
        int gw = (int)mbstowcs(NULL, g->row[0], 0);
        for (int r = 0; r < 3; r++) {
            wchar_t wb[8];
            size_t n = mbstowcs(wb, g->row[r], 7);
            for (size_t k = 0; k < n; k++) {
                int ci = (int)((col + (int)k + (int)(u->frame / 2) + r) %
                               (NLOGO * 2));
                if (ci >= NLOGO) ci = NLOGO * 2 - 1 - ci;   /* ida y vuelta */
                attron(CP(C_LOGO + ci) | A_BOLD);
                mvaddnwstr(y + r, x + col + (int)k, &wb[k], 1);
                attroff(CP(C_LOGO + ci) | A_BOLD);
            }
        }
        col += gw + 1;
    }
    return col;
}

/* ------------------------------------------------------------------ */
/* Playlist local                                                      */
/* ------------------------------------------------------------------ */

static void refresh_playlist(ui_t *u)
{
    uint64_t v = playlist_version(&u->a->playlist);
    if (v == u->pl_ver && u->pl)
        return;
    for (;;) {
        size_t total = 0;
        size_t n = playlist_snapshot(&u->a->playlist, 0, u->pl, u->pl_cap,
                                     &total);
        if (total <= u->pl_cap) {
            u->pl_n = n;
            break;
        }
        size_t cap = total + 64;
        track_info_t *np = realloc(u->pl, cap * sizeof *np);
        if (!np) { u->pl_n = n; break; }
        u->pl = np;
        u->pl_cap = cap;
    }
    u->pl_ver = v;

    /* Mantener el cursor sobre la MISMA pista aunque otros la muevan. */
    bool found = false;
    for (size_t i = 0; i < u->pl_n; i++) {
        if (u->pl[i].id == u->cursor_id) {
            u->cursor = i;
            found = true;
            break;
        }
    }
    if (!found) {
        if (u->cursor >= u->pl_n)
            u->cursor = u->pl_n ? u->pl_n - 1 : 0;
        u->cursor_id = u->pl_n ? u->pl[u->cursor].id : 0;
    }
}

static void cursor_to(ui_t *u, long idx)
{
    if (!u->pl_n) { u->cursor = 0; u->cursor_id = 0; return; }
    if (idx < 0) idx = 0;
    if ((size_t)idx >= u->pl_n) idx = (long)u->pl_n - 1;
    u->cursor = (size_t)idx;
    u->cursor_id = u->pl[u->cursor].id;
}

/* ------------------------------------------------------------------ */
/* Navegador de archivos                                               */
/* ------------------------------------------------------------------ */

static void browser_load(ui_t *u)
{
    free(u->ent);
    u->ent = fs_list_dir(u->cwd, &u->n_ent);
    u->bcur = u->bscroll = 0;
}

static void browser_open(ui_t *u)
{
    u->browsing = true;
    if (!u->cwd[0]) {
        char *rp = realpath(u->a->start_dir, NULL);
        snprintf(u->cwd, sizeof u->cwd, "%s", rp ? rp : u->a->start_dir);
        free(rp);
    }
    browser_load(u);
}

static void browser_up(ui_t *u)
{
    char *slash = strrchr(u->cwd, '/');
    if (!slash)
        return;
    if (slash == u->cwd)
        u->cwd[1] = '\0';
    else
        *slash = '\0';
    browser_load(u);
}

static void add_cb(const char *path, void *ctx)
{
    playlist_add(ctx, path);
}

static void browser_enter(ui_t *u, bool add_all)
{
    if (!u->n_ent && !add_all)
        return;
    char full[MAX_PATH_LEN + 260];
    if (add_all) {
        size_t n = fs_collect(u->cwd, 3, add_cb, &u->a->playlist);
        ui_flash(u, "＋ %zu pistas agregadas desde %s", n, u->cwd);
        return;
    }
    dir_entry_t *e = &u->ent[u->bcur];
    snprintf(full, sizeof full, "%s%s%s", u->cwd,
             strcmp(u->cwd, "/") ? "/" : "", e->name);
    if (e->is_dir) {
        if (strlen(full) >= sizeof u->cwd)
            return;                          /* ruta demasiado larga */
        memcpy(u->cwd, full, strlen(full) + 1);
        browser_load(u);
    } else if (playlist_add(&u->a->playlist, full)) {
        ui_flash(u, "＋ Agregada: %s", e->name);
        if (u->bcur + 1 < u->n_ent)
            u->bcur++;
    }
}

/* ------------------------------------------------------------------ */
/* Paneles                                                             */
/* ------------------------------------------------------------------ */

static void draw_header(ui_t *u, play_state_t ps)
{
    int lw = draw_logo(u, 0, 2);
    attron(CP(C_DIM));
    put(3, 2, u->W - 4,
        "motor concurrente · pthreads · ring buffer productor-consumidor · "
        "rwlock lectores-escritores");
    attroff(CP(C_DIM));

    /* Insignia de estado a la derecha. */
    const char *txt = ps == PS_PLAYING ? "  ▶  PLAY  "
                      : ps == PS_PAUSED ? "  ❚❚ PAUSA "
                                        : "  ■  STOP  ";
    int pair = ps == PS_PLAYING ? C_BADGE_PLAY
               : ps == PS_PAUSED ? C_BADGE_PAUSE : C_BADGE_STOP;
    int bx = u->W - 14;
    if (bx > lw + 4) {
        attron(CP(pair) | A_BOLD);
        put(1, bx, 12, txt);
        attroff(CP(pair) | A_BOLD);
        if (stress_running(&u->a->stress)) {
            attron(CP(C_ERR) | A_BOLD | (u->frame / 8 % 2 ? A_REVERSE : 0));
            put(2, bx, 12, " ⚡ STRESS  ");
            attroff(CP(C_ERR) | A_BOLD | A_REVERSE);
        }
    }
    if (u->flash_until > now_seconds()) {
        int fw = (int)mbstowcs(NULL, u->flash, 0);
        if (fw > 0) {
            int fx = u->W - fw - 3;
            if (fx < 2) fx = 2;
            attron(CP(C_WARN) | A_BOLD);
            put(3, fx, u->W - fx - 1, u->flash);
            attroff(CP(C_WARN) | A_BOLD);
        }
    }
}

static void draw_now_playing(ui_t *u, int y, int x, int h, int w,
                             play_state_t ps, uint64_t cur_id,
                             const play_status_t *st)
{
    box_titled(y, x, h, w, "♫", "Reproduciendo", NULL);
    int ix = x + 3, iw = w - 6;

    track_info_t t = { 0 };
    track_meta_t m = { 0 };
    size_t idx = 0;
    bool have = cur_id && playlist_get(&u->a->playlist, cur_id, &t, &idx);
    bool have_meta = cur_id && decoder_get_meta(&u->a->decoder, cur_id, &m);

    if (!have && !cur_id) {
        attron(CP(C_DIM));
        put(y + 2, ix, iw, "Nada en reproducción. Pulsa ␣ o Enter para "
                           "empezar, 'a' para agregar música.");
        attroff(CP(C_DIM));
    } else {
        const char *title = have_meta && m.title[0] ? m.title
                            : have ? t.title : "(pista eliminada de la lista)";
        attron(CP(C_TITLE) | A_BOLD);
        putf(y + 1, ix, iw, "%s %s",
             ps == PS_PLAYING ? "▶" : ps == PS_PAUSED ? "❚❚" : "■", title);
        attroff(CP(C_TITLE) | A_BOLD);

        attron(CP(C_INFO));
        if (have_meta)
            putf(y + 2, ix, iw, "%s%s%s · %.1f kHz · %s · %u bits",
                 m.artist[0] ? m.artist : "", m.artist[0] ? " · " : "",
                 m.codec, m.rate / 1000.0,
                 m.src_channels == 1 ? "mono" : "estéreo", m.bits);
        else
            put(y + 2, ix, iw, "cargando…");
        attroff(CP(C_INFO));
    }

    /* Barra de progreso. */
    uint64_t pos = 0, total = 0;
    uint32_t rate = 0;
    if (st->active && st->track_id == cur_id) {
        pos = st->pos_frames;
        total = st->total_frames;
        rate = st->rate;
    } else if (have_meta) {
        total = m.total_frames;
        rate = m.rate;
    }
    char t0[32], t1[32];
    fmt_time(t0, sizeof t0, pos, rate);
    fmt_time(t1, sizeof t1, total, rate);
    int by = y + 4;
    attron(CP(C_TEXT));
    put(by, ix, 6, t0);
    attroff(CP(C_TEXT));
    int bw = iw - 14;
    if (bw > 4) {
        int filled = total ? (int)((double)pos / (double)total * bw) : 0;
        if (filled > bw) filled = bw;
        for (int i = 0; i < bw; i++) {
            if (i < filled) {
                attron(CP(C_GRAD + i * 6 / bw) | A_BOLD);
                mvaddstr(by, ix + 7 + i, "━");
                attroff(CP(C_GRAD + i * 6 / bw) | A_BOLD);
            } else if (i == filled) {
                attron(CP(C_HEAD) | A_BOLD);
                mvaddstr(by, ix + 7 + i, "●");
                attroff(CP(C_HEAD) | A_BOLD);
            } else {
                attron(CP(C_SLOT_FREE));
                mvaddstr(by, ix + 7 + i, "─");
                attroff(CP(C_SLOT_FREE));
            }
        }
        attron(CP(C_DIM));
        put(by, ix + 8 + bw, 6, t1);
        attroff(CP(C_DIM));
    }

    /* Pad de efectos: los que suenan ahora se iluminan. */
    unsigned mask = atomic_load(&u->a->fx.active_mask);
    int px = ix;
    attron(CP(C_DIM));
    px += put(y + 3, px, iw, "FX ");
    attroff(CP(C_DIM));
    for (int i = 0; i < FX_COUNT && px < ix + iw; i++) {
        char pad[32];
        snprintf(pad, sizeof pad, " %d %s ", i + 1, fx_name(i));
        bool on = mask & (1u << i);
        attron(on ? CP(C_BADGE_PAUSE) | A_BOLD : CP(C_DIM));
        px += put(y + 3, px, ix + iw - px, pad) + 1;
        attroff(CP(C_BADGE_PAUSE) | A_BOLD | CP(C_DIM));
    }

    /* Volumen y posición en la lista. */
    int vol = atomic_load(&u->a->out.volume);
    int vy = y + 5;
    attron(CP(C_DIM));
    put(vy, ix, 5, "Vol");
    attroff(CP(C_DIM));
    int vw = 20;
    for (int i = 0; i < vw; i++) {
        bool on = i < vol * vw / 100;
        attron(on ? CP(C_ACCENT) | A_BOLD : CP(C_SLOT_FREE));
        mvaddstr(vy, ix + 4 + i, on ? "▮" : "▯");
        attroff(CP(C_ACCENT) | A_BOLD | CP(C_SLOT_FREE));
    }
    attron(CP(C_TEXT));
    putf(vy, ix + 5 + vw, 6, "%3d%%", vol);
    attroff(CP(C_TEXT));
    if (have) {
        attron(CP(C_DIM));
        putf(vy, ix + 12 + vw, iw - 12 - vw, "pista %zu de %zu · id #%llu",
             idx + 1, u->pl_n, (unsigned long long)t.id);
        attroff(CP(C_DIM));
    }
}

static void draw_spectrum(ui_t *u, int y, int x, int h, int w,
                          const viz_data_t *vz, const play_status_t *st,
                          play_state_t ps)
{
    box_titled(y, x, h, w, "≋", "Espectro", "FFT 2048 · Hann");
    int ix = x + 2, iw = w - 4;
    int rows = h - 2 - 2;                /* 2 filas para VU */
    if (rows < 2 || iw < 8)
        return;

    int bands = iw / 3;
    if (bands > 64) bands = 64;
    int bw = iw / bands;                 /* ancho por banda (barra+hueco) */
    int bar = bw > 2 ? bw - 1 : 1;
    int ox = ix + (iw - bands * bw) / 2;

    static const char *EIGHTHS[] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    for (int b = 0; b < bands; b++) {
        float v = u->spec.level[b];
        int units = (int)(v * rows * 8);
        int pk_row = (int)(u->spec.peak[b] * rows);
        for (int r = 0; r < rows; r++) {
            int yy = y + 1 + rows - 1 - r;
            int cell = units - r * 8;
            const char *ch = cell >= 8 ? EIGHTHS[8] : cell > 0 ? EIGHTHS[cell]
                                                                : NULL;
            int gi = r * NGRAD / rows;
            if (ch) {
                attron(CP(C_GRAD + gi) | A_BOLD);
                for (int k = 0; k < bar; k++)
                    mvaddstr(yy, ox + b * bw + k, ch);
                attroff(CP(C_GRAD + gi) | A_BOLD);
            } else if (r == pk_row && pk_row > 0) {
                attron(CP(C_HEAD));
                for (int k = 0; k < bar; k++)
                    mvaddstr(yy, ox + b * bw + k, "▔");
                attroff(CP(C_HEAD));
            }
        }
    }
    if (ps != PS_PLAYING || !st->active) {
        const char *msg = ps == PS_PAUSED ? "❚❚  en pausa" : "· · · silencio · · ·";
        int mw = (int)mbstowcs(NULL, msg, 0);
        attron(CP(C_DIM));
        put(y + 1 + rows / 2, ix + (iw - mw) / 2, iw, msg);
        attroff(CP(C_DIM));
    }

    /* Vúmetros L/R (pico instantáneo + marca de pico con caída). */
    (void)vz;
    for (int c = 0; c < 2; c++) {
        int yy = y + h - 3 + c;
        attron(CP(C_DIM));
        put(yy, ix, 2, c ? "R" : "L");
        attroff(CP(C_DIM));
        int mw = iw - 12;
        int lvl = (int)(u->vu[c] * mw);
        int pk = (int)(u->vu_peak[c] * mw);
        for (int i = 0; i < mw; i++) {
            int gi = i * NGRAD / mw;
            if (i < lvl) {
                attron(CP(C_GRAD + gi) | A_BOLD);
                mvaddstr(yy, ix + 2 + i, "■");
                attroff(CP(C_GRAD + gi) | A_BOLD);
            } else if (i == pk && pk > 0) {
                attron(CP(C_HEAD) | A_BOLD);
                mvaddstr(yy, ix + 2 + i, "▌");
                attroff(CP(C_HEAD) | A_BOLD);
            } else {
                attron(CP(C_SLOT_FREE));
                mvaddstr(yy, ix + 2 + i, "·");
                attroff(CP(C_SLOT_FREE));
            }
        }
        float db = u->vu[c] > 0.0001f ? 20.0f * log10f(u->vu[c]) : -99.0f;
        attron(CP(C_DIM));
        putf(yy, ix + 3 + mw, 9, "%5.1f dB", db < -99 ? -99.0f : db);
        attroff(CP(C_DIM));
    }
}

static int state_color(int s)
{
    switch (s) {
    case TS_RUNNING:        return C_OK;
    case TS_WAIT_NOT_FULL:
    case TS_WAIT_NOT_EMPTY:
    case TS_PREBUFFERING:   return C_WARN;
    case TS_WAIT_CMD:
    case TS_WAIT_REQUEST:   return C_INFO;
    case TS_PAUSED:         return C_PAUSE;
    case TS_EXITED:         return C_ERR;
    default:                return C_DIM;
    }
}

static void draw_thread_row(int y, int x, int w, const char *name,
                            const char *role, int st, const char *extra)
{
    int c = state_color(st);
    attron(CP(c) | A_BOLD);
    mvaddstr(y, x, "●");
    attroff(CP(c) | A_BOLD);
    attron(CP(C_TEXT) | A_BOLD);
    put(y, x + 2, 11, name);
    attroff(CP(C_TEXT) | A_BOLD);
    attron(CP(C_DIM));
    put(y, x + 13, 13, role);
    attroff(CP(C_DIM));
    attron(CP(c));
    put(y, x + 27, 15, thread_state_name((thread_state_t)st));
    attroff(CP(c));
    attron(CP(C_DIM));
    put(y, x + 43, w - 43, extra);
    attroff(CP(C_DIM));
}

static void draw_concurrency(ui_t *u, int y, int x, int h, int w)
{
    app_t *a = u->a;
    rb_snapshot_t rs;
    ringbuf_snapshot(&a->ring, &rs);
    char right[64];
    snprintf(right, sizeof right, "gen %llu", (unsigned long long)rs.gen);
    box_titled(y, x, h, w, "⚙", "Concurrencia", right);
    int ix = x + 2, iw = w - 4;

    /* Ring buffer: un casillero por slot, ocupados de head a tail. */
    attron(CP(C_TEXT) | A_BOLD);
    put(y + 1, ix, 12, "Ring buffer");
    attroff(CP(C_TEXT) | A_BOLD);
    int sx = ix + 12;
    int cap = (int)rs.cap;
    int cellw = (iw - 12 - 18) / cap >= 2 ? 2 : 1;
    for (int i = 0; i < cap; i++) {
        size_t rel = ((size_t)i - rs.head + rs.cap) % rs.cap;
        bool occ = rel < rs.count;
        if (occ) {
            int gi = (int)(rel * 6 / rs.cap);
            attron(CP(C_GRAD + gi) | A_BOLD);
            mvaddstr(y + 1, sx + i * cellw, "█");
            if (cellw == 2) mvaddstr(y + 1, sx + i * cellw + 1, " ");
            attroff(CP(C_GRAD + gi) | A_BOLD);
        } else {
            attron(CP(C_SLOT_FREE));
            mvaddstr(y + 1, sx + i * cellw, "░");
            attroff(CP(C_SLOT_FREE));
        }
        if ((size_t)i == rs.head) {
            attron(CP(C_OK) | A_BOLD);
            mvaddstr(y + 2, sx + i * cellw, "▲");
            attroff(CP(C_OK) | A_BOLD);
        }
        if ((size_t)i == rs.tail && rs.tail != rs.head) {
            attron(CP(C_TITLE) | A_BOLD);
            mvaddstr(y + 2, sx + i * cellw, "△");
            attroff(CP(C_TITLE) | A_BOLD);
        }
    }
    int ex = sx + cap * cellw + 1;
    attron(CP(rs.count <= 2 ? C_ERR : rs.count < rs.cap / 2 ? C_WARN
                                                           : C_OK) | A_BOLD);
    putf(y + 1, ex, ix + iw - ex, "%2zu/%d", rs.count, cap);
    attroff(A_BOLD);
    if (rs.prebuffering)
        put(y + 1, ex + 6, ix + iw - ex - 6, "prebuffer");
    attroff(CP(C_WARN) | CP(C_ERR) | CP(C_OK));
    attron(CP(C_DIM));
    put(y + 2, ix, 12, "▲cons △prod");
    attroff(CP(C_DIM));

    attron(CP(C_DIM));
    putf(y + 3, ix, iw,
         "push %-7llu pop %-7llu espera·lleno %-6llu espera·vacío %-5llu "
         "flush %llu",
         (unsigned long long)atomic_load(&a->ring.pushed),
         (unsigned long long)atomic_load(&a->ring.popped),
         (unsigned long long)atomic_load(&a->ring.waits_full),
         (unsigned long long)atomic_load(&a->ring.waits_empty),
         (unsigned long long)atomic_load(&a->ring.flushes));
    attroff(CP(C_DIM));
    /* Ring del 2.º productor (efectos): consumido con try_pop. */
    rb_snapshot_t fs;
    ringbuf_snapshot(&a->fx_ring, &fs);
    attron(CP(C_TEXT) | A_BOLD);
    put(y + 4, ix, 12, "Ring FX");
    attroff(CP(C_TEXT) | A_BOLD);
    for (size_t i = 0; i < fs.cap; i++) {
        bool occ = i < fs.count;
        attron(occ ? CP(C_TITLE) | A_BOLD : CP(C_SLOT_FREE));
        mvaddstr(y + 4, sx + (int)i * 2, occ ? "█" : "░");
        attroff(CP(C_TITLE) | A_BOLD | CP(C_SLOT_FREE));
    }
    int fx_x = sx + (int)fs.cap * 2 + 1;
    attron(CP(C_DIM));
    fx_x += putf(y + 4, fx_x, ix + iw - fx_x, "%zu/%zu · mezclados %-6llu",
                 fs.count, fs.cap,
                 (unsigned long long)atomic_load(&a->out.fx_mixed));
    attroff(CP(C_DIM));
    uint64_t ur = atomic_load(&a->out.underruns);
    attron(CP(ur ? C_ERR : C_OK) | A_BOLD);
    putf(y + 4, fx_x + 1, ix + iw - fx_x - 1, "underruns %llu%s",
         (unsigned long long)ur,
         atomic_load(&a->out.dry_run) ? " (simulado)" : "");
    attroff(CP(C_ERR) | CP(C_OK) | A_BOLD);

    char ex1[64], ex2[64], ex3[64], ex4[64], ex5[64], ex6[64];
    snprintf(ex6, sizeof ex6, "disparos %llu · rechazados %llu",
             (unsigned long long)atomic_load(&a->fx.triggered),
             (unsigned long long)atomic_load(&a->fx.rejected));
    snprintf(ex1, sizeof ex1, "pistas abiertas %llu",
             (unsigned long long)atomic_load(&a->decoder.tracks_opened));
    snprintf(ex2, sizeof ex2, "%.1f M frames",
             atomic_load(&a->out.frames_played) / 1e6);
    snprintf(ex3, sizeof ex3, "comandos %llu",
             (unsigned long long)atomic_load(&a->player.cmds_handled));
    snprintf(ex4, sizeof ex4, "ops %llu",
             (unsigned long long)atomic_load(&a->stress.ops));
    snprintf(ex5, sizeof ex5, "%.0f fps", u->fps);

    int ty = y + 5;
    draw_thread_row(ty + 0, ix, iw, "Decoder", "productor 1",
                    atomic_load(&a->decoder.state), ex1);
    draw_thread_row(ty + 1, ix, iw, "FX", "productor 2",
                    atomic_load(&a->fx.state), ex6);
    draw_thread_row(ty + 2, ix, iw, "AudioOut", "consumidor",
                    atomic_load(&a->out.state), ex2);
    draw_thread_row(ty + 3, ix, iw, "Controller", "comandos",
                    atomic_load(&a->player.thread_state), ex3);
    draw_thread_row(ty + 4, ix, iw, "Stress", "escritor",
                    stress_running(&a->stress)
                        ? atomic_load(&a->stress.state) : TS_IDLE,
                    stress_running(&a->stress) ? ex4 : "apagado (S)");
    draw_thread_row(ty + 5, ix, iw, "UI", "lector+cmds", TS_RUNNING, ex5);
}

static void draw_playlist(ui_t *u, int y, int x, int h, int w,
                          uint64_t cur_id)
{
    char right[48];
    snprintf(right, sizeof right, "%zu pistas · v%llu", u->pl_n,
             (unsigned long long)u->pl_ver);
    box_titled(y, x, h, w, "☰", "Lista", right);
    int ix = x + 2, iw = w - 5, rows = h - 2;
    if (rows < 1)
        return;
    if (!u->pl_n) {
        attron(CP(C_DIM));
        put(y + 2, ix + 1, iw, "La lista está vacía.");
        put(y + 3, ix + 1, iw, "Pulsa 'a' para explorar y agregar música.");
        attroff(CP(C_DIM));
        return;
    }
    if (u->cursor < u->scroll)
        u->scroll = u->cursor;
    if (u->cursor >= u->scroll + (size_t)rows)
        u->scroll = u->cursor - (size_t)rows + 1;
    if (u->scroll + (size_t)rows > u->pl_n)
        u->scroll = u->pl_n > (size_t)rows ? u->pl_n - (size_t)rows : 0;

    for (int r = 0; r < rows && u->scroll + (size_t)r < u->pl_n; r++) {
        size_t i = u->scroll + (size_t)r;
        const track_info_t *t = &u->pl[i];
        bool is_cur = t->id == cur_id;
        bool is_sel = i == u->cursor;
        int yy = y + 1 + r;
        int attr = is_sel ? CP(C_CURSOR) | A_BOLD
                   : is_cur ? CP(C_CURRENT) | A_BOLD : CP(C_TEXT);
        attron(attr);
        if (is_sel)
            for (int k = 0; k < iw; k++)
                mvaddch(yy, ix + k, ' ');
        char line[MAX_TITLE_LEN + 32];
        snprintf(line, sizeof line, "%s%3zu  %s", is_cur ? "▶" : " ", i + 1,
                 t->title);
        put(yy, ix, iw, line);
        attroff(attr);
    }
    /* Barra de desplazamiento. */
    if (u->pl_n > (size_t)rows) {
        int th = rows * rows / (int)u->pl_n;
        if (th < 1) th = 1;
        int tp = (int)(u->scroll * (size_t)(rows - th) /
                       (u->pl_n - (size_t)rows));
        for (int r = 0; r < rows; r++) {
            bool on = r >= tp && r < tp + th;
            attron(on ? CP(C_ACCENT) : CP(C_SLOT_FREE));
            mvaddstr(y + 1 + r, x + w - 2, on ? "┃" : "│");
            attroff(CP(C_ACCENT) | CP(C_SLOT_FREE));
        }
    }
}

static void draw_browser(ui_t *u, int y, int x, int h, int w)
{
    box_titled(y, x, h, w, "＋", "Agregar música", "Esc cerrar");
    int ix = x + 2, iw = w - 4, rows = h - 4;
    attron(CP(C_INFO));
    size_t cl = strlen(u->cwd);
    if ((int)cl > iw)                      /* mostrar el final de la ruta */
        putf(y + 1, ix, iw, "…%s", u->cwd + cl - (size_t)(iw - 1));
    else
        put(y + 1, ix, iw, u->cwd);
    attroff(CP(C_INFO));
    attron(CP(C_DIM));
    put(y + h - 2, ix, iw, "Enter abrir/agregar · A agregar todo · ← subir");
    attroff(CP(C_DIM));
    if (!u->n_ent) {
        attron(CP(C_DIM));
        put(y + 3, ix, iw, "(sin carpetas ni archivos .wav/.mp3)");
        attroff(CP(C_DIM));
        return;
    }
    if (u->bcur < u->bscroll) u->bscroll = u->bcur;
    if (rows > 0 && u->bcur >= u->bscroll + (size_t)rows)
        u->bscroll = u->bcur - (size_t)rows + 1;
    for (int r = 0; r < rows && u->bscroll + (size_t)r < u->n_ent; r++) {
        size_t i = u->bscroll + (size_t)r;
        dir_entry_t *e = &u->ent[i];
        bool sel = i == u->bcur;
        int attr = sel ? CP(C_CURSOR) | A_BOLD
                   : e->is_dir ? CP(C_ACCENT) : CP(C_TEXT);
        attron(attr);
        if (sel)
            for (int k = 0; k < iw; k++)
                mvaddch(y + 2 + r, ix + k, ' ');
        putf(y + 2 + r, ix, iw, "%s %s%s", e->is_dir ? "▸" : "♪", e->name,
             e->is_dir ? "/" : "");
        attroff(attr);
    }
}

static void draw_log(ui_t *u, int y, int x, int h, int w)
{
    (void)u;
    box_titled(y, x, h, w, "≡", "Eventos de hilos", NULL);
    int rows = h - 2, ix = x + 2, iw = w - 4;
    ev_entry_t ev[64];
    size_t n = evlog_snapshot(ev, rows > 64 ? 64 : (size_t)rows);
    for (size_t i = 0; i < n; i++) {
        int c = ev[i].level == EV_ERROR ? C_ERR : ev[i].level == EV_WARN
                ? C_WARN : ev[i].level == EV_SYNC ? C_SYNC : C_INFO;
        int yy = y + 1 + (int)i;
        attron(CP(C_DIM));
        putf(yy, ix, 9, "%7.2fs", ev[i].t);
        attroff(CP(C_DIM));
        attron(CP(c));
        put(yy, ix + 9, iw - 9, ev[i].msg);
        attroff(CP(c));
    }
}

static void key_hint(int *x, int y, int maxx, const char *key,
                     const char *label)
{
    char buf[64];
    snprintf(buf, sizeof buf, " %s ", key);
    int kw = (int)mbstowcs(NULL, buf, 0);
    int lw = (int)mbstowcs(NULL, label, 0);
    if (*x + kw + lw + 2 >= maxx)
        return;
    attron(CP(C_KEY) | A_BOLD);
    put(y, *x, kw, buf);
    attroff(CP(C_KEY) | A_BOLD);
    attron(CP(C_DIM));
    put(y, *x + kw + 1, lw, label);
    attroff(CP(C_DIM));
    *x += kw + lw + 2;
}

static void draw_helpbar(ui_t *u)
{
    int y = u->H - 1, x = 1;
    if (u->confirm_clear) {
        attron(CP(C_BADGE_STOP) | A_BOLD);
        put(y, 1, u->W - 2, " ¿Vaciar toda la lista? Pulsa 'y' para "
                            "confirmar, cualquier otra tecla cancela ");
        attroff(CP(C_BADGE_STOP) | A_BOLD);
        return;
    }
    key_hint(&x, y, u->W, "␣", "play/pausa");
    key_hint(&x, y, u->W, "n p", "sig/ant");
    key_hint(&x, y, u->W, "← →", "±5s");
    key_hint(&x, y, u->W, "+ -", "vol");
    key_hint(&x, y, u->W, "1-5", "efectos");
    key_hint(&x, y, u->W, "a", "agregar");
    key_hint(&x, y, u->W, "?", "ayuda");
    key_hint(&x, y, u->W, "q", "salir");
    key_hint(&x, y, u->W, "d", "borrar");
    key_hint(&x, y, u->W, "J K", "mover");
    key_hint(&x, y, u->W, "r", "mezclar");
    key_hint(&x, y, u->W, "c", "vaciar");
    key_hint(&x, y, u->W, "S", "stress");
}

static void draw_help_overlay(ui_t *u)
{
    static const char *lines[] = {
        "Espacio      Reproducir / Pausa",
        "Enter        Reproducir la pista seleccionada",
        "s            Detener",
        "n / p        Siguiente / Anterior (>3 s: reinicia la pista)",
        "← / →        Retroceder / Avanzar 5 segundos",
        "+ / -        Volumen",
        "↑ ↓ PgUp PgDn g G   Mover el cursor",
        "J / K        Mover la pista abajo / arriba",
        "d / Supr     Eliminar la pista (aunque esté sonando)",
        "r            Mezclar la lista      c   Vaciar la lista",
        "a / Tab      Explorador de archivos para agregar",
        "S            Activar/desactivar el hilo de estrés",
        "1 … 5        Efectos sobre la música (0 los silencia)",
        "q            Salir (apagado ordenado de todos los hilos)",
        "",
        "Cada acción de la lista toma el wrlock de la playlist; Next/Prev/",
        "Stop/Seek incrementan la generación del ring buffer (flush) y",
        "despiertan a productor y consumidor con pthread_cond_broadcast.",
    };
    int h = (int)ARRAY_LEN(lines) + 4, w = 72;
    if (w > u->W - 4) w = u->W - 4;
    int y = (u->H - h) / 2, x = (u->W - w) / 2;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            mvaddch(y + r, x + c, ' ');
    box_titled(y, x, h, w, "?", "Ayuda", "cualquier tecla cierra");
    for (size_t i = 0; i < ARRAY_LEN(lines); i++) {
        attron(i < 14 ? CP(C_TEXT) : CP(C_DIM));
        put(y + 2 + (int)i, x + 3, w - 6, lines[i]);
        attroff(CP(C_TEXT) | CP(C_DIM));
    }
}

/* ------------------------------------------------------------------ */
/* Render principal                                                    */
/* ------------------------------------------------------------------ */

static void update_viz(ui_t *u, const viz_data_t *vz, const play_status_t *st,
                       play_state_t ps)
{
    bool fresh = vz->serial != u->viz_serial;
    u->viz_serial = vz->serial;
    u->stale_frames = fresh ? 0 : u->stale_frames + 1;
    bool silent = ps != PS_PLAYING || !st->active || u->stale_frames > 10;

    int bands = 64;
    viz_spectrum(&u->spec, vz->window, bands, st->rate, silent);

    float lv[2] = { silent ? 0 : vz->peak_l, silent ? 0 : vz->peak_r };
    for (int c = 0; c < 2; c++) {
        u->vu[c] = lv[c] > u->vu[c] ? lv[c] : u->vu[c] * 0.85f;
        if (u->vu[c] >= u->vu_peak[c])
            u->vu_peak[c] = u->vu[c];
        else
            u->vu_peak[c] -= 0.008f;
        if (u->vu_peak[c] < 0) u->vu_peak[c] = 0;
    }
}

static void draw(ui_t *u)
{
    getmaxyx(stdscr, u->H, u->W);
    erase();
    if (u->W < 90 || u->H < 30) {
        attron(CP(C_WARN) | A_BOLD);
        putf(u->H / 2, 2, u->W - 4,
             "Agranda la terminal (mín. 90x30, actual %dx%d)", u->W, u->H);
        attroff(CP(C_WARN) | A_BOLD);
        refresh();
        return;
    }

    app_t *a = u->a;
    play_state_t ps = player_state(&a->player);
    uint64_t cur_id = player_current_id(&a->player);
    play_status_t st;
    audio_out_get_status(&a->out, &st);
    static viz_data_t vz;                 /* copia local (8 KiB) */
    audio_out_get_viz(&a->out, &vz);
    refresh_playlist(u);
    update_viz(u, &vz, &st, ps);

    draw_header(u, ps);

    int top = 4, bottom = u->H - 1;
    int lw = u->W * 3 / 5;
    int rw = u->W - lw;
    int np_h = 7, cc_h = 12;
    int sp_h = bottom - top - np_h - cc_h;

    draw_now_playing(u, top, 0, np_h, lw, ps, cur_id, &st);
    draw_spectrum(u, top + np_h, 0, sp_h, lw, &vz, &st, ps);
    draw_concurrency(u, bottom - cc_h, 0, cc_h, lw);

    int log_h = cc_h;
    if (u->browsing)
        draw_browser(u, top, lw, bottom - top - log_h, rw);
    else
        draw_playlist(u, top, lw, bottom - top - log_h, rw, cur_id);
    draw_log(u, bottom - log_h, lw, log_h, rw);
    draw_helpbar(u);
    if (u->show_help)
        draw_help_overlay(u);
    refresh();
}

/* ------------------------------------------------------------------ */
/* Teclado                                                             */
/* ------------------------------------------------------------------ */

static void key_browser(ui_t *u, int ch)
{
    switch (ch) {
    case 27: case '\t': case 'a': case 'q':
        u->browsing = false;
        break;
    case KEY_UP: case 'k':
        if (u->bcur) u->bcur--;
        break;
    case KEY_DOWN: case 'j':
        if (u->bcur + 1 < u->n_ent) u->bcur++;
        break;
    case KEY_PPAGE:
        u->bcur = u->bcur > 10 ? u->bcur - 10 : 0;
        break;
    case KEY_NPAGE:
        u->bcur = u->n_ent ? (u->bcur + 10 < u->n_ent ? u->bcur + 10
                                                       : u->n_ent - 1) : 0;
        break;
    case KEY_LEFT: case KEY_BACKSPACE: case 127: case 'h':
        browser_up(u);
        break;
    case KEY_RIGHT: case '\n': case KEY_ENTER: case 'l':
        browser_enter(u, false);
        break;
    case 'A':
        browser_enter(u, true);
        break;
    }
}

static void key_main(ui_t *u, int ch)
{
    app_t *a = u->a;
    refresh_playlist(u);
    track_info_t *sel = u->pl_n ? &u->pl[u->cursor] : NULL;

    switch (ch) {
    case 'q': case 'Q': case 3:
        u->running = false;
        break;
    case ' ':
        app_send(a, CMD_PAUSE_TOGGLE, 0, 0);
        break;
    case 's':
        app_send(a, CMD_STOP, 0, 0);
        break;
    case 'n':
        app_send(a, CMD_NEXT, 0, 0);
        break;
    case 'p':
        app_send(a, CMD_PREV, 0, 0);
        break;
    case KEY_LEFT:
        app_send(a, CMD_SEEK, -5, 0);
        break;
    case KEY_RIGHT:
        app_send(a, CMD_SEEK, 5, 0);
        break;
    case '+': case '=': {
        int v = atomic_load(&a->out.volume) + 5;
        atomic_store(&a->out.volume, v > 100 ? 100 : v);
        break;
    }
    case '-': case '_': {
        int v = atomic_load(&a->out.volume) - 5;
        atomic_store(&a->out.volume, v < 0 ? 0 : v);
        break;
    }
    case '\n': case KEY_ENTER:
        if (sel)
            app_send(a, CMD_PLAY_ID, 0, sel->id);
        break;
    case KEY_UP: case 'k':
        cursor_to(u, (long)u->cursor - 1);
        break;
    case KEY_DOWN: case 'j':
        cursor_to(u, (long)u->cursor + 1);
        break;
    case KEY_PPAGE:
        cursor_to(u, (long)u->cursor - 10);
        break;
    case KEY_NPAGE:
        cursor_to(u, (long)u->cursor + 10);
        break;
    case KEY_HOME: case 'g':
        cursor_to(u, 0);
        break;
    case KEY_END: case 'G':
        cursor_to(u, (long)u->pl_n - 1);
        break;
    case 'd': case KEY_DC:
        if (sel) {
            char title[MAX_TITLE_LEN];
            snprintf(title, sizeof title, "%s", sel->title);
            if (playlist_remove(&a->playlist, sel->id))
                ui_flash(u, "－ Eliminada: %s", title);
            /* El cursor pasa a la pista que ocupó su lugar. */
            u->cursor_id = 0;
        }
        break;
    case 'J':
        if (sel) playlist_move(&a->playlist, sel->id, +1);
        break;
    case 'K':
        if (sel) playlist_move(&a->playlist, sel->id, -1);
        break;
    case 'r':
        playlist_shuffle(&a->playlist);
        ui_flash(u, "⤮ Lista mezclada");
        break;
    case 'c':
        if (u->pl_n)
            u->confirm_clear = true;
        break;
    case 'a': case '\t':
        browser_open(u);
        break;
    case '1': case '2': case '3': case '4': case '5':
        /* Dispara un efecto: no bloquea, el hilo FX lo sintetiza. */
        if (!fx_trigger(&a->fx, ch - '1'))
            ui_flash(u, player_state(&a->player) == PS_PLAYING
                            ? "Demasiados efectos en cola"
                            : "Los efectos suenan sobre la música: pulsa ␣");
        break;
    case '0':
        fx_cancel(&a->fx);
        ui_flash(u, "Efectos silenciados");
        break;
    case 'S':
        if (stress_running(&a->stress)) {
            stress_stop(&a->stress);
            ui_flash(u, "Hilo de estrés detenido");
        } else if (playlist_count(&a->playlist) == 0) {
            ui_flash(u, "Agrega pistas antes de activar el estrés");
        } else {
            stress_start(&a->stress);
            ui_flash(u, "⚡ Hilo de estrés activo");
        }
        break;
    case '?': case 'h':
        u->show_help = true;
        break;
    }
}

static void on_key(ui_t *u, int ch)
{
    if (ch == KEY_RESIZE)
        return;
    if (u->show_help) {
        u->show_help = false;
        return;
    }
    if (u->confirm_clear) {
        u->confirm_clear = false;
        if (ch == 'y' || ch == 'Y') {
            playlist_clear(&u->a->playlist);
            ui_flash(u, "Lista vaciada");
        }
        return;
    }
    if (u->browsing)
        key_browser(u, ch);
    else
        key_main(u, ch);
}

/* ------------------------------------------------------------------ */

int ui_run(app_t *a)
{
    ui_t *u = calloc(1, sizeof *u);
    if (!u)
        return 1;
    u->a = a;
    u->running = true;
    u->pl_ver = UINT64_MAX;
    u->fps_t0 = now_seconds();

    setenv("ESCDELAY", "25", 1);
    initscr();
    cbreak();
    noecho();
    raw();                 /* Ctrl-C llega como tecla (3) */
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    init_colors();

    evlog_add(EV_INFO, "UI: lista con %zu pistas. Pulsa ? para ayuda",
              playlist_count(&a->playlist));

    while (u->running) {
        struct pollfd fds[2] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = a->signal_fd, .events = POLLIN },
        };
        int nfds = a->signal_fd >= 0 ? 2 : 1;
        int rc = poll(fds, (nfds_t)nfds, 33);   /* duerme hasta 33 ms */
        if (rc < 0 && errno != EINTR)
            break;
        if (nfds == 2 && (fds[1].revents & POLLIN)) {
            struct signalfd_siginfo si;
            if (read(a->signal_fd, &si, sizeof si) == (ssize_t)sizeof si) {
                evlog_add(EV_WARN, "UI: señal %u recibida", si.ssi_signo);
                u->running = false;
            }
        }
        int ch;
        while ((ch = getch()) != ERR)
            on_key(u, ch);

        u->frame++;
        u->fps_frames++;
        double t = now_seconds();
        if (t - u->fps_t0 >= 1.0) {
            u->fps = u->fps_frames / (t - u->fps_t0);
            u->fps_frames = 0;
            u->fps_t0 = t;
        }
        draw(u);
    }

    endwin();
    free(u->pl);
    free(u->ent);
    free(u);
    return 0;
}
