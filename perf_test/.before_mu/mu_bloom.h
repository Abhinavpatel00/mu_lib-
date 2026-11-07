#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_bloom — Bloom filter (probabilistic set membership).
 *
 * WHY
 *   Two ways to answer "does this entity/chunk/asset exist?":
 *     1. probe a hash table / index  -> 1+ cache line + branch + indirection
 *     2. ask a Bloom filter          -> a handful of bit tests, no allocation,
 *                                       no indirection, returns FALSE quickly
 *
 *   A Bloom filter NEVER gives false negatives. A false positive means you
 *   fall through to the real lookup. So it is a gate in front of expensive
 *   work: asset streaming, chunk lookups, "any texture in this batch?" tests,
 *   de-duplicating batches before they reach the renderer.
 *
 * MEMORY MODEL
 *   Classic:  k probes double-hashed into one bit array of m bits.
 *             h = hash(key); i = h1; step = h2 (odd);
 *             probe n = (i + n*step) & (m-1).
 *             Odd step + power-of-two m => k distinct positions.
 *
 *   Blocked:  k probes land inside a SINGLE 64-bit cell.
 *             cell   = h1 & (cells-1)
 *             offset = f(h2), step = odd g(h2)
 *             position_i = (offset + step*i) & 63
 *             An odd step is coprime with 64, so the k positions are always
 *             distinct and sweep the WHOLE cell. Test touches ONE word
 *             instead of k scattered ones.
 *
 * SIZING (no <math.h>, all integer):
 *   bits_per_item = 10  ->  ~0.8% false positives, k = 7
 *   bits_per_item = 16  ->  ~0.03% false positives, k = 11
 *   mu_bloom_hash_count_for() computes k = bits_per_item * ln2, rounded.
 *
 * CONTRACT
 *   - Not thread safe. Fixed capacity (choose it up front).
 *   - No deletion. Need deletion? Use a counting variant / mu_bitset.
 *   - Returns FALSE for keys never added; may return TRUE for keys not added.
 *   - Use _init_static so hot paths never allocate.
 * --------------------------------------------------------------------------- */

#ifndef MU_BLOOM_MAX_HASHES
#define MU_BLOOM_MAX_HASHES 32u
#endif

/* -------------------------------------------------------------------------- *
 * Classic Bloom filter: k double-hashed probes over one bit array.
 * -------------------------------------------------------------------------- */

typedef struct mu_bloom
{
    uint64_t* words;      /* bit array, word_count words                        */
    uint32_t  word_count; /* power-of-two words                                 */
    uint32_t  hash_count; /* k                                                  */
    uint32_t  mask;       /* (word_count*64) - 1, branchless bit index wrap      */
    uint32_t  owned;      /* 1 = heap buffer we may free                        */
} mu_bloom;

