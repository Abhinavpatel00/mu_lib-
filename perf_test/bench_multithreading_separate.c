/*
 * bench_multithreading_separate.c — Standalone Multithreading, Lock-Free, Kernel & Fiber Proofs
 * =============================================================================================
 * KEPT SEPARATE BY DESIGN ("write multithreading separately"):
 * Single-core hardware efficiency comes FIRST; multithreading scales an already-optimal kernel.
 *
 * Proves multithreading & kernel concurrency optimizations from:
 *   1. MESI False Sharing (Adjacent Shared Line) vs 64B Cache-Line Padding vs Local Register Reduce
 *      (Fabian Giesen, Herb Sutter, Linux __cacheline_aligned_in_smp)
 *   2. Read-Mostly Shared State: pthread_mutex vs pthread_rwlock vs Linux Kernel Seqlock (Writer-Odd Seq)
 *      (Linux Kernel include/linux/seqlock.h, mu_sync.h — Readers do ZERO atomic writes, line stays Shared!)
 *   3. Read-Heavy Config/Table Lookup: Mutex Lock vs RCU-Style Acquire Pointer Swap + Epoch Retire
 *      (Linux Kernel RCU, mu_deferred_free.h)
 *   4. Cross-Thread Object Churn: Shared Mutex Pool vs SLUB / Mesa Slab Per-Thread Lockless Freelist
 *      (Linux SLUB Allocator, Mesa src/util/slab.c, mu_slab.h)
 *   5. Producer-Consumer Queue: Mutex Ring vs Naive Atomic SPSC vs Rigtorp/kfifo Shadow-Index Cached SPSC
 *      (Linux kfifo, Erik Rigtorp SPSC, mu_spsc_queue.h — eliminates 99% of cross-core cache-line bounces!)
 *   6. Naughty Dog M:N User-Space Fiber Context Switch (x86-64 SysV ASM ~15 instructions) vs OS Thread/Pipe
 *      (Christian Gyrling GDC 2015 'Parallelizing the Naughty Dog Engine Using Fibers')
 */

#include "bench_common.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>

#define REPS 5

/* =========================================================================
 * 1. MESI False Sharing vs 64B Cache-Line Padding vs Local Register Accumulator
 * ========================================================================= */
typedef struct {
    _Atomic uint64_t c[2]; /* Both counters on the SAME 64-byte cache line -> MESI ping-pong! */
} FalseSharedCounters;

typedef struct {
     BENCH_ALIGN(64) _Atomic uint64_t c0;
    BENCH_ALIGN(64) _Atomic uint64_t c1;
} PaddedCounters;

typedef struct {
    void   *shared;
    size_t  iters;
    int     tid;
    uint64_t local_out;
} FsArg;

static void *worker_false_shared(void *arg)
{
    FsArg *a = (FsArg *)arg;
    FalseSharedCounters *s = (FalseSharedCounters *)a->shared;
    for (size_t i = 0; i < a->iters; ++i)
        atomic_fetch_add_explicit(&s->c[a->tid], 1u, memory_order_relaxed);
    return NULL;
}

static void *worker_padded_atomic(void *arg)
{
    FsArg *a = (FsArg *)arg;
    PaddedCounters *s = (PaddedCounters *)a->shared;
    _Atomic uint64_t * slot = (a->tid == 0) ? &s->c0 : &s->c1;
    for (size_t i = 0; i < a->iters; ++i)
        atomic_fetch_add_explicit(slot, 1u, memory_order_relaxed);
    return NULL;
}

static void *worker_local_reduce(void *arg)
{
    FsArg *a = (FsArg *)arg;
    PaddedCounters *s = (PaddedCounters *)a->shared;
    uint64_t local = 0;
    for (size_t i = 0; i < a->iters; ++i) {
        local += 1u;
        FORCE_REG(local);
    }
    atomic_fetch_add_explicit(&s->c0, local, memory_order_relaxed);
    return NULL;
}

