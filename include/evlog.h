/*
 * evlog.h - Registro de eventos concurrente (anillo de mensajes).
 *
 * Cualquier hilo puede llamar evlog_add(); la UI copia los últimos
 * mensajes con evlog_snapshot(). Protegido por un único mutex que es
 * HOJA en el orden global de locks (nunca se toma otro lock dentro).
 */
#ifndef EVLOG_H
#define EVLOG_H

#include <stddef.h>
#include <stdint.h>

#define EVLOG_CAP     128
#define EVLOG_MSG_LEN 120

typedef enum { EV_INFO = 0, EV_SYNC, EV_WARN, EV_ERROR } ev_level_t;

typedef struct {
    uint64_t   seq;
    double     t;                    /* segundos desde el arranque */
    ev_level_t level;
    char       msg[EVLOG_MSG_LEN];
} ev_entry_t;

void     evlog_init(void);
void     evlog_destroy(void);
void     evlog_add(ev_level_t level, const char *fmt, ...)
         __attribute__((format(printf, 2, 3)));
/* Copia hasta max entradas (las más recientes, en orden cronológico).
 * Devuelve cuántas copió. */
size_t   evlog_snapshot(ev_entry_t *out, size_t max);
uint64_t evlog_seq(void);
/* Si se activa, cada evento se imprime también en stderr (modo headless). */
void     evlog_set_echo(int on);
double   now_seconds(void);

#endif
