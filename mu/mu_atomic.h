#pragma once
#ifndef MU_ATOMIC_H
#define MU_ATOMIC_H

/*
 * mu_atomic.h -- the ONE atomic implementation for all of mylib.
 *
 * WHY one file: atomics are where "works on x86, breaks on ARM" bugs live.
 * Three copies (mu_ts_atomic*, mu_atomic_u32, mu_atomic32_t) each with their
 * own compiler-detection + ordering mistakes is three chances to get
 * acquire/release wrong. This header owns:
 *   - backend detection (MSVC Interlocked vs __atomic builtins)
 *   - canonical types: mu_atomic32_t / mu_atomic64_t / mu_atomicptr_t
 *   - load/store (relaxed + acquire/release), fetch_add, exchange,
 *     compare_exchange (bool form), fetch_max
 *   - cpu-relax (pause/yield) for spin loops
 * Everyone else (mu_sync.h spinlock/rwlock/seqlock, mu_thread_system.h,
 * mu_task_scheduler.h) includes this and adds NO new atomic primitives.
 *
 * MEMORY ORDERING RULE (the whole game):
 *   ACQUIRE on load  => nothing inside the critical section hoists above it.
 *   RELEASE on store => nothing inside the critical section sinks below it.
 * Pair them and the handoff is fenced without a full barrier on every op
 * (which is what a mutex costs). x86 gives acquire/release nearly for free;
 * ARM does not, hence explicit orders here instead of bare volatile.
 *
 * TYPES are small structs wrapping one volatile word, NOT bare integers:
 *   - threaded access must go through the functions below (never copy the
 *     struct by value and pretend both copies are "the" atomic),
 *   - .v stays reachable for init/debug.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "mu_macros.h"

/* --------------------------------------------------------------------------
 * Backend detection
 * -------------------------------------------------------------------------- */

#if defined(_MSC_VER)
#  define MU_ATOMIC_MSVC 1
#  include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#  define MU_ATOMIC_GNU 1
#else
#  define MU_ATOMIC_FALLBACK 1
#endif

/* --------------------------------------------------------------------------
 * Canonical types
 * -------------------------------------------------------------------------- */

typedef struct MU_ALIGN(4) mu_atomic32_t { volatile uint32_t v; } mu_atomic32_t;
typedef struct MU_ALIGN(8) mu_atomic64_t { volatile uint64_t v; } mu_atomic64_t;
typedef struct mu_atomicptr_t { void* volatile v; } mu_atomicptr_t;

MU_STATIC_ASSERT(sizeof(mu_atomic32_t) == 4, "mu_atomic32_size");
MU_STATIC_ASSERT(sizeof(mu_atomic64_t) == 8, "mu_atomic64_size");

MU_INLINE void mu_atomic32_init(mu_atomic32_t* a, uint32_t value) { a->v = value; }
MU_INLINE void mu_atomic64_init(mu_atomic64_t* a, uint64_t value) { a->v = value; }
MU_INLINE void mu_atomicptr_init(mu_atomicptr_t* a, void* value)  { a->v = value; }

/* --------------------------------------------------------------------------
 * CPU relax for spin loops: pause (x86) / yield (ARM), so a spinner neither
 * stalls its own pipeline nor starves a hyper-thread sibling holding the lock.
 * -------------------------------------------------------------------------- */

MU_INLINE void mu_cpu_relax(void)
{
#if defined(__GNUC__) || defined(__clang__)
#  if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#  elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#  else
    __asm__ __volatile__("" ::: "memory");
#  endif
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#else
    /* single-threaded fallback: nothing to yield to */
#endif
}

/* --------------------------------------------------------------------------
 * 32-bit atomics
 * -------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_atomic32_load_relaxed(const mu_atomic32_t* a)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_load_n(&a->v, __ATOMIC_RELAXED);
#else
    return a->v;
#endif
}

MU_INLINE uint32_t mu_atomic32_load_acquire(const mu_atomic32_t* a)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
#elif defined(MU_ATOMIC_MSVC)
    uint32_t v = a->v;
    _ReadBarrier();
    return v;
#else
    return a->v;
#endif
}

MU_INLINE void mu_atomic32_store_relaxed(mu_atomic32_t* a, uint32_t value)
{
#if defined(MU_ATOMIC_GNU)
    __atomic_store_n(&a->v, value, __ATOMIC_RELAXED);
#else
    a->v = value;
#endif
}

MU_INLINE void mu_atomic32_store_release(mu_atomic32_t* a, uint32_t value)
{
#if defined(MU_ATOMIC_GNU)
    __atomic_store_n(&a->v, value, __ATOMIC_RELEASE);
#elif defined(MU_ATOMIC_MSVC)
    _WriteBarrier();
    a->v = value;
#else
    a->v = value;
#endif
}

/* Returns the PREVIOUS value. */
MU_INLINE uint32_t mu_atomic32_fetch_add(mu_atomic32_t* a, uint32_t value)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_fetch_add(&a->v, value, __ATOMIC_ACQ_REL);
#elif defined(MU_ATOMIC_MSVC)
    return (uint32_t)_InterlockedExchangeAdd((volatile long*)&a->v, (long)value);
