#pragma once
#ifndef MU_XOR_FILTER_H
#define MU_XOR_FILTER_H

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * mu_xor8_filter — Graf & Lemire Static Xor8 Filter (2020).
 *
 * WHY:
 *   When the key set is immutable after load/cook time (asset GUIDs, shader
 *   permutation cache keys, LSM/SSTable blocks, dictionary words):
 *     - Space: ONLY `floor(1.23 * n) + 32` bytes (~9.84 bits/key, 38% smaller
 *       than a 16-bit/key Bloom filter for the same ~0.39% FPR!)
 *     - Query: EXACTLY 3 independent byte loads (`fp[h0] ^ fp[h1] ^ fp[h2]`),
 *       fully overlapped by hardware Memory-Level Parallelism (MLP) with ZERO
 *       data-dependent branches!
 *     - Zero false negatives; ~1/256 (~0.3906%) false positive rate.
 *
 * CONTRACT:
 *   Input `keys[0..n)` passed to `mu_xor8_build` MUST be duplicate-free
 *   (use `mu_radix_sort_u64` + `unique` first if unsorted/duplicated).
 * --------------------------------------------------------------------------- */

typedef struct mu_xor8_filter
{
    uint8_t* fingerprints;
    uint64_t seed;
    uint32_t block_length; /* total array size = 3 * block_length */
    uint32_t owned;
} mu_xor8_filter;

MU_INLINE uint64_t mu_xor8__murmur64(uint64_t h)
{
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    return h;
}

MU_INLINE uint64_t mu_xor8__mix(uint64_t key, uint64_t seed)
{
    return mu_xor8__murmur64(key + seed);
}

MU_INLINE uint64_t mu_xor8__rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

MU_INLINE uint32_t mu_xor8__reduce(uint32_t hash, uint32_t n)
{
    return (uint32_t)(((uint64_t)hash * (uint64_t)n) >> 32);
}

MU_INLINE uint8_t mu_xor8__fingerprint(uint64_t hash)
{
    return (uint8_t)(hash ^ (hash >> 32));
}

MU_INLINE size_t mu_xor8_byte_size(const mu_xor8_filter* f)
{
    return f ? (size_t)f->block_length * 3u : 0u;
}

/* Zero-copy view over prebaked / memory-mapped fingerprint table (`3 * block_length` bytes). */
MU_INLINE bool mu_xor8_init_view(
    mu_xor8_filter* f,
    const uint8_t*  fingerprints,
    uint32_t        block_length,
    uint64_t        seed)
{
    MU_ASSERT(f);
    MU_MEMSET(f, 0, sizeof(*f));
    if (!fingerprints || block_length == 0u)
        return false;

    f->fingerprints = (uint8_t*)(uintptr_t)fingerprints;
    f->seed         = seed;
    f->block_length = block_length;
    f->owned        = 0u;
    return true;
}

MU_INLINE void mu_xor8_destroy(mu_xor8_filter* f)
{
    if (!f)
        return;
    if (f->owned)
        MU_FREE(f->fingerprints);
    MU_MEMSET(f, 0, sizeof(*f));
}

