#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_chunked_bitset — a bitset that grows in fixed, cache-line-sized chunks.
 *
 * WHY
 *   mu_bitset stores one flat word array. Growing it means one big realloc and
 *   a full copy: O(n) traffic and every pointer/index into the old array is
 *   invalidated.
 *
 *   A chunked bitset grows by allocating one more chunk (CHUNK_WORDS words).
 *   Existing chunks never move, so existing byte offsets stay valid and growth
 *   is O(new chunk), not O(total).
 *
 *   A one-bit-per-chunk SUMMARY word sits alongside the payload, so
 *   find_first() / popcount() / any() skip whole empty chunks instead of
 *   touching every word:
 *
 *       payload  [chunk0: 8 words][chunk1: 8 words][chunk2: 8 words] ...
 *       summary  [  bits marking which chunks are non-empty            ]
 *
 *   CHUNK_BITS defaults to 512 (8 words = one 64-byte cache line), so a chunk
 *   is exactly one line: one fill, one popcount, one line of memory traffic.
 *
 * CONTRACT
 *   - Indices are uint32_t. Capacity grows on demand (owned buffers) or is
 *     fixed up-front (init_static, which never allocates).
 *   - find_first() returns the first set bit >= `from`, else MU_INVALID_INDEX.
 *   - Not thread safe.
 *
 * ITERATION:
 *     for (uint32_t i = mu_chunked_bitset_find_first(&bs, 0);
 *          i != MU_INVALID_INDEX;
 *          i = mu_chunked_bitset_find_next(&bs, i))
 *         work(i);
 * --------------------------------------------------------------------------- */

#ifndef MU_CHUNKED_BITSET_CHUNK_BITS
#define MU_CHUNKED_BITSET_CHUNK_BITS 512u /* 8 words == one 64-byte cache line */
#endif

#define MU_CHUNKED_BITSET_CHUNK_WORDS (MU_CHUNKED_BITSET_CHUNK_BITS / 64u)

typedef bool (*mu_chunked_bitset_visit_fn)(uint32_t value, void* user);

typedef struct mu_chunked_bitset
{
    uint64_t* words;          /* chunk_capacity * CHUNK_WORDS words          */
    uint64_t* summary;        /* ceil(chunk_capacity/64) words               */
    uint32_t  chunk_count;    /* logical chunks covering bit_count            */
    uint32_t  chunk_capacity; /* allocated chunks                             */
    uint32_t  bit_count;      /* logical bits (indices < bit_count)           */
    uint32_t  owned;          /* 1 = heap buffers we may free/grow             */
} mu_chunked_bitset;

/* Internal: chunks required to hold `bits` bits. */
MU_INLINE uint32_t mu_chunked_bitset__chunks_for(uint32_t bits)
{
    return (uint32_t)(((uint64_t)bits + (MU_CHUNKED_BITSET_CHUNK_BITS - 1u)) / MU_CHUNKED_BITSET_CHUNK_BITS);
}

/* Payload words for a static buffer holding `bits` bits. */
MU_INLINE uint32_t mu_chunked_bitset_words_needed(uint32_t bits)
{
    uint32_t chunks = mu_chunked_bitset__chunks_for(bits);
    if (chunks == 0u)
        chunks = 1u;
    return chunks * MU_CHUNKED_BITSET_CHUNK_WORDS;
}

/* Summary words for a static buffer holding `bits` bits. */
MU_INLINE uint32_t mu_chunked_bitset_summary_words_needed(uint32_t bits)
{
    uint32_t chunks = mu_chunked_bitset__chunks_for(bits);
    if (chunks == 0u)
        chunks = 1u;
    return (chunks + 63u) / 64u;
}

/* Internal: grow owned buffers so `chunk` fits. Summary first, so a failure
   leaves capacity unchanged rather than the summary undersized. */