/* 64-bit finalizer (MurmurHash3-style). Not cryptographic. */
MU_INLINE uint64_t mu_bloom_hash(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Internal: smallest power of two >= v, at least 64. */
MU_INLINE uint32_t mu_bloom__pow2_ceil(uint32_t v)
{
    uint32_t p = 64u;
    while (p < v)
    {
        if (p > 0x40000000u)
            return 0x80000000u;
        p <<= 1;
    }
    return p;
}

/* k for a given bits-per-item budget: k ~= bits_per_item * ln2. */
MU_INLINE uint32_t mu_bloom_hash_count_for(uint32_t bits_per_item)
{
    uint32_t k = (bits_per_item * 69u + 50u) / 100u; /* round(bits*0.693) */
    if (k < 1u)
        k = 1u;
    if (k > MU_BLOOM_MAX_HASHES)
        k = MU_BLOOM_MAX_HASHES;
    return k;
}

MU_INLINE bool mu_bloom_init(mu_bloom* bs, uint32_t bits, uint32_t hash_count)
{
    MU_ASSERT(bs);

    uint32_t m = mu_bloom__pow2_ceil(bits < 64u ? 64u : bits);

    bs->word_count = m / 64u;
    bs->hash_count = hash_count;
    if (bs->hash_count < 1u)
        bs->hash_count = 1u;
    if (bs->hash_count > MU_BLOOM_MAX_HASHES)
        bs->hash_count = MU_BLOOM_MAX_HASHES;
    bs->mask  = m - 1u;
    bs->owned = 1;

    bs->words = (uint64_t*)MU_MALLOC((size_t)bs->word_count * sizeof(uint64_t));
    if (!bs->words)
    {
        bs->word_count = 0;
        bs->mask       = 0;
        bs->owned      = 0;
        return false;
    }

    MU_MEMSET(bs->words, 0, (size_t)bs->word_count * sizeof(uint64_t));
    return true;
}

/* Sizing by budget: `items` entries at `bits_per_item` accuracy, k derived. */
MU_INLINE bool mu_bloom_init_per_item(mu_bloom* bs, uint64_t items, uint32_t bits_per_item)
{
    if (bits_per_item == 0u)
        bits_per_item = 1u;

    uint64_t total = items * (uint64_t)bits_per_item;
    if (total < 64ull)
        total = 64ull;
    if (total > 0x80000000ull)
        total = 0x80000000ull;

    return mu_bloom_init(bs, (uint32_t)total, mu_bloom_hash_count_for(bits_per_item));
}

/* Caller-provided storage. Size: (next_pow2(bits) / 64) words. Never allocates. */
MU_INLINE bool mu_bloom_init_static(mu_bloom* bs, uint64_t* words, uint32_t word_count, uint32_t hash_count)
{
    MU_ASSERT(bs);
    if (!words || word_count == 0u)
    {
        bs->words      = NULL;
        bs->word_count = 0;
        bs->mask       = 0;
        bs->hash_count = 0;
        bs->owned      = 0;
        return false;
    }

    /* word_count must be a power of two for the mask to wrap correctly. */
    if ((word_count & (word_count - 1u)) != 0u)
    {
        bs->words      = NULL;
        bs->word_count = 0;
        bs->owned      = 0;
        return false;
    }

    bs->words      = words;
    bs->word_count = word_count;
    bs->hash_count = hash_count;
    if (bs->hash_count < 1u)
        bs->hash_count = 1u;
    if (bs->hash_count > MU_BLOOM_MAX_HASHES)
        bs->hash_count = MU_BLOOM_MAX_HASHES;
    bs->mask  = word_count * 64u - 1u;
    bs->owned = 0;

    MU_MEMSET(bs->words, 0, (size_t)word_count * sizeof(uint64_t));
    return true;
}

MU_INLINE void mu_bloom_destroy(mu_bloom* bs)
{
    if (!bs)
        return;
    if (bs->owned)
        MU_FREE(bs->words);
    bs->words      = NULL;
    bs->word_count = 0;
    bs->mask       = 0;
    bs->owned      = 0;
}

MU_INLINE void mu_bloom_clear(mu_bloom* bs)
{
    if (!bs || !bs->words)
        return;
    MU_MEMSET(bs->words, 0, (size_t)bs->word_count * sizeof(uint64_t));
}

MU_INLINE void mu_bloom_add(mu_bloom* bs, uint64_t key)
{
    MU_ASSERT(bs);
    if (!bs->words)
        return;

    uint64_t h    = mu_bloom_hash(key);
    uint32_t i    = (uint32_t)h & bs->mask;
    uint32_t step = (uint32_t)(h >> 32) | 1u; /* odd => k distinct probes */

    for (uint32_t n = 0; n < bs->hash_count; ++n)
    {
        bs->words[i >> 6] |= (uint64_t)1 << (i & 63u);
        i = (i + step) & bs->mask;
    }
}

MU_INLINE bool mu_bloom_test(const mu_bloom* bs, uint64_t key)
{
    if (!bs || !bs->words)
        return false;

    uint64_t h    = mu_bloom_hash(key);
    uint32_t i    = (uint32_t)h & bs->mask;
    uint32_t step = (uint32_t)(h >> 32) | 1u;

    for (uint32_t n = 0; n < bs->hash_count; ++n)
    {
        if (((bs->words[i >> 6] >> (i & 63u)) & 1u) == 0u)
            return false;
        i = (i + step) & bs->mask;
    }
    return true;
}

MU_INLINE uint64_t mu_bloom_popcount(const mu_bloom* bs)
{
    if (!bs || !bs->words)
        return 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < bs->word_count; ++i)
        total += mu_popcount_u64(bs->words[i]);
    return total;
}

