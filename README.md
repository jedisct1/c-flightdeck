# FlightDeck (standalone C implementation)

This is a C library for FlightDeck and Deck-BO.

You can use it to hash data, to authenticate it, and to encrypt it.

FlightDeck is a recent design by Joan Daemen, Silvia Mella and Gilles Van Assche, from the team behind SHA-3.
It's built on the same permutation as SHA-3, and it's made to process long messages in parallel, which modern CPUs are really good at.

The library has no dependencies.
It's fast on x86-64 CPUs with AVX-512 or AVX2, on 64-bit ARM CPUs, and on WebAssembly with SIMD.
It runs pretty much anywhere else as plain C.

It's a port of the original [Zig implementation](https://github.com/jedisct1/zig-flightdeck), like the [Rust version](https://github.com/jedisct1/rust-flightdeck), and all three give the same results, byte for byte.

## Which part do I need?

- To encrypt messages, use `flightdeck_bo`.
  It encrypts them, and gives you a tag that proves nobody tampered with them.
- To hash something, `flightdeck_hash()` does it in one call.
- For MACs, key derivation or your own protocol, use the deck cipher, `flightdeck`.
  It mixes a key with whatever you feed it, and gives you back as many random-looking bytes as you ask for.
- `flightdeck_od`, the overwrite duplex, is the low-level building block the others are made of.
  You probably won't need it.

Each of them comes in four flavors, which you pick with an argument:

| Instance                   | Security level | Rounds |
| -------------------------- | -------------- | -----: |
| `FLIGHTDECK_TURBOSHAKE128` | 128 bits       |     12 |
| `FLIGHTDECK_TURBOSHAKE256` | 256 bits       |     12 |
| `FLIGHTDECK_SHAKE128`      | 128 bits       |     24 |
| `FLIGHTDECK_SHAKE256`      | 256 bits       |     24 |

The TurboSHAKE ones do half the rounds, so they're about twice as fast.
If you don't have a reason to pick something else, go with `FLIGHTDECK_TURBOSHAKE128`.
Pick a 256-bit one if you need a higher security level, and a SHAKE one if you'd rather have the full 24 rounds.

## Building it

### With make, or any C compiler

The whole library is three files in `src/`: `flightdeck.h`, `flightdeck.c` and `flightdeck_backend.inc`.
You can copy them into your project and compile `flightdeck.c` with any C11 compiler, no special flags needed:

```sh
cc -O3 -c src/flightdeck.c
```

Or just run `make` to get `libflightdeck.a`.

### With Zig

```sh
zig build -Doptimize=ReleaseFast
```

That puts `libflightdeck.a` in `zig-out/lib` and `flightdeck.h` in `zig-out/include`.
Add `-Dlinkage=dynamic` if you want a shared library, and `-Dtarget=...` to cross-compile.

It builds for a baseline CPU unless you pass `-Dcpu=...`, so the library runs on any machine of that architecture.
That doesn't make it slower: the AVX2 and AVX-512 code is there anyway, and gets picked when the program runs.
The same goes for the code that uses the SHA-3 instructions of ARM CPUs, on the systems listed below.

To use it in a Zig project, add it to your `build.zig.zon`:

```sh
zig fetch --save git+https://github.com/jedisct1/c-flightdeck
```

Then link it in your `build.zig`.
This also makes `flightdeck.h` available to your C code:

```zig
const flightdeck = b.dependency("c_flightdeck", .{ .target = target, .optimize = optimize });
exe.root_module.linkLibrary(flightdeck.artifact("flightdeck"));
```

### What runs where

On x86-64 with GCC or Clang, the library comes with code for AVX-512 and AVX2, and uses the best one your CPU and operating system support.
That's decided when your program runs, so the same binary works on any x86-64 CPU.
`flightdeck_implementation()` tells you which one you got: `"avx512"`, `"avx2"`, `"neon-sha3"`, `"neon"`, `"simd128"` or `"portable"`.

You don't need `-march=native`.
It doesn't help, and with GCC and Clang it actually made things a little slower in our tests.

On 64-bit ARM (little-endian AArch64) with GCC or Clang, the library works on three chunks of a long input at once: two in NEON registers, and one in regular registers.

When the CPU has the SHA-3 instructions, the library uses them too, and you get `"neon-sha3"` instead of `"neon"`.
That happens whenever your compiler targets such a CPU, like Apple's Clang and Zig do on Macs.
Otherwise, the library asks the CPU when your program runs, on Linux, Android, FreeBSD and Apple systems.
That needs GCC 14 or Clang 16, and doesn't work in freestanding builds.
`-DFLIGHTDECK_NO_SHA3` turns the SHA-3 code off.

