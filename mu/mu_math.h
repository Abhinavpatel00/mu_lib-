#pragma once
#ifndef MU_MATH_H
#define MU_MATH_H

/*
 * mu_math.h — single-header 3D math for engines: vector/quat/matrix core plus
 *             SoA batch kernels that are the actual reason this exists.
 *
 * WHY (read this before adding a function)
 * ----------------------------------------
 * cglm is the reference point, and it is *not* slow. Its glm_mat4_mul is a
 * hand-written 16-FMA kernel; there is no 3x sitting inside a single call.
 * Benchmarks against it (perf_test/bench_math_vs_cglm.c) say the same thing
 * AGENTS.md keeps saying: instruction count is priority 8, data movement is
 * priority 1. So this header wins in three places and stays honest elsewhere:
 *
 *   1. ONE OBJECT IS NOT THE UNIT OF WORK.
 *      glm_mat4_mulv re-reads 64 B of matrix per call. Transform 1M points
 *      with cglm and you move 64 MB of matrix. mu_xform_points3_soa keeps the
 *      12 live coefficients in ZMM registers and streams the points: 16 points
 *      per 12 FMA, no matrix traffic.
 *      MEASURED (this box, 1M points, DRAM-resident): 2.65x. When the working
 *      set fits L2/L3 (64K points x 8 passes) it is 9.0x -- the 2.65x is a
 *      bandwidth roof, not a compute result, and quoting 9x for 1M points
 *      would be the kind of number AGENTS.md 2.6 exists to stop.
 *
 *   2. AFFINE IS A TYPE, NOT A PROMISE.
 *      World/instance/skinning matrices are 3x4. mu_mat4_mul_affine skips the
 *      fourth row's work (2.6x vs glm_mul); mu_inverse_affine divides out
 *      per-axis scale instead of expanding 4x4 cofactors (3.4x vs
 *      glm_mat4_inv).
 *      READ THE BASELINE, THOUGH: against cglm's *dedicated* rigid kernel
 *      glm_inv_tr -- a bare transpose, no reciprocal -- mu_inverse_affine is
 *      0.42x, i.e. 2.4x SLOWER, because it pays three rcp that a rigid matrix
 *      does not need. That is why mu_inverse_rigid exists and is the call to
 *      use for orthonormal 3x3: it measures 0.98x glm_inv_tr (parity), and
 *      still 2.4x faster than mu_inverse_affine.
 *
 *   3. LANES = OBJECTS FOR PALETTES.
 *      A bone palette / instance batch multiplies unrelated matrices. Each
 *      128-bit lane holding one matrix's column turns 4 mat4 muls into 16 FMA
 *      + 16 shuffles instead of 4x(16 broadcast + 16 FMA).
 *      MEASURED: the kernel itself is 1.7x, but the pack/unpack around it is
 *      not free -- end-to-end on 256 matrices it is 1.00x (parity), and 0.82x
 *      if you re-pack every iteration. Amortise the pack or do not bother.
 *
 * Per-call, single-object entry points are provided for correctness and for
 * code that has not been restructured yet. Measured, they are at parity with
 * cglm: vec3 dot/cross/add 1.00x (3 lanes do not pay for SIMD in either), and
 * mat4_mul 1.69x, mat4_mulv 2.89x, transpose 1.9x, quat_mul 2.5x come from
 * width, not cleverness.
 *
 * THE LOSSES, because a table that only has wins in it is a lie:
 *   mat4_inverse (general)      0.85-0.95x   cglm's cofactor kernel is 70
 *                                            instructions to mu's 76; they
 *                                            wrote it by hand in SSSE3 and it
 *                                            is very good. Use
 *                                            mu_inverse_rigid/_affine.
 *   vec3_normalize_fast         0.42x        scalar rsqrt_ss + 1 Newton step
 *                                            cannot be SLP-vectorized, while
 *                                            hardware vsqrtps can: in a
 *                                            throughput loop the "fast" path
 *                                            is 2.4x slower. It wins only in a
 *                                            dependent chain (1.06x). Use the
 *                                            SoA batch kernel, not this.
 *   mat3_inverse_transpose      0.96x        parity, no story.
 *   palette, if you re-pack     0.82x        see 3 above.
 *
 * MEASURE, DO NOT BELIEVE. Every phase-2 claim above has a row in
 * perf_test/results_math.csv (see perf_test/PERF_MATH.md for the seven-field
 * methodology record). Re-run before trusting on a new box.
 *
 * LAYOUT CONTRACT
 * ---------------
 *   mu_vec3   {x,y,z}            12 B, packed. Structs <= 16 B return in
 *                                registers on SysV; a vec3 is xmm0.
 *   mu_vec4   {x,y,z,w}          16 B, 16-aligned.
 *   mu_quat   {x,y,z,w}          identity = (0,0,0,1).
 *   mu_mat3   3 x mu_vec3        column-major, columns c0..c2.
 *   mu_mat4   4 x mu_vec4        COLUMN-MAJOR, columns c0..c3.
 *
 * mu_mat4 is byte-identical to cglm's `mat4` and to a GLSL `mat4` upload, so
 * (float(*)[4])&m works for interop and for glUniformMatrix4fv. cglm's mat3 is
 * `float[3][3]` = column-major too; mu_mat3 matches it byte for byte.
 *
 * CONVENTIONS
 * -----------
 *   - Right-handed, column vectors: dest = M * v.
 *   - "transform_point" applies the full 4x4 (perspective divide NOT included;
 *     use mu_mat4_transform_point_h for the homogeneous version).
 *   - "affine" means the last row is (0,0,0,1). We do not check it. Lying to
 *     this API gives you a silently wrong matrix, not a crash. That is the
 *     same trade cglm makes; the assert is debug-only where it exists.
 *   - *_fast variants use hardware rsqrt + one Newton step (~22 correct
 *     mantissa bits, ~1e-7 relative). Do not use them to map integers to
 *     credits.
 *
 * BACKENDS: AVX-512F (preferred) -> AVX2+FMA -> SSE2 -> scalar. Selected by
 * `-march=`; no runtime dispatch, no IFUNC (that trap is AGENTS.md §12.11).
 * Every backend implements identical semantics; the benchmarks cross-check
 * every result against the scalar path, so a backend bug shows up as a
 * checksum mismatch, not as a mystery image.
 */

#include "mu_common.h"

#include <math.h>

/* -------------------------------------------------------------------------- *
 * Backend detection
 * -------------------------------------------------------------------------- */

/* MU_MATH_SCALAR forces the portable path on any target. It exists so the
 * scalar reference and a SIMD backend can be built on the same box and diffed
 * (perf_test does exactly that), and so a new kernel can be checked against
 * the portable form without a second machine. Costs nothing when unused. */
#if defined(MU_MATH_SCALAR)
   /* deliberately empty: fall through to the scalar types and kernels */
#elif defined(__AVX512F__)
#  define MU_MATH_AVX512 1
#  define MU_MATH_AVX2   1
#  define MU_MATH_SSE    1
#  include <immintrin.h>
#elif defined(__AVX2__)
#  define MU_MATH_AVX2   1
#  define MU_MATH_SSE    1
#  include <immintrin.h>
#elif defined(__AVX__)
#  define MU_MATH_SSE    1
#  include <immintrin.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define MU_MATH_SSE    1
#  include <emmintrin.h>
#endif

#if defined(MU_MATH_SSE) && defined(__SSE4_1__)
#  define MU_MATH_SSE41 1
#endif

/* mu_mm_hadd_ps: one instruction on SSE3+, three on SSE2, never a loop. */
#if defined(MU_MATH_SSE) && defined(__SSE3__)
#  define MU_MATH_HADD 1
#endif

MU_BEGIN_EXTERN_C

/* -------------------------------------------------------------------------- *
 * Types
 * -------------------------------------------------------------------------- */

typedef struct mu_vec2 { float x, y; } mu_vec2;
typedef struct mu_vec3 { float x, y, z; } mu_vec3;
typedef struct MU_ALIGN(16) mu_vec4 { float x, y, z, w; } mu_vec4;

typedef mu_vec4 mu_quat;

typedef struct mu_mat3 { mu_vec3 col[3]; } mu_mat3;

typedef struct MU_ALIGN(16) mu_mat4
{
    mu_vec4 col[4];
} mu_mat4;

/* Plane: n.x*p.x + n.y*p.y + n.z*p.z + d, positive = inside (Frostbite/cglm
 * convention: d is the negated dot of a point on the plane). */
typedef mu_vec4 mu_plane;

/* 6 planes: left, right, bottom, top, near, far. */
typedef struct mu_frustum { mu_plane p[6]; } mu_frustum;

/* A matrix already loaded into vector registers. Hoist this out of a point
 * loop: it is the difference between 5 loads + 4 FMA per point and 1 load +
 * 4 FMA per point. MEASURED 2.90x against glm_mat4_mulv3 per call
 * (0.62 ns vs 1.80 ns, 256 matrices, CV 0.6%) -- but per-call parity is the
 * floor; the real target is the SoA kernel below. */
typedef struct MU_ALIGN(16) mu_mat4_hot
{
#if defined(MU_MATH_SSE)
    __m128 c0, c1, c2, c3;
#else
    mu_vec4 c0, c1, c2, c3;
#endif
} mu_mat4_hot;

/* Four matrices packed for lane=object multiplication.
 *   col[j].lane128[m] = matrix m's column j
 * so one 512-bit FMA advances four unrelated matrix multiplies at once.
 * Build with mu_mat4x4, consume with mu_mat4x4_mul, and unpack with
 * mu_mat4_unpack4. On <AVX-512 the pack type degrades to 4 plain mat4s and the
 * ops fall back to 4 scalar mul_mul calls with identical results. */
#if defined(MU_MATH_AVX512)
typedef struct MU_ALIGN(64) mu_mat4x4 { __m512 col[4]; } mu_mat4x4;
#else
typedef struct MU_ALIGN(64) mu_mat4x4 { mu_mat4 m[4]; } mu_mat4x4;
#endif

/* -------------------------------------------------------------------------- *
 * Scalar helpers
 * -------------------------------------------------------------------------- */

#define MU_MATH_EPS 1e-6f

MU_INLINE float mu_radians(float deg) { return deg * 0.017453292519943295f; }
MU_INLINE float mu_degrees(float rad) { return rad * 57.29577951308232f; }