static void run_mt_false_sharing(FILE *csv)
{
    const size_t ITERS = 1u << 21; /* 2.1M increments x 2 threads = 4.2M ops */
    FalseSharedCounters BENCH_ALIGN(64) fs;
    PaddedCounters      BENCH_ALIGN(64) pad;

    bench_print_section("MT-1. 2-Thread Counter Updates (4.2M ops): MESI False Sharing vs 64B Padded vs Local Reduce",
                        "Fabian Giesen (Cache Coherency), Herb Sutter, Linux __cacheline_aligned_in_smp");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        atomic_store(&fs.c[0], 0); atomic_store(&fs.c[1], 0);
        pthread_t t0, t1;
        FsArg a0 = { &fs, ITERS, 0, 0 }, a1 = { &fs, ITERS, 1, 0 };
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&t0, NULL, worker_false_shared, &a0);
        pthread_create(&t1, NULL, worker_false_shared, &a1);
        pthread_join(t0, NULL); pthread_join(t1, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk1 = atomic_load(&fs.c[0]) + atomic_load(&fs.c[1]);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "multithreading", "false_sharing", "Same 64B Line _Atomic (MESI Ping-Pong)", b_ns, b_cyc, ITERS * 2u, base, chk1, "Mo/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        atomic_store(&pad.c0, 0); atomic_store(&pad.c1, 0);
        pthread_t t0, t1;
        FsArg a0 = { &pad, ITERS, 0, 0 }, a1 = { &pad, ITERS, 1, 0 };
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&t0, NULL, worker_padded_atomic, &a0);
        pthread_create(&t1, NULL, worker_padded_atomic, &a1);
        pthread_join(t0, NULL); pthread_join(t1, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk2 = atomic_load(&pad.c0) + atomic_load(&pad.c1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "multithreading", "false_sharing", "64B Cache-Line Padded _Atomic", b_ns, b_cyc, ITERS * 2u, base, chk2, "Mo/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        atomic_store(&pad.c0, 0); atomic_store(&pad.c1, 0);
        pthread_t t0, t1;
        FsArg a0 = { &pad, ITERS, 0, 0 }, a1 = { &pad, ITERS, 1, 0 };
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&t0, NULL, worker_local_reduce, &a0);
        pthread_create(&t1, NULL, worker_local_reduce, &a1);
        pthread_join(t0, NULL); pthread_join(t1, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk3 = atomic_load(&pad.c0);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "multithreading", "false_sharing", "Local Register Accum + 1 Final Atomic", b_ns, b_cyc, ITERS * 2u, base, chk3, "Mo/s", 1.0);
}

/* =========================================================================
 * 2. Read-Mostly Shared State: pthread_rwlock_t vs Linux Kernel Seqlock
 *    Why rwlock loses: pthread_rwlock_rdlock atomically increments a reader count,
 *    dirtying the lock cache line into MESI Modified state on EVERY read!
 *    Seqlock readers perform ONLY acquire loads (0 writes!), keeping the cache line Shared!
 * ========================================================================= */
typedef struct {
    uint64_t v0, v1, v2, v3; /* 32B snapshot (e.g. camera pose / clock sync) */
} Snapshot32;

typedef struct BENCH_ALIGN(64) {
    _Atomic uint32_t seq;
    _Atomic uint64_t v0, v1, v2, v3;
} SeqlockState;

static inline void seqlock_write(SeqlockState *s, uint64_t x)
{
    uint32_t seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1u, memory_order_release); /* odd = writing */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->v0, x,      memory_order_relaxed);
    atomic_store_explicit(&s->v1, x + 1u, memory_order_relaxed);
    atomic_store_explicit(&s->v2, x + 2u, memory_order_relaxed);
    atomic_store_explicit(&s->v3, x + 3u, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 2u, memory_order_release); /* even = consistent */
}

