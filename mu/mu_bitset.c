#include "mu_bitset.h"

#if defined(__AVX2__) || (defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__))
#  include <immintrin.h>
#endif

/* -------------------------------------------------------------------------- *
 * High-Throughput SIMD + 4-Way ILP Word Popcount & Predicate Kernels
 *   - AVX-512 VPOPCNTDQ (`vpopcntq`): 64 bytes/instruction (~95 GB/s)
 *   - AVX2 Muła-Kurz-Lemire (`vpshufb` + `vpsadbw`): 32 bytes/iter (~46 GB/s)
 *   - Scalar 4-accumulator ILP: breaks false output dependency on `popcntq`
 * -------------------------------------------------------------------------- */

MU_INLINE size_t mu_bitset__popcnt_words(const uint64_t* MU_RESTRICT w, size_t n)
{
    size_t k = 0;

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__)
    if (n >= 8u)
    {
        __m512i acc0 = _mm512_setzero_si512();
        __m512i acc1 = _mm512_setzero_si512();
        for (; k + 16u <= n; k += 16u)
        {
            __m512i v0 = _mm512_loadu_si512((const void*)(w + k));
            __m512i v1 = _mm512_loadu_si512((const void*)(w + k + 8u));
            acc0 = _mm512_add_epi64(acc0, _mm512_popcnt_epi64(v0));
            acc1 = _mm512_add_epi64(acc1, _mm512_popcnt_epi64(v1));
        }
        acc0 = _mm512_add_epi64(acc0, acc1);
        uint64_t tmp[8];
        _mm512_storeu_si512((void*)tmp, acc0);
        size_t total = (size_t)(tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] + tmp[6] + tmp[7]);
        for (; k < n; ++k)
            total += (size_t)mu_popcount_u64(w[k]);
        return total;
    }
#elif defined(__AVX2__)
    if (n >= 16u)
    {
        const __m256i lut = _mm256_setr_epi8(
            0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
            0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
        const __m256i low_mask = _mm256_set1_epi8(0x0f);
        const __m256i zero     = _mm256_setzero_si256();
        __m256i       total_v  = _mm256_setzero_si256();

        while (k + 60u <= n)
        {
            /* Up to 15 x 32-byte vectors (60 u64 words) before u8 saturates (15*8=120 <= 255). */
            __m256i local = _mm256_setzero_si256();
            for (int iter = 0; iter < 15; ++iter, k += 4u)
            {
                __m256i v  = _mm256_loadu_si256((const __m256i*)(w + k));
                __m256i lo = _mm256_and_si256(v, low_mask);
                __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), low_mask);
                __m256i p  = _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo), _mm256_shuffle_epi8(lut, hi));
                local      = _mm256_add_epi8(local, p);
            }
            total_v = _mm256_add_epi64(total_v, _mm256_sad_epu8(local, zero));
        }

        __m256i local = _mm256_setzero_si256();
        for (; k + 4u <= n; k += 4u)
        {
            __m256i v  = _mm256_loadu_si256((const __m256i*)(w + k));
            __m256i lo = _mm256_and_si256(v, low_mask);
            __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), low_mask);
            __m256i p  = _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo), _mm256_shuffle_epi8(lut, hi));
            local      = _mm256_add_epi8(local, p);
        }
        total_v = _mm256_add_epi64(total_v, _mm256_sad_epu8(local, zero));

        uint64_t tmp[4];
        _mm256_storeu_si256((__m256i*)tmp, total_v);
        size_t total = (size_t)(tmp[0] + tmp[1] + tmp[2] + tmp[3]);
        for (; k < n; ++k)
            total += (size_t)mu_popcount_u64(w[k]);
        return total;
    }