MU_INLINE uint32_t mu_bloom_bit_count(const mu_bloom* bs)
{
    return bs ? bs->word_count * 64u : 0u;
}

/* Fraction of bits set, 0.0 .. 1.0. Above ~0.5 the filter saturates and the
   false-positive rate climbs fast; above ~0.75 it is useless. */
MU_INLINE double mu_bloom_fill_ratio(const mu_bloom* bs)
{
    if (!bs || bs->word_count == 0u)
        return 0.0;
    return (double)mu_bloom_popcount(bs) / (double)((uint64_t)bs->word_count * 64ull);
}

/* -------------------------------------------------------------------------- *
 * Blocked Bloom filter: k probes inside ONE 64-bit cell per key.
 *   test() touches a single word (one cache line) instead of k scattered ones.
 * -------------------------------------------------------------------------- */

typedef struct mu_bloom_blocked
{
    uint64_t* words;      /* cell_count words, one cell each                     */
    uint32_t  cell_count; /* power-of-two                                        */
    uint32_t  hash_count; /* k, <= MU_BLOOM_MAX_HASHES                           */
    uint32_t  mask;       /* cell_count - 1                                      */
    uint32_t  owned;      /* 1 = heap buffer we may free                          */
} mu_bloom_blocked;

MU_INLINE bool mu_bloom_blocked_init(mu_bloom_blocked* bs, uint32_t cells, uint32_t hash_count)
{
    MU_ASSERT(bs);

    uint32_t c = mu_bloom__pow2_ceil(cells < 64u ? 64u : cells);

    bs->cell_count = c;
    bs->hash_count = hash_count;
    if (bs->hash_count < 1u)
        bs->hash_count = 1u;
    if (bs->hash_count > MU_BLOOM_MAX_HASHES)
        bs->hash_count = MU_BLOOM_MAX_HASHES;
    bs->mask  = c - 1u;
    bs->owned = 1;

    bs->words = (uint64_t*)MU_MALLOC((size_t)c * sizeof(uint64_t));
    if (!bs->words)
    {
        bs->cell_count = 0;
        bs->mask       = 0;
        bs->owned      = 0;
        return false;
    }

    MU_MEMSET(bs->words, 0, (size_t)c * sizeof(uint64_t));
    return true;
}

MU_INLINE bool mu_bloom_blocked_init_per_item(mu_bloom_blocked* bs, uint64_t items, uint32_t bits_per_item)
{
    if (bits_per_item == 0u)
        bits_per_item = 1u;

    uint64_t total = items * (uint64_t)bits_per_item;
    uint64_t cells = (total + 63ull) / 64ull;
    if (cells < 64ull)
        cells = 64ull;
    if (cells > 0x80000000ull)
        cells = 0x80000000ull;

    return mu_bloom_blocked_init(bs, (uint32_t)cells, mu_bloom_hash_count_for(bits_per_item));
}

