/*
 * Keccak-p[1600] on FDECK_N states at once, and the FlightDeck functions of a backend built on it.
 *
 * flightdeck.c includes this file once for each backend and number of states, after defining FDECK_N, FDECK_FN() and FDECK_TARGET.
 * Vector code also defines FDECK_V, FDECK_XOR(), FDECK_XOR3(), FDECK_CHI(), FDECK_ROL(), FDECK_SET1(), FDECK_LOAD_LANES() and FDECK_STORE_LANES(), plus FDECK_LOAD_BYTES() for a single state.
 * Without them, the code works on plain 64-bit words.
 *
 * Optional settings:
 * - FDECK_NINTH_STATE, to run a ninth state on plain 64-bit words next to eight in vectors.
 * - FDECK_VECTOR_KEEP(), see flightdeck.c.
 *
 * Loading and storing blocks is written for 4 and 8 states on x86.
 *
 * A vector holds the same lane of every state.
 * A block overwrites the start of a state, so only the lanes after it, the capacity, carry over from one block to the next.
 * They are passed around as `caps[FDECK_N * lane + state]`.
 *
 * Don't loop over the lanes of a state: if the compiler keeps the loop, it keeps the state in memory instead of registers.
 */

#if defined(FDECK_V) && defined(FDECK_VECTOR_KEEP)
#    define FDECK_KEEP(x) FDECK_VECTOR_KEEP(x)
#else
#    define FDECK_KEEP(x) ((void) 0)
#endif

#ifndef FDECK_V
#    define FDECK_V                 uint64_t
#    define FDECK_XOR(a, b)         FDECK_WORD_XOR(a, b)
#    define FDECK_XOR3(a, b, c)     ((a) ^ (b) ^ (c))
#    define FDECK_CHI(a, b, c)      FDECK_WORD_CHI(a, b, c)
#    define FDECK_ROL(a, n)         fdeck_rotl(a, n)
#    define FDECK_SET1(x)           ((uint64_t) (x))
#    define FDECK_LOAD_LANES(p)     (*(p))
#    define FDECK_STORE_LANES(p, v) (*(p) = (v))
#    define FDECK_LOAD_BYTES(p)     fdeck_load_lane(p)
#endif

#ifndef FDECK_NINTH_STATE
#    define FDECK_NINTH_STATE 0
#endif

#define FDECK_INLINE FDECK_ALWAYS_INLINE FDECK_TARGET

#define FDECK_PARITY(a, x) \
    FDECK_XOR(FDECK_XOR3(FDECK_XOR(a[x], a[x + 5]), a[x + 10], a[x + 15]), a[x + 20])

/* What theta adds to the column `x`. */
#define FDECK_THETA(d, x, previous, next)               \
    do {                                                \
        d[x] = FDECK_XOR(previous, FDECK_ROL(next, 1)); \
        FDECK_KEEP(d[x]);                               \
    } while (0)

/*
 * Row `y` of the next state, from five lanes of this one.
 * The first column already went through theta.
 * `V`, `XOR`, `ROL` and `CHI` are the type of a lane and its operations.
 */
#define FDECK_ROW_OF(V, XOR, ROL, CHI, e, a, d, y, s0, r0, s1, r1, s2, r2, s3, r3, s4, r4) \
    do {                                                                                   \
        const V t0   = (s0) % 5 == 0 ? a[s0] : XOR(a[s0], d[(s0) % 5]);                    \
        const V t1   = (s1) % 5 == 0 ? a[s1] : XOR(a[s1], d[(s1) % 5]);                    \
        const V t2   = (s2) % 5 == 0 ? a[s2] : XOR(a[s2], d[(s2) % 5]);                    \
        const V t3   = (s3) % 5 == 0 ? a[s3] : XOR(a[s3], d[(s3) % 5]);                    \
        const V t4   = (s4) % 5 == 0 ? a[s4] : XOR(a[s4], d[(s4) % 5]);                    \
        const V b0   = (r0) == 0 ? t0 : ROL(t0, r0);                                       \
        const V b1   = (r1) == 0 ? t1 : ROL(t1, r1);                                       \
        const V b2   = (r2) == 0 ? t2 : ROL(t2, r2);                                       \
        const V b3   = (r3) == 0 ? t3 : ROL(t3, r3);                                       \
        const V b4   = (r4) == 0 ? t4 : ROL(t4, r4);                                       \
        e[5 * y]     = CHI(b0, b1, b2);                                                    \
        e[5 * y + 1] = CHI(b1, b2, b3);                                                    \
        e[5 * y + 2] = CHI(b2, b3, b4);                                                    \
        e[5 * y + 3] = CHI(b3, b4, b0);                                                    \
        e[5 * y + 4] = CHI(b4, b0, b1);                                                    \
    } while (0)

