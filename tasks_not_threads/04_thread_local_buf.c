/*
 * 04_thread_local_buf.c — TIP 6: Thread-local write buffers (zero contention)
 * ===========================================================================
 * PATTERN:
 *   Every thread has its own _Thread_local buffer.
 *   Writes go there without any synchronisation at all.
 *   At a sync point (end of frame) the main thread reads all thread-local
 *   buffers and merges them into a global read buffer.
 *
 * This is BETTER than double-buffering with a shared atomic index because:
 *   - atomic_fetch_add still causes cache-line bouncing between cores
 *     (the atomic counter's cache line must be transferred to each writer).
 *   - Thread-local buffers have zero cross-core communication during writes.
 *
 * BENCHMARK: shared atomic counter vs thread-local counter, 4 threads.
 *
 * Herb Sutter (Part 1, "The Big Difference"):
 *   Even a single uncontended atomic operation takes ~5ns because the cache
 *   line must be exclusively owned. Multiply by millions of ops/frame and
 *   it adds up. Thread-local removes this entirely.
 *
 * Compile:  gcc -O2 -std=c11 -pthread 04_thread_local_buf.c -o 04_thread_local_buf
 * ===========================================================================
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define N_THREADS  4
#define N_WRITES   (5 * 1000 * 1000)    /* 5 M writes per thread */
#define BUF_CAP    (N_WRITES + 64)

/* ── Method A: shared atomic counter ────────────────────────────────────── */
static _Atomic uint64_t shared_total;

static void *shared_atomic_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < N_WRITES; ++i)
        atomic_fetch_add_explicit(&shared_total, (uint64_t)i,
                                  memory_order_relaxed);
    return NULL;
}

/* ── Method B: thread-local buffer, merged at sync point ────────────────── */

/* Each thread has a private buffer.  No atomic, no mutex, no contention. */
typedef struct {
    uint64_t data[BUF_CAP];
    int      count;
} LocalBuf;

static _Thread_local LocalBuf tl_buf;   /* private per OS thread */

/* The merged global result, filled only at the sync point */
static uint64_t global_total;

static void tl_write(uint64_t val)
{
    /* Pure local write — no synchronisation whatsoever */
    tl_buf.data[tl_buf.count++] = val;
}

/* Called by each thread when its work is done, BEFORE joining.
   In production you'd swap or copy to a second buffer. */
static void tl_flush_into_global(void)
{
    /* This is the merge step at the sync point.
       Still no mutex — main thread reads only after all threads have joined. */
    uint64_t local_sum = 0;
    for (int i = 0; i < tl_buf.count; ++i) local_sum += tl_buf.data[i];
    /* Use an atomic for the final accumulation — only happens once per thread */
    atomic_fetch_add_explicit((_Atomic uint64_t *)&global_total, local_sum,
                              memory_order_relaxed);
}

static void *tl_worker(void *arg)
{
    (void)arg;
    tl_buf.count = 0;
    for (int i = 0; i < N_WRITES; ++i) tl_write((uint64_t)i);
    tl_flush_into_global();
    return NULL;
}

/* ── Benchmark harness ───────────────────────────────────────────────────── */
static double run(void *(*fn)(void*), const char *label)
{
    pthread_t tids[N_THREADS];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N_THREADS; ++i) pthread_create(&tids[i], NULL, fn, NULL);
    for (int i = 0; i < N_THREADS; ++i) pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ms = (t1.tv_sec - t0.tv_sec)*1e3 + (t1.tv_nsec - t0.tv_nsec)*1e-6;
    long total_ops = (long)N_THREADS * N_WRITES;
    printf("  %-36s  %7.1f ms   %6.0f Mwrite/s\n",
           label, ms, (double)total_ops / ms / 1e3);
    return ms;
}

int main(void)
{
    printf("Thread-local vs shared atomic  (%d threads × %d writes)\n\n",
           N_THREADS, N_WRITES);

    atomic_store(&shared_total, 0);
    double t_atomic = run(shared_atomic_worker, "shared _Atomic (relaxed)");

    global_total = 0;
    double t_local  = run(tl_worker,            "_Thread_local buffer (merge at end)");

    /* Verify both produce the same result */
    uint64_t expected = (uint64_t)N_WRITES * (N_WRITES - 1) / 2 * N_THREADS;
    printf("\n  shared_total = %llu  %s\n",
           (unsigned long long)atomic_load(&shared_total),
           atomic_load(&shared_total) == expected ? "✓" : "✗");
    printf("  global_total = %llu  %s\n",
           (unsigned long long)global_total,
           global_total == expected ? "✓" : "✗");
    printf("\n  Thread-local is %.1fx faster than shared atomic\n",
           t_atomic / t_local);

    printf("\nWHY: atomic_fetch_add bounces the cache line between cores.\n");
    printf("     _Thread_local stays in that core's L1 cache exclusively.\n");
    printf("     Merge happens once at the sync point — negligible overhead.\n");
    return 0;
}
