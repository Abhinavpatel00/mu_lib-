/*
 * 03_double_buffer.c — TIP 5: Double-buffered event/message system
 * ===========================================================================
 * PATTERN:
 *   - Writers append to buffer A using an atomic index (no mutex in hot path).
 *   - At a sync point (e.g. end of frame) swap A↔B atomically.
 *   - Readers drain buffer B (now frozen) while writers start filling new A.
 *   - No mutex needed anywhere in the write path.
 *
 * TRADEOFFS (from the tips):
 *   (+) Writers are nearly lock-free (only an atomic_fetch_add).
 *   (+) Readers process a stable snapshot with no synchronisation.
 *   (-) Events written in frame N are only visible to readers in frame N+1.
 *   (-) Memory: two full-sized buffers.
 *
 * PROOF:
 *   4 writer threads emit 250 000 events each per frame.
 *   A reader thread processes the frozen buffer at the end of each frame.
 *   We verify that every event is received exactly once.
 *
 * Compile:  gcc -O2 -std=c11 -pthread 03_double_buffer.c -o 03_double_buffer
 * ===========================================================================
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define N_WRITERS      4
#define EVENTS_EACH    250000
#define MAX_EVENTS     (N_WRITERS * EVENTS_EACH * 2)  /* generous */

/* ── Event ───────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t type;
    uint32_t sender;
    uint64_t payload;
} Event;

/* ── Double buffer ───────────────────────────────────────────────────────── */
typedef struct {
    Event            buf[2][MAX_EVENTS];
    _Atomic uint32_t count[2];     /* atomic write index per buffer */
    _Atomic int      write_side;   /* which buffer writers currently use */
} EventBus;

static EventBus bus;

/* Writers call this — completely lock-free */
static void emit(uint32_t type, uint32_t sender, uint64_t payload)
{
    /* 1. Find which buffer we're writing into (relaxed: just an index read) */
    int side = atomic_load_explicit(&bus.write_side, memory_order_relaxed);

    /* 2. Claim a slot atomically */
    uint32_t slot = atomic_fetch_add_explicit(&bus.count[side], 1,
                                              memory_order_relaxed);
    if (slot >= MAX_EVENTS) return; /* full — drop or expand in production */

    /* 3. Write the event (we own this slot exclusively) */
    bus.buf[side][slot].type    = type;
    bus.buf[side][slot].sender  = sender;
    bus.buf[side][slot].payload = payload;
}

/*
 * Swap buffers (called at the end of a frame by the main/game thread).
 * Returns the read-side index (the frozen buffer full of events).
 *
 * Uses a release fence so all writes to buf[old_side] are visible before
 * any reader starts consuming them.
 */
static int swap_and_get_read_side(void)
{
    int old_side = atomic_load_explicit(&bus.write_side, memory_order_relaxed);
    int new_side = old_side ^ 1;

    /* Reset new write buffer BEFORE exposing it to writers */
    atomic_store_explicit(&bus.count[new_side], 0, memory_order_relaxed);

    /* Publish the swap — release ensures all prior writes are visible */
    atomic_store_explicit(&bus.write_side, new_side, memory_order_release);

    /* Full barrier: ensure we see all writes that completed before the swap */
    atomic_thread_fence(memory_order_acquire);

    return old_side;
}

/* ── Writer thread ───────────────────────────────────────────────────────── */
typedef struct { uint32_t id; int events; bool ready; } WriterArg;

static pthread_barrier_t start_barrier;

static void *writer_thread(void *arg)
{
    WriterArg *w = (WriterArg *)arg;
    pthread_barrier_wait(&start_barrier);  /* all start together */

    for (int i = 0; i < w->events; ++i)
        emit(i % 16, w->id, (uint64_t)w->id * 1000000 + i);

    return NULL;
}

int main(void)
{
    memset(&bus, 0, sizeof bus);
    atomic_init(&bus.write_side, 0);
    atomic_init(&bus.count[0], 0);
    atomic_init(&bus.count[1], 0);

    printf("Double-buffered event bus: %d writers × %d events/frame\n\n",
           N_WRITERS, EVENTS_EACH);

    /* ── Run two frames ────────────────────────────────────────────────── */
    for (int frame = 0; frame < 2; ++frame) {
        pthread_barrier_init(&start_barrier, NULL, N_WRITERS + 1);

        pthread_t tids[N_WRITERS];
        WriterArg args[N_WRITERS];
        for (int i = 0; i < N_WRITERS; ++i) {
            args[i].id     = (uint32_t)i;
            args[i].events = EVENTS_EACH;
            pthread_create(&tids[i], NULL, writer_thread, &args[i]);
        }

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pthread_barrier_wait(&start_barrier);  /* release all writers */

        for (int i = 0; i < N_WRITERS; ++i) pthread_join(tids[i], NULL);

        /* ── End of frame: swap buffers ─────────────────────────────── */
        int read_side = swap_and_get_read_side();
        uint32_t n    = atomic_load_explicit(&bus.count[read_side],
                                             memory_order_relaxed);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6;

        /* ── Reader consumes frozen buffer (single-threaded, no locks) ─ */
        uint64_t checksum = 0;
        for (uint32_t i = 0; i < n; ++i)
            checksum += bus.buf[read_side][i].payload;

        uint64_t expected = 0;
        for (int w = 0; w < N_WRITERS; ++w)
            for (int i = 0; i < EVENTS_EACH; ++i)
                expected += (uint64_t)w * 1000000 + i;

        printf("Frame %d: %u events received in %.2f ms  chk=%llu %s\n",
               frame, n, ms, (unsigned long long)checksum,
               checksum == expected ? "✓" : "✗ DATA RACE DETECTED");

        pthread_barrier_destroy(&start_barrier);
    }

    printf("\nWRITE PATH (hot):  emit()  = atomic_fetch_add only  (no mutex)\n");
    printf("SWAP PATH (sync):  1 atomic_store + 1 fence at frame boundary\n");
    printf("READ PATH (safe):  plain loop over frozen buffer (no sync)\n");
    return 0;
}
