/*
 * 02_atomic_alloc.c — TIP 4: Atomic index into shared buffer (bump allocator)
 * ===========================================================================
 * PATTERN: ptr = atomic_fetch_add(&offset, size)
 *
 * Idea: You have one big buffer (e.g., a per-frame scratch arena).
 * Many tasks need to carve out their own slice of it WITHOUT a mutex.
 * atomic_fetch_add atomically returns the OLD offset and advances it.
 * The task now owns bytes [old_offset, old_offset + size) — exclusively.
 * No lock needed, no contention.
 *
 * Herb Sutter (Part 2): this is a classic use of relaxed-or-release atomics.
 * The fetch_add itself guarantees the slice is unique.
 * The actual writes into the slice don't need any special ordering because
 * no other thread will ever touch those bytes.
 *
 * BENCHMARK: malloc() vs mutex-protected bump vs atomic bump
 *
 * Compile:  gcc -O2 -std=c11 -pthread 02_atomic_alloc.c -o 02_atomic_alloc
 * Run:      ./02_atomic_alloc
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

#define ARENA_SIZE  (128 * 1024 * 1024)  /* 128 MB scratch arena */
#define N_THREADS   4
#define ALLOCS_EACH 500000               /* allocations per thread */
#define ALLOC_SZ    64                   /* bytes per allocation   */

static char  arena[ARENA_SIZE];

/* ── Method A: atomic bump allocator ────────────────────────────────────── */
static _Atomic size_t atomic_offset;

static char *atomic_bump_alloc(size_t sz)
{
    size_t old = atomic_fetch_add_explicit(&atomic_offset, sz,
                                           memory_order_relaxed);
    if (old + sz > ARENA_SIZE) return NULL; /* arena exhausted */
    return arena + old;
}

/* ── Method B: mutex-protected bump ─────────────────────────────────────── */
static pthread_mutex_t bump_mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t mutex_offset;

static char *mutex_bump_alloc(size_t sz)
{
    pthread_mutex_lock(&bump_mutex);
    size_t old = mutex_offset;
    mutex_offset += sz;
    pthread_mutex_unlock(&bump_mutex);
    if (old + sz > ARENA_SIZE) return NULL;
    return arena + old;
}

/* ── Thread work ─────────────────────────────────────────────────────────── */
typedef struct { int mode; _Atomic int64_t *total; } ThreadArg;

static void *worker(void *arg)
{
    ThreadArg *a = (ThreadArg *)arg;
    int64_t local_sum = 0;

    for (int i = 0; i < ALLOCS_EACH; ++i) {
        char *ptr = (a->mode == 0)
                  ? atomic_bump_alloc(ALLOC_SZ)
                  : mutex_bump_alloc(ALLOC_SZ);

        if (!ptr) break;
        /* write something so compiler can't eliminate the allocation */
        memset(ptr, (char)i, ALLOC_SZ);
        local_sum += ptr[0];
    }
    atomic_fetch_add_explicit(a->total, local_sum, memory_order_relaxed);
    return NULL;
}

static double run_bench(int mode, const char *label)
{
    atomic_store(&atomic_offset, 0);
    mutex_offset = 0;

    _Atomic int64_t total = 0;
    pthread_t tids[N_THREADS];
    ThreadArg args[N_THREADS];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < N_THREADS; ++i) {
        args[i].mode  = mode;
        args[i].total = &total;
        pthread_create(&tids[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < N_THREADS; ++i) pthread_join(tids[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6;
    long total_allocs = N_THREADS * ALLOCS_EACH;
    printf("  %-28s  %6.1f ms   %6.0f Malloc/s  (chk=%lld)\n",
           label, ms, total_allocs / ms * 1e3,
           (long long)atomic_load(&total));
    return ms;
}

int main(void)
{
    printf("Atomic bump allocator vs mutex bump  (%d threads, %d allocs each)\n\n",
           N_THREADS, ALLOCS_EACH);
    double t_atomic = run_bench(0, "atomic_fetch_add (no lock)");
    double t_mutex  = run_bench(1, "mutex-protected bump");
    printf("\n  Atomic is %.1fx faster than mutex\n",
           t_mutex / t_atomic);

    printf("\nPATTERN EXPLAINED:\n");
    printf("  size_t slot = atomic_fetch_add(&offset, ALLOC_SZ);\n");
    printf("  char  *ptr  = arena + slot;   // yours exclusively\n");
    printf("  // write freely — no other thread will ever touch [slot, slot+SZ)\n");
    printf("  // Reset at frame end:  atomic_store(&offset, 0);\n");
    return 0;
}