/* For each lane of each row: the lane it comes from, and how far that one is rotated. */
#define FDECK_EACH_ROW(ROW)                           \
    do {                                              \
        ROW(0, 0, 0, 6, 44, 12, 43, 18, 21, 24, 14);  \
        ROW(1, 3, 28, 9, 20, 10, 3, 16, 45, 22, 61);  \
        ROW(2, 1, 1, 7, 6, 13, 25, 19, 8, 20, 18);    \
        ROW(3, 4, 27, 5, 36, 11, 10, 17, 15, 23, 56); \
        ROW(4, 2, 62, 8, 55, 14, 39, 15, 41, 21, 2);  \
    } while (0)

/*
 * One round.
 * `c` is the parity of each column, and gets updated for the next round.
 *
 * The order of the steps is the fastest one we measured.
 * Theta goes to the first column up front, so that 32 registers are enough for the rest.
 */
FDECK_INLINE void
FDECK_FN(round)(FDECK_V a[25], FDECK_V c[5], uint64_t round_constant)
{
    FDECK_V d[5];
    FDECK_V e[25];

    FDECK_THETA(d, 0, c[4], c[1]);
    FDECK_THETA(d, 2, c[1], c[3]);
    FDECK_THETA(d, 4, c[3], c[0]);
    FDECK_THETA(d, 1, c[0], c[2]);
    FDECK_THETA(d, 3, c[2], c[4]);

    a[0]  = FDECK_XOR(a[0], d[0]);
    a[5]  = FDECK_XOR(a[5], d[0]);
    a[10] = FDECK_XOR(a[10], d[0]);
    a[15] = FDECK_XOR(a[15], d[0]);
    a[20] = FDECK_XOR(a[20], d[0]);

#define FDECK_ROW(y, ...) \
    FDECK_ROW_OF(FDECK_V, FDECK_XOR, FDECK_ROL, FDECK_CHI, e, a, d, y, __VA_ARGS__)
    FDECK_EACH_ROW(FDECK_ROW);
#undef FDECK_ROW

    e[0] = FDECK_XOR(e[0], FDECK_SET1(round_constant));
    c[0] = FDECK_PARITY(e, 0);
    c[1] = FDECK_PARITY(e, 1);
    c[2] = FDECK_PARITY(e, 2);
    c[3] = FDECK_PARITY(e, 3);
    c[4] = FDECK_PARITY(e, 4);
    memcpy(a, e, sizeof e);
}

/*
 * The last 12 or 24 rounds of Keccak-f[1600], two per loop iteration.
 *
 * Writing all of them out isn't faster, and the code gets too big for the CPU's cache of decoded instructions.
 * The last two rounds are outside the loop, so that the compiler can skip the lanes of the last one that nobody reads.
 */
FDECK_INLINE void
FDECK_FN(permute)(FDECK_V a[25], unsigned rounds)
{
    FDECK_V c[5];

    c[0] = FDECK_PARITY(a, 0);
    c[1] = FDECK_PARITY(a, 1);
    c[2] = FDECK_PARITY(a, 2);
    c[3] = FDECK_PARITY(a, 3);
    c[4] = FDECK_PARITY(a, 4);
    for (unsigned first = 24 - rounds; first < 22; first += 2) {
        FDECK_FN(round)(a, c, fdeck_round_constants[first]);
        FDECK_FN(round)(a, c, fdeck_round_constants[first + 1]);
    }
    FDECK_FN(round)(a, c, fdeck_round_constants[22]);
    FDECK_FN(round)(a, c, fdeck_round_constants[23]);
}

#define FDECK_CAPACITY_LANES(rate_lanes, DO) \
    do {                                     \
        if (rate_lanes == 16) {              \
            DO(16);                          \
            DO(17);                          \
            DO(18);                          \
            DO(19);                          \
        }                                    \
        DO(20);                              \
        DO(21);                              \
        DO(22);                              \
        DO(23);                              \
        DO(24);                              \
    } while (0)

FDECK_INLINE void
FDECK_FN(load_capacity)(FDECK_V a[25], const uint64_t *caps, unsigned rate_lanes)
{
#define FDECK_DO(lane) a[lane] = FDECK_LOAD_LANES(caps + FDECK_N * (lane - rate_lanes))
    FDECK_CAPACITY_LANES(rate_lanes, FDECK_DO);
#undef FDECK_DO
}

FDECK_INLINE void
FDECK_FN(store_capacity)(uint64_t *caps, const FDECK_V a[25], unsigned rate_lanes)
{
#define FDECK_DO(lane) FDECK_STORE_LANES(caps + FDECK_N * (lane - rate_lanes), a[lane])
    FDECK_CAPACITY_LANES(rate_lanes, FDECK_DO);
#undef FDECK_DO
}

#if FDECK_N == 8