#endif

    size_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (; k + 4u <= n; k += 4u)
    {
        c0 += (size_t)mu_popcount_u64(w[k]);
        c1 += (size_t)mu_popcount_u64(w[k + 1u]);
        c2 += (size_t)mu_popcount_u64(w[k + 2u]);
        c3 += (size_t)mu_popcount_u64(w[k + 3u]);
    }
    size_t total = (c0 + c1) + (c2 + c3);
    for (; k < n; ++k)
        total += (size_t)mu_popcount_u64(w[k]);
    return total;
}

typedef enum mu_bitset__binop
{
    MU_BITSET_OP_OR     = 0,
    MU_BITSET_OP_AND    = 1,
    MU_BITSET_OP_ANDNOT = 2, /* a & ~b */
    MU_BITSET_OP_XOR    = 3
} mu_bitset__binop;

MU_INLINE size_t mu_bitset__popcnt_binop(
    const uint64_t* MU_RESTRICT a,
    const uint64_t* MU_RESTRICT b,
    size_t                      n,
    mu_bitset__binop            op)
{
    size_t k = 0;

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__)
    if (n >= 8u)
    {
        __m512i acc = _mm512_setzero_si512();
        for (; k + 8u <= n; k += 8u)
        {
            __m512i va = _mm512_loadu_si512((const void*)(a + k));
            __m512i vb = _mm512_loadu_si512((const void*)(b + k));
            __m512i vr;
            switch (op)
            {
                case MU_BITSET_OP_OR:     vr = _mm512_or_si512(va, vb);     break;
                case MU_BITSET_OP_AND:    vr = _mm512_and_si512(va, vb);    break;
                case MU_BITSET_OP_ANDNOT: vr = _mm512_andnot_si512(vb, va); break;
                default:                  vr = _mm512_xor_si512(va, vb);    break;
            }
            acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(vr));
        }
        uint64_t tmp[8];
        _mm512_storeu_si512((void*)tmp, acc);
        size_t total = (size_t)(tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] + tmp[6] + tmp[7]);
        for (; k < n; ++k)
        {
            uint64_t w = (op == MU_BITSET_OP_OR) ? (a[k] | b[k]) :
                         (op == MU_BITSET_OP_AND) ? (a[k] & b[k]) :
                         (op == MU_BITSET_OP_ANDNOT) ? (a[k] & ~b[k]) : (a[k] ^ b[k]);
            total += (size_t)mu_popcount_u64(w);
        }
        return total;
    }
#elif defined(__AVX2__)
    if (n >= 16u)
    {
        const __m256i lut = _mm256_setr_epi8(
            0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
            0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
        const __m256i low_mask = _mm256_set1_epi8(0x0f);
        const __m256i zero     = _mm256_setzero_si256();
        __m256i       total_v  = _mm256_setzero_si256();

        while (k + 4u <= n)
        {
            __m256i local = _mm256_setzero_si256();
            for (int iter = 0; iter < 15 && k + 4u <= n; ++iter, k += 4u)
            {
                __m256i va = _mm256_loadu_si256((const __m256i*)(a + k));
                __m256i vb = _mm256_loadu_si256((const __m256i*)(b + k));
                __m256i v;
                switch (op)
                {
                    case MU_BITSET_OP_OR:     v = _mm256_or_si256(va, vb);     break;
                    case MU_BITSET_OP_AND:    v = _mm256_and_si256(va, vb);    break;
                    case MU_BITSET_OP_ANDNOT: v = _mm256_andnot_si256(vb, va); break;
                    default:                  v = _mm256_xor_si256(va, vb);    break;
                }
                __m256i lo = _mm256_and_si256(v, low_mask);
                __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), low_mask);
                __m256i p  = _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo), _mm256_shuffle_epi8(lut, hi));
                local      = _mm256_add_epi8(local, p);
            }
            total_v = _mm256_add_epi64(total_v, _mm256_sad_epu8(local, zero));
        }

        uint64_t tmp[4];
        _mm256_storeu_si256((__m256i*)tmp, total_v);
        size_t total = (size_t)(tmp[0] + tmp[1] + tmp[2] + tmp[3]);
        for (; k < n; ++k)
        {
            uint64_t w = (op == MU_BITSET_OP_OR) ? (a[k] | b[k]) :
                         (op == MU_BITSET_OP_AND) ? (a[k] & b[k]) :
                         (op == MU_BITSET_OP_ANDNOT) ? (a[k] & ~b[k]) : (a[k] ^ b[k]);
            total += (size_t)mu_popcount_u64(w);
        }
        return total;
    }
