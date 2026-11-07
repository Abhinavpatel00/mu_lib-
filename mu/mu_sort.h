#pragma once
#ifndef MU_SORT_H
#define MU_SORT_H

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * mu_sort — high-throughput key-indexed sorts for uint32 / uint64 / float keys.
 *
 * OPTIMIZATIONS IMPLEMENTED:
 *   1. Single-Scan Multi-Histogram (Pierre Terdiman 'Radix Sort Revisited'):
 *      Computes all 4 (u32) or 8 (u64) byte histograms in ONE sequential pass
 *      over `keys[]` instead of re-reading `keys[]` once per byte pass.
 *   2. Uniform-Byte Zero-Pass Skip:
 *      If all n keys share the exact same byte in pass `p` (`hist[p][b0] == n`),
 *      that entire scatter pass is skipped in O(1) — e.g., sorting 100K entity
 *      IDs < 65,536 runs ONLY 2 passes instead of 4 (or 8 for u64)!
 *   3. Zero-Malloc Scratch Variants (`_scratch`):
 *      Caller / frame-arena supplies `tmp_keys` (+ `tmp_payload`), eliminating
 *      all per-frame heap allocations (`AGENTS.md §9.1`).
 *   4. IEEE-754 Float Radix Sort (`mu_radix_sort_f32`, Michael Herf FloatFlip):
 *      Bijectively maps float bits (including negatives & -0.0f) onto monotonic
 *      uint32 order, sorts in O(n) via LSD radix, and unflips in place.
 *
 * CONTRACT:
 *   - Stable: equal keys preserve original relative input order.
 *   - Pass `payload == NULL` to sort keys alone.
 * --------------------------------------------------------------------------- */

#define MU_SORT_SMALL 32u

/* -------------------------------------------------------------------------- *
 * Insertion sort — optimal for tiny n <= 32 (zero branches on nearly-sorted).
 * -------------------------------------------------------------------------- */

MU_INLINE void mu_insertion_sort_u32(uint32_t* MU_RESTRICT keys, uint32_t* MU_RESTRICT payload, uint32_t n)
{
    if (payload)
    {
        for (uint32_t i = 1; i < n; ++i)
        {
            uint32_t key = keys[i];
            uint32_t pay = payload[i];
            uint32_t j   = i;
            while (j > 0u && keys[j - 1u] > key)
            {
                keys[j]    = keys[j - 1u];
                payload[j] = payload[j - 1u];
                --j;
            }
            keys[j]    = key;
            payload[j] = pay;
        }
    }
    else
    {
        for (uint32_t i = 1; i < n; ++i)
        {
            uint32_t key = keys[i];
            uint32_t j   = i;
            while (j > 0u && keys[j - 1u] > key)
            {
                keys[j] = keys[j - 1u];
                --j;
            }
            keys[j] = key;
        }
    }
}

MU_INLINE void mu_insertion_sort_u64(uint64_t* MU_RESTRICT keys, uint32_t* MU_RESTRICT payload, uint32_t n)
{
    if (payload)
    {
        for (uint32_t i = 1; i < n; ++i)
        {
            uint64_t key = keys[i];
            uint32_t pay = payload[i];
            uint32_t j   = i;
            while (j > 0u && keys[j - 1u] > key)
            {
                keys[j]    = keys[j - 1u];
                payload[j] = payload[j - 1u];
                --j;
            }
            keys[j]    = key;
            payload[j] = pay;
        }
    }
    else
    {
        for (uint32_t i = 1; i < n; ++i)
        {
            uint64_t key = keys[i];
            uint32_t j   = i;
            while (j > 0u && keys[j - 1u] > key)
            {
                keys[j] = keys[j - 1u];
                --j;
            }
            keys[j] = key;
        }
    }
}

/* -------------------------------------------------------------------------- *
 * Single-Histogram + Pass-Skipping LSD Radix Sort (Zero-Alloc Scratch Core)
 * -------------------------------------------------------------------------- */

