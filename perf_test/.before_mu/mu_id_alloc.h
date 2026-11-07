#pragma once
#ifndef MU_ID_ALLOC_H
#define MU_ID_ALLOC_H

/*
 * mu_id_alloc.h -- dense, thread-safe, and sparse numeric ID allocators.
 *
 * Tracks allocated IDs with a bitmap. IDs are returned in [0, UINT32_MAX).
 * UINT32_MAX means "allocation failed / exhausted".
 *
 * mu_id_alloc_sparse divides the 32-bit ID space into 1024 independently
 * grown segments, so using a high ID does not reserve 512 MB of address space.
 *
  */

#include "mu_common.h"
#include "mu_sync.h"

MU_BEGIN_EXTERN_C

typedef struct mu_id_alloc
{
    uint32_t* data;
    unsigned num_elements;
    unsigned num_set_elements;
    unsigned lowest_free_idx;
} mu_id_alloc;

MU_INLINE void mu_id_alloc__zero(mu_id_alloc* buf)
{
    buf->data = NULL;
    buf->num_elements = 0;
    buf->num_set_elements = 0;
    buf->lowest_free_idx = 0;
}

MU_INLINE bool mu_id_alloc_resize(mu_id_alloc* buf, unsigned new_num_elements)
{
    if (new_num_elements <= buf->num_elements)
        return true;

#if SIZE_MAX <= UINT32_MAX
    if ((size_t)new_num_elements > SIZE_MAX / sizeof(uint32_t))
        return false;
#endif

    {
        uint32_t* data = (uint32_t*)MU_REALLOC(
            buf->data, (size_t)new_num_elements * sizeof(uint32_t));
        if (!data)
            return false;

        MU_MEMSET(data + buf->num_elements, 0,
                  (size_t)(new_num_elements - buf->num_elements) * sizeof(uint32_t));
        buf->data = data;
        buf->num_elements = new_num_elements;
    }
    return true;
}

MU_INLINE bool mu_id_alloc_init(mu_id_alloc* buf, unsigned initial_num_ids)
{
    MU_ASSERT(buf != NULL);
    mu_id_alloc__zero(buf);
    if (initial_num_ids == 0)
        return false;
    return mu_id_alloc_resize(buf, (initial_num_ids + 31u) / 32u);
}

MU_INLINE void mu_id_alloc_fini(mu_id_alloc* buf)
{
    if (!buf)
        return;
    MU_FREE(buf->data);
    mu_id_alloc__zero(buf);
}

MU_INLINE uint32_t mu_id_alloc_alloc(mu_id_alloc* buf)
{
    unsigned i;
    unsigned num_elements = buf->num_elements;

    for (i = buf->lowest_free_idx; i < num_elements; ++i)
    {
        uint32_t word = buf->data[i];
        unsigned bit;

        if (word == UINT32_MAX)
            continue;

        bit = mu_trailing_zeroes_u64((uint64_t)(uint32_t)~word);
        buf->data[i] = word | (UINT32_C(1) << bit);
        buf->lowest_free_idx = i;
        if (i + 1u > buf->num_set_elements)
            buf->num_set_elements = i + 1u;
        return i * 32u + bit;
    }

    {
        unsigned new_elements = (num_elements ? num_elements : 1u) * 2u;
        if (new_elements <= num_elements)
            return MU_INVALID_INDEX;
        if (!mu_id_alloc_resize(buf, new_elements))
            return MU_INVALID_INDEX;
    }

    buf->lowest_free_idx = num_elements;
    buf->data[num_elements] |= 1u;
    if (num_elements + 1u > buf->num_set_elements)
        buf->num_set_elements = num_elements + 1u;
    return num_elements * 32u;
}

MU_INLINE unsigned mu_id_alloc__find_free_block(const mu_id_alloc* buf, unsigned start)
{
    unsigned i;
    for (i = start; i < buf->num_elements; ++i)
        if (buf->data[i] == 0)
            return i;
    return buf->num_elements;
}