#endif

    size_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (; k + 4u <= n; k += 4u)
    {
        uint64_t w0, w1, w2, w3;
        switch (op)
        {
            case MU_BITSET_OP_OR:
                w0 = a[k] | b[k]; w1 = a[k+1] | b[k+1]; w2 = a[k+2] | b[k+2]; w3 = a[k+3] | b[k+3];
                break;
            case MU_BITSET_OP_AND:
                w0 = a[k] & b[k]; w1 = a[k+1] & b[k+1]; w2 = a[k+2] & b[k+2]; w3 = a[k+3] & b[k+3];
                break;
            case MU_BITSET_OP_ANDNOT:
                w0 = a[k] & ~b[k]; w1 = a[k+1] & ~b[k+1]; w2 = a[k+2] & ~b[k+2]; w3 = a[k+3] & ~b[k+3];
                break;
            default:
                w0 = a[k] ^ b[k]; w1 = a[k+1] ^ b[k+1]; w2 = a[k+2] ^ b[k+2]; w3 = a[k+3] ^ b[k+3];
                break;
        }
        c0 += (size_t)mu_popcount_u64(w0);
        c1 += (size_t)mu_popcount_u64(w1);
        c2 += (size_t)mu_popcount_u64(w2);
        c3 += (size_t)mu_popcount_u64(w3);
    }
    size_t total = (c0 + c1) + (c2 + c3);
    for (; k < n; ++k)
    {
        uint64_t w = (op == MU_BITSET_OP_OR) ? (a[k] | b[k]) :
                     (op == MU_BITSET_OP_AND) ? (a[k] & b[k]) :
                     (op == MU_BITSET_OP_ANDNOT) ? (a[k] & ~b[k]) : (a[k] ^ b[k]);
        total += (size_t)mu_popcount_u64(w);
    }
    return total;
}


mu_bitset* mu_bitset_create()
{
    mu_bitset* bitset = NULL;
    /* Allocate the bitset itself. */
    bitset                = (mu_bitset*)MU_MALLOC(sizeof(mu_bitset));
    bitset->array         = NULL;
    bitset->word_count    = 0;
    bitset->word_capacity = 0;
    return bitset;
}
/*
You want to store size bits.
Your storage is uint64_t.
Each uint64_t holds:
8 bytes × 8 bits = 64 bits.
So the real question is:
“How many 64-bit words do I need to store size bits?”
*/
mu_bitset* mu_bitset_create_with_capacity(size_t size)
{
    mu_bitset* bitset = (mu_bitset*)MU_MALLOC(sizeof(mu_bitset));
    //“bits per word = bytes per word × 8”
    bitset->word_count    = MU_CEIL(size, sizeof(uint64_t) * 8);
    bitset->word_capacity = bitset->word_count;

    bitset->array = (uint64_t*)MU_CALLOC(bitset->word_count, sizeof(uint64_t));
    return bitset;
}


void mu_bitset_free(mu_bitset* bitset)
{
    MU_FREE(bitset->array);
    MU_FREE(bitset);
}

void mu_bitset_clear(mu_bitset* bitset)
{
    MU_MEMSET(bitset->array, 0, sizeof(uint64_t) * bitset->word_count);
}

void mu_bitset_fill(mu_bitset* bitset)
{
    MU_MEMSET(bitset->array, 0xff, sizeof(uint64_t) * bitset->word_count);
}