/* Linear O(n) 3-partition hypergraph peeling builder (Graf & Lemire Alg. 3). */
MU_INLINE bool mu_xor8_build(mu_xor8_filter* f, const uint64_t* keys, uint32_t n)
{
    MU_ASSERT(f);
    MU_MEMSET(f, 0, sizeof(*f));
    if (n == 0u)
        return true;
    MU_ASSERT(keys != NULL);

    uint64_t cap64 = (32ull + (123ull * (uint64_t)n) / 100ull);
    uint32_t blen  = (uint32_t)(cap64 / 3ull);
    if (blen == 0u)
        blen = 1u;
    uint32_t array_len = blen * 3u;

    uint8_t*  fp       = (uint8_t*)MU_CALLOC(array_len, sizeof(uint8_t));
    uint64_t* t2hash   = (uint64_t*)MU_MALLOC((size_t)array_len * sizeof(uint64_t));
    uint32_t* t2count  = (uint32_t*)MU_MALLOC((size_t)array_len * sizeof(uint32_t));
    uint32_t* alone    = (uint32_t*)MU_MALLOC((size_t)array_len * sizeof(uint32_t));
    uint64_t* rev_h    = (uint64_t*)MU_MALLOC((size_t)n * sizeof(uint64_t));
    uint8_t*  rev_part = (uint8_t*)MU_MALLOC((size_t)n * sizeof(uint8_t));

    if (!fp || !t2hash || !t2count || !alone || !rev_h || !rev_part)
    {
        MU_FREE(fp);
        MU_FREE(t2hash);
        MU_FREE(t2count);
        MU_FREE(alone);
        MU_FREE(rev_h);
        MU_FREE(rev_part);
        return false;
    }

    uint64_t rng  = UINT64_C(0x726b2b9d438b9d4d);
    bool     done = false;

    for (uint32_t attempt = 0; attempt < 64u; ++attempt)
    {
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        uint64_t seed = rng * UINT64_C(0x2545F4914F6CDD1D);

        MU_MEMSET(t2hash, 0, (size_t)array_len * sizeof(uint64_t));
        MU_MEMSET(t2count, 0, (size_t)array_len * sizeof(uint32_t));

        for (uint32_t i = 0; i < n; ++i)
        {
            uint64_t h  = mu_xor8__mix(keys[i], seed);
            uint32_t h0 = mu_xor8__reduce((uint32_t)h, blen);
            uint32_t h1 = mu_xor8__reduce((uint32_t)mu_xor8__rotl64(h, 21), blen) + blen;
            uint32_t h2 = mu_xor8__reduce((uint32_t)mu_xor8__rotl64(h, 42), blen) + 2u * blen;

            t2hash[h0] ^= h; ++t2count[h0];
            t2hash[h1] ^= h; ++t2count[h1];
            t2hash[h2] ^= h; ++t2count[h2];
        }

        uint32_t qsize = 0u;
        for (uint32_t i = 0; i < array_len; ++i)
        {
            if (t2count[i] == 1u)
                alone[qsize++] = i;
        }

        uint32_t stack_sz = 0u;
        while (qsize > 0u)
        {
            uint32_t idx = alone[--qsize];
            if (t2count[idx] != 1u)
                continue;

            uint64_t h           = t2hash[idx];
            uint8_t  found_part  = (idx < blen) ? 0u : ((idx < 2u * blen) ? 1u : 2u);
            rev_h[stack_sz]      = h;
            rev_part[stack_sz++] = found_part;

            uint32_t h0 = mu_xor8__reduce((uint32_t)h, blen);
            uint32_t h1 = mu_xor8__reduce((uint32_t)mu_xor8__rotl64(h, 21), blen) + blen;
            uint32_t h2 = mu_xor8__reduce((uint32_t)mu_xor8__rotl64(h, 42), blen) + 2u * blen;

            t2hash[h0] ^= h; if (--t2count[h0] == 1u) alone[qsize++] = h0;
            t2hash[h1] ^= h; if (--t2count[h1] == 1u) alone[qsize++] = h1;
            t2hash[h2] ^= h; if (--t2count[h2] == 1u) alone[qsize++] = h2;
        }

        if (stack_sz == n)
        {
            MU_MEMSET(fp, 0, array_len);
            while (stack_sz > 0u)
            {
                --stack_sz;
                uint64_t h    = rev_h[stack_sz];
                uint8_t  part = rev_part[stack_sz];
                uint32_t h0   = mu_xor8__reduce((uint32_t)h, blen);
                uint32_t h1   = mu_xor8__reduce((uint32_t)mu_xor8__rotl64(h, 21), blen) + blen;
                uint32_t h2   = mu_xor8__reduce((uint32_t)mu_xor8__rotl64(h, 42), blen) + 2u * blen;
                uint8_t  val  = mu_xor8__fingerprint(h) ^ fp[h0] ^ fp[h1] ^ fp[h2];
                uint32_t dst  = (part == 0u) ? h0 : ((part == 1u) ? h1 : h2);
                fp[dst]       = val;
            }
            f->fingerprints = fp;
            f->seed         = seed;
            f->block_length = blen;
            f->owned        = 1u;
            done            = true;
            break;
        }
    }

    MU_FREE(t2hash);
    MU_FREE(t2count);
    MU_FREE(alone);
    MU_FREE(rev_h);
    MU_FREE(rev_part);
    if (!done)
        MU_FREE(fp);
    return done;
}

/* 3 independent byte loads + branchless XOR check! */
MU_INLINE bool mu_xor8_contains(const mu_xor8_filter* f, uint64_t key)
{
    if (!f || f->block_length == 0u)
        return false;

    uint32_t       blen = f->block_length;
    const uint8_t* fp   = f->fingerprints;
    uint64_t       h    = mu_xor8__mix(key, f->seed);
    uint32_t       h0   = mu_xor8__reduce((uint32_t)h, blen);
    uint32_t       h1   = mu_xor8__reduce((uint32_t)mu_xor8__rotl64(h, 21), blen) + blen;
    uint32_t       h2   = mu_xor8__reduce((uint32_t)mu_xor8__rotl64(h, 42), blen) + 2u * blen;
    uint8_t        want = mu_xor8__fingerprint(h);
    return want == (uint8_t)(fp[h0] ^ fp[h1] ^ fp[h2]);
}

MU_END_EXTERN_C

#endif /* MU_XOR_FILTER_H */
