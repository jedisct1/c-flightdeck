/*
 * The overwrite duplex, FlightDeck on top of it, and Deck-BO on top of that.
 * The permutation and the loops over blocks are in flightdeck_backend.inc.
 *
 * Lanes are 64-bit words in the byte order of the machine, and byte strings are little-endian.
 */

#include "flightdeck.h"

#include <stdlib.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#    define FDECK_LITTLE_ENDIAN 1
#else
#    define FDECK_LITTLE_ENDIAN 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#    define FDECK_ALWAYS_INLINE static inline __attribute__((always_inline))
#    define FDECK_NOINLINE      __attribute__((noinline))
#else
#    define FDECK_ALWAYS_INLINE static inline
#    define FDECK_NOINLINE
#endif

#define FDECK_RATE_MAX           160
#define FDECK_CAPACITY_LANES_MAX 9
#define FDECK_CV_LANES_MAX       8

/* Room for the chaining values waiting to be absorbed by the main duplex. */
#define FDECK_PENDING_MAX 4096

/* The `offset` of a deck that has nothing to squeeze yet. */
#define FDECK_NO_OUTPUT UINT64_MAX

/* Trailers of the blocks FlightDeck adds on its own. */
#define FDECK_TRAILER_COUNTER        2u
#define FDECK_TRAILER_CHAINING_VALUE 4u

/* Trailers of Deck-BO. */
#define FDECK_TRAILER_AD_ONLY 4u
#define FDECK_TRAILER_AD      5u
#define FDECK_TRAILER_TAG     13u
#define FDECK_TRAILER_MESSAGE 14u

/*
 * For each instance: variant, name, lanes in a block, lanes in a chaining value, blocks in a chunk, rounds, and the last byte of a block.
 * That last byte is the same as in TurboSHAKE or SHAKE, since the security of the duplex is tied to that sponge function.
 */
#define FDECK_INSTANCES(X)                                          \
    X(FLIGHTDECK_TURBOSHAKE128, turboshake128, 20, 4, 32, 12, 0x80) \
    X(FLIGHTDECK_TURBOSHAKE256, turboshake256, 16, 8, 64, 12, 0x80) \
    X(FLIGHTDECK_SHAKE128, shake128, 20, 4, 32, 24, 0x9f)           \
    X(FLIGHTDECK_SHAKE256, shake256, 16, 8, 64, 24, 0x9f)

#define FDECK_REQUIRE(condition) \
    do {                         \
        if (!(condition)) {      \
            abort();             \
        }                        \
    } while (0)

_Static_assert(sizeof((flightdeck *) 0)->block == FDECK_RATE_MAX,
               "the deck keeps one block of output");

static const uint64_t fdeck_round_constants[24] = {
    0x0000000000000001, 0x0000000000008082, 0x800000000000808a, 0x8000000080008000,
    0x000000000000808b, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
    0x000000000000008a, 0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
    0x000000008000808b, 0x800000000000008b, 0x8000000000008089, 0x8000000000008003,
    0x8000000000008002, 0x8000000000000080, 0x000000000000800a, 0x800000008000000a,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
};

static inline size_t
fdeck_min(size_t a, size_t b)
{
    return a < b ? a : b;
}

static inline uint64_t
fdeck_rotl(uint64_t x, unsigned n)
{
    return (x << n) | (x >> (64 - n));
}

/* Add a bit at the end of a trailer: it goes where the final 1 was, and the 1 moves up. */
static inline uint64_t
fdeck_append_bit(uint64_t trailer, unsigned bit)
{
    uint64_t below = trailer;

    below |= below >> 1;
    below |= below >> 2;
    below |= below >> 4;
    return trailer + ((below - (below >> 1)) << bit);
}

/* The lane that follows a block: its trailer, whether the block is full, and the padding byte. */
static inline uint64_t
fdeck_trailer_lane(unsigned trailer, int full, uint8_t pad)
{
    return fdeck_append_bit(trailer, full ? 1 : 0) | ((uint64_t) pad << 56);
}

static inline void
fdeck_load_lanes(uint64_t *lanes, const uint8_t *in, size_t count)
{
#if FDECK_LITTLE_ENDIAN
    memcpy(lanes, in, 8 * count);
#else
    for (size_t i = 0; i < count; i++) {
        uint64_t lane = 0;
        for (unsigned j = 0; j < 8; j++) {
            lane |= (uint64_t) in[8 * i + j] << (8 * j);
        }
        lanes[i] = lane;
    }
#endif
}

