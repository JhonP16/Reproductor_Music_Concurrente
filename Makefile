# ConcuPlayer - Reproductor de audio concurrente (Proyecto 2, Sistemas Operativos)
#
#   make            -> binario optimizado ./concuplayer
#   make debug      -> ASan + UBSan
#   make tsan       -> ThreadSanitizer
#   make test       -> tests de estrés (ringbuf, playlist)
#   make test-tsan  -> tests de estrés bajo ThreadSanitizer
#   make assets     -> genera WAVs de prueba en assets/
#   make valgrind   -> ejecuta el modo headless bajo valgrind

CC      ?= gcc
BIN     := concuplayer
SRCDIR  := src
INCDIR  := include
BUILD   ?= build

CSTD    := -std=c11 -D_GNU_SOURCE
WARN    := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes
OPT     ?= -O2 -g
EXTRA_CFLAGS ?=
EXTRA_LDFLAGS ?=

# Dependencias externas: solo E/S de audio, decodificación MP3 y dibujo.
PKGS      := alsa libmpg123 ncursesw
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs $(PKGS) 2>/dev/null)
ifeq ($(strip $(PKG_LIBS)),)
PKG_LIBS := -lasound -lmpg123 -lncursesw
endif

CFLAGS  := $(CSTD) $(WARN) $(OPT) -pthread -I$(INCDIR) $(PKG_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS := -pthread $(EXTRA_LDFLAGS)
LDLIBS  := $(PKG_LIBS) -lm

# Núcleo concurrente sin dependencias externas (también usado por los tests).
CORE_SRC := ringbuf.c playlist.c cmdqueue.c evlog.c
APP_SRC  := main.c player.c decoder.c dec_wav.c dec_mp3.c audio_out.c \
            ui.c viz.c stress.c fsutil.c fx.c

CORE_OBJ := $(addprefix $(BUILD)/,$(CORE_SRC:.c=.o))
APP_OBJ  := $(addprefix $(BUILD)/,$(APP_SRC:.c=.o))
TESTS    := $(BUILD)/test_ringbuf $(BUILD)/test_playlist

.PHONY: all debug tsan test test-tsan clean assets valgrind run

all: $(BIN)

$(BIN): $(CORE_OBJ) $(APP_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: $(SRCDIR)/%.c $(wildcard $(INCDIR)/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/test_%: tests/test_%.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

$(BUILD):
	mkdir -p $(BUILD)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

debug:
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fno-omit-frame-pointer" \
	  EXTRA_CFLAGS="-fsanitize=address,undefined" \
	  EXTRA_LDFLAGS="-fsanitize=address,undefined" BIN=$(BIN)-asan

tsan:
	$(MAKE) BUILD=build-tsan OPT="-O1 -g" EXTRA_CFLAGS="-fsanitize=thread" \
	  EXTRA_LDFLAGS="-fsanitize=thread" BIN=$(BIN)-tsan

test-tsan:
	$(MAKE) BUILD=build-tsan OPT="-O1 -g" EXTRA_CFLAGS="-fsanitize=thread" \
	  EXTRA_LDFLAGS="-fsanitize=thread" test

assets:
	python3 tools/gen_tones.py assets

valgrind: $(BIN)
	timeout -s INT 15 valgrind --leak-check=full --show-leak-kinds=definite,indirect \
	  --error-exitcode=1 ./$(BIN) --headless --null --stress assets

run: $(BIN)
	./$(BIN) assets

clean:
	rm -rf build build-asan build-tsan $(BIN) $(BIN)-asan $(BIN)-tsan
