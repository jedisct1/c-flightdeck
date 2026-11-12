/*
 * Long, fixed sequences of calls, each one summed up in 32 bytes.
 *
 * The calls go through the SWEEP_* macros, so that the same sequences can run on another implementation.
 * That's how the expected sums in test.c were made, with the Zig implementation at https://github.com/jedisct1/zig-flightdeck.
 *
 * The file that includes this one defines these macros, where `v` is the variant from 0 to 3 and objects are passed as `void *`:
 *
 *   SWEEP_OD_INIT(v, d, key, key_len)
 *   SWEEP_OD_CLONE_COMPACT(v, clone, d)
 *   SWEEP_OD_DUPLEX(v, d, out, out_len, in, in_len, trailer)
 *   SWEEP_OD_SQUEEZE(v, d, out, out_len)
 *   SWEEP_OD_XOR(v, d, out, in, len)
 *   SWEEP_DECK_INIT, SWEEP_DECK_CLONE_COMPACT, SWEEP_DECK_DUPLEX, SWEEP_DECK_SQUEEZE, SWEEP_DECK_XOR: the same for a deck
 *   SWEEP_BO_INIT(v, tag_len, bo, key, key_len)
 *   SWEEP_BO_ENCRYPT(v, tag_len, bo, c, tag, m, m_len, ad, ad_len)
 *   SWEEP_BO_DECRYPT(v, tag_len, bo, m, c, c_len, tag, ad, ad_len), 0 when the tag matches
 *   SWEEP_BO_ABSORB(v, tag_len, bo, data, len)
 *   SWEEP_BO_SQUEEZE(v, tag_len, bo, out, len)
 */

#ifndef SWEEP_H
#define SWEEP_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Big enough for a duplex, a deck or a session of any implementation. */
typedef union sweep_object {
    uint8_t     bytes[8192];
    max_align_t align;
} sweep_object;

enum { SWEEP_MAX = 160000 };

static void
sweep_pattern(uint8_t *buf, size_t len, size_t multiplier, size_t offset)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t) ((multiplier * i + offset) % 251);
    }
}

static void *
sweep_alloc(size_t size)
{
    void *p = malloc(size);

    if (p == NULL) {
        abort();
    }
    return p;
}

/* Block and chunk lengths in bytes: the odd variants are the 256-bit instances. */
static size_t
sweep_rate(int variant)
{
    return variant & 1 ? 128 : 160;
}

static size_t
sweep_chunk(int variant)
{
    return variant & 1 ? 8192 : 5120;
}

/*
 * Input length for step `i` of the deck sweep, or SIZE_MAX when there are no more steps.
 * Every length up to 329 bytes, then lengths around multiples of a block and of half a chunk, then up to 18 chunks plus a few blocks.
 */
static size_t
sweep_length(int variant, size_t i)
{
    static const int around[5] = { -1, 0, 1, 7, 161 };
    const size_t     rate      = sweep_rate(variant);
    const size_t     chunk     = sweep_chunk(variant);

    if (i < 330) {
        return i;
    }
    i -= 330;
    if (i < 3 * 34) {
        return rate * (2 + i / 3) + i % 3 - 1;
    }
    i -= 3 * 34;
    if (i < 5 * 19) {
        return chunk / 2 * (1 + i / 5) + (size_t) (around[i % 5] + 1) - 1;
    }
    i -= 5 * 19;
    if (i < 12) {
        return chunk * (1 + i) + rate * (3 * i + 1) + (i % 3) * (rate / 2);
    }
    i -= 12;
    if (i < 3) {
        return chunk * (16 + i) + 3 * rate + 5 + i;
    }
    return SIZE_MAX;
}

