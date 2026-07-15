CC      = gcc
CFLAGS = -O3 -std=c11 -Wall -Wextra -lm

ifeq ($(OS),Windows_NT)
LDFLAGS = -lws2_32
EXT     = .exe
else
LDFLAGS = -pthread -lm
EXT     =
endif

all: sender$(EXT) receiver$(EXT)

sender$(EXT): sender.c
	$(CC) $(CFLAGS) -o sender$(EXT) sender.c $(LDFLAGS)

receiver$(EXT): receiver.c
	$(CC) $(CFLAGS) -o receiver$(EXT) receiver.c $(LDFLAGS)

clean:
	rm -f sender$(EXT) receiver$(EXT)

.PHONY: all clean
