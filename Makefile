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

build/test: tests/test.c tests/sweep.h libflightdeck.a | build
	$(CC) $(CFLAGS) $(WARNINGS) -Isrc tests/test.c libflightdeck.a -o $@

test: build/test
	./build/test

build/bench: bench/bench.c libflightdeck.a | build
	$(CC) $(CFLAGS) $(WARNINGS) -Isrc bench/bench.c libflightdeck.a -o $@

bench: build/bench
	./build/bench

clean:
	rm -rf build flightdeck.o libflightdeck.a

.PHONY: all test bench clean