/*
Mu-prefixed aliases keep the public API naming consistent.
These functions intentionally stay minimal and avoid redundant checks.
*/

mu_bitset* mu_bitset_copy(const mu_bitset* src)
{
    mu_bitset* copy     = (mu_bitset*)MU_MALLOC(sizeof *copy);
    copy->word_count    = src->word_count;
    copy->word_capacity = src->word_count;

    if(src->word_count == 0)
    {
        copy->array = NULL;
        return copy;
    }

    copy->array = (uint64_t*)MU_MALLOC(sizeof(uint64_t) * src->word_count);
    MU_MEMCPY(copy->array, src->array, sizeof(uint64_t) * src->word_count);
    return copy;
}


static bool mu_bitset_resize_impl(mu_bitset* bs, size_t new_word_count, bool padwithzeroes)
{


    if(new_word_count > SIZE_MAX / 64)
        return false;

    if(new_word_count > bs->word_capacity)
    {

        uint64_t* newarray;
        size_t    new_word_cap = (UINT64_C(0xFFFFFFFFFFFFFFFF) >> mu_leading_zeroes_u64(new_word_count)) + 1;
        newarray               = (uint64_t*)MU_REALLOC(bs->array, sizeof(uint64_t) * new_word_cap);
        bs->word_capacity      = new_word_cap;
        bs->array              = newarray;
    }


    /* Zero new region if expanding */
    if(padwithzeroes && new_word_count > bs->word_count)
    {
        size_t delta = new_word_count - bs->word_count;
        MU_MEMSET(bs->array + bs->word_count, 0, delta * sizeof(uint64_t));
    }


    bs->word_count = new_word_count;
    return true;
}

bool mu_bitset_resize_words(mu_bitset* bs, size_t new_word_count)
{
    return mu_bitset_resize_impl(bs, new_word_count, true);
}

/*
Map a bit index to storage location:
  word index = bit / 64
  bit offset = bit % 64
*/
void mu_bitset_set(mu_bitset* bs, size_t bit_index)
{
    size_t word_index = bit_index / 64;
    size_t bit_offset = bit_index % 64;

    if(word_index >= bs->word_count)
        mu_bitset_resize_impl(bs, word_index + 1, true);

    bs->array[word_index] |= (UINT64_C(1) << bit_offset);
}

void mu_bitset_reset(mu_bitset* bs, size_t bit_index)
{
    size_t word_index = bit_index / 64;
    size_t bit_offset = bit_index % 64;

    if(word_index >= bs->word_count)
        return;

    bs->array[word_index] &= ~(UINT64_C(1) << bit_offset);
}

bool mu_bitset_test(const mu_bitset* bs, size_t bit_index)
{
    size_t word_index = bit_index / 64;
    size_t bit_offset = bit_index % 64;

    if(word_index >= bs->word_count)
        return false;

    return (bs->array[word_index] & (UINT64_C(1) << bit_offset)) != 0;
}

void mu_bitset_enable_bit(mu_bitset* bs, size_t bit_index)
{
    mu_bitset_set(bs, bit_index);
}

void mu_bitset_disable_bit(mu_bitset* bs, size_t bit_index)
{
    mu_bitset_reset(bs, bit_index);
}

void mu_bitset_set_bit(mu_bitset* bs, size_t bit_index, bool value)
{
    if(value)
    {
        mu_bitset_set(bs, bit_index);
    }
    else
    {
        mu_bitset_reset(bs, bit_index);
    }
}

bool mu_bitset_get_bit(const mu_bitset* bs, size_t bit_index)
{
    return mu_bitset_test(bs, bit_index);
}
/*
Goal:
Shift the entire bitset left by shift bits.
Equivalent to:

bitset *= 2^shift

Think of the bitset as one giant binary number spread across 64-bit chunks.

Memory layout (little-endian words):

array[0]  = lowest  64 bits
array[1]  = next    64 bits
...
array[n-1]= highest 64 bits
*/