#else
    uint32_t old = a->v;
    a->v = old + value;
    return old;
#endif
}

/* Returns the PREVIOUS value. */
MU_INLINE uint32_t mu_atomic32_exchange(mu_atomic32_t* a, uint32_t value)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_exchange_n(&a->v, value, __ATOMIC_ACQ_REL);
#elif defined(MU_ATOMIC_MSVC)
    return (uint32_t)_InterlockedExchange((volatile long*)&a->v, (long)value);
#else
    uint32_t old = a->v;
    a->v = value;
    return old;
#endif
}

/*
 * Strong CAS, bool form: on success stores desired and returns true; on
 * failure refreshes *expected and returns false. (Bool form, not "returns
 * previous", so callers cannot mistake which value they got -- that
 * confusion is the classic CAS bug.)
 */
MU_INLINE bool mu_atomic32_compare_exchange(mu_atomic32_t* a, uint32_t* expected, uint32_t desired)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_compare_exchange_n(&a->v, expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#elif defined(MU_ATOMIC_MSVC)
    uint32_t prev = (uint32_t)_InterlockedCompareExchange((volatile long*)&a->v,
                                                          (long)desired, (long)*expected);
    if (prev == *expected)
        return true;
    *expected = prev;
    return false;
#else
    if (a->v == *expected)
    {
        a->v = desired;
        return true;
    }
    *expected = a->v;
    return false;
#endif
}

/* Sets *dst = max(*dst, value). Returns the previous value. */
MU_INLINE uint32_t mu_atomic32_fetch_max(mu_atomic32_t* dst, uint32_t value)
{
    uint32_t cur = mu_atomic32_load_relaxed(dst);
    for (;;)
    {
        if (cur >= value)
            return cur;
        uint32_t expected = cur;
        if (mu_atomic32_compare_exchange(dst, &expected, value))
            return cur;
        cur = expected;
    }
}

/* --------------------------------------------------------------------------
 * 64-bit atomics
 * -------------------------------------------------------------------------- */

MU_INLINE uint64_t mu_atomic64_load_relaxed(const mu_atomic64_t* a)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_load_n(&a->v, __ATOMIC_RELAXED);
#else
    return a->v;
#endif
}

MU_INLINE uint64_t mu_atomic64_load_acquire(const mu_atomic64_t* a)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
#elif defined(MU_ATOMIC_MSVC)
    uint64_t v = a->v;
    _ReadBarrier();
    return v;
#else
    return a->v;
#endif
}

MU_INLINE void mu_atomic64_store_relaxed(mu_atomic64_t* a, uint64_t value)
{
#if defined(MU_ATOMIC_GNU)
    __atomic_store_n(&a->v, value, __ATOMIC_RELAXED);
#else
    a->v = value;
#endif
}

MU_INLINE void mu_atomic64_store_release(mu_atomic64_t* a, uint64_t value)
{
#if defined(MU_ATOMIC_GNU)
    __atomic_store_n(&a->v, value, __ATOMIC_RELEASE);
#elif defined(MU_ATOMIC_MSVC)
    _WriteBarrier();
    a->v = value;
#else
    a->v = value;
#endif
}

/* Returns the PREVIOUS value. */
MU_INLINE uint64_t mu_atomic64_fetch_add(mu_atomic64_t* a, uint64_t value)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_fetch_add(&a->v, value, __ATOMIC_ACQ_REL);
#elif defined(MU_ATOMIC_MSVC)
    return (uint64_t)_InterlockedExchangeAdd64((volatile long long*)&a->v, (long long)value);
#else
    uint64_t old = a->v;
    a->v = old + value;
    return old;