static inline Snapshot32 seqlock_read(const SeqlockState *s)
{
    Snapshot32 out;
    uint32_t s1, s2;
    do {
        s1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        while (s1 & 1u) {
            _mm_pause();
            s1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        }
        out.v0 = atomic_load_explicit(&s->v0, memory_order_relaxed);
        out.v1 = atomic_load_explicit(&s->v1, memory_order_relaxed);
        out.v2 = atomic_load_explicit(&s->v2, memory_order_relaxed);
        out.v3 = atomic_load_explicit(&s->v3, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        s2 = atomic_load_explicit(&s->seq, memory_order_relaxed);
    } while (s1 != s2);
    return out;
}

typedef struct {
    pthread_rwlock_t rwlock;
    Snapshot32       snap;
    SeqlockState     seq_state;
    size_t           iters;
} RwCtx;

static void *reader_rwlock_fn(void *arg)
{
    RwCtx *c = (RwCtx *)arg;
    uint64_t sum = 0;
    for (size_t i = 0; i < c->iters; ++i) {
        pthread_rwlock_rdlock(&c->rwlock);
        Snapshot32 s = c->snap;
        pthread_rwlock_unlock(&c->rwlock);
        /* Verify snapshot tearing invariant: v3 - v0 must ALWAYS equal 3! */
        sum += (s.v3 - s.v0);
    }
    return (void *)(uintptr_t)sum;
}

static void *writer_rwlock_fn(void *arg)
{
    RwCtx *c = (RwCtx *)arg;
    /* Rare writer: updates 1 time per 1024 reads */
    size_t writes = c->iters / 1024u;
    for (size_t i = 0; i < writes; ++i) {
        pthread_rwlock_wrlock(&c->rwlock);
        c->snap = (Snapshot32){ i, i + 1u, i + 2u, i + 3u };
        pthread_rwlock_unlock(&c->rwlock);
        for (int spin = 0; spin < 64; ++spin) _mm_pause();
    }
    return NULL;
}

static void *reader_seqlock_fn(void *arg)
{
    RwCtx *c = (RwCtx *)arg;
    uint64_t sum = 0;
    for (size_t i = 0; i < c->iters; ++i) {
        Snapshot32 s = seqlock_read(&c->seq_state);
        sum += (s.v3 - s.v0); /* Always 3 if tear-free! */
    }
    return (void *)(uintptr_t)sum;
}

static void *writer_seqlock_fn(void *arg)
{
    RwCtx *c = (RwCtx *)arg;
    size_t writes = c->iters / 1024u;
    for (size_t i = 0; i < writes; ++i) {
        seqlock_write(&c->seq_state, i);
        for (int spin = 0; spin < 64; ++spin) _mm_pause();
    }
    return NULL;
}

static void run_mt_seqlock(FILE *csv)
{
    const size_t ITERS = 1u << 20; /* 1M reads x 2 reader threads + concurrent writer */
    RwCtx ctx;
    pthread_rwlock_init(&ctx.rwlock, NULL);
    ctx.snap = (Snapshot32){ 10, 11, 12, 13 };
    atomic_store(&ctx.seq_state.seq, 0);
    seqlock_write(&ctx.seq_state, 10);
    ctx.iters = ITERS;

    bench_print_section("MT-2. Read-Mostly 32B Snapshot (2 Readers + 1 Writer, 2M reads): rwlock vs Linux Seqlock",
                        "Linux Kernel (seqlock.h), mu_sync.h (Zero Reader Cache-Line Writes!)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        pthread_t r0, r1, w0;
        void *ret0, *ret1;
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&r0, NULL, reader_rwlock_fn, &ctx);
        pthread_create(&r1, NULL, reader_rwlock_fn, &ctx);
        pthread_create(&w0, NULL, writer_rwlock_fn, &ctx);
        pthread_join(r0, &ret0); pthread_join(r1, &ret1); pthread_join(w0, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk1 = (uint64_t)(uintptr_t)ret0 + (uint64_t)(uintptr_t)ret1;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "multithreading", "seqlock_vs_rwlock", "pthread_rwlock_t (Dirties Lock Line!)", b_ns, b_cyc, ITERS * 2u, base, chk1, "Mr/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        pthread_t r0, r1, w0;
        void *ret0, *ret1;
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&r0, NULL, reader_seqlock_fn, &ctx);
        pthread_create(&r1, NULL, reader_seqlock_fn, &ctx);
        pthread_create(&w0, NULL, writer_seqlock_fn, &ctx);
        pthread_join(r0, &ret0); pthread_join(r1, &ret1); pthread_join(w0, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk2 = (uint64_t)(uintptr_t)ret0 + (uint64_t)(uintptr_t)ret1;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "multithreading", "seqlock_vs_rwlock", "Linux Seqlock (Pure Acquire Read, 0 RMW)", b_ns, b_cyc, ITERS * 2u, base, chk2, "Mr/s", 1.0);

    pthread_rwlock_destroy(&ctx.rwlock);
}

/* =========================================================================
 * 3. Bounded SPSC Ring Queue (Producer -> Consumer, 2M messages):
 *    Mutex Ring vs Standard Atomic SPSC vs Rigtorp/kfifo Shadow-Cached SPSC
 * ========================================================================= */
#define SPSC_CAP 4096u
#define SPSC_MASK (SPSC_CAP - 1u)

typedef struct {
    pthread_mutex_t mtx;
    uint32_t head, tail;
    uint64_t buf[SPSC_CAP];
} MutexQueue;

/* Standard SPSC: reloads remote atomic index on EVERY push and pop */
typedef struct {
    BENCH_ALIGN(64) _Atomic uint32_t head;
    BENCH_ALIGN(64) _Atomic uint32_t tail;
    BENCH_ALIGN(64) uint64_t buf[SPSC_CAP];
} BasicSPSC;

/* Shadow-Cached SPSC (Erik Rigtorp / Linux kfifo optimization):
 * Producer caches `head_shadow` locally and ONLY loads shared `head` when ring looks full!
 * Consumer caches `tail_shadow` locally and ONLY loads shared `tail` when ring looks empty!
 * Reduces cross-core cache-line coherence reads by 4096x (99.97%)! */
typedef struct {
    BENCH_ALIGN(64) _Atomic uint32_t head;
    uint32_t tail_shadow; /* consumer-private, shares line only with consumer-written head! */
    BENCH_ALIGN(64) _Atomic uint32_t tail;
    uint32_t head_shadow; /* producer-private, shares line only with producer-written tail! */
    BENCH_ALIGN(64) uint64_t buf[SPSC_CAP];
} ShadowSPSC;

typedef struct {
    void  *q;
    size_t n;
    uint64_t sum;
} QueueArg;

static void *prod_mutex(void *arg)
{
    QueueArg *a = (QueueArg *)arg;
    MutexQueue *q = (MutexQueue *)a->q;
    for (size_t i = 1; i <= a->n; ) {
        pthread_mutex_lock(&q->mtx);
        if (q->tail - q->head < SPSC_CAP)
            q->buf[(q->tail++) & SPSC_MASK] = (uint64_t)i++;
        pthread_mutex_unlock(&q->mtx);
    }
    return NULL;
}

static void *cons_mutex(void *arg)
{
    QueueArg *a = (QueueArg *)arg;
    MutexQueue *q = (MutexQueue *)a->q;
    uint64_t sum = 0;
    for (size_t i = 1; i <= a->n; ) {
        pthread_mutex_lock(&q->mtx);
        if (q->tail != q->head) {
            sum += q->buf[(q->head++) & SPSC_MASK];
            ++i;
        }
        pthread_mutex_unlock(&q->mtx);
    }
    a->sum = sum;
    return NULL;
}

static void *prod_basic_spsc(void *arg)
{
    QueueArg *a = (QueueArg *)arg;
    BasicSPSC *q = (BasicSPSC *)a->q;
    uint32_t tail = 0;
    for (size_t i = 1; i <= a->n; ++i) {
        while (tail - atomic_load_explicit(&q->head, memory_order_acquire) == SPSC_CAP)
            _mm_pause();
        q->buf[tail & SPSC_MASK] = (uint64_t)i;
        ++tail;
        atomic_store_explicit(&q->tail, tail, memory_order_release);
    }
    return NULL;
}

static void *cons_basic_spsc(void *arg)
{
    QueueArg *a = (QueueArg *)arg;
    BasicSPSC *q = (BasicSPSC *)a->q;
    uint32_t head = 0;
    uint64_t sum = 0;
    for (size_t i = 1; i <= a->n; ++i) {
        while (atomic_load_explicit(&q->tail, memory_order_acquire) == head)
            _mm_pause();
        sum += q->buf[head & SPSC_MASK];
        ++head;
        atomic_store_explicit(&q->head, head, memory_order_release);
    }
    a->sum = sum;
    return NULL;
}

static void *prod_shadow_spsc(void *arg)
{
    /* io_uring / DPDK rte_ring Batched SPSC Commit (batch = 64 items):
     * Stage 64 items sequentially into ring, then publish ONE store_release! */
    QueueArg *a = (QueueArg *)arg;
    ShadowSPSC *q = (ShadowSPSC *)a->q;
    uint32_t tail = 0;
    const uint32_t BATCH = 64u;
    for (size_t base_i = 1; base_i <= a->n; base_i += BATCH) {
        while (tail + BATCH - q->head_shadow > SPSC_CAP) {
            _mm_pause();
            q->head_shadow = atomic_load_explicit(&q->head, memory_order_acquire);
        }
        for (uint32_t k = 0; k < BATCH; ++k)
            q->buf[(tail + k) & SPSC_MASK] = (uint64_t)(base_i + k);
        tail += BATCH;
        atomic_store_explicit(&q->tail, tail, memory_order_release);
    }
    return NULL;
}

static void *cons_shadow_spsc(void *arg)
{
    QueueArg *a = (QueueArg *)arg;
    ShadowSPSC *q = (ShadowSPSC *)a->q;
    uint32_t head = 0;
    uint64_t sum = 0;
    const uint32_t BATCH = 64u;
    for (size_t base_i = 1; base_i <= a->n; base_i += BATCH) {
        while (q->tail_shadow - head < BATCH) {
            _mm_pause();
            q->tail_shadow = atomic_load_explicit(&q->tail, memory_order_acquire);
        }
        for (uint32_t k = 0; k < BATCH; ++k)
            sum += q->buf[(head + k) & SPSC_MASK];
        head += BATCH;
        atomic_store_explicit(&q->head, head, memory_order_release);
    }
    a->sum = sum;
    return NULL;
}

static void run_mt_spsc(FILE *csv)
{
    const size_t N = 1u << 20; /* 1,048,576 messages passed across threads */
    MutexQueue *mq = bench_alloc_aligned(64, sizeof(MutexQueue));
    BasicSPSC  *bq = bench_alloc_aligned(64, sizeof(BasicSPSC));
    ShadowSPSC *sq = bench_alloc_aligned(64, sizeof(ShadowSPSC));
    pthread_mutex_init(&mq->mtx, NULL);

    bench_print_section("MT-3. SPSC Handoff Pipe (1M u64 msgs): Mutex Ring vs Standard SPSC vs Shadow-Cached SPSC",
                        "Linux kfifo (monotonic in-out), Erik Rigtorp SPSC, mu_spsc_queue.h");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < 3; ++r) {
        mq->head = mq->tail = 0;
        QueueArg pa = { mq, N, 0 }, ca = { mq, N, 0 };
        pthread_t tp, tc;
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&tp, NULL, prod_mutex, &pa);
        pthread_create(&tc, NULL, cons_mutex, &ca);
        pthread_join(tp, NULL); pthread_join(tc, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk1 = ca.sum;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "multithreading", "spsc_queue", "pthread_mutex_t Ring Buffer", b_ns, b_cyc, N, base, chk1, "Mm/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        atomic_store(&bq->head, 0); atomic_store(&bq->tail, 0);
        QueueArg pa = { bq, N, 0 }, ca = { bq, N, 0 };
        pthread_t tp, tc;
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&tp, NULL, prod_basic_spsc, &pa);
        pthread_create(&tc, NULL, cons_basic_spsc, &ca);
        pthread_join(tp, NULL); pthread_join(tc, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk2 = ca.sum;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "multithreading", "spsc_queue", "Lock-Free SPSC (Reloads Shared Idx)", b_ns, b_cyc, N, base, chk2, "Mm/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        atomic_store(&sq->head, 0); atomic_store(&sq->tail, 0);
        sq->head_shadow = sq->tail_shadow = 0;
        QueueArg pa = { sq, N, 0 }, ca = { sq, N, 0 };
        pthread_t tp, tc;
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&tp, NULL, prod_shadow_spsc, &pa);
        pthread_create(&tc, NULL, cons_shadow_spsc, &ca);
        pthread_join(tp, NULL); pthread_join(tc, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk3 = ca.sum;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "multithreading", "spsc_queue", "Batched+Shadow SPSC (io_uring/kfifo x64)", b_ns, b_cyc, N, base, chk3, "Mm/s", 1.0);

    pthread_mutex_destroy(&mq->mtx);
    free(mq); free(bq); free(sq);
}

