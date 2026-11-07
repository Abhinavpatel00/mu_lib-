---
name: multithreading-dod
description: >
  Design and optimize multi-threaded, lock-free, kernel-style, and fiber-based
  C systems SEPARATELY from single-core logic: MESI false-sharing elimination,
  C11 acquire/release memory ordering, Linux kernel seqlock, RCU/epoch deferred
  reclamation, shadow-cached & batched SPSC/MPMC rings, SLUB/Mesa per-thread
  lockless slab pools, Chase-Lev work-stealing deques, and Naughty Dog M:N
  user-space fiber job systems. Load ONLY when the task involves threads, jobs,
  atomics, lock-free queues, fibers, or scaling an already DOD/SIMD-optimized
  single-core pipeline across physical CPU cores.
---

# Skill: Data-Oriented Multithreading, Lock-Free & Fibers in C (Separated Module)

> **Prime Separation Rule (`AGENTS.md` Part II):**
> NEVER use threads to compensate for bloated AoS structs, pointer chasing, or scalar branches. Optimize single-core layout and SIMD **first** (`dod-performance` + `simd-hardware-c`). Keep single-threaded transform kernels 100% pure (`void transform(T * restrict out, const T * restrict in, size_t n)`) and compose multithreading in a **separate scheduling/dispatch layer** over disjoint `[begin, end)` slices!

---

## 1. Decision Matrix (Proven in `perf_test/bench_multithreading_separate.c`)

| Concurrency Problem | Anti-Pattern | Sanctioned DOD / Kernel Solution | Measured Gain |
|---|---|---|---|
| Parallel reduction / stats | Shared `_Atomic` (`lock add` w/ ignored flags) | Local batching (`<=2` sites) -> `sched_getcpu()&63` 64B bucket array -> TLS | **71.76×** (`4.20×` padded) |
| Read-mostly small POD (`<= 64B`) | Mutex (`osq_lock`) / `pthread_rwlock_t` (`lock xadd`!) | `rwlock` (`1.8–14.6×` w/ double-check) -> **Linux `seqlock` (`mu_sync.h`, 0 reader writes!)** | **27.44×** |
| Read-mostly large table / config | Mutex around every lookup | **RCU Acquire Pointer Load + Epoch/Frame Deferred Free (`mu_deferred_free.h`)** | Near-zero read overhead |
| Spinlock under contention | Bare TAS `while (!cmpxchg)` (`O(N²)` RFO) | **Try-First TTAS + 64B lock/data pad** -> **MCS Queue Lock** at HCC | Eliminates RFO storm |
| Worker pool wakeup at scale | `notify_all()` / `notify_one` loop (`futex_wake` herd!) | **Precise `min(T, J)` wake / `O(√N)` tree wake / post-unlock baton / `sem_wait`** | Kills `N×5µs` wakeup storm |
| 1-Producer -> 1-Consumer pipe | `pthread_mutex_t` queue | **Shadow-Index Cached + Batched Power-of-2 SPSC (`mu_spsc_queue.h`, `kfifo`/`io_uring`)** | **127.12×** |
| Multi-threaded fixed-size alloc | Shared mutex freelist / heap churn | **Per-Worker Arena** (transient) or **SLUB / Mesa Per-Thread Child Slab (`mu_slab.h`)** | **932.84×** |
| Nested job wait / dependency | OS `pthread_cond_wait` / `futex` (~11 µs) | **Naughty Dog 64 KiB User-Space Fiber Switch (`8.58 ns` = 18 cycles) + Atomic Wait Counter** | **1,301.95×** |

---

## 2. Operating Procedure

### Step 1 — Partition Data Ownership First ("Don't Share")
1. Size worker threads to **physical CPU cores** (avoid running heavy AVX2/memory-bound workers on sibling HyperThreads sharing the same 48 KB L1d cache and store buffer).
2. Split arrays into disjoint slices aligned to **64-byte cache-line boundaries** (`grain >= 1024` elements, `>= 25 µs` work per job).
3. For variable-count outputs (parallel filter/cull), use **2-pass prefix sum**:
   - Pass 1: Each worker counts survivors into local register `count[w]`.
   - Sync: Compute exclusive prefix sum `offset[w]`.
   - Pass 2: Each worker writes contiguously into `out + offset[w]` (zero atomic contention!).