#endif
}

/* Returns the PREVIOUS value. */
MU_INLINE uint64_t mu_atomic64_exchange(mu_atomic64_t* a, uint64_t value)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_exchange_n(&a->v, value, __ATOMIC_ACQ_REL);
#elif defined(MU_ATOMIC_MSVC)
    return (uint64_t)_InterlockedExchange64((volatile long long*)&a->v, (long long)value);
#else
    uint64_t old = a->v;
    a->v = value;
    return old;
#endif
}

MU_INLINE bool mu_atomic64_compare_exchange(mu_atomic64_t* a, uint64_t* expected, uint64_t desired)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_compare_exchange_n(&a->v, expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#elif defined(MU_ATOMIC_MSVC)
    uint64_t prev = (uint64_t)_InterlockedCompareExchange64((volatile long long*)&a->v,
                                                            (long long)desired, (long long)*expected);
    if (prev == *expected)
        return true;
    *expected = prev;
    return false;
#else
    if (a->v == *expected)
    {
        a->v = desired;
        return true;
    }
    *expected = a->v;
    return false;
#endif
}

/* Sets *dst = max(*dst, value). Returns the previous value. */
MU_INLINE uint64_t mu_atomic64_fetch_max(mu_atomic64_t* dst, uint64_t value)
{
    uint64_t cur = mu_atomic64_load_relaxed(dst);
    for (;;)
    {
        if (cur >= value)
            return cur;
        uint64_t expected = cur;
        if (mu_atomic64_compare_exchange(dst, &expected, value))
            return cur;
        cur = expected;
    }
}

/* --------------------------------------------------------------------------
 * Pointer atomics (the POINTER is atomic, not the pointee)
 * -------------------------------------------------------------------------- */

MU_INLINE void* mu_atomicptr_load_relaxed(const mu_atomicptr_t* a)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_load_n(&a->v, __ATOMIC_RELAXED);
#else
    return (void*)a->v;
#endif
}

MU_INLINE void* mu_atomicptr_load_acquire(const mu_atomicptr_t* a)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
#elif defined(MU_ATOMIC_MSVC)
    void* v = (void*)a->v;
    _ReadBarrier();
    return v;
#else
    return (void*)a->v;
#endif
}

MU_INLINE void mu_atomicptr_store_relaxed(mu_atomicptr_t* a, void* value)
{
#if defined(MU_ATOMIC_GNU)
    __atomic_store_n(&a->v, value, __ATOMIC_RELAXED);
#else
    a->v = value;
#endif
}

MU_INLINE void mu_atomicptr_store_release(mu_atomicptr_t* a, void* value)
{
#if defined(MU_ATOMIC_GNU)
    __atomic_store_n(&a->v, value, __ATOMIC_RELEASE);
#elif defined(MU_ATOMIC_MSVC)
    _WriteBarrier();
    a->v = value;
#else
    a->v = value;
#endif
}

MU_INLINE void* mu_atomicptr_exchange(mu_atomicptr_t* a, void* value)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_exchange_n(&a->v, value, __ATOMIC_ACQ_REL);
#elif defined(MU_ATOMIC_MSVC)
#  if defined(_WIN64)
    return (void*)_InterlockedExchangePointer((void* volatile*)&a->v, value);
#  else
    return (void*)(uintptr_t)_InterlockedExchange((volatile long*)&a->v, (long)(uintptr_t)value);
#  endif
#else
    void* old = (void*)a->v;
    a->v = value;
    return old;
#endif
}

MU_INLINE bool mu_atomicptr_compare_exchange(mu_atomicptr_t* a, void** expected, void* desired)
{
#if defined(MU_ATOMIC_GNU)
    return __atomic_compare_exchange_n(&a->v, expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#elif defined(MU_ATOMIC_MSVC)
#  if defined(_WIN64)
    void* prev = _InterlockedCompareExchangePointer((void* volatile*)&a->v, desired, *expected);
#  else
    void* prev = (void*)(uintptr_t)_InterlockedCompareExchange(
        (volatile long*)&a->v, (long)(uintptr_t)desired, (long)(uintptr_t)*expected);
#  endif
    if (prev == *expected)
        return true;
    *expected = prev;
    return false;
#else
    if (a->v == *expected)
    {
        a->v = desired;
        return true;
    }
    *expected = (void*)a->v;
    return false;
#endif
}

#endif /* MU_ATOMIC_H */
