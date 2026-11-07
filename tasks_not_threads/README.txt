TALKS STUDIED
=============================================================================
[1] Herb Sutter — "atomic<> Weapons: The C++ Memory Model and Modern Hardware"
    Part 1: https://www.youtube.com/watch?v=A8eCGOqgvH4  (C++ and Beyond 2012)
    Part 2: https://www.youtube.com/watch?v=KeLBd2EJLOU  (C++ and Beyond 2012)

WHAT THE TALKS ARE ABOUT
=============================================================================
Herb Sutter calls these talks "the stuff you really shouldn't have to know, but
will eventually need to". They cover the complete mental model for writing
correct concurrent code: from the C++11 (and C11) memory model down to what
real x86/ARM/POWER hardware actually does.

Both parts together run ~4 hours. Core theme: every high-performance concurrent
program that reaches for lock-free tools is pulling on "big red levers with
flashing warning lights". These talks are the warning labels.

=============================================================================
PART 1 — KEY LEARNINGS
=============================================================================

L1: DATA RACES = UNDEFINED BEHAVIOUR
--------------------------------------
A data race is: two threads access the same location concurrently, at least
one is a write, and there is no synchronisation between them.
Result: undefined behaviour. Not "might give wrong result" — truly undefined.
The compiler is allowed to assume data races do not exist and optimise
accordingly. Broken code on x86 may silently "work" — then break on ARM.

C proof: see 08_memory_model.c

L2: volatile IS NOT FOR THREADING
-----------------------------------
volatile tells the compiler: "re-read this from memory every time, don't cache
in a register". It says NOTHING about ordering, atomicity, or visibility across
threads. It was designed for memory-mapped I/O, not concurrency.

  BAD:  volatile int flag;       // NOT safe across threads
  GOOD: _Atomic int flag;        // C11 atomic — safe

C proof: see 08_memory_model.c (the volatile_broken section)

L3: C11/C++11 MEMORY MODEL — THREE LEVELS
------------------------------------------
Level 1 (safest, slowest):  mutex lock/unlock
Level 2 (middle):           _Atomic with memory_order_seq_cst (default)
Level 3 (fastest, hardest): _Atomic with acquire/release or relaxed ordering

Default atomics (seq_cst) are sequentially consistent — all threads agree on
a single global ordering of all atomic operations. This is safe and fast
enough for most code.

L4: ACQUIRE / RELEASE SEMANTICS
---------------------------------
Release-store: "Everything I wrote before this store is visible to whoever
               does the matching acquire-load."
Acquire-load:  "I will not see any of my OWN subsequent reads until after
               this load."

This is the correct primitive for passing ownership of data between threads.
It is cheaper than seq_cst on ARM/POWER because it doesn't require a full
memory fence — only a one-directional barrier.

C proof: see 06_ownership_transfer.c

L5: RELAXED ATOMICS — DON'T USE THEM
--------------------------------------
memory_order_relaxed gives you only atomicity (no torn reads), but zero
ordering guarantees. Other threads can observe your relaxed writes in any
order. Only safe use: incrementing a counter where the count itself is all
you care about (no other data depends on it).

  OK:  atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
  NOT OK: relaxed flag that guards access to other data

L6: x86 vs ARM — STRONGLY vs WEAKLY ORDERED
---------------------------------------------
x86 uses Total Store Order (TSO): stores are never reordered with other stores,
loads are never reordered with other loads. Many races "accidentally work" on
x86 but instantly break on ARM phones (weakly ordered).
Always write to the C11 memory model, not to "it works on my Intel laptop".

L7: FENCES — PREFER LOAD/STORE ANNOTATIONS OVER STANDALONE FENCES
-------------------------------------------------------------------
atomic_thread_fence(memory_order_seq_cst) is a standalone fence — it applies
to everything around it and is hard to reason about. Prefer annotating the
specific load or store that needs ordering with its memory_order argument.
Easier to review, easier to prove correct.

=============================================================================
PART 2 — KEY LEARNINGS
=============================================================================

L8: LOCKS ARE NOT SLOW — CONTENTION IS SLOW
--------------------------------------------
An uncontended mutex lock/unlock on Linux (futex) takes ~20 ns. Contended
locks where threads queue up are slow. Rule: if a lock is rarely contended,
a mutex is perfectly fine and far easier to reason about than lock-free code.