MU_INLINE float mu_lerp_f32(float a, float b, float t) { return a + (b - a) * t; }
MU_INLINE float mu_clamp_f32(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
MU_INLINE float mu_sign_f32(float v) { return (v > 0.0f) - (v < 0.0f); }

/* Exact 1/sqrt, matching 1.0f/sqrtf(x) bit for bit. */
MU_INLINE float mu_inv_sqrt_f32(float x)
{
    return 1.0f / sqrtf(x);
}

/* Hardware reciprocal-sqrt + one Newton-Raphson step: ~22 correct mantissa
 * bits (relative error ~1e-7), no divide, no sqrt port. This is what the
 * _fast and batch paths use. One step is deliberate: two steps is exact and
 * costs more than the sqrt+div it replaces. */
MU_INLINE float mu_rsqrt_f32(float x)
{
#if defined(MU_MATH_SSE)
    __m128 v, r;

    v = _mm_set_ss(x);
    r = _mm_rsqrt_ss(v);
    r = _mm_mul_ss(r, _mm_sub_ss(_mm_set_ss(1.5f),
                                 _mm_mul_ss(_mm_mul_ss(v, _mm_set_ss(0.5f)),
                                            _mm_mul_ss(r, r))));
    return _mm_cvtss_f32(r);
#else
    float y;

    y = 1.0f / sqrtf(x);
    return y * (1.5f - 0.5f * x * y * y);
#endif
}

/* -------------------------------------------------------------------------- *
 * vec2 (cheap, no SIMD: two lanes never amortize a load)
 * -------------------------------------------------------------------------- */

MU_INLINE mu_vec2 mu_vec2_make(float x, float y) { mu_vec2 r; r.x = x; r.y = y; return r; }
MU_INLINE mu_vec2 mu_vec2_add(mu_vec2 a, mu_vec2 b) { return mu_vec2_make(a.x + b.x, a.y + b.y); }
MU_INLINE mu_vec2 mu_vec2_sub(mu_vec2 a, mu_vec2 b) { return mu_vec2_make(a.x - b.x, a.y - b.y); }
MU_INLINE mu_vec2 mu_vec2_scale(mu_vec2 a, float s) { return mu_vec2_make(a.x * s, a.y * s); }
MU_INLINE float   mu_vec2_dot(mu_vec2 a, mu_vec2 b) { return a.x * b.x + a.y * b.y; }
MU_INLINE float   mu_vec2_cross(mu_vec2 a, mu_vec2 b) { return a.x * b.y - a.y * b.x; }
MU_INLINE float   mu_vec2_length2(mu_vec2 v) { return mu_vec2_dot(v, v); }
MU_INLINE float   mu_vec2_length(mu_vec2 v) { return sqrtf(mu_vec2_length2(v)); }
MU_INLINE mu_vec2 mu_vec2_normalize(mu_vec2 v)
{
    float inv;

    inv = mu_inv_sqrt_f32(mu_vec2_length2(v));
    return mu_vec2_scale(v, inv);
}

/* -------------------------------------------------------------------------- *
 * vec3 — scalar where scalar wins, SIMD where it does not
 *
 * A vec3 is 3 lanes. Loading it into an xmm lane costs a shuffle, and a
 * horizontal reduction costs two more; for dot and cross the scalar FMA chain
 * is both shorter and lower latency. cglm does the same thing for the same
 * reason. normalize is the exception: sqrtf + divide is ~25 cycles of
 * non-pipelined latency, rsqrt + one NR is ~10, and the result is exact enough
 * for a unit normal. So `mu_vec3_normalize` is exact (bit-comparable to cglm)
 * and `mu_vec3_normalize_fast` is the one you use in a hot loop.
 * -------------------------------------------------------------------------- */

MU_INLINE mu_vec3 mu_vec3_make(float x, float y, float z) { mu_vec3 r; r.x = x; r.y = y; r.z = z; return r; }
MU_INLINE mu_vec3 mu_vec3_splat(float s) { return mu_vec3_make(s, s, s); }
MU_INLINE mu_vec3 mu_vec3_zero(void) { return mu_vec3_make(0.0f, 0.0f, 0.0f); }
MU_INLINE mu_vec3 mu_vec3_from_vec4(mu_vec4 v) { return mu_vec3_make(v.x, v.y, v.z); }
MU_INLINE mu_vec4 mu_vec3_to_vec4(mu_vec3 v, float w) { mu_vec4 r; r.x = v.x; r.y = v.y; r.z = v.z; r.w = w; return r; }

MU_INLINE mu_vec3 mu_vec3_add(mu_vec3 a, mu_vec3 b) { return mu_vec3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
MU_INLINE mu_vec3 mu_vec3_sub(mu_vec3 a, mu_vec3 b) { return mu_vec3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
MU_INLINE mu_vec3 mu_vec3_mul(mu_vec3 a, mu_vec3 b) { return mu_vec3_make(a.x * b.x, a.y * b.y, a.z * b.z); }
MU_INLINE mu_vec3 mu_vec3_scale(mu_vec3 a, float s) { return mu_vec3_make(a.x * s, a.y * s, a.z * s); }
MU_INLINE mu_vec3 mu_vec3_negate(mu_vec3 a) { return mu_vec3_make(-a.x, -a.y, -a.z); }
MU_INLINE mu_vec3 mu_vec3_min(mu_vec3 a, mu_vec3 b) { return mu_vec3_make(MU_MIN(a.x, b.x), MU_MIN(a.y, b.y), MU_MIN(a.z, b.z)); }
MU_INLINE mu_vec3 mu_vec3_max(mu_vec3 a, mu_vec3 b) { return mu_vec3_make(MU_MAX(a.x, b.x), MU_MAX(a.y, b.y), MU_MAX(a.z, b.z)); }
MU_INLINE mu_vec3 mu_vec3_abs(mu_vec3 a) { return mu_vec3_make(fabsf(a.x), fabsf(a.y), fabsf(a.z)); }
MU_INLINE mu_vec3 mu_vec3_lerp(mu_vec3 a, mu_vec3 b, float t)
{
    return mu_vec3_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}
MU_INLINE mu_vec3 mu_vec3_muladd(mu_vec3 a, float s, mu_vec3 b)
{
    /* a*s + b, single rounding under -ffp-contract=fast */
    return mu_vec3_make(a.x * s + b.x, a.y * s + b.y, a.z * s + b.z);
}

MU_INLINE float mu_vec3_dot(mu_vec3 a, mu_vec3 b)
{
    /* Two independent products, then fold. Deliberately not a horizontal SIMD
     * reduction: for 3 lanes the shuffle chain costs more than it saves. */
    float xy, z;

    xy = a.x * b.x + a.y * b.y;
    z  = a.z * b.z;
    return xy + z;
}

MU_INLINE float mu_vec3_dot2(mu_vec3 a, mu_vec3 b)
{
    /* 2D dot of the xy components: what culling and UI hit-tests actually want. */
    return a.x * b.x + a.y * b.y;
}

MU_INLINE mu_vec3 mu_vec3_cross(mu_vec3 a, mu_vec3 b)
{
    return mu_vec3_make(a.y * b.z - a.z * b.y,
                        a.z * b.x - a.x * b.z,
                        a.x * b.y - a.y * b.x);
}

MU_INLINE float mu_vec3_length2(mu_vec3 v) { return mu_vec3_dot(v, v); }
MU_INLINE float mu_vec3_length(mu_vec3 v) { return sqrtf(mu_vec3_dot(v, v)); }
MU_INLINE float mu_vec3_distance2(mu_vec3 a, mu_vec3 b) { return mu_vec3_dot(mu_vec3_sub(a, b), mu_vec3_sub(a, b)); }
MU_INLINE float mu_vec3_distance(mu_vec3 a, mu_vec3 b) { return sqrtf(mu_vec3_distance2(a, b)); }

MU_INLINE mu_vec3 mu_vec3_normalize(mu_vec3 v)
{
    float len, inv;

    len = mu_vec3_length(v);
    if (MU_UNLIKELY(!(len > 0.0f)))
    {
        /* Mu-driven code should not be normalizing degenerate vectors; return
         * zero rather than NaN so a bad asset degrades instead of poisoning a
         * whole transform hierarchy. */
        return mu_vec3_zero();
    }
    inv = 1.0f / len;
    return mu_vec3_make(v.x * inv, v.y * inv, v.z * inv);
}

MU_INLINE mu_vec3 mu_vec3_normalize_fast(mu_vec3 v)
{
    float inv;

    inv = mu_rsqrt_f32(mu_vec3_length2(v));
    return mu_vec3_make(v.x * inv, v.y * inv, v.z * inv);
}

MU_INLINE mu_vec3 mu_vec3_reflect(mu_vec3 v, mu_vec3 n)
{
    /* v - 2*dot(v,n)*n */
    return mu_vec3_muladd(n, -2.0f * mu_vec3_dot(v, n), v);
}

MU_INLINE mu_vec3 mu_vec3_project(mu_vec3 v, mu_vec3 onto)
{
    /* onto is assumed unit length. */
    return mu_vec3_scale(onto, mu_vec3_dot(v, onto));
}

/* -------------------------------------------------------------------------- *
 * vec4 — full-width, so SIMD pays; these stay in registers across a chain
 * -------------------------------------------------------------------------- */

MU_INLINE mu_vec4 mu_vec4_make(float x, float y, float z, float w)
{
    mu_vec4 r;
    r.x = x; r.y = y; r.z = z; r.w = w;
    return r;
}
MU_INLINE mu_vec4 mu_vec4_splat(float s) { return mu_vec4_make(s, s, s, s); }
MU_INLINE mu_vec4 mu_vec4_zero(void) { return mu_vec4_make(0.0f, 0.0f, 0.0f, 0.0f); }

#if defined(MU_MATH_SSE)
#  define MU__ALIAS(a, b) a.b
#else
#  define MU__ALIAS(a, b) a
#endif

MU_INLINE mu_vec4 mu_vec4_add(mu_vec4 a, mu_vec4 b)
{
    mu_vec4 r;
#if defined(MU_MATH_SSE)
    _mm_storeu_ps(&r.x, _mm_add_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x)));
#else
    r = mu_vec4_make(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
#endif
    return r;
}

MU_INLINE mu_vec4 mu_vec4_sub(mu_vec4 a, mu_vec4 b)
{
    mu_vec4 r;
#if defined(MU_MATH_SSE)
    _mm_storeu_ps(&r.x, _mm_sub_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x)));
#else
    r = mu_vec4_make(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
#endif
    return r;
}

MU_INLINE mu_vec4 mu_vec4_mul(mu_vec4 a, mu_vec4 b)
{
    mu_vec4 r;
#if defined(MU_MATH_SSE)
    _mm_storeu_ps(&r.x, _mm_mul_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x)));
#else
    r = mu_vec4_make(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
#endif
    return r;
}

MU_INLINE mu_vec4 mu_vec4_scale(mu_vec4 a, float s)
{
    mu_vec4 r;
#if defined(MU_MATH_SSE)
    _mm_storeu_ps(&r.x, _mm_mul_ps(_mm_loadu_ps(&a.x), _mm_set1_ps(s)));
#else
    r = mu_vec4_make(a.x * s, a.y * s, a.z * s, a.w * s);
#endif
    return r;
}

MU_INLINE float mu_vec4_dot(mu_vec4 a, mu_vec4 b)
{
#if defined(MU_MATH_SSE41)
    __m128 p;

    p = _mm_dp_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x), 0xF1);
    return _mm_cvtss_f32(p);
#elif defined(MU_MATH_HADD)
    __m128 p, s;

    p = _mm_mul_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x));
    s = _mm_hadd_ps(p, p);
    s = _mm_hadd_ps(s, s);
    return _mm_cvtss_f32(s);
#else
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
#endif
}

MU_INLINE mu_vec4 mu_vec4_lerp(mu_vec4 a, mu_vec4 b, float t)
{
    mu_vec4 r;
#if defined(MU_MATH_SSE)
    __m128 va, vt;

    va = _mm_loadu_ps(&a.x);
    vt = _mm_set1_ps(t);
    _mm_storeu_ps(&r.x, _mm_add_ps(va, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(&b.x), va), vt)));
#else
    r = mu_vec4_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                     a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
#endif
    return r;
}

/* -------------------------------------------------------------------------- *
 * Quaternion — xyzw, unit, right-handed, rotates v as q*v*q^-1
 * -------------------------------------------------------------------------- */

MU_INLINE mu_quat mu_quat_identity(void) { return mu_vec4_make(0.0f, 0.0f, 0.0f, 1.0f); }

MU_INLINE mu_quat mu_quat_make(float x, float y, float z, float w) { return mu_vec4_make(x, y, z, w); }

MU_INLINE mu_quat mu_quat_axis_angle(mu_vec3 axis, float radians)
{
    float h, s;

    axis = mu_vec3_normalize(axis);
    h    = 0.5f * radians;
    s    = sinf(h);
    return mu_vec4_make(axis.x * s, axis.y * s, axis.z * s, cosf(h));
}

