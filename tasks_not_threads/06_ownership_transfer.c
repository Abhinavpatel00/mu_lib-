/*
 * 06_ownership_transfer.c — TIP 3: Single owner per data; pass by handing off
 * ===========================================================================
 * PATTERN:
 *   Each piece of data has exactly ONE owner at a time.
 *   To "share" it, you transfer ownership: producer writes, releases;
 *   consumer acquires, reads — then owns it.
 *   No mutex. No race. Enforced by the acquire/release memory model.
 *
 * This is the fundamental primitive Herb Sutter calls "the handshake":
 *
 *   PRODUCER                       CONSUMER
 *   ────────                       ────────
 *   write data                     
 *   atomic_store(flag, 1,          ...poll or wait...
 *                memory_order_release)
 *                                  atomic_load(flag,
 *                                              memory_order_acquire)
 *                                  read data  ← guaranteed to see writes
 *
 * The RELEASE store is a one-way barrier: nothing above it leaks below.
 * The ACQUIRE load is a one-way barrier: nothing below it pulls above.
 * Together they form a "synchronises-with" edge in the C11 memory model.
 * This is cheaper than seq_cst on ARM/POWER (no full dmb needed).
 *
 * DEMO: pipeline of 3 stages; data passes through via ownership hand-off.
 *       Stage1 → Stage2 → Stage3 with zero locks.
 *
 * Compile:  gcc -O2 -std=c11 -pthread 06_ownership_transfer.c -o 06_ownership_transfer
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
#include <sched.h>

#define N_ITEMS  1000000

/* ── Data packet ─────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t value;
    uint64_t stage1_result;
    uint64_t stage2_result;
} Packet;

/*
 * ── Lock-free single-slot handoff channel ────────────────────────────────
 * Capacity = 1 (producer waits until consumer takes).
 * Extend to a ring buffer for higher throughput.
 */
typedef struct {
    Packet         item;
    _Atomic int    has_item;    /* 0 = empty (producer may write)
                                   1 = full  (consumer may read)  */
} Channel;

static void channel_send(Channel *ch, Packet *p)
{
    /* Wait until consumer has taken the previous item */
    while (atomic_load_explicit(&ch->has_item, memory_order_relaxed) != 0)
        sched_yield();

    /* Write the data */
    ch->item = *p;

    /* Release: all writes above are visible to whoever does the acquire-load */
    atomic_store_explicit(&ch->has_item, 1, memory_order_release);
}

static void channel_recv(Channel *ch, Packet *out)
{
    /* Wait until producer has placed an item */
    while (atomic_load_explicit(&ch->has_item, memory_order_relaxed) != 1)
        sched_yield();

    /* Acquire: we now own the data; all writes before the release are visible */
    atomic_thread_fence(memory_order_acquire);
    *out = ch->item;

    /* Hand back ownership to producer */
    atomic_store_explicit(&ch->has_item, 0, memory_order_release);
}

/* ── Pipeline channels ───────────────────────────────────────────────────── */
static Channel s1_to_s2;
static Channel s2_to_s3;
static _Atomic int64_t final_sum;
static _Atomic int     done;

/* ── Stage 1: generate values ───────────────────────────────────────────── */
static void *stage1(void *arg)
{
    (void)arg;
    for (int i = 0; i < N_ITEMS; ++i) {
        Packet p = { .value = (uint64_t)i, .stage1_result = (uint64_t)i * i };
        channel_send(&s1_to_s2, &p);
    }
    /* Sentinel */
    Packet end = { .value = UINT64_MAX };
    channel_send(&s1_to_s2, &end);
    return NULL;
}

/* ── Stage 2: transform ──────────────────────────────────────────────────── */
static void *stage2(void *arg)
{
    (void)arg;
    for (;;) {
        Packet p;
        channel_recv(&s1_to_s2, &p);
        if (p.value == UINT64_MAX) {
            channel_send(&s2_to_s3, &p);   /* forward sentinel */
            break;
        }
        p.stage2_result = p.stage1_result + p.value; /* i^2 + i */
        channel_send(&s2_to_s3, &p);
    }
    return NULL;
}

/* ── Stage 3: accumulate ─────────────────────────────────────────────────── */
static void *stage3(void *arg)
{
    (void)arg;
    int64_t sum = 0;
    for (;;) {
        Packet p;
        channel_recv(&s2_to_s3, &p);
        if (p.value == UINT64_MAX) break;
        sum += (int64_t)p.stage2_result;
    }
    atomic_store_explicit(&final_sum, sum, memory_order_release);
    atomic_store_explicit(&done, 1, memory_order_release);
    return NULL;
}

int main(void)
{
    printf("Ownership-transfer pipeline: Stage1 → Stage2 → Stage3\n");
    printf("No mutex. Only acquire/release atomics on the channel flag.\n");
    printf("Processing %d packets...\n\n", N_ITEMS);

    /* Each channel starts empty */
    atomic_init(&s1_to_s2.has_item, 0);
    atomic_init(&s2_to_s3.has_item, 0);
    atomic_init(&done, 0);

    pthread_t t1, t2, t3;
    pthread_create(&t1, NULL, stage1, NULL);
    pthread_create(&t2, NULL, stage2, NULL);
    pthread_create(&t3, NULL, stage3, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    /* Expected: sum of (i^2 + i) for i in [0, N_ITEMS) */
    int64_t expected = 0;
    for (int i = 0; i < N_ITEMS; ++i) expected += (int64_t)i*i + i;

    int64_t got = atomic_load(&final_sum);
    printf("Result: %lld  expected: %lld  %s\n",
           (long long)got, (long long)expected,
           got == expected ? "✓ correct" : "✗ DATA RACE");

    printf("\nMEMORY MODEL:\n");
    printf("  Producer: write data → store(flag, 1, RELEASE)\n");
    printf("  Consumer: load(flag, ACQUIRE) → read data\n");
    printf("  RELEASE + ACQUIRE = 'synchronises-with' edge in C11.\n");
    printf("  Everything written before RELEASE is seen after ACQUIRE.\n");
    printf("  Cost on x86: nearly free (TSO). On ARM: one-way DMB fence.\n");
    return 0;
}