/* The duplex, with keys, blocks, trailers and outputs of every length, output split over several calls, and compact clones. */
static void
sweep_od(int variant, uint8_t digest[32])
{
    const size_t  rate  = sweep_rate(variant);
    sweep_object *acc   = sweep_alloc(sizeof *acc);
    sweep_object *d     = sweep_alloc(sizeof *d);
    sweep_object *clone = sweep_alloc(sizeof *clone);
    uint8_t       in[160];
    uint8_t       out[160];

    sweep_pattern(in, sizeof in, 13, 5);
    SWEEP_DECK_INIT(variant, acc, (const uint8_t *) "sweep", 5);
    for (size_t i = 0; i < 700; i++) {
        const size_t key_len  = i % (rate + 1);
        const size_t in_len   = i % 4 == 0 ? rate : (i * 37) % (rate + 1);
        const size_t out_len  = (i * 11) % (rate + 1);
        const size_t xored    = (rate - out_len) * (i % 3) / 3;
        const size_t squeezed = (rate - out_len - xored) / 2;

        sweep_pattern(in, key_len, 3, i);
        SWEEP_OD_INIT(variant, d, in, key_len);
        sweep_pattern(in, in_len, 7, i);
        SWEEP_OD_DUPLEX(variant, d, out, out_len, in, in_len, 1 + (unsigned) (i % 63));
        SWEEP_OD_XOR(variant, d, out + out_len, in, xored);
        SWEEP_OD_SQUEEZE(variant, d, out + out_len + xored, squeezed);
        SWEEP_OD_XOR(variant, d, out + out_len + xored + squeezed, in,
                     rate - out_len - xored - squeezed);
        SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, out, rate, 1);

        SWEEP_OD_CLONE_COMPACT(variant, clone, d);
        SWEEP_OD_DUPLEX(variant, clone, out, rate, in, (i * 5) % (rate + 1),
                        1 + (unsigned) ((i * 7) % 63));
        SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, out, rate, 1);

        SWEEP_OD_CLONE_COMPACT(variant, d, d);
        SWEEP_OD_DUPLEX(variant, d, out, rate, in, i % 7, 2);
        SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, out, rate, 1);
    }
    SWEEP_DECK_DUPLEX(variant, acc, digest, 32, NULL, 0, 1);
    free(acc);
    free(d);
    free(clone);
}

/* The deck, with keys, strings and outputs of all sizes, output split over several calls, and compact clones. */
static void
sweep_deck(int variant, uint8_t digest[32])
{
    const size_t  rate  = sweep_rate(variant);
    uint8_t      *in    = sweep_alloc(SWEEP_MAX);
    uint8_t      *out   = sweep_alloc(SWEEP_MAX);
    sweep_object *acc   = sweep_alloc(sizeof *acc);
    sweep_object *deck  = sweep_alloc(sizeof *deck);
    sweep_object *clone = sweep_alloc(sizeof *clone);
    uint8_t       key[64];

    /* The XORed output can be longer than the string of its step, so the whole buffer gets filled. */
    sweep_pattern(in, SWEEP_MAX, 11, 3);
    SWEEP_DECK_INIT(variant, acc, (const uint8_t *) "sweep", 5);
    for (size_t i = 0;; i++) {
        const size_t len     = sweep_length(variant, i);
        const size_t key_len = i % 3 == 0 ? 0 : i % 33;
        const size_t out_len = i % 5 == 0 ? 1 + (i * 7) % 2000 : i % 70;
        /* Every fifth step XORs 1 to 21 whole blocks from the start of the output. */
        const size_t squeezed = i % 5 == 0 ? 0 : i % 190;
        const size_t xored    = i % 5 == 0 ? rate * (1 + (i / 5) % 21) : 150 + i % 400;
        const size_t more     = 1 + i % 170;

        if (len == SIZE_MAX) {
            break;
        }
        sweep_pattern(key, key_len, 5, i);
        sweep_pattern(in, len, 1, 17 * i);
        SWEEP_DECK_INIT(variant, deck, key, key_len);
        SWEEP_DECK_DUPLEX(variant, deck, out, out_len, in, len, 1 + (unsigned) (i % 31));
        SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, out, out_len, 1);

        /* A second string, with its output split over three calls. */
        SWEEP_DECK_DUPLEX(variant, deck, NULL, 0, in, (i * 3) % 700, 2);
        SWEEP_DECK_SQUEEZE(variant, deck, out, squeezed);
        SWEEP_DECK_XOR(variant, deck, out + squeezed, in, xored);
        SWEEP_DECK_SQUEEZE(variant, deck, out + squeezed + xored, more);
        SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, out, squeezed + xored + more, 1);

        SWEEP_DECK_CLONE_COMPACT(variant, clone, deck);
        SWEEP_DECK_DUPLEX(variant, clone, out, 32, in, i % 50, 3);
        SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, out, 32, 1);

        SWEEP_DECK_CLONE_COMPACT(variant, deck, deck);
        SWEEP_DECK_DUPLEX(variant, deck, out, 32, in, i % 9, 4);
        SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, out, 32, 1);
    }
    SWEEP_DECK_DUPLEX(variant, acc, digest, 32, NULL, 0, 1);
    free(in);
    free(out);
    free(acc);
    free(deck);
    free(clone);
}

