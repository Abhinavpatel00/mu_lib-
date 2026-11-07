#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_indexed_heap — binary heap keyed by handle (decrease-key capable).
 *
 * WHY: a plain heap cannot lower the priority of an entry you already pushed.
 * A* re-opens a node, the task scheduler reprioritises a job, a work-stealing
 * system changes its mind. This keeps three flat arrays:
 *
 *     handles[i]  -> handle stored at heap slot i
 *     keys[i]     -> its priority   (LOWER = more urgent = nearer the root)
 *     positions[h]-> the heap slot handle h currently sits in (or NONE)
 *
 * so any handle is movable in O(log n) and testable in O(1).
 * Everything is uint32: no pointer chasing, one cache line covers ~16 slots.
 *
 * CONTRACT:
 *   - handles must be < capacity (the position map is indexed by handle).
 *   - Lower key == higher priority (min-heap).
 *   - Not thread safe. No allocation after init.
 * --------------------------------------------------------------------------- */

#define MU_INDEXED_HEAP_NONE 0xffffffffu

typedef struct mu_indexed_heap
{
    uint32_t* handles;   /* slot -> handle                                     */
    uint32_t* keys;      /* slot -> priority                                   */
    uint32_t* positions; /* handle -> slot, or MU_INDEXED_HEAP_NONE            */
    uint32_t  count;
    uint32_t  capacity;
    uint32_t  owned;     /* 1 = heap buffers we may free                       */
} mu_indexed_heap;

MU_INLINE bool mu_indexed_heap_init(mu_indexed_heap* h, uint32_t capacity)
{
    MU_ASSERT(h);
    h->handles   = NULL;
    h->keys      = NULL;
    h->positions = NULL;
    h->count     = 0;
    h->capacity  = capacity;
    h->owned     = 1;

    if (capacity == 0)
        return true;

    h->handles   = (uint32_t*)MU_MALLOC((size_t)capacity * sizeof(uint32_t));
    h->keys      = (uint32_t*)MU_MALLOC((size_t)capacity * sizeof(uint32_t));
    h->positions = (uint32_t*)MU_MALLOC((size_t)capacity * sizeof(uint32_t));
    if (!h->handles || !h->keys || !h->positions)
    {
        MU_FREE(h->handles);
        MU_FREE(h->keys);
        MU_FREE(h->positions);
        h->handles   = NULL;
        h->keys      = NULL;
        h->positions = NULL;
        h->count     = 0;
        h->owned     = 0;
        return false;
    }

    for (uint32_t i = 0; i < capacity; ++i)
        h->positions[i] = MU_INDEXED_HEAP_NONE;
    return true;
}

/* Caller-provided arrays: handles[], keys[], positions[] of `capacity`. */
MU_INLINE bool mu_indexed_heap_init_static(
    mu_indexed_heap* h, uint32_t* handles, uint32_t* keys, uint32_t* positions, uint32_t capacity)
{
    MU_ASSERT(h);
    h->handles   = handles;
    h->keys      = keys;
    h->positions = positions;
    h->count     = 0;
    h->capacity  = capacity;
    h->owned     = 0;

    if (capacity == 0)
        return true;
    if (!handles || !keys || !positions)
    {
        h->count    = 0;
        h->capacity = 0;
        return false;
    }

    for (uint32_t i = 0; i < capacity; ++i)
        positions[i] = MU_INDEXED_HEAP_NONE;
    return true;
}

MU_INLINE void mu_indexed_heap_destroy(mu_indexed_heap* h)
{
    if (!h)
        return;
    if (h->owned)
    {
        MU_FREE(h->handles);
        MU_FREE(h->keys);
        MU_FREE(h->positions);
    }
    h->handles   = NULL;
    h->keys      = NULL;
    h->positions = NULL;
    h->count     = 0;
    h->capacity  = 0;
    h->owned     = 0;
}

MU_INLINE void mu_indexed_heap_clear(mu_indexed_heap* h)
{
    MU_ASSERT(h);
    h->count = 0;
    for (uint32_t i = 0; i < h->capacity; ++i)
        h->positions[i] = MU_INDEXED_HEAP_NONE;
}

/* ------------------------------------------------------------------ */

MU_INLINE void mu_indexed_heap__swap(mu_indexed_heap* h, uint32_t a, uint32_t b)
{
    if (a == b)
        return;

    uint32_t ha = h->handles[a];
    uint32_t hb = h->handles[b];
    uint32_t ka = h->keys[a];
    uint32_t kb = h->keys[b];

    h->handles[a] = hb;
    h->handles[b] = ha;
    h->keys[a]    = kb;
    h->keys[b]    = ka;

    h->positions[ha] = b;
    h->positions[hb] = a;
}

MU_INLINE void mu_indexed_heap__sift_up(mu_indexed_heap* h, uint32_t i)
{
    while (i > 0)
    {
        uint32_t parent = (i - 1u) >> 1;
        if (h->keys[i] >= h->keys[parent])
            break;
        mu_indexed_heap__swap(h, i, parent);
        i = parent;
    }
}

