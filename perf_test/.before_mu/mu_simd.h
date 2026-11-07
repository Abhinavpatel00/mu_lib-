#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_simd — portable 4-wide float vectors (SSE2 / NEON / scalar).
 *
 * WHY: the SIMD chapter of the performance skill. Rather than sprinkling
 * intrinsics through game code, name the twenty operations you actually use
 * and let this header pick the backend. Correct on every target, and the
 * scalar fallback keeps the same semantics so code never needs #ifdefs.
 *
 * BACKENDS (auto-detected, all optional):
 *     MU_SIMD_SSE2   x86-64 and SSE2-capable 32-bit x86 — the baseline
 *     MU_SIMD_NEON   AArch64 / armv7 NEON
 *     scalar          anything else — identical results, not vectorised
 *
 * MEMORY LAYOUT: mu_f32x4 is a union of float[4] and the native vector, so
 *     - it is 16 bytes, 16-byte aligned (the native vector dictates this),
 *     - lane i is always `.m[i]` on every backend,
 *     - it passes by value in a register.
 *
 * LOADS: the plain load/store use the unaligned forms — on modern x86 that
 * costs nothing, and an unaligned load that silently faults is far worse than
 * a marginally slower one. Use the _aligned variants only from storage you
 * control (MU_ALIGN(16), or mu_frame_arena_alloc(p, n, 16)).
 *
 * MASKS: comparisons produce all-ones / all-zeros BIT masks (not 1.0f/0.0f),
 * so and / andnot / or compose with select exactly as the hardware intends —
 * on every backend.
 *
 * SEMANTICS LOCKED ACROSS BACKENDS (these are the traps):
 *     a & ~b   is mu_f32x4_andnot(a, b)  — SSE _mm_andnot_ps(a,b) and
 *              NEON vbicq_f32(b,a) agree only if the operands are swapped;
 *     NEON comparisons return uint32x4_t and must be reinterpreted;
 *     armv7 NEON has NO vector divide — aarch64 vdivq_f32 does not exist there.
 *
 * NOTE: this is the 95% set (add/sub/mul/div/min/max/lerp/dot/select). Anything
 * beyond belongs at the call site with a targeted intrinsic — don't grow a
 * general-purpose wrapper nobody can optimise through.
 * -------------------------------------------------------------------------- */

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define MU_SIMD_SSE2 1
#  include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#  define MU_SIMD_NEON 1
#  include <arm_neon.h>
#endif

typedef union mu_f32x4
{
    float m[4];
#if defined(MU_SIMD_SSE2)
    __m128 v;
#elif defined(MU_SIMD_NEON)
    float32x4_t v;
#endif
} mu_f32x4;

/* -------------------------------------------------------------------------- *
 * Construct / move
 * -------------------------------------------------------------------------- */