/* Turn 8 rows of 8 lanes into 8 columns, or back. */
FDECK_INLINE void
FDECK_FN(transpose)(FDECK_V v[8])
{
    const FDECK_V t0 = _mm512_unpacklo_epi64(v[0], v[1]);
    const FDECK_V t1 = _mm512_unpackhi_epi64(v[0], v[1]);
    const FDECK_V t2 = _mm512_unpacklo_epi64(v[2], v[3]);
    const FDECK_V t3 = _mm512_unpackhi_epi64(v[2], v[3]);
    const FDECK_V t4 = _mm512_unpacklo_epi64(v[4], v[5]);
    const FDECK_V t5 = _mm512_unpackhi_epi64(v[4], v[5]);
    const FDECK_V t6 = _mm512_unpacklo_epi64(v[6], v[7]);
    const FDECK_V t7 = _mm512_unpackhi_epi64(v[6], v[7]);
    const FDECK_V u0 = _mm512_shuffle_i64x2(t0, t2, 0x88);
    const FDECK_V u1 = _mm512_shuffle_i64x2(t0, t2, 0xdd);
    const FDECK_V u2 = _mm512_shuffle_i64x2(t1, t3, 0x88);
    const FDECK_V u3 = _mm512_shuffle_i64x2(t1, t3, 0xdd);
    const FDECK_V u4 = _mm512_shuffle_i64x2(t4, t6, 0x88);
    const FDECK_V u5 = _mm512_shuffle_i64x2(t4, t6, 0xdd);
    const FDECK_V u6 = _mm512_shuffle_i64x2(t5, t7, 0x88);
    const FDECK_V u7 = _mm512_shuffle_i64x2(t5, t7, 0xdd);

    v[0] = _mm512_shuffle_i64x2(u0, u4, 0x88);
    v[1] = _mm512_shuffle_i64x2(u2, u6, 0x88);
    v[2] = _mm512_shuffle_i64x2(u1, u5, 0x88);
    v[3] = _mm512_shuffle_i64x2(u3, u7, 0x88);
    v[4] = _mm512_shuffle_i64x2(u0, u4, 0xdd);
    v[5] = _mm512_shuffle_i64x2(u2, u6, 0xdd);
    v[6] = _mm512_shuffle_i64x2(u1, u5, 0xdd);
    v[7] = _mm512_shuffle_i64x2(u3, u7, 0xdd);
}

FDECK_INLINE FDECK_V
FDECK_FN(load_pair)(const uint8_t *low, const uint8_t *high)
{
    return _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_loadu_si256((const __m256i *) low)),
                              _mm256_loadu_si256((const __m256i *) high), 1);
}

/* Load the block of each state into its first lanes. */
FDECK_INLINE void
FDECK_FN(load_block)(FDECK_V a[25], const uint8_t *const in[8], unsigned rate_lanes)
{
#    define FDECK_ROWS(lane)                                    \
        do {                                                    \
            a[lane]     = _mm512_loadu_si512(in[0] + 8 * lane); \
            a[lane + 1] = _mm512_loadu_si512(in[1] + 8 * lane); \
            a[lane + 2] = _mm512_loadu_si512(in[2] + 8 * lane); \
            a[lane + 3] = _mm512_loadu_si512(in[3] + 8 * lane); \
            a[lane + 4] = _mm512_loadu_si512(in[4] + 8 * lane); \
            a[lane + 5] = _mm512_loadu_si512(in[5] + 8 * lane); \
            a[lane + 6] = _mm512_loadu_si512(in[6] + 8 * lane); \
            a[lane + 7] = _mm512_loadu_si512(in[7] + 8 * lane); \
            FDECK_FN(transpose)(a + lane);                      \
        } while (0)
    FDECK_ROWS(0);
    FDECK_ROWS(8);
#    undef FDECK_ROWS
    if (rate_lanes == 20) {
        /* Four lanes are left. Pairing the states this way only needs shuffles of 128-bit halves. */
        const FDECK_V s02 = FDECK_FN(load_pair)(in[0] + 128, in[2] + 128);
        const FDECK_V s13 = FDECK_FN(load_pair)(in[1] + 128, in[3] + 128);
        const FDECK_V s46 = FDECK_FN(load_pair)(in[4] + 128, in[6] + 128);
        const FDECK_V s57 = FDECK_FN(load_pair)(in[5] + 128, in[7] + 128);
        const FDECK_V t0  = _mm512_unpacklo_epi64(s02, s13);
        const FDECK_V t1  = _mm512_unpackhi_epi64(s02, s13);
        const FDECK_V t2  = _mm512_unpacklo_epi64(s46, s57);
        const FDECK_V t3  = _mm512_unpackhi_epi64(s46, s57);

        a[16] = _mm512_shuffle_i64x2(t0, t2, 0x88);
        a[17] = _mm512_shuffle_i64x2(t1, t3, 0x88);
        a[18] = _mm512_shuffle_i64x2(t0, t2, 0xdd);
        a[19] = _mm512_shuffle_i64x2(t1, t3, 0xdd);
    }
}