MU_INLINE uint32_t mu_id_alloc_alloc_range(mu_id_alloc* buf, unsigned num)
{
    unsigned num_alloc;
    unsigned num_elements;
    unsigned base;
    unsigned i;

    MU_ASSERT(buf != NULL);
    if (num == 0 || num > UINT32_MAX - 31u)
        return MU_INVALID_INDEX;
    if (num == 1)
        return mu_id_alloc_alloc(buf);

    num_alloc = (num + 31u) / 32u;
    num_elements = buf->num_elements;
    base = mu_id_alloc__find_free_block(buf, buf->lowest_free_idx);

    for (;;)
    {
        for (i = base; i < num_elements && i - base < num_alloc && buf->data[i] == 0; ++i)
            ;
        if (i - base == num_alloc)
            break;
        if (i == num_elements)
            break;
        base = (buf->data[i] == 0) ? i : i + 1u;
    }

    if (i - base != num_alloc)
    {
        if (num_elements > (UINT32_MAX - num_alloc) / 2u)
            return MU_INVALID_INDEX;
        if (!mu_id_alloc_resize(buf, num_elements * 2u + num_alloc))
            return MU_INVALID_INDEX;
        num_elements = buf->num_elements;
    }

    for (i = base; i < base + num_alloc - (num % 32u != 0u); ++i)
        buf->data[i] = UINT32_MAX;
    if (num % 32u != 0u)
        buf->data[base + num_alloc - 1u] |= (UINT32_C(1) << (num % 32u)) - 1u;

    if (buf->lowest_free_idx == base)
        buf->lowest_free_idx = base + num / 32u;
    if (base + num_alloc > buf->num_set_elements)
        buf->num_set_elements = base + num_alloc;

    if ((uint64_t)base * 32u + num > (uint64_t)UINT32_MAX + 1u)
        return MU_INVALID_INDEX;

    return base * 32u;
}

MU_INLINE bool mu_id_alloc_test(const mu_id_alloc* buf, uint32_t id)
{
    unsigned idx;
    if (!buf || id == MU_INVALID_INDEX)
        return false;
    idx = id / 32u;
    return idx < buf->num_elements &&
           (buf->data[idx] & (UINT32_C(1) << (id % 32u))) != 0;
}

MU_INLINE void mu_id_alloc_free(mu_id_alloc* buf, uint32_t id)
{
    unsigned idx;
    if (!buf || id == MU_INVALID_INDEX)
        return;

    idx = id / 32u;
    if (idx >= buf->num_elements)
        return;

    if (idx < buf->lowest_free_idx)
        buf->lowest_free_idx = idx;
    buf->data[idx] &= ~(UINT32_C(1) << (id % 32u));

    if (buf->num_set_elements == idx + 1u)
    {
        while (buf->num_set_elements > 0u &&
               buf->data[buf->num_set_elements - 1u] == 0u)
            --buf->num_set_elements;
    }
}

MU_INLINE void mu_id_alloc_reserve(mu_id_alloc* buf, uint32_t id)
{
    unsigned idx;
    MU_ASSERT(buf != NULL);
    if (id == MU_INVALID_INDEX)
        return;

    idx = id / 32u;
    if (idx >= buf->num_elements)
    {
        unsigned new_elements = (idx + 1u) * 2u;
        if (!mu_id_alloc_resize(buf, new_elements))
            return;
    }

    buf->data[idx] |= UINT32_C(1) << (id % 32u);
    if (idx + 1u > buf->num_set_elements)
        buf->num_set_elements = idx + 1u;
}

#define mu_id_alloc_foreach(buf, id)                                                    \
    for (uint32_t _mu_i = 0, _mu_w = 0, _mu_count = (buf)->num_set_elements, id;         \
         _mu_i < _mu_count; ++_mu_i)                                                    \
        for (_mu_w = (buf)->data[_mu_i]; _mu_w != 0; _mu_w &= _mu_w - 1u)                \
            if ((id = _mu_i * 32u + mu_trailing_zeroes_u64(_mu_w)), true)

typedef struct mu_id_alloc_mt
{
    mu_id_alloc buf;
    mu_spinlock lock;
    bool skip_zero;
} mu_id_alloc_mt;

MU_INLINE bool mu_id_alloc_mt_init(mu_id_alloc_mt* buf, unsigned initial_num_ids,
                                   bool skip_zero)
{
    MU_ASSERT(buf != NULL);
    mu_spinlock_init(&buf->lock);
    buf->skip_zero = skip_zero;
    if (!mu_id_alloc_init(&buf->buf, initial_num_ids))
        return false;

    if (skip_zero)
    {
        uint32_t zero = mu_id_alloc_alloc(&buf->buf);
        MU_ASSERT(zero == 0);
        MU_UNUSED(zero);
    }
    return true;
}

MU_INLINE void mu_id_alloc_mt_fini(mu_id_alloc_mt* buf)
{
    if (!buf)
        return;
    mu_id_alloc_fini(&buf->buf);
}

MU_INLINE uint32_t mu_id_alloc_mt_alloc(mu_id_alloc_mt* buf)
{
    uint32_t id;
    MU_ASSERT(buf != NULL);
    mu_spinlock_lock(&buf->lock);
    id = mu_id_alloc_alloc(&buf->buf);
    mu_spinlock_unlock(&buf->lock);
    return id;
}