void mu_bitset_shift_left(mu_bitset* bs, size_t shift)
{
    if(shift == 0 || bs->word_count == 0)
        return;

    size_t word_shift = shift / 64;  // whole 64-bit blocks
    size_t bit_shift  = shift % 64;  // remaining bits
    size_t old_count  = bs->word_count;

    /* Ensure enough space */
    size_t new_count = old_count + word_shift + (bit_shift ? 1 : 0);
    mu_bitset_resize_impl(bs, new_count, true);  // this zeroes new region

    uint64_t* a = bs->array;

    /*
        We move from high → low to avoid overwriting data
        we still need.

        VISUAL EXAMPLE (shift = 70):

            word_shift = 1
            bit_shift  = 6

        Before:
            [ w3 | w2 | w1 | w0 ]

        After:
            [  0 | new3 | new2 | new1 | new0 ]

        where:
            new_i = (old_i << 6) | (old_(i-1) >> 58)
    */

    if(bit_shift == 0)
    {
        /* Pure word shift (easy case) */

        for(size_t i = old_count; i > 0; --i)
            a[i - 1 + word_shift] = a[i - 1];
    }
    else
    {
        /* Word + bit shift */

        /* Highest word: only left shift, no carry-in */
        a[old_count - 1 + word_shift + 1] = a[old_count - 1] >> (64 - bit_shift);

        for(size_t i = old_count - 1; i > 0; --i)
        {
            a[i + word_shift] = (a[i] << bit_shift) | (a[i - 1] >> (64 - bit_shift));
        }

        /* Lowest word: only left shift */
        a[word_shift] = a[0] << bit_shift;
    }

    /* Zero-fill newly created lowest words */
    for(size_t i = 0; i < word_shift; ++i)
        a[i] = 0;
}
void mu_bitset_shift_right(mu_bitset* bs, size_t shift)
{
    if(shift == 0 || bs->word_count == 0)
        return;

    size_t word_shift = shift / 64;  // whole-word shift
    size_t bit_shift  = shift % 64;  // remaining bit shift
    size_t old_count  = bs->word_count;

    if(word_shift >= old_count)
    {
        /* Everything shifts out */
        mu_bitset_resize_impl(bs, 0, false);
        return;
    }

    uint64_t* a = bs->array;

    /*
        VISUAL EXAMPLE (shift = 70):

            word_shift = 1
            bit_shift  = 6

        Before:
            [ w3 | w2 | w1 | w0 ]

        After:
            [ new2 | new1 | new0 ]

        where:
            new_i = (old_(i+1) << (64-6)) | (old_i >> 6)
    */

    if(bit_shift == 0)
    {
        /* Pure word shift */

        for(size_t i = 0; i < old_count - word_shift; ++i)
            a[i] = a[i + word_shift];
    }
    else
    {
        /* Word + bit shift */

        for(size_t i = 0; i + word_shift + 1 < old_count; ++i)
        {
            a[i] = (a[i + word_shift] >> bit_shift) | (a[i + word_shift + 1] << (64 - bit_shift));
        }

        /* Highest remaining word: no carry-in */
        a[old_count - word_shift - 1] = a[old_count - 1] >> bit_shift;
    }

    /* Logical shrink */
    mu_bitset_resize_impl(bs, old_count - word_shift, false);
}


bool mu_bitset_grow(mu_bitset* bs, size_t new_word_count)
{
    if(new_word_count < bs->word_count)
    {
        return false;
    }
    if(new_word_count > SIZE_MAX / 64)
    {
        return false;
    }
    if(bs->word_capacity < new_word_count)
    {
        uint64_t* newarray;
        size_t    newcapacity = bs->word_capacity;
        if(newcapacity == 0)
        {
            newcapacity = 1;
        }
        while(newcapacity < new_word_count)
        {
            newcapacity *= 2;
        }
        if((newarray = (uint64_t*)MU_REALLOC(bs->array, sizeof(uint64_t) * newcapacity)) == NULL)
        {
            return false;
        }
        bs->word_capacity = newcapacity;
        bs->array         = newarray;
    }
    MU_MEMSET(bs->array + bs->word_count, 0, sizeof(uint64_t) * (new_word_count - bs->word_count));
    bs->word_count = new_word_count;
    return true;
}

