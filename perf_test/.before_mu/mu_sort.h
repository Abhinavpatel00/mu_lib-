#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_sort — key-indexed sorts for uint32/uint64 keys.
 *
 * WHY: the sorting chapter of the performance skill. Comparison sorts emit
 * unpredictable branches and indirect calls; radix sort does fixed work per
 * byte of key with a contiguous scatter, and beats comparisons as soon as n is
 * more than a few hundred or the keys are wide.
 *
 * Every function sorts KEYS TOGETHER WITH A uint32 PAYLOAD (an id, an index,
 * a row reference), because a sort in a data-oriented pipeline is almost
 * never over keys alone — you sort in order to permute the thing they name.
 *
 * LAYOUT: LSD radix, 8-bit digits, two buffers ping-ponged. The result is
 * STABLE: equal keys keep their input order (needed for deterministic
 * rebucketing / paint order / scheduling ties).
 *
 *   mu_radix_sort_u32   4 passes   (the common case: entity ids, packed keys)
 *   mu_radix_sort_u64   8 passes   (hashes, pointers-as-ids, timestamps)
 *   mu_insertion_sort_* 1 pass     (n <= 32: fewer moves than any other sort)
 *   mu_sort_indices_*   no permute (builds an ascending index array instead)
 *
 * CONTRACT: pass payload == NULL to sort keys alone. Returns false only on
 * allocation failure (the sort itself never fails otherwise).
 * --------------------------------------------------------------------------- */

#define MU_SORT_SMALL 32u

/* -------------------------------------------------------------------------- *
 * Insertion sort — best for tiny n (the skill's n < 32 rule).
 * -------------------------------------------------------------------------- */

MU_INLINE void mu_insertion_sort_u32(uint32_t* keys, uint32_t* payload, uint32_t n)
{
    for (uint32_t i = 1; i < n; ++i)
    {
        uint32_t key = keys[i];
        uint32_t j   = i;
        while (j > 0 && keys[j - 1u] > key)
        {
            keys[j] = keys[j - 1u];
            if (payload)
                payload[j] = payload[j - 1u];
            --j;
        }
        keys[j] = key;
    }
}

MU_INLINE void mu_insertion_sort_u64(uint64_t* keys, uint32_t* payload, uint32_t n)
{
    for (uint32_t i = 1; i < n; ++i)
    {
        uint64_t key = keys[i];
        uint32_t j   = i;
        while (j > 0 && keys[j - 1u] > key)
        {
            keys[j] = keys[j - 1u];
            if (payload)
                payload[j] = payload[j - 1u];
            --j;
        }
        keys[j] = key;
    }
}

/* -------------------------------------------------------------------------- *
 * LSD radix sort, 8-bit digits.
 *
 * Counting pass (histogram + prefix) then scatter, per byte. The scatter reads
 * `src` sequentially and writes `dst` sequentially, so both streams are
 * prefetch-friendly — no jumping by key value the way quicksort does.
 * -------------------------------------------------------------------------- */

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

    uint32_t* src_k = keys;
    uint32_t* dst_k = tmp_k;
    uint32_t* src_p = payload;
    uint32_t* dst_p = tmp_p;

    for (uint32_t shift = 0; shift < 32u; shift += 8u)
    {
        uint32_t count[256];
        MU_MEMSET(count, 0, sizeof(count));

        for (uint32_t i = 0; i < n; ++i)
            ++count[(src_k[i] >> shift) & 0xffu];

        uint32_t sum = 0;
        for (uint32_t d = 0; d < 256u; ++d)
        {
            uint32_t c = count[d];
            count[d]   = sum;
            sum += c;
        }

        for (uint32_t i = 0; i < n; ++i)
        {
            uint32_t d    = (src_k[i] >> shift) & 0xffu;
            uint32_t at   = count[d]++;
            dst_k[at]     = src_k[i];
            if (payload)
                dst_p[at] = src_p[i];
        }

        {   /* ping-pong: the freshly filled buffer becomes the source */
            uint32_t* tk = src_k;
            src_k        = dst_k;
            dst_k        = tk;
            if (payload)
            {
                uint32_t* tp = src_p;
                src_p        = dst_p;
                dst_p        = tp;
            }
        }
    }

    if (src_k != keys)
        MU_MEMCPY(keys, src_k, (size_t)n * sizeof(uint32_t));
    if (payload && src_p != payload)
        MU_MEMCPY(payload, src_p, (size_t)n * sizeof(uint32_t));

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

    uint64_t* src_k = keys;
    uint64_t* dst_k = tmp_k;
    uint32_t* src_p = payload;
    uint32_t* dst_p = tmp_p;

    for (uint32_t shift = 0; shift < 64u; shift += 8u)
    {
        uint32_t count[256];
        MU_MEMSET(count, 0, sizeof(count));

        for (uint32_t i = 0; i < n; ++i)
            ++count[(uint32_t)((src_k[i] >> shift) & 0xffull)];

        uint32_t sum = 0;
        for (uint32_t d = 0; d < 256u; ++d)
        {
            uint32_t c = count[d];
            count[d]   = sum;
            sum += c;
        }

        for (uint32_t i = 0; i < n; ++i)
        {
            uint32_t d = (uint32_t)((src_k[i] >> shift) & 0xffull);
            uint32_t at = count[d]++;
            dst_k[at]   = src_k[i];
            if (payload)
                dst_p[at] = src_p[i];
        }

        {
            uint64_t* tk = src_k;
            src_k        = dst_k;
            dst_k        = tk;
            if (payload)
            {
                uint32_t* tp = src_p;
                src_p        = dst_p;
                dst_p        = tp;
            }
        }
    }

    if (src_k != keys)
        MU_MEMCPY(keys, src_k, (size_t)n * sizeof(uint64_t));
    if (payload && src_p != payload)
        MU_MEMCPY(payload, src_p, (size_t)n * sizeof(uint32_t));

    MU_FREE(tmp_k);
    MU_FREE(tmp_p);
    return true;
}

/* -------------------------------------------------------------------------- *
 * Index sort — order without moving the data.
 *
 * Use when the keys belong to a big SoA you do not want to permute (you only
 * want a traversal order). Heapsort under the hood: iterative, so the stack
 * cost is flat regardless of input shape, and O(n log n) on the worst input.
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

/* Fill idx with 0..n-1 then sort it ascending by keys[]. */
MU_INLINE void mu_sort_indices_u32_build(const uint32_t* keys, uint32_t* idx, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        idx[i] = i;
    mu_sort_indices_u32(keys, idx, n);
}

MU_INLINE void mu_sort_indices_u64_build(const uint64_t* keys, uint32_t* idx, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        idx[i] = i;
    mu_sort_indices_u64(keys, idx, n);
}
