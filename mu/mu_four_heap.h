#pragma once
#ifndef MU_FOUR_HEAP_H
#define MU_FOUR_HEAP_H

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * mu_four_heap — Cache-Line SoA 4-Ary Min-Heap (uint64 key, uint32 payload).
 *
 * WHY (LaMarca & Ladner; A* / Dijkstra / Event Simulation):
 *   A binary heap scatters children across 2x deeper levels, suffering twice as
 *   many cache misses during `pop` sift-down. A 4-ary heap places all 4 children
 *   of node `i` contiguously at `4*i + 1 .. 4*i + 4` (32 bytes of `uint64_t`
 *   keys = half a cache line!) and halves tree depth (`log4(N) = 0.5 * log2(N)`).
 *   Splitting `keys[]` from `values[]` (SoA) keeps `values[]` out of cache lines
 *   that are only inspected and not swapped!
 * --------------------------------------------------------------------------- */

typedef struct mu_four_heap
{
    uint64_t* keys;
    uint32_t* values;
    uint32_t  count;
    uint32_t  capacity;
    uint32_t  owned;
} mu_four_heap;

MU_INLINE bool mu_four_heap_init(mu_four_heap* h, uint32_t initial_cap)
{
    MU_ASSERT(h);
    MU_MEMSET(h, 0, sizeof(*h));
    if (initial_cap < 16u)
        initial_cap = 16u;

    uint64_t* k = (uint64_t*)MU_MALLOC((size_t)initial_cap * sizeof(uint64_t));
    uint32_t* v = (uint32_t*)MU_MALLOC((size_t)initial_cap * sizeof(uint32_t));
    if (!k || !v)
    {
        MU_FREE(k);
        MU_FREE(v);
        return false;
    }

    h->keys     = k;
    h->values   = v;
    h->count    = 0u;
    h->capacity = initial_cap;
    h->owned    = 1u;
    return true;
}

MU_INLINE bool mu_four_heap_init_static(
    mu_four_heap* h,
    uint64_t*     keys_buf,
    uint32_t*     values_buf,
    uint32_t      capacity)
{
    MU_ASSERT(h);
    MU_MEMSET(h, 0, sizeof(*h));
    if (!keys_buf || !values_buf || capacity == 0u)
        return false;

    h->keys     = keys_buf;
    h->values   = values_buf;
    h->count    = 0u;
    h->capacity = capacity;
    h->owned    = 0u;
    return true;
}

MU_INLINE void mu_four_heap_destroy(mu_four_heap* h)
{
    if (!h)
        return;
    if (h->owned)
    {
        MU_FREE(h->keys);
        MU_FREE(h->values);
    }
    MU_MEMSET(h, 0, sizeof(*h));
}

MU_INLINE void mu_four_heap_clear(mu_four_heap* h)
{
    MU_ASSERT(h);
    h->count = 0u;
}

MU_INLINE bool mu_four_heap_empty(const mu_four_heap* h)
{
    return !h || h->count == 0u;
}

MU_INLINE void mu_four_heap__sift_up(uint64_t* MU_RESTRICT keys, uint32_t* MU_RESTRICT vals, uint32_t idx, uint64_t k, uint32_t v)
{
    while (idx > 0u)
    {
        uint32_t p  = (idx - 1u) >> 2;
        uint64_t pk = keys[p];
        if (pk <= k)
            break;
        keys[idx] = pk;
        vals[idx] = vals[p];
        idx       = p;
    }
    keys[idx] = k;
    vals[idx] = v;
}