MU_INLINE void mu_radix_sort_u32_scratch(
    uint32_t* MU_RESTRICT keys,
    uint32_t* MU_RESTRICT payload,
    uint32_t* MU_RESTRICT tmp_keys,
    uint32_t* MU_RESTRICT tmp_payload,
    uint32_t              n)
{
    if (n < 2u)
        return;
    if (n <= MU_SORT_SMALL)
    {
        mu_insertion_sort_u32(keys, payload, n);
        return;
    }

    MU_ASSERT(tmp_keys != NULL);
    MU_ASSERT(payload == NULL || tmp_payload != NULL);

    /* 1. Single sequential pass builds all 4 byte histograms (4 KB in L1d). */
    uint32_t hist[4][256];
    MU_MEMSET(hist, 0, sizeof(hist));

    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t k = keys[i];
        ++hist[0][(k      ) & 0xffu];
        ++hist[1][(k >>  8) & 0xffu];
        ++hist[2][(k >> 16) & 0xffu];
        ++hist[3][(k >> 24) & 0xffu];
    }

    uint32_t* src_k = keys;
    uint32_t* dst_k = tmp_keys;
    uint32_t* src_p = payload;
    uint32_t* dst_p = tmp_payload;
    uint32_t  k0    = keys[0];

    for (uint32_t pass = 0; pass < 4u; ++pass)
    {
        uint32_t shift = pass * 8u;
        uint32_t b0    = (k0 >> shift) & 0xffu;

        /* Skip uniform byte pass in O(1)! */
        if (hist[pass][b0] == n)
            continue;

        uint32_t* count = hist[pass];
        uint32_t  sum   = 0;
        for (uint32_t d = 0; d < 256u; ++d)
        {
            uint32_t c = count[d];
            count[d]   = sum;
            sum += c;
        }

        if (src_p)
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t k  = src_k[i];
                uint32_t at = count[(k >> shift) & 0xffu]++;
                dst_k[at]   = k;
                dst_p[at]   = src_p[i];
            }
            MU_SWAP(uint32_t*, src_p, dst_p);
        }
        else
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t k  = src_k[i];
                uint32_t at = count[(k >> shift) & 0xffu]++;
                dst_k[at]   = k;
            }
        }
        MU_SWAP(uint32_t*, src_k, dst_k);
    }

    /* If an odd number of passes executed, copy result back to caller buffer. */
    if (src_k != keys)
    {
        MU_MEMCPY(keys, src_k, (size_t)n * sizeof(uint32_t));
        if (payload)
            MU_MEMCPY(payload, src_p, (size_t)n * sizeof(uint32_t));
    }
}

MU_INLINE void mu_radix_sort_u64_scratch(
    uint64_t* MU_RESTRICT keys,
    uint32_t* MU_RESTRICT payload,
    uint64_t* MU_RESTRICT tmp_keys,
    uint32_t* MU_RESTRICT tmp_payload,
    uint32_t              n)
{
    if (n < 2u)
        return;
    if (n <= MU_SORT_SMALL)
    {
        mu_insertion_sort_u64(keys, payload, n);
        return;
    }

    MU_ASSERT(tmp_keys != NULL);
    MU_ASSERT(payload == NULL || tmp_payload != NULL);

    /* 1. Single sequential pass builds all 8 byte histograms (8 KB in L1d). */
    uint32_t hist[8][256];
    MU_MEMSET(hist, 0, sizeof(hist));

    for (uint32_t i = 0; i < n; ++i)
    {
        uint64_t k = keys[i];
        ++hist[0][(uint32_t)( k        & 0xffull)];
        ++hist[1][(uint32_t)((k >>  8) & 0xffull)];
        ++hist[2][(uint32_t)((k >> 16) & 0xffull)];
        ++hist[3][(uint32_t)((k >> 24) & 0xffull)];
        ++hist[4][(uint32_t)((k >> 32) & 0xffull)];
        ++hist[5][(uint32_t)((k >> 40) & 0xffull)];
        ++hist[6][(uint32_t)((k >> 48) & 0xffull)];
        ++hist[7][(uint32_t)((k >> 56) & 0xffull)];
    }

    uint64_t* src_k = keys;
    uint64_t* dst_k = tmp_keys;
    uint32_t* src_p = payload;
    uint32_t* dst_p = tmp_payload;
    uint64_t  k0    = keys[0];

    for (uint32_t pass = 0; pass < 8u; ++pass)
    {
        uint32_t shift = pass * 8u;
        uint32_t b0    = (uint32_t)((k0 >> shift) & 0xffull);

        if (hist[pass][b0] == n)
            continue;

        uint32_t* count = hist[pass];
        uint32_t  sum   = 0;
        for (uint32_t d = 0; d < 256u; ++d)
        {
            uint32_t c = count[d];
            count[d]   = sum;
            sum += c;
        }

        if (src_p)
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                uint64_t k  = src_k[i];
                uint32_t at = count[(uint32_t)((k >> shift) & 0xffull)]++;
                dst_k[at]   = k;
                dst_p[at]   = src_p[i];
            }
            MU_SWAP(uint32_t*, src_p, dst_p);
        }
        else
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                uint64_t k  = src_k[i];
                uint32_t at = count[(uint32_t)((k >> shift) & 0xffull)]++;
                dst_k[at]   = k;
            }
        }
        MU_SWAP(uint64_t*, src_k, dst_k);
    }

    if (src_k != keys)
    {
        MU_MEMCPY(keys, src_k, (size_t)n * sizeof(uint64_t));
        if (payload)
            MU_MEMCPY(payload, src_p, (size_t)n * sizeof(uint32_t));
    }
}

