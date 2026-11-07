#pragma once
#ifndef MU_SWISS_MAP_H
#define MU_SWISS_MAP_H

#include "mu_common.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define MU_SWISS_SSE2 1
#  include <emmintrin.h>
#endif

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * mu_swiss_map & mu_dense_map — SIMD H2 Control-Byte Flat Hash Tables.
 *
 * TWO COMPLEMENTARY HIGH-PERFORMANCE HASH TABLES:
 *
 * 1. `mu_swiss_map` (Abseil flat_hash_map style, uint64 -> uint64):
 *    - 1-byte `ctrl[]` metadata array (0x80 EMPTY, 0xFE DELETED, 0x00..0x7F H2)
 *    - Probes 16 slots in parallel via SSE2 `_mm_cmpeq_epi8 + _mm_movemask_epi8`
 *      (or 64-bit SWAR on pure C99 targets — ABI-identical 16-slot group width!)
 *    - Accepts ANY uint64_t key (including 0 and UINT64_MAX — no reserved key!)
 *    - Quadratic triangular group probing (`step += 16`) eliminates primary
 *      clustering while touching only 1 control cache line on >98% of lookups.
 *
 * 2. `mu_dense_map` (ankerl::unordered_dense style, uint64 -> uint64):
 *    - Stores keys & values densely in `keys[0..count)` and `values[0..count)`
 *    - SIMD H2 control table stores `uint32_t` indices into the dense arrays
 *    - O(1) swap-and-pop removal + unit-stride SoA iteration (60x faster than
 *      sparse/node hash iteration!).
 * --------------------------------------------------------------------------- */

#define MU_SWISS_GROUP_WIDTH 16u
#define MU_SWISS_EMPTY       ((uint8_t)0x80u)
#define MU_SWISS_DELETED     ((uint8_t)0xFEu)

