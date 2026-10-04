/*
 * common.h - Tipos y constantes compartidas por todos los módulos.
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define APP_NAME        "ConcuPlayer"
#define MAX_PATH_LEN    1024
#define MAX_TITLE_LEN   256

/* Formato PCM interno: siempre int16 intercalado (interleaved). */
#define MAX_CHANNELS    2
#define CHUNK_FRAMES    2048            /* frames por chunk del ring buffer */
#define RING_SLOTS      32              /* chunks en el ring buffer        */
#define PREBUFFER_SLOTS 8               /* marca alta para arrancar salida */

/* Estados que cada hilo publica para la UI (panel de concurrencia). */
typedef enum {
    TS_IDLE = 0,
    TS_RUNNING,
    TS_WAIT_NOT_FULL,     /* productor bloqueado: búfer lleno   */
    TS_WAIT_NOT_EMPTY,    /* consumidor bloqueado: búfer vacío  */
    TS_WAIT_CMD,          /* controlador esperando comandos     */
    TS_WAIT_REQUEST,      /* decodificador sin pista asignada   */
    TS_WAIT_TRIGGER,      /* hilo FX sin efectos que sintetizar */
    TS_PAUSED,
    TS_PREBUFFERING,
    TS_EXITED
} thread_state_t;

const char *thread_state_name(thread_state_t s);

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif
