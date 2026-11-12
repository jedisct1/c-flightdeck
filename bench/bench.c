/* Speed of the four instances: keyed input and output, hashing, and Deck-BO encryption. */

#define _POSIX_C_SOURCE 199309L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flightdeck.h"

enum { SIZES = 5 };
static const size_t sizes[SIZES] = { 64, 1024, 8192, 65536, 1 << 20 };

typedef struct buffers {
    flightdeck_variant variant;
    flightdeck         deck;
    flightdeck_bo      bo;
    uint8_t           *in;
    uint8_t           *out;
    size_t             size;
} buffers;

/* Keeps the compiler from dropping results nobody reads. */
static volatile uint8_t sink;

static double
now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + 1e-9 * (double) ts.tv_nsec;
}

static void
input(buffers *b)
{
    flightdeck deck = b->deck;
    uint8_t    out[32];

    flightdeck_duplex(&deck, out, sizeof out, b->in, b->size, FLIGHTDECK_TRAILER_EMPTY);
    sink ^= out[0];
}

static void
output(buffers *b)
{
    flightdeck deck = b->deck;

    flightdeck_duplex(&deck, NULL, 0, NULL, 0, FLIGHTDECK_TRAILER_EMPTY);
    flightdeck_xor(&deck, b->out, b->in, b->size);
    sink ^= b->out[0];
}

static void
hash(buffers *b)
{
    uint8_t digest[64];

    flightdeck_hash(b->variant, digest, 2 * flightdeck_key_len(b->variant), b->in, b->size);
    sink ^= digest[0];
}

static void
encrypt(buffers *b)
{
    flightdeck_bo bo = b->bo;
    uint8_t       tag[32];

    flightdeck_bo_encrypt(&bo, b->out, tag, b->in, b->size, NULL, 0);
    sink ^= tag[0] ^ b->out[0];
}

/* The best throughput of `op` over a few runs, in MiB/s. */
static double
measure(void (*op)(buffers *), buffers *b)
{
    double best = 0;

    for (int run = 0; run < 5; run++) {
        const double start      = now();
        double       elapsed    = 0;
        size_t       iterations = 0;

        while (elapsed < 0.02) {
            for (int i = 0; i < 16; i++) {
                op(b);
            }
            iterations += 16;
            elapsed = now() - start;
        }
        const double throughput = (double) (iterations * b->size) / elapsed / (1 << 20);
        if (throughput > best) {
            best = throughput;
        }
    }
    return best;
}

int
main(void)
{
    static const char *const names[] = { "TurboSHAKE128", "TurboSHAKE256", "SHAKE128", "SHAKE256" };
    static const struct {
        const char *name;
        void (*run)(buffers *);
    } ops[] = {
        { "FlightDeck input", input },
        { "FlightDeck output", output },
        { "FlightDeck hash", hash },
        { "BO encrypt", encrypt },
    };
    uint8_t key[32];
    buffers b;
    char    label[64];

    b.in  = malloc(sizes[SIZES - 1]);
    b.out = malloc(sizes[SIZES - 1]);
    if (b.in == NULL || b.out == NULL) {
        return 1;
    }
    for (size_t i = 0; i < sizes[SIZES - 1]; i++) {
        b.in[i] = (uint8_t) (i * 31);
    }
    memset(key, 0x42, sizeof key);

    printf("%-34s", "MiB/s");
    for (int s = 0; s < SIZES; s++) {
        printf("%10zu", sizes[s]);
    }
    printf("\n");

    for (size_t op = 0; op < sizeof ops / sizeof ops[0]; op++) {
        for (size_t v = 0; v < sizeof names / sizeof names[0]; v++) {
            snprintf(label, sizeof label, "%s-%s", names[v], ops[op].name);
            printf("%-34s", label);
            b.variant = (flightdeck_variant) v;
            flightdeck_init(&b.deck, b.variant, key, sizeof key);
            flightdeck_bo_init(&b.bo, b.variant, flightdeck_key_len(b.variant), key, sizeof key);
            for (int s = 0; s < SIZES; s++) {
                b.size = sizes[s];
                printf("%10.0f", measure(ops[op].run, &b));
                fflush(stdout);
            }
            printf("\n");
        }
    }
    free(b.in);
    free(b.out);
    return 0;
}