MU_INLINE bool mu_radix_sort_u32(uint32_t* keys, uint32_t* payload, uint32_t n)
{
    if (n < 2u)
        return true;
    if (n <= MU_SORT_SMALL)
    {
        mu_insertion_sort_u32(keys, payload, n);
        return true;
    }

    uint32_t* tmp_k = (uint32_t*)MU_MALLOC((size_t)n * sizeof(uint32_t));
    uint32_t* tmp_p = NULL;
    if (payload)
    {
        tmp_p = (uint32_t*)MU_MALLOC((size_t)n * sizeof(uint32_t));
        if (!tmp_p)
        {
            MU_FREE(tmp_k);
            return false;
        }
    }
    if (!tmp_k)
    {
        MU_FREE(tmp_p);
        return false;
    }

    mu_radix_sort_u32_scratch(keys, payload, tmp_k, tmp_p, n);

    MU_FREE(tmp_k);
    MU_FREE(tmp_p);
    return true;
}

MU_INLINE bool mu_radix_sort_u64(uint64_t* keys, uint32_t* payload, uint32_t n)
{
    if (n < 2u)
        return true;
    if (n <= MU_SORT_SMALL)
    {
        mu_insertion_sort_u64(keys, payload, n);
        return true;
    }

    uint64_t* tmp_k = (uint64_t*)MU_MALLOC((size_t)n * sizeof(uint64_t));
    uint32_t* tmp_p = NULL;
    if (payload)
    {
        tmp_p = (uint32_t*)MU_MALLOC((size_t)n * sizeof(uint32_t));
        if (!tmp_p)
        {
            MU_FREE(tmp_k);
            return false;
        }
    }
    if (!tmp_k)
    {
        MU_FREE(tmp_p);
        return false;
    }

    mu_radix_sort_u64_scratch(keys, payload, tmp_k, tmp_p, n);

    MU_FREE(tmp_k);
    MU_FREE(tmp_p);
    return true;
}

/* -------------------------------------------------------------------------- *
 * IEEE-754 Float Radix Sort (Michael Herf FloatFlip bijection)
 * -------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_sort_float_to_radix_key(float f)
{
    uint32_t u;
    MU_MEMCPY(&u, &f, sizeof(u));
    uint32_t mask = (uint32_t)(-(int32_t)(u >> 31)) | 0x80000000u;
    return u ^ mask;
}

MU_INLINE float mu_sort_radix_key_to_float(uint32_t k)
{
    uint32_t mask = ((k >> 31) - 1u) | 0x80000000u;
    uint32_t u    = k ^ mask;
    float    f;
    MU_MEMCPY(&f, &u, sizeof(f));
    return f;
}

MU_INLINE void mu_radix_sort_f32_scratch(
    float*    MU_RESTRICT keys,
    uint32_t* MU_RESTRICT payload,
    uint32_t* MU_RESTRICT tmp_keys,
    uint32_t* MU_RESTRICT tmp_payload,
    uint32_t              n)
{
    if (n < 2u)
        return;

    uint32_t* ukeys = (uint32_t*)keys;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t u;
        MU_MEMCPY(&u, &keys[i], sizeof(u));
        uint32_t mask = (uint32_t)(-(int32_t)(u >> 31)) | 0x80000000u;
        ukeys[i]      = u ^ mask;
    }

    mu_radix_sort_u32_scratch(ukeys, payload, tmp_keys, tmp_payload, n);

    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t k    = ukeys[i];
        uint32_t mask = ((k >> 31) - 1u) | 0x80000000u;
        uint32_t u    = k ^ mask;
        MU_MEMCPY(&keys[i], &u, sizeof(u));
    }
}

MU_INLINE bool mu_radix_sort_f32(float* keys, uint32_t* payload, uint32_t n)
{
    if (n < 2u)
        return true;

    uint32_t* tmp_k = (uint32_t*)MU_MALLOC((size_t)n * sizeof(uint32_t));
    uint32_t* tmp_p = payload ? (uint32_t*)MU_MALLOC((size_t)n * sizeof(uint32_t)) : NULL;
    if (!tmp_k || (payload && !tmp_p))
    {
        MU_FREE(tmp_k);
        MU_FREE(tmp_p);
        return false;
    }

    mu_radix_sort_f32_scratch(keys, payload, tmp_k, tmp_p, n);

    MU_FREE(tmp_k);
    MU_FREE(tmp_p);
    return true;
}

/* -------------------------------------------------------------------------- *
 * Index sort — order without moving the data (Heapsort, O(1) extra space).
 * -------------------------------------------------------------------------- */

MU_INLINE void mu_sort_indices_u32(const uint32_t* keys, uint32_t* idx, uint32_t n)
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

MU_INLINE void mu_sort_indices_u64(const uint64_t* keys, uint32_t* idx, uint32_t n)
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

MU_END_EXTERN_C

#endif /* MU_SORT_H */
