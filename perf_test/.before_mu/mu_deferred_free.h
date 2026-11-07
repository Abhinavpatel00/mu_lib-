#pragma once
#ifndef MU_DEFERRED_FREE_H
#define MU_DEFERRED_FREE_H

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_deferred_free -- retire objects N frames after they become unreachable.
 *
 * WHY
 * ---
 * Renderers, job systems and streaming code often cannot free an object the
 * moment the simulation drops it: a GPU submit, a worker or another frame may
 * still hold a raw pointer. A frame-delayed free queue turns "wait for safe
 * point" into a constant-time push and a once-per-frame drain.
 *
 * LAYOUT
 * ------
 * `frames` buckets, each holding up to `capacity` pointers:
 *
 *     cursor
 *       |
 *       v
 *     [ frame k ][ frame k+1 ][ ... ][ frame k+N-1 ]
 *       pushed      older...          freed next
 *
 * Pushes go to `cursor`. `mu_deferred_free_advance()` moves `cursor` forward
 * and frees the bucket it lands on, so a pointer pushed now survives exactly
 * `frames` advances. With `frames = 3`, an object freed on frame 0 is released
 * on frame 3.
 *
 * If a bucket overflows, `push` returns false and the caller decides whether
 * to drop the object immediately (or grow `capacity`).
 *
 * THREADING
 * ---------
 * This queue is not internally synchronised. Typical use is one producer
 * (simulation) and one consumer/drain point (frame boundary), or one lock
 * around it. Multi-producer handoff belongs in a queue such as `mu_spsc`.
 *
 * CONTRACT
 * --------
 *   - `free_fn == NULL` means `MU_FREE(ptr)`.
 *   - `free_fn` must accept pointers pushed by the caller.
 *   - `destroy()` drains every bucket, including the current one.
 * --------------------------------------------------------------------------- */

typedef void (*mu_deferred_free_fn)(void* ptr, void* user);

typedef struct mu_deferred_free
{
    void**             buckets;  /* frames * capacity pointers */
    uint32_t*          counts;   /* one count per frame        */
    uint32_t           frames;
    uint32_t           capacity;
    uint32_t           cursor;
    mu_deferred_free_fn free_fn;
    void*              user;
} mu_deferred_free;

MU_INLINE bool mu_deferred_free_init(mu_deferred_free* q, uint32_t frames,
                                     uint32_t capacity,
                                     mu_deferred_free_fn free_fn, void* user)
{
    size_t total;
    uint32_t i;

    if (!q || frames == 0 || capacity == 0)
        return false;

    if ((size_t)frames > (size_t)-1 / (size_t)capacity)
        return false;

    total = (size_t)frames * (size_t)capacity;

    q->buckets  = (void**)MU_CALLOC(total, sizeof(void*));
    q->counts   = (uint32_t*)MU_CALLOC(frames, sizeof(uint32_t));
    q->frames   = frames;
    q->capacity = capacity;
    q->cursor   = 0;
    q->free_fn  = free_fn;
    q->user     = user;

    if (!q->buckets || !q->counts)
    {
        MU_FREE(q->buckets);
        MU_FREE(q->counts);
        q->buckets  = NULL;
        q->counts   = NULL;
        q->frames   = 0;
        q->capacity = 0;
        return false;
    }

    for (i = 0; i < frames; ++i)
        q->counts[i] = 0;

    return true;
}

MU_INLINE void mu_deferred_free__release(mu_deferred_free* q, void* ptr)
{
    if (q->free_fn)
        q->free_fn(ptr, q->user);
    else
        MU_FREE(ptr);
}

MU_INLINE uint32_t mu_deferred_free_pending(const mu_deferred_free* q)
{
    uint32_t total = 0;
    uint32_t i;

    if (!q)
        return 0;

    for (i = 0; i < q->frames; ++i)
        total += q->counts[i];
    return total;
}

MU_INLINE bool mu_deferred_free_push(mu_deferred_free* q, void* ptr)
{
    uint32_t n;

    if (!q || !ptr || q->frames == 0)
        return false;

    n = q->counts[q->cursor];
    if (n >= q->capacity)
        return false;

    q->buckets[(size_t)q->cursor * q->capacity + n] = ptr;
    q->counts[q->cursor] = n + 1u;
    return true;
}

/* Advance one frame. Frees the bucket that just became current. */
MU_INLINE void mu_deferred_free_advance(mu_deferred_free* q)
{
    uint32_t n;
    uint32_t i;
    size_t base;

    if (!q || q->frames == 0)
        return;

    q->cursor = (q->cursor + 1u == q->frames) ? 0u : (q->cursor + 1u);

    n    = q->counts[q->cursor];
    base = (size_t)q->cursor * q->capacity;

    for (i = 0; i < n; ++i)
        mu_deferred_free__release(q, q->buckets[base + i]);

    q->counts[q->cursor] = 0;
}

/* Free everything now. Use at shutdown or when a hard safe point is known. */
MU_INLINE void mu_deferred_free_drain(mu_deferred_free* q)
{
    uint32_t frame;

    if (!q)
        return;

    for (frame = 0; frame < q->frames; ++frame)
    {
        uint32_t i;
        size_t base = (size_t)frame * q->capacity;

        for (i = 0; i < q->counts[frame]; ++i)
            mu_deferred_free__release(q, q->buckets[base + i]);

        q->counts[frame] = 0;
    }

    q->cursor = 0;
}

MU_INLINE void mu_deferred_free_destroy(mu_deferred_free* q)
{
    if (!q)
        return;

    mu_deferred_free_drain(q);
    MU_FREE(q->buckets);
    MU_FREE(q->counts);

    q->buckets  = NULL;
    q->counts   = NULL;
    q->frames   = 0;
    q->capacity = 0;
    q->cursor   = 0;
    q->free_fn  = NULL;
    q->user     = NULL;
}

#endif /* MU_DEFERRED_FREE_H */