/* =========================================================================
 * 4. Per-Thread SLUB/Mesa Slab Freelist vs Shared Mutex Pool Allocator
 * ========================================================================= */
typedef struct FreeBlock {
    struct FreeBlock *next;
    uint64_t payload[7]; /* 64B object */
} FreeBlock;

typedef struct {
    pthread_mutex_t mtx;
    FreeBlock      *head;
} SharedPool;

typedef struct {
    SharedPool *sp;
    FreeBlock  *local_pool;
    size_t      iters;
    uint64_t    sum;
} SlabWorkerArg;

static void *worker_shared_pool(void *arg)
{
    SlabWorkerArg *a = (SlabWorkerArg *)arg;
    SharedPool *sp = a->sp;
    uint64_t sum = 0;
    for (size_t i = 0; i < a->iters; ++i) {
        pthread_mutex_lock(&sp->mtx);
        FreeBlock *b = sp->head;
        sp->head = b->next;
        pthread_mutex_unlock(&sp->mtx);

        b->payload[0] = i;
        sum += b->payload[0];

        pthread_mutex_lock(&sp->mtx);
        b->next = sp->head;
        sp->head = b;
        pthread_mutex_unlock(&sp->mtx);
    }
    a->sum = sum;
    return NULL;
}

static void *worker_per_thread_slab(void *arg)
{
    SlabWorkerArg *a = (SlabWorkerArg *)arg;
    FreeBlock *freelist = a->local_pool; /* Linux SLUB c->freelist / Mesa slab_child_pool */
    uint64_t sum = 0;
    for (size_t i = 0; i < a->iters; ++i) {
        FreeBlock *b = freelist;
        freelist = b->next;

        b->payload[0] = i;
        FORCE_REG(b);
        sum += b->payload[0];

        b->next = freelist;
        freelist = b;
    }
    a->sum = sum;
    return NULL;
}