/*
 * Write the block of each state, one state after the other, XORed with `in` if there is one.
 * This overwrites the block in `a`.
 */
FDECK_INLINE void
FDECK_FN(store_block)(uint8_t *out, const uint8_t *in, FDECK_V a[25], unsigned rate_lanes)
{
    const size_t rate = 8 * (size_t) rate_lanes;

#    define FDECK_ROW_OUT(lane, i)                                                         \
        do {                                                                               \
            FDECK_V row = a[lane + i];                                                     \
            if (in != NULL) {                                                              \
                row = _mm512_xor_si512(row, _mm512_loadu_si512(in + i * rate + 8 * lane)); \
            }                                                                              \
            _mm512_storeu_si512(out + i * rate + 8 * lane, row);                           \
        } while (0)
#    define FDECK_ROWS(lane)               \
        do {                               \
            FDECK_FN(transpose)(a + lane); \
            FDECK_ROW_OUT(lane, 0);        \
            FDECK_ROW_OUT(lane, 1);        \
            FDECK_ROW_OUT(lane, 2);        \
            FDECK_ROW_OUT(lane, 3);        \
            FDECK_ROW_OUT(lane, 4);        \
            FDECK_ROW_OUT(lane, 5);        \
            FDECK_ROW_OUT(lane, 6);        \
            FDECK_ROW_OUT(lane, 7);        \
        } while (0)
    FDECK_ROWS(0);
    FDECK_ROWS(8);
#    undef FDECK_ROWS
#    undef FDECK_ROW_OUT
    if (rate_lanes == 20) {
        const FDECK_V t0   = _mm512_unpacklo_epi64(a[16], a[17]);
        const FDECK_V t1   = _mm512_unpackhi_epi64(a[16], a[17]);
        const FDECK_V t2   = _mm512_unpacklo_epi64(a[18], a[19]);
        const FDECK_V t3   = _mm512_unpackhi_epi64(a[18], a[19]);
        const FDECK_V low  = _mm512_setr_epi64(0, 1, 8, 9, 2, 3, 10, 11);
        const FDECK_V high = _mm512_setr_epi64(4, 5, 12, 13, 6, 7, 14, 15);

/* `pair` holds the last four lanes of the states `i` and `j`. */
#    define FDECK_PAIR_OUT(pair, i, j)                                                             \
        do {                                                                                       \
            FDECK_V rows = pair;                                                                   \
            if (in != NULL) {                                                                      \
                rows = _mm512_xor_si512(                                                           \
                    rows, FDECK_FN(load_pair)(in + i * rate + 128, in + j * rate + 128));          \
            }                                                                                      \
            _mm256_storeu_si256((__m256i *) (out + i * rate + 128), _mm512_castsi512_si256(rows)); \
            _mm256_storeu_si256((__m256i *) (out + j * rate + 128),                                \
                                _mm512_extracti64x4_epi64(rows, 1));                               \
        } while (0)
        FDECK_PAIR_OUT(_mm512_permutex2var_epi64(t0, low, t2), 0, 2);
        FDECK_PAIR_OUT(_mm512_permutex2var_epi64(t1, low, t3), 1, 3);
        FDECK_PAIR_OUT(_mm512_permutex2var_epi64(t0, high, t2), 4, 6);
        FDECK_PAIR_OUT(_mm512_permutex2var_epi64(t1, high, t3), 5, 7);
#    undef FDECK_PAIR_OUT
    }
}

#    define FDECK_COUNTERS(index)                                \
        _mm512_add_epi64(_mm512_set1_epi64((long long) (index)), \
                         _mm512_setr_epi64(0, 1, 2, 3, 4, 5, 6, 7))

#elif FDECK_N == 4

FDECK_INLINE void
FDECK_FN(transpose)(FDECK_V v[4])
{
    const FDECK_V t0 = _mm256_unpacklo_epi64(v[0], v[1]);
    const FDECK_V t1 = _mm256_unpackhi_epi64(v[0], v[1]);
    const FDECK_V t2 = _mm256_unpacklo_epi64(v[2], v[3]);
    const FDECK_V t3 = _mm256_unpackhi_epi64(v[2], v[3]);

    v[0] = _mm256_permute2x128_si256(t0, t2, 0x20);
    v[1] = _mm256_permute2x128_si256(t1, t3, 0x20);
    v[2] = _mm256_permute2x128_si256(t0, t2, 0x31);
    v[3] = _mm256_permute2x128_si256(t1, t3, 0x31);
}