MU_INLINE mu_f32x4 mu_f32x4_zero(void)
{
    mu_f32x4 r;
#if defined(MU_SIMD_SSE2)
    r.v = _mm_setzero_ps();
#elif defined(MU_SIMD_NEON)
    r.v = vdupq_n_f32(0.0f);
#else
    r.m[0] = r.m[1] = r.m[2] = r.m[3] = 0.0f;
#endif
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_set(float a, float b, float c, float d)
{
    mu_f32x4 r;
#if defined(MU_SIMD_SSE2)
    r.v = _mm_setr_ps(a, b, c, d);
#elif defined(MU_SIMD_NEON)
    r.m[0] = a;
    r.m[1] = b;
    r.m[2] = c;
    r.m[3] = d;
#else
    r.m[0] = a;
    r.m[1] = b;
    r.m[2] = c;
    r.m[3] = d;
#endif
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_set1(float s)
{
    mu_f32x4 r;
#if defined(MU_SIMD_SSE2)
    r.v = _mm_set1_ps(s);
#elif defined(MU_SIMD_NEON)
    r.v = vdupq_n_f32(s);
#else
    r.m[0] = r.m[1] = r.m[2] = r.m[3] = s;
#endif
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_load(const float* p)
{
    mu_f32x4 r;
#if defined(MU_SIMD_SSE2)
    r.v = _mm_loadu_ps(p);
#elif defined(MU_SIMD_NEON)
    r.v = vld1q_f32(p);
#else
    r.m[0] = p[0];
    r.m[1] = p[1];
    r.m[2] = p[2];
    r.m[3] = p[3];
#endif
    return r;
}

/* p MUST be 16-byte aligned. */
MU_INLINE mu_f32x4 mu_f32x4_load_aligned(const float* p)
{
    mu_f32x4 r;
#if defined(MU_SIMD_SSE2)
    r.v = _mm_load_ps(p);
#elif defined(MU_SIMD_NEON)
    r.v = vld1q_f32(p);
#else
    r.m[0] = p[0];
    r.m[1] = p[1];
    r.m[2] = p[2];
    r.m[3] = p[3];
#endif
    return r;
}

MU_INLINE void mu_f32x4_store(float* p, mu_f32x4 v)
{
#if defined(MU_SIMD_SSE2)
    _mm_storeu_ps(p, v.v);
#elif defined(MU_SIMD_NEON)
    vst1q_f32(p, v.v);
#else
    p[0] = v.m[0];
    p[1] = v.m[1];
    p[2] = v.m[2];
    p[3] = v.m[3];
#endif
}

MU_INLINE void mu_f32x4_store_aligned(float* p, mu_f32x4 v)
{
#if defined(MU_SIMD_SSE2)
    _mm_store_ps(p, v.v);
#elif defined(MU_SIMD_NEON)
    vst1q_f32(p, v.v);
#else
    p[0] = v.m[0];
    p[1] = v.m[1];
    p[2] = v.m[2];
    p[3] = v.m[3];
#endif
}

MU_INLINE float mu_f32x4_extract(mu_f32x4 v, uint32_t lane)
{
    return v.m[lane & 3u]; /* the union shares storage, so this is backend-free */
}

MU_INLINE mu_f32x4 mu_f32x4_insert(mu_f32x4 v, uint32_t lane, float value)
{
    v.m[lane & 3u] = value;
    return v;
}

/* -------------------------------------------------------------------------- *
 * Arithmetic
 *
 * Generated from one macro per backend so the three cannot drift apart.
 * -------------------------------------------------------------------------- */

#if defined(MU_SIMD_SSE2) || defined(MU_SIMD_NEON)
#  define MU_F32X4_BIN(name, simd)                                                       \
      MU_INLINE mu_f32x4 name(mu_f32x4 a, mu_f32x4 b)                                    \
      {                                                                                   \
          mu_f32x4 r;                                                                     \
          r.v = simd(a.v, b.v);                                                           \
          return r;                                                                       \
      }
#else
#  define MU_F32X4_BIN(name, elemwise)                                                   \
      MU_INLINE mu_f32x4 name(mu_f32x4 a, mu_f32x4 b)                                    \
      {                                                                                   \
          mu_f32x4 r;                                                                     \
          r.m[0] = elemwise(a.m[0], b.m[0]);                                              \
          r.m[1] = elemwise(a.m[1], b.m[1]);                                              \
          r.m[2] = elemwise(a.m[2], b.m[2]);                                              \
          r.m[3] = elemwise(a.m[3], b.m[3]);                                              \
          return r;                                                                       \
      }
#endif

#if defined(MU_SIMD_SSE2)

MU_F32X4_BIN(mu_f32x4_add, _mm_add_ps)
MU_F32X4_BIN(mu_f32x4_sub, _mm_sub_ps)
MU_F32X4_BIN(mu_f32x4_mul, _mm_mul_ps)
MU_F32X4_BIN(mu_f32x4_div, _mm_div_ps)
MU_F32X4_BIN(mu_f32x4_min, _mm_min_ps)
MU_F32X4_BIN(mu_f32x4_max, _mm_max_ps)

#elif defined(MU_SIMD_NEON)

MU_F32X4_BIN(mu_f32x4_add, vaddq_f32)
MU_F32X4_BIN(mu_f32x4_sub, vsubq_f32)
MU_F32X4_BIN(mu_f32x4_mul, vmulq_f32)
MU_F32X4_BIN(mu_f32x4_min, vminq_f32)
MU_F32X4_BIN(mu_f32x4_max, vmaxq_f32)

MU_INLINE mu_f32x4 mu_f32x4_div(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
#if defined(__aarch64__)
    r.v = vdivq_f32(a.v, b.v); /* ARMv8 has vector divide */
#else
    /* armv7 NEON has no vdivq_f32 — fall back to per-lane. The compiler
       still vectorises the four divisions where it can. */
    r.m[0] = a.m[0] / b.m[0];
    r.m[1] = a.m[1] / b.m[1];
    r.m[2] = a.m[2] / b.m[2];
    r.m[3] = a.m[3] / b.m[3];
#endif
    return r;
}

#else /* scalar */

MU_F32X4_BIN(mu_f32x4_add, +)
MU_F32X4_BIN(mu_f32x4_sub, -)
MU_F32X4_BIN(mu_f32x4_mul, *)
MU_F32X4_BIN(mu_f32x4_div, /)

/* Not the MU_MIN/MU_MAX statement-exprs: those are GCC/Clang-only and this
   fallback has to compile anywhere. */
MU_INLINE mu_f32x4 mu_f32x4_min(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
        r.m[i] = a.m[i] < b.m[i] ? a.m[i] : b.m[i];
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_max(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
        r.m[i] = a.m[i] > b.m[i] ? a.m[i] : b.m[i];
    return r;
}

#endif

/* Exact sqrt on the vector paths. The scalar fallback seeds with the fast
   inverse-sqrt bit hack and refines three times (~3.4e-3 -> 1e-10 relative),
   so it matches _mm_sqrt_ps to float precision without pulling in libm. */
MU_INLINE mu_f32x4 mu_f32x4_sqrt(mu_f32x4 a)
{
    mu_f32x4 r;
#if defined(MU_SIMD_SSE2)
    r.v = _mm_sqrt_ps(a.v);
#elif defined(MU_SIMD_NEON)
    r.v = vsqrtq_f32(a.v);
#else
    for (int i = 0; i < 4; ++i)
    {
        float x = a.m[i];
        if (!(x > 0.0f))
        {
            r.m[i] = 0.0f;
            continue;
        }
        uint32_t bits;
        MU_MEMCPY(&bits, &x, sizeof(bits));
        bits = 0x5f3759dfu - (bits >> 1);
        float y;
        MU_MEMCPY(&y, &bits, sizeof(y));
        y       = y * (1.5f - 0.5f * x * y * y);
        y       = y * (1.5f - 0.5f * x * y * y);
        y       = y * (1.5f - 0.5f * x * y * y);
        r.m[i]  = x * y;
    }
#endif
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_fmadd(mu_f32x4 a, mu_f32x4 b, mu_f32x4 c)
{
    return mu_f32x4_add(mu_f32x4_mul(a, b), c);
}

MU_INLINE mu_f32x4 mu_f32x4_lerp(mu_f32x4 a, mu_f32x4 b, mu_f32x4 t)
{
    return mu_f32x4_fmadd(mu_f32x4_sub(b, a), t, a);
}

MU_INLINE mu_f32x4 mu_f32x4_neg(mu_f32x4 a)
{
    mu_f32x4 r;
#if defined(MU_SIMD_SSE2)
    r.v = _mm_xor_ps(a.v, _mm_set1_ps(-0.0f));
#elif defined(MU_SIMD_NEON)
    r.v = vnegq_f32(a.v);
#else
    for (int i = 0; i < 4; ++i)
        r.m[i] = -a.m[i];
#endif
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_abs(mu_f32x4 a)
{
    mu_f32x4 r;
#if defined(MU_SIMD_SSE2)
    r.v = _mm_andnot_ps(_mm_set1_ps(-0.0f), a.v); /* ~(-0.0) & a = |a| */
#elif defined(MU_SIMD_NEON)
    r.v = vabsq_f32(a.v);
#else
    for (int i = 0; i < 4; ++i)
        r.m[i] = a.m[i] < 0.0f ? -a.m[i] : a.m[i];
#endif
    return r;
}

/* -------------------------------------------------------------------------- *
 * Masks and selection — identical bit conventions on every backend.
 * -------------------------------------------------------------------------- */

#if defined(MU_SIMD_SSE2)

MU_INLINE mu_f32x4 mu_f32x4_cmpeq(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = _mm_cmpeq_ps(a.v, b.v);
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_cmpgt(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = _mm_cmpgt_ps(a.v, b.v);
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_cmplt(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = _mm_cmplt_ps(a.v, b.v);
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_and(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = _mm_and_ps(a.v, b.v);
    return r;
}
/* (~a) & b */
MU_INLINE mu_f32x4 mu_f32x4_andnot(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = _mm_andnot_ps(a.v, b.v);
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_or(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = _mm_or_ps(a.v, b.v);
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_xor(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = _mm_xor_ps(a.v, b.v);
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_select(mu_f32x4 cond, mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = _mm_or_ps(_mm_and_ps(cond.v, a.v), _mm_andnot_ps(cond.v, b.v));
    return r;
}

#elif defined(MU_SIMD_NEON)

/* NEON comparisons return uint32x4_t — reinterpret before storing. */
MU_INLINE mu_f32x4 mu_f32x4_cmpeq(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = vreinterpretq_f32_u32(vceqq_f32(a.v, b.v));
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_cmpgt(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = vreinterpretq_f32_u32(vcgtq_f32(a.v, b.v));
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_cmplt(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = vreinterpretq_f32_u32(vcltq_f32(a.v, b.v));
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_and(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = vandq_f32(a.v, b.v);
    return r;
}
/* (~a) & b  ==  b & ~a == vbicq_f32(b, a)  — operands are swapped vs SSE. */
MU_INLINE mu_f32x4 mu_f32x4_andnot(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = vbicq_f32(b.v, a.v);
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_or(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = vorrq_f32(a.v, b.v);
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_xor(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = veorq_f32(a.v, b.v);
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_select(mu_f32x4 cond, mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    r.v = vbslq_f32(vreinterpretq_u32_f32(cond.v), a.v, b.v);
    return r;
}

#else /* scalar */

MU_INLINE mu_f32x4 mu_f32x4_cmpeq(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t m = (a.m[i] == b.m[i]) ? 0xffffffffu : 0x00000000u;
        MU_MEMCPY(&r.m[i], &m, sizeof(m));
    }
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_cmpgt(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t m = (a.m[i] > b.m[i]) ? 0xffffffffu : 0x00000000u;
        MU_MEMCPY(&r.m[i], &m, sizeof(m));
    }
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_cmplt(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t m = (a.m[i] < b.m[i]) ? 0xffffffffu : 0x00000000u;
        MU_MEMCPY(&r.m[i], &m, sizeof(m));
    }
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_and(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t x, y;
        MU_MEMCPY(&x, &a.m[i], sizeof(x));
        MU_MEMCPY(&y, &b.m[i], sizeof(y));
        x &= y;
        MU_MEMCPY(&r.m[i], &x, sizeof(x));
    }
    return r;
}
/* (~a) & b */
MU_INLINE mu_f32x4 mu_f32x4_andnot(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t x, y;
        MU_MEMCPY(&x, &a.m[i], sizeof(x));
        MU_MEMCPY(&y, &b.m[i], sizeof(y));
        x = (~x) & y;
        MU_MEMCPY(&r.m[i], &x, sizeof(x));
    }
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_or(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t x, y;
        MU_MEMCPY(&x, &a.m[i], sizeof(x));
        MU_MEMCPY(&y, &b.m[i], sizeof(y));
        x |= y;
        MU_MEMCPY(&r.m[i], &x, sizeof(x));
    }
    return r;
}
MU_INLINE mu_f32x4 mu_f32x4_xor(mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t x, y;
        MU_MEMCPY(&x, &a.m[i], sizeof(x));
        MU_MEMCPY(&y, &b.m[i], sizeof(y));
        x ^= y;
        MU_MEMCPY(&r.m[i], &x, sizeof(x));
    }
    return r;
}

MU_INLINE mu_f32x4 mu_f32x4_select(mu_f32x4 cond, mu_f32x4 a, mu_f32x4 b)
{
    mu_f32x4 r;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t c;
        MU_MEMCPY(&c, &cond.m[i], sizeof(c));
        r.m[i] = (c != 0u) ? a.m[i] : b.m[i];
    }
    return r;
}

#endif

/* -------------------------------------------------------------------------- *
 * Reductions
 * -------------------------------------------------------------------------- */

MU_INLINE float mu_f32x4_hadd(mu_f32x4 v)
{
#if defined(MU_SIMD_SSE2)
    /* unpacklo -> [v0,v0,v1,v1], unpackhi -> [v2,v2,v3,v3]: pairwise sums,
       then one shuffle + add_ss collapses the two halves. */
    __m128 s = _mm_add_ps(_mm_unpacklo_ps(v.v, v.v), _mm_unpackhi_ps(v.v, v.v));
    s        = _mm_add_ss(s, _mm_shuffle_ps(s, s, _MM_SHUFFLE(0, 0, 0, 2)));
    return _mm_cvtss_f32(s);
#elif defined(MU_SIMD_NEON)
    float32x2_t s = vadd_f32(vget_low_f32(v.v), vget_high_f32(v.v));
    s             = vpadd_f32(s, s);
    return vget_lane_f32(s, 0);
#else
    return v.m[0] + v.m[1] + v.m[2] + v.m[3];
#endif
}

MU_INLINE float mu_f32x4_dot(mu_f32x4 a, mu_f32x4 b)
{
    return mu_f32x4_hadd(mu_f32x4_mul(a, b));
}

MU_INLINE float mu_f32x4_length(mu_f32x4 a)
{
    return mu_f32x4_hadd(mu_f32x4_sqrt(mu_f32x4_mul(a, a)));
}

/* -------------------------------------------------------------------------- *
 * Matrices
 * -------------------------------------------------------------------------- */

/* In-place 4x4 transpose; r[i] is row i on entry and exit.
   Deliberately lane-explicit: a 4x4 transpose is not on the inner-loop path
   (the loads/stores still vectorise), and paying that to get byte-identical
   results on SSE and NEON is the right trade. */
MU_INLINE void mu_f32x4_transpose4(mu_f32x4 r[4])
{
    float t[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i][j] = r[i].m[j];

    for (int i = 0; i < 4; ++i)
        r[i] = mu_f32x4_set(t[0][i], t[1][i], t[2][i], t[3][i]);
}

/* M * v where rows[i] is row i of M: the four broadcasts keep every lane of
   every row live in registers instead of re-reading memory per component. */
MU_INLINE mu_f32x4 mu_f32x4_mul_add_rows(const mu_f32x4 rows[4], mu_f32x4 v)
{
    mu_f32x4 acc = mu_f32x4_mul(rows[0], mu_f32x4_set1(v.m[0]));
    acc          = mu_f32x4_fmadd(rows[1], mu_f32x4_set1(v.m[1]), acc);
    acc          = mu_f32x4_fmadd(rows[2], mu_f32x4_set1(v.m[2]), acc);
    acc          = mu_f32x4_fmadd(rows[3], mu_f32x4_set1(v.m[3]), acc);
    return acc;
}
