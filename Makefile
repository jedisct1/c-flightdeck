CFLAGS ?= -O3
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion

SOURCES = src/flightdeck.c src/flightdeck.h src/flightdeck_backend.inc

all: libflightdeck.a

libflightdeck.a: flightdeck.o
	$(AR) rcs $@ $^

flightdeck.o: $(SOURCES)
	$(CC) $(CFLAGS) $(WARNINGS) -c src/flightdeck.c -o $@

clean:
	rm -f flightdeck.o libflightdeck.a

.PHONY: all clean
