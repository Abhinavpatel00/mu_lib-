#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_hier_bitset — hierarchical / multi-level bitset (2+ level summary bitmap).
 *
 * WHY
 *   A flat bitset answers "is anything set?" and "find the first set bit" in
 *   O(bits). For a sparse domain (millions of IDs, most never touched) that is
 *   a full scan of cold memory.
 *
 *   A hierarchical bitset keeps a summary word above every 64 payload words:
 *
 *       level 0 (payload)   [ 64b ][ 64b ][ 64b ] ...   <-- the actual bits
 *       level 1 (summary)   [   marks which level-0 words are non-zero   ]
 *       level 2 (summary)   [   marks which level-1 words are non-zero   ]
 *       ...
 *       root                [   one word tells you "anything set at all"  ]
 *
 *   Levels are stored back to back in ONE contiguous allocation, so there is
 *   no pointer chasing and no per-level malloc.
 *
 * WHAT YOU GET
 *   any()        O(top level)     ~O(1): skip empties from the root down.
 *   find_first() O(levels)        ctz descent, no scanning of empty blocks.
 *   find_next()  O(levels)        ascend until a set bit to the right, descend.
 *   set/reset    O(levels)        updates the summary chain on 0<->nonzero only.
 *
 * CONTRACT
 *   - FIXED capacity: size is chosen at init. Indices are uint32_t.
 *   - Not thread safe. Not growable (use mu_chunked_bitset for that).
 *   - find_* return MU_INVALID_INDEX when no bit is set.
 *   - Use _init_static for hot paths so nothing touches the heap.
 *
 * ITERATION (the cheap way):
 *     for (uint32_t i = mu_hier_bitset_find_first(&bs);
 *          i != MU_INVALID_INDEX;
 *          i = mu_hier_bitset_find_next(&bs, i))
 *         work(i);
 * --------------------------------------------------------------------------- */

#ifndef MU_HIER_BITSET_MAX_LEVELS
#define MU_HIER_BITSET_MAX_LEVELS 8u
#endif

typedef bool (*mu_hier_bitset_visit_fn)(uint32_t value, void* user);

typedef struct mu_hier_bitset
{
    uint64_t* words;                           /* all levels, contiguous          */
    uint32_t  words_count;                     /* total words allocated           */
    uint32_t  bit_count;                       /* capacity in bits                */
    uint32_t  level_count;                     /* 1 == payload only (tiny sets)   */
    uint32_t  base[MU_HIER_BITSET_MAX_LEVELS]; /* first word index of each level  */
    uint32_t  count[MU_HIER_BITSET_MAX_LEVELS];/* word count of each level        */
    uint32_t  owned;                           /* 1 = heap buffer we may free     */
} mu_hier_bitset;

/* Internal: derive the level layout (bases/counts) for `bits`. */
MU_INLINE void mu_hier_bitset__layout(mu_hier_bitset* bs, uint32_t bits)
{
    uint32_t w = (uint32_t)(((uint64_t)bits + 63ull) / 64ull);
    if (w == 0u)
        w = 1u;

    uint32_t lv    = 0;
    uint32_t total = w;

    bs->base[0]  = 0;
    bs->count[0] = w;

    while (w > 1u && lv + 1u < MU_HIER_BITSET_MAX_LEVELS)
    {
        w = (w + 63u) / 64u;
        ++lv;
        bs->base[lv]  = total;
        bs->count[lv] = w;
        total += w;
    }

    bs->level_count = lv + 1u;
    bs->words_count = total;
    bs->bit_count   = bits;
}

/* Words needed for a static buffer holding `bits` bits (all levels). */
MU_INLINE uint32_t mu_hier_bitset_words_needed(uint32_t bits)
{
    mu_hier_bitset tmp;
    mu_hier_bitset__layout(&tmp, bits);
    return tmp.words_count;
}

MU_INLINE bool mu_hier_bitset_init(mu_hier_bitset* bs, uint32_t bits)
{
    MU_ASSERT(bs);
    mu_hier_bitset__layout(bs, bits);

    bs->owned = 1;
    bs->words = (uint64_t*)MU_MALLOC((size_t)bs->words_count * sizeof(uint64_t));
    if (!bs->words)
    {
        bs->words_count = 0;
        bs->level_count = 0;
        bs->bit_count   = 0;
        bs->owned       = 0;
        return false;
    }

    MU_MEMSET(bs->words, 0, (size_t)bs->words_count * sizeof(uint64_t));
    return true;
}

/* Caller-provided storage. Size it with mu_hier_bitset_words_needed(bits).
   Never allocates; set()/reset() still work, they just cannot fail to fit. */