MU_INLINE void mu_id_alloc_mt_free(mu_id_alloc_mt* buf, uint32_t id)
{
    MU_ASSERT(buf != NULL);
    if (id == 0 && buf->skip_zero)
        return;
    mu_spinlock_lock(&buf->lock);
    mu_id_alloc_free(&buf->buf, id);
    mu_spinlock_unlock(&buf->lock);
}

#define MU_ID_ALLOC_SPARSE_SEGMENTS 1024u
#define MU_ID_ALLOC_SPARSE_LOG2_IDS_PER_SEGMENT 22u
#define MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT \
    (UINT32_C(1) << MU_ID_ALLOC_SPARSE_LOG2_IDS_PER_SEGMENT)
#define MU_ID_ALLOC_SPARSE_ELEMS_PER_SEGMENT \
    (MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT / 32u)

typedef struct mu_id_alloc_sparse
{
    mu_id_alloc segment[MU_ID_ALLOC_SPARSE_SEGMENTS];
} mu_id_alloc_sparse;

MU_INLINE bool mu_id_alloc_sparse_init(mu_id_alloc_sparse* buf)
{
    unsigned i;
    MU_ASSERT(buf != NULL);

    for (i = 0; i < MU_ID_ALLOC_SPARSE_SEGMENTS; ++i)
    {
        if (!mu_id_alloc_init(&buf->segment[i], 1))
        {
            while (i > 0)
                mu_id_alloc_fini(&buf->segment[--i]);
            return false;
        }
    }
    return true;
}

MU_INLINE void mu_id_alloc_sparse_fini(mu_id_alloc_sparse* buf)
{
    unsigned i;
    if (!buf)
        return;
    for (i = 0; i < MU_ID_ALLOC_SPARSE_SEGMENTS; ++i)
        mu_id_alloc_fini(&buf->segment[i]);
}

MU_INLINE uint32_t mu_id_alloc_sparse_alloc(mu_id_alloc_sparse* buf)
{
    unsigned i;
    MU_ASSERT(buf != NULL);

    for (i = 0; i < MU_ID_ALLOC_SPARSE_SEGMENTS; ++i)
    {
        if (buf->segment[i].lowest_free_idx < MU_ID_ALLOC_SPARSE_ELEMS_PER_SEGMENT)
        {
            uint32_t id = mu_id_alloc_alloc(&buf->segment[i]);
            if (id == MU_INVALID_INDEX)
                continue;
            if (id < MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT)
                return i * MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT + id;
            mu_id_alloc_free(&buf->segment[i], id);
        }
    }
    return MU_INVALID_INDEX;
}

MU_INLINE uint32_t mu_id_alloc_sparse_alloc_range(mu_id_alloc_sparse* buf, unsigned num)
{
    unsigned i;
    unsigned num_elems;
    MU_ASSERT(buf != NULL);
    if (num == 0 || num > MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT)
        return MU_INVALID_INDEX;

    num_elems = (num + 31u) / 32u;
    for (i = 0; i < MU_ID_ALLOC_SPARSE_SEGMENTS; ++i)
    {
        mu_id_alloc* seg = &buf->segment[i];
        if (seg->lowest_free_idx + num_elems <= MU_ID_ALLOC_SPARSE_ELEMS_PER_SEGMENT)
        {
            uint32_t base = mu_id_alloc_alloc_range(seg, num);
            if (base != MU_INVALID_INDEX &&
                base + num <= MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT)
            {
                return i * MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT + base;
            }
            if (base != MU_INVALID_INDEX)
            {
                unsigned j;
                for (j = 0; j < num; ++j)
                    mu_id_alloc_free(seg, base + j);
            }
        }
    }
    return MU_INVALID_INDEX;
}

MU_INLINE void mu_id_alloc_sparse_free(mu_id_alloc_sparse* buf, uint32_t id)
{
    MU_ASSERT(buf != NULL);
    if (id == MU_INVALID_INDEX)
        return;
    mu_id_alloc_free(&buf->segment[id / MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT],
                     id % MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT);
}

MU_INLINE void mu_id_alloc_sparse_reserve(mu_id_alloc_sparse* buf, uint32_t id)
{
    MU_ASSERT(buf != NULL);
    if (id == MU_INVALID_INDEX)
        return;
    mu_id_alloc_reserve(&buf->segment[id / MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT],
                        id % MU_ID_ALLOC_SPARSE_IDS_PER_SEGMENT);
}

MU_END_EXTERN_C

#endif /* MU_ID_ALLOC_H */
