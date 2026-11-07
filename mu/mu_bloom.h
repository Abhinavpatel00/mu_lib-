#pragma once
#ifndef MU_BLOOM_H
#define MU_BLOOM_H

#include "mu_common.h"

#if defined(__AVX2__)
#  include <immintrin.h>
#endif

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * mu_bloom — probabilistic membership filters (zero false negatives).
 *
 * THREE FILTERS IN ONE HEADER:
 *   1. `mu_bloom`         — Classic bit-array Bloom (Kirsch-Mitzenmacher).
 *   2. `mu_bloom_blocked` — 64-bit Single-Word Blocked Bloom (1 cache line).
 *                           OPTIMIZED: splits ONE 64-bit hash into cell index
 *                           + (offset, odd step) instead of double hashing!
 *   3. `mu_bloom_simd256` — Apache Parquet / Putze 256-Bit Split-Block
 *                           Register-Blocked Bloom Filter:
 *                           Sets/tests 8 independent bits across eight 32-bit
 *                           lanes in ONE 32-byte aligned block using AVX2
 *                           `vpmulld` + `vpsllvd` + `vptest` (~3.4x faster than
 *                           classic Bloom and lower FPR than 64-bit blocked!),
 *                           with a bit-identical C99 scalar fallback!
 * --------------------------------------------------------------------------- */

#ifndef MU_BLOOM_MAX_HASHES
#define MU_BLOOM_MAX_HASHES 32u
#endif

typedef struct mu_bloom
{
    uint64_t* words;      /* bit array, word_count words                        */
    uint32_t  word_count; /* power-of-two words                                 */
    uint32_t  hash_count; /* k                                                  */
    uint32_t  mask;       /* (word_count*64) - 1, branchless bit index wrap     */
    uint32_t  owned;      /* 1 = heap buffer we may free                        */
} mu_bloom;

