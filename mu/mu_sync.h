#pragma once

#include "mu_common.h"
#include "mu_atomic.h"

/* ---------------------------------------------------------------------------
 * mu_sync -- spinlock, rwlock, seqlock.
 *
 * WHY: mu_thread_system / mu_task_scheduler own threads, but there was no
 * home for the primitives those threads synchronise with. One header, one
 * naming scheme, one set of memory orderings.
 *
 * ATOMICS LIVE IN mu_atomic.h -- this file defines NO atomic primitives of
 * its own. It only composes them into locks (this is the "few possibilities
 * of bugs" rule: exactly one backend to audit).
 *
 * MEMORY ORDERING RULE (this is the whole game):
 *   ACQUIRE on load  => no memory access inside the critical section may be
 *                       hoisted ABOVE the load.
 *   RELEASE on store  => no memory access inside the critical section may be
 *                       sunk BELOW the store.
 *   Pair them and the critical section is fenced against the rest of the
 *   world without a full barrier on every operation (which is what a mutex
 *   costs you). x86 gives you this for free; ARM does not, hence explicit
 *   orders rather than relaxed.
 *
 * CHOICES:
 *   spinlock   -- uncontended lock/unlock is one atomic RMW; correct for
 *                short critical sections. Always pairs its wait with a CPU
 *                pause so a spinning thread does not stall the pipeline or
 *                starve the core it shares with the holder.
 *   rwlock     -- many readers, exclusive writer. Optimistic reader count, so
 *                an uncontended read is one atomic add + one sub.
 *   seqlock    -- for lock-free READ of plain (non-atomic) data: the writer
 *                bumps an odd counter around the update, readers retry if
 *                they saw an odd or changed counter. Reads scale to any
 *                number of cores and cost zero writes.
 *
 * CONTRACT: not recursive (a lock you already hold will deadlock you).
 * --------------------------------------------------------------------------- */

/* mu_atomic.h owns cpu-relax; keep the old name working. */
#ifndef MU_CPU_RELAX
#  define MU_CPU_RELAX() mu_cpu_relax()
#endif

/* Thin aliases so existing mu_sync users keep compiling. */
typedef mu_atomic32_t  mu_atomic_u32;
typedef mu_atomicptr_t mu_atomic_ptr;

MU_INLINE void mu_atomic_store_u32(mu_atomic_u32* a, uint32_t value)
{
    mu_atomic32_store_release(a, value);
}

MU_INLINE uint32_t mu_atomic_load_u32(const mu_atomic_u32* a)
{
    return mu_atomic32_load_acquire(a);
}

/* Compare-and-swap. Returns true if the stored value was `expected`. */
MU_INLINE bool mu_atomic_cas_u32(mu_atomic_u32* a, uint32_t* expected, uint32_t desired)
{
    return mu_atomic32_compare_exchange(a, expected, desired);
}

/* Atomic read-modify-write: returns the PREVIOUS value. */
MU_INLINE uint32_t mu_atomic_fetch_add_u32(mu_atomic_u32* a, uint32_t value)
{
    return mu_atomic32_fetch_add(a, value);
}

MU_INLINE uint32_t mu_atomic_exchange_u32(mu_atomic_u32* a, uint32_t value)
{
    return mu_atomic32_exchange(a, value);
}

MU_INLINE void mu_atomic_store_ptr(mu_atomic_ptr* a, void* value)
{
    mu_atomicptr_store_release(a, value);
}

MU_INLINE void* mu_atomic_load_ptr(const mu_atomic_ptr* a)
{
    return mu_atomicptr_load_acquire(a);
}

MU_INLINE void mu_atomic_init_u32(mu_atomic_u32* a, uint32_t value)
{
    mu_atomic32_init(a, value);
}

/* -------------------------------------------------------------------------- *
 * Spinlock — one word, pause-spinning, no syscalls.
 * -------------------------------------------------------------------------- */

typedef struct mu_spinlock
{
    mu_atomic_u32 state; /* 0 = free, 1 = held */
} mu_spinlock;

MU_INLINE void mu_spinlock_init(mu_spinlock* s)
{
    mu_atomic_init_u32(&s->state, 0);
}

MU_INLINE void mu_spinlock_lock(mu_spinlock* s)
{
    for (;;)
    {
        /* Fast path: try to grab the free lock with a single RMW. */
        uint32_t expected = 0;
        if (mu_atomic_cas_u32(&s->state, &expected, 1))
            return;

        /* Contended: read-spin instead of hammering the cache line with
           CAS. The line is in nobody's exclusive state while we spin, so
           this costs far less coherence traffic. */
        while (mu_atomic_load_u32(&s->state) != 0)
            MU_CPU_RELAX();
    }
}