MU_INLINE bool mu_hier_bitset_init_static(mu_hier_bitset* bs, uint64_t* words, uint32_t words_capacity, uint32_t bits)
{
    MU_ASSERT(bs);
    mu_hier_bitset__layout(bs, bits);

    if (!words || words_capacity < bs->words_count)
    {
        bs->words        = NULL;
        bs->words_count  = 0;
        bs->level_count  = 0;
        bs->bit_count    = 0;
        bs->owned        = 0;
        return false;
    }

    bs->words = words;
    bs->owned = 0;
    MU_MEMSET(bs->words, 0, (size_t)bs->words_count * sizeof(uint64_t));
    return true;
}

MU_INLINE void mu_hier_bitset_destroy(mu_hier_bitset* bs)
{
    if (!bs)
        return;
    if (bs->owned)
        MU_FREE(bs->words);
    bs->words       = NULL;
    bs->words_count = 0;
    bs->level_count = 0;
    bs->bit_count   = 0;
    bs->owned       = 0;
}

/* Set every bit to 0. Keeps capacity. */
MU_INLINE void mu_hier_bitset_clear(mu_hier_bitset* bs)
{
    if (!bs || !bs->words)
        return;
    MU_MEMSET(bs->words, 0, (size_t)bs->words_count * sizeof(uint64_t));
}

/* ------------------------------------------------------------------ *
 * Point updates — only walk the summary chain when a word flips
 * 0 -> nonzero (set) or nonzero -> 0 (reset).
 * ------------------------------------------------------------------ */

MU_INLINE void mu_hier_bitset_set(mu_hier_bitset* bs, uint32_t index)
{
    MU_ASSERT(bs);
    MU_ASSERT(bs->words);
    if (index >= bs->bit_count)
        return;

    uint32_t w    = index >> 6;
    uint64_t prev = bs->words[w];

    bs->words[w] |= (uint64_t)1 << (index & 63u);
    if (prev != 0u)
        return; /* summary above already correct */

    /* Word became non-empty: mark it in every ancestor. */
    for (uint32_t lv = 1; lv < bs->level_count; ++lv)
    {
        uint32_t ci    = w; /* child index at level lv-1 */
        uint32_t pw_idx = ci >> 6;
        uint64_t pold   = bs->words[bs->base[lv] + pw_idx];

        bs->words[bs->base[lv] + pw_idx] |= (uint64_t)1 << (ci & 63u);
        if (pold != 0u)
            break; /* ancestor already non-empty, chain stops */

        w = pw_idx;
    }
}

MU_INLINE void mu_hier_bitset_reset(mu_hier_bitset* bs, uint32_t index)
{
    MU_ASSERT(bs);
    if (!bs->words || index >= bs->bit_count)
        return;

    uint32_t w    = index >> 6;
    uint64_t prev = bs->words[w];

    bs->words[w] &= ~((uint64_t)1 << (index & 63u));
    if (prev == 0u || bs->words[w] != 0u)
        return; /* was already empty, or still has bits set */

    /* Word became empty: clear it in every ancestor that empties too. */
    for (uint32_t lv = 1; lv < bs->level_count; ++lv)
    {
        uint32_t ci     = w;
        uint32_t pw_idx = ci >> 6;
        uint64_t pw     = bs->words[bs->base[lv] + pw_idx];

        pw &= ~((uint64_t)1 << (ci & 63u));
        bs->words[bs->base[lv] + pw_idx] = pw;
        if (pw != 0u)
            break; /* ancestor still non-empty, chain stops */

        w = pw_idx;
    }
}

MU_INLINE bool mu_hier_bitset_test(const mu_hier_bitset* bs, uint32_t index)
{
    if (!bs || !bs->words || index >= bs->bit_count)
        return false;
    return (bs->words[index >> 6] >> (index & 63u)) & 1u;
}

/* ------------------------------------------------------------------ *
 * Queries
 * ------------------------------------------------------------------ */

MU_INLINE bool mu_hier_bitset_any(const mu_hier_bitset* bs)
{
    if (!bs || !bs->words || bs->level_count == 0)
        return false;

    uint32_t lv = bs->level_count - 1u;
    for (uint32_t i = 0; i < bs->count[lv]; ++i)
        if (bs->words[bs->base[lv] + i] != 0u)
            return true;
    return false;
}

MU_INLINE bool mu_hier_bitset_empty(const mu_hier_bitset* bs)
{
    return !mu_hier_bitset_any(bs);
}

MU_INLINE uint32_t mu_hier_bitset_capacity(const mu_hier_bitset* bs)
{
    return bs ? bs->bit_count : 0u;
}