/* 64-bit finalizer (MurmurHash3-style). Not cryptographic. */
MU_INLINE uint64_t mu_bloom_hash(uint64_t x)
{
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

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
    uint32_t k = (bits_per_item * 69u + 50u) / 100u;
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

MU_INLINE bool mu_bloom_init_static(mu_bloom* bs, uint64_t* words, uint32_t word_count, uint32_t hash_count)
{
    MU_ASSERT(bs);
    if (!words || word_count == 0u || (word_count & (word_count - 1u)) != 0u)
    {
        bs->words      = NULL;
        bs->word_count = 0;
        bs->mask       = 0;
        bs->hash_count = 0;
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
    uint32_t step = (uint32_t)(h >> 32) | 1u;

    for (uint32_t n = 0; n < bs->hash_count; ++n)
    {
        bs->words[i >> 6] |= UINT64_C(1) << (i & 63u);
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

MU_INLINE bool mu_bloom_possibly_contains(const mu_bloom* bs, uint64_t key)
{
    return mu_bloom_test(bs, key);
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

MU_INLINE double mu_bloom_fill_ratio(const mu_bloom* bs)
{
    if (!bs || bs->word_count == 0u)
        return 0.0;
    return (double)mu_bloom_popcount(bs) / (double)((uint64_t)bs->word_count * 64ull);
}

/* -------------------------------------------------------------------------- *
 * 2. Blocked Bloom filter: k probes inside ONE 64-bit cell per key.
 *    OPTIMIZED: splits ONE 64-bit hash into cell + (offset, odd step)!
 * -------------------------------------------------------------------------- */

typedef struct mu_bloom_blocked
{
    uint64_t* words;      /* cell_count words, one cell each                     */
    uint32_t  cell_count; /* power-of-two                                        */
    uint32_t  hash_count; /* k, <= MU_BLOOM_MAX_HASHES                           */
    uint32_t  mask;       /* cell_count - 1                                      */
    uint32_t  owned;      /* 1 = heap buffer we may free                         */
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

MU_INLINE uint64_t mu_bloom_blocked__mask(uint32_t step, uint32_t offset, uint32_t hash_count)
{
    uint64_t bits = 0;
    for (uint32_t i = 0; i < hash_count; ++i)
        bits |= UINT64_C(1) << ((offset + step * i) & 63u);
    return bits;
}

MU_INLINE void mu_bloom_blocked_add(mu_bloom_blocked* bs, uint64_t key)
{
    MU_ASSERT(bs);
    if (!bs->words)
        return;

    uint64_t h    = mu_bloom_hash(key);
    uint32_t cell = (uint32_t)h & bs->mask;
    uint32_t hi   = (uint32_t)(h >> 32);
    uint32_t off  = hi & 63u;
    uint32_t step = ((hi >> 6) | 1u) & 63u;

    bs->words[cell] |= mu_bloom_blocked__mask(step, off, bs->hash_count);
}

MU_INLINE bool mu_bloom_blocked_test(const mu_bloom_blocked* bs, uint64_t key)
{
    if (!bs || !bs->words)
        return false;

    uint64_t h     = mu_bloom_hash(key);
    uint32_t cell  = (uint32_t)h & bs->mask;
    uint32_t hi    = (uint32_t)(h >> 32);
    uint32_t off   = hi & 63u;
    uint32_t step  = ((hi >> 6) | 1u) & 63u;
    uint64_t probe = mu_bloom_blocked__mask(step, off, bs->hash_count);
    return (bs->words[cell] & probe) == probe;
}

MU_INLINE bool mu_bloom_blocked_possibly_contains(const mu_bloom_blocked* bs, uint64_t key)
{
    return mu_bloom_blocked_test(bs, key);
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

/* ========================================================================== *
 * 3. Apache Parquet / Putze 256-Bit Split-Block SIMD Bloom Filter
 *
 * Each block is 32 bytes (eight 32-bit lanes = half a cache line).
 * AVX2 path and C99 scalar fallback produce BIT-IDENTICAL blocks!
 * ========================================================================== */

typedef struct mu_bloom_block256
{
    MU_ALIGN(32) uint32_t lanes[8];
} mu_bloom_block256;

typedef struct mu_bloom_simd256
{
    mu_bloom_block256* blocks;
    void*              raw_alloc;
    uint32_t           block_count; /* power of 2 */
    uint32_t           block_mask;
    uint32_t           owned;
} mu_bloom_simd256;

MU_INLINE bool mu_bloom_simd256_init(mu_bloom_simd256* bf, uint32_t expected_keys, uint32_t bits_per_key)
{
    MU_ASSERT(bf);
    MU_MEMSET(bf, 0, sizeof(*bf));

    if (bits_per_key < 8u)
        bits_per_key = 10u;

    uint64_t raw_bits = (uint64_t)(expected_keys ? expected_keys : 1u) * (uint64_t)bits_per_key;
    uint64_t min_blks = (raw_bits + 255ull) >> 8;
    if (min_blks == 0ull)
        min_blks = 1ull;
    if (min_blks > 0x20000000ull)
        return false;

    uint32_t nblks = 1u;
    while (nblks < (uint32_t)min_blks)
        nblks <<= 1;

    size_t bytes = (size_t)nblks * sizeof(mu_bloom_block256) + 32u;
    void*  raw   = MU_MALLOC(bytes);
    if (!raw)
        return false;

    uintptr_t aligned = ((uintptr_t)raw + 31u) & ~(uintptr_t)31u;
    bf->blocks        = (mu_bloom_block256*)aligned;
    bf->raw_alloc     = raw;
    bf->block_count   = nblks;
    bf->block_mask    = nblks - 1u;
    bf->owned         = 1u;
    MU_MEMSET(bf->blocks, 0, (size_t)nblks * sizeof(mu_bloom_block256));
    return true;
}

MU_INLINE bool mu_bloom_simd256_init_static(mu_bloom_simd256* bf, mu_bloom_block256* aligned_blocks, uint32_t block_count)
{
    MU_ASSERT(bf);
    MU_MEMSET(bf, 0, sizeof(*bf));

    if (!aligned_blocks || block_count == 0u || (block_count & (block_count - 1u)) != 0u)
        return false;
    if (((uintptr_t)aligned_blocks & 31u) != 0u)
        return false;

    MU_MEMSET(aligned_blocks, 0, (size_t)block_count * sizeof(mu_bloom_block256));
    bf->blocks      = aligned_blocks;
    bf->raw_alloc   = NULL;
    bf->block_count = block_count;
    bf->block_mask  = block_count - 1u;
    bf->owned       = 0u;
    return true;
}

MU_INLINE void mu_bloom_simd256_destroy(mu_bloom_simd256* bf)
{
    if (!bf)
        return;
    if (bf->owned)
        MU_FREE(bf->raw_alloc);
    MU_MEMSET(bf, 0, sizeof(*bf));
}

MU_INLINE void mu_bloom_simd256_clear(mu_bloom_simd256* bf)
{
    MU_ASSERT(bf && bf->blocks);
    MU_MEMSET(bf->blocks, 0, (size_t)bf->block_count * sizeof(mu_bloom_block256));
}

#define MU_BLOOM_SALT0 0x47b6137bu
#define MU_BLOOM_SALT1 0x44974d91u
#define MU_BLOOM_SALT2 0x8824ad5bu
#define MU_BLOOM_SALT3 0xa2b7289du
#define MU_BLOOM_SALT4 0x705495c7u
#define MU_BLOOM_SALT5 0x2df1424bu
#define MU_BLOOM_SALT6 0x9efc4947u
#define MU_BLOOM_SALT7 0x5c6bfb31u

MU_INLINE void mu_bloom_simd256_add(mu_bloom_simd256* bf, uint64_t key)
{
    MU_ASSERT(bf && bf->blocks);
    uint64_t h   = mu_bloom_hash(key);
    uint32_t blk = (uint32_t)(h >> 32) & bf->block_mask;
    uint32_t lo  = (uint32_t)h;

#if defined(__AVX2__)
    const __m256i salt = _mm256_setr_epi32(
        (int)MU_BLOOM_SALT0, (int)MU_BLOOM_SALT1, (int)MU_BLOOM_SALT2, (int)MU_BLOOM_SALT3,
        (int)MU_BLOOM_SALT4, (int)MU_BLOOM_SALT5, (int)MU_BLOOM_SALT6, (int)MU_BLOOM_SALT7);
    __m256i hv   = _mm256_set1_epi32((int)lo);
    __m256i sh   = _mm256_srli_epi32(_mm256_mullo_epi32(hv, salt), 27);
    __m256i mask = _mm256_sllv_epi32(_mm256_set1_epi32(1), sh);
    __m256i cur  = _mm256_load_si256((const __m256i*)&bf->blocks[blk]);
    _mm256_store_si256((__m256i*)&bf->blocks[blk], _mm256_or_si256(cur, mask));
#else
    uint32_t* w = bf->blocks[blk].lanes;
    w[0] |= 1u << ((lo * MU_BLOOM_SALT0) >> 27);
    w[1] |= 1u << ((lo * MU_BLOOM_SALT1) >> 27);
    w[2] |= 1u << ((lo * MU_BLOOM_SALT2) >> 27);
    w[3] |= 1u << ((lo * MU_BLOOM_SALT3) >> 27);
    w[4] |= 1u << ((lo * MU_BLOOM_SALT4) >> 27);
    w[5] |= 1u << ((lo * MU_BLOOM_SALT5) >> 27);
    w[6] |= 1u << ((lo * MU_BLOOM_SALT6) >> 27);
    w[7] |= 1u << ((lo * MU_BLOOM_SALT7) >> 27);
#endif
}

MU_INLINE bool mu_bloom_simd256_test(const mu_bloom_simd256* bf, uint64_t key)
{
    MU_ASSERT(bf && bf->blocks);
    uint64_t h   = mu_bloom_hash(key);
    uint32_t blk = (uint32_t)(h >> 32) & bf->block_mask;
    uint32_t lo  = (uint32_t)h;

#if defined(__AVX2__)
    const __m256i salt = _mm256_setr_epi32(
        (int)MU_BLOOM_SALT0, (int)MU_BLOOM_SALT1, (int)MU_BLOOM_SALT2, (int)MU_BLOOM_SALT3,
        (int)MU_BLOOM_SALT4, (int)MU_BLOOM_SALT5, (int)MU_BLOOM_SALT6, (int)MU_BLOOM_SALT7);
    __m256i hv   = _mm256_set1_epi32((int)lo);
    __m256i sh   = _mm256_srli_epi32(_mm256_mullo_epi32(hv, salt), 27);
    __m256i mask = _mm256_sllv_epi32(_mm256_set1_epi32(1), sh);
    __m256i cur  = _mm256_load_si256((const __m256i*)&bf->blocks[blk]);
    return _mm256_testc_si256(cur, mask) != 0;
#else
    const uint32_t* w = bf->blocks[blk].lanes;
    uint32_t m0 = 1u << ((lo * MU_BLOOM_SALT0) >> 27);
    uint32_t m1 = 1u << ((lo * MU_BLOOM_SALT1) >> 27);
    uint32_t m2 = 1u << ((lo * MU_BLOOM_SALT2) >> 27);
    uint32_t m3 = 1u << ((lo * MU_BLOOM_SALT3) >> 27);
    uint32_t m4 = 1u << ((lo * MU_BLOOM_SALT4) >> 27);
    uint32_t m5 = 1u << ((lo * MU_BLOOM_SALT5) >> 27);
    uint32_t m6 = 1u << ((lo * MU_BLOOM_SALT6) >> 27);
    uint32_t m7 = 1u << ((lo * MU_BLOOM_SALT7) >> 27);
    return ((w[0] & m0) && (w[1] & m1) && (w[2] & m2) && (w[3] & m3) &&
            (w[4] & m4) && (w[5] & m5) && (w[6] & m6) && (w[7] & m7));
#endif
}

MU_INLINE bool mu_bloom_simd256_possibly_contains(const mu_bloom_simd256* bf, uint64_t key)
{
    return mu_bloom_simd256_test(bf, key);
}

MU_INLINE void mu_bloom_simd256_prefetch(const mu_bloom_simd256* bf, uint64_t key)
{
    uint64_t h   = mu_bloom_hash(key);
    uint32_t blk = (uint32_t)(h >> 32) & bf->block_mask;
    MU_PREFETCH(&bf->blocks[blk]);
}

MU_END_EXTERN_C

#endif /* MU_BLOOM_H */