MU_INLINE bool mu_spinlock_try_lock(mu_spinlock* s)
{
    uint32_t expected = 0;
    return mu_atomic_cas_u32(&s->state, &expected, 1);
}

MU_INLINE void mu_spinlock_unlock(mu_spinlock* s)
{
    mu_atomic_store_u32(&s->state, 0);
}

/* -------------------------------------------------------------------------- *
 * Reader/writer lock — shared reads, exclusive writes.
 *
 * Layout: low 31 bits = active readers, high bit = writer pending/held.
 * A writer sets the flag, then waits for readers to drain; readers that see
 * the flag wait for it to clear. Readers never block each other.
 * -------------------------------------------------------------------------- */

#define MU_RWLOCK_WRITER 0x80000000u

typedef struct mu_rwlock
{
    mu_atomic_u32 state;
} mu_rwlock;

MU_INLINE void mu_rwlock_init(mu_rwlock* l)
{
    mu_atomic_init_u32(&l->state, 0);
}

MU_INLINE void mu_rwlock_lock_read(mu_rwlock* l)
{
    for (;;)
    {
        uint32_t s = mu_atomic_load_u32(&l->state);
        while (s & MU_RWLOCK_WRITER)
        {
            MU_CPU_RELAX();
            s = mu_atomic_load_u32(&l->state);
        }
        uint32_t expected = s;
        if (mu_atomic_cas_u32(&l->state, &expected, s + 1u))
            return;
    }
}

MU_INLINE void mu_rwlock_unlock_read(mu_rwlock* l)
{
    mu_atomic_fetch_add_u32(&l->state, (uint32_t)-1);
}

MU_INLINE void mu_rwlock_lock_write(mu_rwlock* l)
{
    for (;;)
    {
        uint32_t expected = 0;
        if (mu_atomic_cas_u32(&l->state, &expected, MU_RWLOCK_WRITER))
            break;
        MU_CPU_RELAX();
    }

    /* Now wait for in-flight readers to leave. */
    while ((mu_atomic_load_u32(&l->state) & ~MU_RWLOCK_WRITER) != 0)
        MU_CPU_RELAX();
}

MU_INLINE void mu_rwlock_unlock_write(mu_rwlock* l)
{
    mu_atomic_store_u32(&l->state, 0);
}

MU_INLINE bool mu_rwlock_try_lock_write(mu_rwlock* l)
{
    uint32_t expected = 0;
    return mu_atomic_cas_u32(&l->state, &expected, MU_RWLOCK_WRITER);
}

/* -------------------------------------------------------------------------- *
 * Seqlock — lock-free reads of plain data.
 *
 * The writer brackets its update with seq = 2k -> 2k+1 -> 2k+2.
 * Readers snapshot seq, read the data WITHOUT locking, then re-check seq:
 * if it changed (or is odd) the read overlapped a write and they retry.
 *
 * Best for a handful of scalars published at ~frame rate and read by many
 * threads: zero writes on the read side, so it scales perfectly, unlike a
 * rwlock where every reader does a CAS on one shared line.
 * -------------------------------------------------------------------------- */

typedef struct mu_seqlock
{
    mu_atomic_u32 seq;
} mu_seqlock;

MU_INLINE void mu_seqlock_init(mu_seqlock* l)
{
    mu_atomic_init_u32(&l->seq, 0);
}

/* Writer side. Bracket the update: */
MU_INLINE void mu_seqlock_write_begin(mu_seqlock* l)
{
    uint32_t s = mu_atomic_load_u32(&l->seq);
    mu_atomic_store_u32(&l->seq, s + 1u); /* becomes odd: readers spin/retry */
}

MU_INLINE void mu_seqlock_write_end(mu_seqlock* l)
{
    uint32_t s = mu_atomic_load_u32(&l->seq);
    mu_atomic_store_u32(&l->seq, s + 1u); /* even again: update visible */
}

MU_INLINE uint32_t mu_seqlock_read_begin(const mu_seqlock* l)
{
    uint32_t s = mu_atomic_load_u32(&l->seq);
    while (s & 1u)
    {
        MU_CPU_RELAX();
        s = mu_atomic_load_u32(&l->seq);
    }
    return s;
}

/* Returns false if a write overlapped the read (retry then). */
MU_INLINE bool mu_seqlock_read_end(const mu_seqlock* l, uint32_t snapshot)
{
    return mu_atomic_load_u32(&l->seq) == snapshot;
}