On small in-order cores like the Cortex-A53 and A55, NEON probably doesn't help, and `-DFLIGHTDECK_PORTABLE` may be faster on long inputs.
We haven't measured that, though: all of our ARM numbers come from Apple's CPUs.

WebAssembly can't check for SIMD at run time, so you choose when you build.
Compile with `-msimd128` to get the SIMD code, which wasmtime, wasmer and current browsers all support.
`zig build` turns it on for you when you target WebAssembly, unless you pick a CPU yourself.

There's one catch: with wasmer's LLVM backend, the plain C code is faster on long inputs.
If that's what you use, build with `-DFLIGHTDECK_PORTABLE`, or `zig build -Dportable`.

Everywhere else, you get the plain C code.

## Hashing

```c
#include "flightdeck.h"

uint8_t digest[32];

flightdeck_hash(FLIGHTDECK_TURBOSHAKE128, digest, sizeof digest, message, message_len);
```

The digest can be as long as you want.
Ask for at least 32 bytes with the 128-bit instances, and 64 bytes with the 256-bit ones.
Finding two messages with the same digest is then as hard as it is with SHAKE or TurboSHAKE.

Need to hash several fields together?
Use the deck cipher with an empty key, since that's all the hash function is:

```c
flightdeck hasher;

flightdeck_init(&hasher, FLIGHTDECK_TURBOSHAKE128, NULL, 0);
flightdeck_duplex(&hasher, NULL, 0, name, name_len, FLIGHTDECK_TRAILER_EMPTY);
flightdeck_duplex(&hasher, digest, sizeof digest, value, value_len, FLIGHTDECK_TRAILER_EMPTY);
```

Each call adds one string, and the digest covers all of them, in order.
There's no way to mix up `"ab"` then `"c"` with `"a"` then `"bc"`, so you don't need length prefixes.

Before you use it as a hash function, keep in mind that:

- It's not a standard hash function.
  The digests don't match TurboSHAKE, KangarooTwelve or anything else, so if other software has to compute them, use one of those instead.
- The FlightDeck paper mentions hashing without a key as future work.
  If that gets specified someday, the digests may not match the ones you get here.
- Only collision resistance is backed by a proof.

## Authenticated encryption

Deck-BO works on sessions: every message is authenticated along with all the messages that came before it.

```c
flightdeck_bo sender, receiver;
uint8_t tag[32];

flightdeck_bo_init(&sender, FLIGHTDECK_TURBOSHAKE128, sizeof tag, key, 16);
flightdeck_bo_init(&receiver, FLIGHTDECK_TURBOSHAKE128, sizeof tag, key, 16);

flightdeck_bo_encrypt(&sender, ciphertext, tag, message, message_len, ad, ad_len);

if (flightdeck_bo_decrypt(&receiver, decrypted, ciphertext, message_len, tag, ad, ad_len) != 0) {
    /* Forged or damaged: `decrypted` was zeroed, and the session didn't move. */
}
```

The key should be `flightdeck_key_len()` random bytes, which is 16 for the 128-bit instances and 32 for the 256-bit ones.

There's no nonce.
So if two sessions with the same key go through the same messages, and then both encrypt the same message with the same associated data, they get the same ciphertext.
If that matters to you, put a nonce or a counter in the associated data.

You choose the tag length when the session starts.
16 bytes is plenty to stop forgeries.
A tag twice as long as the security level, so 32 bytes with the 128-bit instances, also commits to the key, the associated data and the message.

You can encrypt and decrypt in place: the output can be the same buffer as the input.

### Sessions

A session is just a structure.
`flightdeck_bo_init()` starts it, and everything you encrypt, decrypt or absorb moves it forward.
You keep it around for as long as the conversation lasts, one on each side.

Encrypting a message and decrypting it leave the session in the same state.
So a single session can carry both directions, as long as the two sides take turns:

```c
flightdeck_bo alice, bob;
uint8_t buffer[4];
uint8_t tag[16];

flightdeck_bo_init(&alice, FLIGHTDECK_TURBOSHAKE128, sizeof tag, key, 16);
flightdeck_bo_init(&bob, FLIGHTDECK_TURBOSHAKE128, sizeof tag, key, 16);

flightdeck_bo_encrypt(&alice, buffer, tag, (const uint8_t *) "ping", 4, NULL, 0);
flightdeck_bo_decrypt(&bob, buffer, buffer, sizeof buffer, tag, NULL, 0);

flightdeck_bo_encrypt(&bob, buffer, tag, (const uint8_t *) "pong", 4, NULL, 0);
flightdeck_bo_decrypt(&alice, buffer, buffer, sizeof buffer, tag, NULL, 0);
```

