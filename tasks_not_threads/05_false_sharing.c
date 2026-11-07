/*
 * 05_false_sharing.c — TIP 7: Cache-line alignment to prevent false sharing
 * ===========================================================================
 * FALSE SHARING: two threads write to DIFFERENT variables that happen to live
 * in the SAME 64-byte cache line. Every write by thread A invalidates the
 * cache line in thread B's L1 cache, forcing a reload — even though they
 * never touch each other's data.
 *
 * PROOF via benchmark:
 *   SLOW: array of counters packed together → all in the same cache lines
 *   FAST: each counter padded to its own 64-byte cache line
 *
 * Expected: FAST is 5-10× faster on a real multi-core machine.
 *
 * Herb Sutter (Part 1): "False sharing is as bad as true sharing from a
 * performance perspective. The hardware doesn't know you're accessing
 * different variables — it only knows about cache lines."
 *
 * Compile:  gcc -O2 -std=c11 -pthread 05_false_sharing.c -o 05_false_sharing
 * ===========================================================================
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <string.h>

#define CACHE_LINE   64
#define N_THREADS    4
#define ITERS        50000000LL      /* 50 M increments per thread */

/* ── BAD: packed — all counters share cache lines ────────────────────────── */
typedef struct {
    uint64_t count;                  /* 8 bytes; 4 of these fit in one line */
} CounterPacked;

/* ── GOOD: padded — each counter owns a full cache line ──────────────────── */
typedef struct {
    uint64_t count;
    char     _pad[CACHE_LINE - sizeof(uint64_t)];   /* 56 bytes padding */
} __attribute__((aligned(CACHE_LINE))) CounterAligned;

_Static_assert(sizeof(CounterAligned) == CACHE_LINE, "must be 64 bytes");

/* ── Shared arrays (aligned to page boundary) ────────────────────────────── */
static CounterPacked   slow_counters[N_THREADS];
static CounterAligned  fast_counters[N_THREADS];

typedef struct { int id; int mode; } Arg;

static void *worker(void *varg)
{
    Arg *a = (Arg *)varg;
    if (a->mode == 0) {
        /* SLOW: write to packed counter — thrashes shared cache lines */
        for (long long i = 0; i < ITERS; ++i)
            slow_counters[a->id].count++;
    } else {
        /* FAST: write to cache-line-padded counter — stays in L1 */
        for (long long i = 0; i < ITERS; ++i)
            fast_counters[a->id].count++;
    }
    return NULL;
}

static double bench(int mode, const char *label)
{
    pthread_t tids[N_THREADS];
    Arg args[N_THREADS];
    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N_THREADS; ++i) {
        args[i].id   = i;
        args[i].mode = mode;
        pthread_create(&tids[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < N_THREADS; ++i) pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ms = (t1.tv_sec - t0.tv_sec)*1e3 + (t1.tv_nsec - t0.tv_nsec)*1e-6;
    long long total = (long long)N_THREADS * ITERS;
    printf("  %-38s  %6.0f ms   %5.0f Minc/s\n",
           label, ms, (double)total / ms / 1e3);
    return ms;
}

int main(void)
{
    printf("False sharing benchmark (%d threads × %lld increments)\n", N_THREADS, ITERS);
    printf("sizeof(CounterPacked)  = %zu bytes\n", sizeof(CounterPacked));
    printf("sizeof(CounterAligned) = %zu bytes  (1 cache line)\n\n",
           sizeof(CounterAligned));

    /* verify addresses */
    printf("CounterPacked addresses:\n");
    for (int i = 0; i < N_THREADS; ++i)
        printf("  [%d] %p  (offset from [0]: %zu)\n", i,
               (void*)&slow_counters[i],
               (size_t)((char*)&slow_counters[i] - (char*)&slow_counters[0]));

    printf("CounterAligned addresses:\n");
    for (int i = 0; i < N_THREADS; ++i)
        printf("  [%d] %p  (offset from [0]: %zu)\n", i,
               (void*)&fast_counters[i],
               (size_t)((char*)&fast_counters[i] - (char*)&fast_counters[0]));
    printf("\n");

    double t_slow = bench(0, "SLOW: packed   (false sharing)");
    double t_fast = bench(1, "FAST: aligned  (private cache line)");

    printf("\n  Speedup: %.1fx\n", t_slow / t_fast);
    printf("\nFIX IN ONE LINE:\n");
    printf("  char _pad[CACHE_LINE - sizeof(your_field)];  "
           "// in your per-thread struct\n");
    printf("  __attribute__((aligned(64)))                 "
           "// on the struct itself\n");

    printf("\nVERIFY CORRECTNESS:\n");
    long long expected = ITERS;
    int ok = 1;
    for (int i = 0; i < N_THREADS; ++i) {
        if ((long long)slow_counters[i].count != expected) ok = 0;
        if ((long long)fast_counters[i].count != expected) ok = 0;
    }
    printf("  All counters == %lld: %s\n", expected, ok ? "✓" : "✗");
    return 0;
}
