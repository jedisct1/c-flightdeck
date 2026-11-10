/*
 * FlightDeck, a deck cipher that processes long inputs and outputs in parallel.
 *
 * A deck cipher takes a key and byte strings of any length, and gives back as many bytes as you want after each string.
 * Those bytes depend on the key and on every string so far.
 * With an empty key, it's a hash function.
 *
 * - flightdeck: the deck cipher.
 * - flightdeck_od: the overwrite duplex the deck cipher is built on, one block at a time.
 *
 * Calls that the comments below rule out stop the program with abort().
 * Pointers aren't checked, so a buffer has to be as long as its length says.
 *
 * A buffer can be NULL when its length is 0.
 * The buffers of a call must not overlap, unless the function says two of them can be the same.
 * Threads can use the library freely, as long as two of them don't use the same structure at the same time.
 *
 * FlightDeck: https://eprint.iacr.org/2026/2801
 * Overwrite duplex: https://eprint.iacr.org/2024/1618
 */

#ifndef FLIGHTDECK_H
#define FLIGHTDECK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The TurboSHAKE instances use 12 rounds of Keccak-p[1600] and are about twice as fast as the SHAKE ones, which use 24.
 * 128 and 256 are the security levels in bits.
 */
typedef enum flightdeck_variant {
    FLIGHTDECK_TURBOSHAKE128 = 0,
    FLIGHTDECK_TURBOSHAKE256 = 1,
    FLIGHTDECK_SHAKE128      = 2,
    FLIGHTDECK_SHAKE256      = 3
} flightdeck_variant;

/* Recommended key length in bytes: 16 for the 128-bit instances, 32 for the 256-bit ones. */
size_t flightdeck_key_len(flightdeck_variant variant);

/* Longest key in bytes, which is also the block size of the overwrite duplex: 160 or 128. */
size_t flightdeck_key_len_max(flightdeck_variant variant);

/*
 * A trailer is a few bits of domain separation, least significant bit first, followed by a 1 bit.
 * So 1 is the empty trailer, 2 is the single bit 0, and 0 isn't a valid trailer.
 * FlightDeck takes trailers from 1 to 31, and the overwrite duplex from 1 to 63.
 */
#define FLIGHTDECK_TRAILER_EMPTY  1u
#define FLIGHTDECK_TRAILER_MAX    31u
#define FLIGHTDECK_OD_TRAILER_MAX 63u

struct flightdeck_instance;

/*
 * The overwrite duplex.
 * Copy the structure to clone it.
 * Don't read or change its fields.
 */
typedef struct flightdeck_od {
    uint64_t                          state[25];
    const struct flightdeck_instance *instance;
    size_t                            offset;
} flightdeck_od;

/*
 * Set the key, of at most flightdeck_key_len_max() bytes.
 * Nothing can be squeezed before the first duplex call.
 */
void flightdeck_od_init(flightdeck_od *d, flightdeck_variant variant, const uint8_t *key,
                        size_t key_len);

/*
 * A clone that can only absorb the next block: what is left of the current output doesn't come along.
 * `clone` can be `d` itself.
 */
void flightdeck_od_clone_compact(flightdeck_od *clone, const flightdeck_od *d);

/*
 * Absorb a block and its trailer, then squeeze `out_len` bytes.
 * Both lengths are at most flightdeck_key_len_max().
 */
void flightdeck_od_duplex(flightdeck_od *d, uint8_t *out, size_t out_len, const uint8_t *in,
                          size_t in_len, unsigned trailer);

/* Squeeze more bytes after a duplex call, up to flightdeck_key_len_max() bytes in total. */
void flightdeck_od_squeeze(flightdeck_od *d, uint8_t *out, size_t out_len);

/* Like flightdeck_od_squeeze(), but XOR the output with `in`. They can be the same buffer. */
void flightdeck_od_xor(flightdeck_od *d, uint8_t *out, const uint8_t *in, size_t len);

/*
 * The deck cipher.
 * Copy the structure to clone it.
 * Don't read or change its fields.
 */
typedef struct flightdeck {
    flightdeck_od d;
    uint8_t       block[160];
    uint64_t      offset;
} flightdeck;

/*
 * Set the key, of at most flightdeck_key_len_max() bytes.
 * Nothing can be squeezed before the first duplex call.
 */
void flightdeck_init(flightdeck *deck, flightdeck_variant variant, const uint8_t *key,
                     size_t key_len);

/*
 * A clone that can only absorb the next string: what is left of the current output doesn't come along.
 * `clone` can be `deck` itself.
 */
void flightdeck_clone_compact(flightdeck *clone, const flightdeck *deck);

/* Absorb a byte string of any length and its trailer, then squeeze `out_len` bytes. */
void flightdeck_duplex(flightdeck *deck, uint8_t *out, size_t out_len, const uint8_t *in,
                       size_t in_len, unsigned trailer);

/* Squeeze more bytes after a duplex call, less than 2^64 - 1 bytes in total. */
void flightdeck_squeeze(flightdeck *deck, uint8_t *out, size_t out_len);

/* Like flightdeck_squeeze(), but XOR the output with `in`. They can be the same buffer. */
void flightdeck_xor(flightdeck *deck, uint8_t *out, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif
