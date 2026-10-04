# ConcuPlayer 

Reproductor de audio para terminal escrito en C (POSIX/pthreads) para el
**Proyecto 2 de Sistemas Operativos: Concurrencia y Sincronización**.
El motor de reproducción (productor–consumidor sobre un búfer circular)
está desacoplado de una lista de reproducción compartida (lectores–escritores
con `pthread_rwlock_t`), y todo se visualiza en vivo en una TUI ncurses.

```
  █▀▀ █▀█ █▄ █ █▀▀ █ █   █▀█ █   ▄▀█ █ █ █▀▀ █▀█
  █   █ █ █ ▀█ █   █ █   █▀▀ █   █▀█ ▀█▀ █▀▀ █▀▄
  ▀▀▀ ▀▀▀ ▀  ▀ ▀▀▀ ▀▀▀   ▀   ▀▀▀ ▀ ▀  ▀  ▀▀▀ ▀ ▀
```

## Vídeo Sustentación:


## Características

- **WAV** (parser RIFF propio: PCM 8/16/24/32 bits, float 32, mono/estéreo)
  y **MP3** (libmpg123, con etiquetas ID3).
- Salida **ALSA**, con reconfiguración automática entre pistas de distinta
  frecuencia de muestreo y transición sin hueco entre pistas iguales.
- Play / Pausa / Stop / Siguiente / Anterior / Seek ±5 s / Volumen con
  respuesta inmediata.
- Agregar, eliminar (incluso la pista que suena), reordenar, mezclar y
  vaciar la lista **mientras se reproduce**.
- Interfaz con: logo animado, barra de progreso, **analizador de espectro
  (FFT propia)**, vúmetros, **ocupación del ring buffer slot a slot**,
  estado de cada hilo (RUNNING / WAIT not_full / WAIT not_empty / PAUSED…),
  log de eventos de sincronización y explorador de archivos.
- **Efectos de sonido sobre la música** (teclas `1`–`5`): un segundo hilo
  productor sintetiza aplausos, bocina, campanas, láser y tambor, y el
  consumidor los **mezcla** en tiempo real sin que la canción se detenga.
- **Hilo de estrés** (`S` o `--stress`) que modifica la lista y salta pistas
  aleatoriamente para demostrar la robustez.

## Dependencias

```bash
sudo apt install build-essential pkg-config libasound2-dev libmpg123-dev \
                 libncurses-dev pipewire-alsa valgrind
```

> `pipewire-alsa` solo hace falta si el sistema usa PipeWire (p. ej. Kali,
> Debian, Ubuntu recientes): conecta el dispositivo ALSA `default` con
> PipeWire. Sin él, ALSA no puede abrir la tarjeta y el reproductor pasa
> automáticamente a **salida simulada** (funciona todo, pero sin sonido).

## Compilar y ejecutar

```bash
make                 # genera ./concuplayer
make assets          # crea 5 WAV de prueba en assets/ (formatos variados)
./concuplayer assets ~/Música
```

Opciones:

| Opción | Efecto |
|---|---|
| `archivos / carpetas` | Se agregan a la lista (carpetas recursivas) |
| `--stress` | Inicia el hilo de estrés |
| `--headless` | Sin interfaz: reproduce y registra eventos por stderr |
| `--device NOMBRE` | Dispositivo ALSA (por defecto `default`) |
| `--null` | Salida simulada (sin abrir dispositivo) |

## Teclas

| Tecla | Acción | Tecla | Acción |
|---|---|---|---|
| `Espacio` | Play / Pausa | `a` / `Tab` | Explorador de archivos |
| `Enter` | Reproducir selección | `d` / `Supr` | Eliminar pista |
| `s` | Stop | `J` / `K` | Mover pista abajo / arriba |
| `n` / `p` | Siguiente / Anterior | `r` | Mezclar |
| `←` / `→` | −5 s / +5 s | `c` | Vaciar lista (pide confirmación) |
| `+` / `-` | Volumen | `S` | Activar / detener estrés |
| `1` … `5` | Efecto sobre la música | `0` | Silenciar efectos |
| `↑ ↓ PgUp PgDn g G` | Cursor | `?` | Ayuda |
| `q` | Salir | | |

En el explorador: `Enter` abre carpeta o agrega archivo, `A` agrega toda la
carpeta, `←` sube de nivel, `Esc` cierra.

## Pruebas

```bash
make test         # estrés de ring buffer y playlist
make test-tsan    # los mismos tests bajo ThreadSanitizer
make tsan  && ./concuplayer-tsan --headless --null --stress assets
make debug && ./concuplayer-asan --headless --null --stress assets
make valgrind     # sin fugas de memoria
```

## Estructura

```
include/  src/
  ringbuf     búfer circular productor-consumidor (mutex + 2 conds, generaciones)
  playlist    lista compartida lectores-escritores (rwlock, sin punteros expuestos)
  cmdqueue    cola de comandos UI → controlador (mutex + cond)
  player      hilo controlador: máquina de estados Play/Pause/Stop
  decoder     hilo productor 1 + dec_wav / dec_mp3 (fuentes de audio)
  fx          hilo productor 2: efectos sintetizados (ring propio)
  audio_out   hilo consumidor → ALSA: mezcla música + efectos, pausa, vúmetros
  ui, viz     interfaz ncurses y FFT
  stress      hilo de estrés
  evlog       registro de eventos concurrente
tests/        pruebas de estrés multihilo
docs/PROTOCOLO_CONCURRENCIA.md   diseño y argumentación de la sincronización
tools/gen_tones.py               generador de WAV de prueba
```

El diseño concurrente completo (hilos, invariantes, orden de locks,
diagramas de secuencia, apagado) está en
[`docs/PROTOCOLO_CONCURRENCIA.md`](docs/PROTOCOLO_CONCURRENCIA.md).