MU_INLINE bool mu_chunked_bitset__ensure(mu_chunked_bitset* bs, uint32_t chunk)
{
    if (chunk < bs->chunk_capacity)
        return true;
    if (!bs->owned)
        return false; /* static buffer, out of room */

    uint32_t old_cap = bs->chunk_capacity;
    uint32_t cap     = old_cap ? old_cap : 8u;
    while (cap <= chunk)
    {
        if (cap > 0x40000000u)
            return false;
        cap <<= 1;
    }

    uint32_t  old_sw = (old_cap + 63u) / 64u;
    uint32_t  sw     = (cap + 63u) / 64u;
    uint64_t* sum    = (uint64_t*)MU_REALLOC(bs->summary, (size_t)sw * sizeof(uint64_t));
    if (!sum)
        return false;
    MU_MEMSET(sum + old_sw, 0, (size_t)(sw - old_sw) * sizeof(uint64_t));
    bs->summary = sum;

    size_t    old_nw = (size_t)old_cap * MU_CHUNKED_BITSET_CHUNK_WORDS;
    size_t    new_nw = (size_t)cap * MU_CHUNKED_BITSET_CHUNK_WORDS;
    uint64_t* words  = (uint64_t*)MU_REALLOC(bs->words, new_nw * sizeof(uint64_t));
    if (!words)
        return false;
    MU_MEMSET(words + old_nw, 0, (new_nw - old_nw) * sizeof(uint64_t));

    bs->words         = words;
    bs->chunk_capacity = cap;
    return true;
}

MU_INLINE void mu_chunked_bitset_init(mu_chunked_bitset* bs)
{
    MU_ASSERT(bs);
    bs->words         = NULL;
    bs->summary       = NULL;
    bs->chunk_count   = 0;
    bs->chunk_capacity = 0;
    bs->bit_count     = 0;
    bs->owned         = 1;
}

/* Caller-provided buffers (one payload, one summary). Never allocates.
   Size them with mu_chunked_bitset_words_needed() /
   mu_chunked_bitset_summary_words_needed() for the same `bits`. */
MU_INLINE bool mu_chunked_bitset_init_static(
    mu_chunked_bitset* bs, uint64_t* words, uint64_t* summary, uint32_t chunk_capacity, uint32_t bits)
{
    MU_ASSERT(bs);
    if (!words || !summary || chunk_capacity == 0u)
    {
        mu_chunked_bitset_init(bs);
        bs->owned = 0;
        return false;
    }

    uint32_t sw = (chunk_capacity + 63u) / 64u;

    bs->words         = words;
    bs->summary       = summary;
    bs->chunk_capacity = chunk_capacity;
    bs->chunk_count   = 0;
    bs->bit_count     = bits;
    bs->owned         = 0;

    MU_MEMSET(bs->words, 0, (size_t)chunk_capacity * MU_CHUNKED_BITSET_CHUNK_WORDS * sizeof(uint64_t));
    MU_MEMSET(bs->summary, 0, (size_t)sw * sizeof(uint64_t));
    return true;
}

MU_INLINE void mu_chunked_bitset_destroy(mu_chunked_bitset* bs)
{
    if (!bs)
        return;
    if (bs->owned)
    {
        MU_FREE(bs->words);
        MU_FREE(bs->summary);
    }
    bs->words          = NULL;
    bs->summary        = NULL;
    bs->chunk_count    = 0;
    bs->chunk_capacity = 0;
    bs->bit_count      = 0;
    bs->owned          = 0;
}

/* Ensure room for `bits` bits without touching them. */
MU_INLINE bool mu_chunked_bitset_reserve(mu_chunked_bitset* bs, uint32_t bits)
{
    MU_ASSERT(bs);
    uint32_t chunks = mu_chunked_bitset__chunks_for(bits);
    if (chunks == 0u)
        return true;
    if (!mu_chunked_bitset__ensure(bs, chunks - 1u))
        return false;
    if (chunks > bs->chunk_count)
        bs->chunk_count = chunks;
    if (bits > bs->bit_count)
        bs->bit_count = bits;
    return true;
}

/* ------------------------------------------------------------------ *
 * Point updates
 * ------------------------------------------------------------------ */

