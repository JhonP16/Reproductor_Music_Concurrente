/*
 * main.c - Arranque, modo headless y apagado ordenado.
 *
 *   concuplayer [opciones] [archivos | directorios ...]
 *     --headless     sin interfaz: reproduce la lista y registra eventos
 *     --stress       inicia el hilo de estrés (modificaciones aleatorias)
 *     --device NAME  dispositivo ALSA (por defecto "default")
 *     --null         no abrir dispositivo: salida simulada (para pruebas)
 */
#include "app.h"
#include "evlog.h"
#include "fsutil.h"
#include "source.h"

#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

void app_send(app_t *a, cmd_type_t type, int64_t arg_i, uint64_t arg_u)
{
    command_t c = { .type = type, .arg_i = arg_i, .arg_u = arg_u };
    if (!cmdqueue_push(&a->cmdq, c))
        evlog_add(EV_WARN, "UI: cola de comandos llena, '%s' descartado",
                  cmd_name(type));
}

static void add_cb(const char *path, void *ctx)
{
    playlist_add(ctx, path);
}

static void usage(const char *argv0)
{
    printf("Uso: %s [--headless] [--stress] [--device NOMBRE] [--null] "
           "[archivos|directorios...]\n", argv0);
}

/* Bucle sin UI: espera señales con timeout (bloqueante, no gira). */
static int run_headless(app_t *a)
{
    app_send(a, CMD_PLAY, 0, 0);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    struct timespec tick = { 1, 0 };
    bool started = false;
    for (;;) {
        int sig = sigtimedwait(&set, NULL, &tick);
        if (sig == SIGINT || sig == SIGTERM) {
            fprintf(stderr, "\nSeñal %d: saliendo\n", sig);
            break;
        }
        play_state_t s = player_state(&a->player);
        play_status_t st;
        audio_out_get_status(&a->out, &st);
        if (s != PS_STOPPED || atomic_load(&a->decoder.tracks_opened) > 0)
            started = true;
        if (started && s == PS_STOPPED && !stress_running(&a->stress))
            break;                         /* terminó la lista */
        if (st.active && st.rate)
            fprintf(stderr, "  [estado] pista #%llu  %llu:%02llu  búfer %zu/%d\n",
                    (unsigned long long)st.track_id,
                    (unsigned long long)(st.pos_frames / st.rate / 60),
                    (unsigned long long)(st.pos_frames / st.rate % 60),
                    ringbuf_count(&a->ring), RING_SLOTS);
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool headless = false, stress = false, null_out = false;
    const char *device = "default";
    static app_t app;                       /* grande: fuera de la pila */
    app_t *a = &app;

    setlocale(LC_ALL, "");
    evlog_init();
    if (mp3_global_init() != 0) {
        fprintf(stderr, "No se pudo inicializar libmpg123\n");
        return 1;
    }
    if (playlist_init(&a->playlist) != 0 || ringbuf_init(&a->ring) != 0 ||
        ringbuf_init_ex(&a->fx_ring, FX_SLOTS, 0) != 0) {
        fprintf(stderr, "Error de inicialización\n");
        return 1;
    }
    cmdqueue_init(&a->cmdq);
    stress_init(&a->stress, &a->playlist, &a->cmdq, &a->fx);
    if (!getcwd(a->start_dir, sizeof a->start_dir))
        snprintf(a->start_dir, sizeof a->start_dir, ".");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--stress")) stress = true;
        else if (!strcmp(argv[i], "--null")) null_out = true;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc)
            device = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            size_t n = fs_collect(argv[i], 4, add_cb, &a->playlist);
            if (n == 0)
                evlog_add(EV_WARN, "'%s': sin audio soportado", argv[i]);
            if (fs_is_dir(argv[i]))
                snprintf(a->start_dir, sizeof a->start_dir, "%s", argv[i]);
        }
    }
    if (headless)
        evlog_set_echo(1);
    if (headless && playlist_count(&a->playlist) == 0) {
        fprintf(stderr, "Modo headless sin archivos de audio.\n");
        usage(argv[0]);
        return 1;
    }

    /*
     * Señales: se bloquean ANTES de crear hilos (los hilos heredan la
     * máscara), así SIGINT/SIGTERM solo se atienden en el hilo principal
     * vía signalfd/sigtimedwait, y SIGWINCH solo lo recibe ncurses.
     */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    if (decoder_start(&a->decoder, &a->playlist, &a->ring) != 0 ||
        audio_out_start(&a->out, &a->ring, &a->fx_ring, &a->cmdq,
                        null_out ? NULL : device) != 0 ||
        fx_start(&a->fx, &a->fx_ring, &a->out) != 0 ||
        player_start(&a->player, &a->cmdq, &a->playlist, &a->ring,
                     &a->decoder, &a->out, &a->fx) != 0) {
        fprintf(stderr, "No se pudieron crear los hilos\n");
        return 1;
    }
    if (stress)
        stress_start(&a->stress);

    sigset_t winch;
    sigemptyset(&winch);
    sigaddset(&winch, SIGWINCH);
    pthread_sigmask(SIG_UNBLOCK, &winch, NULL);   /* solo en este hilo */
    sigdelset(&set, SIGWINCH);
    a->signal_fd = headless ? -1 : signalfd(-1, &set, SFD_CLOEXEC);

    int rc = headless ? run_headless(a) : ui_run(a);

    /* ---------- Apagado ordenado (ver docs/PROTOCOLO_CONCURRENCIA.md) */
    stress_stop(&a->stress);                 /* 1. deja de generar carga */
    app_send(a, CMD_QUIT, 0, 0);             /* 2. controlador termina   */
    cmdqueue_close(&a->cmdq);
    player_join(&a->player);
    decoder_shutdown(&a->decoder);           /* 3. despertar a todos     */
    fx_shutdown(&a->fx);
    audio_out_shutdown(&a->out);
    ringbuf_shutdown(&a->ring);
    ringbuf_shutdown(&a->fx_ring);
    decoder_join(&a->decoder);               /* 4. esperar y liberar     */
    fx_join(&a->fx);
    audio_out_join(&a->out);

    if (a->signal_fd >= 0)
        close(a->signal_fd);
    stress_destroy(&a->stress);
    ringbuf_destroy(&a->ring);
    ringbuf_destroy(&a->fx_ring);
    playlist_destroy(&a->playlist);
    cmdqueue_destroy(&a->cmdq);
    mp3_global_exit();
    evlog_destroy();
    if (headless)
        fprintf(stderr, "Todos los hilos terminaron; recursos liberados.\n");
    return rc;
}
