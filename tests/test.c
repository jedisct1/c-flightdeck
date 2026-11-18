/*
 * Tests for the overwrite duplex, FlightDeck and Deck-BO.
 *
 * `make test` builds this file three times: with the best backend for the CPU, without AVX-512, and with the plain C code only.
 * `make test-wasm` builds it once more, for WebAssembly.
 */

#define _DEFAULT_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#    include <signal.h>
#    include <sys/mman.h>
#    include <sys/wait.h>
#    include <unistd.h>
#    ifndef MAP_ANONYMOUS
#        define MAP_ANONYMOUS MAP_ANON
#    endif
#    define HAVE_POSIX 1
#else
#    define HAVE_POSIX 0
#endif

#include "flightdeck.h"

#define SWEEP_OD_INIT(v, d, key, n) \
    flightdeck_od_init((flightdeck_od *) (d), (flightdeck_variant) (v), key, n)
#define SWEEP_OD_CLONE_COMPACT(v, clone, d) \
    flightdeck_od_clone_compact((flightdeck_od *) (clone), (const flightdeck_od *) (d))
#define SWEEP_OD_DUPLEX(v, d, out, out_len, in, in_len, trailer) \
    flightdeck_od_duplex((flightdeck_od *) (d), out, out_len, in, in_len, trailer)
#define SWEEP_OD_SQUEEZE(v, d, out, n) flightdeck_od_squeeze((flightdeck_od *) (d), out, n)
#define SWEEP_OD_XOR(v, d, out, in, n) flightdeck_od_xor((flightdeck_od *) (d), out, in, n)
#define SWEEP_DECK_INIT(v, deck, key, n) \
    flightdeck_init((flightdeck *) (deck), (flightdeck_variant) (v), key, n)
#define SWEEP_DECK_CLONE_COMPACT(v, clone, deck) \
    flightdeck_clone_compact((flightdeck *) (clone), (const flightdeck *) (deck))
#define SWEEP_DECK_DUPLEX(v, deck, out, out_len, in, in_len, trailer) \
    flightdeck_duplex((flightdeck *) (deck), out, out_len, in, in_len, trailer)
#define SWEEP_DECK_SQUEEZE(v, deck, out, n) flightdeck_squeeze((flightdeck *) (deck), out, n)
#define SWEEP_DECK_XOR(v, deck, out, in, n) flightdeck_xor((flightdeck *) (deck), out, in, n)
#define SWEEP_BO_INIT(v, t, bo, key, n) \
    flightdeck_bo_init((flightdeck_bo *) (bo), (flightdeck_variant) (v), t, key, n)
#define SWEEP_BO_ENCRYPT(v, t, bo, c, tag, m, m_len, ad, ad_len) \
    flightdeck_bo_encrypt((flightdeck_bo *) (bo), c, tag, m, m_len, ad, ad_len)
#define SWEEP_BO_DECRYPT(v, t, bo, m, c, c_len, tag, ad, ad_len) \
    flightdeck_bo_decrypt((flightdeck_bo *) (bo), m, c, c_len, tag, ad, ad_len)
#define SWEEP_BO_ABSORB(v, t, bo, data, n) flightdeck_bo_absorb((flightdeck_bo *) (bo), data, n)
#define SWEEP_BO_SQUEEZE(v, t, bo, out, n) flightdeck_bo_squeeze((flightdeck_bo *) (bo), out, n)
#include "sweep.h"

static int failures;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                      \
            printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static const flightdeck_variant variants[4] = { FLIGHTDECK_TURBOSHAKE128, FLIGHTDECK_TURBOSHAKE256,
                                                FLIGHTDECK_SHAKE128, FLIGHTDECK_SHAKE256 };

static unsigned
hex_digit(char c)
{
    return c <= '9' ? (unsigned) (c - '0') : (unsigned) (c - 'a' + 10);
}

static void
from_hex(uint8_t *out, const char *hex)
{
    for (size_t i = 0; hex[2 * i] != 0; i++) {
        out[i] = (uint8_t) (hex_digit(hex[2 * i]) << 4 | hex_digit(hex[2 * i + 1]));
    }
}

static int
equals_hex(const uint8_t *bytes, const char *hex)
{
    uint8_t expected[64];

    from_hex(expected, hex);
    return memcmp(bytes, expected, strlen(hex) / 2) == 0;
}

