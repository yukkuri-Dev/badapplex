CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
LDLIBS  += $(shell pkg-config --libs   sdl2 2>/dev/null || sdl2-config --libs)

all: bapxplay

bapxplay: player.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f bapxplay

.PHONY: all clean
