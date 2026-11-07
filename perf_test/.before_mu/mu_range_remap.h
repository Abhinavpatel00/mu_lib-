#pragma once
#ifndef MU_RANGE_REMAP_H
#define MU_RANGE_REMAP_H

/*
 * mu_range_remap.h -- sorted map from closed unsigned ranges to pointers.
 *
 * Ranges never overlap. Lookup is O(log n) over a flat sorted array, so it is
 * cache-friendly and allocation-light. Use for virtual address/page maps,
 * sparse resource ranges, asset range tables, and GPU descriptor mappings.
 *
 */

#include "mu_common.h"

MU_BEGIN_EXTERN_C

typedef struct mu_range_entry
{
    uint32_t start;
    uint32_t end; /* inclusive */
    void* ptr;
} mu_range_entry;

typedef struct mu_range_remap
{
    mu_range_entry* entries;
    uint32_t count;
    uint32_t capacity;
    uint32_t owned;
} mu_range_remap;

MU_INLINE void mu_range_remap_init(mu_range_remap* remap)
{
    MU_ASSERT(remap != NULL);
    remap->entries = NULL;
    remap->count = 0;
    remap->capacity = 0;
    remap->owned = 1;
}

MU_INLINE bool mu_range_remap_init_static(mu_range_remap* remap,
                                          mu_range_entry* entries,
                                          uint32_t capacity)
{
    MU_ASSERT(remap != NULL);
    remap->entries = entries;
    remap->count = 0;
    remap->capacity = capacity;
    remap->owned = 0;
    return capacity == 0 || entries != NULL;
}

MU_INLINE void mu_range_remap_destroy(mu_range_remap* remap)
{
    if (!remap)
        return;
    if (remap->owned)
        MU_FREE(remap->entries);
    remap->entries = NULL;
    remap->count = 0;
    remap->capacity = 0;
    remap->owned = 0;
}

MU_INLINE void mu_range_remap_clear(mu_range_remap* remap)
{
    MU_ASSERT(remap != NULL);
    remap->count = 0;
}

MU_INLINE bool mu_range_remap_reserve(mu_range_remap* remap, uint32_t needed)
{
    uint32_t capacity;
    mu_range_entry* entries;

    MU_ASSERT(remap != NULL);
    if (needed <= remap->capacity)
        return true;
    if (!remap->owned)
        return false;

    capacity = remap->capacity ? remap->capacity : 16u;
    while (capacity < needed)
    {
        if (capacity > UINT32_MAX / 2u)
        {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }

#if SIZE_MAX <= UINT32_MAX
    if ((size_t)capacity > SIZE_MAX / sizeof(mu_range_entry))
        return false;
#endif

    entries = (mu_range_entry*)MU_REALLOC(remap->entries,
                                          (size_t)capacity * sizeof(mu_range_entry));
    if (!entries)
        return false;

    remap->entries = entries;
    remap->capacity = capacity;
    return true;
}

MU_INLINE uint32_t mu_range_remap__lower_bound(const mu_range_remap* remap,
                                               uint32_t value)
{
    uint32_t lo = 0;
    uint32_t hi = remap->count;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (remap->entries[mid].start < value)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

MU_INLINE const mu_range_entry* mu_range_remap_find(const mu_range_remap* remap,
                                                    uint32_t value)
{
    uint32_t lo = 0;
    uint32_t hi;
    MU_ASSERT(remap != NULL);

    hi = remap->count;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (remap->entries[mid].end < value)
            lo = mid + 1u;
        else
            hi = mid;
    }

    if (lo < remap->count && remap->entries[lo].start <= value)
        return &remap->entries[lo];
    return NULL;
}

MU_INLINE void* mu_range_remap_get(const mu_range_remap* remap, uint32_t value)
{
    const mu_range_entry* entry = mu_range_remap_find(remap, value);
    return entry ? entry->ptr : NULL;
}

MU_INLINE bool mu_range_remap_contains(const mu_range_remap* remap, uint32_t value)
{
    return mu_range_remap_find(remap, value) != NULL;
}

/* Insert a closed range. Returns the entry, or NULL on overlap/OOM.
 * If ptr is NULL, only an exact existing range is returned/updated. */
MU_INLINE mu_range_entry* mu_range_remap_insert(mu_range_remap* remap,
                                                uint32_t start,
                                                uint32_t end,
                                                void* ptr)
{
    uint32_t at;
    MU_ASSERT(remap != NULL);

    if (start > end)
        return NULL;

    at = mu_range_remap__lower_bound(remap, start);

    if (at < remap->count &&
        remap->entries[at].start == start &&
        remap->entries[at].end == end)
    {
        if (ptr)
            remap->entries[at].ptr = ptr;
        return &remap->entries[at];
    }

    if (!ptr)
        return NULL;

    if (at > 0 && remap->entries[at - 1u].end >= start)
        return NULL;
    if (at < remap->count && remap->entries[at].start <= end)
        return NULL;

    if (!mu_range_remap_reserve(remap, remap->count + 1u))
        return NULL;

    if (at < remap->count)
    {
        MU_MEMMOVE(&remap->entries[at + 1u], &remap->entries[at],
                   (size_t)(remap->count - at) * sizeof(mu_range_entry));
    }

    remap->entries[at].start = start;
    remap->entries[at].end = end;
    remap->entries[at].ptr = ptr;
    ++remap->count;
    return &remap->entries[at];
}

/* Like insert(), but if start matches an existing range and end is inside it,
 * truncate the existing range and update its pointer. */
MU_INLINE mu_range_entry* mu_range_remap_insert_truncate(mu_range_remap* remap,
                                                         uint32_t start,
                                                         uint32_t end,
                                                         void* ptr)
{
    uint32_t at;
    MU_ASSERT(remap != NULL);

    if (start > end)
        return NULL;

    at = mu_range_remap__lower_bound(remap, start);
    if (at < remap->count && remap->entries[at].start == start)
    {
        if (end > remap->entries[at].end)
            return NULL;
        if (ptr)
            remap->entries[at].ptr = ptr;
        remap->entries[at].end = end;
        return &remap->entries[at];
    }

    return mu_range_remap_insert(remap, start, end, ptr);
}

MU_INLINE bool mu_range_remap_remove(mu_range_remap* remap, uint32_t start, uint32_t end)
{
    uint32_t at;
    MU_ASSERT(remap != NULL);

    at = mu_range_remap__lower_bound(remap, start);
    if (at >= remap->count ||
        remap->entries[at].start != start ||
        remap->entries[at].end != end)
        return false;

    if (at + 1u < remap->count)
    {
        MU_MEMMOVE(&remap->entries[at], &remap->entries[at + 1u],
                   (size_t)(remap->count - at - 1u) * sizeof(mu_range_entry));
    }
    --remap->count;
    return true;
}

MU_INLINE uint32_t mu_range_remap_count(const mu_range_remap* remap)
{
    return remap ? remap->count : 0u;
}

MU_INLINE const mu_range_entry* mu_range_remap_at(const mu_range_remap* remap,
                                                  uint32_t index)
{
    MU_ASSERT(remap != NULL && index < remap->count);
    return &remap->entries[index];
}

MU_END_EXTERN_C

#endif /* MU_RANGE_REMAP_H */
