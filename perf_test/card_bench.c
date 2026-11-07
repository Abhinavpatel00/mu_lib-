/*
 * card_bench.c
 * ============================================================
 * Benchmark: 64-bit single-word compare  vs  two 32-bit compares
 * on a tightly-packed Card struct.
 *
 * Card layout (8 bytes = one 64-bit machine word):
 *   [31:0]  next      uint32_t   – linked-list index / pointer
 *   [39:32] info      uint8_t    – packed suit(2) | rank(4) | tag(2)
 *   [63:40] title     char[3]    – 3-char short name (+ info = 4B upper word)
 *
 * Two 32-bit view:
 *   lo32 = next
 *   hi32 = info | title[0]<<8 | title[1]<<16 | title[2]<<24
 *
 * Compile (done by Makefile):
 *   gcc -O2 -march=native -std=c11 card_bench.c -o card_bench
 * ============================================================
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ── Card: exactly 8 bytes, no padding ────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t next;          /* 4 B */
    uint8_t  info;          /* 1 B  – suit:2 | rank:4 | tag:2 */
    char     title[3];      /* 3 B  – e.g. "AH\0" */
} Card;                     /* total = 8 B */

_Static_assert(sizeof(Card) == 8, "Card must be 8 bytes");

/* ── Helpers ────────────────────────────────────────────────── */
static inline uint64_t card_as_u64(const Card *c)
{
    uint64_t v;
    memcpy(&v, c, 8);   /* safe strict-aliasing copy */
    return v;
}

/* ── Method 1: single 64-bit equality ───────────────────────── */
__attribute__((noinline))
bool eq_64(const Card *a, const Card *b)
{
    return card_as_u64(a) == card_as_u64(b);
}

/* ── Method 2: two 32-bit equalities ────────────────────────── */
__attribute__((noinline))
bool eq_2x32(const Card *a, const Card *b)
{
    uint32_t alo, ahi, blo, bhi;
    memcpy(&alo, (const char *)a,     4);
    memcpy(&ahi, (const char *)a + 4, 4);
    memcpy(&blo, (const char *)b,     4);
    memcpy(&bhi, (const char *)b + 4, 4);
    return (alo == blo) & (ahi == bhi);   /* bitwise & avoids branch */
}

/* ── Method 3: field-by-field (naïve, what a beginner writes) ── */
__attribute__((noinline))
bool eq_fields(const Card *a, const Card *b)
{
    return a->next == b->next
        && a->info == b->info
        && a->title[0] == b->title[0]
        && a->title[1] == b->title[1]
        && a->title[2] == b->title[2];
}

/* ── Method 4: memcmp (what the std lib does) ───────────────── */
__attribute__((noinline))
bool eq_memcmp(const Card *a, const Card *b)
{
    return memcmp(a, b, sizeof(Card)) == 0;
}

/* ── Timing ──────────────────────────────────────────────────── */
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── Dataset ─────────────────────────────────────────────────── */
#define N        (1 << 24)   /* 16 M cards */
#define REPS     5

static Card *deck;

static void build_deck(void)
{
    deck = aligned_alloc(64, N * sizeof(Card));
    if (!deck) { perror("alloc"); exit(1); }

    /* fill with reproducible pseudo-random data */
    uint64_t rng = 0xdeadbeefcafebabe;
    for (int i = 0; i < N; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        memcpy(&deck[i], &rng, 8);
    }
    /* force ~50 % match rate: every even card == odd card */
    for (int i = 0; i < N - 1; i += 2)
        deck[i] = deck[i + 1];
}

/* ── Run one method N-1 comparisons, return time + checksum ─── */
#define BENCH(name, fn)                                             \
do {                                                                \
    double best = 1e9;                                              \
    uint64_t chk = 0;                                              \
    for (int r = 0; r < REPS; ++r) {                               \
        double t0 = now_sec();                                      \
        for (int i = 0; i < N - 1; ++i)                            \
            chk += fn(&deck[i], &deck[i + 1]);                     \
        double dt = now_sec() - t0;                                 \
        if (dt < best) best = dt;                                   \
    }                                                               \
    printf("%-20s  best=%.4f s   %6.0f Mcmp/s   chk=%llu\n",       \
           name, best, (N-1)/best/1e6, (unsigned long long)chk);   \
} while(0)

int main(void)
{
    printf("sizeof(Card)=%zu  (should be 8)\n\n", sizeof(Card));
    build_deck();

    printf("%-20s  %-16s  %-16s  %s\n",
           "Method", "Best time", "Throughput", "Checksum");
    printf("--------------------------------------------------------------\n");

    BENCH("eq_64  (1×64)",   eq_64);
    BENCH("eq_2x32 (2×32)",  eq_2x32);
    BENCH("eq_fields (naïve)",eq_fields);
    BENCH("eq_memcmp",        eq_memcmp);

    free(deck);
    return 0;
}
