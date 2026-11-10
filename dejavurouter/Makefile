CC ?= cc
LIBEVENT_PREFIX ?= $(CURDIR)/third_party/libevent/usr
EVENT_LIB ?= $(LIBEVENT_PREFIX)/lib/x86_64-linux-gnu
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS += -Ithird_party/cJSON -I$(LIBEVENT_PREFIX)/include
LDFLAGS += -pthread $(EVENT_LIB)/libevent_extra.a $(EVENT_LIB)/libevent_core.a -lm

all: dejavurouter

dejavurouter: src/async_router.c third_party/cJSON/cJSON.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/async_router.c third_party/cJSON/cJSON.c $(LDFLAGS)

asan: src/async_router.c third_party/cJSON/cJSON.c
	$(CC) -O1 -g -std=c11 -Wall -Wextra -pthread -fsanitize=address,undefined \
		$(CPPFLAGS) -o dejavurouter-asan src/async_router.c third_party/cJSON/cJSON.c \
		$(LDFLAGS)

legacy: src/dejavurouter.c
	$(CC) $(CFLAGS) -o dejavurouter-legacy src/dejavurouter.c -pthread

clean:
	rm -f dejavurouter dejavurouter-asan dejavurouter-legacy

.PHONY: all asan legacy clean
