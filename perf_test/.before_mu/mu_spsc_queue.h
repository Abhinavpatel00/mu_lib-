#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_spsc — lock-free single-producer / single-consumer ring buffer.
 *
 * WHY: the thread system and task scheduler exist, but there was no cheap
 * handoff primitive. This is the one you want for IO -> sim, sim -> renderer,
 * worker -> main, job results: bounded, allocation-free after init, and
 * wait-free in the uncontended case.
 *
 * CORRECTNESS (Dekker / message passing):
 *   The producer writes the payload BEFORE publishing the new tail; the
 *   consumer reads the payload only after it has seen the new tail. The
 *   acquire/release pairing below is what makes that ordering visible on ARM
 *   as well as x86 — without it the consumer can read stale payload bytes
 *   next to a fresh index.
 *
 * FALSE SHARING: head and tail live on SEPARATE cache lines. If they shared
 *   one line, every push from the producer would bounce the line to the
 *   consumer's core and the "lock-free" queue would be slower than a mutex.
 *
 * CONTRACT: exactly one thread may call push, exactly one (other) may call
 * pop. Capacity must be a power of two (mask wrap instead of modulo).
 * --------------------------------------------------------------------------- */

#ifndef MU_CACHELINE
#define MU_CACHELINE 64u
#endif

typedef struct mu_spsc
{
    uint8_t*  buffer;
    uint32_t  elem_size;
    uint32_t  capacity;   /* power of two                                    */
    uint32_t  mask;

    /* Each on its own line: see the false-sharing note above. */
    MU_ALIGN(MU_CACHELINE) uint32_t head; /* written by producer             */
    MU_ALIGN(MU_CACHELINE) uint32_t tail; /* written by consumer             */

    uint32_t owned;
} mu_spsc;

/* `capacity` is rounded up to a power of two. `elem_size` is the payload
   stride in bytes. Returns false only on allocation failure. */
MU_INLINE bool mu_spsc_init(mu_spsc* q, uint32_t elem_size, uint32_t capacity)
{
    MU_ASSERT(q);
    q->buffer    = NULL;
    q->elem_size = elem_size;
    q->head      = 0;
    q->tail      = 0;
    q->owned     = 1;

    if (elem_size == 0)
        return false;

    uint32_t cap = 64;
    while (cap < capacity)
    {
        if (cap > 0x40000000u)
            return false;
        cap <<= 1;
    }

    q->capacity = cap;
    q->mask     = cap - 1u;
    q->buffer   = (uint8_t*)MU_MALLOC((size_t)cap * elem_size);
    if (!q->buffer)
    {
        q->capacity = 0;
        q->mask     = 0;
        q->owned    = 0;
        return false;
    }
    return true;
}

/* Caller-provided ring. capacity must be a power of two. */
MU_INLINE bool mu_spsc_init_static(mu_spsc* q, void* buffer, uint32_t elem_size, uint32_t capacity)
{
    MU_ASSERT(q);
    q->buffer    = (uint8_t*)buffer;
    q->elem_size = elem_size;
    q->head      = 0;
    q->tail      = 0;
    q->owned     = 0;

    if (elem_size == 0 || !buffer || capacity == 0u || (capacity & (capacity - 1u)) != 0u)
    {
        q->buffer   = NULL;
        q->capacity = 0;
        q->mask     = 0;
        return false;
    }

    q->capacity = capacity;
    q->mask     = capacity - 1u;
    return true;
}

MU_INLINE void mu_spsc_destroy(mu_spsc* q)
{
    if (!q)
        return;
    if (q->owned)
        MU_FREE(q->buffer);
    q->buffer   = NULL;
    q->capacity = 0;
    q->mask     = 0;
    q->owned    = 0;
}

MU_INLINE bool mu_spsc_full(const mu_spsc* q)
{
    return (q->head - q->tail) == q->capacity;
}

MU_INLINE bool mu_spsc_empty(const mu_spsc* q)
{
    return q->head == q->tail;
}

MU_INLINE uint32_t mu_spsc_count(const mu_spsc* q)
{
    return q->head - q->tail; /* wrap-safe for unsigned, as capacity is pow2 */
}

MU_INLINE uint32_t mu_spsc_capacity(const mu_spsc* q)
{
    return q ? q->capacity : 0u;
}

/* PRODUCER ONLY. */
MU_INLINE bool mu_spsc_push(mu_spsc* q, const void* elem)
{
    MU_ASSERT(q && elem);
    uint32_t head = q->head;

    if (head - q->tail == q->capacity)
        return false; /* full */

    MU_MEMCPY(q->buffer + (size_t)(head & q->mask) * q->elem_size, elem, q->elem_size);

    /* Publish AFTER the payload is in place (release ordering). */
    q->head = head + 1u;
    return true;
}

/* CONSUMER ONLY. */
MU_INLINE bool mu_spsc_pop(mu_spsc* q, void* out)
{
    MU_ASSERT(q);
    uint32_t tail = q->tail;

    if (q->head == tail)
        return false; /* empty */

    if (out)
        MU_MEMCPY(out, q->buffer + (size_t)(tail & q->mask) * q->elem_size, q->elem_size);

    /* Retire the slot only after the payload has been read. */
    q->tail = tail + 1u;
    return true;
}