L9: DON'T MAKE DATA STRUCTURES INTERNALLY THREAD-SAFE
------------------------------------------------------
A queue that internally locks on push/pop has coarse-grained locking. It pays
the cost on every operation even when you know only one thread will ever push.
Better: make the data structure plain, and decide at the call site when to
synchronise (or design the system so you don't need to).

L10: THE RULE FOR LOCK-FREE CODE
----------------------------------
Lock-free = at least one thread always makes progress.
Wait-free  = every thread always makes progress in bounded steps.
Most "lock-free" structures are only lock-free in theory; in practice they
spin on CAS failures and become as bad as a mutex under contention.
Use lock-free only where you have a measured bottleneck.

L11: CAS (COMPARE-AND-SWAP) IS YOUR BUILDING BLOCK
----------------------------------------------------
All lock-free algorithms reduce to: "atomically read value, compute new value,
write new value only if it hasn't changed".
atomic_compare_exchange_strong / atomic_compare_exchange_weak

L12: ABA PROBLEM
-----------------
CAS checks that the value LOOKS the same, not that nothing happened.
Thread A reads ptr=X. Thread B pops X, pushes Y, then pushes X back.
Thread A's CAS succeeds even though the list changed. Use version tags or
hazard pointers for pointer-based lock-free structures.

=============================================================================
PRACTICAL TIPS — C CODE PROOF INDEX
=============================================================================

TIP 1: Use tasks not threads — run in a thread pool
     → 01_task_pool.c
     → Shows: simple work-stealing-style pool; threads wait on condition var;
       tasks are function pointers + data pushed to a queue.

TIP 2: One worker per REAL hardware core (not logical/HT)
     → 01_task_pool.c  (reads /proc/cpuinfo or uses sched_getaffinity)
     → On this machine: 4 real cores, 8 HT threads → use 4 workers.

TIP 3: Single owner per data — pass ownership, don't share
     → 06_ownership_transfer.c
     → Uses acquire/release to hand a buffer from producer to consumer
       with zero locks.

TIP 4: Atomic index into shared buffer (lock-free bump allocator)
     → 02_atomic_alloc.c
     → ptr = atomic_fetch_add(&offset, size) → your private slice.

TIP 5: Double-buffered event/message system
     → 03_double_buffer.c
     → Writers → buffer A; at frame end swap A↔B; readers consume B.
       No mutex needed anywhere in the hot path.

TIP 6: Thread-local write buffers (zero-contention writes)
     → 04_thread_local_buf.c
     → Each thread writes to its own _Thread_local buffer; merge at sync point.
       Compare throughput vs a shared atomic counter.

TIP 7: Cache-line alignment — avoid false sharing
     → 05_false_sharing.c
     → Benchmark: counter per thread in a packed array (slow, false sharing)
       vs aligned to 64 bytes (fast, private cache lines).

TIP 8: Task dependencies (ordering without locks)
     → 07_particle_parallel.c
     → Split 1 000 000 particles into N sub-tasks; a collector task depends on
       all of them; implemented with atomic countdown latch.

TIP 9: volatile vs _Atomic — proof that volatile is broken
     → 08_memory_model.c
     → Shows that volatile allows the compiler to reorder; _Atomic with
       acquire/release does not. Inspects generated assembly.

=============================================================================
GOLDEN RULES (one sentence each)
=============================================================================
1.  Threads are OS resources; tasks are units of work. Always pool threads.
2.  One worker per real core. Hyperthreads share an execution unit.
3.  Data has one owner at a time. Ownership is transferred, not shared.
4.  Atomic bump allocation is a lock-free slab from a shared buffer.
5.  Double-buffer messages to avoid mutex in the hot path.
6.  Thread-local write buffers give zero-contention writes.
7.  False sharing kills performance. Align per-thread data to cache lines.
8.  Dependencies beat mutexes. Let the scheduler enforce ordering.
9.  volatile is for hardware registers. _Atomic is for threading.
10. Default to seq_cst atomics. Use acquire/release only with measurements.
11. Relaxed atomics: only for independent counters.
12. Uncontended mutex ≈ 20 ns. Don't fear mutexes; fear contention.
13. Never make a data structure internally thread-safe. Decide at the call site.

=============================================================================
COMPILE ALL EXAMPLES
=============================================================================
  make all        — build everything
  make bench      — run benchmarks (false sharing, double buffer, particles)
  make clean      — remove binaries

Each file can also be compiled standalone:
  gcc -O2 -march=native -std=c11 -pthread <file>.c -o <file>
=============================================================================
