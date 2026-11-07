#pragma once
#ifndef MU_PERFECT_HASH_H
#define MU_PERFECT_HASH_H

#include "mu_common.h"

MU_BEGIN_EXTERN_C

typedef struct mu_phf
{
    uint64_t* keys;
    uint64_t* values;
    uint32_t* displacement;
    uint32_t n;
    uint32_t bucket_count;
    uint64_t seed;
} mu_phf;

typedef struct mu_phf__bucket
{
    uint32_t count;
    uint32_t index;
} mu_phf__bucket;

MU_INLINE uint64_t mu_phf__mix(uint64_t x)
{
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

MU_INLINE uint32_t mu_phf__h1(uint64_t key, uint64_t seed, uint32_t bucket_count)
{
    return (uint32_t)(mu_phf__mix(key ^ seed) % bucket_count);
}

MU_INLINE uint32_t mu_phf__h2(uint64_t key, uint64_t seed, uint32_t d, uint32_t n)
{
    uint64_t x = key ^ (seed + UINT64_C(0x9e3779b97f4a7c15));
    x ^= (uint64_t)d * UINT64_C(0xbf58476d1ce4e5b9);
    return (uint32_t)(mu_phf__mix(x) % n);
}

MU_INLINE int mu_phf__bucket_cmp(const void* a, const void* b)
{
    const mu_phf__bucket* x = (const mu_phf__bucket*)a;
    const mu_phf__bucket* y = (const mu_phf__bucket*)b;

    if (x->count != y->count)
        return x->count > y->count ? -1 : 1;
    if (x->index != y->index)
        return x->index < y->index ? -1 : 1;
    return 0;
}

MU_INLINE int mu_phf__u64_cmp(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

MU_INLINE void mu_phf_init(mu_phf* phf)
{
    if (!phf)
        return;
    phf->keys = NULL;
    phf->values = NULL;
    phf->displacement = NULL;
    phf->n = 0;
    phf->bucket_count = 0;
    phf->seed = 0;
}

MU_INLINE void mu_phf_destroy(mu_phf* phf)
{
    if (!phf)
        return;
    MU_FREE(phf->keys);
    MU_FREE(phf->values);
    MU_FREE(phf->displacement);
    mu_phf_init(phf);
}

MU_INLINE bool mu_phf__build_attempt(mu_phf* phf,
                                     const uint64_t* keys,
                                     const uint64_t* values,
                                     uint32_t n,
                                     uint32_t bucket_count,
                                     uint64_t seed)
{
    uint32_t* counts = NULL;
    uint32_t* offsets = NULL;
    uint32_t* cursor = NULL;
    uint32_t* order = NULL;
    mu_phf__bucket* buckets = NULL;
    uint64_t* occupied = NULL;
    uint32_t* mark = NULL;
    size_t word_count;
    uint32_t i;
    uint32_t b;
    uint32_t stamp = 0;
    uint32_t placed = 0;
    bool ok = false;

    word_count = ((size_t)n + 63u) / 64u;

    phf->keys = (uint64_t*)MU_MALLOC((size_t)n * sizeof(uint64_t));
    phf->values = (uint64_t*)MU_MALLOC((size_t)n * sizeof(uint64_t));
    phf->displacement = (uint32_t*)MU_CALLOC(bucket_count, sizeof(uint32_t));
    counts = (uint32_t*)MU_CALLOC(bucket_count, sizeof(uint32_t));
    offsets = (uint32_t*)MU_MALLOC(((size_t)bucket_count + 1u) * sizeof(uint32_t));
    cursor = (uint32_t*)MU_MALLOC((size_t)bucket_count * sizeof(uint32_t));
    order = (uint32_t*)MU_MALLOC((size_t)n * sizeof(uint32_t));
    buckets = (mu_phf__bucket*)MU_MALLOC((size_t)bucket_count * sizeof(mu_phf__bucket));
    occupied = (uint64_t*)MU_CALLOC(word_count, sizeof(uint64_t));
    mark = (uint32_t*)MU_CALLOC(n, sizeof(uint32_t));

    if (!phf->keys || !phf->values || !phf->displacement || !counts || !offsets ||
        !cursor || !order || !buckets || !occupied || !mark)
        goto cleanup;

    for (i = 0; i < n; ++i)
    {
        b = mu_phf__h1(keys[i], seed, bucket_count);
        counts[b] += 1;
    }

    offsets[0] = 0;
    for (b = 0; b < bucket_count; ++b)
    {
        offsets[b + 1] = offsets[b] + counts[b];
        cursor[b] = offsets[b];
    }

    for (i = 0; i < n; ++i)
    {
        b = mu_phf__h1(keys[i], seed, bucket_count);
        order[cursor[b]++] = i;
    }

    for (b = 0; b < bucket_count; ++b)
    {
        buckets[b].count = counts[b];
        buckets[b].index = b;
    }

    qsort(buckets, bucket_count, sizeof(mu_phf__bucket), mu_phf__bucket_cmp);

    for (i = 0; i < bucket_count; ++i)
    {
        uint32_t start = offsets[buckets[i].index];
        uint32_t count = buckets[i].count;
        bool placed_bucket = false;

        if (count == 0)
            continue;

        b = buckets[i].index;

        for (uint32_t d = 0; d < n; ++d)
        {
            uint32_t current;
            bool fits = true;

            if (stamp == 0)
            {
                memset(mark, 0, (size_t)n * sizeof(uint32_t));
                stamp = 1;
            }
            current = stamp++;

            for (uint32_t p = start; p < start + count; ++p)
            {
                uint32_t slot = mu_phf__h2(keys[order[p]], seed, d, n);

                if ((occupied[slot >> 6] & (UINT64_C(1) << (slot & 63u))) != 0 ||
                    mark[slot] == current)
                {
                    fits = false;
                    break;
                }
                mark[slot] = current;
            }

            if (fits)
            {
                for (uint32_t p = start; p < start + count; ++p)
                {
                    uint32_t original = order[p];
                    uint32_t slot = mu_phf__h2(keys[original], seed, d, n);

                    occupied[slot >> 6] |= UINT64_C(1) << (slot & 63u);
                    phf->keys[slot] = keys[original];
                    phf->values[slot] = values ? values[original] : (uint64_t)original;
                }
                phf->displacement[b] = d;
                placed += count;
                placed_bucket = true;
                break;
            }
        }

        if (!placed_bucket)
            goto cleanup;
    }

    ok = placed == n;

cleanup:
    MU_FREE(counts);
    MU_FREE(offsets);
    MU_FREE(cursor);
    MU_FREE(order);
    MU_FREE(buckets);
    MU_FREE(occupied);
    MU_FREE(mark);
    return ok;
}

MU_INLINE bool mu_phf_build(mu_phf* phf,
                            const uint64_t* keys,
                            const uint64_t* values,
                            uint32_t n,
                            uint64_t seed)
{
    uint32_t bucket_count;
    uint32_t attempt;

    if (!phf)
        return false;

    mu_phf_destroy(phf);

    if (n == 0)
        return true;
    if (!keys)
        return false;

    if (n > 1)
    {
        uint64_t* sorted = (uint64_t*)MU_MALLOC((size_t)n * sizeof(uint64_t));
        uint32_t i;

        if (!sorted)
            return false;
        memcpy(sorted, keys, (size_t)n * sizeof(uint64_t));
        qsort(sorted, n, sizeof(uint64_t), mu_phf__u64_cmp);

        for (i = 1; i < n; ++i)
        {
            if (sorted[i] == sorted[i - 1])
            {
                MU_FREE(sorted);
                return false;
            }
        }
        MU_FREE(sorted);
    }

    bucket_count = n / 3u + (n % 3u != 0u);

    for (attempt = 0; attempt < 1024u; ++attempt)
    {
        uint64_t attempt_seed =
            seed + (uint64_t)attempt * UINT64_C(0x9e3779b97f4a7c15);

        if (mu_phf__build_attempt(phf, keys, values, n, bucket_count, attempt_seed))
        {
            phf->n = n;
            phf->bucket_count = bucket_count;
            phf->seed = attempt_seed;
            return true;
        }

        mu_phf_destroy(phf);
    }

    return false;
}

MU_INLINE bool mu_phf_lookup(const mu_phf* phf, uint64_t key, uint64_t* out_value)
{
    uint32_t bucket;
    uint32_t slot;

    if (!phf || phf->n == 0 || phf->bucket_count == 0 ||
        !phf->keys || !phf->values || !phf->displacement)
        return false;

    bucket = mu_phf__h1(key, phf->seed, phf->bucket_count);
    slot = mu_phf__h2(key, phf->seed, phf->displacement[bucket], phf->n);

    if (phf->keys[slot] != key)
        return false;

    if (out_value)
        *out_value = phf->values[slot];
    return true;
}

MU_INLINE bool mu_phf_contains(const mu_phf* phf, uint64_t key)
{
    return mu_phf_lookup(phf, key, NULL);
}

MU_INLINE uint64_t mu_phf_value_or(const mu_phf* phf, uint64_t key, uint64_t fallback)
{
    uint64_t value;
    return mu_phf_lookup(phf, key, &value) ? value : fallback;
}

MU_INLINE uint32_t mu_phf_count(const mu_phf* phf)
{
    return phf ? phf->n : 0;
}

MU_INLINE uint32_t mu_phf_bucket_count(const mu_phf* phf)
{
    return phf ? phf->bucket_count : 0;
}

MU_INLINE uint64_t mu_phf_seed(const mu_phf* phf)
{
    return phf ? phf->seed : 0;
}

MU_INLINE bool mu_phf_empty(const mu_phf* phf)
{
    return !phf || phf->n == 0;
}

MU_END_EXTERN_C

#endif