MU_INLINE void mu_indexed_heap__sift_down(mu_indexed_heap* h, uint32_t i)
{
    for (;;)
    {
        uint32_t left  = (i << 1) + 1u;
        uint32_t right = left + 1u;
        uint32_t best  = i;

        if (left < h->count && h->keys[left] < h->keys[best])
            best = left;
        if (right < h->count && h->keys[right] < h->keys[best])
            best = right;
        if (best == i)
            break;

        mu_indexed_heap__swap(h, i, best);
        i = best;
    }
}

MU_INLINE bool mu_indexed_heap_contains(const mu_indexed_heap* h, uint32_t handle)
{
    if (!h || handle >= h->capacity)
        return false;
    return h->positions[handle] != MU_INDEXED_HEAP_NONE;
}

MU_INLINE uint32_t mu_indexed_heap_priority(const mu_indexed_heap* h, uint32_t handle)
{
    if (!h || handle >= h->capacity || h->positions[handle] == MU_INDEXED_HEAP_NONE)
        return MU_INDEXED_HEAP_NONE;
    return h->keys[h->positions[handle]];
}

/* Insert handle, or re-prioritise it if already present. O(log n). */
MU_INLINE bool mu_indexed_heap_push(mu_indexed_heap* h, uint32_t handle, uint32_t key)
{
    MU_ASSERT(h);
    if (handle >= h->capacity)
        return false;

    uint32_t pos = h->positions[handle];
    if (pos != MU_INDEXED_HEAP_NONE)
    {
        uint32_t old = h->keys[pos];
        h->keys[pos] = key;
        if (key < old)
            mu_indexed_heap__sift_up(h, pos);
        else if (key > old)
            mu_indexed_heap__sift_down(h, pos);
        return true;
    }

    if (h->count >= h->capacity)
        return false;

    uint32_t slot = h->count++;
    h->handles[slot]  = handle;
    h->keys[slot]     = key;
    h->positions[handle] = slot;
    mu_indexed_heap__sift_up(h, slot);
    return true;
}

/* Lower an existing entry's priority (fast path: only sift_up is needed). */
MU_INLINE bool mu_indexed_heap_decrease(mu_indexed_heap* h, uint32_t handle, uint32_t key)
{
    MU_ASSERT(h);
    if (handle >= h->capacity)
        return false;

    uint32_t pos = h->positions[handle];
    if (pos == MU_INDEXED_HEAP_NONE)
        return false;

    MU_ASSERT(key <= h->keys[pos]);
    h->keys[pos] = key;
    mu_indexed_heap__sift_up(h, pos);
    return true;
}

/* Remove and return the most urgent handle, or NONE. */
MU_INLINE uint32_t mu_indexed_heap_pop(mu_indexed_heap* h)
{
    MU_ASSERT(h);
    if (h->count == 0)
        return MU_INDEXED_HEAP_NONE;

    uint32_t top = h->handles[0];
    h->positions[top] = MU_INDEXED_HEAP_NONE;

    if (--h->count > 0)
    {
        h->handles[0]         = h->handles[h->count];
        h->keys[0]            = h->keys[h->count];
        h->positions[h->handles[0]] = 0;
        mu_indexed_heap__sift_down(h, 0);
    }
    return top;
}

MU_INLINE uint32_t mu_indexed_heap_peek(const mu_indexed_heap* h)
{
    if (!h || h->count == 0)
        return MU_INDEXED_HEAP_NONE;
    return h->handles[0];
}

/* Remove a specific entry. Returns it, or NONE if absent. */
MU_INLINE uint32_t mu_indexed_heap_remove(mu_indexed_heap* h, uint32_t handle)
{
    MU_ASSERT(h);
    if (handle >= h->capacity)
        return MU_INDEXED_HEAP_NONE;

    uint32_t pos = h->positions[handle];
    if (pos == MU_INDEXED_HEAP_NONE)
        return MU_INDEXED_HEAP_NONE;

    h->positions[handle] = MU_INDEXED_HEAP_NONE;

    if (--h->count > 0 && pos < h->count)
    {
        h->handles[pos]                  = h->handles[h->count];
        h->keys[pos]                     = h->keys[h->count];
        h->positions[h->handles[pos]]    = pos;

        /* The replacement came from the end, so its ordering can break in
           either direction: try both (each is O(log n)). */
        mu_indexed_heap__sift_up(h, pos);
        mu_indexed_heap__sift_down(h, pos);
    }
    return handle;
}

MU_INLINE uint32_t mu_indexed_heap_count(const mu_indexed_heap* h)
{
    return h ? h->count : 0u;
}

MU_INLINE bool mu_indexed_heap_empty(const mu_indexed_heap* h)
{
    return !h || h->count == 0;
}