/* The end of a `size`-byte buffer, followed by an unreadable page where mmap() exists, so that going past the end crashes. */
static uint8_t *
guarded(size_t size)
{
#if HAVE_POSIX
    const size_t page = (size_t) sysconf(_SC_PAGESIZE);
    const size_t room = (size / page + 1) * page;
    uint8_t     *region =
        mmap(NULL, room + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (region == MAP_FAILED || mprotect(region + room, page, PROT_NONE) != 0) {
        abort();
    }
    return region + room;
#else
    return (uint8_t *) sweep_alloc(size) + size;
#endif
}

/* The sweeps of sweep.h, against the sums that the Zig implementation gives (https://github.com/jedisct1/zig-flightdeck, commit e5099b0). */
static void
test_sweeps(void)
{
    static const char *const expected[4][4] = {
        {
            "d8f108411c2c3ee7500ee75631388db3e821cdff88db591b142b4fce2aacd3c8",
            "0cde75eb484320944fd70a99a5dd14e3f38c074b87bba071fc35102d81b3123e",
            "798d790110a3247042429aa7e7f661c21b1e6e187c2a49869eeeb12c1a0bddc1",
            "def06a5124d9ab9c2a2f93c656be46f9ff7b6f90a30de5ffeafe64056b02bc02",
        },
        {
            "20877b189aebd040ac01f768a92b198a4ca3d8595a5b95723a20d7f47a01d2c3",
            "6fae1f327bd985d53bf816461ff70086b38f6f94edcfa64408d7b521297b8bcb",
            "f96a79df1233260aa99a540e28430cf06a4f23e5b503fc0223498cf523127ae5",
            "b9fa2a6c3cf559c90af7090df97e1776c3680453b777e77f2c59713625118b91",
        },
        {
            "4985c8308bf622602b77b8ea04edcc6a29082f8c43cfc10d389a00d444113795",
            "aa0aa58c1730038c0c64ab460d594539dc9210bd16864b0c7c7f1b4253d83afc",
            "08269eb318f4aed6d8015788296c5a780dde55b61019d76d026647d1e9b99aaf",
            "6c90e883c5c9401d7ee78cd42a1274190a97960ee1ec77822f370b7ea2627260",
        },
        {
            "d56d56d99d0fe741c066f3a7653e3c624a909521640aa78bcd922243b8df17f7",
            "5d831edce3a7edea5a9a4c63ff37269d5ec6c84b5a39122583109033bd27f8e3",
            "849476f728a6f950b76d8f1e3dd47bfdacb1a6a846a9a976f993bd4c65101935",
            "81822035c16961d0eacec2f9e451d00613834ef26643a173e8a8a26409f9f80e",
        },
    };
    void (*const sweeps[4])(int, uint8_t[32]) = { sweep_od, sweep_deck, sweep_bo, sweep_long };

    for (int sweep = 0; sweep < 4; sweep++) {
        for (int v = 0; v < 4; v++) {
            uint8_t digest[32];

            sweeps[sweep](v, digest);
            CHECK(equals_hex(digest, expected[sweep][v]));
        }
    }
}

/* Vectors made with XKCP (https://github.com/XKCP/XKCP, commit d20c8fc): four duplex calls in a row, checking the last 32 bytes of each output. */
static void
test_deck_vectors(void)
{
    static const char *const expected[4][4] = {
        {
            "7647c9011410b46aed34896b978d4cf7c6de2a2b02dde9cac3db2d9f0d2d4cd1",
            "42bf0b8604f465b3ed708904287b72af6d38a4b313ff7a43f2d6ebbf08a9ce56",
            "df4abd09ab62f58d3d417cd060c7b1125f12985ad7ff319042633e9685c89f59",
            "ad2ea7d74ee3216ceb8c1e7349aa7affe0a257219bbb8bee2aecc472eace57a1",
        },
        {
            "3f28f4f4ed9df27113e56956de92c68fe0ca4d00c3ebc6201bb6e43079851b52",
            "eda592a2605bfc55d541b14849443c794a13c9d117b7d7da60942374ec8b0cb8",
            "7f5c22615c068bb10c36a2da08b85a07d64a1a2702fb0d1dd98548afba694d5b",
            "8661fa82d4652cfad8e26b7b5b2ecb464035b703b774f35b8bb1b3ba17d4833a",
        },
        {
            "46522d87bb182c09cb3894f631f0d2038ae9c519ee8a83d9889e9444d9cac055",
            "b8fdbf7c9ce6de5926f483d3727270e8da3e7c4cbe705d11b2aa04a227f531f0",
            "0e2866d9275fa42f13afa406b83dbf5217d3885a53eed1ec27f0fffc8cc38eea",
            "e30f7c5b3cb6c97665159f35d59238a3c1ae808ff381afb2edc77e6a10dc3bd7",
        },
        {
            "48a3a3e770baa4b4ad1c8433a686033a66274d4b7557e58fefd889c65f6f13d5",
            "a240322bf03fa890d4b9804b574988bd1e224bd2b02e277068fa82ddcbb0ed67",
            "80646d3d6e1af65b3b3aefcc52af92c1ce607b429964d935da5149fe90f46bd6",
            "0de4155a80187d7967547300fe5033e2d2811fde27cfeaf0932e0076000f8b8d",
        },
    };
    enum { OUT_MAX = 50000 };
    uint8_t  key[16];
    uint8_t *in  = sweep_alloc(12 * 8192);
    uint8_t *out = sweep_alloc(OUT_MAX);

    sweep_pattern(key, sizeof key, 1, 0);
    for (int v = 0; v < 4; v++) {
        const size_t   chunk       = sweep_chunk(v);
        const size_t   in_lens[4]  = { 0, chunk + 1, 11 * chunk + chunk / 2 + 17, 1 };
        const unsigned trailers[4] = { 1, 7, 31, 2 };
        const size_t   out_lens[4] = { 32, 32, 32, OUT_MAX };
        flightdeck     deck;

        flightdeck_init(&deck, variants[v], key, sizeof key);
        for (size_t i = 0; i < 4; i++) {
            sweep_pattern(in, in_lens[i], 1, 17 * i);
            flightdeck_duplex(&deck, out, out_lens[i], in, in_lens[i], trailers[i]);
            CHECK(equals_hex(out + out_lens[i] - 32, expected[v][i]));
        }
    }
    free(in);
    free(out);
}

/* Vectors made with the same version of XKCP: a session of four short messages, then a long message with long associated data, encrypted and decrypted in place. */
static void
test_bo_vectors(void)
{
    static const char *const sessions[2][4] = {
        {
            "1c106e7483215251a6d39d1b7ced5081",
            "a1ee10ab51029ad2520e746fa3616d10",
            "c0e75ee6dbd1815bace517f6e80e78c4fe8de69c68",
            "fd79fad9b021cf4e7d5e2f8a45a5fec28b5d267ff026b830e3636a05424870a0589dc31a036457f4b2ffa6"
            "78aeb4742fcd8b280e257586cc",
        },
        {
            "50a6969d91e4a3cf6cbab92862e5331d",
            "50ca4c773b1fae7dd184da8e1a57cb11",
            "aaa5df8bcbfdc6e371c38b184644023fe70204d650",
            "f045a1839d595957f5719040ee4ede0d6739e3661ff96bba0115410180fb8429c3b39f2085e9192d5b82af"
            "48bbc2ebc63f552116f9d440ff",
        },
    };
    static const char *const long_ciphertexts[2] = {
        "448ad0859b25d898a569dbc54e9f10a7845a2085334be423dd62081236ef3477",
        "8f73baf8369dbd41a3b5d2d90952d136150794452602586aeb99e98c4af29c20",
    };
    static const char *const long_tags[2] = {
        "bfea226043c96f981d2a87ec6b595fd7517fca51190b24c87aaa4b4f46285baf",
        "e531fa17ca00de8db462ddfe079be91e2e55d0b06ea72932f7ecffe19255c3aa",
    };
    static const size_t ad_lens[4] = { 0, 5, 0, 7 };
    static const size_t m_lens[4]  = { 0, 0, 5, 40 };
    enum { AD_LEN = 20000, M_LEN = 80000 };
    uint8_t  key[32];
    uint8_t *ad       = sweep_alloc(AD_LEN);
    uint8_t *m        = sweep_alloc(M_LEN);
    uint8_t *expected = sweep_alloc(M_LEN);

    sweep_pattern(key, sizeof key, 1, 0);
    for (int v = 0; v < 2; v++) {
        flightdeck_bo sender;
        flightdeck_bo receiver;
        uint8_t       c[40];
        uint8_t       decrypted[40];
        uint8_t       tag[32];

        flightdeck_bo_init(&sender, variants[v], 16, key, 16);
        receiver = sender;
        for (size_t i = 0; i < 4; i++) {
            sweep_pattern(ad, ad_lens[i], 1, 17 * i);
            sweep_pattern(m, m_lens[i], 3, 29 * i);
            flightdeck_bo_encrypt(&sender, c, tag, m, m_lens[i], ad, ad_lens[i]);
            from_hex(expected, sessions[v][i]);
            CHECK(memcmp(c, expected, m_lens[i]) == 0);
            CHECK(memcmp(tag, expected + m_lens[i], 16) == 0);
            CHECK(flightdeck_bo_decrypt(&receiver, decrypted, c, m_lens[i], tag, ad, ad_lens[i]) ==
                  0);
            CHECK(memcmp(decrypted, m, m_lens[i]) == 0);
        }

        sweep_pattern(ad, AD_LEN, 1, 0);
        sweep_pattern(m, M_LEN, 3, 0);
        flightdeck_bo_init(&sender, variants[v], 32, key, 32);
        receiver = sender;
        flightdeck_bo_encrypt(&sender, m, tag, m, M_LEN, ad, AD_LEN);
        CHECK(equals_hex(m + M_LEN - 32, long_ciphertexts[v]));
        CHECK(equals_hex(tag, long_tags[v]));
        CHECK(flightdeck_bo_decrypt(&receiver, m, m, M_LEN, tag, ad, AD_LEN) == 0);
        sweep_pattern(expected, M_LEN, 3, 0);
        CHECK(memcmp(m, expected, M_LEN) == 0);
    }
    free(ad);
    free(m);
    free(expected);
}

/*
 * flightdeck_hash() has to give what a deck with an empty key gives.
 * Inputs and digests end right before a guard page, since reading too far wouldn't change a digest.
 */
static void
test_hash(void)
{
    static const size_t out_lens[9] = { 0, 1, 31, 32, 33, 64, 65, 160, 700 };
    uint8_t            *in_end      = guarded(19 * 8192);
    uint8_t            *out_end     = guarded(700);
    uint8_t             expected[700];
    size_t              step = 0;

    for (int v = 0; v < 4; v++) {
        const size_t rate     = sweep_rate(v);
        const size_t chunk    = sweep_chunk(v);
        const size_t tails[7] = { 0, 1, rate - 1, rate, rate + 1, chunk / 2, chunk - 1 };

        for (size_t chunks = 0; chunks <= 18; chunks++) {
            for (size_t k = 0; k < 7; k++, step++) {
                const size_t len     = chunks * chunk + tails[k];
                const size_t out_len = out_lens[step % 9];
                flightdeck   deck;

                sweep_pattern(in_end - len, len, 3, step);
                flightdeck_hash(variants[v], out_end - out_len, out_len, in_end - len, len);
                flightdeck_init(&deck, variants[v], NULL, 0);
                flightdeck_duplex(&deck, expected, out_len, in_end - len, len,
                                  FLIGHTDECK_TRAILER_EMPTY);
                CHECK(memcmp(out_end - out_len, expected, out_len) == 0);
            }
        }
    }
}

/*
 * Output that ends right before a guard page, since reading or writing too far wouldn't change any result.
 * First deck output XORed in place, with lengths around the batch sizes of the backends, then duplex output.
 */
static void
test_output_ends(void)
{
    static const size_t blocks[6] = { 1, 4, 5, 8, 9, 20 };
    uint8_t            *end       = guarded(20 * 160 + 1);
    uint8_t             expected[20 * 160 + 1];

    for (int v = 0; v < 4; v++) {
        flightdeck    start;
        flightdeck_od d;

        flightdeck_init(&start, variants[v], (const uint8_t *) "key", 3);
        flightdeck_duplex(&start, NULL, 0, NULL, 0, FLIGHTDECK_TRAILER_EMPTY);
        for (size_t k = 0; k < 6; k++) {
            for (size_t extra = 0; extra < 3; extra++) {
                const size_t len  = blocks[k] * sweep_rate(v) + extra - 1;
                flightdeck   deck = start;

                sweep_pattern(expected, len, 7, k);
                memcpy(end - len, expected, len);
                flightdeck_xor(&deck, end - len, end - len, len);
                deck = start;
                flightdeck_xor(&deck, end - len, end - len, len);
                CHECK(memcmp(end - len, expected, len) == 0);
            }
        }
        flightdeck_od_init(&d, variants[v], NULL, 0);
        flightdeck_od_duplex(&d, end - 7, 7, NULL, 0, FLIGHTDECK_TRAILER_EMPTY);
        flightdeck_od_squeeze(&d, end - 9, 9);
        flightdeck_od_xor(&d, end - 11, end - 11, 11);
    }
}

/* Deck-BO has to reject messages that aren't authentic, zero their plaintext, and leave the session as it was. */
static void
test_bo_rejects(void)
{
    static const uint8_t m[9]     = { 'a', ' ', 'm', 'e', 's', 's', 'a', 'g', 'e' };
    static const uint8_t ad[4]    = { 'd', 'a', 't', 'a' };
    static const uint8_t zeros[9] = { 0 };

    for (int v = 0; v < 4; v++) {
        flightdeck_bo sender;
        flightdeck_bo receiver;
        uint8_t       c[9];
        uint8_t       out[9];
        uint8_t       tag[200];
        uint8_t       squeezed[10];
        uint8_t       expected[10];

        /*
         * Every byte of the tag has to count, for a short tag and for a long one that the library compares in several pieces.
         * The message is empty, because with a message any change to the tag would also change the whole plaintext.
         */
        for (size_t tag_len = 16; tag_len <= 200; tag_len += 184) {
            flightdeck_bo_init(&sender, variants[v], tag_len, (const uint8_t *) "key", 3);
            receiver = sender;
            flightdeck_bo_encrypt(&sender, NULL, tag, NULL, 0, ad, sizeof ad);
            for (size_t i = 0; i < tag_len; i++) {
                tag[i] ^= (uint8_t) (1 << (i % 8));
                CHECK(flightdeck_bo_decrypt(&receiver, NULL, NULL, 0, tag, ad, sizeof ad) == -1);
                tag[i] ^= (uint8_t) (1 << (i % 8));
            }
            CHECK(flightdeck_bo_decrypt(&receiver, NULL, NULL, 0, tag, ad, sizeof ad) == 0);
        }

        /* A damaged ciphertext, between two squeezes. */
        flightdeck_bo_init(&sender, variants[v], 16, (const uint8_t *) "key", 3);
        flightdeck_bo_absorb(&sender, ad, sizeof ad);
        receiver = sender;
        flightdeck_bo_squeeze(&sender, expected, sizeof expected);
        flightdeck_bo_squeeze(&receiver, squeezed, 5);
        flightdeck_bo_encrypt(&sender, c, tag, m, sizeof m, ad, sizeof ad);
        c[0] ^= 1;
        memset(out, 0xff, sizeof out);
        CHECK(flightdeck_bo_decrypt(&receiver, out, c, sizeof c, tag, ad, sizeof ad) == -1);
        CHECK(memcmp(out, zeros, sizeof out) == 0);
        c[0] ^= 1;

        /* The session didn't move: squeezing picks up where it left off, and the real message is accepted. */
        flightdeck_bo_squeeze(&receiver, squeezed + 5, 5);
        CHECK(memcmp(squeezed, expected, sizeof expected) == 0);
        CHECK(flightdeck_bo_decrypt(&receiver, out, c, sizeof c, tag, ad, sizeof ad) == 0);
    }
}

static void
test_parameters(void)
{
    for (int v = 0; v < 4; v++) {
        CHECK(flightdeck_key_len(variants[v]) == (v & 1 ? 32 : 16));
        CHECK(flightdeck_key_len_max(variants[v]) == sweep_rate(v));
    }

#ifdef FLIGHTDECK_PORTABLE
    CHECK(strcmp(flightdeck_implementation(), "portable") == 0);
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    /*
     * When the compiler's runtime sees AVX2 or AVX-512, the library has to use it too.
     * The reverse isn't checked, because with some C runtimes the builtin always says no.
     */
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi") &&
        __builtin_cpu_supports("bmi2")) {
        const char *expected = "avx2";

#    ifndef FLIGHTDECK_NO_AVX512
        if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl")) {
            expected = "avx512";
        }
#    endif
        CHECK(strcmp(flightdeck_implementation(), expected) == 0);
    }