size_t mu_bitset_count(const mu_bitset* bs)
{
    return mu_bitset__popcnt_words(bs->array, bs->word_count);
}

bool mu_bitset_inplace_union(mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    for(size_t k = 0; k < minlength; ++k)
    {
        b1->array[k] |= b2->array[k];
    }
    if(b2->word_count > b1->word_count)
    {
        size_t oldsize = b1->word_count;
        if(!mu_bitset_resize_words(b1, b2->word_count))
            return false;
        MU_MEMCPY(b1->array + oldsize, b2->array + oldsize, (b2->word_count - oldsize) * sizeof(uint64_t));
    }
    return true;
}

bool mu_bitset_empty(const mu_bitset* bs)
{
    size_t k = 0;
#if defined(__AVX2__)
    for (; k + 4u <= bs->word_count; k += 4u)
    {
        __m256i v = _mm256_loadu_si256((const __m256i*)(bs->array + k));
        if (!_mm256_testz_si256(v, v))
            return false;
    }
#endif
    for(; k < bs->word_count; k++)
    {
        if(bs->array[k] != 0)
        {
            return false;
        }
    }
    return true;
}

size_t mu_bitset_minimum(const mu_bitset* bs)
{
    for(size_t k = 0; k < bs->word_count; k++)
    {
        uint64_t w = bs->array[k];
        if(w != 0)
        {
            return mu_trailing_zeroes_u64(w) + k * 64;
        }
    }
    return SIZE_MAX;
}

size_t mu_bitset_maximum(const mu_bitset* bs)
{
    for(size_t k = bs->word_count; k > 0; k--)
    {
        uint64_t w = bs->array[k - 1];
        if(w != 0)
        {
            return 63 - mu_leading_zeroes_u64(w) + (k - 1) * 64;
        }
    }
    return 0;
}

/* Returns true if bitsets share no common elements, false otherwise.
 *
 * Performs early-out if common element found. */
bool mu_bitsets_disjoint(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t k         = 0;

#if defined(__AVX2__)
    for (; k + 4u <= minlength; k += 4u)
    {
        __m256i va = _mm256_loadu_si256((const __m256i*)(b1->array + k));
        __m256i vb = _mm256_loadu_si256((const __m256i*)(b2->array + k));
        if (!_mm256_testz_si256(va, vb))
            return false;
    }
#endif
    for(; k < minlength; k++)
    {
        if((b1->array[k] & b2->array[k]) != 0)
            return false;
    }
    return true;
}

/* Returns true if bitsets contain at least 1 common element, false if they are
 * disjoint.
 *
 * Performs early-out if common element found. */
bool mu_bitsets_intersect(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    return !mu_bitsets_disjoint(b1, b2);
}

/* Returns true if b has any bits set in or after b->array[starting_loc]. */
static bool mu_bitset_any_bits_set(const mu_bitset* b, size_t starting_loc)
{
    if(starting_loc >= b->word_count)
    {
        return false;
    }
    for(size_t k = starting_loc; k < b->word_count; k++)
    {
        if(b->array[k] != 0)
            return true;
    }
    return false;
}

bool mu_bitset_equal(const mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b)
{
    size_t min_size = a->word_count < b->word_count ? a->word_count : b->word_count;

    for(size_t k = 0; k < min_size; ++k)
    {
        if(a->array[k] != b->array[k])
        {
            return false;
        }
    }

    if(a->word_count > b->word_count)
    {
        return !mu_bitset_any_bits_set(a, b->word_count);
    }

    return !mu_bitset_any_bits_set(b, a->word_count);
}

