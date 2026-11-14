# No -march=native: the AVX-512 code is picked at run time anyway, and it made things slightly slower.
CFLAGS ?= -O3
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion

SOURCES = src/flightdeck.c src/flightdeck.h src/flightdeck_backend.inc

all: libflightdeck.a

libflightdeck.a: flightdeck.o
	$(AR) rcs $@ $^

flightdeck.o: $(SOURCES)
	$(CC) $(CFLAGS) $(WARNINGS) -c src/flightdeck.c -o $@

build:
	mkdir -p build

# The tests run twice: with the best backend for this CPU, and with plain C only.
build/test: tests/test.c tests/sweep.h libflightdeck.a | build
	$(CC) $(CFLAGS) $(WARNINGS) -Isrc tests/test.c libflightdeck.a -o $@

build/test_portable: tests/test.c tests/sweep.h $(SOURCES) | build
	$(CC) $(CFLAGS) $(WARNINGS) -DFLIGHTDECK_PORTABLE -Isrc tests/test.c src/flightdeck.c -o $@

test: build/test build/test_portable
	./build/test
	./build/test_portable

build/bench: bench/bench.c libflightdeck.a | build
	$(CC) $(CFLAGS) $(WARNINGS) -Isrc bench/bench.c libflightdeck.a -o $@

bench: build/bench
	./build/bench

clean:
	rm -rf build flightdeck.o libflightdeck.a

.PHONY: all test bench clean
