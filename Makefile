CC ?= gcc
CFLAGS ?= -O2 -Wall

ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
endif

all: sender receiver

sender: sender.c
	$(CC) $(CFLAGS) -o sender sender.c $(LDFLAGS)

receiver: receiver.c
	$(CC) $(CFLAGS) -o receiver receiver.c $(LDFLAGS)

clean:
	rm -f sender receiver sender.exe receiver.exe
