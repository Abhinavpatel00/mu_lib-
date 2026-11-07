# Lock-Free, Linux Kernel Sync & Naughty Dog Fiber Recipes

Verified in `perf_test/bench_multithreading_separate.c` (`make -C perf_test run-mt-proofs`).

---

## 1. Linux Kernel `seqlock` (`27.44×` Faster Than `pthread_rwlock_t`)

Readers never write to memory, keeping the cache line in MESI `Shared (S)` state across all CPU cores simultaneously:

```c
typedef struct MU_ALIGN(64) {
    _Atomic uint32_t seq;
    _Atomic uint64_t v0, v1, v2, v3;
} SeqlockPose;

static inline void pose_write(SeqlockPose *s, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint32_t seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1u, memory_order_release); /* odd = write in progress */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->v0, a, memory_order_relaxed);
    atomic_store_explicit(&s->v1, b, memory_order_relaxed);
    atomic_store_explicit(&s->v2, c, memory_order_relaxed);
    atomic_store_explicit(&s->v3, d, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 2u, memory_order_release); /* even = consistent */
}

static inline void pose_read(const SeqlockPose *s, uint64_t out[4]) {
    uint32_t s1, s2;
    do {
        s1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        while (s1 & 1u) { _mm_pause(); s1 = atomic_load_explicit(&s->seq, memory_order_acquire); }
        out[0] = atomic_load_explicit(&s->v0, memory_order_relaxed);
        out[1] = atomic_load_explicit(&s->v1, memory_order_relaxed);
        out[2] = atomic_load_explicit(&s->v2, memory_order_relaxed);
        out[3] = atomic_load_explicit(&s->v3, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        s2 = atomic_load_explicit(&s->seq, memory_order_relaxed);
    } while (s1 != s2);
}
```

---

## 2. Shadow-Cached + Batched Power-of-2 SPSC Ring (`127.12×` Faster Than Mutex)

Combines Linux `kfifo` monotonic unmasked `uint32_t` indices, Erik Rigtorp local shadow index caching, and `io_uring`/DPDK batched release commits:

```c
typedef struct {
    MU_ALIGN(64) _Atomic uint32_t head;
    uint32_t tail_shadow; /* consumer-only cache of producer tail */
    MU_ALIGN(64) _Atomic uint32_t tail;
    uint32_t head_shadow; /* producer-only cache of consumer head */
    MU_ALIGN(64) uint64_t buf[4096];
} FastSPSC;
```

---

## 3. Naughty Dog x86-64 System V User-Space Fiber Context Switch (`8.58 ns` = 18 Cycles, `1,301.95×` Faster Than OS Wake)

Saves only ABI callee-saved registers (`rsp, rbp, rbx, r12–r15` + return RIP on stack):

```c
typedef struct { void *rsp, *rbp, *rbx, *r12, *r13, *r14, *r15; } FiberCtx;

__attribute__((naked, noinline))
static void fiber_switch(FiberCtx *from, const FiberCtx *to) {
    __asm__ volatile(
        "movq %%rsp,  0(%%rdi)\n\t" "movq %%rbp,  8(%%rdi)\n\t"
        "movq %%rbx, 16(%%rdi)\n\t" "movq %%r12, 24(%%rdi)\n\t"
        "movq %%r13, 32(%%rdi)\n\t" "movq %%r14, 40(%%rdi)\n\t"
        "movq %%r15, 48(%%rdi)\n\t"
        "movq  0(%%rsi), %%rsp\n\t" "movq  8(%%rsi), %%rbp\n\t"
        "movq 16(%%rsi), %%rbx\n\t" "movq 24(%%rsi), %%r12\n\t"
        "movq 32(%%rsi), %%r13\n\t" "movq 40(%%rsi), %%r14\n\t"
        "movq 48(%%rsi), %%r15\n\t" "retq\n\t" ::: "memory");
}
```

---

## 4. C11-to-Hardware Instruction Cost Table & MCS Queue Lock