In real code, check what `flightdeck_bo_decrypt()` returns, of course.

If both sides can send whenever they want, their messages can cross, and the two sessions stop matching.
In that case, use two sessions, one for each direction, each with its own key.
There's an example below.

`flightdeck_bo_absorb()` mixes something into a session without sending anything: a session ID, a nonce, or the transcript of a handshake, for example.
Both sides have to absorb the same thing at the same point.

`flightdeck_bo_squeeze()` gives you bytes that depend on the key and on everything that happened in the session so far.
Both sides get the same bytes, so that's how you get new keys out of a session:

```c
uint8_t alice_key[32];
uint8_t bob_key[32];

flightdeck_bo_absorb(&alice, (const uint8_t *) "session 1234", 12);
flightdeck_bo_absorb(&bob, (const uint8_t *) "session 1234", 12);

flightdeck_bo_squeeze(&alice, alice_key, sizeof alice_key);
flightdeck_bo_squeeze(&bob, bob_key, sizeof bob_key);
/* alice_key and bob_key are the same. */
```

You can encrypt, decrypt, absorb and squeeze in any order you like.
The only rule is that both sides go through the same messages and the same absorbs, in the same order.

Squeezing doesn't count, because it doesn't move the session forward: what comes next is the same whether you squeezed or not.
If you squeeze again, you get the bytes that follow, not the same ones, until the next message or absorb.
There has to be something in the session first, though: squeezing a brand new session stops the program.

An absorb is really just an empty message whose tag gets thrown away.
If you'd rather have the other side check what it absorbed, send that tag instead:

```c
flightdeck_bo_encrypt(&alice, NULL, tag, NULL, 0, (const uint8_t *) "session 1234", 12);

if (flightdeck_bo_decrypt(&bob, NULL, NULL, 0, tag, (const uint8_t *) "session 1234", 12) != 0) {
    /* The two sides don't agree on the session. */
}
```

What you squeeze comes after the tag of the last message, and never includes it.
So tags can travel in the clear while the bytes you squeeze stay secret.

A few more things worth knowing:

- A message that doesn't authenticate doesn't move the session.
  So a forged or damaged message does no harm: the real one still decrypts after it.
- A message that gets lost or arrives out of order is another story: nothing sent after it decrypts until it shows up.
  So sessions need a transport that delivers everything, in order.
- Copying the structure copies the session, if you want to save it or try something on the side.
- Messages and absorbs are Deck-BO exactly as the paper specifies it.
  Squeezing is an addition, so don't expect it from other implementations.

Back to the case where both sides send whenever they want.
Squeeze two keys from a session that both sides share, and start one session for each direction.
Here's the client side, and the server does the same thing with the two keys swapped:

```c
uint8_t keys[32];
flightdeck_bo sending, receiving;

flightdeck_bo_squeeze(&handshake, keys, sizeof keys);

flightdeck_bo_init(&sending, FLIGHTDECK_TURBOSHAKE128, 16, keys, 16);        /* client to server */
flightdeck_bo_init(&receiving, FLIGHTDECK_TURBOSHAKE128, 16, keys + 16, 16); /* server to client */
```

## The deck cipher

A deck cipher takes strings of any length, and after each one, it gives you as many bytes as you want.
Those bytes depend on the key and on everything it was given so far.

```c
flightdeck deck;
uint8_t mac[32];
uint8_t more[1000];

flightdeck_init(&deck, FLIGHTDECK_TURBOSHAKE128, key, 16);

/* Absorb a string, and get 32 bytes that depend on the key and on that string. */
flightdeck_duplex(&deck, mac, sizeof mac, message, message_len, FLIGHTDECK_TRAILER_EMPTY);

/* Get more bytes from the same call. */
flightdeck_squeeze(&deck, more, sizeof more);

/* Absorb another string. The output now depends on both strings. */
flightdeck_duplex(&deck, NULL, 0, nonce, nonce_len, FLIGHTDECK_TRAILER_EMPTY);

/* The output can also be XORed into a buffer, to use it as a keystream. */
flightdeck_xor(&deck, ciphertext, plaintext, plaintext_len);
```

The last argument of `flightdeck_duplex()` is a trailer: a few bits you can add to a string, so that different kinds of input can never be confused.
It's a number made of those bits, least significant first, followed by a final 1.
So `FLIGHTDECK_TRAILER_EMPTY`, which is 1, means no bits at all, 2 is the single bit 0, 3 is the single bit 1, and so on, up to 31.

Need a clone? Just copy the structure.
There's also `flightdeck_clone_compact()`, which gives you a clone that can only absorb the next string: whatever was left of the current output doesn't come along.