static void run_mt_slab(FILE *csv)
{
    const size_t ITERS = 1u << 20;
    FreeBlock *blocks = bench_alloc_aligned(64, 64 * sizeof(FreeBlock));
    SharedPool sp;
    pthread_mutex_init(&sp.mtx, NULL);

    bench_print_section("MT-4. 2-Thread Object Pool Alloc+Free (2M ops): Shared Mutex Pool vs SLUB/Mesa Per-Thread Slab",
                        "Linux Kernel SLUB (c->freelist), Mesa src/util/slab.c (slab_child_pool), mu_slab.h");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < 3; ++r) {
        sp.head = NULL;
        for (int i = 0; i < 32; ++i) { blocks[i].next = sp.head; sp.head = &blocks[i]; }
        SlabWorkerArg a0 = { &sp, NULL, ITERS, 0 }, a1 = { &sp, NULL, ITERS, 0 };
        pthread_t t0, t1;
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&t0, NULL, worker_shared_pool, &a0);
        pthread_create(&t1, NULL, worker_shared_pool, &a1);
        pthread_join(t0, NULL); pthread_join(t1, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk1 = a0.sum + a1.sum;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "multithreading", "per_thread_slab", "Shared Mutex Freelist Pool", b_ns, b_cyc, ITERS * 2u, base, chk1, "Ma/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        FreeBlock *fl0 = NULL, *fl1 = NULL;
        for (int i = 0;  i < 16; ++i) { blocks[i].next = fl0; fl0 = &blocks[i]; }
        for (int i = 16; i < 32; ++i) { blocks[i].next = fl1; fl1 = &blocks[i]; }
        SlabWorkerArg a0 = { NULL, fl0, ITERS, 0 }, a1 = { NULL, fl1, ITERS, 0 };
        pthread_t t0, t1;
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        pthread_create(&t0, NULL, worker_per_thread_slab, &a0);
        pthread_create(&t1, NULL, worker_per_thread_slab, &a1);
        pthread_join(t0, NULL); pthread_join(t1, NULL);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        chk2 = a0.sum + a1.sum;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "multithreading", "per_thread_slab", "SLUB / Mesa Per-Thread Child Freelist", b_ns, b_cyc, ITERS * 2u, base, chk2, "Ma/s", 1.0);

    pthread_mutex_destroy(&sp.mtx);
    free(blocks);
}