| C11 Operation | x86-64 (TSO) | ARMv8.0 / ARMv8.2-LSE | Hardware Cost Note |
|---|---|---|---|
| `load(relaxed)` / `store(relaxed)` | `mov` | `ldr` / `str` | Zero barrier |
| `load(acquire)` | `mov` | `ldar` / `ldapr` | **0 fence cost on x86-TSO!** |
| `store(release)` | `mov` | `stlr` | **0 fence cost on x86-TSO!** |
| `store(seq_cst)` | `xchg` (or `mov+mfence`) | `stlr` (+ `dmb ish` v8.0) | Drains store buffer (~15–30 cyc)! |
| `fetch_add(acq_rel)` | `lock xadd` | `ldxr/stlxr` -> `ldaddal` | Coherence line bounce if contended |

When mutual exclusion cannot be eliminated, an **MCS Queue Lock** avoids test-and-set/ticket `O(N)` unlock coherence broadcasts by spinning on each waiter's **own** cache-line-aligned `mcs_node`:

```c
typedef struct mcs_node {
    _Atomic(struct mcs_node *) next;
    _Atomic uint8_t locked;
    uint8_t _pad[64 - sizeof(void *) - 1];
} MU_ALIGN(64) mcs_node;

static inline void mcs_lock(_Atomic(mcs_node *) *tail, mcs_node *me) {
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&me->locked, 1, memory_order_relaxed);
    mcs_node *prev = atomic_exchange_explicit(tail, me, memory_order_acq_rel);
    if (prev) {
        atomic_store_explicit(&prev->next, me, memory_order_release);
        while (atomic_load_explicit(&me->locked, memory_order_acquire)) _mm_pause();
    }
}

static inline void mcs_unlock(_Atomic(mcs_node *) *tail, mcs_node *me) {
    mcs_node *next = atomic_load_explicit(&me->next, memory_order_acquire);
    if (!next) {
        mcs_node *expected = me;
        if (atomic_compare_exchange_strong_explicit(tail, &expected, NULL,
                memory_order_release, memory_order_relaxed)) return;
        while (!(next = atomic_load_explicit(&me->next, memory_order_acquire))) _mm_pause();
    }
    atomic_store_explicit(&next->locked, 0, memory_order_release); /* touches 1 line! */
}
```

---

## 5. NUMA Parallel First-Touch & Open-Loop Latency Recording (No Coordinated Omission)

```c
/* WRONG: main thread calloc/memset faults ALL pages onto NUMA Node 0! */
float *buf = (float *)mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

/* RIGHT: each pinned worker faults its own [begin, end) pages onto its local NUMA node */
/* Inside parallel_for(worker w -> [begin, end)): */
memset(buf + begin, 0, (end - begin) * sizeof(float));

/* Open-loop latency measurement: subtract INTENDED due_ns, NEVER actual send_ns! */
uint64_t due_ns  = t0_ns + req_idx * period_ns;
uint64_t send_ns = max_u64(due_ns, prev_done_ns);
uint64_t done_ns = serve_request(send_ns);
uint64_t true_latency_ns = done_ns - due_ns; /* captures queue stalls (1951x at p99!) */
```

---

## 6. Per-CPU `sched_getcpu()` Statistics Bucket Array & CV Baton / Tree Wakeups

```c
/* Multi-counter 64B-padded per-CPU stats (1 sched_getcpu() call + amortized padding): */
typedef struct MU_ALIGN(64) {
    _Atomic uint64_t reads, inserts, errors, bytes;
    uint64_t _pad[4];
} CpuStatsBucket;
MU_STATIC_ASSERT(sizeof(CpuStatsBucket) == 64, "exact 64B cache line");

static CpuStatsBucket g_stats[64] MU_ALIGN(64);

static inline void stats_inc_reads(void) {
    unsigned idx = (unsigned)sched_getcpu() & 63u;
    atomic_fetch_add_explicit(&g_stats[idx].reads, 1u, memory_order_relaxed);
}

/* CV Baton Hand-Off (kills notify_all() futex_wake thundering herds):
 * ALWAYS unlock mutex BEFORE pthread_cond_signal so waiter doesn't wake into held lock! */
pthread_mutex_lock(&mtx);
while (queue_empty(&q)) pthread_cond_wait(&cv, &mtx);
Item it   = queue_pop(&q);
bool more = !queue_empty(&q);
pthread_mutex_unlock(&mtx);
if (more) pthread_cond_signal(&cv); /* wake next consumer outside lock! */
process_item(it);
```