/* Set-bit count. Skips empty payload words using the summary level. */
MU_INLINE uint64_t mu_hier_bitset_popcount(const mu_hier_bitset* bs)
{
    if (!bs || !bs->words)
        return 0;

    uint64_t total = 0;

    if (bs->level_count > 1u)
    {
        for (uint32_t i = 0; i < bs->count[1]; ++i)
        {
            uint64_t s = bs->words[bs->base[1] + i];
            while (s != 0u)
            {
                uint32_t b = (uint32_t)mu_trailing_zeroes_u64(s);
                uint32_t w = i * 64u + b;
                if (w < bs->count[0])
                    total += mu_popcount_u64(bs->words[w]);
                s &= s - 1u;
            }
        }
        return total;
    }

    for (uint32_t w = 0; w < bs->count[0]; ++w)
        total += mu_popcount_u64(bs->words[w]);
    return total;
}

/* ------------------------------------------------------------------ *
 * Search — first set word at `level` with index >= `from`.
 *
 * Ascends the summary chain until an ancestor has a set bit to the right of
 * `from`, then descends with ctz(). Bounded by level_count, not by the size
 * of the domain. Internal.
 * ------------------------------------------------------------------ */
MU_INLINE bool mu_hier_bitset__first_at(const mu_hier_bitset* bs, uint32_t level, uint32_t from, uint32_t* out)
{
    uint32_t lv  = level;
    uint32_t idx = from;

    for (;;)
    {
        if (idx >= bs->count[lv])
            return false;

        if (lv + 1u >= bs->level_count)
        {
            /* Top level: linear scan here, then descend back to `level`. */
            uint32_t w = idx;
            while (w < bs->count[lv] && bs->words[bs->base[lv] + w] == 0u)
                ++w;
            if (w >= bs->count[lv])
                return false;

            while (lv > level)
            {
                uint64_t word = bs->words[bs->base[lv] + w];
                w             = w * 64u + (uint32_t)mu_trailing_zeroes_u64(word);
                --lv;
            }
            *out = w;
            return true;
        }

        {
            uint64_t pw = bs->words[bs->base[lv + 1u] + (idx >> 6)];
            uint64_t m  = ~(uint64_t)0 << (idx & 63u);
            pw &= m;

            if (pw == 0u)
            {
                /* Nothing at or after idx in this parent word; step past it. */
                idx = (idx >> 6) + 1u;
                ++lv;
                continue;
            }

            idx = (idx >> 6) * 64u + (uint32_t)mu_trailing_zeroes_u64(pw);
            while (lv > level)
            {
                uint64_t word = bs->words[bs->base[lv] + idx];
                idx           = idx * 64u + (uint32_t)mu_trailing_zeroes_u64(word);
                --lv;
            }
            *out = idx;
            return true;
        }
    }
}

/* First set bit >= 0, or MU_INVALID_INDEX. */
MU_INLINE uint32_t mu_hier_bitset_find_first(const mu_hier_bitset* bs)
{
    if (!bs || !bs->words || bs->bit_count == 0)
        return MU_INVALID_INDEX;

    uint32_t w;
    if (!mu_hier_bitset__first_at(bs, 0, 0, &w))
        return MU_INVALID_INDEX;

    return w * 64u + (uint32_t)mu_trailing_zeroes_u64(bs->words[w]);
}

/* First set bit strictly greater than `after`, or MU_INVALID_INDEX. */
MU_INLINE uint32_t mu_hier_bitset_find_next(const mu_hier_bitset* bs, uint32_t after)
{
    if (!bs || !bs->words || bs->bit_count == 0)
        return MU_INVALID_INDEX;

    uint64_t start = (uint64_t)after + 1ull;
    if (start >= (uint64_t)bs->bit_count)
        return MU_INVALID_INDEX;

    uint32_t word_index = (uint32_t)start >> 6;
    uint64_t rem        = bs->words[word_index] & (~(uint64_t)0 << (start & 63ull));
    if (rem != 0u)
        return word_index * 64u + (uint32_t)mu_trailing_zeroes_u64(rem);

    uint32_t w;
    if (!mu_hier_bitset__first_at(bs, 0, word_index + 1u, &w))
        return MU_INVALID_INDEX;

    return w * 64u + (uint32_t)mu_trailing_zeroes_u64(bs->words[w]);
}

/* Visitor callback: return false to stop early. */
MU_INLINE bool mu_hier_bitset_for_each(const mu_hier_bitset* bs, mu_hier_bitset_visit_fn visitor, void* user)
{
    if (!visitor)
        return false;

    for (uint32_t i = mu_hier_bitset_find_first(bs); i != MU_INVALID_INDEX;
         i           = mu_hier_bitset_find_next(bs, i))
    {
        if (!visitor(i, user))
            return false;
    }
    return true;
}