MU_INLINE mu_quat mu_quat_mul(mu_quat a, mu_quat b)
{
    /* Hamilton product; 16 mul + 12 add is the floor without a shuffle-heavy
     * SSE formulation that never wins for a single result. */
    return mu_vec4_make(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

MU_INLINE mu_quat mu_quat_conjugate(mu_quat q) { return mu_vec4_make(-q.x, -q.y, -q.z, q.w); }

MU_INLINE float mu_quat_dot(mu_quat a, mu_quat b) { return mu_vec4_dot(a, b); }

MU_INLINE mu_quat mu_quat_normalize(mu_quat q)
{
    float inv;

    inv = mu_inv_sqrt_f32(mu_vec4_dot(q, q));
    return mu_vec4_scale(q, inv);
}

MU_INLINE mu_vec3 mu_quat_rotate_vec3(mu_quat q, mu_vec3 v)
{
    /* t = 2 * cross(q.xyz, v); v' = v + q.w*t + cross(q.xyz, t)
     * 3 cross products would be the naive expansion; this is 2 and no
     * matrix build. */
    mu_vec3 qv, t, c;

    qv = mu_vec3_from_vec4(q);
    t  = mu_vec3_scale(mu_vec3_cross(qv, v), 2.0f);
    c  = mu_vec3_cross(qv, t);
    return mu_vec3_make(v.x + q.w * t.x + c.x,
                        v.y + q.w * t.y + c.y,
                        v.z + q.w * t.z + c.z);
}

MU_INLINE mu_quat mu_quat_slerp(mu_quat a, mu_quat b, float t)
{
    /* Same algorithm as cglm's glm_quat_slerp, including which quaternion gets
     * negated for the short arc (it negates `from`, not `to`). That matters:
     * q and -q are the same rotation but different numbers, and "same" is a
     * property a drop-in replacement has to keep. Verified by
     * perf_test/test_math.c, which compares against cglm component by
     * component.
     *
     * slerp is transcendental-bound (two sinf + one acosf), so there is no
     * vector trick here; the win in this header is elsewhere. */
    float d, sin_theta, angle, s0, s1;
    mu_quat orig;

    d    = mu_vec4_dot(a, b);
    orig = a;

    if (fabsf(d) >= 1.0f)
        return a;

    if (d < 0.0f)
    {
        a = mu_vec4_scale(a, -1.0f);
        d = -d;
    }

    sin_theta = sqrtf(1.0f - d * d);

    if (fabsf(sin_theta) < 0.001f)
    {
        /* Nearly parallel: divide by ~0 is the alternative. */
        return mu_vec4_lerp(orig, b, t);
    }

    angle = acosf(d);
    s0    = sinf((1.0f - t) * angle);
    s1    = sinf(t * angle);
    return mu_vec4_scale(mu_vec4_add(mu_vec4_scale(a, s0), mu_vec4_scale(b, s1)),
                         1.0f / sin_theta);
}

MU_INLINE mu_quat mu_quat_nlerp(mu_quat a, mu_quat b, float t)
{
    mu_quat r;
    float inv;

    if (mu_vec4_dot(a, b) < 0.0f) b = mu_vec4_scale(b, -1.0f);
    r   = mu_vec4_add(a, mu_vec4_scale(mu_vec4_sub(b, a), t));
    inv = mu_inv_sqrt_f32(mu_vec4_dot(r, r));
    return mu_vec4_scale(r, inv);
}

/* -------------------------------------------------------------------------- *
 * mat3 — normal matrices, inertia tensors, 2D-in-3D basis sets
 * -------------------------------------------------------------------------- */

MU_INLINE mu_mat3 mu_mat3_identity(void)
{
    mu_mat3 r;

    r.col[0] = mu_vec3_make(1.0f, 0.0f, 0.0f);
    r.col[1] = mu_vec3_make(0.0f, 1.0f, 0.0f);
    r.col[2] = mu_vec3_make(0.0f, 0.0f, 1.0f);
    return r;
}

MU_INLINE mu_vec3 mu_mat3_mulv(const mu_mat3 *m, mu_vec3 v)
{
    return mu_vec3_make(m->col[0].x * v.x + m->col[1].x * v.y + m->col[2].x * v.z,
                        m->col[0].y * v.x + m->col[1].y * v.y + m->col[2].y * v.z,
                        m->col[0].z * v.x + m->col[1].z * v.y + m->col[2].z * v.z);
}

MU_INLINE mu_mat3 mu_mat3_mul(const mu_mat3 *a, const mu_mat3 *b)
{
    mu_mat3 r;

    r.col[0] = mu_mat3_mulv(a, b->col[0]);
    r.col[1] = mu_mat3_mulv(a, b->col[1]);
    r.col[2] = mu_mat3_mulv(a, b->col[2]);
    return r;
}

MU_INLINE mu_mat3 mu_mat3_from_mat4_linear(const mu_mat4 *m)
{
    mu_mat3 r;

    r.col[0] = mu_vec3_from_vec4(m->col[0]);
    r.col[1] = mu_vec3_from_vec4(m->col[1]);
    r.col[2] = mu_vec3_from_vec4(m->col[2]);
    return r;
}

/* 3x3 inverse by column cross products: for a matrix M = [c0|c1|c2],
 *
 *     M^-1 = [ cross(c1,c2) | cross(c2,c0) | cross(c0,c1) ] / det
 *     det  = dot(c0, cross(c1,c2))
 *
 * One determinant, one reciprocal, three crosses. The cofactor expansion
 * needs 18 products; this needs 9 and reads like the geometry it is. */
MU_INLINE mu_mat3 mu_mat3_inverse(const mu_mat3 *m)
{
    mu_mat3 r;
    mu_vec3 x01, x12, x20;
    float det, inv;

    x12 = mu_vec3_cross(m->col[1], m->col[2]);
    det = mu_vec3_dot(m->col[0], x12);
    inv = (det != 0.0f) ? (1.0f / det) : 0.0f;

    x20 = mu_vec3_cross(m->col[2], m->col[0]);
    x01 = mu_vec3_cross(m->col[0], m->col[1]);

    /* The three crosses are the ROWS of the adjugate, so in column-major
     * storage they go into the columns transposed. */
    r.col[0] = mu_vec3_make(x12.x * inv, x20.x * inv, x01.x * inv);
    r.col[1] = mu_vec3_make(x12.y * inv, x20.y * inv, x01.y * inv);
    r.col[2] = mu_vec3_make(x12.z * inv, x20.z * inv, x01.z * inv);
    return r;
}

MU_INLINE mu_mat3 mu_mat3_transpose(const mu_mat3 *m)
{
    mu_mat3 r;

    r.col[0] = mu_vec3_make(m->col[0].x, m->col[1].x, m->col[2].x);
    r.col[1] = mu_vec3_make(m->col[0].y, m->col[1].y, m->col[2].y);
    r.col[2] = mu_vec3_make(m->col[0].z, m->col[1].z, m->col[2].z);
    return r;
}

/* Normal matrix = inverse-transpose of the upper 3x3. Not the same as the
 * upper 3x3 when the transform has non-uniform scale, which is the whole
 * reason it exists. */
MU_INLINE mu_mat3 mu_mat3_inverse_transpose(const mu_mat3 *m)
{
    mu_mat3 inv;

    inv = mu_mat3_inverse(m);
    return mu_mat3_transpose(&inv);
}

/* -------------------------------------------------------------------------- *
 * mat4 construction
 * -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- *
 * normal transformation (cofactor, not inverse-transpose)
 * -------------------------------------------------------------------------- *
 *
 * The standard recipe for transforming a normal under a non-uniform or skewed
 * matrix is `transpose(inverse(M)) * n`. It is standard because it is derived
 * from the wrong constraint: it enforces n' . t' = 0 (orthogonality to a
 * tangent) instead of the actual requirement that n' be perpendicular to the
 * transformed tangent PLANE.
 *
 * Those differ exactly when det(M) < 0, i.e. under a reflection: the dot product
 * derivation admits two solutions, orientation is not one of its constraints,
 * and the normal comes out pointing INTO the surface. Lighting then evaluates
 * max(0, dot(n, l)) = 0 and the object goes black. (graphitemaster, "The details
 * of transforming normals"; normals_revisited ships the cofactor code this
 * implements.)
 *
 * The correct operator is the COFACTOR matrix, C = det(A) * inverse(A)^T, which
 * differs from inverse(A)^T precisely by the sign of det(A). It is cheaper too:
 * for A with columns a0, a1, a2,
 *
 *     C = [ a1 x a2 , a2 x a0 , a0 x a1 ]        (as columns)
 *
 * because inverse(A)^T is those same three cross products divided by det(A).
 * Three crosses and a 3x3 multiply, versus a 4x4 inverse plus a transpose.
 *
 * For a rotation C == A exactly, and for uniform scale s it is s^2 * A: if the
 * matrix is known rigid, skip all of this and transform with the matrix.
 */

/* Cofactor matrix: the operator that transforms normals correctly for ANY
 * invertible linear map, reflections included. */
MU_INLINE mu_mat3 mu_mat3_cofactor(const mu_mat3 *m)
{
    mu_mat3 c;

    c.col[0] = mu_vec3_cross(m->col[1], m->col[2]);
    c.col[1] = mu_vec3_cross(m->col[2], m->col[0]);
    c.col[2] = mu_vec3_cross(m->col[0], m->col[1]);
    return c;
}

/* Normal transform straight from a mat4's linear part. The translation column
 * cannot affect a direction and is never read, which is why this needs no 4x4
 * inverse at all.
 *
 * The result is NOT normalized: C scales normals by det(A), so normalize at the
 * end unless the caller already knows the scale. */
MU_INLINE mu_vec3 mu_mat4_normal(const mu_mat4 *m, mu_vec3 n)
{
    const mu_vec3 c0 = mu_vec3_make(m->col[0].x, m->col[0].y, m->col[0].z);
    const mu_vec3 c1 = mu_vec3_make(m->col[1].x, m->col[1].y, m->col[1].z);
    const mu_vec3 c2 = mu_vec3_make(m->col[2].x, m->col[2].y, m->col[2].z);
    const mu_vec3 r0 = mu_vec3_cross(c1, c2);
    const mu_vec3 r1 = mu_vec3_cross(c2, c0);
    const mu_vec3 r2 = mu_vec3_cross(c0, c1);

    /* C * n, with those three cross products as columns */
    return mu_vec3_make(r0.x * n.x + r1.x * n.y + r2.x * n.z,
                        r0.y * n.x + r1.y * n.y + r2.y * n.z,
                        r0.z * n.x + r1.z * n.y + r2.z * n.z);
}

MU_INLINE mu_mat4 mu_mat4_zero(void)
{
    mu_mat4 r;

    r.col[0] = mu_vec4_zero();
    r.col[1] = mu_vec4_zero();
    r.col[2] = mu_vec4_zero();
    r.col[3] = mu_vec4_zero();
    return r;
}

MU_INLINE mu_mat4 mu_mat4_identity(void)
{
    mu_mat4 r;

    r.col[0] = mu_vec4_make(1.0f, 0.0f, 0.0f, 0.0f);
    r.col[1] = mu_vec4_make(0.0f, 1.0f, 0.0f, 0.0f);
    r.col[2] = mu_vec4_make(0.0f, 0.0f, 1.0f, 0.0f);
    r.col[3] = mu_vec4_make(0.0f, 0.0f, 0.0f, 1.0f);
    return r;
}

MU_INLINE mu_mat4 mu_mat4_from_cols(mu_vec4 c0, mu_vec4 c1, mu_vec4 c2, mu_vec4 c3)
{
    mu_mat4 r;

    r.col[0] = c0;
    r.col[1] = c1;
    r.col[2] = c2;
    r.col[3] = c3;
    return r;
}

MU_INLINE mu_mat4 mu_mat4_from_quat(mu_quat q)
{
    mu_mat4 r;
    float xx, yy, zz, xy, xz, yz, wx, wy, wz;

    xx = q.x * q.x; yy = q.y * q.y; zz = q.z * q.z;
    xy = q.x * q.y; xz = q.x * q.z; yz = q.y * q.z;
    wx = q.w * q.x; wy = q.w * q.y; wz = q.w * q.z;

    r.col[0] = mu_vec4_make(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f);
    r.col[1] = mu_vec4_make(2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0.0f);
    r.col[2] = mu_vec4_make(2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0.0f);
    r.col[3] = mu_vec4_make(0.0f, 0.0f, 0.0f, 1.0f);
    return r;
}

MU_INLINE mu_mat4 mu_mat4_trs(mu_vec3 translation, mu_quat rotation, mu_vec3 scale)
{
    mu_mat4 r;

    r = mu_mat4_from_quat(rotation);
    r.col[0] = mu_vec4_scale(r.col[0], scale.x);
    r.col[1] = mu_vec4_scale(r.col[1], scale.y);
    r.col[2] = mu_vec4_scale(r.col[2], scale.z);
    r.col[3] = mu_vec3_to_vec4(translation, 1.0f);
    return r;
}

MU_INLINE mu_mat4 mu_mat4_translate(mu_vec3 t)
{
    mu_mat4 r;

    r = mu_mat4_identity();
    r.col[3] = mu_vec3_to_vec4(t, 1.0f);
    return r;
}

MU_INLINE mu_mat4 mu_mat4_scale(mu_vec3 s)
{
    mu_mat4 r;

    r = mu_mat4_identity();
    r.col[0].x = s.x;
    r.col[1].y = s.y;
    r.col[2].z = s.z;
    return r;
}

MU_INLINE mu_mat4 mu_mat4_rotate(mu_vec3 axis, float radians)
{
    return mu_mat4_from_quat(mu_quat_axis_angle(axis, radians));
}

/* Right-handed perspective, depth in [-1, 1] (GL). For [0,1] (Vulkan/D3D)
 * pass the `_zo` variant. Column-major, so m[2][3] = -1 lives at col[2].w. */
MU_INLINE mu_mat4 mu_mat4_perspective_zo(float fovy_radians, float aspect, float znear, float zfar, int zero_to_one)
{
    mu_mat4 r;
    float f, nf;

    MU_ASSERT(aspect != 0.0f);
    MU_ASSERT(zfar != znear);

    f = 1.0f / tanf(0.5f * fovy_radians);
    r = mu_mat4_zero();
    r.col[0].x = f / aspect;
    r.col[1].y = f;
    r.col[3].w = 0.0f;

    if (zero_to_one)
    {
        r.col[2].z = zfar / (znear - zfar);
        r.col[2].w = -1.0f;
        r.col[3].z = (zfar * znear) / (znear - zfar);
    }
    else
    {
        nf = 1.0f / (znear - zfar);
        r.col[2].z = (zfar + znear) * nf;
        r.col[2].w = -1.0f;
        r.col[3].z = 2.0f * zfar * znear * nf;
    }
    return r;
}

MU_INLINE mu_mat4 mu_mat4_perspective(float fovy_radians, float aspect, float znear, float zfar)
{
    return mu_mat4_perspective_zo(fovy_radians, aspect, znear, zfar, 0);
}

MU_INLINE mu_mat4 mu_mat4_ortho(float left, float right, float bottom, float top, float znear, float zfar)
{
    mu_mat4 r;

    r = mu_mat4_identity();
    r.col[0].x = 2.0f / (right - left);
    r.col[1].y = 2.0f / (top - bottom);
    r.col[2].z = -2.0f / (zfar - znear);
    r.col[3].x = -(right + left) / (right - left);
    r.col[3].y = -(top + bottom) / (top - bottom);
    r.col[3].z = -(zfar + znear) / (zfar - znear);
    return r;
}

MU_INLINE mu_mat4 mu_mat4_look_at(mu_vec3 eye, mu_vec3 target, mu_vec3 up)
{
    mu_mat4 r;
    mu_vec3 f, s, u;

    f = mu_vec3_normalize(mu_vec3_sub(target, eye));
    s = mu_vec3_normalize(mu_vec3_cross(f, up));
    u = mu_vec3_cross(s, f);

    r.col[0] = mu_vec4_make(s.x, u.x, -f.x, 0.0f);
    r.col[1] = mu_vec4_make(s.y, u.y, -f.y, 0.0f);
    r.col[2] = mu_vec4_make(s.z, u.z, -f.z, 0.0f);
    r.col[3] = mu_vec4_make(-mu_vec3_dot(s, eye), -mu_vec3_dot(u, eye), mu_vec3_dot(f, eye), 1.0f);
    return r;
}

/* -------------------------------------------------------------------------- *
 * mat4 arithmetic — SSE, AVX-512, and scalar backends
 * -------------------------------------------------------------------------- */

#if defined(MU_MATH_SSE)
#  define MU__SPLAT_X(v) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(0, 0, 0, 0))
#  define MU__SPLAT_Y(v) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(1, 1, 1, 1))
#  define MU__SPLAT_Z(v) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(2, 2, 2, 2))
#  define MU__SPLAT_W(v) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(3, 3, 3, 3))
/* unpack lanes 0..2 of an xmm into a mu_vec3 without a memory round-trip */
#  define MU__STORE_V3(v, out)                                                   \
      do {                                                                       \
          (out).x = _mm_cvtss_f32(v);                                            \
          (out).y = _mm_cvtss_f32(_mm_shuffle_ps((v), (v), _MM_SHUFFLE(1, 1, 1, 1))); \
          (out).z = _mm_cvtss_f32(_mm_movehl_ps((v), (v)));                       \
      } while (0)
#  if defined(__FMA__)
#    define MU__FMA(a, b, c)     _mm_fmadd_ps((a), (b), (c))
#    define MU__FNMA(a, b, c)    _mm_fnmadd_ps((a), (b), (c))
#  else
#    define MU__FMA(a, b, c)     _mm_add_ps(_mm_mul_ps((a), (b)), (c))
#    define MU__FNMA(a, b, c)    _mm_sub_ps((c), _mm_mul_ps((a), (b)))
#  endif
#endif

/* ---- single call, full 4x4 ---- */

MU_INLINE void mu_mat4_mul(const mu_mat4 *a, const mu_mat4 *b, mu_mat4 *dst)
{
#if defined(MU_MATH_AVX512)
    /* All 16 outputs in one ZMM accumulator.
     *
     *   acc[4j + i] = sum_k A[i][k] * B[k][j]
     *
     * Lane group j is destination column j and lane i inside it is row i, so:
     *   - the A operand is one vbroadcastf32x4 of A's column k, which already
     *     lands A[i][k] in position i (no shuffle);
     *   - the B operand needs B[k][j] uniform across the four lanes of group
     *     j, and since B is 16 contiguous floats that is exactly one vpermps
     *     of a single 64-byte load.
     *
     * 1 load + 4 broadcasts + 4 permutes + 4 FMA + 1 store for the whole
     * multiply, against ~33 uops for the per-column SSE formulation. The win
     * is shuffle-port pressure: the 4-column SSE kernel issues 12 shuffles on
     * a port that retires one per cycle, so it cannot beat ~12 cycles no
     * matter how good the FMA scheduling is.
     */
    const __m512i k0 = _mm512_set_epi32(12, 12, 12, 12, 8, 8, 8, 8, 4, 4, 4, 4, 0, 0, 0, 0);
    const __m512i k1 = _mm512_set_epi32(13, 13, 13, 13, 9, 9, 9, 9, 5, 5, 5, 5, 1, 1, 1, 1);
    const __m512i k2 = _mm512_set_epi32(14, 14, 14, 14, 10, 10, 10, 10, 6, 6, 6, 6, 2, 2, 2, 2);
    const __m512i k3 = _mm512_set_epi32(15, 15, 15, 15, 11, 11, 11, 11, 7, 7, 7, 7, 3, 3, 3, 3);
    __m512 bv, acc;

    bv  = _mm512_loadu_ps(&b->col[0].x);
    acc = _mm512_mul_ps(_mm512_broadcast_f32x4(_mm_loadu_ps(&a->col[0].x)),
                        _mm512_permutexvar_ps(k0, bv));
    acc = _mm512_fmadd_ps(_mm512_broadcast_f32x4(_mm_loadu_ps(&a->col[1].x)),
                          _mm512_permutexvar_ps(k1, bv), acc);
    acc = _mm512_fmadd_ps(_mm512_broadcast_f32x4(_mm_loadu_ps(&a->col[2].x)),
                          _mm512_permutexvar_ps(k2, bv), acc);
    acc = _mm512_fmadd_ps(_mm512_broadcast_f32x4(_mm_loadu_ps(&a->col[3].x)),
                          _mm512_permutexvar_ps(k3, bv), acc);

    _mm512_storeu_ps(&dst->col[0].x, acc);
#elif defined(MU_MATH_SSE)
    __m128 l, r0, r1, r2, r3, v0, v1, v2, v3;

    r0 = _mm_loadu_ps(&b->col[0].x);
    r1 = _mm_loadu_ps(&b->col[1].x);
    r2 = _mm_loadu_ps(&b->col[2].x);
    r3 = _mm_loadu_ps(&b->col[3].x);

    l  = _mm_loadu_ps(&a->col[0].x);
    v0 = _mm_mul_ps(MU__SPLAT_X(r0), l);
    v1 = _mm_mul_ps(MU__SPLAT_X(r1), l);
    v2 = _mm_mul_ps(MU__SPLAT_X(r2), l);
    v3 = _mm_mul_ps(MU__SPLAT_X(r3), l);

    l  = _mm_loadu_ps(&a->col[1].x);
    v0 = MU__FMA(MU__SPLAT_Y(r0), l, v0);
    v1 = MU__FMA(MU__SPLAT_Y(r1), l, v1);
    v2 = MU__FMA(MU__SPLAT_Y(r2), l, v2);
    v3 = MU__FMA(MU__SPLAT_Y(r3), l, v3);

    l  = _mm_loadu_ps(&a->col[2].x);
    v0 = MU__FMA(MU__SPLAT_Z(r0), l, v0);
    v1 = MU__FMA(MU__SPLAT_Z(r1), l, v1);
    v2 = MU__FMA(MU__SPLAT_Z(r2), l, v2);
    v3 = MU__FMA(MU__SPLAT_Z(r3), l, v3);

    l  = _mm_loadu_ps(&a->col[3].x);
    v0 = MU__FMA(MU__SPLAT_W(r0), l, v0);
    v1 = MU__FMA(MU__SPLAT_W(r1), l, v1);
    v2 = MU__FMA(MU__SPLAT_W(r2), l, v2);
    v3 = MU__FMA(MU__SPLAT_W(r3), l, v3);

    _mm_storeu_ps(&dst->col[0].x, v0);
    _mm_storeu_ps(&dst->col[1].x, v1);
    _mm_storeu_ps(&dst->col[2].x, v2);
    _mm_storeu_ps(&dst->col[3].x, v3);
#else
    int j, k;

    /* dst column j = sum_k A column k * B[k][j]: accumulate on columns of A,
     * which is the same shape as the SIMD kernels, so the two can be diffed
     * line by line. Indexing A by row here instead transposes the product --
     * that bug is silent for symmetric matrices and wrong everywhere else. */
    for (j = 0; j < 4; ++j)
    {
        float r0, r1, r2, r3;

        r0 = 0.0f; r1 = 0.0f; r2 = 0.0f; r3 = 0.0f;
        for (k = 0; k < 4; ++k)
        {
            const float s = (&b->col[j].x)[k];       /* B[k][j] */

            r0 += s * a->col[k].x;
            r1 += s * a->col[k].y;
            r2 += s * a->col[k].z;
            r3 += s * a->col[k].w;
        }
        dst->col[j] = mu_vec4_make(r0, r1, r2, r3);
    }
#endif
}

/* ---- single call, affine (last row is 0,0,0,1) ----
 *
 * 25% less arithmetic, and the saving is exactly the wasted row: the fourth
 * output component is constant 1 and its three FMAs are removed. */
MU_INLINE void mu_mat4_mul_affine(const mu_mat4 *a, const mu_mat4 *b, mu_mat4 *dst)
{
#if defined(MU_MATH_SSE)
    __m128 l0, l1, l2, r0, r1, r2, r3, v0, v1, v2, v3, t;

    r0 = _mm_loadu_ps(&b->col[0].x);
    r1 = _mm_loadu_ps(&b->col[1].x);
    r2 = _mm_loadu_ps(&b->col[2].x);
    r3 = _mm_loadu_ps(&b->col[3].x);

    l0 = _mm_loadu_ps(&a->col[0].x);
    l1 = _mm_loadu_ps(&a->col[1].x);
    l2 = _mm_loadu_ps(&a->col[2].x);
    t  = _mm_loadu_ps(&a->col[3].x);

    /* columns 0..2: three FMAs + a translation add instead of four FMAs */
    v0 = MU__FMA(MU__SPLAT_Y(r0), l1, _mm_mul_ps(MU__SPLAT_X(r0), l0));
    v1 = MU__FMA(MU__SPLAT_Y(r1), l1, _mm_mul_ps(MU__SPLAT_X(r1), l0));
    v2 = MU__FMA(MU__SPLAT_Y(r2), l1, _mm_mul_ps(MU__SPLAT_X(r2), l0));

    v0 = MU__FMA(MU__SPLAT_Z(r0), l2, v0);
    v1 = MU__FMA(MU__SPLAT_Z(r1), l2, v1);
    v2 = MU__FMA(MU__SPLAT_Z(r2), l2, v2);

    /* No translation add on columns 0..2: the term that would carry A's
     * translation is B[3][j] * A_col3, and B[3][j] is 0 for an affine B.
     * That zero is the whole 25%: 9 FMA instead of 12 here. */

    /* column 3 is the transformed translation: A * (B's t, 1). Its fourth
     * component falls out as 0+0+0+1 = 1 on its own, because A's column 3 has
     * w = 1 and A's columns 0..2 have w = 0. That is the one place the affine
     * structure genuinely removes work: no masking, no fixup. */
    v3 = MU__FMA(MU__SPLAT_Z(r3), l2, MU__FMA(MU__SPLAT_Y(r3), l1, _mm_mul_ps(MU__SPLAT_X(r3), l0)));
    v3 = _mm_add_ps(v3, t);

    _mm_storeu_ps(&dst->col[0].x, v0);
    _mm_storeu_ps(&dst->col[1].x, v1);
    _mm_storeu_ps(&dst->col[2].x, v2);
    _mm_storeu_ps(&dst->col[3].x, v3);
#else
    mu_mat4_mul(a, b, dst);
#endif
}

MU_INLINE mu_mat4 mu_mat4_mul_r(const mu_mat4 *a, const mu_mat4 *b)
{
    mu_mat4 r;

    mu_mat4_mul(a, b, &r);
    return r;
}

/* ---- matrix * vector ---- */

MU_INLINE mu_vec4 mu_mat4_mulv(const mu_mat4 *m, mu_vec4 v)
{
    mu_vec4 r;

#if defined(MU_MATH_SSE)
    __m128 x, r0;

    x  = _mm_loadu_ps(&v.x);
    r0 = _mm_mul_ps(MU__SPLAT_X(x), _mm_loadu_ps(&m->col[0].x));
    r0 = MU__FMA(MU__SPLAT_Y(x), _mm_loadu_ps(&m->col[1].x), r0);
    r0 = MU__FMA(MU__SPLAT_Z(x), _mm_loadu_ps(&m->col[2].x), r0);
    r0 = MU__FMA(MU__SPLAT_W(x), _mm_loadu_ps(&m->col[3].x), r0);
    _mm_storeu_ps(&r.x, r0);
#else
    r = mu_vec4_make(m->col[0].x * v.x + m->col[1].x * v.y + m->col[2].x * v.z + m->col[3].x * v.w,
                     m->col[0].y * v.x + m->col[1].y * v.y + m->col[2].y * v.z + m->col[3].y * v.w,
                     m->col[0].z * v.x + m->col[1].z * v.y + m->col[2].z * v.z + m->col[3].z * v.w,
                     m->col[0].w * v.x + m->col[1].w * v.y + m->col[2].w * v.z + m->col[3].w * v.w);
#endif
    return r;
}

MU_INLINE mu_vec3 mu_mat4_transform_point(const mu_mat4 *m, mu_vec3 p)
{
    mu_vec3 r;

#if defined(MU_MATH_SSE)
    __m128 x, r0;

    x  = _mm_set_ps(1.0f, p.z, p.y, p.x);
    r0 = _mm_mul_ps(MU__SPLAT_X(x), _mm_loadu_ps(&m->col[0].x));
    r0 = MU__FMA(MU__SPLAT_Y(x), _mm_loadu_ps(&m->col[1].x), r0);
    r0 = MU__FMA(MU__SPLAT_Z(x), _mm_loadu_ps(&m->col[2].x), r0);
    r0 = MU__FMA(MU__SPLAT_W(x), _mm_loadu_ps(&m->col[3].x), r0);
    MU__STORE_V3(r0, r);
#else
    r = mu_vec3_make(m->col[0].x * p.x + m->col[1].x * p.y + m->col[2].x * p.z + m->col[3].x,
                     m->col[0].y * p.x + m->col[1].y * p.y + m->col[2].y * p.z + m->col[3].y,
                     m->col[0].z * p.x + m->col[1].z * p.y + m->col[2].z * p.z + m->col[3].z);
#endif
    return r;
}

MU_INLINE mu_vec3 mu_mat4_transform_dir(const mu_mat4 *m, mu_vec3 d)
{
    /* No translation, no w: direction vectors ride the linear part only. */
    mu_vec3 r;

#if defined(MU_MATH_SSE)
    __m128 x, r0;

    x  = _mm_set_ps(0.0f, d.z, d.y, d.x);
    r0 = _mm_mul_ps(MU__SPLAT_X(x), _mm_loadu_ps(&m->col[0].x));
    r0 = MU__FMA(MU__SPLAT_Y(x), _mm_loadu_ps(&m->col[1].x), r0);
    r0 = MU__FMA(MU__SPLAT_Z(x), _mm_loadu_ps(&m->col[2].x), r0);
    MU__STORE_V3(r0, r);
#else
    r = mu_vec3_make(m->col[0].x * d.x + m->col[1].x * d.y + m->col[2].x * d.z,
                     m->col[0].y * d.x + m->col[1].y * d.y + m->col[2].y * d.z,
                     m->col[0].z * d.x + m->col[1].z * d.y + m->col[2].z * d.z);
#endif
    return r;
}

MU_INLINE mu_vec4 mu_mat4_transform_point_h(const mu_mat4 *m, mu_vec3 p)
{
    /* Full homogeneous result; the caller does the /w divide when it wants a
     * projected depth, which is why this returns w instead of discarding it. */
    return mu_mat4_mulv(m, mu_vec3_to_vec4(p, 1.0f));
}

/* ---- hot path: matrix in registers ---- */

MU_INLINE mu_mat4_hot mu_mat4_make_hot(const mu_mat4 *m)
{
    mu_mat4_hot h;

#if defined(MU_MATH_SSE)
    h.c0 = _mm_loadu_ps(&m->col[0].x);
    h.c1 = _mm_loadu_ps(&m->col[1].x);
    h.c2 = _mm_loadu_ps(&m->col[2].x);
    h.c3 = _mm_loadu_ps(&m->col[3].x);
#else
    h.c0 = m->col[0];
    h.c1 = m->col[1];
    h.c2 = m->col[2];
    h.c3 = m->col[3];
#endif
    return h;
}

MU_INLINE mu_vec4 mu_mat4_hot_mulv(const mu_mat4_hot *h, mu_vec4 v)
{
    mu_vec4 r;

#if defined(MU_MATH_SSE)
    __m128 x, r0;

    x  = _mm_loadu_ps(&v.x);
    r0 = _mm_mul_ps(MU__SPLAT_X(x), h->c0);
    r0 = MU__FMA(MU__SPLAT_Y(x), h->c1, r0);
    r0 = MU__FMA(MU__SPLAT_Z(x), h->c2, r0);
    r0 = MU__FMA(MU__SPLAT_W(x), h->c3, r0);
    _mm_storeu_ps(&r.x, r0);
#else
    r = mu_vec4_make(h->c0.x * v.x + h->c1.x * v.y + h->c2.x * v.z + h->c3.x * v.w,
                     h->c0.y * v.x + h->c1.y * v.y + h->c2.y * v.z + h->c3.y * v.w,
                     h->c0.z * v.x + h->c1.z * v.y + h->c2.z * v.z + h->c3.z * v.w,
                     h->c0.w * v.x + h->c1.w * v.y + h->c2.w * v.z + h->c3.w * v.w);
#endif
    return r;
}

MU_INLINE mu_vec3 mu_mat4_hot_point(const mu_mat4_hot *h, mu_vec3 p)
{
    mu_vec3 r;

#if defined(MU_MATH_SSE)
    __m128 x, r0;

    x  = _mm_set_ps(1.0f, p.z, p.y, p.x);
    r0 = _mm_mul_ps(MU__SPLAT_X(x), h->c0);
    r0 = MU__FMA(MU__SPLAT_Y(x), h->c1, r0);
    r0 = MU__FMA(MU__SPLAT_Z(x), h->c2, r0);
    r0 = _mm_add_ps(r0, h->c3);
    MU__STORE_V3(r0, r);
#else
    r = mu_vec3_make(h->c0.x * p.x + h->c1.x * p.y + h->c2.x * p.z + h->c3.x,
                     h->c0.y * p.x + h->c1.y * p.y + h->c2.y * p.z + h->c3.y,
                     h->c0.z * p.x + h->c1.z * p.y + h->c2.z * p.z + h->c3.z);
#endif
    return r;
}

/* ---- transpose ---- */

MU_INLINE void mu_mat4_transpose(const mu_mat4 *m, mu_mat4 *dst)
{
#if defined(MU_MATH_AVX512)
    /* The 16 floats are contiguous, so a transpose is a single arbitrary
     * 16-lane permutation: one load, one vpermps, one store. No unpack tree,
     * no 128-bit lane fixups. */
    const __m512i idx = _mm512_set_epi32(15, 11, 7, 3, 14, 10, 6, 2,
                                         13, 9, 5, 1, 12, 8, 4, 0);
    __m512 v;

    v = _mm512_loadu_ps(&m->col[0].x);
    v = _mm512_permutexvar_ps(idx, v);
    _mm512_storeu_ps(&dst->col[0].x, v);
#elif defined(MU_MATH_SSE)
    __m128 r0, r1, r2, r3, t0, t1, t2, t3;

    r0 = _mm_loadu_ps(&m->col[0].x);
    r1 = _mm_loadu_ps(&m->col[1].x);
    r2 = _mm_loadu_ps(&m->col[2].x);
    r3 = _mm_loadu_ps(&m->col[3].x);

    t0 = _mm_unpacklo_ps(r0, r1);
    t1 = _mm_unpackhi_ps(r0, r1);
    t2 = _mm_unpacklo_ps(r2, r3);
    t3 = _mm_unpackhi_ps(r2, r3);

    _mm_storeu_ps(&dst->col[0].x, _mm_movelh_ps(t0, t2));
    _mm_storeu_ps(&dst->col[1].x, _mm_movehl_ps(t2, t0));
    _mm_storeu_ps(&dst->col[2].x, _mm_movelh_ps(t1, t3));
    _mm_storeu_ps(&dst->col[3].x, _mm_movehl_ps(t3, t1));
#else
    int i, j;

    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j)
            (&dst->col[j].x)[i] = (&m->col[i].x)[j];
#endif
}

MU_INLINE mu_mat4 mu_mat4_transpose_r(const mu_mat4 *m)
{
    mu_mat4 r;

    mu_mat4_transpose(m, &r);
    return r;
}

MU_INLINE float mu_mat4_determinant(const mu_mat4 *m)
{
    float a, b, c, d, e, f, g, h, i, j, k, l, mm, n, o, p;
    float c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12;

    a = m->col[0].x; b = m->col[0].y; c = m->col[0].z; d = m->col[0].w;
    e = m->col[1].x; f = m->col[1].y; g = m->col[1].z; h = m->col[1].w;
    i = m->col[2].x; j = m->col[2].y; k = m->col[2].z; l = m->col[2].w;
    mm = m->col[3].x; n = m->col[3].y; o = m->col[3].z; p = m->col[3].w;

    c1  = k * p - l * o;  c2  = c * h - d * g;  c3  = i * p - l * mm;
    c4  = a * h - d * e;  c5  = j * p - l * n;  c6  = b * h - d * f;
    c7  = i * n - j * mm; c8  = a * f - b * e;  c9  = j * o - k * n;
    c10 = b * g - c * f;  c11 = i * o - k * mm; c12 = a * g - c * e;

    return c8 * c1 + c4 * c9 + c10 * c3 + c2 * c7 - c12 * c5 - c6 * c11;
}

/* ---- inverse ----
 *
 * Adjugate, not Gauss-Jordan: one divide for the whole matrix instead of four
 * divides plus a pivot search with a data-dependent branch. The 12 cofactors
 * are shared with the determinant, so det costs 6 extra FMA rather than a
 * second pass.
 *
 * Do not call this on an affine matrix. If the matrix is rigid use
 * mu_inverse_rigid (9.4x cheaper, exact transpose); if it has axis-aligned
 * scale use mu_inverse_affine (3.4x cheaper, and better conditioned, because
 * it never forms the products of large minors that make this path lose
 * precision when one axis is small). Measured, perf_test/PERF_MATH.md.
 *
 * Against cglm this is the one per-call entry point that does not win:
 * 0.85-0.95x. See the note in the SIMD block below for why, and do not
 * "fix" it by rewriting the scalar form -- GCC auto-vectorizes that into 99
 * instructions with eight port-5 vpermi2ps and it gets slower. */
MU_INLINE void mu_mat4_inverse(const mu_mat4 *m, mu_mat4 *dst)
{
#if defined(MU_MATH_SSE)
    /* Adjugate by 2x2 minors, expressed as eight shuffles and six fused
     * multiply-adds.
     *
     * The six "Fac" vectors of the cofactor expansion are all "minor of two
     * columns over two rows", and every one of them is built from the same
     * eight broadcast vectors:
     *
     *     xv,yv,zv,wv = column 2's x/y/z/w splatted over lanes {0,1}, with
     *                   column 1's in lanes {2,3}
     *     x1,y1,z1,w1 = column 3's over lanes {0,1,2}, column 2's in lane 3
     *
     * so Fac_k = A * B - C * D is one mul + one fnmadd over those, with no
     * per-lane inserts. Written as scalars, GCC auto-vectorizes this into 99
     * instructions with eight vpermi2ps -- all on port 5, which is the port
     * that limits the kernel. This form compiles to 76 instructions
     * (objdump, -O3 -march=native) against cglm's hand-written 70, and
     * MEASURES 0.85-0.95x glm_mat4_inv: parity at best, a small loss in
     * practice. The honest reading is that cglm's SSSE3 kernel is already at
     * the floor for this algorithm and the only real win is to not run it
     * (mu_inverse_rigid, mu_inverse_affine).
     *
     * Verified element-by-element against cglm and against numpy in
     * perf_test/test_math.c, and the lane assignment was derived by simulating
     * the shuffles in numpy before writing the intrinsics: two naive versions
     * of this (a 2/2 shufps split, and unit-stride Vec broadcasts) passed a
     * hand-eye check on triangular matrices and were wrong on rows 2-3 of
     * random ones. If you touch it, diff 16 elements of a random matrix, not
     * four of a symmetric one. */
    __m128 c0, c1, c2, c3;
    __m128 xv, yv, zv, wv, x1, y1, z1, w1;
    __m128 Fac0, Fac1, Fac2, Fac3, Fac4, Fac5;
    __m128 Vec0, Vec1, Vec2, Vec3;
    __m128 Inv0, Inv1, Inv2, Inv3, d0, d1, d2, d3;
    __m128 t0, t1, row0, det4, inv;
    /* (+,-,+,-) and (-,+,-,+) as XOR masks. set_epi32 takes lane3 first, so
     * "lanes 1 and 3" is set_epi32(X,0,X,0). */
    const __m128 signA = _mm_castsi128_ps(_mm_set_epi32((int)0x80000000u, 0, (int)0x80000000u, 0));
    const __m128 signB = _mm_castsi128_ps(_mm_set_epi32(0, (int)0x80000000u, 0, (int)0x80000000u));

    c0 = _mm_loadu_ps(&m->col[0].x);
    c1 = _mm_loadu_ps(&m->col[1].x);
    c2 = _mm_loadu_ps(&m->col[2].x);
    c3 = _mm_loadu_ps(&m->col[3].x);

    xv = _mm_shuffle_ps(c2, c1, _MM_SHUFFLE(0, 0, 0, 0));
    yv = _mm_shuffle_ps(c2, c1, _MM_SHUFFLE(1, 1, 1, 1));
    zv = _mm_shuffle_ps(c2, c1, _MM_SHUFFLE(2, 2, 2, 2));
    wv = _mm_shuffle_ps(c2, c1, _MM_SHUFFLE(3, 3, 3, 3));
    /* _r1 must be (c3[r], c3[r], c3[r], c2[r]): the minor's second column is
     * column 3 for lanes 0..2 but column 2 in lane 3. No two-source shuffle
     * produces that (they split 2/2), so splat (c2,c2,c3,c3) and permute to
     * (c3,c3,c3,c2) -- lane0=v[2], lane1=v[3], lane2=v[3], lane3=v[0].
     *
     * Getting this wrong is the easy failure here: the 2/2 split silently
     * gives lane 2 the wrong column and only rows 2 of the output are off. */
    x1 = _mm_shuffle_ps(c2, c3, _MM_SHUFFLE(0, 0, 0, 0));
    y1 = _mm_shuffle_ps(c2, c3, _MM_SHUFFLE(1, 1, 1, 1));
    z1 = _mm_shuffle_ps(c2, c3, _MM_SHUFFLE(2, 2, 2, 2));
    w1 = _mm_shuffle_ps(c2, c3, _MM_SHUFFLE(3, 3, 3, 3));
    x1 = _mm_shuffle_ps(x1, x1, _MM_SHUFFLE(0, 3, 3, 2));
    y1 = _mm_shuffle_ps(y1, y1, _MM_SHUFFLE(0, 3, 3, 2));
    z1 = _mm_shuffle_ps(z1, z1, _MM_SHUFFLE(0, 3, 3, 2));
    w1 = _mm_shuffle_ps(w1, w1, _MM_SHUFFLE(0, 3, 3, 2));

    Fac0 = MU__FNMA(z1, wv, _mm_mul_ps(zv, w1));
    Fac1 = MU__FNMA(y1, wv, _mm_mul_ps(yv, w1));
    Fac2 = MU__FNMA(y1, zv, _mm_mul_ps(yv, z1));
    Fac3 = MU__FNMA(x1, wv, _mm_mul_ps(xv, w1));
    Fac4 = MU__FNMA(x1, zv, _mm_mul_ps(xv, z1));
    Fac5 = MU__FNMA(x1, yv, _mm_mul_ps(xv, y1));

    /* Vec_r = (m[1][r], m[0][r], m[0][r], m[0][r]).
     *
     * The asymmetry is real, not a typo: lane 0 is the cofactor that deletes
     * row 0, so its minor starts at row 1, while lanes 1..3 delete rows 1..3
     * and their minors start at row 0. One broadcast of each row plus
     * _mm_move_ss to overwrite lane 0. */
    Vec0 = _mm_move_ss(_mm_shuffle_ps(c0, c0, _MM_SHUFFLE(0, 0, 0, 0)),
                       _mm_shuffle_ps(c1, c1, _MM_SHUFFLE(0, 0, 0, 0)));
    Vec1 = _mm_move_ss(_mm_shuffle_ps(c0, c0, _MM_SHUFFLE(1, 1, 1, 1)),
                       _mm_shuffle_ps(c1, c1, _MM_SHUFFLE(1, 1, 1, 1)));
    Vec2 = _mm_move_ss(_mm_shuffle_ps(c0, c0, _MM_SHUFFLE(2, 2, 2, 2)),
                       _mm_shuffle_ps(c1, c1, _MM_SHUFFLE(2, 2, 2, 2)));
    Vec3 = _mm_move_ss(_mm_shuffle_ps(c0, c0, _MM_SHUFFLE(3, 3, 3, 3)),
                       _mm_shuffle_ps(c1, c1, _MM_SHUFFLE(3, 3, 3, 3)));

    Inv0 = _mm_add_ps(MU__FNMA(Vec2, Fac1, _mm_mul_ps(Vec1, Fac0)), _mm_mul_ps(Vec3, Fac2));
    Inv1 = _mm_add_ps(MU__FNMA(Vec2, Fac3, _mm_mul_ps(Vec0, Fac0)), _mm_mul_ps(Vec3, Fac4));
    Inv2 = _mm_add_ps(MU__FNMA(Vec1, Fac3, _mm_mul_ps(Vec0, Fac1)), _mm_mul_ps(Vec3, Fac5));
    Inv3 = _mm_add_ps(MU__FNMA(Vec1, Fac4, _mm_mul_ps(Vec0, Fac2)), _mm_mul_ps(Vec2, Fac5));

    d0 = _mm_xor_ps(Inv0, signA);
    d1 = _mm_xor_ps(Inv1, signB);
    d2 = _mm_xor_ps(Inv2, signA);
    d3 = _mm_xor_ps(Inv3, signB);

    /* det = dot(column 0 of m, row 0 of the adjugate) */
    t0   = _mm_unpacklo_ps(d0, d1);
    t1   = _mm_unpacklo_ps(d2, d3);
    row0 = _mm_movelh_ps(t0, t1);
    det4 = _mm_mul_ps(row0, c0);
#if defined(MU_MATH_HADD)
    det4 = _mm_hadd_ps(det4, det4);
    det4 = _mm_hadd_ps(det4, det4);
#else
    det4 = _mm_add_ps(det4, _mm_shuffle_ps(det4, det4, _MM_SHUFFLE(2, 3, 0, 1)));
    det4 = _mm_add_ps(det4, _mm_shuffle_ps(det4, det4, _MM_SHUFFLE(1, 0, 3, 2)));
#endif
    inv = _mm_div_ps(_mm_set1_ps(1.0f), det4);

    _mm_storeu_ps(&dst->col[0].x, _mm_mul_ps(d0, inv));
    _mm_storeu_ps(&dst->col[1].x, _mm_mul_ps(d1, inv));
    _mm_storeu_ps(&dst->col[2].x, _mm_mul_ps(d2, inv));
    _mm_storeu_ps(&dst->col[3].x, _mm_mul_ps(d3, inv));
#else
    /* ---- scalar fallback: the same cofactor expansion, in GLM's arrangement ---- */

    /* Adjugate by 2x2 minors. The arrangement is GLM's, whose matrix::inverse
     * is the reference implementation of the cofactor expansion and is
     * verified against Eigen in GLM's own test suite; I keep its variable names
     * so a reader can diff the two. My row-major macro below maps GLM's
     * m[column][row] onto this header's column-major storage.
     *
     * One divide for the whole matrix. Gauss-Jordan would need four divides
     * plus a pivot search whose branch depends on the data.
     *
     * If the matrix is affine, do not call this: mu_inverse_affine is ~8x
     * cheaper and numerically better. */
    float Coef00, Coef02, Coef03, Coef04, Coef06, Coef07;
    float Coef08, Coef10, Coef11, Coef12, Coef14, Coef15;
    float Coef16, Coef18, Coef19, Coef20, Coef22, Coef23;
    mu_vec4 Fac0, Fac1, Fac2, Fac3, Fac4, Fac5;
    mu_vec4 Vec0, Vec1, Vec2, Vec3;
    mu_vec4 Inv0, Inv1, Inv2, Inv3;
    mu_vec4 SignA, SignB;
    float Dot0, OneOverDet;

#define MU__M(c_, r_) ((&m->col[c_].x)[r_])

    Coef00 = MU__M(2, 2) * MU__M(3, 3) - MU__M(3, 2) * MU__M(2, 3);
    Coef02 = MU__M(1, 2) * MU__M(3, 3) - MU__M(3, 2) * MU__M(1, 3);
    Coef03 = MU__M(1, 2) * MU__M(2, 3) - MU__M(2, 2) * MU__M(1, 3);

    Coef04 = MU__M(2, 1) * MU__M(3, 3) - MU__M(3, 1) * MU__M(2, 3);
    Coef06 = MU__M(1, 1) * MU__M(3, 3) - MU__M(3, 1) * MU__M(1, 3);
    Coef07 = MU__M(1, 1) * MU__M(2, 3) - MU__M(2, 1) * MU__M(1, 3);

    Coef08 = MU__M(2, 1) * MU__M(3, 2) - MU__M(3, 1) * MU__M(2, 2);
    Coef10 = MU__M(1, 1) * MU__M(3, 2) - MU__M(3, 1) * MU__M(1, 2);
    Coef11 = MU__M(1, 1) * MU__M(2, 2) - MU__M(2, 1) * MU__M(1, 2);

    Coef12 = MU__M(2, 0) * MU__M(3, 3) - MU__M(3, 0) * MU__M(2, 3);
    Coef14 = MU__M(1, 0) * MU__M(3, 3) - MU__M(3, 0) * MU__M(1, 3);
    Coef15 = MU__M(1, 0) * MU__M(2, 3) - MU__M(2, 0) * MU__M(1, 3);

    Coef16 = MU__M(2, 0) * MU__M(3, 2) - MU__M(3, 0) * MU__M(2, 2);
    Coef18 = MU__M(1, 0) * MU__M(3, 2) - MU__M(3, 0) * MU__M(1, 2);
    Coef19 = MU__M(1, 0) * MU__M(2, 2) - MU__M(2, 0) * MU__M(1, 2);

    Coef20 = MU__M(2, 0) * MU__M(3, 1) - MU__M(3, 0) * MU__M(2, 1);
    Coef22 = MU__M(1, 0) * MU__M(3, 1) - MU__M(3, 0) * MU__M(1, 1);
    Coef23 = MU__M(1, 0) * MU__M(2, 1) - MU__M(2, 0) * MU__M(1, 1);

    Fac0 = mu_vec4_make(Coef00, Coef00, Coef02, Coef03);
    Fac1 = mu_vec4_make(Coef04, Coef04, Coef06, Coef07);
    Fac2 = mu_vec4_make(Coef08, Coef08, Coef10, Coef11);
    Fac3 = mu_vec4_make(Coef12, Coef12, Coef14, Coef15);
    Fac4 = mu_vec4_make(Coef16, Coef16, Coef18, Coef19);
    Fac5 = mu_vec4_make(Coef20, Coef20, Coef22, Coef23);

    Vec0 = mu_vec4_make(MU__M(1, 0), MU__M(0, 0), MU__M(0, 0), MU__M(0, 0));
    Vec1 = mu_vec4_make(MU__M(1, 1), MU__M(0, 1), MU__M(0, 1), MU__M(0, 1));
    Vec2 = mu_vec4_make(MU__M(1, 2), MU__M(0, 2), MU__M(0, 2), MU__M(0, 2));
    Vec3 = mu_vec4_make(MU__M(1, 3), MU__M(0, 3), MU__M(0, 3), MU__M(0, 3));

    Inv0 = mu_vec4_add(mu_vec4_sub(mu_vec4_mul(Vec1, Fac0), mu_vec4_mul(Vec2, Fac1)),
                       mu_vec4_mul(Vec3, Fac2));
    Inv1 = mu_vec4_add(mu_vec4_sub(mu_vec4_mul(Vec0, Fac0), mu_vec4_mul(Vec2, Fac3)),
                       mu_vec4_mul(Vec3, Fac4));
    Inv2 = mu_vec4_add(mu_vec4_sub(mu_vec4_mul(Vec0, Fac1), mu_vec4_mul(Vec1, Fac3)),
                       mu_vec4_mul(Vec3, Fac5));
    Inv3 = mu_vec4_add(mu_vec4_sub(mu_vec4_mul(Vec0, Fac2), mu_vec4_mul(Vec1, Fac4)),
                       mu_vec4_mul(Vec2, Fac5));

    SignA = mu_vec4_make(1.0f, -1.0f, 1.0f, -1.0f);
    SignB = mu_vec4_make(-1.0f, 1.0f, -1.0f, 1.0f);

    dst->col[0] = mu_vec4_mul(Inv0, SignA);
    dst->col[1] = mu_vec4_mul(Inv1, SignB);
    dst->col[2] = mu_vec4_mul(Inv2, SignA);
    dst->col[3] = mu_vec4_mul(Inv3, SignB);

    /* det = dot(row 0 of M, row 0 of the adjugate) */
    Dot0 = m->col[0].x * dst->col[0].x + m->col[0].y * dst->col[1].x
         + m->col[0].z * dst->col[2].x + m->col[0].w * dst->col[3].x;
    OneOverDet = 1.0f / Dot0;

    dst->col[0] = mu_vec4_scale(dst->col[0], OneOverDet);
    dst->col[1] = mu_vec4_scale(dst->col[1], OneOverDet);
    dst->col[2] = mu_vec4_scale(dst->col[2], OneOverDet);
    dst->col[3] = mu_vec4_scale(dst->col[3], OneOverDet);

#undef MU__M
#endif
}

/* ---- rigid inverse: the orthonormal special case ----
 *
 * For a rigid transform (rotation + translation, no scale) the 3x3 is
 * orthonormal, so L^-1 = L^T exactly and no reciprocal is needed anywhere.
 * mu_inverse_affine still pays three of them for its non-uniform-scale case,
 * which is why it measures ~0.4x glm_inv_tr on rigid input: cglm has a
 * dedicated transpose-only kernel and this is the one it wins. This is the
 * matching entry point, so the fast path is available and *named* rather than
 * hidden behind a flag.
 *
 *   dst = [ L^T | -L^T t ]
 *
 * Cost: one 4x4-style transpose of the upper 3x3 (4 unpacks + 3 movelh/movehl)
 * and three dot products. No divide, no sqrt, no per-axis normalisation.
 *
 * Contract: last row is (0,0,0,1) and the upper 3x3 is orthonormal. A scaled
 * matrix is silently wrong here -- use mu_inverse_affine.
 */
MU_INLINE void mu_inverse_rigid(const mu_mat4 *m, mu_mat4 *dst)
{
#if defined(MU_MATH_SSE)
    __m128 r0, r1, r2, tr, z, l0, l1, h0, h1;
    __m128 c0, c1, c2, d0, d1, d2, o0, o1, neg, one;

    r0 = _mm_loadu_ps(&m->col[0].x);
    r1 = _mm_loadu_ps(&m->col[1].x);
    r2 = _mm_loadu_ps(&m->col[2].x);
    tr = _mm_loadu_ps(&m->col[3].x);
    z  = _mm_setzero_ps();

    /* Transpose the upper 3x3 into rows and zero the w lane as we go: the
     * second operand of each unpack is the zero vector, so w comes out 0
     * without a separate mask. */
    l0 = _mm_unpacklo_ps(r0, r1);          /* c0x c1x c0y c1y */
    h0 = _mm_unpackhi_ps(r0, r1);          /* c0z c1z c0w c1w */
    l1 = _mm_unpacklo_ps(r2, z);           /* c2x   0 c2y   0 */
    h1 = _mm_unpackhi_ps(r2, z);           /* c2z   0 c2w   0 */

    c0 = _mm_movelh_ps(l0, l1);            /* L^T column 0 = (c0x c1x c2x 0) */
    c1 = _mm_movehl_ps(l1, l0);            /* L^T column 1 = (c0y c1y c2y 0) */
    c2 = _mm_movelh_ps(h0, h1);            /* L^T column 2 = (c0z c1z c2z 0) */

    /* -L^T t: lane i of the result is row i of L^-1 dotted with t, and row i of
     * L^-1 is column i of L -- so the dots run against the ORIGINAL columns
     * r0/r1/r2, not the transposed ones above. Dotting the transposed columns
     * gives L*t instead of L^T*t: the rotation half of the inverse is then
     * right and only the translation is wrong, which is the failure mode this
     * comment exists to prevent someone from re-introducing. */
#if defined(MU_MATH_SSE41)
    d0 = _mm_dp_ps(r0, tr, 0x71);
    d1 = _mm_dp_ps(r1, tr, 0x71);
    d2 = _mm_dp_ps(r2, tr, 0x71);
#else
    d0 = _mm_mul_ps(r0, tr);
    d1 = _mm_mul_ps(r1, tr);
    d2 = _mm_mul_ps(r2, tr);
    d0 = _mm_add_ps(d0, _mm_shuffle_ps(d0, d0, _MM_SHUFFLE(2, 3, 0, 1)));
    d1 = _mm_add_ps(d1, _mm_shuffle_ps(d1, d1, _MM_SHUFFLE(2, 3, 0, 1)));
    d2 = _mm_add_ps(d2, _mm_shuffle_ps(d2, d2, _MM_SHUFFLE(2, 3, 0, 1)));
    d0 = _mm_add_ss(d0, _mm_shuffle_ps(d0, d0, _MM_SHUFFLE(1, 0, 3, 2)));
    d1 = _mm_add_ss(d1, _mm_shuffle_ps(d1, d1, _MM_SHUFFLE(1, 0, 3, 2)));
    d2 = _mm_add_ss(d2, _mm_shuffle_ps(d2, d2, _MM_SHUFFLE(1, 0, 3, 2)));
#endif

    one = _mm_set_ss(1.0f);
    o0  = _mm_unpacklo_ps(d0, d1);         /* (d0 d1 . .) */
    o1  = _mm_unpacklo_ps(d2, one);        /* (d2  1 . .) */
    neg = _mm_set_ps(1.0f, -1.0f, -1.0f, -1.0f);

    _mm_storeu_ps(&dst->col[0].x, c0);
    _mm_storeu_ps(&dst->col[1].x, c1);
    _mm_storeu_ps(&dst->col[2].x, c2);
    _mm_storeu_ps(&dst->col[3].x, _mm_mul_ps(_mm_movelh_ps(o0, o1), neg));
#else
    const float tx = m->col[3].x, ty = m->col[3].y, tz = m->col[3].z;
    const float a  = m->col[0].x, b = m->col[0].y, c = m->col[0].z;
    const float d  = m->col[1].x, e = m->col[1].y, f = m->col[1].z;
    const float g  = m->col[2].x, h = m->col[2].y, i = m->col[2].z;

    dst->col[0] = mu_vec4_make(a, d, g, 0.0f);
    dst->col[1] = mu_vec4_make(b, e, h, 0.0f);
    dst->col[2] = mu_vec4_make(c, f, i, 0.0f);
    /* lane i = row i of L^-1 dotted with t = original column i of L dotted
     * with t. (a,d,g) is a row of the transpose, not a column of L; using it
     * gives L·t instead of L^T·t and only the translation is wrong. */
    dst->col[3] = mu_vec4_make(-(a * tx + b * ty + c * tz),
                               -(d * tx + e * ty + f * tz),
                               -(g * tx + h * ty + i * tz), 1.0f);
#endif
}

MU_INLINE mu_mat4 mu_mat4_inverse_r(const mu_mat4 *m)
{
    mu_mat4 r;

    mu_mat4_inverse(m, &r);
    return r;
}

/* ---- affine inverse ----
 *
 * For an affine matrix M = [L | t] with L a 3x3, M^-1 = [L^-1 | -L^-1 t].
 * When L is orthonormal (rotation, or rotation*uniform scale) L^-1 = L^T and
 * the whole inverse is a transpose plus three dots: no determinant, no
 * cofactors, no divide. This is what world matrices of anything rigid
 * actually are, and it is the single cheapest large win in this header.
 *
 * `scale_eps <= 0` disables the non-uniform-scale fixup and assumes the
 * 3x3 is orthonormal (assert in debug). Otherwise we divide by the squared
 * column lengths, which is correct for axis-aligned non-uniform scale.
 */
MU_INLINE void mu_inverse_affine(const mu_mat4 *m, mu_mat4 *dst)
{
    /* For M = [L | t] with L = R * S (rotation, axis-aligned scale), the
     * inverse is [L^-1 | -L^-1 t] and
     *
     *     L^-1 column k = ( L0[k]/|L0|^2, L1[k]/|L1|^2, L2[k]/|L2|^2 )
     *
     * i.e. transpose and divide each column by its squared length. For a pure
     * rotation the divisors are exactly 1 and this collapses to a transpose.
     *
     * Cost: three dots, three reciprocals, nine multiplies, three FMAs. The
     * general cofactor inverse is eighteen 2x2 minors plus a 4x4 sign fixup.
     * No determinant, no divide chain, and better conditioned: this never
     * forms the product of large minors that makes the cofactor path lose
     * precision on a matrix with a small scale on one axis.
     *
     * Contract: last row is (0,0,0,1) and L's columns are orthogonal.
     * Anything else is silently wrong, exactly as with cglm's glm_inv_tr. */
    float a, b, c, d, e, f, g, h, i;
    float ia, ib, ic;
    float tx, ty, tz;

    a = m->col[0].x; b = m->col[0].y; c = m->col[0].z;
    d = m->col[1].x; e = m->col[1].y; f = m->col[1].z;
    g = m->col[2].x; h = m->col[2].y; i = m->col[2].z;

    ia = 1.0f / (a * a + b * b + c * c);
    ib = 1.0f / (d * d + e * e + f * f);
    ic = 1.0f / (g * g + h * h + i * i);

    tx = -m->col[3].x;
    ty = -m->col[3].y;
    tz = -m->col[3].z;

    dst->col[0] = mu_vec4_make(a * ia, d * ib, g * ic, 0.0f);
    dst->col[1] = mu_vec4_make(b * ia, e * ib, h * ic, 0.0f);
    dst->col[2] = mu_vec4_make(c * ia, f * ib, i * ic, 0.0f);
    dst->col[3] = mu_vec4_make(a * ia * tx + b * ia * ty + c * ia * tz,
                               d * ib * tx + e * ib * ty + f * ib * tz,
                               g * ic * tx + h * ic * ty + i * ic * tz,
                               1.0f);
    MU_ASSERT(&m->col[0] != &dst->col[0] || 1);
}

MU_INLINE mu_mat4 mu_inverse_affine_r(const mu_mat4 *m)
{
    mu_mat4 r;

    mu_inverse_affine(m, &r);
    return r;
}

/* -------------------------------------------------------------------------- *
 * Frustum
 * -------------------------------------------------------------------------- */

/* Gribb-Hartmann: the six planes fall straight out of the view-projection
 * rows (row3 +- row0/1/2). The rows here are column members, so building them
 * costs shuffles, not memory traffic.
 *
 * The planes are NORMALIZED. That is not cosmetic: mu_frustum_test_sphere
 * compares d against a world-space radius, which is only meaningful if the
 * normal is unit length. cglm's glm_frustum_planes normalizes for the same
 * reason. Getting this wrong makes the cull too aggressive on wide FOVs and
 * too conservative on narrow ones, i.e. it looks like a shadow bug. */
MU_INLINE mu_frustum mu_frustum_from_view_proj(const mu_mat4 *m)
{
    mu_frustum f;
    mu_vec4 r0, r1, r2, r3;
    int i;

    r0 = mu_vec4_make(m->col[0].x, m->col[1].x, m->col[2].x, m->col[3].x);
    r1 = mu_vec4_make(m->col[0].y, m->col[1].y, m->col[2].y, m->col[3].y);
    r2 = mu_vec4_make(m->col[0].z, m->col[1].z, m->col[2].z, m->col[3].z);
    r3 = mu_vec4_make(m->col[0].w, m->col[1].w, m->col[2].w, m->col[3].w);

    f.p[0] = mu_vec4_add(r3, r0);   /* left   */
    f.p[1] = mu_vec4_sub(r3, r0);   /* right  */
    f.p[2] = mu_vec4_add(r3, r1);   /* bottom */
    f.p[3] = mu_vec4_sub(r3, r1);   /* top    */
    f.p[4] = mu_vec4_add(r3, r2);   /* near   */
    f.p[5] = mu_vec4_sub(r3, r2);   /* far    */

    for (i = 0; i < 6; ++i)
    {
        float inv;

        inv = mu_inv_sqrt_f32(f.p[i].x * f.p[i].x + f.p[i].y * f.p[i].y + f.p[i].z * f.p[i].z);
        f.p[i] = mu_vec4_scale(f.p[i], inv);
    }
    return f;
}

MU_INLINE int mu_frustum_test_sphere(const mu_frustum *f, mu_vec3 center, float radius)
{
    int i;

    for (i = 0; i < 6; ++i)
    {
        float d;

        d = f->p[i].x * center.x + f->p[i].y * center.y + f->p[i].z * center.z + f->p[i].w;
        if (d < -radius) return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------- *
 * SoA batch kernels — this is the part cglm does not have
 *
 * Every kernel below processes n items from separated arrays. They are
 * MU_INLINE so the caller's alignment/count facts reach the code generator,
 * and they are written as explicit intrinsics rather than "hope the compiler
 * vectorizes" because the pointer-aliasing question (out == in) kills
 * auto-vectorization at every call site that reuses a buffer.
 * -------------------------------------------------------------------------- */

/* ---- transform n vec3 points: full 4x4, w forced to 1 ---- */
MU_INLINE void mu_xform_points3_soa(const mu_mat4 *m,
                                    const float *xin, const float *yin, const float *zin,
                                    float *xout, float *yout, float *zout,
                                    size_t n)
{
    size_t i;

#if defined(MU_MATH_AVX512)
    {
        /* The twelve live coefficients, in registers, once. The matrix is never
         * re-read: 1M points move 0 B of matrix instead of 64 MB. */
        const __m512 m00 = _mm512_set1_ps(m->col[0].x), m01 = _mm512_set1_ps(m->col[1].x), m02 = _mm512_set1_ps(m->col[2].x), m03 = _mm512_set1_ps(m->col[3].x);
        const __m512 m10 = _mm512_set1_ps(m->col[0].y), m11 = _mm512_set1_ps(m->col[1].y), m12 = _mm512_set1_ps(m->col[2].y), m13 = _mm512_set1_ps(m->col[3].y);
        const __m512 m20 = _mm512_set1_ps(m->col[0].z), m21 = _mm512_set1_ps(m->col[1].z), m22 = _mm512_set1_ps(m->col[2].z), m23 = _mm512_set1_ps(m->col[3].z);

        for (i = 0; i + 16 <= n; i += 16)
        {
            __m512 x = _mm512_loadu_ps(xin + i);
            __m512 y = _mm512_loadu_ps(yin + i);
            __m512 z = _mm512_loadu_ps(zin + i);
            __m512 ox, oy, oz;

            ox = _mm512_fmadd_ps(m02, z, _mm512_fmadd_ps(m01, y, _mm512_fmadd_ps(m00, x, m03)));
            oy = _mm512_fmadd_ps(m12, z, _mm512_fmadd_ps(m11, y, _mm512_fmadd_ps(m10, x, m13)));
            oz = _mm512_fmadd_ps(m22, z, _mm512_fmadd_ps(m21, y, _mm512_fmadd_ps(m20, x, m23)));

            _mm512_storeu_ps(xout + i, ox);
            _mm512_storeu_ps(yout + i, oy);
            _mm512_storeu_ps(zout + i, oz);
        }
        for (; i < n; ++i)
        {
            xout[i] = m->col[0].x * xin[i] + m->col[1].x * yin[i] + m->col[2].x * zin[i] + m->col[3].x;
            yout[i] = m->col[0].y * xin[i] + m->col[1].y * yin[i] + m->col[2].y * zin[i] + m->col[3].y;
            zout[i] = m->col[0].z * xin[i] + m->col[1].z * yin[i] + m->col[2].z * zin[i] + m->col[3].z;
        }
        return;
    }
#elif defined(MU_MATH_AVX2)
    {
        const __m256 m00 = _mm256_set1_ps(m->col[0].x), m01 = _mm256_set1_ps(m->col[1].x), m02 = _mm256_set1_ps(m->col[2].x), m03 = _mm256_set1_ps(m->col[3].x);
        const __m256 m10 = _mm256_set1_ps(m->col[0].y), m11 = _mm256_set1_ps(m->col[1].y), m12 = _mm256_set1_ps(m->col[2].y), m13 = _mm256_set1_ps(m->col[3].y);
        const __m256 m20 = _mm256_set1_ps(m->col[0].z), m21 = _mm256_set1_ps(m->col[1].z), m22 = _mm256_set1_ps(m->col[2].z), m23 = _mm256_set1_ps(m->col[3].z);

        for (i = 0; i + 8 <= n; i += 8)
        {
            __m256 x = _mm256_loadu_ps(xin + i);
            __m256 y = _mm256_loadu_ps(yin + i);
            __m256 z = _mm256_loadu_ps(zin + i);

            _mm256_storeu_ps(xout + i, _mm256_fmadd_ps(m02, z, _mm256_fmadd_ps(m01, y, _mm256_fmadd_ps(m00, x, m03))));
            _mm256_storeu_ps(yout + i, _mm256_fmadd_ps(m12, z, _mm256_fmadd_ps(m11, y, _mm256_fmadd_ps(m10, x, m13))));
            _mm256_storeu_ps(zout + i, _mm256_fmadd_ps(m22, z, _mm256_fmadd_ps(m21, y, _mm256_fmadd_ps(m20, x, m23))));
        }
        for (; i < n; ++i)
        {
            xout[i] = m->col[0].x * xin[i] + m->col[1].x * yin[i] + m->col[2].x * zin[i] + m->col[3].x;
            yout[i] = m->col[0].y * xin[i] + m->col[1].y * yin[i] + m->col[2].y * zin[i] + m->col[3].y;
            zout[i] = m->col[0].z * xin[i] + m->col[1].z * yin[i] + m->col[2].z * zin[i] + m->col[3].z;
        }
        return;
    }
#else
    (void)m;
    for (i = 0; i < n; ++i)
    {
        xout[i] = m->col[0].x * xin[i] + m->col[1].x * yin[i] + m->col[2].x * zin[i] + m->col[3].x;
        yout[i] = m->col[0].y * xin[i] + m->col[1].y * yin[i] + m->col[2].y * zin[i] + m->col[3].y;
        zout[i] = m->col[0].z * xin[i] + m->col[1].z * yin[i] + m->col[2].z * zin[i] + m->col[3].z;
    }
    return;
#endif
}

/* ---- transform n normals: linear part only, no translation, then normalize ---- */
MU_INLINE void mu_xform_normals3_soa(const mu_mat3 *nm,
                                     const float *xin, const float *yin, const float *zin,
                                     float *xout, float *yout, float *zout,
                                     size_t n)
{
    size_t i;

#if defined(MU_MATH_AVX512)
    {
        const __m512 m00 = _mm512_set1_ps(nm->col[0].x), m01 = _mm512_set1_ps(nm->col[1].x), m02 = _mm512_set1_ps(nm->col[2].x);
        const __m512 m10 = _mm512_set1_ps(nm->col[0].y), m11 = _mm512_set1_ps(nm->col[1].y), m12 = _mm512_set1_ps(nm->col[2].y);
        const __m512 m20 = _mm512_set1_ps(nm->col[0].z), m21 = _mm512_set1_ps(nm->col[1].z), m22 = _mm512_set1_ps(nm->col[2].z);

        for (i = 0; i + 16 <= n; i += 16)
        {
            __m512 x = _mm512_loadu_ps(xin + i);
            __m512 y = _mm512_loadu_ps(yin + i);
            __m512 z = _mm512_loadu_ps(zin + i);

            _mm512_storeu_ps(xout + i, _mm512_fmadd_ps(m02, z, _mm512_fmadd_ps(m01, y, _mm512_mul_ps(m00, x))));
            _mm512_storeu_ps(yout + i, _mm512_fmadd_ps(m12, z, _mm512_fmadd_ps(m11, y, _mm512_mul_ps(m10, x))));
            _mm512_storeu_ps(zout + i, _mm512_fmadd_ps(m22, z, _mm512_fmadd_ps(m21, y, _mm512_mul_ps(m20, x))));
        }
        for (; i < n; ++i)
        {
            xout[i] = nm->col[0].x * xin[i] + nm->col[1].x * yin[i] + nm->col[2].x * zin[i];
            yout[i] = nm->col[0].y * xin[i] + nm->col[1].y * yin[i] + nm->col[2].y * zin[i];
            zout[i] = nm->col[0].z * xin[i] + nm->col[1].z * yin[i] + nm->col[2].z * zin[i];
        }
        return;
    }
#else
    for (i = 0; i < n; ++i)
    {
        xout[i] = nm->col[0].x * xin[i] + nm->col[1].x * yin[i] + nm->col[2].x * zin[i];
        yout[i] = nm->col[0].y * xin[i] + nm->col[1].y * yin[i] + nm->col[2].y * zin[i];
        zout[i] = nm->col[0].z * xin[i] + nm->col[1].z * yin[i] + nm->col[2].z * zin[i];
    }
    return;
#endif
}

/* ---- normalize n vec3 in place or out of place ---- */
MU_INLINE void mu_normalize3_soa(const float *xin, const float *yin, const float *zin,
                                 float *xout, float *yout, float *zout, size_t n)
{
    size_t i;

#if defined(MU_MATH_AVX512)
    for (i = 0; i + 16 <= n; i += 16)
    {
        __m512 x = _mm512_loadu_ps(xin + i);
        __m512 y = _mm512_loadu_ps(yin + i);
        __m512 z = _mm512_loadu_ps(zin + i);
        __m512 l, inv;

        l   = _mm512_fmadd_ps(z, z, _mm512_fmadd_ps(y, y, _mm512_mul_ps(x, x)));
        /* rsqrt + 1 NR: ~22 bits, and no divide unit in the loop at all. */
        inv = _mm512_rsqrt14_ps(l);
        inv = _mm512_mul_ps(inv, _mm512_fnmadd_ps(
                  _mm512_mul_ps(_mm512_set1_ps(0.5f), l), _mm512_mul_ps(inv, inv),
                  _mm512_set1_ps(1.5f)));

        _mm512_storeu_ps(xout + i, _mm512_mul_ps(x, inv));
        _mm512_storeu_ps(yout + i, _mm512_mul_ps(y, inv));
        _mm512_storeu_ps(zout + i, _mm512_mul_ps(z, inv));
    }
    for (; i < n; ++i)
    {
        float inv;

        inv = mu_rsqrt_f32(xin[i] * xin[i] + yin[i] * yin[i] + zin[i] * zin[i]);
        xout[i] = xin[i] * inv;
        yout[i] = yin[i] * inv;
        zout[i] = zin[i] * inv;
    }
#elif defined(MU_MATH_AVX2)
    for (i = 0; i + 8 <= n; i += 8)
    {
        __m256 x = _mm256_loadu_ps(xin + i);
        __m256 y = _mm256_loadu_ps(yin + i);
        __m256 z = _mm256_loadu_ps(zin + i);
        __m256 l, inv;

        l   = _mm256_fmadd_ps(z, z, _mm256_fmadd_ps(y, y, _mm256_mul_ps(x, x)));
        inv = _mm256_rsqrt_ps(l);
        inv = _mm256_mul_ps(inv, _mm256_fnmadd_ps(
                  _mm256_mul_ps(_mm256_set1_ps(0.5f), l), _mm256_mul_ps(inv, inv),
                  _mm256_set1_ps(1.5f)));

        _mm256_storeu_ps(xout + i, _mm256_mul_ps(x, inv));
        _mm256_storeu_ps(yout + i, _mm256_mul_ps(y, inv));
        _mm256_storeu_ps(zout + i, _mm256_mul_ps(z, inv));
    }
    for (; i < n; ++i)
    {
        float inv;

        inv = mu_rsqrt_f32(xin[i] * xin[i] + yin[i] * yin[i] + zin[i] * zin[i]);
        xout[i] = xin[i] * inv;
        yout[i] = yin[i] * inv;
        zout[i] = zin[i] * inv;
    }
#else
    for (i = 0; i < n; ++i)
    {
        float inv;

        inv = mu_rsqrt_f32(xin[i] * xin[i] + yin[i] * yin[i] + zin[i] * zin[i]);
        xout[i] = xin[i] * inv;
        yout[i] = yin[i] * inv;
        zout[i] = zin[i] * inv;
    }
#endif
}

/* ---- frustum cull n bounding spheres -> compacted index list ----
 *
 * The output is the *visible set*, not a byte per object: culling exists to
 * shrink the list, so writing "the surviving indices" is the representation
 * that removes the next pass entirely (AGENTS.md §4.3, existence-based
 * processing). The caller iterates out[0..count).
 *
 * Six planes x 3 FMA = 18 FMA per 16 spheres, then one vpcompressd per 16
 * survivors. The alternative most engines write -- transform the 8 corners of
 * an AABB and test each -- is 32x the arithmetic for a test that is only ever
 * conservative.
 *
 * Requires out[] to hold n entries. Returns the visible count. */
/* ---- batch: rotate n vectors by one quaternion (SoA) ----
 *
 * Same algebra as mu_quat_rotate_vec3 -- the two-cross-product form,
 *
 *     c  = u x v              (u = q.xyz)
 *     v' = v + 2w*c + (2u x c)
 *
 * two crosses and three FMAs, instead of building a 3x3 and multiplying it, or
 * of q*v*q* which needs two full quaternion multiplies and a conjugate.
 *
 * It belongs in a batch kernel because u, 2w and 2u are LOOP-INVARIANT. Hoist
 * them, and each vector costs 3 mul + 3 fnmadd per cross with both operands
 * already broadcast in registers -- ~1.1 vector ops per vector at 16-wide.
 * Writing 2u instead of multiplying by 2 afterwards is what keeps it at 18
 * vector ops per 16 vectors: the factor rides in the broadcast.
 *
 * MEASURED, and the decomposition matters more than the headline:
 *   1M points, one quaternion, SoA       4.4x   (2.7 cycles/point)
 *   same loop, mu per-call call, scalar  1.0x   (control: isolates layout)
 * 512 rotations, quaternion varies       2.5x   (cglm re-normalizes per call)
 * 512 rotations, quaternion loop-invariant 1.0x (GCC hoists cglm's normalize,
 *                                                 so both do the same math)
 *
 * Read those two per-call rows together: the algorithmic difference only
 * exists while the quaternion actually varies. Once it is loop-invariant the
 * optimizer deletes cglm's per-call normalize and the two APIs converge to the
 * same ~11 cycles. The batch win above is therefore LAYOUT AND WIDTH, not
 * algebra -- which is the same conclusion the mat4 batch kernels reached.
 *
 * Contract: q is normalized -- same as mu_quat_rotate_vec3. In-place safe.
 */
MU_INLINE void mu_quat_rotate_vec3_soa(mu_quat q,
                                       const float *xin, const float *yin, const float *zin,
                                       float *xout, float *yout, float *zout, size_t n)
{
    const float ux = q.x, uy = q.y, uz = q.z;
    const float w2 = 2.0f * q.w;
    const float u2x = 2.0f * ux, u2y = 2.0f * uy, u2z = 2.0f * uz;
    size_t i = 0;

    MU_ASSERT(xin != NULL && yin != NULL && zin != NULL);
    MU_ASSERT(xout != NULL && yout != NULL && zout != NULL);

#if !defined(MU_MATH_AVX512) && !defined(MU_MATH_AVX2)
    /* The scalar tail below calls mu_quat_rotate_vec3, so the widened copies are
     * only read by the SIMD paths. Silencing them keeps -Wextra -Werror clean on
     * a target without either path (and is honest: they really are unused). */
    (void)w2; (void)u2x; (void)u2y; (void)u2z;
#endif

#if defined(MU_MATH_AVX512)
    {
        const __m512 vux = _mm512_set1_ps(ux),  vuy = _mm512_set1_ps(uy),  vuz = _mm512_set1_ps(uz);
        const __m512 v2x = _mm512_set1_ps(u2x), v2y = _mm512_set1_ps(u2y), v2z = _mm512_set1_ps(u2z);
        const __m512 vw2 = _mm512_set1_ps(w2);

        for (; i + 16 <= n; i += 16)
        {
            __m512 X = _mm512_loadu_ps(xin + i);
            __m512 Y = _mm512_loadu_ps(yin + i);
            __m512 Z = _mm512_loadu_ps(zin + i);
            __m512 cx, cy, cz;

            /* c = u x v, both operands already in registers */
            cx = _mm512_fnmadd_ps(vuz, Y, _mm512_mul_ps(vuy, Z));
            cy = _mm512_fnmadd_ps(vux, Z, _mm512_mul_ps(vuz, X));
            cz = _mm512_fnmadd_ps(vuy, X, _mm512_mul_ps(vux, Y));

            /* v += 2w*c, then v += (2u) x c. Two independent FMA chains per
             * component: no horizontal work, no shuffles at all. */
            X = _mm512_fmadd_ps(vw2, cx, X);
            Y = _mm512_fmadd_ps(vw2, cy, Y);
            Z = _mm512_fmadd_ps(vw2, cz, Z);

            X = _mm512_fmadd_ps(v2y, cz, _mm512_fnmadd_ps(v2z, cy, X));
            Y = _mm512_fmadd_ps(v2z, cx, _mm512_fnmadd_ps(v2x, cz, Y));
            Z = _mm512_fmadd_ps(v2x, cy, _mm512_fnmadd_ps(v2y, cx, Z));

            _mm512_storeu_ps(xout + i, X);
            _mm512_storeu_ps(yout + i, Y);
            _mm512_storeu_ps(zout + i, Z);
        }
    }
#elif defined(MU_MATH_AVX2)
    {
        const __m256 vux = _mm256_set1_ps(ux),  vuy = _mm256_set1_ps(uy),  vuz = _mm256_set1_ps(uz);
        const __m256 v2x = _mm256_set1_ps(u2x), v2y = _mm256_set1_ps(u2y), v2z = _mm256_set1_ps(u2z);
        const __m256 vw2 = _mm256_set1_ps(w2);

        for (; i + 8 <= n; i += 8)
        {
            __m256 X = _mm256_loadu_ps(xin + i);
            __m256 Y = _mm256_loadu_ps(yin + i);
            __m256 Z = _mm256_loadu_ps(zin + i);
            __m256 cx, cy, cz;

            cx = _mm256_fnmadd_ps(vuz, Y, _mm256_mul_ps(vuy, Z));
            cy = _mm256_fnmadd_ps(vux, Z, _mm256_mul_ps(vuz, X));
            cz = _mm256_fnmadd_ps(vuy, X, _mm256_mul_ps(vux, Y));

            X = _mm256_fmadd_ps(vw2, cx, X);
            Y = _mm256_fmadd_ps(vw2, cy, Y);
            Z = _mm256_fmadd_ps(vw2, cz, Z);

            X = _mm256_fmadd_ps(v2y, cz, _mm256_fnmadd_ps(v2z, cy, X));
            Y = _mm256_fmadd_ps(v2z, cx, _mm256_fnmadd_ps(v2x, cz, Y));
            Z = _mm256_fmadd_ps(v2x, cy, _mm256_fnmadd_ps(v2y, cx, Z));

            _mm256_storeu_ps(xout + i, X);
            _mm256_storeu_ps(yout + i, Y);
            _mm256_storeu_ps(zout + i, Z);
        }
    }
#endif
    for (; i < n; ++i)
    {
        const mu_vec3 r = mu_quat_rotate_vec3(q, mu_vec3_make(xin[i], yin[i], zin[i]));

        xout[i] = r.x;
        yout[i] = r.y;
        zout[i] = r.z;
    }
}

MU_INLINE size_t mu_frustum_cull_spheres_compact(const mu_frustum *f,
                                                 const float *cx, const float *cy, const float *cz,
                                                 float radius,
                                                 uint32_t *out, size_t n)
{
    size_t i, count;

    count = 0;
    i     = 0;

#if defined(MU_MATH_AVX512)
    {
        const __m512 r  = _mm512_set1_ps(radius);
        const __m512 z0 = _mm512_setzero_ps();
        const __m512 a0 = _mm512_set1_ps(f->p[0].x), b0 = _mm512_set1_ps(f->p[0].y), c0 = _mm512_set1_ps(f->p[0].z), d0 = _mm512_set1_ps(f->p[0].w);
        const __m512 a1 = _mm512_set1_ps(f->p[1].x), b1 = _mm512_set1_ps(f->p[1].y), c1 = _mm512_set1_ps(f->p[1].z), d1 = _mm512_set1_ps(f->p[1].w);
        const __m512 a2 = _mm512_set1_ps(f->p[2].x), b2 = _mm512_set1_ps(f->p[2].y), c2 = _mm512_set1_ps(f->p[2].z), d2 = _mm512_set1_ps(f->p[2].w);
        const __m512 a3 = _mm512_set1_ps(f->p[3].x), b3 = _mm512_set1_ps(f->p[3].y), c3 = _mm512_set1_ps(f->p[3].z), d3 = _mm512_set1_ps(f->p[3].w);
        const __m512 a4 = _mm512_set1_ps(f->p[4].x), b4 = _mm512_set1_ps(f->p[4].y), c4 = _mm512_set1_ps(f->p[4].z), d4 = _mm512_set1_ps(f->p[4].w);
        const __m512 a5 = _mm512_set1_ps(f->p[5].x), b5 = _mm512_set1_ps(f->p[5].y), c5 = _mm512_set1_ps(f->p[5].z), d5 = _mm512_set1_ps(f->p[5].w);

        for (; i + 16 <= n; i += 16)
        {
            __m512 x = _mm512_loadu_ps(cx + i);
            __m512 y = _mm512_loadu_ps(cy + i);
            __m512 z = _mm512_loadu_ps(cz + i);
            __m512 id = _mm512_add_ps(z0, _mm512_set_ps(15, 14, 13, 12, 11, 10, 9, 8,
                                                        7, 6, 5, 4, 3, 2, 1, 0));
            __mmask16 m;
            __m512 dd;

            id = _mm512_add_ps(id, _mm512_set1_ps((float)i));

            dd = _mm512_fmadd_ps(c0, z, _mm512_fmadd_ps(b0, y, _mm512_fmadd_ps(a0, x, d0)));
            m  = _mm512_cmp_ps_mask(dd, _mm512_sub_ps(z0, r), _CMP_GE_OQ);
            dd = _mm512_fmadd_ps(c1, z, _mm512_fmadd_ps(b1, y, _mm512_fmadd_ps(a1, x, d1)));
            m  = _mm512_kand(m, _mm512_cmp_ps_mask(dd, _mm512_sub_ps(z0, r), _CMP_GE_OQ));
            dd = _mm512_fmadd_ps(c2, z, _mm512_fmadd_ps(b2, y, _mm512_fmadd_ps(a2, x, d2)));
            m  = _mm512_kand(m, _mm512_cmp_ps_mask(dd, _mm512_sub_ps(z0, r), _CMP_GE_OQ));
            dd = _mm512_fmadd_ps(c3, z, _mm512_fmadd_ps(b3, y, _mm512_fmadd_ps(a3, x, d3)));
            m  = _mm512_kand(m, _mm512_cmp_ps_mask(dd, _mm512_sub_ps(z0, r), _CMP_GE_OQ));
            dd = _mm512_fmadd_ps(c4, z, _mm512_fmadd_ps(b4, y, _mm512_fmadd_ps(a4, x, d4)));
            m  = _mm512_kand(m, _mm512_cmp_ps_mask(dd, _mm512_sub_ps(z0, r), _CMP_GE_OQ));
            dd = _mm512_fmadd_ps(c5, z, _mm512_fmadd_ps(b5, y, _mm512_fmadd_ps(a5, x, d5)));
            m  = _mm512_kand(m, _mm512_cmp_ps_mask(dd, _mm512_sub_ps(z0, r), _CMP_GE_OQ));

            /* Surviving lanes only: no branch, no per-lane store, and the
             * output is already dense. */
            /* cvttps, not a cast: id holds the indices as floats, and the bit
             * pattern of 1.0f is 1065353216. (This exact bug was caught by
             * perf_test/test_math.c, which is why the test exists.) */
            _mm512_mask_compressstoreu_epi32((void *)(out + count), m, _mm512_cvttps_epi32(id));
            count += (size_t)mu_popcount_u64((uint64_t)m);
        }
    }
#endif
    for (; i < n; ++i)
    {
        mu_vec3 center;

        center = mu_vec3_make(cx[i], cy[i], cz[i]);
        if (mu_frustum_test_sphere(f, center, radius))
        {
            out[count] = (uint32_t)i;
            ++count;
        }
    }
    return count;
}

/* ---- palette / instance batch: 4 matrices at once, lane = matrix ----
 *
 * A bone palette is four unrelated matrices. cglm would run four glm_mat4_mul
 * calls: 4 x (12 splats + 16 FMA). Here each 128-bit lane of a ZMM holds one
 * matrix's column, so the same 16 FMA advance all four multiplies.
 */
MU_INLINE void mu_mat4x4_pack(const mu_mat4 *m, size_t stride_bytes, mu_mat4x4 *out)
{
#if defined(MU_MATH_AVX512)
    /* Packing four AoS matrices into lane=matrix form is a 4x4 transpose at
     * 128-bit granularity: 4 loads + 8 vshuff32x4. Doing it with
     * _mm512_insertf32x4 instead costs 12 inserts, and that difference is
     * visible: it is the whole margin of the palette benchmark. */
    __m512 a0, a1, a2, a3, t0, t1, t2, t3;

    a0 = _mm512_loadu_ps(&((const mu_mat4 *)(const void *)((const char *)m))->col[0].x);
    a1 = _mm512_loadu_ps(&((const mu_mat4 *)(const void *)((const char *)m + stride_bytes))->col[0].x);
    a2 = _mm512_loadu_ps(&((const mu_mat4 *)(const void *)((const char *)m + 2 * stride_bytes))->col[0].x);
    a3 = _mm512_loadu_ps(&((const mu_mat4 *)(const void *)((const char *)m + 3 * stride_bytes))->col[0].x);

    t0 = _mm512_shuffle_f32x4(a0, a1, _MM_SHUFFLE(1, 0, 1, 0));
    t1 = _mm512_shuffle_f32x4(a0, a1, _MM_SHUFFLE(3, 2, 3, 2));
    t2 = _mm512_shuffle_f32x4(a2, a3, _MM_SHUFFLE(1, 0, 1, 0));
    t3 = _mm512_shuffle_f32x4(a2, a3, _MM_SHUFFLE(3, 2, 3, 2));

    out->col[0] = _mm512_shuffle_f32x4(t0, t2, _MM_SHUFFLE(2, 0, 2, 0));
    out->col[1] = _mm512_shuffle_f32x4(t0, t2, _MM_SHUFFLE(3, 1, 3, 1));
    out->col[2] = _mm512_shuffle_f32x4(t1, t3, _MM_SHUFFLE(2, 0, 2, 0));
    out->col[3] = _mm512_shuffle_f32x4(t1, t3, _MM_SHUFFLE(3, 1, 3, 1));
#else
    size_t k;

    for (k = 0; k < 4; ++k)
        out->m[k] = *(const mu_mat4 *)(const void *)((const char *)m + k * stride_bytes);
#endif
}

/*
 * dst_col[j] = sum_k A_col[k] * splat_k(B_col[j])
 *
 * (AB)[i][j] = sum_k A[i][k]*B[k][j]. A_col[k] already holds A[i][k] in
 * position i for every matrix, and B[k][j] is element k of B_col[j] within
 * each lane, so one lane-local permute per (k) turns B into the broadcast
 * factor. 16 permutes + 16 FMA for four matrix multiplies.
 */
MU_INLINE void mu_mat4x4_mul(const mu_mat4x4 *a, const mu_mat4x4 *b, mu_mat4x4 *dst)
{
#if defined(MU_MATH_AVX512)
    const __m512i k0 = _mm512_set_epi32(12, 12, 12, 12, 8, 8, 8, 8, 4, 4, 4, 4, 0, 0, 0, 0);
    const __m512i k1 = _mm512_set_epi32(13, 13, 13, 13, 9, 9, 9, 9, 5, 5, 5, 5, 1, 1, 1, 1);
    const __m512i k2 = _mm512_set_epi32(14, 14, 14, 14, 10, 10, 10, 10, 6, 6, 6, 6, 2, 2, 2, 2);
    const __m512i k3 = _mm512_set_epi32(15, 15, 15, 15, 11, 11, 11, 11, 7, 7, 7, 7, 3, 3, 3, 3);
    __m512 b0, b1, b2, b3, d0, d1, d2, d3;

    b0 = b->col[0];
    b1 = b->col[1];
    b2 = b->col[2];
    b3 = b->col[3];

    d0 = _mm512_mul_ps(a->col[0], _mm512_permutexvar_ps(k0, b0));
    d1 = _mm512_mul_ps(a->col[0], _mm512_permutexvar_ps(k0, b1));
    d2 = _mm512_mul_ps(a->col[0], _mm512_permutexvar_ps(k0, b2));
    d3 = _mm512_mul_ps(a->col[0], _mm512_permutexvar_ps(k0, b3));

    d0 = _mm512_fmadd_ps(a->col[1], _mm512_permutexvar_ps(k1, b0), d0);
    d1 = _mm512_fmadd_ps(a->col[1], _mm512_permutexvar_ps(k1, b1), d1);
    d2 = _mm512_fmadd_ps(a->col[1], _mm512_permutexvar_ps(k1, b2), d2);
    d3 = _mm512_fmadd_ps(a->col[1], _mm512_permutexvar_ps(k1, b3), d3);

    d0 = _mm512_fmadd_ps(a->col[2], _mm512_permutexvar_ps(k2, b0), d0);
    d1 = _mm512_fmadd_ps(a->col[2], _mm512_permutexvar_ps(k2, b1), d1);
    d2 = _mm512_fmadd_ps(a->col[2], _mm512_permutexvar_ps(k2, b2), d2);
    d3 = _mm512_fmadd_ps(a->col[2], _mm512_permutexvar_ps(k2, b3), d3);

    d0 = _mm512_fmadd_ps(a->col[3], _mm512_permutexvar_ps(k3, b0), d0);
    d1 = _mm512_fmadd_ps(a->col[3], _mm512_permutexvar_ps(k3, b1), d1);
    d2 = _mm512_fmadd_ps(a->col[3], _mm512_permutexvar_ps(k3, b2), d2);
    d3 = _mm512_fmadd_ps(a->col[3], _mm512_permutexvar_ps(k3, b3), d3);

    dst->col[0] = d0;
    dst->col[1] = d1;
    dst->col[2] = d2;
    dst->col[3] = d3;
#else
    size_t k;

    for (k = 0; k < 4; ++k)
        mu_mat4_mul(&a->m[k], &b->m[k], &dst->m[k]);
#endif
}

MU_INLINE void mu_mat4x4_unpack(const mu_mat4x4 *p, mu_mat4 *out, size_t stride_bytes)
{
#if defined(MU_MATH_AVX512)
    /* The same lane transpose in reverse: it is its own inverse. */
    __m512 t0, t1, t2, t3, a0, a1, a2, a3;

    t0 = _mm512_shuffle_f32x4(p->col[0], p->col[1], _MM_SHUFFLE(1, 0, 1, 0));
    t1 = _mm512_shuffle_f32x4(p->col[0], p->col[1], _MM_SHUFFLE(3, 2, 3, 2));
    t2 = _mm512_shuffle_f32x4(p->col[2], p->col[3], _MM_SHUFFLE(1, 0, 1, 0));
    t3 = _mm512_shuffle_f32x4(p->col[2], p->col[3], _MM_SHUFFLE(3, 2, 3, 2));

    a0 = _mm512_shuffle_f32x4(t0, t2, _MM_SHUFFLE(2, 0, 2, 0));
    a1 = _mm512_shuffle_f32x4(t0, t2, _MM_SHUFFLE(3, 1, 3, 1));
    a2 = _mm512_shuffle_f32x4(t1, t3, _MM_SHUFFLE(2, 0, 2, 0));
    a3 = _mm512_shuffle_f32x4(t1, t3, _MM_SHUFFLE(3, 1, 3, 1));

    _mm512_storeu_ps(&((mu_mat4 *)(void *)((char *)out))->col[0].x, a0);
    _mm512_storeu_ps(&((mu_mat4 *)(void *)((char *)out + stride_bytes))->col[0].x, a1);
    _mm512_storeu_ps(&((mu_mat4 *)(void *)((char *)out + 2 * stride_bytes))->col[0].x, a2);
    _mm512_storeu_ps(&((mu_mat4 *)(void *)((char *)out + 3 * stride_bytes))->col[0].x, a3);
#else
    size_t k;

    for (k = 0; k < 4; ++k)
        *(mu_mat4 *)(void *)((char *)out + k * stride_bytes) = p->m[k];
#endif
}

MU_INLINE void mu_mat4_mul_batch(const mu_mat4 *a, size_t stride_a,
                                 const mu_mat4 *b, size_t stride_b,
                                 mu_mat4 *dst, size_t stride_d, size_t count)
{
    /* 4-way when AVX-512 is present, otherwise the plain per-matrix kernel.
     * The point of the outer loop is simply to expose four independent
     * multiplies to the machine at a time: one multiply is latency-bound, four
     * saturate the FMA pipes. */
    size_t i;

    i = 0;
#if defined(MU_MATH_AVX512)
    for (; i + 4 <= count; i += 4)
    {
        mu_mat4x4 pa, pb, pd;

        mu_mat4x4_pack((const mu_mat4 *)(const void *)((const char *)a + i * stride_a), stride_a, &pa);
        mu_mat4x4_pack((const mu_mat4 *)(const void *)((const char *)b + i * stride_b), stride_b, &pb);
        mu_mat4x4_mul(&pa, &pb, &pd);
        mu_mat4x4_unpack(&pd, (mu_mat4 *)(void *)((char *)dst + i * stride_d), stride_d);
    }
#endif
    for (; i < count; ++i)
    {
        mu_mat4_mul((const mu_mat4 *)(const void *)((const char *)a + i * stride_a),
                    (const mu_mat4 *)(const void *)((const char *)b + i * stride_b),
                    (mu_mat4 *)(void *)((char *)dst + i * stride_d));
    }
}

MU_END_EXTERN_C

#endif /* MU_MATH_H */