FDECK_INLINE void
FDECK_FN(load_block)(FDECK_V a[25], const uint8_t *const in[4], unsigned rate_lanes)
{
#    define FDECK_ROWS(lane)                                                        \
        do {                                                                        \
            a[lane]     = _mm256_loadu_si256((const __m256i *) (in[0] + 8 * lane)); \
            a[lane + 1] = _mm256_loadu_si256((const __m256i *) (in[1] + 8 * lane)); \
            a[lane + 2] = _mm256_loadu_si256((const __m256i *) (in[2] + 8 * lane)); \
            a[lane + 3] = _mm256_loadu_si256((const __m256i *) (in[3] + 8 * lane)); \
            FDECK_FN(transpose)(a + lane);                                          \
        } while (0)
    FDECK_ROWS(0);
    FDECK_ROWS(4);
    FDECK_ROWS(8);
    FDECK_ROWS(12);
    if (rate_lanes == 20) {
        FDECK_ROWS(16);
    }
#    undef FDECK_ROWS
}

FDECK_INLINE void
FDECK_FN(store_block)(uint8_t *out, const uint8_t *in, FDECK_V a[25], unsigned rate_lanes)
{
    const size_t rate = 8 * (size_t) rate_lanes;

#    define FDECK_ROW_OUT(lane, i)                                                          \
        do {                                                                                \
            FDECK_V row = a[lane + i];                                                      \
            if (in != NULL) {                                                               \
                row = _mm256_xor_si256(                                                     \
                    row, _mm256_loadu_si256((const __m256i *) (in + i * rate + 8 * lane))); \
            }                                                                               \
            _mm256_storeu_si256((__m256i *) (out + i * rate + 8 * lane), row);              \
        } while (0)
#    define FDECK_ROWS(lane)               \
        do {                               \
            FDECK_FN(transpose)(a + lane); \
            FDECK_ROW_OUT(lane, 0);        \
            FDECK_ROW_OUT(lane, 1);        \
            FDECK_ROW_OUT(lane, 2);        \
            FDECK_ROW_OUT(lane, 3);        \
        } while (0)
    FDECK_ROWS(0);
    FDECK_ROWS(4);
    FDECK_ROWS(8);
    FDECK_ROWS(12);
    if (rate_lanes == 20) {
        FDECK_ROWS(16);
    }
#    undef FDECK_ROWS
#    undef FDECK_ROW_OUT
}

#    define FDECK_COUNTERS(index) \
        _mm256_add_epi64(_mm256_set1_epi64x((long long) (index)), _mm256_setr_epi64x(0, 1, 2, 3))

#else

FDECK_INLINE void
FDECK_FN(load_block)(FDECK_V a[25], const uint8_t *const in[1], unsigned rate_lanes)
{
#    define FDECK_LANES(lane)                                      \
        do {                                                       \
            a[lane]     = FDECK_LOAD_BYTES(in[0] + 8 * lane);      \
            a[lane + 1] = FDECK_LOAD_BYTES(in[0] + 8 * lane + 8);  \
            a[lane + 2] = FDECK_LOAD_BYTES(in[0] + 8 * lane + 16); \
            a[lane + 3] = FDECK_LOAD_BYTES(in[0] + 8 * lane + 24); \
        } while (0)
    FDECK_LANES(0);
    FDECK_LANES(4);
    FDECK_LANES(8);
    FDECK_LANES(12);
    if (rate_lanes == 20) {
        FDECK_LANES(16);
    }
#    undef FDECK_LANES
}

#endif

FDECK_INLINE void
FDECK_FN(store_state)(uint64_t *states, const FDECK_V a[25])
{
#define FDECK_LANES(lane)                                              \
    do {                                                               \
        FDECK_STORE_LANES(states + FDECK_N * lane, a[lane]);           \
        FDECK_STORE_LANES(states + FDECK_N * (lane + 1), a[lane + 1]); \
        FDECK_STORE_LANES(states + FDECK_N * (lane + 2), a[lane + 2]); \
        FDECK_STORE_LANES(states + FDECK_N * (lane + 3), a[lane + 3]); \
        FDECK_STORE_LANES(states + FDECK_N * (lane + 4), a[lane + 4]); \
    } while (0)
    FDECK_LANES(0);
    FDECK_LANES(5);
    FDECK_LANES(10);
    FDECK_LANES(15);
    FDECK_LANES(20);
#undef FDECK_LANES
}

/*
 * Each state `i` absorbs `nblocks` full blocks found one after the other at `in[i]`, none of them the last of a string.
 *
 * If `last` is set, each state then absorbs the block at `last[i]`, followed by the lane `last_trailers[i]`.
 * That block is always full, since the caller pads it.
 * The first `cv_lanes` lanes of each state then go to `out[FDECK_N * lane + state]`.
 * With a single state and `whole_state` set, the whole state goes to `out` instead, and the capacity isn't written back.
 *
 * Keep this as a single function.
 * A second copy of the rounds just for the last block didn't fit in the CPU's cache of decoded instructions along with this one, and short hashes got 250 cycles slower.
 */
