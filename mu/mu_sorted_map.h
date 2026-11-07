#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_sorted_map — flat, sorted (key, value) table with binary search.
 *
 * WHY: for read-mostly static tables (glyph atlas, keybinds, name->id, config,
 * string tables) a hash table is the wrong tool: buckets, load factors and
 * hash-quality cliffs. A sorted array gives log2(n) probes that are contiguous
 * in memory and fully prefetchable, with zero wasted bytes.
 *
 * MEMORY MODEL (hot/cold split, AGENT.md data-layout):
 *     keys[]   uint64, sorted ascending  <- the search only ever touches this
 *     values[] elem_size bytes each      <- cold, loaded only once on a hit
 * so a failed probe reads log2(n) * 8 bytes and never brings values into cache.
 *
 * CONTRACT: keys are unique. Not thread safe.
 *   - put() inserts in order: O(n) memmove, fine for small/medium tables.
 *   - mu_sorted_map_sort() bulk-builds from unsorted input: O(n log n).
 *   - sorted ascending => traversal is already cache- and branch-predictor
 *     friendly (good for prefix-range scans).
 * --------------------------------------------------------------------------- */

typedef struct mu_sorted_map
{
    uint64_t* keys;      /* sorted ascending                                 */
    uint8_t*  values;    /* count * elem_size                                */
    uint32_t  count;
    uint32_t  capacity;
    uint32_t  elem_size;
    uint32_t  owned;     /* 1 = heap buffers we may free                     */
} mu_sorted_map;

/* Internal: lower_bound. Sets *out to the first index with keys[i] >= key. */
MU_INLINE bool mu_sorted_map__find_index(const mu_sorted_map* m, uint64_t key, uint32_t* out)
{
    uint32_t lo = 0;
    uint32_t hi = m->count;

    while (lo < hi)
    {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (m->keys[mid] < key)
            lo = mid + 1u;
        else
            hi = mid;
    }

    *out = lo;
    return (lo < m->count) && (m->keys[lo] == key);
}

MU_INLINE bool mu_sorted_map_init(mu_sorted_map* m, uint32_t elem_size, uint32_t capacity)
{
    MU_ASSERT(m);
    m->keys      = NULL;
    m->values    = NULL;
    m->count     = 0;
    m->capacity  = capacity;
    m->elem_size = elem_size;
    m->owned     = 1;

    if (capacity == 0 || elem_size == 0)
        return elem_size != 0;

    m->keys   = (uint64_t*)MU_MALLOC((size_t)capacity * sizeof(uint64_t));
    m->values = (uint8_t*)MU_MALLOC((size_t)capacity * elem_size);
    if (!m->keys || !m->values)
    {
        MU_FREE(m->keys);
        MU_FREE(m->values);
        m->keys     = NULL;
        m->values   = NULL;
        m->count    = 0;
        m->owned    = 0;
        return false;
    }
    return true;
}

/* Caller-provided arrays. values must hold capacity * elem_size bytes. */
MU_INLINE bool mu_sorted_map_init_static(
    mu_sorted_map* m, uint64_t* keys, void* values, uint32_t capacity, uint32_t elem_size)
{
    MU_ASSERT(m);
    m->keys      = keys;
    m->values    = (uint8_t*)values;
    m->count     = 0;
    m->capacity  = capacity;
    m->elem_size = elem_size;
    m->owned     = 0;

    if (elem_size == 0)
        return false;
    if (capacity > 0 && (!keys || !values))
    {
        m->count    = 0;
        m->capacity = 0;
        return false;
    }
    return true;
}

MU_INLINE void mu_sorted_map_destroy(mu_sorted_map* m)
{
    if (!m)
        return;
    if (m->owned)
    {
        MU_FREE(m->keys);
        MU_FREE(m->values);
    }
    m->keys     = NULL;
    m->values   = NULL;
    m->count    = 0;
    m->capacity = 0;
    m->owned    = 0;
}

