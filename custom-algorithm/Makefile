CC      ?= gcc
CFLAGS  ?= -Wall -O2
LDFLAGS ?=

all: router

router: router.c router.h
	$(CC) $(CFLAGS) -pthread router.c -o router $(LDFLAGS)

clean:
	rm -f router *.o