### Step 2 — Enforce MESI Cache-Line & C11 Memory-Order Hygiene
1. Pad every per-worker mutable struct, atomic cursor, or deque bottom/top to `MU_ALIGN(64)` (or `128B` when adjacent-line spatial prefetch is hot).
2. Never place read-only fields (`capacity`, `mask`, `buf_ptr`) on the same 64B cache line as an `_Atomic` index or spinlock!
3. Use explicit `<stdatomic.h>` memory orders:
   - `memory_order_relaxed` for independent stats/IDs.
   - `memory_order_acquire` / `memory_order_release` for queue handoffs, seqlocks, and pointer publication (compiles to plain `mov` on x86-64 TSO — zero `mfence`!).
   - Reserve `memory_order_seq_cst` strictly for Dekker-style Chase-Lev last-element steal races.

### Step 3 — Use `mylib` Concurrency Primitives
- `mu_task_scheduler.h` (`mu_sched_parallel_for`, task priorities, completion counters)
- `mu_thread_system.h` (worker pool + atomics)
- `mu_spsc_queue.h` / `mu_mpmc_queue.h` (bounded lock-free queues)
- `mu_sync.h` (`mu_seqlock`, lightweight spin/Parker primitives)
- `mu_deferred_free.h` (epoch-based safe memory reclamation)
- `mu_slab.h` (per-thread fast-path + remote-free slab allocator)
- See `tasks_not_threads/` (`01_task_pool.c` .. `08_memory_model.c`) and `references/lockfree-fibers-kernel.md` for complete reference patterns.

### Step 4 — Apply Scaling Laws (`USL`), NUMA First-Touch, `resctrl` & Tail-Latency Rules
1. **Check Gunther's USL & Kingman's Tail Law:** $C(N) = \frac{N}{1 + \alpha(N-1) + \beta N(N-1)}$ — pairwise coherency $\beta N(N-1)$ causes **retrograde throughput loss** past $N_{\max} = \lfloor\sqrt{(1-\alpha)/\beta}\rfloor$! Diagnose false sharing with `perf c2c` (`HITM`). Keep utilization $\rho$ below saturation and crush service jitter $c_s^2$ (Kingman's formula).
2. **NUMA First-Touch Placement (`Allocation != Placement`):** Touch/zero large `mmap`/`malloc` arrays **inside `parallel_for`** with the exact same static slice map as compute so minor page faults (`~670 ns/page`) place pages on each worker's local NUMA node (`numastat` `numa_hit`; watch Xeon `SNC` / EPYC `NPS` modes and single-controller congestion).
3. **Kernel Boundary, `resctrl` & Zero TLB Shootdowns:** Keep clocks on `vDSO`, batch I/O via `io_uring`/polled rings, partition shared L3 ways & memory bandwidth via `/sys/fs/resctrl` (RDT/QoS/MPAM), avoid `munmap`/`MADV_DONTNEED` cross-core **TLB shootdown IPIs** on hot paths, and use user-space fibers across `1–100 µs` killer microseconds.
4. **Defeat Coordinated Omission:** Benchmark latency open-loop from **scheduled `due_time`** (`wrk2` style) and record `p50, p99, p99.9, max` in `HdrHistogram` (closed-loop generators hide stalls by `1,000×+`!).
5. **Triage Core-Count Scaling Walls (`Flow D` Jumper Table, `perf c2c --double-cl` & CV Herds):**
   - **Dual-profile jumpers:** Sweep `1, 2, nproc/4, nproc/2, nproc` (throughput separate from `perf record`!) to find where marginal scaling drops `< 0.80/core`. Compare top-15 `--no-children` functions at 1 core (`taskset -c 0 perf record -F 99 -g -- cmd` — omit `-a`!) vs N cores (or **N/2 vs N** when `>7` functions jump). Flag jumpers: Rank `▲>=3`, `% >= 2×`, `+3%`, or new `>= 2%`.
   - **`perf c2c --double-cl` offset traps:** Discriminate via cache-line byte **`Offset`**, watching: (a) `slots[tid].done` same field name at different offsets = **false sharing**; (b) 2 hot global counters on 1 line = **both true & false sharing**; (c) `0x00..0x3f` vs `0x40..0x7f` = adjacent streamer pair.
   - **CV Thundering Herd vs Lock Contention:** `futex_wake` / `try_to_wake_up` + normal `perf lock stat` wait + huge `context-switches` = thundering herd (invisible to `lock_stat`!); fix with precise `min(T, J)` wakeups, `O(√N)` tree wakeup, post-unlock `notify_one()` baton hand-off, or `sem_wait`.