MU_INLINE void mu_sorted_map_clear(mu_sorted_map* m)
{
    MU_ASSERT(m);
    m->count = 0;
}

MU_INLINE bool mu_sorted_map_reserve(mu_sorted_map* m, uint32_t needed)
{
    MU_ASSERT(m);
    if (needed <= m->capacity)
        return true;
    if (!m->owned)
        return false;

    uint32_t cap = m->capacity ? m->capacity : 16u;
    while (cap < needed)
    {
        if (cap > 0x40000000u)
            return false;
        cap <<= 1;
    }

    uint64_t* k = (uint64_t*)MU_REALLOC(m->keys, (size_t)cap * sizeof(uint64_t));
    if (!k)
        return false;
    m->keys = k;

    uint8_t* v = (uint8_t*)MU_REALLOC(m->values, (size_t)cap * m->elem_size);
    if (!v)
        return false; /* keys already grew; capacity stays put, so still safe */
    m->values   = v;
    m->capacity = cap;
    return true;
}

/* ------------------------------------------------------------------ *
 * Lookup
 * ------------------------------------------------------------------ */

MU_INLINE const void* mu_sorted_map_find(const mu_sorted_map* m, uint64_t key)
{
    uint32_t i;
    if (!m || !mu_sorted_map__find_index(m, key, &i))
        return NULL;
    return m->values + (size_t)i * m->elem_size;
}

MU_INLINE bool mu_sorted_map_get(const mu_sorted_map* m, uint64_t key, void* out_value)
{
    uint32_t i;
    if (!m || !mu_sorted_map__find_index(m, key, &i))
        return false;
    if (out_value)
        MU_MEMCPY(out_value, m->values + (size_t)i * m->elem_size, m->elem_size);
    return true;
}

MU_INLINE bool mu_sorted_map_contains(const mu_sorted_map* m, uint64_t key)
{
    uint32_t i;
    return m && mu_sorted_map__find_index(m, key, &i);
}

/* ------------------------------------------------------------------ *
 * Mutation
 * ------------------------------------------------------------------ */

/* Insert, or overwrite in place if the key exists. */
MU_INLINE bool mu_sorted_map_put(mu_sorted_map* m, uint64_t key, const void* value)
{
    MU_ASSERT(m && value);

    uint32_t at;
    bool     found = mu_sorted_map__find_index(m, key, &at);

    if (found)
    {
        MU_MEMCPY(m->values + (size_t)at * m->elem_size, value, m->elem_size);
        return true;
    }

    if (!mu_sorted_map_reserve(m, m->count + 1u))
        return false;

    /* Make room: keys and values are parallel, both shift by one slot. */
    if (at < m->count)
    {
        MU_MEMMOVE(m->keys + at + 1u, m->keys + at, (size_t)(m->count - at) * sizeof(uint64_t));
        MU_MEMMOVE(m->values + (size_t)(at + 1u) * m->elem_size,
                   m->values + (size_t)at * m->elem_size,
                   (size_t)(m->count - at) * m->elem_size);
    }

    m->keys[at] = key;
    MU_MEMCPY(m->values + (size_t)at * m->elem_size, value, m->elem_size);
    ++m->count;
    return true;
}

MU_INLINE bool mu_sorted_map_remove(mu_sorted_map* m, uint64_t key)
{
    uint32_t at;
    if (!m || !mu_sorted_map__find_index(m, key, &at))
        return false;

    if (at + 1u < m->count)
    {
        MU_MEMMOVE(m->keys + at, m->keys + at + 1u, (size_t)(m->count - at - 1u) * sizeof(uint64_t));
        MU_MEMMOVE(m->values + (size_t)at * m->elem_size,
                   m->values + (size_t)(at + 1u) * m->elem_size,
                   (size_t)(m->count - at - 1u) * m->elem_size);
    }
    --m->count;
    return true;
}

