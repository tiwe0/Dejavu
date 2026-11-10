CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?= -pthread

all: dejavurouter

dejavurouter: src/dejavurouter.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f dejavurouter

.PHONY: all clean