FDECK_INLINE void
FDECK_FN(absorb)(uint64_t *caps, const uint8_t *const in[FDECK_N], size_t nblocks,
                 const uint8_t *const last[FDECK_N], const uint64_t *last_trailers, uint64_t *out,
                 int whole_state, unsigned rate_lanes, unsigned cv_lanes, unsigned rounds,
                 uint8_t pad)
{
    const size_t   rate  = 8 * (size_t) rate_lanes;
    const size_t   total = nblocks + (last != NULL);
    const uint8_t *blocks[FDECK_N];
    FDECK_V        trailer = FDECK_SET1(fdeck_trailer_lane(FLIGHTDECK_TRAILER_EMPTY, 1, pad));
    FDECK_V        a[25];

    memcpy(blocks, in, sizeof blocks);
    FDECK_FN(load_capacity)(a, caps, rate_lanes);
    for (size_t i = 0; i < total; i++) {
        if (i == nblocks) {
            memcpy(blocks, last, sizeof blocks);
            trailer = FDECK_LOAD_LANES(last_trailers);
        }
        FDECK_FN(load_block)(a, blocks, rate_lanes);
        a[rate_lanes] = FDECK_XOR(a[rate_lanes], trailer);
        FDECK_FN(permute)(a, rounds);
        blocks[0] += rate;
#if FDECK_N > 1
        blocks[1] += rate;
#endif
#if FDECK_N > 2
        blocks[2] += rate;
        blocks[3] += rate;
#endif
#if FDECK_N > 4
        blocks[4] += rate;
        blocks[5] += rate;
        blocks[6] += rate;
        blocks[7] += rate;
#endif

        /* Only read these lanes here, so that the compiler only computes them for the last block. */
        if (i == nblocks) {
            if (FDECK_N == 1 && whole_state) {
                FDECK_FN(store_state)(out, a);
                return;
            }
            FDECK_STORE_LANES(out, a[0]);
            FDECK_STORE_LANES(out + FDECK_N, a[1]);
            FDECK_STORE_LANES(out + FDECK_N * 2, a[2]);
            FDECK_STORE_LANES(out + FDECK_N * 3, a[3]);
            if (cv_lanes == 8) {
                FDECK_STORE_LANES(out + FDECK_N * 4, a[4]);
                FDECK_STORE_LANES(out + FDECK_N * 5, a[5]);
                FDECK_STORE_LANES(out + FDECK_N * 6, a[6]);
                FDECK_STORE_LANES(out + FDECK_N * 7, a[7]);
            }
        }
    }
    FDECK_FN(store_capacity)(caps, a, rate_lanes);
}

#if FDECK_NINTH_STATE

/*
 * One round on eight states in vectors and a ninth one on plain 64-bit words.
 *
 * The vector code leaves the CPU's integer units idle, so the ninth state is nearly free.
 * The rows of the two are interleaved, otherwise the CPU doesn't run them side by side.
 */
FDECK_INLINE void
FDECK_FN(round9)(FDECK_V a[25], uint64_t ninth[25], uint64_t round_constant)
{
    const FDECK_V  c0       = FDECK_PARITY(a, 0);
    const FDECK_V  c1       = FDECK_PARITY(a, 1);
    const FDECK_V  c2       = FDECK_PARITY(a, 2);
    const FDECK_V  c3       = FDECK_PARITY(a, 3);
    const FDECK_V  c4       = FDECK_PARITY(a, 4);
    const uint64_t ninth_c0 = ninth[0] ^ ninth[5] ^ ninth[10] ^ ninth[15] ^ ninth[20];
    const uint64_t ninth_c1 = ninth[1] ^ ninth[6] ^ ninth[11] ^ ninth[16] ^ ninth[21];
    const uint64_t ninth_c2 = ninth[2] ^ ninth[7] ^ ninth[12] ^ ninth[17] ^ ninth[22];
    const uint64_t ninth_c3 = ninth[3] ^ ninth[8] ^ ninth[13] ^ ninth[18] ^ ninth[23];
    const uint64_t ninth_c4 = ninth[4] ^ ninth[9] ^ ninth[14] ^ ninth[19] ^ ninth[24];
    FDECK_V        d[5];
    FDECK_V        e[25];
    uint64_t       ninth_d[5];
    uint64_t       ninth_e[25];

    FDECK_THETA(d, 0, c4, c1);
    FDECK_THETA(d, 2, c1, c3);
    FDECK_THETA(d, 4, c3, c0);
    FDECK_THETA(d, 1, c0, c2);
    FDECK_THETA(d, 3, c2, c4);
    ninth_d[0] = ninth_c4 ^ fdeck_rotl(ninth_c1, 1);
    ninth_d[1] = ninth_c0 ^ fdeck_rotl(ninth_c2, 1);
    ninth_d[2] = ninth_c1 ^ fdeck_rotl(ninth_c3, 1);
    ninth_d[3] = ninth_c2 ^ fdeck_rotl(ninth_c4, 1);
    ninth_d[4] = ninth_c3 ^ fdeck_rotl(ninth_c0, 1);

    a[0]  = FDECK_XOR(a[0], d[0]);
    a[5]  = FDECK_XOR(a[5], d[0]);
    a[10] = FDECK_XOR(a[10], d[0]);
    a[15] = FDECK_XOR(a[15], d[0]);
    a[20] = FDECK_XOR(a[20], d[0]);
    ninth[0] ^= ninth_d[0];
    ninth[5] ^= ninth_d[0];
    ninth[10] ^= ninth_d[0];
    ninth[15] ^= ninth_d[0];
    ninth[20] ^= ninth_d[0];

#    define FDECK_ROW(y, ...)                                                                  \
        do {                                                                                   \
            FDECK_ROW_OF(FDECK_V, FDECK_XOR, FDECK_ROL, FDECK_CHI, e, a, d, y, __VA_ARGS__);   \
            FDECK_ROW_OF(uint64_t, FDECK_WORD_XOR, fdeck_rotl, FDECK_WORD_CHI, ninth_e, ninth, \
                         ninth_d, y, __VA_ARGS__);                                             \
        } while (0)
    FDECK_EACH_ROW(FDECK_ROW);
#    undef FDECK_ROW

    e[0] = FDECK_XOR(e[0], FDECK_SET1(round_constant));
    ninth_e[0] ^= round_constant;
    memcpy(a, e, sizeof e);
    memcpy(ninth, ninth_e, sizeof ninth_e);
}