MU_INLINE uint64_t mu_swiss__hash(uint64_t x)
{
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

MU_INLINE uint8_t mu_swiss__h2(uint64_t h)
{
    return (uint8_t)(h & 0x7Fu); /* top bit 0 -> valid occupied H2 fingerprint */
}

/* Match 16 control bytes against `h2` (0x00..0x7F), returning 16-bit mask. */
MU_INLINE uint32_t mu_swiss__match_h2(const uint8_t* ctrl, uint8_t h2)
{
#if defined(MU_SWISS_SSE2)
    __m128i c = _mm_loadu_si128((const __m128i*)ctrl);
    __m128i t = _mm_set1_epi8((char)h2);
    return (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(c, t));
#else
    uint64_t lo, hi;
    MU_MEMCPY(&lo, ctrl, 8);
    MU_MEMCPY(&hi, ctrl + 8, 8);
    uint64_t pat = UINT64_C(0x0101010101010101) * (uint64_t)h2;
    uint64_t x0  = lo ^ pat;
    uint64_t x1  = hi ^ pat;
    uint64_t m0  = (x0 - UINT64_C(0x0101010101010101)) & ~x0 & UINT64_C(0x8080808080808080);
    uint64_t m1  = (x1 - UINT64_C(0x0101010101010101)) & ~x1 & UINT64_C(0x8080808080808080);
    uint32_t b0  = (uint32_t)((m0 * UINT64_C(0x0002040810204081)) >> 56) & 0xFFu;
    uint32_t b1  = (uint32_t)((m1 * UINT64_C(0x0002040810204081)) >> 56) & 0xFFu;
    return b0 | (b1 << 8);
#endif
}

/* Match EMPTY (0x80) bytes in a 16-byte group, returning 16-bit mask. */
MU_INLINE uint32_t mu_swiss__match_empty(const uint8_t* ctrl)
{
#if defined(MU_SWISS_SSE2)
    __m128i c = _mm_loadu_si128((const __m128i*)ctrl);
    __m128i e = _mm_set1_epi8((char)(int8_t)-128); /* 0x80 */
    return (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(c, e));
#else
    return mu_swiss__match_h2(ctrl, MU_SWISS_EMPTY);
#endif
}

/* Match EMPTY (0x80) or DELETED (0xFE) — i.e., any slot with high bit 1! */
MU_INLINE uint32_t mu_swiss__match_empty_or_deleted(const uint8_t* ctrl)
{
#if defined(MU_SWISS_SSE2)
    __m128i c = _mm_loadu_si128((const __m128i*)ctrl);
    return (uint32_t)_mm_movemask_epi8(c);
#else
    uint64_t lo, hi;
    MU_MEMCPY(&lo, ctrl, 8);
    MU_MEMCPY(&hi, ctrl + 8, 8);
    uint64_t m0 = lo & UINT64_C(0x8080808080808080);
    uint64_t m1 = hi & UINT64_C(0x8080808080808080);
    uint32_t b0 = (uint32_t)((m0 * UINT64_C(0x0002040810204081)) >> 56) & 0xFFu;
    uint32_t b1 = (uint32_t)((m1 * UINT64_C(0x0002040810204081)) >> 56) & 0xFFu;
    return b0 | (b1 << 8);
#endif
}

MU_INLINE void mu_swiss__set_ctrl(uint8_t* ctrl, uint32_t mask, uint32_t idx, uint8_t val)
{
    ctrl[idx] = val;
    /* Mirror first 16 slots at ctrl[capacity .. capacity + 15] for wrap-free loads: */
    if (idx < MU_SWISS_GROUP_WIDTH)
        ctrl[(mask + 1u) + idx] = val;
}

/* Abseil `was_never_full` theorem for unaligned 16-wide probe groups:
   The contiguous non-EMPTY run containing `slot` has length `lz + tz`.
   No 16-wide probe window containing `slot` could ever have been full iff
   `lz + tz < 16` — allowing `EMPTY` restoration instead of a tombstone! */
MU_INLINE bool mu_swiss__was_never_full(const uint8_t* ctrl, uint32_t mask, uint32_t slot)
{
    uint32_t before = mu_swiss__match_empty(ctrl + ((slot - MU_SWISS_GROUP_WIDTH) & mask));
    uint32_t after  = mu_swiss__match_empty(ctrl + slot);
    uint32_t tz     = mu_trailing_zeroes_u32(after);                 /* 1..16 (32 if 0) */
    uint32_t lz     = (before == 0u) ? 16u : mu_leading_zeroes_u32(before << 16u);
    return (lz + tz) < MU_SWISS_GROUP_WIDTH;
}

/* ========================================================================== *
 * 1. mu_swiss_map — Flat SIMD Control-Byte Hash Table
 * ========================================================================== */

typedef struct mu_swiss_map
{
    uint8_t*  ctrl;        /* size: capacity + MU_SWISS_GROUP_WIDTH */
    uint64_t* keys;        /* size: capacity                        */
    uint64_t* values;      /* size: capacity                        */
    uint32_t  capacity;    /* power of two >= 16                    */
    uint32_t  mask;
    uint32_t  count;       /* live entries                          */
    uint32_t  growth_left; /* remaining EMPTY slots before rehash   */
    uint32_t  owned;
} mu_swiss_map;

MU_INLINE uint32_t mu_swiss__round_cap(uint32_t min_cap)
{
    uint32_t cap = MU_SWISS_GROUP_WIDTH;
    while (cap < min_cap)
    {
        if (cap > 0x40000000u)
            return 0u;
        cap <<= 1;
    }
    return cap;
}

MU_INLINE bool mu_swiss_map_init_with_capacity(mu_swiss_map* m, uint32_t cap)
{
    MU_ASSERT(m);
    MU_MEMSET(m, 0, sizeof(*m));

    cap = mu_swiss__round_cap(cap < MU_SWISS_GROUP_WIDTH ? MU_SWISS_GROUP_WIDTH : cap);
    if (cap == 0u)
        return false;

    uint8_t*  ctrl = (uint8_t*)MU_MALLOC((size_t)cap + MU_SWISS_GROUP_WIDTH);
    uint64_t* keys = (uint64_t*)MU_MALLOC((size_t)cap * sizeof(uint64_t));
    uint64_t* vals = (uint64_t*)MU_MALLOC((size_t)cap * sizeof(uint64_t));
    if (!ctrl || !keys || !vals)
    {
        MU_FREE(ctrl);
        MU_FREE(keys);
        MU_FREE(vals);
        return false;
    }

    MU_MEMSET(ctrl, MU_SWISS_EMPTY, (size_t)cap + MU_SWISS_GROUP_WIDTH);
    m->ctrl        = ctrl;
    m->keys        = keys;
    m->values      = vals;
    m->capacity    = cap;
    m->mask        = cap - 1u;
    m->count       = 0u;
    m->growth_left = (cap * 7u) / 8u; /* 87.5% max load factor! */
    m->owned       = 1u;
    return true;
}

MU_INLINE bool mu_swiss_map_init(mu_swiss_map* m, uint32_t expected_items)
{
    uint64_t need = ((uint64_t)(expected_items ? expected_items : 16u) * 8ull + 6ull) / 7ull;
    if (need > 0x40000000ull)
        return false;
    return mu_swiss_map_init_with_capacity(m, (uint32_t)need);
}

/* `ctrl_buf` MUST have at least `capacity + 16` bytes; `capacity` power of 2 >= 16. */
MU_INLINE bool mu_swiss_map_init_static(
    mu_swiss_map* m,
    uint8_t*      ctrl_buf,
    uint64_t*     keys_buf,
    uint64_t*     values_buf,
    uint32_t      capacity)
{
    MU_ASSERT(m);
    MU_MEMSET(m, 0, sizeof(*m));

    if (!ctrl_buf || !keys_buf || !values_buf)
        return false;
    if (capacity < MU_SWISS_GROUP_WIDTH || (capacity & (capacity - 1u)) != 0u)
        return false;

    MU_MEMSET(ctrl_buf, MU_SWISS_EMPTY, (size_t)capacity + MU_SWISS_GROUP_WIDTH);
    m->ctrl        = ctrl_buf;
    m->keys        = keys_buf;
    m->values      = values_buf;
    m->capacity    = capacity;
    m->mask        = capacity - 1u;
    m->count       = 0u;
    m->growth_left = (capacity * 7u) / 8u;
    m->owned       = 0u;
    return true;
}

MU_INLINE void mu_swiss_map_destroy(mu_swiss_map* m)
{
    if (!m)
        return;
    if (m->owned)
    {
        MU_FREE(m->ctrl);
        MU_FREE(m->keys);
        MU_FREE(m->values);
    }
    MU_MEMSET(m, 0, sizeof(*m));
}

MU_INLINE void mu_swiss_map_clear(mu_swiss_map* m)
{
    MU_ASSERT(m && m->ctrl);
    MU_MEMSET(m->ctrl, MU_SWISS_EMPTY, (size_t)m->capacity + MU_SWISS_GROUP_WIDTH);
    m->count       = 0u;
    m->growth_left = (m->capacity * 7u) / 8u;
}

MU_INLINE bool mu_swiss_map_get(const mu_swiss_map* m, uint64_t key, uint64_t* out_val)
{
    if (!m || m->capacity == 0u)
        return false;

    uint64_t h    = mu_swiss__hash(key);
    uint8_t  h2   = mu_swiss__h2(h);
    uint32_t mask = m->mask;
    uint32_t pos  = (uint32_t)(h >> 7) & mask;
    uint32_t step = MU_SWISS_GROUP_WIDTH;

    for (;;)
    {
        uint32_t match = mu_swiss__match_h2(m->ctrl + pos, h2);
        while (match != 0u)
        {
            uint32_t bit  = (uint32_t)mu_trailing_zeroes_u32(match);
            uint32_t slot = (pos + bit) & mask;
            if (MU_LIKELY(m->keys[slot] == key))
            {
                if (out_val)
                    *out_val = m->values[slot];
                return true;
            }
            match &= match - 1u;
        }
        if (MU_LIKELY(mu_swiss__match_empty(m->ctrl + pos) != 0u))
            return false;

        pos   = (pos + step) & mask;
        step += MU_SWISS_GROUP_WIDTH;
    }
}

MU_INLINE uint32_t mu_swiss_map__find_first_non_full(const mu_swiss_map* m, uint64_t h)
{
    uint32_t mask = m->mask;
    uint32_t pos  = (uint32_t)(h >> 7) & mask;
    uint32_t step = MU_SWISS_GROUP_WIDTH;
    for (;;)
    {
        uint32_t avail = mu_swiss__match_empty_or_deleted(m->ctrl + pos);
        if (avail != 0u)
            return (pos + (uint32_t)mu_trailing_zeroes_u32(avail)) & mask;
        pos   = (pos + step) & mask;
        step += MU_SWISS_GROUP_WIDTH;
    }
}

MU_INLINE bool mu_swiss_map_rehash(mu_swiss_map* m, uint32_t new_cap)
{
    if (!m->owned)
        return false;

    mu_swiss_map next;
    if (!mu_swiss_map_init_with_capacity(&next, new_cap))
        return false;

    for (uint32_t i = 0; i < m->capacity; ++i)
    {
        uint8_t c = m->ctrl[i];
        if ((c & 0x80u) == 0u) /* live H2 entry */
        {
            uint64_t k    = m->keys[i];
            uint64_t h    = mu_swiss__hash(k);
            uint32_t slot = mu_swiss_map__find_first_non_full(&next, h);
            mu_swiss__set_ctrl(next.ctrl, next.mask, slot, mu_swiss__h2(h));
            next.keys[slot]   = k;
            next.values[slot] = m->values[i];
            ++next.count;
            --next.growth_left;
        }
    }

    mu_swiss_map_destroy(m);
    *m = next;
    return true;
}

MU_INLINE bool mu_swiss_map_set(mu_swiss_map* m, uint64_t key, uint64_t value)
{
    MU_ASSERT(m);
    if (m->capacity == 0u && !mu_swiss_map_init(m, 16u))
        return false;

    uint64_t h    = mu_swiss__hash(key);
    uint8_t  h2   = mu_swiss__h2(h);
    uint32_t mask = m->mask;
    uint32_t pos  = (uint32_t)(h >> 7) & mask;
    uint32_t step = MU_SWISS_GROUP_WIDTH;

    for (;;)
    {
        uint32_t match = mu_swiss__match_h2(m->ctrl + pos, h2);
        while (match != 0u)
        {
            uint32_t bit  = (uint32_t)mu_trailing_zeroes_u32(match);
            uint32_t slot = (pos + bit) & mask;
            if (MU_LIKELY(m->keys[slot] == key))
            {
                m->values[slot] = value;
                return true;
            }
            match &= match - 1u;
        }
        if (mu_swiss__match_empty(m->ctrl + pos) != 0u)
            break;

        pos   = (pos + step) & mask;
        step += MU_SWISS_GROUP_WIDTH;
    }

    if (MU_UNLIKELY(m->growth_left == 0u))
    {
        /* Rehash to same size if >50% of non-live slots are tombstones, else double: */
        uint32_t next_cap = (m->count * 2u < m->capacity) ? m->capacity : (m->capacity * 2u);
        if (!mu_swiss_map_rehash(m, next_cap))
            return false;
    }

    uint32_t slot   = mu_swiss_map__find_first_non_full(m, h);
    uint8_t  prev_c = m->ctrl[slot];
    if (prev_c == MU_SWISS_EMPTY)
        --m->growth_left;

    mu_swiss__set_ctrl(m->ctrl, m->mask, slot, h2);
    m->keys[slot]   = key;
    m->values[slot] = value;
    ++m->count;
    return true;
}

MU_INLINE bool mu_swiss_map_remove(mu_swiss_map* m, uint64_t key)
{
    if (!m || m->count == 0u)
        return false;

    uint64_t h    = mu_swiss__hash(key);
    uint8_t  h2   = mu_swiss__h2(h);
    uint32_t mask = m->mask;
    uint32_t pos  = (uint32_t)(h >> 7) & mask;
    uint32_t step = MU_SWISS_GROUP_WIDTH;

    for (;;)
    {
        uint32_t match = mu_swiss__match_h2(m->ctrl + pos, h2);
        while (match != 0u)
        {
            uint32_t bit  = (uint32_t)mu_trailing_zeroes_u32(match);
            uint32_t slot = (pos + bit) & mask;
            if (MU_LIKELY(m->keys[slot] == key))
            {
                bool can_empty = mu_swiss__was_never_full(m->ctrl, mask, slot);
                uint8_t mark   = can_empty ? MU_SWISS_EMPTY : MU_SWISS_DELETED;
                if (can_empty)
                    ++m->growth_left;
                mu_swiss__set_ctrl(m->ctrl, mask, slot, mark);
                --m->count;
                return true;
            }
            match &= match - 1u;
        }
        if (mu_swiss__match_empty(m->ctrl + pos) != 0u)
            return false;

        pos   = (pos + step) & mask;
        step += MU_SWISS_GROUP_WIDTH;
    }
}

/* ========================================================================== *
 * 2. mu_dense_map — Contiguous SoA Arrays + SIMD H2 Index (`unordered_dense`)
 *
 * Iterate directly with zero branches or empty-slot skips:
 *   for (uint32_t i = 0; i < map.count; ++i) { use(map.keys[i], map.values[i]); }
 * ========================================================================== */

typedef struct mu_dense_map
{
    uint8_t*  ctrl;           /* size: bucket_cap + 16  */
    uint32_t* slots;          /* size: bucket_cap       */
    uint64_t* keys;           /* dense [0 .. count)     */
    uint64_t* values;         /* dense [0 .. count)     */
    uint32_t  bucket_cap;     /* power of two >= 16     */
    uint32_t  bucket_mask;
    uint32_t  dense_cap;
    uint32_t  count;
    uint32_t  growth_left;
    uint32_t  owned;
} mu_dense_map;

MU_INLINE bool mu_dense_map_init(mu_dense_map* m, uint32_t expected_items)
{
    MU_ASSERT(m);
    MU_MEMSET(m, 0, sizeof(*m));

    uint64_t need_b = ((uint64_t)(expected_items ? expected_items : 16u) * 8ull + 6ull) / 7ull;
    if (need_b > 0x40000000ull)
        return false;

    uint32_t bcap = mu_swiss__round_cap((uint32_t)need_b);
    uint32_t dcap = (bcap * 7u) / 8u;

    uint8_t*  ctrl  = (uint8_t*)MU_MALLOC((size_t)bcap + MU_SWISS_GROUP_WIDTH);
    uint32_t* slots = (uint32_t*)MU_MALLOC((size_t)bcap * sizeof(uint32_t));
    uint64_t* keys  = (uint64_t*)MU_MALLOC((size_t)dcap * sizeof(uint64_t));
    uint64_t* vals  = (uint64_t*)MU_MALLOC((size_t)dcap * sizeof(uint64_t));
    if (!ctrl || !slots || !keys || !vals)
    {
        MU_FREE(ctrl);
        MU_FREE(slots);
        MU_FREE(keys);
        MU_FREE(vals);
        return false;
    }

    MU_MEMSET(ctrl, MU_SWISS_EMPTY, (size_t)bcap + MU_SWISS_GROUP_WIDTH);
    m->ctrl        = ctrl;
    m->slots       = slots;
    m->keys        = keys;
    m->values      = vals;
    m->bucket_cap  = bcap;
    m->bucket_mask = bcap - 1u;
    m->dense_cap   = dcap;
    m->count       = 0u;
    m->growth_left = dcap;
    m->owned       = 1u;
    return true;
}

MU_INLINE bool mu_dense_map_init_static(
    mu_dense_map* m,
    uint8_t*      ctrl_buf,   /* >= bucket_cap + 16 */
    uint32_t*     slots_buf,  /* >= bucket_cap      */
    uint64_t*     keys_buf,   /* >= dense_cap       */
    uint64_t*     values_buf, /* >= dense_cap       */
    uint32_t      bucket_cap,
    uint32_t      dense_cap)
{
    MU_ASSERT(m);
    MU_MEMSET(m, 0, sizeof(*m));

    if (!ctrl_buf || !slots_buf || !keys_buf || !values_buf)
        return false;
    if (bucket_cap < MU_SWISS_GROUP_WIDTH || (bucket_cap & (bucket_cap - 1u)) != 0u)
        return false;

    uint32_t max_load = (bucket_cap * 7u) / 8u;
    if (dense_cap == 0u || dense_cap > max_load)
        dense_cap = max_load;

    MU_MEMSET(ctrl_buf, MU_SWISS_EMPTY, (size_t)bucket_cap + MU_SWISS_GROUP_WIDTH);
    m->ctrl        = ctrl_buf;
    m->slots       = slots_buf;
    m->keys        = keys_buf;
    m->values      = values_buf;
    m->bucket_cap  = bucket_cap;
    m->bucket_mask = bucket_cap - 1u;
    m->dense_cap   = dense_cap;
    m->count       = 0u;
    m->growth_left = dense_cap;
    m->owned       = 0u;
    return true;
}

MU_INLINE void mu_dense_map_destroy(mu_dense_map* m)
{
    if (!m)
        return;
    if (m->owned)
    {
        MU_FREE(m->ctrl);
        MU_FREE(m->slots);
        MU_FREE(m->keys);
        MU_FREE(m->values);
    }
    MU_MEMSET(m, 0, sizeof(*m));
}

MU_INLINE void mu_dense_map_clear(mu_dense_map* m)
{
    MU_ASSERT(m && m->ctrl);
    MU_MEMSET(m->ctrl, MU_SWISS_EMPTY, (size_t)m->bucket_cap + MU_SWISS_GROUP_WIDTH);
    m->count       = 0u;
    m->growth_left = m->dense_cap;
}

MU_INLINE bool mu_dense_map_get(const mu_dense_map* m, uint64_t key, uint64_t* out_val)
{
    if (!m || m->bucket_cap == 0u)
        return false;

    uint64_t h    = mu_swiss__hash(key);
    uint8_t  h2   = mu_swiss__h2(h);
    uint32_t mask = m->bucket_mask;
    uint32_t pos  = (uint32_t)(h >> 7) & mask;
    uint32_t step = MU_SWISS_GROUP_WIDTH;

    for (;;)
    {
        uint32_t match = mu_swiss__match_h2(m->ctrl + pos, h2);
        while (match != 0u)
        {
            uint32_t bit  = (uint32_t)mu_trailing_zeroes_u32(match);
            uint32_t bslot = (pos + bit) & mask;
            uint32_t didx  = m->slots[bslot];
            if (MU_LIKELY(m->keys[didx] == key))
            {
                if (out_val)
                    *out_val = m->values[didx];
                return true;
            }
            match &= match - 1u;
        }
        if (MU_LIKELY(mu_swiss__match_empty(m->ctrl + pos) != 0u))
            return false;

        pos   = (pos + step) & mask;
        step += MU_SWISS_GROUP_WIDTH;
    }
}

MU_INLINE uint32_t mu_dense_map__find_non_full(const mu_dense_map* m, uint64_t h)
{
    uint32_t mask = m->bucket_mask;
    uint32_t pos  = (uint32_t)(h >> 7) & mask;
    uint32_t step = MU_SWISS_GROUP_WIDTH;
    for (;;)
    {
        uint32_t avail = mu_swiss__match_empty_or_deleted(m->ctrl + pos);
        if (avail != 0u)
            return (pos + (uint32_t)mu_trailing_zeroes_u32(avail)) & mask;
        pos   = (pos + step) & mask;
        step += MU_SWISS_GROUP_WIDTH;
    }
}

MU_INLINE bool mu_dense_map_rehash(mu_dense_map* m, uint32_t new_bcap)
{
    if (!m->owned)
        return false;

    uint32_t bcap = mu_swiss__round_cap(new_bcap);
    if (bcap == 0u)
        return false;
    uint32_t dcap = (bcap * 7u) / 8u;

    uint8_t*  nctrl  = (uint8_t*)MU_MALLOC((size_t)bcap + MU_SWISS_GROUP_WIDTH);
    uint32_t* nslots = (uint32_t*)MU_MALLOC((size_t)bcap * sizeof(uint32_t));
    if (!nctrl || !nslots)
    {
        MU_FREE(nctrl);
        MU_FREE(nslots);
        return false;
    }

    if (dcap > m->dense_cap)
    {
        uint64_t* nkeys = (uint64_t*)MU_REALLOC(m->keys, (size_t)dcap * sizeof(uint64_t));
        if (!nkeys)
        {
            MU_FREE(nctrl);
            MU_FREE(nslots);
            return false;
        }
        m->keys = nkeys;

        uint64_t* nvals = (uint64_t*)MU_REALLOC(m->values, (size_t)dcap * sizeof(uint64_t));
        if (!nvals)
        {
            MU_FREE(nctrl);
            MU_FREE(nslots);
            return false;
        }
        m->values    = nvals;
        m->dense_cap = dcap;
    }

    MU_MEMSET(nctrl, MU_SWISS_EMPTY, (size_t)bcap + MU_SWISS_GROUP_WIDTH);
    MU_FREE(m->ctrl);
    MU_FREE(m->slots);
    m->ctrl        = nctrl;
    m->slots       = nslots;
    m->bucket_cap  = bcap;
    m->bucket_mask = bcap - 1u;
    m->growth_left = m->dense_cap - m->count;

    /* Sequential re-index over dense keys[0..count) — blazing fast! */
    for (uint32_t i = 0; i < m->count; ++i)
    {
        uint64_t h     = mu_swiss__hash(m->keys[i]);
        uint32_t bslot = mu_dense_map__find_non_full(m, h);
        mu_swiss__set_ctrl(m->ctrl, m->bucket_mask, bslot, mu_swiss__h2(h));
        m->slots[bslot] = i;
    }
    return true;
}

MU_INLINE bool mu_dense_map_set(mu_dense_map* m, uint64_t key, uint64_t value)
{
    MU_ASSERT(m);
    if (m->bucket_cap == 0u && !mu_dense_map_init(m, 16u))
        return false;

    uint64_t h    = mu_swiss__hash(key);
    uint8_t  h2   = mu_swiss__h2(h);
    uint32_t mask = m->bucket_mask;
    uint32_t pos  = (uint32_t)(h >> 7) & mask;
    uint32_t step = MU_SWISS_GROUP_WIDTH;

    for (;;)
    {
        uint32_t match = mu_swiss__match_h2(m->ctrl + pos, h2);
        while (match != 0u)
        {
            uint32_t bit   = (uint32_t)mu_trailing_zeroes_u32(match);
            uint32_t bslot = (pos + bit) & mask;
            uint32_t didx  = m->slots[bslot];
            if (MU_LIKELY(m->keys[didx] == key))
            {
                m->values[didx] = value;
                return true;
            }
            match &= match - 1u;
        }
        if (mu_swiss__match_empty(m->ctrl + pos) != 0u)
            break;

        pos   = (pos + step) & mask;
        step += MU_SWISS_GROUP_WIDTH;
    }

    if (MU_UNLIKELY(m->growth_left == 0u || m->count >= m->dense_cap))
    {
        uint32_t next_bcap = (m->count * 2u < m->bucket_cap) ? m->bucket_cap : (m->bucket_cap * 2u);
        if (!mu_dense_map_rehash(m, next_bcap))
            return false;
    }

    uint32_t bslot = mu_dense_map__find_non_full(m, h);
    if (m->ctrl[bslot] == MU_SWISS_EMPTY)
        --m->growth_left;

    uint32_t didx         = m->count++;
    m->keys[didx]         = key;
    m->values[didx]       = value;
    m->slots[bslot]       = didx;
    mu_swiss__set_ctrl(m->ctrl, m->bucket_mask, bslot, h2);
    return true;
}

MU_INLINE bool mu_dense_map_remove(mu_dense_map* m, uint64_t key)
{
    if (!m || m->count == 0u)
        return false;

    uint64_t h    = mu_swiss__hash(key);
    uint8_t  h2   = mu_swiss__h2(h);
    uint32_t mask = m->bucket_mask;
    uint32_t pos  = (uint32_t)(h >> 7) & mask;
    uint32_t step = MU_SWISS_GROUP_WIDTH;

    for (;;)
    {
        uint32_t match = mu_swiss__match_h2(m->ctrl + pos, h2);
        while (match != 0u)
        {
            uint32_t bit   = (uint32_t)mu_trailing_zeroes_u32(match);
            uint32_t bslot = (pos + bit) & mask;
            uint32_t didx  = m->slots[bslot];
            if (MU_LIKELY(m->keys[didx] == key))
            {
                bool can_empty = mu_swiss__was_never_full(m->ctrl, mask, bslot);
                if (can_empty)
                    ++m->growth_left;
                mu_swiss__set_ctrl(m->ctrl, mask, bslot, can_empty ? MU_SWISS_EMPTY : MU_SWISS_DELETED);

                uint32_t last = --m->count;
                if (didx != last)
                {
                    uint64_t moved_k = m->keys[last];
                    m->keys[didx]    = moved_k;
                    m->values[didx]  = m->values[last];

                    /* Update moved element's slot index in O(1): */
                    uint64_t mh    = mu_swiss__hash(moved_k);
                    uint8_t  mh2   = mu_swiss__h2(mh);
                    uint32_t mpos  = (uint32_t)(mh >> 7) & mask;
                    uint32_t mstep = MU_SWISS_GROUP_WIDTH;
                    for (;;)
                    {
                        uint32_t mm = mu_swiss__match_h2(m->ctrl + mpos, mh2);
                        while (mm != 0u)
                        {
                            uint32_t mb    = (uint32_t)mu_trailing_zeroes_u32(mm);
                            uint32_t mslot = (mpos + mb) & mask;
                            if (m->slots[mslot] == last)
                            {
                                m->slots[mslot] = didx;
                                return true;
                            }
                            mm &= mm - 1u;
                        }
                        mpos   = (mpos + mstep) & mask;
                        mstep += MU_SWISS_GROUP_WIDTH;
                    }
                }
                return true;
            }
            match &= match - 1u;
        }
        if (mu_swiss__match_empty(m->ctrl + pos) != 0u)
            return false;

        pos   = (pos + step) & mask;
        step += MU_SWISS_GROUP_WIDTH;
    }
}

MU_END_EXTERN_C

#endif /* MU_SWISS_MAP_H */