Keys are normally `flightdeck_key_len()` random bytes, but anything up to `flightdeck_key_len_max()` bytes is accepted.
That's 160 bytes for the 128-bit instances and 128 bytes for the 256-bit ones.

## The fine print

The library calls `abort()` when it's used in a way the header rules out: a key that's too long, a trailer of 0, squeezing before anything was absorbed, and so on.
Those are bugs in the calling code, not errors to recover from.

Also:

- Pointers aren't checked, so every buffer has to be as long as its length says.
  A buffer can be NULL when its length is 0.
- Buffers must not overlap, except where the header says two of them can be the same, like when you encrypt in place.
- Threads can use the library freely, as long as two of them don't use the same structure at the same time.
- There's no streaming interface: each message or string has to be in memory, in one piece, when you pass it.

## Speed

TurboSHAKE128, in MiB/s, on one core of a Ryzen 7 7700 (Zen 4), built with GCC 15:

| MiB/s   | Backend | 64 B | 1 KiB | 8 KiB | 64 KiB | 1 MiB |
| ------- | ------- | ---: | ----: | ----: | -----: | ----: |
| Hashing | AVX-512 |  505 |  1416 |  2343 |   6146 |  6431 |
|         | AVX2    |  424 |  1132 |  1503 |   3041 |  3336 |
|         | Plain C |  370 |   962 |  1032 |   1061 |  1063 |
| Deck-BO | AVX-512 |  235 |   926 |  1622 |   2972 |  3059 |
|         | AVX2    |  195 |   693 |  1002 |   1568 |  1647 |
|         | Plain C |  169 |   434 |   471 |    482 |   481 |

The AVX2 rows come from the same CPU with its AVX-512 code turned off.
A CPU that only has AVX2, an Intel one in particular, will give different numbers.

With AVX-512, Clang builds are about 8% faster on 64-byte messages, and up to 16% slower on long ones.

For comparison, on the same machine, XKCP, the reference implementation, hashes at about 175 MiB/s with 64-byte messages, 880 MiB/s with 8 KiB and 5100 MiB/s with 1 MiB.

On one performance core of an Apple M5 Max, built with Apple's Clang 21:

| MiB/s   | Backend      | 64 B | 1 KiB | 8 KiB | 64 KiB | 1 MiB |
| ------- | ------------ | ---: | ----: | ----: | -----: | ----: |
| Hashing | NEON + SHA-3 | 1056 |  2481 |  3925 |   5596 |  6005 |
|         | NEON         |  795 |  2085 |  2357 |   3539 |  3660 |
|         | Plain C      |  808 |  2124 |  2346 |   2440 |  2424 |
| Deck-BO | NEON + SHA-3 |  438 |  1499 |  2223 |   2753 |  2858 |
|         | NEON         |  363 |  1197 |  1398 |   1779 |  1817 |
|         | Plain C      |  385 |   967 |  1047 |   1088 |  1096 |

The NEON rows come from the same CPU, built with `-mcpu=generic -DFLIGHTDECK_NO_SHA3` so that the compiler can't sneak in SHA-3 instructions.

ARM CPUs without those instructions are usually much smaller than this one, so expect different numbers there.

To get the numbers for your own machine, run `make bench` or `zig build bench`.
They cover all four instances.

## Running the tests

```sh
make test
make test-wasm
zig build test
```

`make test` runs the tests three times: with the best code for your CPU, with the second best, and with plain C only.
The second best is AVX2 instead of AVX-512 on x86-64, and NEON without SHA-3 on 64-bit ARM.

On a Mac with Apple silicon, the compiler may use SHA-3 instructions even in the NEON code.
To test without them, and to test how the library detects them at run time, use `make clean && make test CFLAGS="-O3 -mcpu=generic"`.

`make test-wasm` runs them on WebAssembly with SIMD.
It needs `zig` and `wasmer`, and you can swap those for other tools with `WASM_CC` and `WASM_RUN`.

With Zig, `zig build test -Dtarget=wasm32-wasi -fwasmtime` does the same on WebAssembly, with wasmtime.

The tests check the output against test vectors from XKCP, and against the Zig implementation.

## Papers

FlightDeck comes from ["Parallelizable modes for duplex-based authenticated encryption"](https://eprint.iacr.org/2026/2801) by Joan Daemen, Silvia Mella and Gilles Van Assche.

The overwrite duplex and Deck-BO come from ["Shaking up authenticated encryption"](https://eprint.iacr.org/2024/1618) by Joan Daemen, Seth Hoffert, Silvia Mella, Gilles Van Assche and Ronny Van Keer.

## License

MIT, see [LICENSE](LICENSE).