/*
 * absorb() for nine states.
 * Their lanes are at `caps[9 * lane + state]` and go to `out[9 * lane + state]`.
 */
FDECK_INLINE void
FDECK_FN(absorb9)(uint64_t *caps, const uint8_t *const in[9], size_t nblocks,
                  const uint8_t *const last[9], const uint64_t *last_trailers, uint64_t *out,
                  unsigned rate_lanes, unsigned cv_lanes, unsigned rounds, uint8_t pad)
{
    const size_t   rate  = 8 * (size_t) rate_lanes;
    const size_t   total = nblocks + (last != NULL);
    const uint8_t *blocks[9];
    FDECK_V        trailer       = FDECK_SET1(fdeck_trailer_lane(FLIGHTDECK_TRAILER_EMPTY, 1, pad));
    uint64_t       ninth_trailer = fdeck_trailer_lane(FLIGHTDECK_TRAILER_EMPTY, 1, pad);
    FDECK_V        a[25];
    uint64_t       ninth[25];

    memcpy(blocks, in, sizeof blocks);
#    define FDECK_DO(lane)                                                  \
        do {                                                                \
            a[lane]     = FDECK_LOAD_LANES(caps + 9 * (lane - rate_lanes)); \
            ninth[lane] = caps[9 * (lane - rate_lanes) + 8];                \
        } while (0)
    FDECK_CAPACITY_LANES(rate_lanes, FDECK_DO);
#    undef FDECK_DO
    for (size_t i = 0; i < total; i++) {
        if (i == nblocks) {
            memcpy(blocks, last, sizeof blocks);
            trailer       = FDECK_LOAD_LANES(last_trailers);
            ninth_trailer = last_trailers[8];
        }
        FDECK_FN(load_block)(a, blocks, rate_lanes);
        fdeck_load_block_c(ninth, blocks + 8, rate_lanes);
        a[rate_lanes] = FDECK_XOR(a[rate_lanes], trailer);
        ninth[rate_lanes] ^= ninth_trailer;

        for (unsigned first = 24 - rounds; first < 22; first += 2) {
            FDECK_FN(round9)(a, ninth, fdeck_round_constants[first]);
            FDECK_FN(round9)(a, ninth, fdeck_round_constants[first + 1]);
        }
        FDECK_FN(round9)(a, ninth, fdeck_round_constants[22]);
        FDECK_FN(round9)(a, ninth, fdeck_round_constants[23]);

        blocks[0] += rate;
        blocks[1] += rate;
        blocks[2] += rate;
        blocks[3] += rate;
        blocks[4] += rate;
        blocks[5] += rate;
        blocks[6] += rate;
        blocks[7] += rate;
        blocks[8] += rate;

        if (i == nblocks) {
#    define FDECK_DO(lane)                              \
        do {                                            \
            FDECK_STORE_LANES(out + 9 * lane, a[lane]); \
            out[9 * lane + 8] = ninth[lane];            \
        } while (0)
            FDECK_DO(0);
            FDECK_DO(1);
            FDECK_DO(2);
            FDECK_DO(3);
            if (cv_lanes == 8) {
                FDECK_DO(4);
                FDECK_DO(5);
                FDECK_DO(6);
                FDECK_DO(7);
            }
#    undef FDECK_DO
        }
    }
#    define FDECK_DO(lane)                                              \
        do {                                                            \
            FDECK_STORE_LANES(caps + 9 * (lane - rate_lanes), a[lane]); \
            caps[9 * (lane - rate_lanes) + 8] = ninth[lane];            \
        } while (0)
    FDECK_CAPACITY_LANES(rate_lanes, FDECK_DO);
#    undef FDECK_DO
}

