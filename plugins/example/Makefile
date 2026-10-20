# jash example plugin — builds on macOS (ld64) and Linux (GNU ld).
CC ?= cc
CFLAGS = -shared -fPIC -I../../c -I../../c/sys -Wall -Wextra -Wpedantic -Werror \
         -DJASH_TARGET_POSIX -D_POSIX_C_SOURCE=200809L

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # ld64: resolve jash_* symbols against the host executable at load time.
    LDFLAGS = -undefined dynamic_lookup
else
    # GNU ld: undefined symbols in a shared object are resolved at dlopen
    # time against the loading executable's exported (dynamic-list) symbols.
    LDFLAGS =
endif

all: hello.so

hello.so: plugin.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f *.so