/* Returns true if b1 has all of b2's bits set.
 *
 * Performs early out if a bit is found in b2 that is not found in b1. */
bool mu_bitset_contains_all(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t min_size = b1->word_count;
    if(b1->word_count > b2->word_count)
    {
        min_size = b2->word_count;
    }
    for(size_t k = 0; k < min_size; k++)
    {
        if((b1->array[k] & b2->array[k]) != b2->array[k])
        {
            return false;
        }
    }
    if(b2->word_count > b1->word_count)
    {
        /* Need to check if b2 has any bits set beyond b1's array */
        return !mu_bitset_any_bits_set(b2, b1->word_count);
    }
    return true;
}

size_t mu_bitset_union_count(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t answer    = mu_bitset__popcnt_binop(b1->array, b2->array, minlength, MU_BITSET_OP_OR);
    if (b2->word_count > b1->word_count)
        answer += mu_bitset__popcnt_words(b2->array + minlength, b2->word_count - minlength);
    else if (b1->word_count > b2->word_count)
        answer += mu_bitset__popcnt_words(b1->array + minlength, b1->word_count - minlength);
    return answer;
}

void mu_bitset_inplace_intersection(mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t k         = 0;
    for(; k < minlength; ++k)
    {
        b1->array[k] &= b2->array[k];
    }
    for(; k < b1->word_count; ++k)
    {
        b1->array[k] = 0;
    }
}

size_t mu_bitset_intersection_count(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    return mu_bitset__popcnt_binop(b1->array, b2->array, minlength, MU_BITSET_OP_AND);
}

void mu_bitset_inplace_difference(mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t k         = 0;
    for(; k < minlength; ++k)
    {
        b1->array[k] &= ~(b2->array[k]);
    }
}

size_t mu_bitset_difference_count(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t answer    = mu_bitset__popcnt_binop(b1->array, b2->array, minlength, MU_BITSET_OP_ANDNOT);
    if (b1->word_count > minlength)
        answer += mu_bitset__popcnt_words(b1->array + minlength, b1->word_count - minlength);
    return answer;
}

bool mu_bitset_inplace_symmetric_difference(mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t k         = 0;
    for(; k < minlength; ++k)
    {
        b1->array[k] ^= b2->array[k];
    }
    if(b2->word_count > b1->word_count)
    {
        size_t oldsize = b1->word_count;
        if(!mu_bitset_resize_words(b1, b2->word_count))
            return false;
        MU_MEMCPY(b1->array + oldsize, b2->array + oldsize, (b2->word_count - oldsize) * sizeof(uint64_t));
    }
    return true;
}

size_t mu_bitset_symmetric_difference_count(const mu_bitset* MU_RESTRICT b1, const mu_bitset* MU_RESTRICT b2)
{
    size_t minlength = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t answer    = mu_bitset__popcnt_binop(b1->array, b2->array, minlength, MU_BITSET_OP_XOR);
    if (b2->word_count > b1->word_count)
        answer += mu_bitset__popcnt_words(b2->array + minlength, b2->word_count - minlength);
    else if (b1->word_count > b2->word_count)
        answer += mu_bitset__popcnt_words(b1->array + minlength, b1->word_count - minlength);
    return answer;
}

bool mu_bitset_xor(mu_bitset* MU_RESTRICT out, const mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b)
{
    size_t min_size = a->word_count < b->word_count ? a->word_count : b->word_count;
    size_t max_size = a->word_count > b->word_count ? a->word_count : b->word_count;

    if(!mu_bitset_resize_words(out, max_size))
    {
        return false;
    }

    size_t k = 0;
    for(; k < min_size; ++k)
    {
        out->array[k] = a->array[k] ^ b->array[k];
    }

    if(a->word_count > b->word_count)
    {
        for(; k < max_size; ++k)
        {
            out->array[k] = a->array[k];
        }
    }
    else
    {
        for(; k < max_size; ++k)
        {
            out->array[k] = b->array[k];
        }
    }
    return true;
}