#    define FDECK_ABSORB9_FUNCTION(name, rate_lanes, cv_lanes, rounds, pad)                       \
        FDECK_TARGET static void FDECK_FN(name##_absorb9)(                                        \
            uint64_t *caps, const uint8_t *const *in, size_t nblocks, const uint8_t *const *last, \
            const uint64_t *last_trailers, uint64_t *out, int whole_state)                        \
        {                                                                                         \
            (void) whole_state;                                                                   \
            FDECK_FN(absorb9)(caps, in, nblocks, last, last_trailers, out, rate_lanes, cv_lanes,  \
                              rounds, pad);                                                       \
        }
#else
#    define FDECK_ABSORB9_FUNCTION(name, rate_lanes, cv_lanes, rounds, pad)
#endif

#if FDECK_N > 1

/*
 * The output blocks `index` to `index + FDECK_N - 1` of a deck, given the capacity of its main duplex.
 * They are written one after the other, XORed with `in` if there is one.
 */
FDECK_INLINE void
FDECK_FN(output)(uint8_t *out, const uint8_t *in, const uint64_t *cap, uint64_t index,
                 unsigned rate_lanes, unsigned rounds, uint8_t pad)
{
    const FDECK_V zero = FDECK_SET1(0);
    FDECK_V       a[25];

    a[0] = FDECK_COUNTERS(index);
#    define FDECK_ZERO(lane)    \
        do {                    \
            a[lane]     = zero; \
            a[lane + 1] = zero; \
            a[lane + 2] = zero; \
        } while (0)
    FDECK_ZERO(1);
    FDECK_ZERO(4);
    FDECK_ZERO(7);
    FDECK_ZERO(10);
    FDECK_ZERO(13);
    if (rate_lanes == 20) {
        a[16] = zero;
        FDECK_ZERO(17);
    }
#    undef FDECK_ZERO
#    define FDECK_DO(lane) a[lane] = FDECK_SET1(cap[lane - rate_lanes])
    FDECK_CAPACITY_LANES(rate_lanes, FDECK_DO);
#    undef FDECK_DO
    a[rate_lanes] =
        FDECK_XOR(a[rate_lanes], FDECK_SET1(fdeck_trailer_lane(FDECK_TRAILER_COUNTER, 1, pad)));
    FDECK_FN(permute)(a, rounds);
    FDECK_FN(store_block)(out, in, a, rate_lanes);
}

#    undef FDECK_COUNTERS

#    define FDECK_OUTPUT_FUNCTION(name, rate_lanes, rounds, pad)                              \
        FDECK_TARGET static void FDECK_FN(name##_output)(uint8_t *out, const uint8_t *in,     \
                                                         const uint64_t *cap, uint64_t index) \
        {                                                                                     \
            FDECK_FN(output)(out, in, cap, index, rate_lanes, rounds, pad);                   \
        }

#else
#    define FDECK_OUTPUT_FUNCTION(name, rate_lanes, rounds, pad)
#endif

/* The functions of each instance: the ones above, with its parameters as constants. */
#define FDECK_FUNCTIONS(variant, name, rate_lanes, cv_lanes, blocks_per_chunk, rounds, pad)    \
    FDECK_TARGET static void FDECK_FN(name##_absorb)(                                          \
        uint64_t *caps, const uint8_t *const *in, size_t nblocks, const uint8_t *const *last,  \
        const uint64_t *last_trailers, uint64_t *out, int whole_state)                         \
    {                                                                                          \
        FDECK_FN(absorb)(caps, in, nblocks, last, last_trailers, out, whole_state, rate_lanes, \
                         cv_lanes, rounds, pad);                                               \
    }                                                                                          \
    FDECK_OUTPUT_FUNCTION(name, rate_lanes, rounds, pad)                                       \
    FDECK_ABSORB9_FUNCTION(name, rate_lanes, cv_lanes, rounds, pad)

FDECK_INSTANCES(FDECK_FUNCTIONS)

#undef FDECK_FUNCTIONS
#undef FDECK_ABSORB9_FUNCTION
#undef FDECK_OUTPUT_FUNCTION
#undef FDECK_CAPACITY_LANES
#undef FDECK_EACH_ROW
#undef FDECK_ROW_OF
#undef FDECK_THETA
#undef FDECK_PARITY
#undef FDECK_INLINE

#undef FDECK_N
#undef FDECK_V
#undef FDECK_FN
#undef FDECK_TARGET
#undef FDECK_KEEP
#undef FDECK_XOR
#undef FDECK_XOR3
#undef FDECK_CHI
#undef FDECK_ROL
#undef FDECK_SET1
#undef FDECK_LOAD_LANES
#undef FDECK_STORE_LANES
#undef FDECK_LOAD_BYTES
#undef FDECK_NINTH_STATE