/* ------------------------------------------------------------------ *
 * Ordered access — a sorted map is its own sorted key list.
 * ------------------------------------------------------------------ */

MU_INLINE uint64_t mu_sorted_map_key_at(const mu_sorted_map* m, uint32_t i)
{
    MU_ASSERT(m && i < m->count);
    return m->keys[i];
}

MU_INLINE const void* mu_sorted_map_value_at(const mu_sorted_map* m, uint32_t i)
{
    MU_ASSERT(m && i < m->count);
    return m->values + (size_t)i * m->elem_size;
}

MU_INLINE uint32_t mu_sorted_map_count(const mu_sorted_map* m)
{
    return m ? m->count : 0u;
}

MU_INLINE bool mu_sorted_map_empty(const mu_sorted_map* m)
{
    return !m || m->count == 0;
}

/* ------------------------------------------------------------------ *
 * Bulk build: sort an unsorted filled buffer into order.
 *
 * Approach: sort a permutation of indices over keys[] (keys are read-only
 * during the sort, so the working set stays hot), then scatter through that
 * permutation into one temp block and copy back.
 *
 * The permutation uses heapsort: iterative (no recursion, so no stack growth
 * regardless of input) and O(n log n) worst case with no adversarial input.
 * ------------------------------------------------------------------ */

MU_INLINE void mu_sorted_map__sort_index(uint32_t* idx, uint32_t n, const uint64_t* keys)
{
    if (n < 2u)
        return;

    for (uint32_t start = n / 2u; start-- > 0;)
    {
        uint32_t root = start;
        for (;;)
        {
            uint32_t child = (root << 1) + 1u;
            if (child >= n)
                break;
            if (child + 1u < n && keys[idx[child + 1u]] > keys[idx[child]])
                ++child;
            if (keys[idx[root]] >= keys[idx[child]])
                break;
            MU_SWAP(uint32_t, idx[root], idx[child]);
            root = child;
        }
    }

    for (uint32_t end = n; end-- > 1;)
    {
        MU_SWAP(uint32_t, idx[0], idx[end]);

        uint32_t root = 0;
        for (;;)
        {
            uint32_t child = (root << 1) + 1u;
            if (child >= end)
                break;
            if (child + 1u < end && keys[idx[child + 1u]] > keys[idx[child]])
                ++child;
            if (keys[idx[root]] >= keys[idx[child]])
                break;
            MU_SWAP(uint32_t, idx[root], idx[child]);
            root = child;
        }
    }
}

/* Sort `count` entries in place. The buffer must already be filled
   (count set, keys/values populated) but is not yet ordered. */
MU_INLINE bool mu_sorted_map_sort(mu_sorted_map* m)
{
    MU_ASSERT(m);
    if (m->count < 2u)
        return true;

    size_t  n     = m->count;
    size_t  es    = m->elem_size;
    size_t  idx_bytes  = n * sizeof(uint32_t);
    size_t  keys_bytes = n * sizeof(uint64_t);
    size_t  vals_bytes = n * es;

    uint8_t* block = (uint8_t*)MU_MALLOC(idx_bytes + keys_bytes + vals_bytes);
    if (!block)
        return false;

    uint32_t* idx    = (uint32_t*)block;
    uint64_t* tmp_k  = (uint64_t*)(block + idx_bytes);
    uint8_t*  tmp_v  = block + idx_bytes + keys_bytes;

    for (size_t i = 0; i < n; ++i)
        idx[i] = (uint32_t)i;

    mu_sorted_map__sort_index(idx, (uint32_t)n, m->keys);

    for (size_t i = 0; i < n; ++i)
    {
        tmp_k[i] = m->keys[idx[i]];
        MU_MEMCPY(tmp_v + i * es, m->values + (size_t)idx[i] * es, es);
    }

    MU_MEMCPY(m->keys, tmp_k, keys_bytes);
    MU_MEMCPY(m->values, tmp_v, vals_bytes);

    MU_FREE(block);
    return true;
}