MU_INLINE void mu_four_heap__sift_down(uint64_t* MU_RESTRICT keys, uint32_t* MU_RESTRICT vals, uint32_t n, uint32_t idx, uint64_t k, uint32_t v)
{
    for (;;)
    {
        uint32_t c0 = (idx << 2) + 1u;
        if (c0 >= n)
            break;

        uint32_t min_c;
        uint64_t min_k;

        if (MU_LIKELY(c0 + 3u < n))
        {
            /* Fast branchless tournament across all 4 contiguous children! */
            uint64_t k0 = keys[c0];
            uint64_t k1 = keys[c0 + 1u];
            uint64_t k2 = keys[c0 + 2u];
            uint64_t k3 = keys[c0 + 3u];

            uint32_t m01 = (k1 < k0) ? (c0 + 1u) : c0;
            uint64_t v01 = (k1 < k0) ? k1 : k0;
            uint32_t m23 = (k3 < k2) ? (c0 + 3u) : (c0 + 2u);
            uint64_t v23 = (k3 < k2) ? k3 : k2;

            min_c = (v23 < v01) ? m23 : m01;
            min_k = (v23 < v01) ? v23 : v01;
        }
        else
        {
            min_c = c0;
            min_k = keys[c0];
            for (uint32_t c = c0 + 1u; c < n; ++c)
            {
                if (keys[c] < min_k)
                {
                    min_k = keys[c];
                    min_c = c;
                }
            }
        }

        if (k <= min_k)
            break;

        keys[idx] = min_k;
        vals[idx] = vals[min_c];
        idx       = min_c;
    }
    keys[idx] = k;
    vals[idx] = v;
}

MU_INLINE bool mu_four_heap__grow(mu_four_heap* h)
{
    if (!h->owned)
        return false;
    uint32_t ncap = h->capacity ? (h->capacity * 2u) : 16u;
    if (ncap < h->capacity)
        return false;

    uint64_t* nk = (uint64_t*)MU_REALLOC(h->keys, (size_t)ncap * sizeof(uint64_t));
    if (!nk)
        return false;
    h->keys = nk;

    uint32_t* nv = (uint32_t*)MU_REALLOC(h->values, (size_t)ncap * sizeof(uint32_t));
    if (!nv)
        return false;
    h->values   = nv;
    h->capacity = ncap;
    return true;
}

MU_INLINE bool mu_four_heap_push(mu_four_heap* h, uint64_t key, uint32_t value)
{
    MU_ASSERT(h);
    if (h->count == h->capacity && !mu_four_heap__grow(h))
        return false;

    uint32_t idx = h->count++;
    mu_four_heap__sift_up(h->keys, h->values, idx, key, value);
    return true;
}

MU_INLINE bool mu_four_heap_peek(const mu_four_heap* h, uint64_t* out_key, uint32_t* out_val)
{
    if (!h || h->count == 0u)
        return false;
    if (out_key)
        *out_key = h->keys[0];
    if (out_val)
        *out_val = h->values[0];
    return true;
}

MU_INLINE bool mu_four_heap_pop(mu_four_heap* h, uint64_t* out_key, uint32_t* out_val)
{
    if (!h || h->count == 0u)
        return false;

    if (out_key)
        *out_key = h->keys[0];
    if (out_val)
        *out_val = h->values[0];

    uint32_t last = --h->count;
    if (last > 0u)
        mu_four_heap__sift_down(h->keys, h->values, last, 0u, h->keys[last], h->values[last]);
    return true;
}

/* Fused pop-top + push-new in ONE sift-down (half the work of pop + push!). */
MU_INLINE void mu_four_heap_replace_top(mu_four_heap* h, uint64_t new_key, uint32_t new_val)
{
    MU_ASSERT(h && h->count > 0u);
    mu_four_heap__sift_down(h->keys, h->values, h->count, 0u, new_key, new_val);
}

/* Floyd O(n) bottom-up bulk heapify on existing `h->keys[0..n)` & `h->values[0..n)`. */
MU_INLINE void mu_four_heap_heapify_in_place(mu_four_heap* h, uint32_t n)
{
    MU_ASSERT(h && n <= h->capacity);
    h->count = n;
    if (n <= 1u)
        return;

    uint32_t i = ((n - 2u) >> 2) + 1u;
    while (i > 0u)
    {
        --i;
        mu_four_heap__sift_down(h->keys, h->values, n, i, h->keys[i], h->values[i]);
    }
}

MU_END_EXTERN_C

#endif /* MU_FOUR_HEAP_H */
