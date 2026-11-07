#pragma once
#ifndef MU_COMMON_H
#define MU_COMMON_H

/*
 * mu_common.h — numeric and bit helpers for the core mu/ family.
 *
 * All macros (MU_INLINE, MU_ASSERT, MU_MALLOC, MU_RESTRICT, ...) now live in
 * the single shared layer, ../mu_macros.h. This header only provides the
 * count-leading/trailing-zeroes and popcount intrinsics, plus <stdio.h> for
 * historical convenience (mu_perf uses printf).
 */

#include "mu_macros.h"
#include <stdio.h>

MU_BEGIN_EXTERN_C

/* --------------------------------------------------------------------------
 * 64-bit bit intrinsics.
 *   - trailing_zeroes / leading_zeroes return 64 for input 0.
 *   - popcount returns [0, 64].
 * -------------------------------------------------------------------------- */

#if defined(_MSC_VER) && !defined(__clang__)

#include <intrin.h>

MU_INLINE uint32_t mu_trailing_zeroes_u64(uint64_t x)
{
    unsigned long idx;
    if (x == 0)
        return 64u;

#if defined(_WIN64)
    _BitScanForward64(&idx, x);
    return (uint32_t)idx;
#else
    if ((uint32_t)x != 0)
    {
        _BitScanForward(&idx, (uint32_t)x);
        return (uint32_t)idx;
    }
    _BitScanForward(&idx, (uint32_t)(x >> 32));
    return (uint32_t)(idx + 32);
#endif
}

MU_INLINE uint32_t mu_leading_zeroes_u64(uint64_t x)
{
    unsigned long idx;
    if (x == 0)
        return 64u;

#if defined(_WIN64)
    _BitScanReverse64(&idx, x);
    return 63u - (uint32_t)idx;
#else
    if ((x >> 32) != 0)
    {
        _BitScanReverse(&idx, (uint32_t)(x >> 32));
        return 31u - (uint32_t)idx;
    }
    _BitScanReverse(&idx, (uint32_t)x);
    return 63u - (uint32_t)idx;
#endif
}

MU_INLINE uint32_t mu_popcount_u64(uint64_t x)
{
#if defined(_WIN64)
    return (uint32_t)__popcnt64(x);
#else
    return (uint32_t)(__popcnt((uint32_t)x) + __popcnt((uint32_t)(x >> 32)));
#endif
}

MU_INLINE uint32_t mu_trailing_zeroes_u32(uint32_t x)
{
    unsigned long idx;
    if (x == 0u)
        return 32u;
    _BitScanForward(&idx, x);
    return (uint32_t)idx;
}

MU_INLINE uint32_t mu_leading_zeroes_u32(uint32_t x)
{
    unsigned long idx;
    if (x == 0u)
        return 32u;
    _BitScanReverse(&idx, x);
    return 31u - (uint32_t)idx;
}

MU_INLINE uint32_t mu_popcount_u32(uint32_t x)
{
    return (uint32_t)__popcnt(x);
}

#else /* GCC / Clang / anything with builtins */

/* count trailing zero bits; returns 64 for x == 0 (branchless) */
MU_INLINE uint32_t mu_trailing_zeroes_u64(uint64_t x)
{
    return (uint32_t)__builtin_ctzll(x | (uint64_t)(x == 0))
         + ((uint32_t)(x == 0) * 64u);
}

/* count leading zero bits; returns 64 for x == 0 (branchless) */
MU_INLINE uint32_t mu_leading_zeroes_u64(uint64_t x)
{
    return (uint32_t)__builtin_clzll(x | (uint64_t)(x == 0))
         + (uint32_t)(x == 0);
}

MU_INLINE uint32_t mu_popcount_u64(uint64_t x)
{
    return (uint32_t)__builtin_popcountll(x);
}

MU_INLINE uint32_t mu_trailing_zeroes_u32(uint32_t x)
{
    return (uint32_t)__builtin_ctz(x | (uint32_t)(x == 0u))
         + ((uint32_t)(x == 0u) * 32u);
}

MU_INLINE uint32_t mu_leading_zeroes_u32(uint32_t x)
{
    return (uint32_t)__builtin_clz(x | (uint32_t)(x == 0u))
         + (uint32_t)(x == 0u);
}

MU_INLINE uint32_t mu_popcount_u32(uint32_t x)
{
    return (uint32_t)__builtin_popcount(x);
}

#endif

MU_END_EXTERN_C

#endif /* MU_COMMON_H */
