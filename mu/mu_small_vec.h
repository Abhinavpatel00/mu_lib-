#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_small_vec — vector with caller-supplied inline storage, optional overflow.
 *
 * WHY: the common case is "usually a handful, occasionally more" (batch lists,
 * touched-object lists, per-system scratch). A plain array wastes space or
 * mallocs on the first push; a stretchy buffer always mallocs. This keeps the
 * first N elements in a buffer YOU provide (hot path: no allocation at all)
 * and only spills to the heap if you opt in with the growable form.
 *
 * Two modes, chosen at init:
 *     mu_small_vec_init(...)          inline only  -> push() returns false when full
 *     mu_small_vec_init_growable(...) inline first -> heap overflow after inline_cap
 *
 * The static mode is the one AGENT.md wants in hot loops: it cannot allocate,
 * so a full list is an explicit, cheap, detectable condition instead of a
 * hidden malloc on the render thread.
 *
 * CONTRACT: elem_size bytes per element; inline buffer must hold at least
 * inline_cap * elem_size bytes. Not thread safe.
 * --------------------------------------------------------------------------- */

typedef struct mu_small_vec
{
    uint8_t*  data;         /* inline buffer, then heap once it overflows   */
    uint32_t  count;
    uint32_t  capacity;     /* current (>= inline_capacity once grown)      */
    uint32_t  elem_size;
    uint32_t  inline_capacity;
    uint32_t  growable;     /* 1 = may spill to the heap                    */
    uint32_t  heap_owned;   /* 1 = data is heap memory we must free         */
} mu_small_vec;

/* Inline-only: never allocates. `inline_buf` must outlive the vector. */
MU_INLINE bool mu_small_vec_init(mu_small_vec* v, void* inline_buf, uint32_t inline_capacity, uint32_t elem_size)
{
    MU_ASSERT(v);
    v->data            = (uint8_t*)inline_buf;
    v->count           = 0;
    v->capacity        = inline_capacity;
    v->elem_size       = elem_size;
    v->inline_capacity = inline_capacity;
    v->growable         = 0;
    v->heap_owned       = 0;

    if (elem_size == 0 || (!inline_buf && inline_capacity > 0))
    {
        v->count    = 0;
        v->capacity = 0;
        return false;
    }
    return true;
}

/* Same, but spills to the heap past inline_capacity. */
MU_INLINE bool mu_small_vec_init_growable(
    mu_small_vec* v, void* inline_buf, uint32_t inline_capacity, uint32_t elem_size)
{
    if (!mu_small_vec_init(v, inline_buf, inline_capacity, elem_size))
        return false;
    v->growable = 1;
    return true;
}

MU_INLINE void mu_small_vec_destroy(mu_small_vec* v)
{
    if (!v)
        return;
    if (v->heap_owned)
        MU_FREE(v->data);
    v->data            = NULL;
    v->count           = 0;
    v->capacity        = 0;
    v->heap_owned      = 0;
    v->growable         = 0;
}

MU_INLINE void mu_small_vec_clear(mu_small_vec* v)
{
    MU_ASSERT(v);
    v->count = 0;
}

MU_INLINE uint32_t mu_small_vec_count(const mu_small_vec* v)
{
    return v ? v->count : 0u;
}

MU_INLINE uint32_t mu_small_vec_capacity(const mu_small_vec* v)
{
    return v ? v->capacity : 0u;
}

MU_INLINE bool mu_small_vec_empty(const mu_small_vec* v)
{
    return !v || v->count == 0;
}

MU_INLINE bool mu_small_vec_full(const mu_small_vec* v)
{
    return !v || v->count >= v->capacity;
}

/* Ensure room for `needed` elements. Inline-only mode never grows. */
MU_INLINE bool mu_small_vec_reserve(mu_small_vec* v, uint32_t needed)
{
    MU_ASSERT(v);
    if (needed <= v->capacity)
        return true;
    if (!v->growable)
        return false; /* inline-only: an overflow is a real, visible condition */

    uint32_t cap = v->capacity ? v->capacity : 8u;
    while (cap < needed)
    {
        if (cap > 0x40000000u)
            return false;
        cap <<= 1;
    }

    uint8_t* fresh;
    if (v->heap_owned)
    {
        fresh = (uint8_t*)MU_REALLOC(v->data, (size_t)cap * v->elem_size);
        if (!fresh)
            return false;
        v->data = fresh;
    }
    else
    {
        /* First spill: copy out of the caller's inline buffer. */
        fresh = (uint8_t*)MU_MALLOC((size_t)cap * v->elem_size);
        if (!fresh)
            return false;
        if (v->count > 0)
            MU_MEMCPY(fresh, v->data, (size_t)v->count * v->elem_size);
        v->data       = fresh;
        v->heap_owned = 1;
    }

    v->capacity = cap;
    return true;
}

/* Returns false when full (inline mode) or on allocation failure. */
MU_INLINE bool mu_small_vec_push(mu_small_vec* v, const void* elem)
{
    MU_ASSERT(v && elem);
    if (v->count >= v->capacity && !mu_small_vec_reserve(v, v->count + 1u))
        return false;

    MU_MEMCPY(v->data + (size_t)v->count * v->elem_size, elem, v->elem_size);
    ++v->count;
    return true;
}

MU_INLINE void mu_small_vec_pop(mu_small_vec* v)
{
    MU_ASSERT(v);
    if (v->count > 0)
        --v->count;
}

MU_INLINE void* mu_small_vec_at(mu_small_vec* v, uint32_t i)
{
    MU_ASSERT(v && i < v->count);
    return v->data + (size_t)i * v->elem_size;
}

MU_INLINE const void* mu_small_vec_at_const(const mu_small_vec* v, uint32_t i)
{
    MU_ASSERT(v && i < v->count);
    return v->data + (size_t)i * v->elem_size;
}

MU_INLINE void* mu_small_vec_back(mu_small_vec* v)
{
    MU_ASSERT(v && v->count > 0);
    return v->data + (size_t)(v->count - 1u) * v->elem_size;
}

/* Swap-remove: O(1), order not preserved. Right for scratch/touched lists. */
MU_INLINE void mu_small_vec_remove_swap(mu_small_vec* v, uint32_t i)
{
    MU_ASSERT(v && i < v->count);
    if (i + 1u != v->count)
        MU_MEMCPY(v->data + (size_t)i * v->elem_size,
                  v->data + (size_t)(v->count - 1u) * v->elem_size,
                  v->elem_size);
    --v->count;
}

/* Order-preserving remove: O(n) memmove. */
MU_INLINE void mu_small_vec_remove(mu_small_vec* v, uint32_t i)
{
    MU_ASSERT(v && i < v->count);
    if (i + 1u < v->count)
        MU_MEMMOVE(v->data + (size_t)i * v->elem_size,
                   v->data + (size_t)(i + 1u) * v->elem_size,
                   (size_t)(v->count - i - 1u) * v->elem_size);
    --v->count;
}