#endif
}

#if HAVE_POSIX

enum { MISUSES = 15, ABORTED = 42 };

/* Makes one of the calls that the header forbids, or none when `which` is MISUSES. */
static void
misuse(int which)
{
    static const uint8_t zeros[161] = { 0 };
    uint8_t              out[161]   = { 0 };
    flightdeck_od        d;
    flightdeck_od        d_clone;
    flightdeck           deck;
    flightdeck           deck_clone;
    flightdeck_bo        bo;

    flightdeck_od_init(&d, FLIGHTDECK_TURBOSHAKE128, NULL, 0);
    flightdeck_init(&deck, FLIGHTDECK_TURBOSHAKE128, NULL, 0);
    switch (which) {
    case 0: /* a variant that doesn't exist */
        flightdeck_hash((flightdeck_variant) 4, out, 32, NULL, 0);
        break;
    case 1: /* a key longer than a block of a 256-bit instance */
        flightdeck_od_init(&d, FLIGHTDECK_TURBOSHAKE256, zeros, 129);
        break;
    case 2: /* squeezing before the first duplex call */
        flightdeck_od_squeeze(&d, out, 1);
        break;
    case 3: /* squeezing a compact clone */
        flightdeck_od_duplex(&d, NULL, 0, NULL, 0, 1);
        flightdeck_od_clone_compact(&d_clone, &d);
        flightdeck_od_squeeze(&d_clone, out, 1);
        break;
    case 4: /* 0 is not a trailer */
        flightdeck_od_duplex(&d, NULL, 0, NULL, 0, 0);
        break;
    case 5: /* a trailer that is too long */
        flightdeck_od_duplex(&d, NULL, 0, NULL, 0, FLIGHTDECK_OD_TRAILER_MAX + 1);
        break;
    case 6: /* a block that is too long */
        flightdeck_od_duplex(&d, NULL, 0, zeros, 161, 1);
        break;
    case 7: /* squeezing past the end of the block */
        flightdeck_od_duplex(&d, out, 100, NULL, 0, 1);
        flightdeck_od_squeeze(&d, out, 61);
        break;
    case 8: /* XORing past the end of the block */
        flightdeck_od_duplex(&d, out, 50, NULL, 0, 1);
        flightdeck_od_xor(&d, out, out, 50);
        flightdeck_od_xor(&d, out, out, 61);
        break;
    case 9: /* squeezing before the first duplex call */
        flightdeck_squeeze(&deck, out, 1);
        break;
    case 10: /* squeezing a compact clone */
        flightdeck_duplex(&deck, NULL, 0, NULL, 0, 1);
        flightdeck_clone_compact(&deck_clone, &deck);
        flightdeck_squeeze(&deck_clone, out, 1);
        break;
    case 11: /* 0 is not a trailer */
        flightdeck_duplex(&deck, NULL, 0, NULL, 0, 0);
        break;
    case 12: /* a trailer that is too long */
        flightdeck_duplex(&deck, NULL, 0, NULL, 0, FLIGHTDECK_TRAILER_MAX + 1);
        break;
    case 13: /* XORing with no input */
        flightdeck_duplex(&deck, NULL, 0, NULL, 0, 1);
        flightdeck_xor(&deck, out, NULL, 4);
        break;
    case 14: /* an empty tag */
        flightdeck_bo_init(&bo, FLIGHTDECK_TURBOSHAKE128, 0, NULL, 0);
        break;
    default:
        break;
    }
}

static void
on_abort(int signal_number)
{
    (void) signal_number;
    _exit(ABORTED);
}

/*
 * Runs a misuse in a child process and tells whether it called abort().
 * The child catches SIGABRT itself, because the system's crash reporter can take seconds for each crash.
 */
static int
aborts(int which)
{
    int         status = 0;
    const pid_t pid    = fork();

    if (pid == 0) {
        signal(SIGABRT, on_abort);
        misuse(which);
        _exit(0);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == ABORTED;
}

static void
test_misuse(void)
{
    for (int which = 0; which < MISUSES; which++) {
        CHECK(aborts(which));
    }
    CHECK(!aborts(MISUSES));
}

#endif

int
main(void)
{
    test_parameters();
    test_sweeps();
    test_deck_vectors();
    test_bo_vectors();
    test_hash();
    test_output_ends();
    test_bo_rejects();
#if HAVE_POSIX
    test_misuse();
#endif
    if (failures != 0) {
        printf("%d checks failed (%s)\n", failures, flightdeck_implementation());
        return 1;
    }
    printf("all tests passed (%s)\n", flightdeck_implementation());
    return 0;
}