bool mu_bitset_or(mu_bitset* MU_RESTRICT out, const mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b)
{
    size_t min_size = a->word_count < b->word_count ? a->word_count : b->word_count;
    size_t max_size = a->word_count > b->word_count ? a->word_count : b->word_count;

    if(!mu_bitset_resize_words(out, max_size))
    {
        return false;
    }

    size_t k = 0;
    for(; k < min_size; ++k)
    {
        out->array[k] = a->array[k] | b->array[k];
    }

    if(a->word_count > b->word_count)
    {
        for(; k < max_size; ++k)
        {
            out->array[k] = a->array[k];
        }
    }
    else
    {
        for(; k < max_size; ++k)
        {
            out->array[k] = b->array[k];
        }
    }
    return true;
}

bool mu_bitset_and(mu_bitset* MU_RESTRICT out, const mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b)
{
    size_t min_size = a->word_count < b->word_count ? a->word_count : b->word_count;
    size_t max_size = a->word_count > b->word_count ? a->word_count : b->word_count;

    if(!mu_bitset_resize_words(out, max_size))
    {
        return false;
    }

    size_t k = 0;
    for(; k < min_size; ++k)
    {
        out->array[k] = a->array[k] & b->array[k];
    }

    for(; k < max_size; ++k)
    {
        out->array[k] = 0;
    }
    return true;
}

bool mu_bitset_not(mu_bitset* MU_RESTRICT out, const mu_bitset* MU_RESTRICT a)
{
    if(!mu_bitset_resize_words(out, a->word_count))
    {
        return false;
    }

    for(size_t k = 0; k < a->word_count; ++k)
    {
        out->array[k] = ~a->array[k];
    }
    return true;
}

bool mu_bitset_xor_assign(mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b)
{
    return mu_bitset_inplace_symmetric_difference(a, b);
}

bool mu_bitset_or_assign(mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b)
{
    return mu_bitset_inplace_union(a, b);
}

bool mu_bitset_and_assign(mu_bitset* MU_RESTRICT a, const mu_bitset* MU_RESTRICT b)
{
    mu_bitset_inplace_intersection(a, b);
    return true;
}

void mu_bitset_traverse(const mu_bitset* bs, mu_bitset_visit_fn visitor, void* user)
{
    size_t i = 0;
    while(mu_bitset_next_set_bit(bs, &i))
    {
        if(!visitor(i, user))
        {
            return;
        }
        ++i;
    }
}

void mu_bitset_traverse_range(const mu_bitset* bs, mu_bitset_visit_fn visitor, void* user, size_t begin, size_t count)
{
    if(count == 0)
    {
        return;
    }

    size_t end = begin + count;
    if(end < begin)
    {
        end = SIZE_MAX;
    }

    size_t i = begin;
    while(mu_bitset_next_set_bit(bs, &i) && i < end)
    {
        if(!visitor(i, user))
        {
            return;
        }
        ++i;
    }
}

bool mu_bitset_trim(mu_bitset* bs)
{
    size_t newsize = bs->word_count;
    while(newsize > 0)
    {
        if(bs->array[newsize - 1] == 0)
            newsize -= 1;
        else
            break;
    }
    if(bs->word_capacity == newsize)
        return true;

    uint64_t* newarray;
    if((newarray = (uint64_t*)MU_REALLOC(bs->array, sizeof(uint64_t) * newsize)) == NULL)
    {
        return false;
    }
    bs->array         = newarray;
    bs->word_capacity = newsize;
    bs->word_count    = newsize;
    return true;
}

void mu_bitset_print(const mu_bitset* b)
{
    printf("{");
    for(size_t i = 0; mu_bitset_next_set_bit(b, &i); i++)
    {
        printf("%zu, ", i);
    }
    printf("}");
}