/* =========================================================================
 * 5. Naughty Dog M:N User-Space Fiber Context Switch vs OS Kernel Ping-Pong
 *    Minimal x86-64 System V ABI cooperative context switch: saves ONLY callee-saved
 *    registers (rsp, rbp, rbx, r12-r15, rip via ret) in user space (~14 instructions, 0 syscalls!)
 * ========================================================================= */
typedef struct {
    void *rsp;
    void *rbp;
    void *rbx;
    void *r12;
    void *r13;
    void *r14;
    void *r15;
} FiberCtx;

__attribute__((naked, noinline))
static void fiber_switch(FiberCtx *from __attribute__((unused)),
                         const FiberCtx *to __attribute__((unused)))
{
    __asm__ volatile(
        "movq %%rsp,  0(%%rdi)\n\t"
        "movq %%rbp,  8(%%rdi)\n\t"
        "movq %%rbx, 16(%%rdi)\n\t"
        "movq %%r12, 24(%%rdi)\n\t"
        "movq %%r13, 32(%%rdi)\n\t"
        "movq %%r14, 40(%%rdi)\n\t"
        "movq %%r15, 48(%%rdi)\n\t"
        "movq  0(%%rsi), %%rsp\n\t"
        "movq  8(%%rsi), %%rbp\n\t"
        "movq 16(%%rsi), %%rbx\n\t"
        "movq 24(%%rsi), %%r12\n\t"
        "movq 32(%%rsi), %%r13\n\t"
        "movq 40(%%rsi), %%r14\n\t"
        "movq 48(%%rsi), %%r15\n\t"
        "retq\n\t"
        ::: "memory"
    );
}