static inline void
fdeck_store_lanes(uint8_t *out, const uint64_t *lanes, size_t count)
{
#if FDECK_LITTLE_ENDIAN
    memcpy(out, lanes, 8 * count);
#else
    for (size_t i = 0; i < count; i++) {
        for (unsigned j = 0; j < 8; j++) {
            out[8 * i + j] = (uint8_t) (lanes[i] >> (8 * j));
        }
    }
#endif
}

static inline uint64_t
fdeck_load_lane(const uint8_t *in)
{
    uint64_t lane;

    fdeck_load_lanes(&lane, in, 1);
    return lane;
}

/* The bytes of `count` lanes: the lanes themselves on a little-endian machine, a copy in `buffer` otherwise. */
static inline const uint8_t *
fdeck_lane_bytes(const uint64_t *lanes, size_t count, uint8_t *buffer)
{
#if FDECK_LITTLE_ENDIAN
    (void) count;
    (void) buffer;
    return (const uint8_t *) lanes;
#else
    fdeck_store_lanes(buffer, lanes, count);
    return buffer;
#endif
}

/* Copy `stream` to `out`, XORed with `in` if there is one. `in` can be `out`. */
static inline void
fdeck_xor_bytes(uint8_t *out, const uint8_t *in, const uint8_t *stream, size_t len)
{
    if (in == NULL) {
        memcpy(out, stream, len);
    } else if (in == out) {
        /* A separate loop, because Clang doesn't use vector instructions for the next one when both are the same buffer. */
        for (size_t i = 0; i < len; i++) {
            out[i] ^= stream[i];
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            out[i] = in[i] ^ stream[i];
        }
    }
}

/*
 * Zero a small buffer that gets read right away.
 * It's done 80 bytes at a time, because GCC turns anything bigger into an instruction that is slow to start, and the read has to wait for it.
 */
FDECK_ALWAYS_INLINE void
fdeck_clear(void *data, size_t len)
{
    uint8_t *bytes = data;

    for (; len > 80; len -= 80) {
        memset(bytes, 0, 80);
        bytes += 80;
    }
    memset(bytes, 0, len);
}

/* Zero a buffer in a way the compiler can't optimize away. */
static void
fdeck_wipe(void *data, size_t len)
{
    if (len == 0) {
        return;
    }
    memset(data, 0, len);
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(data) : "memory");
#else
    volatile uint8_t *bytes = (volatile uint8_t *) data;
    while (len-- > 0) {
        *bytes++ = 0;
    }
#endif
}

/* See absorb() in flightdeck_backend.inc. */
typedef void fdeck_absorb_fn(uint64_t *caps, const uint8_t *const *in, size_t nblocks,
                             const uint8_t *const *last, const uint64_t *last_trailers,
                             uint64_t *out, int whole_state);

struct flightdeck_instance {
    unsigned         rate_lanes;
    unsigned         cv_lanes;
    unsigned         blocks_per_chunk;
    uint8_t          pad;
    fdeck_absorb_fn *absorb;
};

typedef const struct flightdeck_instance fdeck_instance;

/* Lane operations when a lane is a plain 64-bit word. */
#define FDECK_WORD_XOR(a, b)    ((a) ^ (b))
#define FDECK_WORD_CHI(a, b, c) ((a) ^ (~(b) & (c)))

#define FDECK_N        1
#define FDECK_FN(name) fdeck_##name##_c
#define FDECK_TARGET
#include "flightdeck_backend.inc"