MU_INLINE void mu_chunked_bitset_set(mu_chunked_bitset* bs, uint32_t index)
{
    MU_ASSERT(bs);
    uint32_t chunk = index / MU_CHUNKED_BITSET_CHUNK_BITS;
    if (!mu_chunked_bitset__ensure(bs, chunk))
        return; /* OOM or static buffer full */

    uint64_t* cw = bs->words + (size_t)chunk * MU_CHUNKED_BITSET_CHUNK_WORDS;
    uint32_t  w  = (index / 64u) % MU_CHUNKED_BITSET_CHUNK_WORDS;

    cw[w] |= (uint64_t)1 << (index & 63u);

    /* Summary is idempotent: one read-modify-write, branchless. */
    uint32_t si = chunk >> 6;
    bs->summary[si] |= (uint64_t)1 << (chunk & 63u);

    if (chunk + 1u > bs->chunk_count)
        bs->chunk_count = chunk + 1u;
    if (index + 1u > bs->bit_count)
        bs->bit_count = index + 1u;
}

MU_INLINE void mu_chunked_bitset_reset(mu_chunked_bitset* bs, uint32_t index)
{
    MU_ASSERT(bs);
    if (!bs->words || index >= bs->bit_count)
        return;

    uint32_t chunk = index / MU_CHUNKED_BITSET_CHUNK_BITS;
    if (chunk >= bs->chunk_count)
        return;

    uint64_t* cw = bs->words + (size_t)chunk * MU_CHUNKED_BITSET_CHUNK_WORDS;
    uint32_t  w  = (index / 64u) % MU_CHUNKED_BITSET_CHUNK_WORDS;

    cw[w] &= ~((uint64_t)1 << (index & 63u));

    /* Clear the summary bit only when the whole chunk emptied. */
    uint64_t acc = 0;
    for (uint32_t i = 0; i < MU_CHUNKED_BITSET_CHUNK_WORDS; ++i)
        acc |= cw[i];
    if (acc == 0u)
        bs->summary[chunk >> 6] &= ~((uint64_t)1 << (chunk & 63u));
}

MU_INLINE bool mu_chunked_bitset_test(const mu_chunked_bitset* bs, uint32_t index)
{
    if (!bs || !bs->words || index >= bs->bit_count)
        return false;
    uint32_t chunk = index / MU_CHUNKED_BITSET_CHUNK_BITS;
    if (chunk >= bs->chunk_count)
        return false;
    const uint64_t* cw = bs->words + (size_t)chunk * MU_CHUNKED_BITSET_CHUNK_WORDS;
    return (cw[(index / 64u) % MU_CHUNKED_BITSET_CHUNK_WORDS] >> (index & 63u)) & 1u;
}

/* Wipe everything back to an empty (but still allocated) bitset. */
MU_INLINE void mu_chunked_bitset_clear(mu_chunked_bitset* bs)
{
    if (!bs || !bs->words)
        return;
    MU_MEMSET(bs->words, 0, (size_t)bs->chunk_capacity * MU_CHUNKED_BITSET_CHUNK_WORDS * sizeof(uint64_t));
    MU_MEMSET(bs->summary, 0, (size_t)((bs->chunk_capacity + 63u) / 64u) * sizeof(uint64_t));
    bs->chunk_count = 0;
    bs->bit_count   = 0;
}

/* ------------------------------------------------------------------ *
 * Queries
 * ------------------------------------------------------------------ */

MU_INLINE bool mu_chunked_bitset_any(const mu_chunked_bitset* bs)
{
    if (!bs || !bs->summary || bs->chunk_count == 0u)
        return false;
    uint32_t sw = (bs->chunk_count + 63u) / 64u;
    for (uint32_t i = 0; i < sw; ++i)
        if (bs->summary[i] != 0u)
            return true;
    return false;
}

MU_INLINE bool mu_chunked_bitset_empty(const mu_chunked_bitset* bs)
{
    return !mu_chunked_bitset_any(bs);
}

MU_INLINE uint32_t mu_chunked_bitset_capacity(const mu_chunked_bitset* bs)
{
    return bs ? bs->bit_count : 0u;
}

MU_INLINE uint32_t mu_chunked_bitset_chunk_count(const mu_chunked_bitset* bs)
{
    return bs ? bs->chunk_count : 0u;
}