/* Deck-BO: two sessions of messages, with and without plaintext or associated data, and absorbs and squeezes in between. */
static void
sweep_bo(int variant, uint8_t digest[32])
{
    static const size_t lengths[12] = {
        0, 5, 0, 40, 159, 160, 161, 1000, 5120, 6000, 11000, 47000
    };
    uint8_t      *m        = sweep_alloc(SWEEP_MAX);
    uint8_t      *ad       = sweep_alloc(SWEEP_MAX);
    uint8_t      *c        = sweep_alloc(SWEEP_MAX);
    sweep_object *acc      = sweep_alloc(sizeof *acc);
    sweep_object *sender   = sweep_alloc(sizeof *sender);
    sweep_object *receiver = sweep_alloc(sizeof *receiver);
    uint8_t       key[32];
    uint8_t       tag[100];
    uint8_t       status;

    sweep_pattern(key, sizeof key, 1, 0);
    /* What gets absorbed between messages can be longer than the associated data of the step, so the whole buffer gets filled. */
    sweep_pattern(ad, SWEEP_MAX, 9, 1);
    SWEEP_DECK_INIT(variant, acc, (const uint8_t *) "sweep", 5);

    /* A short tag, then one that is longer than a block of the 256-bit instances. */
    for (size_t tag_len = 16; tag_len <= 100; tag_len += 84) {
        SWEEP_BO_INIT(variant, tag_len, sender, key, tag_len == 16 ? 16 : 32);
        SWEEP_BO_INIT(variant, tag_len, receiver, key, tag_len == 16 ? 16 : 32);
        for (size_t i = 0; i < 12; i++) {
            const size_t m_len  = lengths[i];
            const size_t ad_len = lengths[(i * 7 + 3) % 12];

            sweep_pattern(m, m_len, 3, 29 * i);
            sweep_pattern(ad, ad_len, 1, 17 * i);
            SWEEP_BO_ENCRYPT(variant, tag_len, sender, c, tag, m, m_len, ad, ad_len);
            SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, c, m_len, 1);
            SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, tag, tag_len, 2);

            status = (uint8_t) (SWEEP_BO_DECRYPT(variant, tag_len, receiver, c, c, m_len, tag, ad,
                                                 ad_len) != 0);
            status |= (uint8_t) (2 * (memcmp(c, m, m_len) != 0));
            SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, &status, 1, 3);

            SWEEP_BO_ABSORB(variant, tag_len, sender, ad, i * 30);
            SWEEP_BO_ABSORB(variant, tag_len, receiver, ad, i * 30);
            SWEEP_BO_SQUEEZE(variant, tag_len, sender, c, 20 + 50 * i);
            SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, c, 20 + 50 * i, 4);
            SWEEP_BO_SQUEEZE(variant, tag_len, receiver, c, 3);
            SWEEP_DECK_DUPLEX(variant, acc, NULL, 0, c, 3, 5);
        }
    }
    SWEEP_DECK_DUPLEX(variant, acc, digest, 32, NULL, 0, 1);
    free(m);
    free(ad);
    free(c);
    free(acc);
    free(sender);
    free(receiver);
}

/*
 * Two long strings, of about 3 and 1.5 MB.
 * Chaining values only reach the main duplex in more than one go when a string is longer than about half a megabyte.
 */
static void
sweep_long(int variant, uint8_t digest[32])
{
    enum { LEN = 3 * 1024 * 1024 + 4321 };
    uint8_t      *in   = sweep_alloc(LEN);
    sweep_object *deck = sweep_alloc(sizeof *deck);

    sweep_pattern(in, LEN, 5, 7);
    SWEEP_DECK_INIT(variant, deck, (const uint8_t *) "long", 4);
    SWEEP_DECK_DUPLEX(variant, deck, NULL, 0, in, LEN, 1);
    SWEEP_DECK_DUPLEX(variant, deck, digest, 32, in + 1, LEN / 2 + 77, 5);
    free(in);
    free(deck);
}

#endif