#define FDECK_C_INSTANCE(variant, name, rate_lanes, cv_lanes, blocks_per_chunk, rounds, pad) \
    [variant] = { rate_lanes, cv_lanes, blocks_per_chunk, pad, fdeck_##name##_absorb_c },

static const struct flightdeck_instance fdeck_instances[] = { FDECK_INSTANCES(FDECK_C_INSTANCE) };

static fdeck_instance *
fdeck_instance_of(flightdeck_variant variant)
{
    FDECK_REQUIRE((size_t) variant < sizeof fdeck_instances / sizeof fdeck_instances[0]);
    return &fdeck_instances[variant];
}

static inline size_t
fdeck_rate(fdeck_instance *im)
{
    return 8 * (size_t) im->rate_lanes;
}

static inline size_t
fdeck_capacity_len(fdeck_instance *im)
{
    return 8 * (25 - (size_t) im->rate_lanes);
}

/*
 * The last block of a string, the way the backends want it: a full block, and the lane that follows it.
 * A partial block is copied to `buffer` and padded with a 1 bit, then zeros.
 */
static const uint8_t *
fdeck_last_block(fdeck_instance *im, uint8_t *buffer, const uint8_t *in, size_t len,
                 unsigned trailer, uint64_t *trailer_lane)
{
    const size_t whole = len - len % 8;
    uint64_t     lane  = (uint64_t) 0x01 << (8 * (len % 8));

    *trailer_lane = fdeck_trailer_lane(trailer, len == fdeck_rate(im), im->pad);
    if (len == fdeck_rate(im)) {
        return in;
    }
    for (size_t i = whole; i < len; i++) {
        lane |= (uint64_t) in[i] << (8 * (i - whole));
    }
    /* This block is read right away, and that read is slow after small writes, so no byte is written on its own. */
    fdeck_clear(buffer, FDECK_RATE_MAX);
    if (whole != 0) {
        memcpy(buffer, in, whole);
    }
    fdeck_store_lanes(buffer + whole, &lane, 1);
    return buffer;
}

/* Absorb full blocks that aren't the last of a string into a state given by its capacity. */
static void
fdeck_absorb_blocks(fdeck_instance *im, uint64_t *cap, const uint8_t *in, size_t nblocks)
{
    if (nblocks != 0) {
        im->absorb(cap, &in, nblocks, NULL, NULL, NULL, 0);
    }
}

/* Not inlined: short strings never need it, and they get slower when its code is inlined. */
FDECK_NOINLINE static size_t fdeck_absorb_chunks(fdeck_instance *im, uint64_t *cap,
                                                 const uint8_t *in, size_t len, uint8_t *pending);

/*
 * Absorb a string of any length into a state given by its capacity, with `trailer` after the last block.
 * Then write the whole state to `out` if `whole_state` is set, or just the lanes of a chaining value.
 */
static void
fdeck_absorb_string(fdeck_instance *im, uint64_t *cap, const uint8_t *in, size_t len,
                    unsigned trailer, uint64_t *out, int whole_state)
{
    const size_t   rate = fdeck_rate(im);
    uint8_t        pending[FDECK_PENDING_MAX];
    uint8_t        padded[FDECK_RATE_MAX];
    const uint8_t *last;
    uint64_t       trailer_lane;
    size_t         nblocks = 0;

    /* Past one chunk, the other chunks give chaining values, and what's left for this state comes back in `pending`. */
    if (len > rate * im->blocks_per_chunk) {
        len = fdeck_absorb_chunks(im, cap, in, len, pending);
        in  = pending;
    }
    if (len != 0) {
        nblocks = (len - 1) / rate;
    }
    last = fdeck_last_block(im, padded, len == 0 ? in : in + nblocks * rate, len - nblocks * rate,
                            trailer, &trailer_lane);
    im->absorb(cap, &in, nblocks, &last, &trailer_lane, out, whole_state);
}

static void
fdeck_duplex(fdeck_instance *im, uint64_t state[25], const uint8_t *in, size_t len,
             unsigned trailer)
{
    fdeck_absorb_string(im, state + im->rate_lanes, in, len, trailer, state, 1);
}

/* The chaining value of a chunk, full or not, starting from the capacity `cap`. */
static void
fdeck_chaining_value(fdeck_instance *im, uint8_t *cv, const uint64_t *cap, const uint8_t *in,
                     size_t len)
{
    uint64_t clone[FDECK_CAPACITY_LANES_MAX];
    uint64_t lanes[FDECK_CV_LANES_MAX];

    memcpy(clone, cap, fdeck_capacity_len(im));
    fdeck_absorb_string(im, clone, in, len, FDECK_TRAILER_CHAINING_VALUE, lanes, 0);
    fdeck_store_lanes(cv, lanes, im->cv_lanes);
}

/*
 * Where the next `count` chaining values go.
 * They pile up in `pending`, and the main duplex absorbs them several blocks at a time.
 */
static uint8_t *
fdeck_reserve_cvs(fdeck_instance *im, uint64_t *cap, uint8_t *pending, size_t *pending_len,
                  size_t count)
{
    const size_t rate = fdeck_rate(im);
    const size_t len  = 8 * (size_t) im->cv_lanes * count;
    uint8_t     *cvs;

    if (*pending_len + len > FDECK_PENDING_MAX) {
        /* More values follow, so none of these blocks is the last of the string. */
        const size_t nblocks = *pending_len / rate;

        fdeck_absorb_blocks(im, cap, pending, nblocks);
        *pending_len -= nblocks * rate;
        memmove(pending, pending + nblocks * rate, *pending_len);
    }
    cvs = pending + *pending_len;
    *pending_len += len;
    return cvs;
}

/*
 * Absorb an input longer than a chunk.
 * What the main duplex still has to absorb is left in `pending`, and its length is returned.
 *
 * The main duplex, given by its capacity, absorbs the first chunk.
 * Every other chunk goes to a compact clone of the main duplex as it was before the call, which gives a chaining value.
 * The main duplex then absorbs the chaining values.
 */
static size_t
fdeck_absorb_chunks(fdeck_instance *im, uint64_t *cap, const uint8_t *in, size_t len,
                    uint8_t *pending)
{
    const size_t chunk = fdeck_rate(im) * im->blocks_per_chunk;
    uint64_t     start[FDECK_CAPACITY_LANES_MAX];
    size_t       pending_len = 0;

    memcpy(start, cap, fdeck_capacity_len(im));
    fdeck_absorb_blocks(im, cap, in, im->blocks_per_chunk);
    in += chunk;
    len -= chunk;
    while (len != 0) {
        const size_t n = fdeck_min(len, chunk);

        fdeck_chaining_value(im, fdeck_reserve_cvs(im, cap, pending, &pending_len, 1), start, in,
                             n);
        in += n;
        len -= n;
    }
    return pending_len;
}

/* Output block number `index`, at least 1, of a deck, given the capacity of its main duplex. */
static void
fdeck_output_block(fdeck_instance *im, uint8_t *block, const uint64_t *cap, uint64_t index)
{
    uint8_t  counter[FDECK_RATE_MAX] = { 0 };
    uint64_t state[25];

    fdeck_store_lanes(counter, &index, 1);
    memcpy(state + im->rate_lanes, cap, fdeck_capacity_len(im));
    fdeck_duplex(im, state, counter, fdeck_rate(im), FDECK_TRAILER_COUNTER);
    fdeck_store_lanes(block, state, im->rate_lanes);
}

/* Squeeze `len` bytes from a deck, XORed with `in` if there is one. */
static void
fdeck_output(flightdeck *deck, uint8_t *out, const uint8_t *in, size_t len)
{
    fdeck_instance *im     = deck->d.instance;
    const size_t    rate   = fdeck_rate(im);
    const uint64_t *cap    = deck->d.state + im->rate_lanes;
    uint64_t        offset = deck->offset;
    size_t          i      = 0;

    /* This also catches a deck that has nothing to squeeze yet. */
    FDECK_REQUIRE(len < FDECK_NO_OUTPUT - offset);

    /* The first block comes from the main duplex. */
    if (offset < rate && len != 0) {
        uint8_t block[FDECK_RATE_MAX];

        i = fdeck_min(len, rate - (size_t) offset);
        fdeck_xor_bytes(out, in, fdeck_lane_bytes(deck->d.state, im->rate_lanes, block) + offset,
                        i);
        offset += i;
    }
    /* Then the rest of the block where the previous call stopped. */
    if (offset % rate != 0 && i < len) {
        const size_t used = (size_t) (offset % rate);
        const size_t n    = fdeck_min(len - i, rate - used);

        fdeck_xor_bytes(out + i, in == NULL ? NULL : in + i, deck->block + used, n);
        offset += n;
        i += n;
    }
    /* Every other block comes from a compact clone that absorbed the block number. */
    while (i < len) {
        const size_t n = fdeck_min(len - i, rate);
        uint8_t      block[FDECK_RATE_MAX];

        fdeck_output_block(im, block, cap, offset / rate);
        fdeck_xor_bytes(out + i, in == NULL ? NULL : in + i, block, n);
        /* Keep a partly used block for the next call. */
        if (n != rate) {
            memcpy(deck->block, block, rate);
        }
        offset += n;
        i += n;
    }
    deck->offset = offset;
}

/* Squeeze `len` bytes from a duplex, XORed with `in` if there is one. */
static void
fdeck_od_output(flightdeck_od *d, uint8_t *out, const uint8_t *in, size_t len)
{
    fdeck_instance *im = d->instance;
    uint8_t         block[FDECK_RATE_MAX];

    FDECK_REQUIRE(d->offset <= fdeck_rate(im) && len <= fdeck_rate(im) - d->offset);
    if (len != 0) {
        fdeck_xor_bytes(out, in, fdeck_lane_bytes(d->state, im->rate_lanes, block) + d->offset,
                        len);
    }
    d->offset += len;
}

size_t
flightdeck_key_len(flightdeck_variant variant)
{
    return 4 * (25 - (size_t) fdeck_instance_of(variant)->rate_lanes - 1);
}

size_t
flightdeck_key_len_max(flightdeck_variant variant)
{
    return fdeck_rate(fdeck_instance_of(variant));
}

void
flightdeck_od_init(flightdeck_od *d, flightdeck_variant variant, const uint8_t *key, size_t key_len)
{
    fdeck_instance *im = fdeck_instance_of(variant);

    FDECK_REQUIRE(key_len <= fdeck_rate(im));
    fdeck_clear(d->state, sizeof d->state);
    d->instance = im;
    fdeck_duplex(im, d->state, key, key_len, FLIGHTDECK_TRAILER_EMPTY);
    d->offset = fdeck_rate(im);
}

void
flightdeck_od_clone_compact(flightdeck_od *clone, const flightdeck_od *d)
{
    fdeck_instance *im = d->instance;

    memset(clone->state, 0, fdeck_rate(im));
    memmove(clone->state + im->rate_lanes, d->state + im->rate_lanes, fdeck_capacity_len(im));
    clone->instance = im;
    clone->offset   = fdeck_rate(im);
}

void
flightdeck_od_duplex(flightdeck_od *d, uint8_t *out, size_t out_len, const uint8_t *in,
                     size_t in_len, unsigned trailer)
{
    FDECK_REQUIRE(trailer >= 1 && trailer <= FLIGHTDECK_OD_TRAILER_MAX);
    FDECK_REQUIRE(in_len <= fdeck_rate(d->instance));
    fdeck_duplex(d->instance, d->state, in, in_len, trailer);
    d->offset = 0;
    fdeck_od_output(d, out, NULL, out_len);
}

void
flightdeck_od_squeeze(flightdeck_od *d, uint8_t *out, size_t out_len)
{
    fdeck_od_output(d, out, NULL, out_len);
}

void
flightdeck_od_xor(flightdeck_od *d, uint8_t *out, const uint8_t *in, size_t len)
{
    FDECK_REQUIRE(in != NULL || len == 0);
    fdeck_od_output(d, out, in, len);
}

void
flightdeck_init(flightdeck *deck, flightdeck_variant variant, const uint8_t *key, size_t key_len)
{
    flightdeck_od_init(&deck->d, variant, key, key_len);
    deck->offset = FDECK_NO_OUTPUT;
}

void
flightdeck_clone_compact(flightdeck *clone, const flightdeck *deck)
{
    flightdeck_od_clone_compact(&clone->d, &deck->d);
    clone->offset = FDECK_NO_OUTPUT;
}

void
flightdeck_duplex(flightdeck *deck, uint8_t *out, size_t out_len, const uint8_t *in, size_t in_len,
                  unsigned trailer)
{
    FDECK_REQUIRE(trailer >= 1 && trailer <= FLIGHTDECK_TRAILER_MAX);
    fdeck_duplex(deck->d.instance, deck->d.state, in, in_len,
                 (unsigned) fdeck_append_bit(trailer, 1));
    deck->offset = 0;
    fdeck_output(deck, out, NULL, out_len);
}

void
flightdeck_squeeze(flightdeck *deck, uint8_t *out, size_t out_len)
{
    fdeck_output(deck, out, NULL, out_len);
}

void
flightdeck_xor(flightdeck *deck, uint8_t *out, const uint8_t *in, size_t len)
{
    FDECK_REQUIRE(in != NULL || len == 0);
    fdeck_output(deck, out, in, len);
}

void
flightdeck_hash(flightdeck_variant variant, uint8_t *out, size_t out_len, const uint8_t *in,
                size_t in_len)
{
    flightdeck deck;

    flightdeck_init(&deck, variant, NULL, 0);
    flightdeck_duplex(&deck, out, out_len, in, in_len, FLIGHTDECK_TRAILER_EMPTY);
}

void
flightdeck_bo_init(flightdeck_bo *bo, flightdeck_variant variant, size_t tag_len,
                   const uint8_t *key, size_t key_len)
{
    FDECK_REQUIRE(tag_len != 0);
    flightdeck_init(&bo->deck, variant, key, key_len);
    bo->tag_len = tag_len;
}

void
flightdeck_bo_encrypt(flightdeck_bo *bo, uint8_t *c, uint8_t *tag, const uint8_t *m, size_t m_len,
                      const uint8_t *ad, size_t ad_len)
{
    flightdeck keystream;

    if (m_len == 0) {
        flightdeck_duplex(&bo->deck, tag, bo->tag_len, ad, ad_len, FDECK_TRAILER_AD_ONLY);
        return;
    }
    if (ad_len != 0) {
        flightdeck_duplex(&bo->deck, NULL, 0, ad, ad_len, FDECK_TRAILER_AD);
    }
    flightdeck_clone_compact(&keystream, &bo->deck);
    flightdeck_duplex(&bo->deck, tag, bo->tag_len, m, m_len, FDECK_TRAILER_MESSAGE);
    flightdeck_duplex(&keystream, NULL, 0, tag, bo->tag_len, FDECK_TRAILER_TAG);
    flightdeck_xor(&keystream, c, m, m_len);
}

/*
 * Squeeze a tag from a deck in pieces, since it can be any length, and compare it with `tag` in constant time.
 * Returns 0 if they match.
 * With a NULL `tag`, the tag is only skipped.
 */
static unsigned
fdeck_bo_check_tag(flightdeck *deck, const uint8_t *tag, size_t tag_len)
{
    uint8_t  piece[64];
    unsigned difference = 0;

    while (tag_len != 0) {
        const size_t n = fdeck_min(tag_len, sizeof piece);

        flightdeck_squeeze(deck, piece, n);
        if (tag != NULL) {
            for (size_t i = 0; i < n; i++) {
                difference |= piece[i] ^ tag[i];
            }
            tag += n;
        }
        tag_len -= n;
    }
    return difference;
}

int
flightdeck_bo_decrypt(flightdeck_bo *bo, uint8_t *m, const uint8_t *c, size_t c_len,
                      const uint8_t *tag, const uint8_t *ad, size_t ad_len)
{
    flightdeck deck;

    /* Work on a clone, so that a message that doesn't authenticate leaves the session alone. */
    flightdeck_clone_compact(&deck, &bo->deck);
    if (c_len == 0) {
        flightdeck_duplex(&deck, NULL, 0, ad, ad_len, FDECK_TRAILER_AD_ONLY);
    } else {
        flightdeck keystream;

        if (ad_len != 0) {
            flightdeck_duplex(&deck, NULL, 0, ad, ad_len, FDECK_TRAILER_AD);
        }
        flightdeck_clone_compact(&keystream, &deck);
        flightdeck_duplex(&keystream, NULL, 0, tag, bo->tag_len, FDECK_TRAILER_TAG);
        flightdeck_xor(&keystream, m, c, c_len);
        flightdeck_duplex(&deck, NULL, 0, m, c_len, FDECK_TRAILER_MESSAGE);
    }
    if (fdeck_bo_check_tag(&deck, tag, bo->tag_len) != 0) {
        fdeck_wipe(m, c_len);
        return -1;
    }
    bo->deck = deck;
    return 0;
}

void
flightdeck_bo_absorb(flightdeck_bo *bo, const uint8_t *data, size_t data_len)
{
    /*
     * Same as an empty message.
     * Its tag is skipped, so that flightdeck_bo_squeeze() starts after it.
     */
    flightdeck_duplex(&bo->deck, NULL, 0, data, data_len, FDECK_TRAILER_AD_ONLY);
    fdeck_bo_check_tag(&bo->deck, NULL, bo->tag_len);
}

void
flightdeck_bo_squeeze(flightdeck_bo *bo, uint8_t *out, size_t out_len)
{
    flightdeck_squeeze(&bo->deck, out, out_len);
}