MU_INLINE bool mu_bloom_blocked_init_static(mu_bloom_blocked* bs, uint64_t* cells, uint32_t cell_count, uint32_t hash_count)
{
    MU_ASSERT(bs);
    if (!cells || cell_count == 0u || (cell_count & (cell_count - 1u)) != 0u)
    {
        bs->words      = NULL;
        bs->cell_count = 0;
        bs->owned      = 0;
        return false;
    }

    bs->words      = cells;
    bs->cell_count = cell_count;
    bs->hash_count = hash_count;
    if (bs->hash_count < 1u)
        bs->hash_count = 1u;
    if (bs->hash_count > MU_BLOOM_MAX_HASHES)
        bs->hash_count = MU_BLOOM_MAX_HASHES;
    bs->mask  = cell_count - 1u;
    bs->owned = 0;

    MU_MEMSET(bs->words, 0, (size_t)cell_count * sizeof(uint64_t));
    return true;
}

MU_INLINE void mu_bloom_blocked_destroy(mu_bloom_blocked* bs)
{
    if (!bs)
        return;
    if (bs->owned)
        MU_FREE(bs->words);
    bs->words      = NULL;
    bs->cell_count = 0;
    bs->mask       = 0;
    bs->owned      = 0;
}

MU_INLINE void mu_bloom_blocked_clear(mu_bloom_blocked* bs)
{
    if (!bs || !bs->words)
        return;
    MU_MEMSET(bs->words, 0, (size_t)bs->cell_count * sizeof(uint64_t));
}

/* Internal: build the k-bit mask inside one cell.
   step is odd, so (offset + step*i) mod 64 visits 64 distinct positions before
   repeating: an arithmetic progression over the whole cell, never a repeat. */
MU_INLINE uint64_t mu_bloom_blocked__mask(uint32_t step, uint32_t offset, uint32_t hash_count)
{
    uint64_t bits = 0;
    for (uint32_t i = 0; i < hash_count; ++i)
        bits |= (uint64_t)1 << ((offset + step * i) & 63u);
    return bits;
}

MU_INLINE void mu_bloom_blocked_add(mu_bloom_blocked* bs, uint64_t key)
{
    MU_ASSERT(bs);
    if (!bs->words)
        return;

    uint64_t h1    = mu_bloom_hash(key);
    uint64_t h2    = mu_bloom_hash(h1); /* independent layout of positions */
    uint32_t cell  = (uint32_t)h1 & bs->mask;
    uint32_t step  = (uint32_t)(h2 >> 32) | 1u; /* odd  => full-cell coverage */
    uint32_t off   = (uint32_t)h2 & 63u;

    bs->words[cell] |= mu_bloom_blocked__mask(step, off, bs->hash_count);
}

MU_INLINE bool mu_bloom_blocked_test(const mu_bloom_blocked* bs, uint64_t key)
{
    if (!bs || !bs->words)
        return false;

    uint64_t h1    = mu_bloom_hash(key);
    uint64_t h2    = mu_bloom_hash(h1);
    uint32_t cell  = (uint32_t)h1 & bs->mask;
    uint32_t step  = (uint32_t)(h2 >> 32) | 1u;
    uint32_t off   = (uint32_t)h2 & 63u;
    uint64_t probe = mu_bloom_blocked__mask(step, off, bs->hash_count);
    return (bs->words[cell] & probe) == probe;
}

MU_INLINE uint64_t mu_bloom_blocked_popcount(const mu_bloom_blocked* bs)
{
    if (!bs || !bs->words)
        return 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < bs->cell_count; ++i)
        total += mu_popcount_u64(bs->words[i]);
    return total;
}

MU_INLINE uint32_t mu_bloom_blocked_bit_count(const mu_bloom_blocked* bs)
{
    return bs ? bs->cell_count * 64u : 0u;
}

MU_INLINE double mu_bloom_blocked_fill_ratio(const mu_bloom_blocked* bs)
{
    if (!bs || bs->cell_count == 0u)
        return 0.0;
    return (double)mu_bloom_blocked_popcount(bs) / (double)((uint64_t)bs->cell_count * 64ull);
}
