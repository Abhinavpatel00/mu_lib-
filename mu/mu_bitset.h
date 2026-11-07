#pragma once

#include "mu_common.h"


typedef struct mu_bitset
{
    uint64_t*  array;  // mu_strict hona chahiye but lsp complaining       /* pointer to 64-bit word storage */
    size_t                word_count;    /* number words stored */
    size_t                word_capacity; /* allocated capacity in 64-bit words */
} mu_bitset;

/* Create a new bitset. Return NULL in case of failure. */
mu_bitset* mu_bitset_create(void);

/* Create a new bitset able to contain size bits. Return NULL in case of failure. */
mu_bitset* mu_bitset_create_with_capacity(size_t size);

/* Free memory. */
void mu_bitset_free(mu_bitset* bitset);

/* Set all bits to zero. */
void mu_bitset_clear(mu_bitset* bitset);

/* Set all bits to one. */
void mu_bitset_fill(mu_bitset* bitset);

/* Create a copy. */
mu_bitset* mu_bitset_copy(const mu_bitset* src);

/* Resize in 64-bit words. New words are zeroed. */
bool mu_bitset_resize_words(mu_bitset* bs, size_t new_word_count);

/* Grow in 64-bit words. */
bool mu_bitset_grow(mu_bitset* bs, size_t new_word_count);

/* attempts to recover unused memory, return false in case of reallocation failure */
bool mu_bitset_trim(mu_bitset* bs);

/* shifts all bits by shift positions so values 1,2,10 become 1+shift,2+shift,10+shift */
void mu_bitset_shift_left(mu_bitset* bs, size_t shift);

/* shifts all bits by shift positions so values 1,2,10 become 1-shift,2-shift,10-shift */
void mu_bitset_shift_right(mu_bitset* bs, size_t shift);

/* Set/reset/test bits. */
void mu_bitset_set(mu_bitset* bs, size_t bit_index);
void mu_bitset_reset(mu_bitset* bs, size_t bit_index);
bool mu_bitset_test(const mu_bitset* bs, size_t bit_index);
void mu_bitset_enable_bit(mu_bitset* bs, size_t bit_index);
void mu_bitset_disable_bit(mu_bitset* bs, size_t bit_index);
void mu_bitset_set_bit(mu_bitset* bs, size_t bit_index, bool value);
bool mu_bitset_get_bit(const mu_bitset* bs, size_t bit_index);

/* Query sizes. */
MU_INLINE size_t mu_bitset_size_in_bytes(const mu_bitset* bs)
{
    return bs->word_count * sizeof(uint64_t);
}

MU_INLINE size_t mu_bitset_size_in_bits(const mu_bitset* bs)
{
    return bs->word_count * 64;
}

MU_INLINE size_t mu_bitset_size_in_words(const mu_bitset* bs)
{
    return bs->word_count;
}

/* Set algebra and counters. */
size_t mu_bitset_count(const mu_bitset* bs);
bool   mu_bitset_empty(const mu_bitset* bs);
size_t mu_bitset_minimum(const mu_bitset* bs);
size_t mu_bitset_maximum(const mu_bitset* bs);
bool   mu_bitset_equal(const mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b);
bool   mu_bitsets_disjoint(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
bool   mu_bitsets_intersect(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
bool   mu_bitset_contains_all(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
bool   mu_bitset_inplace_union(mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
size_t mu_bitset_union_count(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
void   mu_bitset_inplace_intersection(mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
size_t mu_bitset_intersection_count(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
void   mu_bitset_inplace_difference(mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
size_t mu_bitset_difference_count(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
bool   mu_bitset_inplace_symmetric_difference(mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
size_t mu_bitset_symmetric_difference_count(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2);
bool   mu_bitset_xor(mu_bitset* MU_RESTRICT out, const mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b);
bool   mu_bitset_or(mu_bitset* MU_RESTRICT out, const mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b);
bool   mu_bitset_and(mu_bitset* MU_RESTRICT out, const mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b);
bool   mu_bitset_not(mu_bitset* MU_RESTRICT out, const mu_bitset* MU_RESTRICT a);
bool   mu_bitset_xor_assign(mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b);
bool   mu_bitset_or_assign(mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b);
bool   mu_bitset_and_assign(mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b);

/* Iteration helpers. */
typedef bool (*mu_bitset_iterator)(size_t value, void* param);
typedef mu_bitset_iterator mu_bitset_visit_fn;

void mu_bitset_traverse(const mu_bitset* bs, mu_bitset_visit_fn visitor, void* user);
void mu_bitset_traverse_range(const mu_bitset* bs, mu_bitset_visit_fn visitor, void* user, size_t begin, size_t count);

MU_INLINE bool mu_bitset_next_set_bit(const mu_bitset* bs, size_t* i)
{
    size_t x = *i / 64;
    if(x >= bs->word_count)
    {
        return false;
    }
    uint64_t w = bs->array[x];
    w >>= (*i & 63);
    if(w != 0)
    {
        *i += (size_t)mu_trailing_zeroes_u64(w);
        return true;
    }
    x++;
    while(x < bs->word_count)
    {
        w = bs->array[x];
        if(w != 0)
        {
            *i = x * 64 + (size_t)mu_trailing_zeroes_u64(w);
            return true;
        }
        x++;
    }
    return false;
}

MU_INLINE size_t mu_bitset_next_set_bits(const mu_bitset* bs, size_t* buffer, size_t capacity, size_t* startfrom)
{
    if(capacity == 0)
        return 0;

    size_t x = *startfrom / 64;
    if(x >= bs->word_count)
    {
        return 0;
    }

    uint64_t w = bs->array[x];
    w &= ~((UINT64_C(1) << (*startfrom & 63)) - 1);

    size_t howmany = 0;
    size_t base    = x << 6;
    while(howmany < capacity)
    {
        while(w != 0)
        {
            uint64_t t        = w & (~w + 1);
            int      r        = mu_trailing_zeroes_u64(w);
            buffer[howmany++] = (size_t)r + base;
            if(howmany == capacity)
                goto end;
            w ^= t;
        }
        x += 1;
        if(x == bs->word_count)
        {
            break;
        }
        base += 64;
        w = bs->array[x];
    }
end:
    if(howmany > 0)
    {
        *startfrom = buffer[howmany - 1];
    }
    return howmany;
}

MU_INLINE bool mu_bitset_for_each(const mu_bitset* bs, mu_bitset_iterator iterator, void* ptr)
{
    size_t base = 0;
    for(size_t i = 0; i < bs->word_count; ++i)
    {
        uint64_t w = bs->array[i];
        while(w != 0)
        {
            uint64_t t = w & (~w + 1);
            int      r = mu_trailing_zeroes_u64(w);
            if(!iterator((size_t)r + base, ptr))
                return false;
            w ^= t;
        }
        base += 64;
    }
    return true;
}

void mu_bitset_print(const mu_bitset* b);