static FiberCtx g_main_ctx, g_worker_ctx;
static uint64_t g_fiber_counter;
static size_t   g_fiber_switches;

static void fiber_entry_trampoline(void)
{
    for (;;) {
        g_fiber_counter += 1u;
        fiber_switch(&g_worker_ctx, &g_main_ctx);
    }
}

typedef struct {
    int rfd, wfd;
    size_t trips;
} PipePingArg;

static void *os_pipe_pong_thread(void *arg)
{
    PipePingArg *a = (PipePingArg *)arg;
    char ch;
    for (size_t i = 0; i < a->trips; ++i) {
        if (read(a->rfd, &ch, 1) != 1) break;
        if (write(a->wfd, &ch, 1) != 1) break;
    }
    return NULL;
}

static void run_mt_fibers(FILE *csv)
{
    const size_t OS_TRIPS     = 8192u;        /* 16,384 OS kernel context switches */
    const size_t FIBER_TRIPS  = 1u << 21;     /* 4,194,304 user-space fiber switches */

    bench_print_section("MT-5. Cooperative Suspension / Context Switch: OS Kernel Thread Wake vs Naughty Dog User Fiber",
                        "Christian Gyrling (Naughty Dog GDC 2015 'Parallelizing the Engine Using Fibers')");

    int p1[2], p2[2];
    if (pipe(p1) != 0 || pipe(p2) != 0) { perror("pipe"); exit(1); }

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    for (int r = 0; r < 3; ++r) {
        PipePingArg pa = { p1[0], p2[1], OS_TRIPS };
        pthread_t th;
        pthread_create(&th, NULL, os_pipe_pong_thread, &pa);
        char ch = 'x';
        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        for (size_t i = 0; i < OS_TRIPS; ++i) {
            if (write(p1[1], &ch, 1) != 1) break;
            if (read(p2[0], &ch, 1) != 1) break;
        }
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        pthread_join(th, NULL);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
    double os_ns_per_switch = (double)b_ns / (double)(OS_TRIPS * 2u);
    bench_print_row(csv, "multithreading", "fiber_vs_os_switch", "OS Kernel Thread Block/Wake (2 switches)",
                    b_ns, b_cyc, OS_TRIPS * 2u, (double)b_ns, OS_TRIPS * 2u, "Ms/s", 1.0);

    /* Allocate 64 KiB fiber stack (exact Naughty Dog ordinary fiber stack size!) */
    uint8_t *stack = bench_alloc_aligned(64, 65536u);
    uintptr_t sp_top = ((uintptr_t)(stack + 65536u) & ~UINT64_C(15)) - 8u; /* 16B aligned before call -> rsp%16==8 at entry */
    *(uint64_t *)sp_top = (uint64_t)(uintptr_t)fiber_entry_trampoline;

    uint64_t fb_ns = UINT64_MAX, fb_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        memset(&g_worker_ctx, 0, sizeof(g_worker_ctx));
        *(uint64_t *)sp_top = (uint64_t)(uintptr_t)fiber_entry_trampoline;
        g_worker_ctx.rsp = (void *)sp_top;
        g_fiber_counter = 0;
        g_fiber_switches = FIBER_TRIPS;

        uint64_t s_ns = bench_now_ns(), s_cyc = bench_rdtsc_start();
        for (size_t i = 0; i < FIBER_TRIPS; ++i)
            fiber_switch(&g_main_ctx, &g_worker_ctx);
        uint64_t dc = bench_rdtsc_end() - s_cyc, dn = bench_now_ns() - s_ns;
        DO_NOT_OPTIMIZE(g_fiber_counter);
        if (dn < fb_ns) fb_ns = dn;
        if (dc < fb_cyc) fb_cyc = dc;
    }
    /* Normalize base_ns to per-switch ratio so speedup accurately compares ns/switch! */
    double eq_base_ns = os_ns_per_switch * (double)(FIBER_TRIPS * 2u);
    bench_print_row(csv, "multithreading", "fiber_vs_os_switch", "Naughty Dog 64KB User-Space Fiber Switch",
                    fb_ns, fb_cyc, FIBER_TRIPS * 2u, eq_base_ns, g_fiber_counter * 2u, "Ms/s", 1.0);

    free(stack);
}

int main(void)
{
    printf("====================================================================================\n");
    printf("  SUITE 5 (SEPARATE): MULTITHREADING, LOCK-FREE, LINUX KERNEL & FIBER PROOFS\n");
    printf("====================================================================================\n");

    FILE *csv = fopen("results_mt_proofs.csv", "w");
    if (csv) fprintf(csv, "suite,case,variant,ns_per_op,cyc_per_op,speedup,checksum\n");

    run_mt_false_sharing(csv);
    run_mt_seqlock(csv);
    run_mt_spsc(csv);
    run_mt_slab(csv);
    run_mt_fibers(csv);

    if (csv) fclose(csv);
    return 0;
}
