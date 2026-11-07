#pragma once
#ifndef MU_WORKLIST_H
#define MU_WORKLIST_H

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_worklist -- unique double-ended work queue over [0, capacity).
 *
 * WHY
 * ---
 * Graphs, job systems and streaming code constantly need "process this set of
 * things, never twice, and be able to take from either end". A plain array
 * plus a bool per slot wastes memory and invites O(n) scans. A deque alone
 * cannot deduplicate. This is both, with one bit per possible entry.
 *
 * Mesa's util/u_worklist stores pointers to indices; this version stores the
 * indices themselves. That removes the stable-address requirement and keeps
 * the hot data contiguous.
 *
 * MEMORY
 * ------
 *     entries[capacity] : ring of live indices, contiguous
 *     present[words]    : membership bitset, 1 bit per possible index
 *
 *     start -> [ 7 | 2 | 9 | 4 | . | . | . | . ]
 *               ^ start          ^ start+count-1 (tail)
 *
 * OPERATIONS
 * ----------
 *   push_head / push_tail : O(1), no-op (false) if already present or full.
 *   pop_head  / pop_tail  : O(1), removes and clears membership.
 *   contains              : O(1).
 *   peek_head / peek_tail : O(1).
 *
 * The queue never allocates after init. For hot paths prefer the static form
 * with caller-owned arrays.
 * --------------------------------------------------------------------------- */

typedef struct mu_worklist
{
    uint32_t* entries;   /* ring buffer of indices                       */
    uint64_t* present;   /* membership bitset                            */
    uint32_t  capacity;  /* legal indices are [0, capacity)              */
    uint32_t  count;     /* number of entries currently queued           */
    uint32_t  start;     /* ring offset of the head                      */
    uint32_t  words;     /* number of 64-bit words in present            */
    uint8_t   owns_memory;
} mu_worklist;

MU_INLINE void mu_worklist__clear_bits(mu_worklist* w)
{
    if (w->words)
        MU_MEMSET(w->present, 0, (size_t)w->words * sizeof(uint64_t));
}

MU_INLINE bool mu_worklist_init(mu_worklist* w, uint32_t capacity)
{
    if (!w || capacity == 0)
        return false;

    w->entries     = NULL;
    w->present     = NULL;
    w->capacity    = capacity;
    w->count       = 0;
    w->start       = 0;
    w->words       = capacity / 64u + ((capacity % 64u) != 0u);
    w->owns_memory = 1;

    w->entries = (uint32_t*)MU_MALLOC((size_t)capacity * sizeof(uint32_t));
    w->present = (uint64_t*)MU_CALLOC((size_t)w->words, sizeof(uint64_t));

    if (!w->entries || !w->present)
    {
        MU_FREE(w->entries);
        MU_FREE(w->present);
        w->entries     = NULL;
        w->present     = NULL;
        w->capacity    = 0;
        w->words       = 0;
        w->owns_memory = 0;
        return false;
    }

    return true;
}

/* Caller owns `entries[capacity]` and `present[(capacity+63)/64]`. */
MU_INLINE bool mu_worklist_init_static(
    mu_worklist* w, uint32_t* entries, uint64_t* present, uint32_t capacity)
{
    if (!w || !entries || !present || capacity == 0)
        return false;

    w->entries     = entries;
    w->present     = present;
    w->capacity    = capacity;
    w->count       = 0;
    w->start       = 0;
    w->words       = capacity / 64u + ((capacity % 64u) != 0u);

    mu_worklist__clear_bits(w);
    w->owns_memory = 0;
    return true;
}

MU_INLINE void mu_worklist_destroy(mu_worklist* w)
{
    if (!w)
        return;

    if (w->owns_memory)
    {
        MU_FREE(w->entries);
        MU_FREE(w->present);
    }

    w->entries     = NULL;
    w->present     = NULL;
    w->capacity    = 0;
    w->count       = 0;
    w->start       = 0;
    w->words       = 0;
    w->owns_memory = 0;
}