/* Set-bit count. Driven by the summary so empty chunks cost one bit test. */
MU_INLINE uint64_t mu_chunked_bitset_popcount(const mu_chunked_bitset* bs)
{
    if (!bs || !bs->words || bs->chunk_count == 0u)
        return 0;

    uint64_t total = 0;
    uint32_t sw     = (bs->chunk_count + 63u) / 64u;

    for (uint32_t i = 0; i < sw; ++i)
    {
        uint64_t s = bs->summary[i];
        while (s != 0u)
        {
            uint32_t b     = (uint32_t)mu_trailing_zeroes_u64(s);
            uint32_t chunk = i * 64u + b;
            if (chunk < bs->chunk_count)
            {
                const uint64_t* cw = bs->words + (size_t)chunk * MU_CHUNKED_BITSET_CHUNK_WORDS;
                for (uint32_t w = 0; w < MU_CHUNKED_BITSET_CHUNK_WORDS; ++w)
                    total += mu_popcount_u64(cw[w]);
            }
            s &= s - 1u;
        }
    }
    return total;
}

/* ------------------------------------------------------------------ *
 * Search
 * ------------------------------------------------------------------ */

/* First set bit >= `from`, or MU_INVALID_INDEX. */
MU_INLINE uint32_t mu_chunked_bitset_find_first(const mu_chunked_bitset* bs, uint32_t from)
{
    if (!bs || !bs->words || from >= bs->bit_count)
        return MU_INVALID_INDEX;

    uint32_t word = from / 64u;
    uint64_t rem  = bs->words[word] & (~(uint64_t)0 << (from & 63u));
    if (rem != 0u)
        return word * 64u + (uint32_t)mu_trailing_zeroes_u64(rem);

    /* Finish the current chunk, then let the summary skip the empty ones. */
    uint32_t chunk = from / MU_CHUNKED_BITSET_CHUNK_BITS;
    uint32_t w     = (word % MU_CHUNKED_BITSET_CHUNK_WORDS) + 1u;
    for (; w < MU_CHUNKED_BITSET_CHUNK_WORDS; ++w)
    {
        uint64_t x = bs->words[(size_t)chunk * MU_CHUNKED_BITSET_CHUNK_WORDS + w];
        if (x != 0u)
            return (chunk * MU_CHUNKED_BITSET_CHUNK_WORDS + w) * 64u + (uint32_t)mu_trailing_zeroes_u64(x);
    }

    ++chunk;
    while (chunk < bs->chunk_count)
    {
        uint32_t  si = chunk >> 6;
        uint64_t  s  = bs->summary[si] & (~(uint64_t)0 << (chunk & 63u));
        if (s == 0u)
        {
            chunk = (si + 1u) * 64u; /* skip the whole 64-chunk run */
            continue;
        }

        uint32_t hit = si * 64u + (uint32_t)mu_trailing_zeroes_u64(s);
        const uint64_t* cw = bs->words + (size_t)hit * MU_CHUNKED_BITSET_CHUNK_WORDS;
        for (w = 0; w < MU_CHUNKED_BITSET_CHUNK_WORDS; ++w)
        {
            if (cw[w] != 0u)
                return (hit * MU_CHUNKED_BITSET_CHUNK_WORDS + w) * 64u + (uint32_t)mu_trailing_zeroes_u64(cw[w]);
        }
        chunk = hit + 1u; /* summary was stale/never set; do not spin */
    }

    return MU_INVALID_INDEX;
}

MU_INLINE uint32_t mu_chunked_bitset_find_next(const mu_chunked_bitset* bs, uint32_t after)
{
    if (!bs)
        return MU_INVALID_INDEX;
    uint64_t start = (uint64_t)after + 1ull;
    if (start > 0xffffffffull)
        return MU_INVALID_INDEX;
    return mu_chunked_bitset_find_first(bs, (uint32_t)start);
}

MU_INLINE bool mu_chunked_bitset_for_each(const mu_chunked_bitset* bs, mu_chunked_bitset_visit_fn visitor, void* user)
{
    if (!visitor)
        return false;

    for (uint32_t i = mu_chunked_bitset_find_first(bs, 0); i != MU_INVALID_INDEX;
         i           = mu_chunked_bitset_find_next(bs, i))
    {
        if (!visitor(i, user))
            return false;
    }
    return true;
}
