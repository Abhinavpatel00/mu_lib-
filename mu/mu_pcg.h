
#pragma  once 

#include "mu_common.h"
typedef struct
{
    uint64_t state;
    uint64_t inc;
} mu_pcg32;

/* ------------------ Scalar ------------------ */

MU_INLINE void mu_pcg32_init(mu_pcg32* rng, uint64_t seed, uint64_t seq)
{
    rng->state = 0u;
    rng->inc   = (seq << 1u) | 1u;
    rng->state = rng->state * 6364136223846793005ULL + rng->inc;
    rng->state += seed;
    rng->state = rng->state * 6364136223846793005ULL + rng->inc;
}

MU_INLINE uint32_t mu_pcg32_next_u32(mu_pcg32* rng)
{
    uint64_t oldstate = rng->state;
    rng->state        = oldstate * 6364136223846793005ULL + rng->inc;

    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot        = (uint32_t)(oldstate >> 59u);

    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}


/* ------------------ SIMD Batch (AVX2) ------------------ */
//
// #if defined(__AVX2__)
// #include <immintrin.h>
//
// typedef struct
// {
//     __m256i state;
//     __m256i inc;
// } mu_pcg32x4;
//
// /* initialize 4 parallel streams */
// MU_INLINE void mu_pcg32x4_init(mu_pcg32x4* rng,
//                                    uint64_t      seed0,
//                                    uint64_t      seed1,
//                                    uint64_t      seed2,
//                                    uint64_t      seed3,
//                                    uint64_t      seq0,
//                                    uint64_t      seq1,
//                                    uint64_t      seq2,
//                                    uint64_t      seq3)
// {
//     __m256i seeds = _mm256_set_epi64x(seed3, seed2, seed1, seed0);
//     __m256i seqs  = _mm256_set_epi64x(seq3, seq2, seq1, seq0);
//
//     rng->state = _mm256_setzero_si256();
//     rng->inc   = _mm256_or_si256(_mm256_slli_epi64(seqs, 1), _mm256_set1_epi64x(1));
//
//     __m256i mul = _mm256_set1_epi64x(6364136223846793005ULL);
//
//     rng->state = _mm256_add_epi64(_mm256_mullo_epi64(rng->state, mul), rng->inc);
//
//     rng->state = _mm256_add_epi64(rng->state, seeds);
//
//     rng->state = _mm256_add_epi64(_mm256_mullo_epi64(rng->state, mul), rng->inc);
// }
//
// MU_INLINE __m256i mu_pcg32x4_next_u32(mu_pcg32x4* rng)
// {
//     __m256i oldstate = rng->state;
//     __m256i mul      = _mm256_set1_epi64x(6364136223846793005ULL);
//
//     rng->state = _mm256_add_epi64(_mm256_mullo_epi64(oldstate, mul), rng->inc);
//
//     __m256i xorshifted = _mm256_srli_epi64(_mm256_xor_si256(_mm256_srli_epi64(oldstate, 18), oldstate), 27);
//
//     __m256i rot = _mm256_srli_epi64(oldstate, 59);
//
//     __m256i xs32  = _mm256_cvtepi64_epi32(xorshifted);
//     __m256i rot32 = _mm256_cvtepi64_epi32(rot);
//
//     __m256i r1 = _mm256_srlv_epi32(xs32, rot32);
//     __m256i r2 =
//         _mm256_sllv_epi32(xs32, _mm256_and_si256(_mm256_sub_epi32(_mm256_set1_epi32(32), rot32), _mm256_set1_epi32(31)));
//
//     return _mm256_or_si256(r1, r2);
// }
//
//#endif