MU_INLINE void mu_worklist_clear(mu_worklist* w)
{
    MU_ASSERT(w);
    mu_worklist__clear_bits(w);
    w->count = 0;
    w->start = 0;
}

MU_INLINE bool mu_worklist_empty(const mu_worklist* w)
{
    return !w || w->count == 0;
}

MU_INLINE bool mu_worklist_full(const mu_worklist* w)
{
    return w && w->count >= w->capacity;
}

MU_INLINE uint32_t mu_worklist_count(const mu_worklist* w)
{
    return w ? w->count : 0u;
}

MU_INLINE uint32_t mu_worklist_capacity(const mu_worklist* w)
{
    return w ? w->capacity : 0u;
}

MU_INLINE bool mu_worklist__test(const mu_worklist* w, uint32_t value)
{
    return (w->present[value >> 6] >> (value & 63u)) & 1u;
}

MU_INLINE void mu_worklist__set(mu_worklist* w, uint32_t value)
{
    w->present[value >> 6] |= UINT64_C(1) << (value & 63u);
}

MU_INLINE void mu_worklist__reset(mu_worklist* w, uint32_t value)
{
    w->present[value >> 6] &= ~(UINT64_C(1) << (value & 63u));
}

MU_INLINE bool mu_worklist_contains(const mu_worklist* w, uint32_t value)
{
    return w && value < w->capacity && mu_worklist__test(w, value);
}

MU_INLINE uint32_t mu_worklist__ring(const mu_worklist* w, uint32_t offset)
{
    uint32_t tail_space = w->capacity - w->start;
    return (offset >= tail_space) ? (offset - tail_space) : (w->start + offset);
}

MU_INLINE uint32_t mu_worklist_at(const mu_worklist* w, uint32_t offset)
{
    MU_ASSERT(w && offset < w->count);
    return w->entries[mu_worklist__ring(w, offset)];
}

MU_INLINE uint32_t mu_worklist_peek_head(const mu_worklist* w, uint32_t fallback)
{
    if (!w || w->count == 0)
        return fallback;
    return w->entries[w->start];
}

MU_INLINE uint32_t mu_worklist_peek_tail(const mu_worklist* w, uint32_t fallback)
{
    if (!w || w->count == 0)
        return fallback;
    return w->entries[mu_worklist__ring(w, w->count - 1u)];
}

/* Returns false if already queued, out of range, or full. */
MU_INLINE bool mu_worklist_push_tail(mu_worklist* w, uint32_t value)
{
    if (!w || value >= w->capacity || w->count >= w->capacity)
        return false;
    if (mu_worklist__test(w, value))
        return false;

    w->entries[mu_worklist__ring(w, w->count)] = value;
    mu_worklist__set(w, value);
    ++w->count;
    return true;
}

/* Returns false if already queued, out of range, or full. */
MU_INLINE bool mu_worklist_push_head(mu_worklist* w, uint32_t value)
{
    if (!w || value >= w->capacity || w->count >= w->capacity)
        return false;
    if (mu_worklist__test(w, value))
        return false;

    w->start = (w->start == 0u) ? (w->capacity - 1u) : (w->start - 1u);
    w->entries[w->start] = value;
    mu_worklist__set(w, value);
    ++w->count;
    return true;
}

MU_INLINE bool mu_worklist_pop_head(mu_worklist* w, uint32_t* out_value)
{
    if (!w || w->count == 0)
        return false;

    uint32_t value = w->entries[w->start];
    mu_worklist__reset(w, value);
    w->start = (w->start + 1u == w->capacity) ? 0u : (w->start + 1u);
    --w->count;

    if (out_value)
        *out_value = value;
    return true;
}

MU_INLINE bool mu_worklist_pop_tail(mu_worklist* w, uint32_t* out_value)
{
    if (!w || w->count == 0)
        return false;

    uint32_t idx   = mu_worklist__ring(w, w->count - 1u);
    uint32_t value = w->entries[idx];
    mu_worklist__reset(w, value);
    --w->count;

    if (out_value)
        *out_value = value;
    return true;
}

#endif /* MU_WORKLIST_H */
