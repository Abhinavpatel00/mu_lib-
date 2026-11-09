#ifndef MU_NOISE_MATH_H
#define MU_NOISE_MATH_H

#if defined(__clang__)
#define MU_NOISE_BATCH_SIMD _Pragma("clang loop vectorize(enable)")
#elif defined(__GNUC__)
#define MU_NOISE_BATCH_SIMD _Pragma("GCC ivdep")
#else
#define MU_NOISE_BATCH_SIMD
#endif

/*
    mu_noise_math.h
    ---------------------------------------------------------------------------
    Standalone C99 single-header procedural noise library.

    Goal:
    - Provide practical "families" of noise used in games/tools.
    - Keep implementation dependency-free and data-oriented.
    - Explain the math visually and directly in code comments.

    Included noise families:
    1)  White / hash noise       (uncorrelated random value per lattice cell)
    2)  Value noise              (interpolated lattice scalars)
    3)  Gradient / Perlin noise  (interpolated corner gradients)
    4)  Simplex noise            (simplex cells; fewer directional artifacts)
    5)  OpenSimplex2-inspired    (rotated simplex variants)
    6)  Worley / cellular noise  (F1/F2 distances, cell IDs, centers)
    7)  Voronoi-style helpers    (borders, cracks, cell lookup)
    8)  Fractal compositions     (fBm, turbulence, billow, ridged)
    9)  Domain warping           (coordinate perturbation, recursive warps)
    10) Curl noise               (divergence-free 2D/3D vector fields)
    11) Derivative sampling      (analytic 2D/3D/4D value/perlin/simplex + finite differences)
    12) 4D noise                 (white/value/perlin/simplex/Worley, periodic variants)
    13) Periodic / tileable      (value, perlin, natively tileable simplex, Worley)
    14) Batch sampling           (struct-of-arrays noise and derivative batches)
    15) Material recipes         (terrain, marble, wood, clouds, caustics, rust)

    ---------------------------------------------------------------------------
    CONVENTIONS
    ---------------------------------------------------------------------------

    Coordinates:
      - Smooth samplers are continuous in the real domain; integer coordinates
        are lattice cell boundaries for grid noise. White noise is
        discontinuous by design.
      - 1D/2D/3D/4D variants use the obvious `x`, `x,y`, `x,y,z`, `x,y,z,w`
        argument order.
      - Seeds are arbitrary uint32_t values. Two different seeds produce
        statistically independent fields (not guaranteed cryptographically).

    Output ranges:
      - white/value/perlin/simplex/fBm/billow/ridged: ~[-1, 1].
      - turbulence: ~[0, 1].
      - worley distances: >= 0; F2-F1 is >= 0.
      - periodic samplers have the same output range as their base sampler.
      - due to finite float precision and empirical normalizations, exact
        extrema are approximate. Clamp only when a consumer requires it.

    Periods:
      - Periods are expressed in lattice cells, not in samples.
      - `period <= 0` disables periodicity on that axis.
      - `mu_noise_*_periodic` functions tile exactly at multiples of the
        period. Derivatives may be only C0 at the seam for wrappers that use
        domain blending (simplex); value/perlin hash wrapping is C2.

    Determinism:
      - The hash is integer-only and fully deterministic for a given seed.
      - Floating-point results may differ slightly across compilers/ISAs
        because FMA contraction and libm implementations differ.

    Threading:
      - Every function is pure and reentrant. There is no global state, no
        allocation, and no locking.

    ---------------------------------------------------------------------------
    VISUAL QUICK MAP
    ---------------------------------------------------------------------------

      White noise:
        integer cell -> hash -> random scalar

      Value noise:
         c00 ----- c10
          |         |
          |   p     |    p is blended by smooth interpolation
          |         |
         c01 ----- c11

      Perlin noise:
         each corner stores a pseudo-random gradient direction g
         value at p is dot(g, p-corner), then smoothly interpolated

      Simplex 2D:
        skew square grid into equilateral triangles
        evaluate 3 triangle corners only

      Simplex 4D:
        rank the four local coordinates, visit five hypertriangle corners

      Worley:
        each cell has feature point f
        return F1 (nearest dist), F2 (2nd nearest dist), cell IDs, centers

      fBm:
        sum_{octaves} amp_i * noise(freq_i * p), optionally normalized

      Domain warp:
        p' = p + warp_amp * W(freq * p)
        value = N(p')

      Curl (2D):
        v = ( dPsi/dy, -dPsi/dx )
        so div(v) = 0 (incompressible flow)

      Derivatives:
        d/dp [ t^4 * dot(g, p-c) ] =
            4*t^3*(dt/dp)*dot(g,p-c) + t^4*g,  dt/dp = -2*(p-c)

    ---------------------------------------------------------------------------
    Usage examples
    ---------------------------------------------------------------------------

      float n  = mu_noise_simplex2(12.3f, 9.1f, 1337u);
      float fb = mu_noise_fbm2(mu_noise_perlin2, 12.3f, 9.1f, 1337u, 6, 2.0f, 0.5f);
      float p  = mu_noise_perlin2_periodic(12.3f, 9.1f, 128, 128, 1337u);
      float os = mu_noise_opensimplex2_2d(12.3f, 9.1f, 1337u);

      mu_noise_worley2_result w =
          mu_noise_worley2(12.3f, 9.1f, 1337u, MU_NOISE_DIST_EUCLIDEAN);
      float border = w.f2 - w.f1;

      mu_noise_sample2 s = mu_noise_perlin2_deriv(12.3f, 9.1f, 1337u);
      float h = s.value;          // scalar height
      float gx = s.gradient.x;    // dh/dx
      float gy = s.gradient.y;    // dh/dy

      float h4 = mu_noise_simplex4(1.0f, 2.0f, 3.0f, 4.0f, 1337u);

    ---------------------------------------------------------------------------
*/

#include <stdint.h>
#include <math.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "mu_macros.h"
#include "mu_math_legacy.h"   /* mu_vec2/3/4, mu_vec*_dot/add/sub/scale: one math layer */

/* ------------------------------------------------------------------------- */
/* Basic structs and enums                                                   */
/* ------------------------------------------------------------------------- */

/*
    Vector types.

    These used to be three private structs plus a private copy of dot/add/sub/
    cross, which is exactly the "competing API family" the library rules forbid:
    a caller could not hand a noise direction to mu_vec3_normalize without a
    conversion, and there were two implementations of the same maths to keep in
    step. They are now aliases of mu_math's types, so noise vectors are mu_vecs
    and interoperate with everything else in the library for free.

    The mu_noise_v2/v3/v4 constructors and the mu_noise_v*_dot/add/... helpers
    stay as names (they read better inside this file) and forward to mu_math.
*/
typedef mu_vec2 mu_noise_vec2;
typedef mu_vec3 mu_noise_vec3;
typedef mu_vec4 mu_noise_vec4;

typedef enum mu_noise_distance_metric {
    MU_NOISE_DIST_EUCLIDEAN = 0,
    MU_NOISE_DIST_MANHATTAN = 1,
    MU_NOISE_DIST_CHEBYSHEV = 2
} mu_noise_distance_metric;

typedef struct mu_noise_worley2_result {
    float f1;            /* distance to nearest feature point            */
    float f2;            /* distance to second-nearest feature point     */
    int cell_id1;        /* hashed ID of the nearest cell                */
    int cell_id2;        /* hashed ID of the second-nearest cell         */
    mu_noise_vec2 center1; /* absolute position of the nearest feature   */
    mu_noise_vec2 center2; /* absolute position of the second feature    */
} mu_noise_worley2_result;

typedef struct mu_noise_worley3_result {
    float f1;
    float f2;
    int cell_id1;
    int cell_id2;
    mu_noise_vec3 center1;
    mu_noise_vec3 center2;
} mu_noise_worley3_result;

typedef struct mu_noise_worley4_result {
    float f1;
    float f2;
    int cell_id1;
    int cell_id2;
    mu_noise_vec4 center1;
    mu_noise_vec4 center2;
} mu_noise_worley4_result;

typedef struct mu_noise_sample2 {
    float value;
    mu_noise_vec2 gradient;
} mu_noise_sample2;

typedef struct mu_noise_sample3 {
    float value;
    mu_noise_vec3 gradient;
} mu_noise_sample3;

typedef struct mu_noise_sample4 {
    float value;
    mu_noise_vec4 gradient;
} mu_noise_sample4;

typedef float (*mu_noise_fn1)(float x, uint32_t seed);
typedef float (*mu_noise_fn2)(float x, float y, uint32_t seed);
typedef float (*mu_noise_fn3)(float x, float y, float z, uint32_t seed);
typedef float (*mu_noise_fn4)(float x, float y, float z, float w, uint32_t seed);

typedef mu_noise_vec2 (*mu_noise_vfn2)(float x, float y, uint32_t seed);
typedef mu_noise_vec3 (*mu_noise_vfn3)(float x, float y, float z, uint32_t seed);
typedef mu_noise_vec4 (*mu_noise_vfn4)(float x, float y, float z, float w, uint32_t seed);

typedef mu_noise_sample2 (*mu_noise_dfn2)(float x, float y, uint32_t seed);
typedef mu_noise_sample3 (*mu_noise_dfn3)(float x, float y, float z, uint32_t seed);
typedef mu_noise_sample4 (*mu_noise_dfn4)(float x, float y, float z, float w, uint32_t seed);

/* ------------------------------------------------------------------------- */
/* Scalar/vector helpers                                                     */
/* ------------------------------------------------------------------------- */

MU_INLINE float mu_noise_abs(float x) { return fabsf(x); }
MU_INLINE float mu_noise_floor(float x) { return floorf(x); }
MU_INLINE float mu_noise_sqrt(float x) { return sqrtf(x); }

MU_INLINE float mu_noise_clamp(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

MU_INLINE float mu_noise_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

MU_INLINE float mu_noise_fract(float x)
{
    return x - floorf(x);
}

MU_INLINE int mu_noise_mod_i(int x, int period)
{
    if (period <= 0) return x;
    {
        int m = x % period;
        return (m < 0) ? (m + period) : m;
    }
}

MU_INLINE float mu_noise_wrap_period(float x, float period)
{
    if (period <= 0.0f) return x;
    {
        float m = fmodf(x, period);
        return (m < 0.0f) ? (m + period) : m;
    }
}

MU_INLINE float mu_noise_smoothstep3(float t)
{
    /* cubic smoothstep: 3t^2 - 2t^3 */
    return t * t * (3.0f - 2.0f * t);
}

MU_INLINE float mu_noise_smootherstep5(float t)
{
    /*
        quintic smoothstep: 6t^5 - 15t^4 + 10t^3

        Why quintic for gradient/simplex interpolation?
        - first derivative at boundaries is 0
        - second derivative at boundaries is 0
        This reduces visible grid seams.
    */
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

MU_INLINE float mu_noise_remap_neg1_pos1_to_0_1(float v)
{
    return 0.5f * v + 0.5f;
}

MU_INLINE mu_noise_vec2 mu_noise_v2(float x, float y)
{
    return mu_vec2_make(x, y);
}

MU_INLINE mu_noise_vec3 mu_noise_v3(float x, float y, float z)
{
    return mu_vec3_make(x, y, z);
}

MU_INLINE mu_noise_vec4 mu_noise_v4(float x, float y, float z, float w)
{
    return mu_vec4_make(x, y, z, w);
}

MU_INLINE float mu_noise_v2_dot(mu_noise_vec2 a, mu_noise_vec2 b) { return mu_vec2_dot(a, b); }
MU_INLINE float mu_noise_v3_dot(mu_noise_vec3 a, mu_noise_vec3 b) { return mu_vec3_dot(a, b); }
MU_INLINE float mu_noise_v4_dot(mu_noise_vec4 a, mu_noise_vec4 b) { return mu_vec4_dot(a, b); }

MU_INLINE float mu_noise_v2_len(mu_noise_vec2 a) { return mu_vec2_length(a); }
MU_INLINE float mu_noise_v3_len(mu_noise_vec3 a) { return mu_vec3_length(a); }
MU_INLINE float mu_noise_v4_len(mu_noise_vec4 a) { return sqrtf(mu_vec4_dot(a, a)); }

MU_INLINE mu_noise_vec2 mu_noise_v2_add(mu_noise_vec2 a, mu_noise_vec2 b) { return mu_vec2_add(a, b); }
MU_INLINE mu_noise_vec3 mu_noise_v3_add(mu_noise_vec3 a, mu_noise_vec3 b) { return mu_vec3_add(a, b); }
MU_INLINE mu_noise_vec4 mu_noise_v4_add(mu_noise_vec4 a, mu_noise_vec4 b) { return mu_vec4_add(a, b); }

MU_INLINE mu_noise_vec2 mu_noise_v2_sub(mu_noise_vec2 a, mu_noise_vec2 b) { return mu_vec2_sub(a, b); }
MU_INLINE mu_noise_vec3 mu_noise_v3_sub(mu_noise_vec3 a, mu_noise_vec3 b) { return mu_vec3_sub(a, b); }
MU_INLINE mu_noise_vec4 mu_noise_v4_sub(mu_noise_vec4 a, mu_noise_vec4 b) { return mu_vec4_sub(a, b); }

/* Scalar scale, not component-wise: mu_noise_v3_mul(a, s) means a * s. */
MU_INLINE mu_noise_vec2 mu_noise_v2_mul(mu_noise_vec2 a, float s) { return mu_vec2_scale(a, s); }
MU_INLINE mu_noise_vec3 mu_noise_v3_mul(mu_noise_vec3 a, float s) { return mu_vec3_scale(a, s); }
MU_INLINE mu_noise_vec4 mu_noise_v4_mul(mu_noise_vec4 a, float s) { return mu_vec4_scale(a, s); }

MU_INLINE mu_noise_vec3 mu_noise_v3_cross(mu_noise_vec3 a, mu_noise_vec3 b) { return mu_vec3_cross(a, b); }

/* Epsilon-guarded normalisation: a zero-length vector normalises to zero
 * instead of NaN, which is what noise code wants at lattice corners. */
MU_INLINE mu_noise_vec2 mu_noise_v2_normalize(mu_noise_vec2 a, float epsilon)
{
    float len = mu_vec2_length(a);
    if (len <= epsilon) return mu_vec2_make(0.0f, 0.0f);
    return mu_vec2_scale(a, 1.0f / len);
}

MU_INLINE mu_noise_vec3 mu_noise_v3_normalize(mu_noise_vec3 a, float epsilon)
{
    float len = mu_vec3_length(a);
    if (len <= epsilon) return mu_vec3_make(0.0f, 0.0f, 0.0f);
    return mu_vec3_scale(a, 1.0f / len);
}

/* ------------------------------------------------------------------------- */
/* Hashes and randoms                                                        */
/* ------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_noise_hash_u32(uint32_t x)
{
    /* A small high-quality integer avalanche hash. */
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

MU_INLINE uint32_t mu_noise_hash_combine(uint32_t h, uint32_t k)
{
    h ^= mu_noise_hash_u32(k + 0x9e3779b9U + (h << 6) + (h >> 2));
    return h;
}

MU_INLINE uint32_t mu_noise_hash_1i(int x, uint32_t seed)
{
    uint32_t h = mu_noise_hash_u32(seed ^ 0xA1B3C6D9U);
    h = mu_noise_hash_combine(h, (uint32_t)x);
    return mu_noise_hash_u32(h);
}

MU_INLINE uint32_t mu_noise_hash_2i(int x, int y, uint32_t seed)
{
    uint32_t h = mu_noise_hash_u32(seed ^ 0xD1B54A35U);
    h = mu_noise_hash_combine(h, (uint32_t)x);
    h = mu_noise_hash_combine(h, (uint32_t)y);
    return mu_noise_hash_u32(h);
}

MU_INLINE uint32_t mu_noise_hash_3i(int x, int y, int z, uint32_t seed)
{
    uint32_t h = mu_noise_hash_u32(seed ^ 0x94D049BBU);
    h = mu_noise_hash_combine(h, (uint32_t)x);
    h = mu_noise_hash_combine(h, (uint32_t)y);
    h = mu_noise_hash_combine(h, (uint32_t)z);
    return mu_noise_hash_u32(h);
}

MU_INLINE uint32_t mu_noise_hash_4i(int x, int y, int z, int w, uint32_t seed)
{
    uint32_t h = mu_noise_hash_u32(seed ^ 0x1B56C4E9U);
    h = mu_noise_hash_combine(h, (uint32_t)x);
    h = mu_noise_hash_combine(h, (uint32_t)y);
    h = mu_noise_hash_combine(h, (uint32_t)z);
    h = mu_noise_hash_combine(h, (uint32_t)w);
    return mu_noise_hash_u32(h);
}

MU_INLINE uint32_t mu_noise_hash_1i_periodic(int x, int period_x, uint32_t seed)
{
    return mu_noise_hash_1i(mu_noise_mod_i(x, period_x), seed);
}

MU_INLINE uint32_t mu_noise_hash_2i_periodic(int x, int y, int period_x, int period_y, uint32_t seed)
{
    return mu_noise_hash_2i(mu_noise_mod_i(x, period_x), mu_noise_mod_i(y, period_y), seed);
}

MU_INLINE uint32_t mu_noise_hash_3i_periodic(int x, int y, int z, int period_x, int period_y, int period_z, uint32_t seed)
{
    return mu_noise_hash_3i(
        mu_noise_mod_i(x, period_x),
        mu_noise_mod_i(y, period_y),
        mu_noise_mod_i(z, period_z),
        seed
    );
}

MU_INLINE uint32_t mu_noise_hash_4i_periodic(int x, int y, int z, int w,
                                             int period_x, int period_y,
                                             int period_z, int period_w,
                                             uint32_t seed)
{
    return mu_noise_hash_4i(
        mu_noise_mod_i(x, period_x),
        mu_noise_mod_i(y, period_y),
        mu_noise_mod_i(z, period_z),
        mu_noise_mod_i(w, period_w),
        seed
    );
}

MU_INLINE float mu_noise_hash_to_unit_float(uint32_t h)
{
    /* 24-bit mantissa-friendly mapping [0, 1). */
    return (float)(h >> 8) * (1.0f / 16777216.0f);
}

MU_INLINE float mu_noise_hash_to_signed_float(uint32_t h)
{
    return mu_noise_hash_to_unit_float(h) * 2.0f - 1.0f;
}

/* ------------------------------------------------------------------------- */
/* 1) White noise                                                            */
/* ------------------------------------------------------------------------- */

/*
    White noise is "uncorrelated" between integer cells.

    N(i) = hash(i)

    Sampling at continuous x,y,z usually means taking floor first.
*/
MU_INLINE float mu_noise_white1(float x, uint32_t seed)
{
    int ix = (int)floorf(x);
    return mu_noise_hash_to_signed_float(mu_noise_hash_1i(ix, seed));
}

MU_INLINE float mu_noise_white2(float x, float y, uint32_t seed)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    return mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix, iy, seed));
}

MU_INLINE float mu_noise_white3(float x, float y, float z, uint32_t seed)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    int iz = (int)floorf(z);
    return mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix, iy, iz, seed));
}

MU_INLINE float mu_noise_white4(float x, float y, float z, float w, uint32_t seed)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    int iz = (int)floorf(z);
    int iw = (int)floorf(w);
    return mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix, iy, iz, iw, seed));
}

/* ------------------------------------------------------------------------- */
/* 2) Value noise                                                            */
/* ------------------------------------------------------------------------- */

/*
    Value noise stores random scalar values at lattice corners and blends.

    1D:
      i0 = floor(x), i1=i0+1, t=fract(x)
      N(x) = lerp(v(i0), v(i1), fade(t))

    2D:
        v00 ---- v10
         |        |
         |   p    |    bilinear blend with smooth t_x,t_y
         |        |
        v01 ---- v11

    3D:
      same pattern with trilinear blend over 8 corners.
*/
MU_INLINE float mu_noise_value1(float x, uint32_t seed)
{
    int i0 = (int)floorf(x);
    int i1 = i0 + 1;
    float t = x - (float)i0;
    float u = mu_noise_smootherstep5(t);

    float v0 = mu_noise_hash_to_signed_float(mu_noise_hash_1i(i0, seed));
    float v1 = mu_noise_hash_to_signed_float(mu_noise_hash_1i(i1, seed));

    return mu_noise_lerp(v0, v1, u);
}

MU_INLINE float mu_noise_value2(float x, float y, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);

    float v00 = mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix0, iy0, seed));
    float v10 = mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix1, iy0, seed));
    float v01 = mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix0, iy1, seed));
    float v11 = mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix1, iy1, seed));

    float a = mu_noise_lerp(v00, v10, ux);
    float b = mu_noise_lerp(v01, v11, ux);
    return mu_noise_lerp(a, b, uy);
}

MU_INLINE float mu_noise_value3(float x, float y, float z, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int iz0 = (int)floorf(z);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;
    int iz1 = iz0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float tz = z - (float)iz0;

    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float uz = mu_noise_smootherstep5(tz);

    float c000 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix0, iy0, iz0, seed));
    float c100 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix1, iy0, iz0, seed));
    float c010 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix0, iy1, iz0, seed));
    float c110 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix1, iy1, iz0, seed));
    float c001 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix0, iy0, iz1, seed));
    float c101 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix1, iy0, iz1, seed));
    float c011 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix0, iy1, iz1, seed));
    float c111 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix1, iy1, iz1, seed));

    float x00 = mu_noise_lerp(c000, c100, ux);
    float x10 = mu_noise_lerp(c010, c110, ux);
    float x01 = mu_noise_lerp(c001, c101, ux);
    float x11 = mu_noise_lerp(c011, c111, ux);

    float y0 = mu_noise_lerp(x00, x10, uy);
    float y1 = mu_noise_lerp(x01, x11, uy);

    return mu_noise_lerp(y0, y1, uz);
}

/*
    Periodic/tileable value noise:
    - periods are in lattice cells.
    - period<=0 on any axis falls back to non-periodic hashing on that axis.
*/
MU_INLINE float mu_noise_value1_periodic(float x, int period_x, uint32_t seed)
{
    if (period_x <= 0) return mu_noise_value1(x, seed);

    {
        int i0 = (int)floorf(x);
        int i1 = i0 + 1;
        float t = x - (float)i0;
        float u = mu_noise_smootherstep5(t);

        float v0 = mu_noise_hash_to_signed_float(mu_noise_hash_1i_periodic(i0, period_x, seed));
        float v1 = mu_noise_hash_to_signed_float(mu_noise_hash_1i_periodic(i1, period_x, seed));
        return mu_noise_lerp(v0, v1, u);
    }
}

MU_INLINE float mu_noise_value2_periodic(float x, float y, int period_x, int period_y, uint32_t seed)
{
    if (period_x <= 0 || period_y <= 0) return mu_noise_value2(x, y, seed);

    {
        int ix0 = (int)floorf(x);
        int iy0 = (int)floorf(y);
        int ix1 = ix0 + 1;
        int iy1 = iy0 + 1;

        float tx = x - (float)ix0;
        float ty = y - (float)iy0;
        float ux = mu_noise_smootherstep5(tx);
        float uy = mu_noise_smootherstep5(ty);

        float v00 = mu_noise_hash_to_signed_float(mu_noise_hash_2i_periodic(ix0, iy0, period_x, period_y, seed));
        float v10 = mu_noise_hash_to_signed_float(mu_noise_hash_2i_periodic(ix1, iy0, period_x, period_y, seed));
        float v01 = mu_noise_hash_to_signed_float(mu_noise_hash_2i_periodic(ix0, iy1, period_x, period_y, seed));
        float v11 = mu_noise_hash_to_signed_float(mu_noise_hash_2i_periodic(ix1, iy1, period_x, period_y, seed));

        {
            float a = mu_noise_lerp(v00, v10, ux);
            float b = mu_noise_lerp(v01, v11, ux);
            return mu_noise_lerp(a, b, uy);
        }
    }
}

MU_INLINE float mu_noise_value3_periodic(float x, float y, float z, int period_x, int period_y, int period_z, uint32_t seed)
{
    if (period_x <= 0 || period_y <= 0 || period_z <= 0) return mu_noise_value3(x, y, z, seed);

    {
        int ix0 = (int)floorf(x);
        int iy0 = (int)floorf(y);
        int iz0 = (int)floorf(z);
        int ix1 = ix0 + 1;
        int iy1 = iy0 + 1;
        int iz1 = iz0 + 1;

        float tx = x - (float)ix0;
        float ty = y - (float)iy0;
        float tz = z - (float)iz0;
        float ux = mu_noise_smootherstep5(tx);
        float uy = mu_noise_smootherstep5(ty);
        float uz = mu_noise_smootherstep5(tz);

        float c000 = mu_noise_hash_to_signed_float(mu_noise_hash_3i_periodic(ix0, iy0, iz0, period_x, period_y, period_z, seed));
        float c100 = mu_noise_hash_to_signed_float(mu_noise_hash_3i_periodic(ix1, iy0, iz0, period_x, period_y, period_z, seed));
        float c010 = mu_noise_hash_to_signed_float(mu_noise_hash_3i_periodic(ix0, iy1, iz0, period_x, period_y, period_z, seed));
        float c110 = mu_noise_hash_to_signed_float(mu_noise_hash_3i_periodic(ix1, iy1, iz0, period_x, period_y, period_z, seed));
        float c001 = mu_noise_hash_to_signed_float(mu_noise_hash_3i_periodic(ix0, iy0, iz1, period_x, period_y, period_z, seed));
        float c101 = mu_noise_hash_to_signed_float(mu_noise_hash_3i_periodic(ix1, iy0, iz1, period_x, period_y, period_z, seed));
        float c011 = mu_noise_hash_to_signed_float(mu_noise_hash_3i_periodic(ix0, iy1, iz1, period_x, period_y, period_z, seed));
        float c111 = mu_noise_hash_to_signed_float(mu_noise_hash_3i_periodic(ix1, iy1, iz1, period_x, period_y, period_z, seed));

        {
            float x00 = mu_noise_lerp(c000, c100, ux);
            float x10 = mu_noise_lerp(c010, c110, ux);
            float x01 = mu_noise_lerp(c001, c101, ux);
            float x11 = mu_noise_lerp(c011, c111, ux);
            float y0 = mu_noise_lerp(x00, x10, uy);
            float y1 = mu_noise_lerp(x01, x11, uy);
            return mu_noise_lerp(y0, y1, uz);
        }
    }
}

/*
    4D value noise.

    There are 16 corners. Interpolate along x, then y, then z, then w:

        x000 = lerp(c0000, c1000, ux)
        x100 = lerp(c0100, c1100, ux)
        ...
        y00  = lerp(x000, x100, uy)
        ...
        z0   = lerp(y00, y10, uz)
        ...
        out  = lerp(z0, z1, uw)

    The naming cXYZW means X is the most significant lattice bit.
*/
MU_INLINE float mu_noise_value4(float x, float y, float z, float w, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int iz0 = (int)floorf(z);
    int iw0 = (int)floorf(w);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;
    int iz1 = iz0 + 1;
    int iw1 = iw0 + 1;

    float ux = mu_noise_smootherstep5(x - (float)ix0);
    float uy = mu_noise_smootherstep5(y - (float)iy0);
    float uz = mu_noise_smootherstep5(z - (float)iz0);
    float uw = mu_noise_smootherstep5(w - (float)iw0);

    float c0000 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix0, iy0, iz0, iw0, seed));
    float c1000 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix1, iy0, iz0, iw0, seed));
    float c0100 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix0, iy1, iz0, iw0, seed));
    float c1100 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix1, iy1, iz0, iw0, seed));
    float c0010 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix0, iy0, iz1, iw0, seed));
    float c1010 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix1, iy0, iz1, iw0, seed));
    float c0110 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix0, iy1, iz1, iw0, seed));
    float c1110 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix1, iy1, iz1, iw0, seed));
    float c0001 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix0, iy0, iz0, iw1, seed));
    float c1001 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix1, iy0, iz0, iw1, seed));
    float c0101 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix0, iy1, iz0, iw1, seed));
    float c1101 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix1, iy1, iz0, iw1, seed));
    float c0011 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix0, iy0, iz1, iw1, seed));
    float c1011 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix1, iy0, iz1, iw1, seed));
    float c0111 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix0, iy1, iz1, iw1, seed));
    float c1111 = mu_noise_hash_to_signed_float(mu_noise_hash_4i(ix1, iy1, iz1, iw1, seed));

    float x000 = mu_noise_lerp(c0000, c1000, ux);
    float x100 = mu_noise_lerp(c0100, c1100, ux);
    float x010 = mu_noise_lerp(c0010, c1010, ux);
    float x110 = mu_noise_lerp(c0110, c1110, ux);
    float x001 = mu_noise_lerp(c0001, c1001, ux);
    float x101 = mu_noise_lerp(c0101, c1101, ux);
    float x011 = mu_noise_lerp(c0011, c1011, ux);
    float x111 = mu_noise_lerp(c0111, c1111, ux);

    float y00 = mu_noise_lerp(x000, x100, uy);
    float y10 = mu_noise_lerp(x010, x110, uy);
    float y01 = mu_noise_lerp(x001, x101, uy);
    float y11 = mu_noise_lerp(x011, x111, uy);

    float z0 = mu_noise_lerp(y00, y10, uz);
    float z1 = mu_noise_lerp(y01, y11, uz);

    return mu_noise_lerp(z0, z1, uw);
}

MU_INLINE float mu_noise_value4_periodic(float x, float y, float z, float w,
                                         int period_x, int period_y,
                                         int period_z, int period_w,
                                         uint32_t seed)
{
    int ix0, iy0, iz0, iw0, ix1, iy1, iz1, iw1;
    float ux, uy, uz, uw;
    float c0000, c1000, c0100, c1100, c0010, c1010, c0110, c1110;
    float c0001, c1001, c0101, c1101, c0011, c1011, c0111, c1111;
    float x000, x100, x010, x110, x001, x101, x011, x111;
    float y00, y10, y01, y11, z0, z1;

    if (period_x <= 0 || period_y <= 0 || period_z <= 0 || period_w <= 0)
        return mu_noise_value4(x, y, z, w, seed);

    ix0 = (int)floorf(x);
    iy0 = (int)floorf(y);
    iz0 = (int)floorf(z);
    iw0 = (int)floorf(w);
    ix1 = ix0 + 1;
    iy1 = iy0 + 1;
    iz1 = iz0 + 1;
    iw1 = iw0 + 1;

    ux = mu_noise_smootherstep5(x - (float)ix0);
    uy = mu_noise_smootherstep5(y - (float)iy0);
    uz = mu_noise_smootherstep5(z - (float)iz0);
    uw = mu_noise_smootherstep5(w - (float)iw0);

    c0000 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix0, iy0, iz0, iw0, period_x, period_y, period_z, period_w, seed));
    c1000 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix1, iy0, iz0, iw0, period_x, period_y, period_z, period_w, seed));
    c0100 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix0, iy1, iz0, iw0, period_x, period_y, period_z, period_w, seed));
    c1100 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix1, iy1, iz0, iw0, period_x, period_y, period_z, period_w, seed));
    c0010 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix0, iy0, iz1, iw0, period_x, period_y, period_z, period_w, seed));
    c1010 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix1, iy0, iz1, iw0, period_x, period_y, period_z, period_w, seed));
    c0110 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix0, iy1, iz1, iw0, period_x, period_y, period_z, period_w, seed));
    c1110 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix1, iy1, iz1, iw0, period_x, period_y, period_z, period_w, seed));
    c0001 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix0, iy0, iz0, iw1, period_x, period_y, period_z, period_w, seed));
    c1001 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix1, iy0, iz0, iw1, period_x, period_y, period_z, period_w, seed));
    c0101 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix0, iy1, iz0, iw1, period_x, period_y, period_z, period_w, seed));
    c1101 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix1, iy1, iz0, iw1, period_x, period_y, period_z, period_w, seed));
    c0011 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix0, iy0, iz1, iw1, period_x, period_y, period_z, period_w, seed));
    c1011 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix1, iy0, iz1, iw1, period_x, period_y, period_z, period_w, seed));
    c0111 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix0, iy1, iz1, iw1, period_x, period_y, period_z, period_w, seed));
    c1111 = mu_noise_hash_to_signed_float(mu_noise_hash_4i_periodic(ix1, iy1, iz1, iw1, period_x, period_y, period_z, period_w, seed));

    x000 = mu_noise_lerp(c0000, c1000, ux);
    x100 = mu_noise_lerp(c0100, c1100, ux);
    x010 = mu_noise_lerp(c0010, c1010, ux);
    x110 = mu_noise_lerp(c0110, c1110, ux);
    x001 = mu_noise_lerp(c0001, c1001, ux);
    x101 = mu_noise_lerp(c0101, c1101, ux);
    x011 = mu_noise_lerp(c0011, c1011, ux);
    x111 = mu_noise_lerp(c0111, c1111, ux);

    y00 = mu_noise_lerp(x000, x100, uy);
    y10 = mu_noise_lerp(x010, x110, uy);
    y01 = mu_noise_lerp(x001, x101, uy);
    y11 = mu_noise_lerp(x011, x111, uy);

    z0 = mu_noise_lerp(y00, y10, uz);
    z1 = mu_noise_lerp(y01, y11, uz);

    return mu_noise_lerp(z0, z1, uw);
}

/* ------------------------------------------------------------------------- */
/* 3) Gradient Perlin noise                                                  */
/* ------------------------------------------------------------------------- */

/*
    Perlin idea:
      - At each lattice corner, choose pseudo-random gradient vector g.
      - Compute local offset d = p - corner.
      - Corner contribution is dot(g, d).
      - Smoothly blend corner contributions.

    Why dot(g,d)?
      It creates directional variation while preserving continuity.
*/

MU_INLINE mu_noise_vec2 mu_noise_grad2_from_hash(uint32_t h)
{
    /* 8 directions around the unit circle (cheap and isotropic enough). */
    switch (h & 7U)
    {
        case 0U: return mu_noise_v2( 1.0f,  0.0f);
        case 1U: return mu_noise_v2(-1.0f,  0.0f);
        case 2U: return mu_noise_v2( 0.0f,  1.0f);
        case 3U: return mu_noise_v2( 0.0f, -1.0f);
        case 4U: return mu_noise_v2( 0.70710678f,  0.70710678f);
        case 5U: return mu_noise_v2(-0.70710678f,  0.70710678f);
        case 6U: return mu_noise_v2( 0.70710678f, -0.70710678f);
        default: return mu_noise_v2(-0.70710678f, -0.70710678f);
    }
}

MU_INLINE mu_noise_vec3 mu_noise_grad3_from_hash(uint32_t h)
{
    /* 12 classic Perlin gradients in 3D. */
    switch (h % 12U)
    {
        case 0U:  return mu_noise_v3( 1.0f,  1.0f,  0.0f);
        case 1U:  return mu_noise_v3(-1.0f,  1.0f,  0.0f);
        case 2U:  return mu_noise_v3( 1.0f, -1.0f,  0.0f);
        case 3U:  return mu_noise_v3(-1.0f, -1.0f,  0.0f);
        case 4U:  return mu_noise_v3( 1.0f,  0.0f,  1.0f);
        case 5U:  return mu_noise_v3(-1.0f,  0.0f,  1.0f);
        case 6U:  return mu_noise_v3( 1.0f,  0.0f, -1.0f);
        case 7U:  return mu_noise_v3(-1.0f,  0.0f, -1.0f);
        case 8U:  return mu_noise_v3( 0.0f,  1.0f,  1.0f);
        case 9U:  return mu_noise_v3( 0.0f, -1.0f,  1.0f);
        case 10U: return mu_noise_v3( 0.0f,  1.0f, -1.0f);
        default:  return mu_noise_v3( 0.0f, -1.0f, -1.0f);
    }
}

/*
    4D Perlin gradients.

    The 32 directions are generated from a 5-bit hash:
      bits 0..1: which axis is zero
      bits 2..4: signs of the other three axes

    This is equivalent to the classic 32-entry table but avoids a table in
    the header. Each vector has length sqrt(3).
*/
MU_INLINE mu_noise_vec4 mu_noise_grad4_from_hash(uint32_t h)
{
    unsigned zero_axis = h & 3U;
    float a = (h & 4U)  ? -1.0f : 1.0f;
    float b = (h & 8U)  ? -1.0f : 1.0f;
    float c = (h & 16U) ? -1.0f : 1.0f;

    switch (zero_axis)
    {
        case 0U: return mu_noise_v4(0.0f, a, b, c);
        case 1U: return mu_noise_v4(a, 0.0f, b, c);
        case 2U: return mu_noise_v4(a, b, 0.0f, c);
        default: return mu_noise_v4(a, b, c, 0.0f);
    }
}

MU_INLINE float mu_noise_perlin1(float x, uint32_t seed)
{
    int i0 = (int)floorf(x);
    int i1 = i0 + 1;

    float t = x - (float)i0;
    float u = mu_noise_smootherstep5(t);

    float g0 = (mu_noise_hash_1i(i0, seed) & 1U) ? 1.0f : -1.0f;
    float g1 = (mu_noise_hash_1i(i1, seed) & 1U) ? 1.0f : -1.0f;

    float n0 = g0 * t;
    float n1 = g1 * (t - 1.0f);

    /* Scale to roughly [-1,1]. */
    return 2.0f * mu_noise_lerp(n0, n1, u);
}

MU_INLINE float mu_noise_perlin2(float x, float y, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;

    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);

    mu_noise_vec2 g00 = mu_noise_grad2_from_hash(mu_noise_hash_2i(ix0, iy0, seed));
    mu_noise_vec2 g10 = mu_noise_grad2_from_hash(mu_noise_hash_2i(ix1, iy0, seed));
    mu_noise_vec2 g01 = mu_noise_grad2_from_hash(mu_noise_hash_2i(ix0, iy1, seed));
    mu_noise_vec2 g11 = mu_noise_grad2_from_hash(mu_noise_hash_2i(ix1, iy1, seed));

    float n00 = mu_noise_v2_dot(g00, mu_noise_v2(tx,        ty));
    float n10 = mu_noise_v2_dot(g10, mu_noise_v2(tx - 1.0f, ty));
    float n01 = mu_noise_v2_dot(g01, mu_noise_v2(tx,        ty - 1.0f));
    float n11 = mu_noise_v2_dot(g11, mu_noise_v2(tx - 1.0f, ty - 1.0f));

    float a = mu_noise_lerp(n00, n10, ux);
    float b = mu_noise_lerp(n01, n11, ux);

    /* empirical normalization */
    return 1.41421356f * mu_noise_lerp(a, b, uy);
}

MU_INLINE float mu_noise_perlin3(float x, float y, float z, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int iz0 = (int)floorf(z);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;
    int iz1 = iz0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float tz = z - (float)iz0;

    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float uz = mu_noise_smootherstep5(tz);

    mu_noise_vec3 g000 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix0, iy0, iz0, seed));
    mu_noise_vec3 g100 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix1, iy0, iz0, seed));
    mu_noise_vec3 g010 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix0, iy1, iz0, seed));
    mu_noise_vec3 g110 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix1, iy1, iz0, seed));
    mu_noise_vec3 g001 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix0, iy0, iz1, seed));
    mu_noise_vec3 g101 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix1, iy0, iz1, seed));
    mu_noise_vec3 g011 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix0, iy1, iz1, seed));
    mu_noise_vec3 g111 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix1, iy1, iz1, seed));

    float n000 = mu_noise_v3_dot(g000, mu_noise_v3(tx,        ty,        tz));
    float n100 = mu_noise_v3_dot(g100, mu_noise_v3(tx - 1.0f, ty,        tz));
    float n010 = mu_noise_v3_dot(g010, mu_noise_v3(tx,        ty - 1.0f, tz));
    float n110 = mu_noise_v3_dot(g110, mu_noise_v3(tx - 1.0f, ty - 1.0f, tz));
    float n001 = mu_noise_v3_dot(g001, mu_noise_v3(tx,        ty,        tz - 1.0f));
    float n101 = mu_noise_v3_dot(g101, mu_noise_v3(tx - 1.0f, ty,        tz - 1.0f));
    float n011 = mu_noise_v3_dot(g011, mu_noise_v3(tx,        ty - 1.0f, tz - 1.0f));
    float n111 = mu_noise_v3_dot(g111, mu_noise_v3(tx - 1.0f, ty - 1.0f, tz - 1.0f));

    float x00 = mu_noise_lerp(n000, n100, ux);
    float x10 = mu_noise_lerp(n010, n110, ux);
    float x01 = mu_noise_lerp(n001, n101, ux);
    float x11 = mu_noise_lerp(n011, n111, ux);

    float y0 = mu_noise_lerp(x00, x10, uy);
    float y1 = mu_noise_lerp(x01, x11, uy);

    return 1.15470054f * mu_noise_lerp(y0, y1, uz);
}

/*
    4D Perlin noise.

    Each of the 16 hypercube corners contributes dot(g, p-corner). The
    corner values are then reduced with 15 lerps along x, y, z, w.

    The output normalization is empirical (same approach as the 2D/3D
    variants). It keeps typical values inside [-1, 1] without clamping.
*/
MU_INLINE float mu_noise_perlin4(float x, float y, float z, float w, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int iz0 = (int)floorf(z);
    int iw0 = (int)floorf(w);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;
    int iz1 = iz0 + 1;
    int iw1 = iw0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float tz = z - (float)iz0;
    float tw = w - (float)iw0;

    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float uz = mu_noise_smootherstep5(tz);
    float uw = mu_noise_smootherstep5(tw);

    mu_noise_vec4 g0000 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix0, iy0, iz0, iw0, seed));
    mu_noise_vec4 g1000 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix1, iy0, iz0, iw0, seed));
    mu_noise_vec4 g0100 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix0, iy1, iz0, iw0, seed));
    mu_noise_vec4 g1100 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix1, iy1, iz0, iw0, seed));
    mu_noise_vec4 g0010 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix0, iy0, iz1, iw0, seed));
    mu_noise_vec4 g1010 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix1, iy0, iz1, iw0, seed));
    mu_noise_vec4 g0110 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix0, iy1, iz1, iw0, seed));
    mu_noise_vec4 g1110 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix1, iy1, iz1, iw0, seed));
    mu_noise_vec4 g0001 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix0, iy0, iz0, iw1, seed));
    mu_noise_vec4 g1001 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix1, iy0, iz0, iw1, seed));
    mu_noise_vec4 g0101 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix0, iy1, iz0, iw1, seed));
    mu_noise_vec4 g1101 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix1, iy1, iz0, iw1, seed));
    mu_noise_vec4 g0011 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix0, iy0, iz1, iw1, seed));
    mu_noise_vec4 g1011 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix1, iy0, iz1, iw1, seed));
    mu_noise_vec4 g0111 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix0, iy1, iz1, iw1, seed));
    mu_noise_vec4 g1111 = mu_noise_grad4_from_hash(mu_noise_hash_4i(ix1, iy1, iz1, iw1, seed));

    float n0000 = mu_noise_v4_dot(g0000, mu_noise_v4(tx,        ty,        tz,        tw));
    float n1000 = mu_noise_v4_dot(g1000, mu_noise_v4(tx - 1.0f, ty,        tz,        tw));
    float n0100 = mu_noise_v4_dot(g0100, mu_noise_v4(tx,        ty - 1.0f, tz,        tw));
    float n1100 = mu_noise_v4_dot(g1100, mu_noise_v4(tx - 1.0f, ty - 1.0f, tz,        tw));
    float n0010 = mu_noise_v4_dot(g0010, mu_noise_v4(tx,        ty,        tz - 1.0f, tw));
    float n1010 = mu_noise_v4_dot(g1010, mu_noise_v4(tx - 1.0f, ty,        tz - 1.0f, tw));
    float n0110 = mu_noise_v4_dot(g0110, mu_noise_v4(tx,        ty - 1.0f, tz - 1.0f, tw));
    float n1110 = mu_noise_v4_dot(g1110, mu_noise_v4(tx - 1.0f, ty - 1.0f, tz - 1.0f, tw));
    float n0001 = mu_noise_v4_dot(g0001, mu_noise_v4(tx,        ty,        tz,        tw - 1.0f));
    float n1001 = mu_noise_v4_dot(g1001, mu_noise_v4(tx - 1.0f, ty,        tz,        tw - 1.0f));
    float n0101 = mu_noise_v4_dot(g0101, mu_noise_v4(tx,        ty - 1.0f, tz,        tw - 1.0f));
    float n1101 = mu_noise_v4_dot(g1101, mu_noise_v4(tx - 1.0f, ty - 1.0f, tz,        tw - 1.0f));
    float n0011 = mu_noise_v4_dot(g0011, mu_noise_v4(tx,        ty,        tz - 1.0f, tw - 1.0f));
    float n1011 = mu_noise_v4_dot(g1011, mu_noise_v4(tx - 1.0f, ty,        tz - 1.0f, tw - 1.0f));
    float n0111 = mu_noise_v4_dot(g0111, mu_noise_v4(tx,        ty - 1.0f, tz - 1.0f, tw - 1.0f));
    float n1111 = mu_noise_v4_dot(g1111, mu_noise_v4(tx - 1.0f, ty - 1.0f, tz - 1.0f, tw - 1.0f));

    float x000 = mu_noise_lerp(n0000, n1000, ux);
    float x100 = mu_noise_lerp(n0100, n1100, ux);
    float x010 = mu_noise_lerp(n0010, n1010, ux);
    float x110 = mu_noise_lerp(n0110, n1110, ux);
    float x001 = mu_noise_lerp(n0001, n1001, ux);
    float x101 = mu_noise_lerp(n0101, n1101, ux);
    float x011 = mu_noise_lerp(n0011, n1011, ux);
    float x111 = mu_noise_lerp(n0111, n1111, ux);

    float y00 = mu_noise_lerp(x000, x100, uy);
    float y10 = mu_noise_lerp(x010, x110, uy);
    float y01 = mu_noise_lerp(x001, x101, uy);
    float y11 = mu_noise_lerp(x011, x111, uy);

    float z0 = mu_noise_lerp(y00, y10, uz);
    float z1 = mu_noise_lerp(y01, y11, uz);

    return mu_noise_lerp(z0, z1, uw);
}

MU_INLINE float mu_noise_perlin4_periodic(float x, float y, float z, float w,
                                          int period_x, int period_y,
                                          int period_z, int period_w,
                                          uint32_t seed)
{
    int ix0, iy0, iz0, iw0, ix1, iy1, iz1, iw1;
    float tx, ty, tz, tw, ux, uy, uz, uw;
    mu_noise_vec4 g0000, g1000, g0100, g1100, g0010, g1010, g0110, g1110;
    mu_noise_vec4 g0001, g1001, g0101, g1101, g0011, g1011, g0111, g1111;
    float n0000, n1000, n0100, n1100, n0010, n1010, n0110, n1110;
    float n0001, n1001, n0101, n1101, n0011, n1011, n0111, n1111;
    float x000, x100, x010, x110, x001, x101, x011, x111;
    float y00, y10, y01, y11, z0, z1;

    if (period_x <= 0 || period_y <= 0 || period_z <= 0 || period_w <= 0)
        return mu_noise_perlin4(x, y, z, w, seed);

    ix0 = (int)floorf(x);
    iy0 = (int)floorf(y);
    iz0 = (int)floorf(z);
    iw0 = (int)floorf(w);
    ix1 = ix0 + 1;
    iy1 = iy0 + 1;
    iz1 = iz0 + 1;
    iw1 = iw0 + 1;

    tx = x - (float)ix0;
    ty = y - (float)iy0;
    tz = z - (float)iz0;
    tw = w - (float)iw0;

    ux = mu_noise_smootherstep5(tx);
    uy = mu_noise_smootherstep5(ty);
    uz = mu_noise_smootherstep5(tz);
    uw = mu_noise_smootherstep5(tw);

    g0000 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix0, iy0, iz0, iw0, period_x, period_y, period_z, period_w, seed));
    g1000 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix1, iy0, iz0, iw0, period_x, period_y, period_z, period_w, seed));
    g0100 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix0, iy1, iz0, iw0, period_x, period_y, period_z, period_w, seed));
    g1100 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix1, iy1, iz0, iw0, period_x, period_y, period_z, period_w, seed));
    g0010 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix0, iy0, iz1, iw0, period_x, period_y, period_z, period_w, seed));
    g1010 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix1, iy0, iz1, iw0, period_x, period_y, period_z, period_w, seed));
    g0110 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix0, iy1, iz1, iw0, period_x, period_y, period_z, period_w, seed));
    g1110 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix1, iy1, iz1, iw0, period_x, period_y, period_z, period_w, seed));
    g0001 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix0, iy0, iz0, iw1, period_x, period_y, period_z, period_w, seed));
    g1001 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix1, iy0, iz0, iw1, period_x, period_y, period_z, period_w, seed));
    g0101 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix0, iy1, iz0, iw1, period_x, period_y, period_z, period_w, seed));
    g1101 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix1, iy1, iz0, iw1, period_x, period_y, period_z, period_w, seed));
    g0011 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix0, iy0, iz1, iw1, period_x, period_y, period_z, period_w, seed));
    g1011 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix1, iy0, iz1, iw1, period_x, period_y, period_z, period_w, seed));
    g0111 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix0, iy1, iz1, iw1, period_x, period_y, period_z, period_w, seed));
    g1111 = mu_noise_grad4_from_hash(mu_noise_hash_4i_periodic(ix1, iy1, iz1, iw1, period_x, period_y, period_z, period_w, seed));

    n0000 = mu_noise_v4_dot(g0000, mu_noise_v4(tx,        ty,        tz,        tw));
    n1000 = mu_noise_v4_dot(g1000, mu_noise_v4(tx - 1.0f, ty,        tz,        tw));
    n0100 = mu_noise_v4_dot(g0100, mu_noise_v4(tx,        ty - 1.0f, tz,        tw));
    n1100 = mu_noise_v4_dot(g1100, mu_noise_v4(tx - 1.0f, ty - 1.0f, tz,        tw));
    n0010 = mu_noise_v4_dot(g0010, mu_noise_v4(tx,        ty,        tz - 1.0f, tw));
    n1010 = mu_noise_v4_dot(g1010, mu_noise_v4(tx - 1.0f, ty,        tz - 1.0f, tw));
    n0110 = mu_noise_v4_dot(g0110, mu_noise_v4(tx,        ty - 1.0f, tz - 1.0f, tw));
    n1110 = mu_noise_v4_dot(g1110, mu_noise_v4(tx - 1.0f, ty - 1.0f, tz - 1.0f, tw));
    n0001 = mu_noise_v4_dot(g0001, mu_noise_v4(tx,        ty,        tz,        tw - 1.0f));
    n1001 = mu_noise_v4_dot(g1001, mu_noise_v4(tx - 1.0f, ty,        tz,        tw - 1.0f));
    n0101 = mu_noise_v4_dot(g0101, mu_noise_v4(tx,        ty - 1.0f, tz,        tw - 1.0f));
    n1101 = mu_noise_v4_dot(g1101, mu_noise_v4(tx - 1.0f, ty - 1.0f, tz,        tw - 1.0f));
    n0011 = mu_noise_v4_dot(g0011, mu_noise_v4(tx,        ty,        tz - 1.0f, tw - 1.0f));
    n1011 = mu_noise_v4_dot(g1011, mu_noise_v4(tx - 1.0f, ty,        tz - 1.0f, tw - 1.0f));
    n0111 = mu_noise_v4_dot(g0111, mu_noise_v4(tx,        ty - 1.0f, tz - 1.0f, tw - 1.0f));
    n1111 = mu_noise_v4_dot(g1111, mu_noise_v4(tx - 1.0f, ty - 1.0f, tz - 1.0f, tw - 1.0f));

    x000 = mu_noise_lerp(n0000, n1000, ux);
    x100 = mu_noise_lerp(n0100, n1100, ux);
    x010 = mu_noise_lerp(n0010, n1010, ux);
    x110 = mu_noise_lerp(n0110, n1110, ux);
    x001 = mu_noise_lerp(n0001, n1001, ux);
    x101 = mu_noise_lerp(n0101, n1101, ux);
    x011 = mu_noise_lerp(n0011, n1011, ux);
    x111 = mu_noise_lerp(n0111, n1111, ux);

    y00 = mu_noise_lerp(x000, x100, uy);
    y10 = mu_noise_lerp(x010, x110, uy);
    y01 = mu_noise_lerp(x001, x101, uy);
    y11 = mu_noise_lerp(x011, x111, uy);

    z0 = mu_noise_lerp(y00, y10, uz);
    z1 = mu_noise_lerp(y01, y11, uz);

    return mu_noise_lerp(z0, z1, uw);
}

MU_INLINE float mu_noise_perlin1_periodic(float x, int period_x, uint32_t seed)
{
    if (period_x <= 0) return mu_noise_perlin1(x, seed);

    {
        int i0 = (int)floorf(x);
        int i1 = i0 + 1;
        float t = x - (float)i0;
        float u = mu_noise_smootherstep5(t);

        float g0 = (mu_noise_hash_1i_periodic(i0, period_x, seed) & 1U) ? 1.0f : -1.0f;
        float g1 = (mu_noise_hash_1i_periodic(i1, period_x, seed) & 1U) ? 1.0f : -1.0f;
        float n0 = g0 * t;
        float n1 = g1 * (t - 1.0f);
        return 2.0f * mu_noise_lerp(n0, n1, u);
    }
}

MU_INLINE float mu_noise_perlin2_periodic(float x, float y, int period_x, int period_y, uint32_t seed)
{
    if (period_x <= 0 || period_y <= 0) return mu_noise_perlin2(x, y, seed);

    {
        int ix0 = (int)floorf(x);
        int iy0 = (int)floorf(y);
        int ix1 = ix0 + 1;
        int iy1 = iy0 + 1;

        float tx = x - (float)ix0;
        float ty = y - (float)iy0;
        float ux = mu_noise_smootherstep5(tx);
        float uy = mu_noise_smootherstep5(ty);

        mu_noise_vec2 g00 = mu_noise_grad2_from_hash(mu_noise_hash_2i_periodic(ix0, iy0, period_x, period_y, seed));
        mu_noise_vec2 g10 = mu_noise_grad2_from_hash(mu_noise_hash_2i_periodic(ix1, iy0, period_x, period_y, seed));
        mu_noise_vec2 g01 = mu_noise_grad2_from_hash(mu_noise_hash_2i_periodic(ix0, iy1, period_x, period_y, seed));
        mu_noise_vec2 g11 = mu_noise_grad2_from_hash(mu_noise_hash_2i_periodic(ix1, iy1, period_x, period_y, seed));

        float n00 = mu_noise_v2_dot(g00, mu_noise_v2(tx,        ty));
        float n10 = mu_noise_v2_dot(g10, mu_noise_v2(tx - 1.0f, ty));
        float n01 = mu_noise_v2_dot(g01, mu_noise_v2(tx,        ty - 1.0f));
        float n11 = mu_noise_v2_dot(g11, mu_noise_v2(tx - 1.0f, ty - 1.0f));

        {
            float a = mu_noise_lerp(n00, n10, ux);
            float b = mu_noise_lerp(n01, n11, ux);
            return 1.41421356f * mu_noise_lerp(a, b, uy);
        }
    }
}

MU_INLINE float mu_noise_perlin3_periodic(float x, float y, float z, int period_x, int period_y, int period_z, uint32_t seed)
{
    if (period_x <= 0 || period_y <= 0 || period_z <= 0) return mu_noise_perlin3(x, y, z, seed);

    {
        int ix0 = (int)floorf(x);
        int iy0 = (int)floorf(y);
        int iz0 = (int)floorf(z);
        int ix1 = ix0 + 1;
        int iy1 = iy0 + 1;
        int iz1 = iz0 + 1;

        float tx = x - (float)ix0;
        float ty = y - (float)iy0;
        float tz = z - (float)iz0;

        float ux = mu_noise_smootherstep5(tx);
        float uy = mu_noise_smootherstep5(ty);
        float uz = mu_noise_smootherstep5(tz);

        mu_noise_vec3 g000 = mu_noise_grad3_from_hash(mu_noise_hash_3i_periodic(ix0, iy0, iz0, period_x, period_y, period_z, seed));
        mu_noise_vec3 g100 = mu_noise_grad3_from_hash(mu_noise_hash_3i_periodic(ix1, iy0, iz0, period_x, period_y, period_z, seed));
        mu_noise_vec3 g010 = mu_noise_grad3_from_hash(mu_noise_hash_3i_periodic(ix0, iy1, iz0, period_x, period_y, period_z, seed));
        mu_noise_vec3 g110 = mu_noise_grad3_from_hash(mu_noise_hash_3i_periodic(ix1, iy1, iz0, period_x, period_y, period_z, seed));
        mu_noise_vec3 g001 = mu_noise_grad3_from_hash(mu_noise_hash_3i_periodic(ix0, iy0, iz1, period_x, period_y, period_z, seed));
        mu_noise_vec3 g101 = mu_noise_grad3_from_hash(mu_noise_hash_3i_periodic(ix1, iy0, iz1, period_x, period_y, period_z, seed));
        mu_noise_vec3 g011 = mu_noise_grad3_from_hash(mu_noise_hash_3i_periodic(ix0, iy1, iz1, period_x, period_y, period_z, seed));
        mu_noise_vec3 g111 = mu_noise_grad3_from_hash(mu_noise_hash_3i_periodic(ix1, iy1, iz1, period_x, period_y, period_z, seed));

        float n000 = mu_noise_v3_dot(g000, mu_noise_v3(tx,        ty,        tz));
        float n100 = mu_noise_v3_dot(g100, mu_noise_v3(tx - 1.0f, ty,        tz));
        float n010 = mu_noise_v3_dot(g010, mu_noise_v3(tx,        ty - 1.0f, tz));
        float n110 = mu_noise_v3_dot(g110, mu_noise_v3(tx - 1.0f, ty - 1.0f, tz));
        float n001 = mu_noise_v3_dot(g001, mu_noise_v3(tx,        ty,        tz - 1.0f));
        float n101 = mu_noise_v3_dot(g101, mu_noise_v3(tx - 1.0f, ty,        tz - 1.0f));
        float n011 = mu_noise_v3_dot(g011, mu_noise_v3(tx,        ty - 1.0f, tz - 1.0f));
        float n111 = mu_noise_v3_dot(g111, mu_noise_v3(tx - 1.0f, ty - 1.0f, tz - 1.0f));

        {
            float x00 = mu_noise_lerp(n000, n100, ux);
            float x10 = mu_noise_lerp(n010, n110, ux);
            float x01 = mu_noise_lerp(n001, n101, ux);
            float x11 = mu_noise_lerp(n011, n111, ux);
            float y0 = mu_noise_lerp(x00, x10, uy);
            float y1 = mu_noise_lerp(x01, x11, uy);
            return 1.15470054f * mu_noise_lerp(y0, y1, uz);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 4) Simplex noise                                                          */
/* ------------------------------------------------------------------------- */

/*
    SIMPLEX 2D IDEA (visual):

        square grid --(skew)--> equilateral triangle grid

        We find triangle containing point p, then evaluate ONLY 3 corners.

        contribution from each corner c:
          t = r^2 - ||p-c||^2
          if t > 0:  n += t^4 * dot(grad(c), p-c)

        t^4 gives compact support and smooth decay.
*/

MU_INLINE float mu_noise_simplex2(float x, float y, uint32_t seed)
{
    const float F2 = 0.36602540378443864676f; /* 0.5*(sqrt(3)-1) */
    const float G2 = 0.21132486540518711775f; /* (3-sqrt(3))/6 */

    float s = (x + y) * F2;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);

    float t = (float)(i + j) * G2;
    float X0 = (float)i - t;
    float Y0 = (float)j - t;
    float x0 = x - X0;
    float y0 = y - Y0;

    int i1;
    int j1;
    if (x0 > y0) { i1 = 1; j1 = 0; }
    else         { i1 = 0; j1 = 1; }

    float x1 = x0 - (float)i1 + G2;
    float y1 = y0 - (float)j1 + G2;
    float x2 = x0 - 1.0f + 2.0f * G2;
    float y2 = y0 - 1.0f + 2.0f * G2;

    float n0 = 0.0f;
    float n1 = 0.0f;
    float n2 = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0.0f)
    {
        mu_noise_vec2 g0 = mu_noise_grad2_from_hash(mu_noise_hash_2i(i, j, seed));
        t0 *= t0;
        n0 = t0 * t0 * mu_noise_v2_dot(g0, mu_noise_v2(x0, y0));
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0.0f)
    {
        mu_noise_vec2 g1 = mu_noise_grad2_from_hash(mu_noise_hash_2i(i + i1, j + j1, seed));
        t1 *= t1;
        n1 = t1 * t1 * mu_noise_v2_dot(g1, mu_noise_v2(x1, y1));
    }

    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0.0f)
    {
        mu_noise_vec2 g2 = mu_noise_grad2_from_hash(mu_noise_hash_2i(i + 1, j + 1, seed));
        t2 *= t2;
        n2 = t2 * t2 * mu_noise_v2_dot(g2, mu_noise_v2(x2, y2));
    }

    return 70.0f * (n0 + n1 + n2);
}

MU_INLINE float mu_noise_simplex3(float x, float y, float z, uint32_t seed)
{
    const float F3 = 1.0f / 3.0f;
    const float G3 = 1.0f / 6.0f;

    float s = (x + y + z) * F3;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);
    int k = (int)floorf(z + s);

    float t = (float)(i + j + k) * G3;
    float X0 = (float)i - t;
    float Y0 = (float)j - t;
    float Z0 = (float)k - t;

    float x0 = x - X0;
    float y0 = y - Y0;
    float z0 = z - Z0;

    int i1, j1, k1;
    int i2, j2, k2;

    if (x0 >= y0)
    {
        if (y0 >= z0)
        {
            i1 = 1; j1 = 0; k1 = 0;
            i2 = 1; j2 = 1; k2 = 0;
        }
        else if (x0 >= z0)
        {
            i1 = 1; j1 = 0; k1 = 0;
            i2 = 1; j2 = 0; k2 = 1;
        }
        else
        {
            i1 = 0; j1 = 0; k1 = 1;
            i2 = 1; j2 = 0; k2 = 1;
        }
    }
    else
    {
        if (y0 < z0)
        {
            i1 = 0; j1 = 0; k1 = 1;
            i2 = 0; j2 = 1; k2 = 1;
        }
        else if (x0 < z0)
        {
            i1 = 0; j1 = 1; k1 = 0;
            i2 = 0; j2 = 1; k2 = 1;
        }
        else
        {
            i1 = 0; j1 = 1; k1 = 0;
            i2 = 1; j2 = 1; k2 = 0;
        }
    }

    float x1 = x0 - (float)i1 + G3;
    float y1 = y0 - (float)j1 + G3;
    float z1 = z0 - (float)k1 + G3;

    float x2 = x0 - (float)i2 + 2.0f * G3;
    float y2 = y0 - (float)j2 + 2.0f * G3;
    float z2 = z0 - (float)k2 + 2.0f * G3;

    float x3 = x0 - 1.0f + 3.0f * G3;
    float y3 = y0 - 1.0f + 3.0f * G3;
    float z3 = z0 - 1.0f + 3.0f * G3;

    float n0 = 0.0f;
    float n1 = 0.0f;
    float n2 = 0.0f;
    float n3 = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0 - z0 * z0;
    if (t0 > 0.0f)
    {
        mu_noise_vec3 g0 = mu_noise_grad3_from_hash(mu_noise_hash_3i(i, j, k, seed));
        t0 *= t0;
        n0 = t0 * t0 * mu_noise_v3_dot(g0, mu_noise_v3(x0, y0, z0));
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1 - z1 * z1;
    if (t1 > 0.0f)
    {
        mu_noise_vec3 g1 = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + i1, j + j1, k + k1, seed));
        t1 *= t1;
        n1 = t1 * t1 * mu_noise_v3_dot(g1, mu_noise_v3(x1, y1, z1));
    }

    float t2 = 0.5f - x2 * x2 - y2 * y2 - z2 * z2;
    if (t2 > 0.0f)
    {
        mu_noise_vec3 g2 = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + i2, j + j2, k + k2, seed));
        t2 *= t2;
        n2 = t2 * t2 * mu_noise_v3_dot(g2, mu_noise_v3(x2, y2, z2));
    }

    float t3 = 0.5f - x3 * x3 - y3 * y3 - z3 * z3;
    if (t3 > 0.0f)
    {
        mu_noise_vec3 g3 = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + 1, j + 1, k + 1, seed));
        t3 *= t3;
        n3 = t3 * t3 * mu_noise_v3_dot(g3, mu_noise_v3(x3, y3, z3));
    }

    return 72.0f * (n0 + n1 + n2 + n3);
}

/*
    4D simplex noise.

    Unskew the input into a 4D simplex lattice, rank the four local
    coordinates, then visit five corners:

        corner 0: no increment
        corner 1: increment the largest coordinate
        corner 2: increment the two largest
        corner 3: increment the three largest
        corner 4: increment all four

    Ranking replaces the long nested comparison trees used by some
    implementations and makes the corner selection easy to audit.
*/
MU_INLINE float mu_noise_simplex4(float x, float y, float z, float w, uint32_t seed)
{
    const float F4 = 0.30901699437494742410f; /* (sqrt(5)-1)/4 */
    const float G4 = 0.13819660112501051518f; /* (5-sqrt(5))/20 */

    float s = (x + y + z + w) * F4;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);
    int k = (int)floorf(z + s);
    int l = (int)floorf(w + s);

    float t = (float)(i + j + k + l) * G4;
    float x0 = x - ((float)i - t);
    float y0 = y - ((float)j - t);
    float z0 = z - ((float)k - t);
    float w0 = w - ((float)l - t);

    int rx = (x0 > y0) + (x0 > z0) + (x0 > w0);
    int ry = (y0 > x0) + (y0 > z0) + (y0 > w0);
    int rz = (z0 > x0) + (z0 > y0) + (z0 > w0);
    int rw = (w0 > x0) + (w0 > y0) + (w0 > z0);

    int i1 = (rx >= 3);
    int j1 = (ry >= 3);
    int k1 = (rz >= 3);
    int l1 = (rw >= 3);

    int i2 = (rx >= 2);
    int j2 = (ry >= 2);
    int k2 = (rz >= 2);
    int l2 = (rw >= 2);

    int i3 = (rx >= 1);
    int j3 = (ry >= 1);
    int k3 = (rz >= 1);
    int l3 = (rw >= 1);

    float x1 = x0 - (float)i1 + G4;
    float y1 = y0 - (float)j1 + G4;
    float z1 = z0 - (float)k1 + G4;
    float w1 = w0 - (float)l1 + G4;

    float x2 = x0 - (float)i2 + 2.0f * G4;
    float y2 = y0 - (float)j2 + 2.0f * G4;
    float z2 = z0 - (float)k2 + 2.0f * G4;
    float w2 = w0 - (float)l2 + 2.0f * G4;

    float x3 = x0 - (float)i3 + 3.0f * G4;
    float y3 = y0 - (float)j3 + 3.0f * G4;
    float z3 = z0 - (float)k3 + 3.0f * G4;
    float w3 = w0 - (float)l3 + 3.0f * G4;

    float x4 = x0 - 1.0f + 4.0f * G4;
    float y4 = y0 - 1.0f + 4.0f * G4;
    float z4 = z0 - 1.0f + 4.0f * G4;
    float w4 = w0 - 1.0f + 4.0f * G4;

    float n0 = 0.0f;
    float n1 = 0.0f;
    float n2 = 0.0f;
    float n3 = 0.0f;
    float n4 = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0 - z0 * z0 - w0 * w0;
    if (t0 > 0.0f)
    {
        mu_noise_vec4 g0 = mu_noise_grad4_from_hash(mu_noise_hash_4i(i, j, k, l, seed));
        t0 *= t0;
        n0 = t0 * t0 * mu_noise_v4_dot(g0, mu_noise_v4(x0, y0, z0, w0));
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1 - z1 * z1 - w1 * w1;
    if (t1 > 0.0f)
    {
        mu_noise_vec4 g1 = mu_noise_grad4_from_hash(mu_noise_hash_4i(i + i1, j + j1, k + k1, l + l1, seed));
        t1 *= t1;
        n1 = t1 * t1 * mu_noise_v4_dot(g1, mu_noise_v4(x1, y1, z1, w1));
    }

    float t2 = 0.5f - x2 * x2 - y2 * y2 - z2 * z2 - w2 * w2;
    if (t2 > 0.0f)
    {
        mu_noise_vec4 g2 = mu_noise_grad4_from_hash(mu_noise_hash_4i(i + i2, j + j2, k + k2, l + l2, seed));
        t2 *= t2;
        n2 = t2 * t2 * mu_noise_v4_dot(g2, mu_noise_v4(x2, y2, z2, w2));
    }

    float t3 = 0.5f - x3 * x3 - y3 * y3 - z3 * z3 - w3 * w3;
    if (t3 > 0.0f)
    {
        mu_noise_vec4 g3 = mu_noise_grad4_from_hash(mu_noise_hash_4i(i + i3, j + j3, k + k3, l + l3, seed));
        t3 *= t3;
        n3 = t3 * t3 * mu_noise_v4_dot(g3, mu_noise_v4(x3, y3, z3, w3));
    }

    float t4 = 0.5f - x4 * x4 - y4 * y4 - z4 * z4 - w4 * w4;
    if (t4 > 0.0f)
    {
        mu_noise_vec4 g4 = mu_noise_grad4_from_hash(mu_noise_hash_4i(i + 1, j + 1, k + 1, l + 1, seed));
        t4 *= t4;
        n4 = t4 * t4 * mu_noise_v4_dot(g4, mu_noise_v4(x4, y4, z4, w4));
    }

    return 62.0f * (n0 + n1 + n2 + n3 + n4);
}

/* ------------------------------------------------------------------------- */
/* 4b) OpenSimplex2 / OpenSimplex2S-inspired variants                       */
/* ------------------------------------------------------------------------- */

/*
    These variants are intentionally lightweight and branch-free wrappers
    around the core simplex sampler:

    - OpenSimplex2-like: rotate domain before simplex sampling to reduce
      axis-aligned artifacts.
    - OpenSimplex2S-like: blend two differently shifted OpenSimplex2-like
      samples for a softer, less directional spectrum.

    Notes:
    - They are compatible in spirit/style with OpenSimplex2 families.
    - They keep this header compact and dependency-free.
*/
MU_INLINE float mu_noise_opensimplex2_2d(float x, float y, uint32_t seed)
{
    const float r = 0.70710678118654752440f;
    float xr = (x + y) * r;
    float yr = (y - x) * r;
    return mu_noise_simplex2(xr, yr, seed ^ 0x9E3779B9u);
}

MU_INLINE float mu_noise_opensimplex2s_2d(float x, float y, uint32_t seed)
{
    float a = mu_noise_opensimplex2_2d(x, y, seed);
    float b = mu_noise_opensimplex2_2d(x + 31.416f, y - 19.731f, seed ^ 0x85EBCA77u);
    return 0.5f * (a + b);
}

MU_INLINE float mu_noise_opensimplex2_3d(float x, float y, float z, uint32_t seed)
{
    const float k1 = 0.70710678118654752440f; /* 1/sqrt(2) */
    const float k2 = 0.40824829046386301637f; /* 1/sqrt(6) */
    const float k3 = 0.57735026918962576451f; /* 1/sqrt(3) */

    float xr = (x - y) * k1;
    float yr = (x + y - 2.0f * z) * k2;
    float zr = (x + y + z) * k3;

    return mu_noise_simplex3(xr, yr, zr, seed ^ 0xC2B2AE35u);
}

MU_INLINE float mu_noise_opensimplex2s_3d(float x, float y, float z, uint32_t seed)
{
    float a = mu_noise_opensimplex2_3d(x, y, z, seed);
    float b = mu_noise_opensimplex2_3d(x + 17.0f, y - 23.0f, z + 11.0f, seed ^ 0x27D4EB2Fu);
    return 0.5f * (a + b);
}

/*
    Tileable simplex wrappers.

    Classic simplex itself is not naturally periodic in Cartesian coordinates.
    To build seamless tiles, we periodicize by blending wrapped domain samples.

    2D visual:
       N(x,y) = bilerp(
         S(xr,      yr),
         S(xr-px,   yr),
         S(xr,    yr-py),
         S(xr-px, yr-py),
         tx, ty)

      xr = wrap(x, px), tx = xr / px, etc.
*/
/*
    Native periodic simplex.

    Ordinary simplex uses a skewed lattice that does not align with Cartesian
    axes, so wrapping its integer corners does not tile along x/y/z. These
    functions use an axis-aligned simplex (A_n) grid instead:

      2D:  u = x + 0.5*y,        v = y
      3D:  u = y + z,            v = x + z,        w = x + y
      4D:  u = y + z + w,        v = x + z + w,
           t = x + y + w,        q = x + y + z

    Changing one Cartesian coordinate by an integer period shifts exactly one
    of the transformed coordinates by that period, so the simplex lattice tiles.
    The 2D/3D versions wrap Cartesian corner coordinates before hashing. The
    4D version hashes the transformed lattice indices with periodic hash
    lookups, which keeps its periodicity exact without introducing a larger
    Cartesian wrap. No domain blending and no multiple simplex evaluations are
    required. The grids are slightly stretched compared with ordinary simplex.

    Periods are positive integer lattice periods. If any period is <= 0, the
    corresponding non-periodic standard simplex sampler is used.
*/
MU_INLINE float mu_noise_simplex2_periodic_impl(float x, float y,
                                                int period_x, int period_y,
                                                uint32_t seed)
{
    float px = (float)period_x;
    float py = (float)period_y;
    int ii[3];
    int jj[3];
    float sum = 0.0f;

    x = mu_noise_wrap_period(x, px);
    y = mu_noise_wrap_period(y, py);

    {
        float u = x + 0.5f * y;
        float v = y;
        int i0 = (int)floorf(u);
        int j0 = (int)floorf(v);
        float fu = u - (float)i0;
        float fv = v - (float)j0;
        int i1 = (fu > fv) ? 1 : 0;
        int j1 = (fu > fv) ? 0 : 1;

        ii[0] = i0;     jj[0] = j0;
        ii[1] = i0 + i1; jj[1] = j0 + j1;
        ii[2] = i0 + 1;  jj[2] = j0 + 1;
    }

    for (int n = 0; n < 3; ++n)
    {
        float cx = (float)ii[n] - 0.5f * (float)jj[n];
        float cy = (float)jj[n];
        float wx = mu_noise_wrap_period(cx, px);
        float wy = mu_noise_wrap_period(cy, py);
        int hx = (int)floorf(wx + 0.5f * wy + 0.5f);
        int hy = (int)floorf(wy + 0.5f);
        mu_noise_vec2 g = mu_noise_grad2_from_hash(mu_noise_hash_2i(hx, hy, seed));
        float dx = x - cx;
        float dy = y - cy;
        float t = 0.8f - dx * dx - dy * dy;

        if (t > 0.0f)
        {
            t *= t;
            sum += t * t * mu_noise_v2_dot(g, mu_noise_v2(dx, dy));
        }
    }

    return 11.0f * sum;
}

MU_INLINE float mu_noise_simplex3_periodic_impl(float x, float y, float z,
                                                int period_x, int period_y,
                                                int period_z, uint32_t seed)
{
    float px = (float)period_x;
    float py = (float)period_y;
    float pz = (float)period_z;
    int ii[4];
    int jj[4];
    int kk[4];
    float sum = 0.0f;

    x = mu_noise_wrap_period(x, px);
    y = mu_noise_wrap_period(y, py);
    z = mu_noise_wrap_period(z, pz);

    {
        float u = y + z;
        float v = x + z;
        float w = x + y;
        int i0 = (int)floorf(u);
        int j0 = (int)floorf(v);
        int k0 = (int)floorf(w);
        float fu = u - (float)i0;
        float fv = v - (float)j0;
        float fw = w - (float)k0;
        int i1, j1, k1;
        int i2, j2, k2;

        if (fu >= fv)
        {
            if (fv >= fw) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
            else if (fu >= fw) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
            else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
        }
        else
        {
            if (fv < fw) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
            else if (fu < fw) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
            else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        }

        ii[0] = i0;      jj[0] = j0;      kk[0] = k0;
        ii[1] = i0 + i1; jj[1] = j0 + j1; kk[1] = k0 + k1;
        ii[2] = i0 + i2; jj[2] = j0 + j2; kk[2] = k0 + k2;
        ii[3] = i0 + 1;  jj[3] = j0 + 1;  kk[3] = k0 + 1;
    }

    for (int n = 0; n < 4; ++n)
    {
        float cx = 0.5f * (-(float)ii[n] + (float)jj[n] + (float)kk[n]);
        float cy = 0.5f * ((float)ii[n] - (float)jj[n] + (float)kk[n]);
        float cz = 0.5f * ((float)ii[n] + (float)jj[n] - (float)kk[n]);
        float wx = mu_noise_wrap_period(cx, px);
        float wy = mu_noise_wrap_period(cy, py);
        float wz = mu_noise_wrap_period(cz, pz);
        int hx = (int)floorf(wy + wz + 0.5f);
        int hy = (int)floorf(wx + wz + 0.5f);
        int hz = (int)floorf(wx + wy + 0.5f);
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(hx, hy, hz, seed));
        float dx = x - cx;
        float dy = y - cy;
        float dz = z - cz;
        float t = 0.5f - dx * dx - dy * dy - dz * dz;

        if (t > 0.0f)
        {
            t *= t;
            sum += t * t * mu_noise_v3_dot(g, mu_noise_v3(dx, dy, dz));
        }
    }

    return 72.0f * sum;
}

MU_INLINE float mu_noise_simplex4_periodic_impl(float x, float y, float z, float w,
                                                int period_x, int period_y,
                                                int period_z, int period_w,
                                                uint32_t seed)
{
    int ii[5];
    int jj[5];
    int kk[5];
    int ll[5];
    float sum = 0.0f;

    {
        float u = y + z + w;
        float v = x + z + w;
        float t = x + y + w;
        float q = x + y + z;
        int i0 = (int)floorf(u);
        int j0 = (int)floorf(v);
        int k0 = (int)floorf(t);
        int l0 = (int)floorf(q);
        float fu = u - (float)i0;
        float fv = v - (float)j0;
        float fw = t - (float)k0;
        float fx = q - (float)l0;
        int qu = (int)(fu * 65536.0f + 0.5f);
        int qv = (int)(fv * 65536.0f + 0.5f);
        int qw = (int)(fw * 65536.0f + 0.5f);
        int qx = (int)(fx * 65536.0f + 0.5f);
        int rx = (qu > qv || (qu == qv && 0 > 1)) +
                 (qu > qw || (qu == qw && 0 > 2)) +
                 (qu > qx || (qu == qx && 0 > 3));
        int ry = (qv > qu || (qv == qu && 1 > 0)) +
                 (qv > qw || (qv == qw && 1 > 2)) +
                 (qv > qx || (qv == qx && 1 > 3));
        int rz = (qw > qu || (qw == qu && 2 > 0)) +
                 (qw > qv || (qw == qv && 2 > 1)) +
                 (qw > qx || (qw == qx && 2 > 3));
        int rw = (qx > qu || (qx == qu && 3 > 0)) +
                 (qx > qv || (qx == qv && 3 > 1)) +
                 (qx > qw || (qx == qw && 3 > 2));
        int i1 = (rx >= 3);
        int j1 = (ry >= 3);
        int k1 = (rz >= 3);
        int l1 = (rw >= 3);
        int i2 = (rx >= 2);
        int j2 = (ry >= 2);
        int k2 = (rz >= 2);
        int l2 = (rw >= 2);
        int i3 = (rx >= 1);
        int j3 = (ry >= 1);
        int k3 = (rz >= 1);
        int l3 = (rw >= 1);

        ii[0] = i0;      jj[0] = j0;      kk[0] = k0;      ll[0] = l0;
        ii[1] = i0 + i1; jj[1] = j0 + j1; kk[1] = k0 + k1; ll[1] = l0 + l1;
        ii[2] = i0 + i2; jj[2] = j0 + j2; kk[2] = k0 + k2; ll[2] = l0 + l2;
        ii[3] = i0 + i3; jj[3] = j0 + j3; kk[3] = k0 + k3; ll[3] = l0 + l3;
        ii[4] = i0 + 1;  jj[4] = j0 + 1;  kk[4] = k0 + 1;  ll[4] = l0 + 1;
    }

    for (int n = 0; n < 5; ++n)
    {
        float si = (float)ii[n];
        float sj = (float)jj[n];
        float sk = (float)kk[n];
        float sl = (float)ll[n];
        float cx = (-2.0f * si + sj + sk + sl) / 3.0f;
        float cy = (si - 2.0f * sj + sk + sl) / 3.0f;
        float cz = (si + sj - 2.0f * sk + sl) / 3.0f;
        float cw = (si + sj + sk - 2.0f * sl) / 3.0f;
        mu_noise_vec4 g = mu_noise_grad4_from_hash(
            mu_noise_hash_4i_periodic(ii[n], jj[n], kk[n], ll[n],
                                      period_x, period_y, period_z, period_w,
                                      seed));
        float dx = x - cx;
        float dy = y - cy;
        float dz = z - cz;
        float dw = w - cw;
        float t = 0.34f - dx * dx - dy * dy - dz * dz - dw * dw;

        if (t > 0.0f)
        {
            t *= t;
            sum += t * t * mu_noise_v4_dot(g, mu_noise_v4(dx, dy, dz, dw));
        }
    }

    return 350.0f * sum;
}

MU_INLINE float mu_noise_simplex2_periodic(float x, float y, int period_x, int period_y, uint32_t seed)
{
    if (period_x <= 0 || period_y <= 0) return mu_noise_simplex2(x, y, seed);
    return mu_noise_simplex2_periodic_impl(x, y, period_x, period_y, seed);
}

MU_INLINE float mu_noise_simplex3_periodic(float x, float y, float z, int period_x, int period_y, int period_z, uint32_t seed)
{
    if (period_x <= 0 || period_y <= 0 || period_z <= 0)
        return mu_noise_simplex3(x, y, z, seed);
    return mu_noise_simplex3_periodic_impl(x, y, z, period_x, period_y, period_z, seed);
}

MU_INLINE float mu_noise_simplex4_periodic(float x, float y, float z, float w,
                                           int period_x, int period_y,
                                           int period_z, int period_w,
                                           uint32_t seed)
{
    if (period_x <= 0 || period_y <= 0 || period_z <= 0 || period_w <= 0)
        return mu_noise_simplex4(x, y, z, w, seed);
    return mu_noise_simplex4_periodic_impl(x, y, z, w, period_x, period_y,
                                           period_z, period_w, seed);
}

/* ------------------------------------------------------------------------- */
/* 4c) Derivative sampling                                                   */
/* ------------------------------------------------------------------------- */

/*
    Noise derivatives are useful for:
      - terrain normals and slope-aware texturing
      - erosion / thermal simulation
      - flow fields and heightfield lighting
      - feature detection (gradient magnitude)
      - analytic anti-aliasing of procedural materials

    Two strategies are provided:

    1) Analytic derivatives for the core value/perlin/simplex samplers.
       These are exact and cost roughly one extra small expression per
       interpolation corner.

    2) Finite-difference helpers for arbitrary callback noise. These work
       with any mu_noise_fn2 / mu_noise_fn3, including fractal compositions,
       but require 4 / 6 extra samples and an epsilon choice.

    The interpolation fade is quintic:

        f(t)  = 6t^5 - 15t^4 + 10t^3
        f'(t) = 30t^2 (t - 1)^2

    which is used by every analytic derivative below.
*/
MU_INLINE float mu_noise_fade_derivative5(float t)
{
    float d = t * (t - 1.0f);
    return 30.0f * d * d;
}

MU_INLINE mu_noise_sample2 mu_noise_value2_deriv(float x, float y, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float fx = mu_noise_fade_derivative5(tx);
    float fy = mu_noise_fade_derivative5(ty);

    float v00 = mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix0, iy0, seed));
    float v10 = mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix1, iy0, seed));
    float v01 = mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix0, iy1, seed));
    float v11 = mu_noise_hash_to_signed_float(mu_noise_hash_2i(ix1, iy1, seed));

    float a = mu_noise_lerp(v00, v10, ux);
    float b = mu_noise_lerp(v01, v11, ux);

    mu_noise_sample2 out;
    out.value = mu_noise_lerp(a, b, uy);
    out.gradient.x = ((v10 - v00) * (1.0f - uy) + (v11 - v01) * uy) * fx;
    out.gradient.y = (b - a) * fy;
    return out;
}

MU_INLINE mu_noise_sample3 mu_noise_value3_deriv(float x, float y, float z, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int iz0 = (int)floorf(z);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;
    int iz1 = iz0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float tz = z - (float)iz0;
    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float uz = mu_noise_smootherstep5(tz);
    float fx = mu_noise_fade_derivative5(tx);
    float fy = mu_noise_fade_derivative5(ty);
    float fz = mu_noise_fade_derivative5(tz);

    float c000 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix0, iy0, iz0, seed));
    float c100 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix1, iy0, iz0, seed));
    float c010 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix0, iy1, iz0, seed));
    float c110 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix1, iy1, iz0, seed));
    float c001 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix0, iy0, iz1, seed));
    float c101 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix1, iy0, iz1, seed));
    float c011 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix0, iy1, iz1, seed));
    float c111 = mu_noise_hash_to_signed_float(mu_noise_hash_3i(ix1, iy1, iz1, seed));

    float a00 = mu_noise_lerp(c000, c100, ux);
    float a10 = mu_noise_lerp(c010, c110, ux);
    float a01 = mu_noise_lerp(c001, c101, ux);
    float a11 = mu_noise_lerp(c011, c111, ux);
    float b0 = mu_noise_lerp(a00, a10, uy);
    float b1 = mu_noise_lerp(a01, a11, uy);

    float db0x = (1.0f - uy) * ((c100 - c000) * fx) + uy * ((c110 - c010) * fx);
    float db1x = (1.0f - uy) * ((c101 - c001) * fx) + uy * ((c111 - c011) * fx);

    mu_noise_sample3 out;
    out.value = mu_noise_lerp(b0, b1, uz);
    out.gradient.x = (1.0f - uz) * db0x + uz * db1x;
    out.gradient.y = (1.0f - uz) * ((a10 - a00) * fy) + uz * ((a11 - a01) * fy);
    out.gradient.z = (b1 - b0) * fz;
    return out;
}

MU_INLINE mu_noise_sample2 mu_noise_perlin2_deriv(float x, float y, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float fx = mu_noise_fade_derivative5(tx);
    float fy = mu_noise_fade_derivative5(ty);

    mu_noise_vec2 g00 = mu_noise_grad2_from_hash(mu_noise_hash_2i(ix0, iy0, seed));
    mu_noise_vec2 g10 = mu_noise_grad2_from_hash(mu_noise_hash_2i(ix1, iy0, seed));
    mu_noise_vec2 g01 = mu_noise_grad2_from_hash(mu_noise_hash_2i(ix0, iy1, seed));
    mu_noise_vec2 g11 = mu_noise_grad2_from_hash(mu_noise_hash_2i(ix1, iy1, seed));

    float n00 = mu_noise_v2_dot(g00, mu_noise_v2(tx,        ty));
    float n10 = mu_noise_v2_dot(g10, mu_noise_v2(tx - 1.0f, ty));
    float n01 = mu_noise_v2_dot(g01, mu_noise_v2(tx,        ty - 1.0f));
    float n11 = mu_noise_v2_dot(g11, mu_noise_v2(tx - 1.0f, ty - 1.0f));

    float a = mu_noise_lerp(n00, n10, ux);
    float b = mu_noise_lerp(n01, n11, ux);
    const float scale = 1.41421356f;

    mu_noise_sample2 out;
    out.value = scale * mu_noise_lerp(a, b, uy);
    out.gradient.x = scale * (
        (1.0f - uy) * ((1.0f - ux) * g00.x + ux * g10.x + fx * (n10 - n00)) +
        uy * ((1.0f - ux) * g01.x + ux * g11.x + fx * (n11 - n01)));
    out.gradient.y = scale * (
        (b - a) * fy +
        (1.0f - uy) * ((1.0f - ux) * g00.y + ux * g10.y) +
        uy * ((1.0f - ux) * g01.y + ux * g11.y));
    return out;
}

MU_INLINE mu_noise_sample3 mu_noise_perlin3_deriv(float x, float y, float z, uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int iz0 = (int)floorf(z);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;
    int iz1 = iz0 + 1;

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float tz = z - (float)iz0;
    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float uz = mu_noise_smootherstep5(tz);
    float fx = mu_noise_fade_derivative5(tx);
    float fy = mu_noise_fade_derivative5(ty);
    float fz = mu_noise_fade_derivative5(tz);

    mu_noise_vec3 g000 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix0, iy0, iz0, seed));
    mu_noise_vec3 g100 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix1, iy0, iz0, seed));
    mu_noise_vec3 g010 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix0, iy1, iz0, seed));
    mu_noise_vec3 g110 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix1, iy1, iz0, seed));
    mu_noise_vec3 g001 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix0, iy0, iz1, seed));
    mu_noise_vec3 g101 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix1, iy0, iz1, seed));
    mu_noise_vec3 g011 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix0, iy1, iz1, seed));
    mu_noise_vec3 g111 = mu_noise_grad3_from_hash(mu_noise_hash_3i(ix1, iy1, iz1, seed));

    float n000 = mu_noise_v3_dot(g000, mu_noise_v3(tx,        ty,        tz));
    float n100 = mu_noise_v3_dot(g100, mu_noise_v3(tx - 1.0f, ty,        tz));
    float n010 = mu_noise_v3_dot(g010, mu_noise_v3(tx,        ty - 1.0f, tz));
    float n110 = mu_noise_v3_dot(g110, mu_noise_v3(tx - 1.0f, ty - 1.0f, tz));
    float n001 = mu_noise_v3_dot(g001, mu_noise_v3(tx,        ty,        tz - 1.0f));
    float n101 = mu_noise_v3_dot(g101, mu_noise_v3(tx - 1.0f, ty,        tz - 1.0f));
    float n011 = mu_noise_v3_dot(g011, mu_noise_v3(tx,        ty - 1.0f, tz - 1.0f));
    float n111 = mu_noise_v3_dot(g111, mu_noise_v3(tx - 1.0f, ty - 1.0f, tz - 1.0f));

    float a00 = mu_noise_lerp(n000, n100, ux);
    float a10 = mu_noise_lerp(n010, n110, ux);
    float a01 = mu_noise_lerp(n001, n101, ux);
    float a11 = mu_noise_lerp(n011, n111, ux);
    float b0 = mu_noise_lerp(a00, a10, uy);
    float b1 = mu_noise_lerp(a01, a11, uy);

    float da00x = (1.0f - ux) * g000.x + ux * g100.x + fx * (n100 - n000);
    float da10x = (1.0f - ux) * g010.x + ux * g110.x + fx * (n110 - n010);
    float da01x = (1.0f - ux) * g001.x + ux * g101.x + fx * (n101 - n001);
    float da11x = (1.0f - ux) * g011.x + ux * g111.x + fx * (n111 - n011);

    float da00y = (1.0f - ux) * g000.y + ux * g100.y;
    float da10y = (1.0f - ux) * g010.y + ux * g110.y;
    float da01y = (1.0f - ux) * g001.y + ux * g101.y;
    float da11y = (1.0f - ux) * g011.y + ux * g111.y;

    float db0z = (1.0f - uy) * ((1.0f - ux) * g000.z + ux * g100.z) +
                 uy * ((1.0f - ux) * g010.z + ux * g110.z);
    float db1z = (1.0f - uy) * ((1.0f - ux) * g001.z + ux * g101.z) +
                 uy * ((1.0f - ux) * g011.z + ux * g111.z);

    const float scale = 1.15470054f;

    mu_noise_sample3 out;
    out.value = scale * mu_noise_lerp(b0, b1, uz);
    out.gradient.x = scale * (
        (1.0f - uz) * ((1.0f - uy) * da00x + uy * da10x) +
        uz * ((1.0f - uy) * da01x + uy * da11x));
    out.gradient.y = scale * (
        (1.0f - uz) * ((1.0f - uy) * da00y + uy * da10y + fy * (a10 - a00)) +
        uz * ((1.0f - uy) * da01y + uy * da11y + fy * (a11 - a01)));
    out.gradient.z = scale * ((b1 - b0) * fz + (1.0f - uz) * db0z + uz * db1z);
    return out;
}

MU_INLINE mu_noise_sample2 mu_noise_simplex2_deriv(float x, float y, uint32_t seed)
{
    const float F2 = 0.36602540378443864676f;
    const float G2 = 0.21132486540518711775f;

    float s = (x + y) * F2;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);

    float t = (float)(i + j) * G2;
    float x0 = x - ((float)i - t);
    float y0 = y - ((float)j - t);

    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; }
    else         { i1 = 0; j1 = 1; }

    float x1 = x0 - (float)i1 + G2;
    float y1 = y0 - (float)j1 + G2;
    float x2 = x0 - 1.0f + 2.0f * G2;
    float y2 = y0 - 1.0f + 2.0f * G2;

    float value = 0.0f;
    float gx = 0.0f;
    float gy = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0.0f)
    {
        mu_noise_vec2 g = mu_noise_grad2_from_hash(mu_noise_hash_2i(i, j, seed));
        float d = mu_noise_v2_dot(g, mu_noise_v2(x0, y0));
        float t2 = t0 * t0;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t0 * x0 * d;
        gy += t4 * g.y - 8.0f * t2 * t0 * y0 * d;
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0.0f)
    {
        mu_noise_vec2 g = mu_noise_grad2_from_hash(mu_noise_hash_2i(i + i1, j + j1, seed));
        float d = mu_noise_v2_dot(g, mu_noise_v2(x1, y1));
        float t2 = t1 * t1;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t1 * x1 * d;
        gy += t4 * g.y - 8.0f * t2 * t1 * y1 * d;
    }

    float t2v = 0.5f - x2 * x2 - y2 * y2;
    if (t2v > 0.0f)
    {
        mu_noise_vec2 g = mu_noise_grad2_from_hash(mu_noise_hash_2i(i + 1, j + 1, seed));
        float d = mu_noise_v2_dot(g, mu_noise_v2(x2, y2));
        float t2 = t2v * t2v;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t2v * x2 * d;
        gy += t4 * g.y - 8.0f * t2 * t2v * y2 * d;
    }

    mu_noise_sample2 out;
    out.value = 70.0f * value;
    out.gradient = mu_noise_v2(70.0f * gx, 70.0f * gy);
    return out;
}

MU_INLINE mu_noise_sample3 mu_noise_simplex3_deriv(float x, float y, float z, uint32_t seed)
{
    const float F3 = 1.0f / 3.0f;
    const float G3 = 1.0f / 6.0f;

    float s = (x + y + z) * F3;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);
    int k = (int)floorf(z + s);

    float t = (float)(i + j + k) * G3;
    float x0 = x - ((float)i - t);
    float y0 = y - ((float)j - t);
    float z0 = z - ((float)k - t);

    int i1, j1, k1;
    int i2, j2, k2;

    if (x0 >= y0)
    {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    }
    else
    {
        if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }

    float x1 = x0 - (float)i1 + G3;
    float y1 = y0 - (float)j1 + G3;
    float z1 = z0 - (float)k1 + G3;

    float x2 = x0 - (float)i2 + 2.0f * G3;
    float y2 = y0 - (float)j2 + 2.0f * G3;
    float z2 = z0 - (float)k2 + 2.0f * G3;

    float x3 = x0 - 1.0f + 3.0f * G3;
    float y3 = y0 - 1.0f + 3.0f * G3;
    float z3 = z0 - 1.0f + 3.0f * G3;

    float value = 0.0f;
    float gx = 0.0f;
    float gy = 0.0f;
    float gz = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0 - z0 * z0;
    if (t0 > 0.0f)
    {
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(i, j, k, seed));
        float d = mu_noise_v3_dot(g, mu_noise_v3(x0, y0, z0));
        float t2 = t0 * t0;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t0 * x0 * d;
        gy += t4 * g.y - 8.0f * t2 * t0 * y0 * d;
        gz += t4 * g.z - 8.0f * t2 * t0 * z0 * d;
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1 - z1 * z1;
    if (t1 > 0.0f)
    {
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + i1, j + j1, k + k1, seed));
        float d = mu_noise_v3_dot(g, mu_noise_v3(x1, y1, z1));
        float t2 = t1 * t1;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t1 * x1 * d;
        gy += t4 * g.y - 8.0f * t2 * t1 * y1 * d;
        gz += t4 * g.z - 8.0f * t2 * t1 * z1 * d;
    }

    float t2v = 0.5f - x2 * x2 - y2 * y2 - z2 * z2;
    if (t2v > 0.0f)
    {
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + i2, j + j2, k + k2, seed));
        float d = mu_noise_v3_dot(g, mu_noise_v3(x2, y2, z2));
        float t2 = t2v * t2v;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t2v * x2 * d;
        gy += t4 * g.y - 8.0f * t2 * t2v * y2 * d;
        gz += t4 * g.z - 8.0f * t2 * t2v * z2 * d;
    }

    float t3v = 0.5f - x3 * x3 - y3 * y3 - z3 * z3;
    if (t3v > 0.0f)
    {
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + 1, j + 1, k + 1, seed));
        float d = mu_noise_v3_dot(g, mu_noise_v3(x3, y3, z3));
        float t2 = t3v * t3v;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t3v * x3 * d;
        gy += t4 * g.y - 8.0f * t2 * t3v * y3 * d;
        gz += t4 * g.z - 8.0f * t2 * t3v * z3 * d;
    }

    mu_noise_sample3 out;
    out.value = 72.0f * value;
    out.gradient = mu_noise_v3(72.0f * gx, 72.0f * gy, 72.0f * gz);
    return out;
}

MU_INLINE mu_noise_sample4 mu_noise_value4_deriv(float x, float y, float z, float w,
                                                uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int iz0 = (int)floorf(z);
    int iw0 = (int)floorf(w);

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float tz = z - (float)iz0;
    float tw = w - (float)iw0;
    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float uz = mu_noise_smootherstep5(tz);
    float uw = mu_noise_smootherstep5(tw);
    float fx = mu_noise_fade_derivative5(tx);
    float fy = mu_noise_fade_derivative5(ty);
    float fz = mu_noise_fade_derivative5(tz);
    float fw = mu_noise_fade_derivative5(tw);
    float value = 0.0f;
    float gx = 0.0f;
    float gy = 0.0f;
    float gz = 0.0f;
    float gw = 0.0f;

    for (int ci = 0; ci < 16; ++ci)
    {
        int bx = ci & 1;
        int by = (ci >> 1) & 1;
        int bz = (ci >> 2) & 1;
        int bw = (ci >> 3) & 1;
        float wx = bx ? ux : (1.0f - ux);
        float wy = by ? uy : (1.0f - uy);
        float wz = bz ? uz : (1.0f - uz);
        float ww = bw ? uw : (1.0f - uw);
        float p = wx * wy * wz * ww;
        float v = mu_noise_hash_to_signed_float(
            mu_noise_hash_4i(ix0 + bx, iy0 + by, iz0 + bz, iw0 + bw, seed));

        value += v * p;
        gx += v * (bx ? fx : -fx) * wy * wz * ww;
        gy += v * wx * (by ? fy : -fy) * wz * ww;
        gz += v * wx * wy * (bz ? fz : -fz) * ww;
        gw += v * wx * wy * wz * (bw ? fw : -fw);
    }

    mu_noise_sample4 out;
    out.value = value;
    out.gradient = mu_noise_v4(gx, gy, gz, gw);
    return out;
}

MU_INLINE mu_noise_sample4 mu_noise_perlin4_deriv(float x, float y, float z, float w,
                                                 uint32_t seed)
{
    int ix0 = (int)floorf(x);
    int iy0 = (int)floorf(y);
    int iz0 = (int)floorf(z);
    int iw0 = (int)floorf(w);

    float tx = x - (float)ix0;
    float ty = y - (float)iy0;
    float tz = z - (float)iz0;
    float tw = w - (float)iw0;
    float ux = mu_noise_smootherstep5(tx);
    float uy = mu_noise_smootherstep5(ty);
    float uz = mu_noise_smootherstep5(tz);
    float uw = mu_noise_smootherstep5(tw);
    float fx = mu_noise_fade_derivative5(tx);
    float fy = mu_noise_fade_derivative5(ty);
    float fz = mu_noise_fade_derivative5(tz);
    float fw = mu_noise_fade_derivative5(tw);
    float value = 0.0f;
    float gx = 0.0f;
    float gy = 0.0f;
    float gz = 0.0f;
    float gw = 0.0f;

    for (int ci = 0; ci < 16; ++ci)
    {
        int bx = ci & 1;
        int by = (ci >> 1) & 1;
        int bz = (ci >> 2) & 1;
        int bw = (ci >> 3) & 1;
        float wx = bx ? ux : (1.0f - ux);
        float wy = by ? uy : (1.0f - uy);
        float wz = bz ? uz : (1.0f - uz);
        float ww = bw ? uw : (1.0f - uw);
        float p = wx * wy * wz * ww;
        mu_noise_vec4 g = mu_noise_grad4_from_hash(
            mu_noise_hash_4i(ix0 + bx, iy0 + by, iz0 + bz, iw0 + bw, seed));
        float n = mu_noise_v4_dot(g, mu_noise_v4(tx - (float)bx, ty - (float)by,
                                                 tz - (float)bz, tw - (float)bw));

        value += n * p;
        gx += n * (bx ? fx : -fx) * wy * wz * ww + p * g.x;
        gy += n * wx * (by ? fy : -fy) * wz * ww + p * g.y;
        gz += n * wx * wy * (bz ? fz : -fz) * ww + p * g.z;
        gw += n * wx * wy * wz * (bw ? fw : -fw) + p * g.w;
    }

    mu_noise_sample4 out;
    out.value = value;
    out.gradient = mu_noise_v4(gx, gy, gz, gw);
    return out;
}

MU_INLINE mu_noise_sample4 mu_noise_simplex4_deriv(float x, float y, float z, float w,
                                                  uint32_t seed)
{
    const float F4 = 0.30901699437494742410f;
    const float G4 = 0.13819660112501051518f;

    float s = (x + y + z + w) * F4;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);
    int k = (int)floorf(z + s);
    int l = (int)floorf(w + s);

    float t = (float)(i + j + k + l) * G4;
    float x0 = x - ((float)i - t);
    float y0 = y - ((float)j - t);
    float z0 = z - ((float)k - t);
    float w0 = w - ((float)l - t);

    int rx = (x0 > y0) + (x0 > z0) + (x0 > w0);
    int ry = (y0 > x0) + (y0 > z0) + (y0 > w0);
    int rz = (z0 > x0) + (z0 > y0) + (z0 > w0);
    int rw = (w0 > x0) + (w0 > y0) + (w0 > z0);

    int i1 = (rx >= 3);
    int j1 = (ry >= 3);
    int k1 = (rz >= 3);
    int l1 = (rw >= 3);
    int i2 = (rx >= 2);
    int j2 = (ry >= 2);
    int k2 = (rz >= 2);
    int l2 = (rw >= 2);
    int i3 = (rx >= 1);
    int j3 = (ry >= 1);
    int k3 = (rz >= 1);
    int l3 = (rw >= 1);

    float x1 = x0 - (float)i1 + G4;
    float y1 = y0 - (float)j1 + G4;
    float z1 = z0 - (float)k1 + G4;
    float w1 = w0 - (float)l1 + G4;
    float x2 = x0 - (float)i2 + 2.0f * G4;
    float y2 = y0 - (float)j2 + 2.0f * G4;
    float z2 = z0 - (float)k2 + 2.0f * G4;
    float w2 = w0 - (float)l2 + 2.0f * G4;
    float x3 = x0 - (float)i3 + 3.0f * G4;
    float y3 = y0 - (float)j3 + 3.0f * G4;
    float z3 = z0 - (float)k3 + 3.0f * G4;
    float w3 = w0 - (float)l3 + 3.0f * G4;
    float x4 = x0 - 1.0f + 4.0f * G4;
    float y4 = y0 - 1.0f + 4.0f * G4;
    float z4 = z0 - 1.0f + 4.0f * G4;
    float w4 = w0 - 1.0f + 4.0f * G4;

    float value = 0.0f;
    float gx = 0.0f;
    float gy = 0.0f;
    float gz = 0.0f;
    float gw = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0 - z0 * z0 - w0 * w0;
    if (t0 > 0.0f)
    {
        mu_noise_vec4 g = mu_noise_grad4_from_hash(mu_noise_hash_4i(i, j, k, l, seed));
        float d = mu_noise_v4_dot(g, mu_noise_v4(x0, y0, z0, w0));
        float t2 = t0 * t0;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t0 * x0 * d;
        gy += t4 * g.y - 8.0f * t2 * t0 * y0 * d;
        gz += t4 * g.z - 8.0f * t2 * t0 * z0 * d;
        gw += t4 * g.w - 8.0f * t2 * t0 * w0 * d;
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1 - z1 * z1 - w1 * w1;
    if (t1 > 0.0f)
    {
        mu_noise_vec4 g = mu_noise_grad4_from_hash(mu_noise_hash_4i(i + i1, j + j1, k + k1, l + l1, seed));
        float d = mu_noise_v4_dot(g, mu_noise_v4(x1, y1, z1, w1));
        float t2 = t1 * t1;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t1 * x1 * d;
        gy += t4 * g.y - 8.0f * t2 * t1 * y1 * d;
        gz += t4 * g.z - 8.0f * t2 * t1 * z1 * d;
        gw += t4 * g.w - 8.0f * t2 * t1 * w1 * d;
    }

    float t2v = 0.5f - x2 * x2 - y2 * y2 - z2 * z2 - w2 * w2;
    if (t2v > 0.0f)
    {
        mu_noise_vec4 g = mu_noise_grad4_from_hash(mu_noise_hash_4i(i + i2, j + j2, k + k2, l + l2, seed));
        float d = mu_noise_v4_dot(g, mu_noise_v4(x2, y2, z2, w2));
        float t2 = t2v * t2v;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t2v * x2 * d;
        gy += t4 * g.y - 8.0f * t2 * t2v * y2 * d;
        gz += t4 * g.z - 8.0f * t2 * t2v * z2 * d;
        gw += t4 * g.w - 8.0f * t2 * t2v * w2 * d;
    }

    float t3v = 0.5f - x3 * x3 - y3 * y3 - z3 * z3 - w3 * w3;
    if (t3v > 0.0f)
    {
        mu_noise_vec4 g = mu_noise_grad4_from_hash(mu_noise_hash_4i(i + i3, j + j3, k + k3, l + l3, seed));
        float d = mu_noise_v4_dot(g, mu_noise_v4(x3, y3, z3, w3));
        float t2 = t3v * t3v;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t3v * x3 * d;
        gy += t4 * g.y - 8.0f * t2 * t3v * y3 * d;
        gz += t4 * g.z - 8.0f * t2 * t3v * z3 * d;
        gw += t4 * g.w - 8.0f * t2 * t3v * w3 * d;
    }

    float t4v = 0.5f - x4 * x4 - y4 * y4 - z4 * z4 - w4 * w4;
    if (t4v > 0.0f)
    {
        mu_noise_vec4 g = mu_noise_grad4_from_hash(mu_noise_hash_4i(i + 1, j + 1, k + 1, l + 1, seed));
        float d = mu_noise_v4_dot(g, mu_noise_v4(x4, y4, z4, w4));
        float t2 = t4v * t4v;
        float t4 = t2 * t2;
        value += t4 * d;
        gx += t4 * g.x - 8.0f * t2 * t4v * x4 * d;
        gy += t4 * g.y - 8.0f * t2 * t4v * y4 * d;
        gz += t4 * g.z - 8.0f * t2 * t4v * z4 * d;
        gw += t4 * g.w - 8.0f * t2 * t4v * w4 * d;
    }

    mu_noise_sample4 out;
    out.value = 62.0f * value;
    out.gradient = mu_noise_v4(62.0f * gx, 62.0f * gy, 62.0f * gz, 62.0f * gw);
    return out;
}

/*
    Finite-difference sampling for arbitrary callback noise.

    Central differences are second-order accurate. `eps` is in input units;
    values <= 0 select a sensible default. Pick eps relative to the smallest
    feature you care about: too large smooths details, too small loses
    precision. For lattice noise, 1e-3 is usually safe.
*/
MU_INLINE mu_noise_sample2 mu_noise_sample2_fd(mu_noise_fn2 base, float x, float y,
                                               uint32_t seed, float eps)
{
    float e = (eps > 0.0f) ? eps : 1.0e-3f;
    mu_noise_sample2 out;

    out.value = base(x, y, seed);
    out.gradient.x = (base(x + e, y, seed) - base(x - e, y, seed)) / (2.0f * e);
    out.gradient.y = (base(x, y + e, seed) - base(x, y - e, seed)) / (2.0f * e);
    return out;
}

MU_INLINE mu_noise_sample3 mu_noise_sample3_fd(mu_noise_fn3 base, float x, float y,
                                               float z, uint32_t seed, float eps)
{
    float e = (eps > 0.0f) ? eps : 1.0e-3f;
    mu_noise_sample3 out;

    out.value = base(x, y, z, seed);
    out.gradient.x = (base(x + e, y, z, seed) - base(x - e, y, z, seed)) / (2.0f * e);
    out.gradient.y = (base(x, y + e, z, seed) - base(x, y - e, z, seed)) / (2.0f * e);
    out.gradient.z = (base(x, y, z + e, seed) - base(x, y, z - e, seed)) / (2.0f * e);
    return out;
}

/* Discrete Laplacian: useful for curvature, erosion, and heat diffusion. */
MU_INLINE float mu_noise_laplacian2_fd(mu_noise_fn2 base, float x, float y,
                                       uint32_t seed, float eps)
{
    float e = (eps > 0.0f) ? eps : 1.0e-3f;
    float c = base(x, y, seed);
    return (base(x + e, y, seed) + base(x - e, y, seed) +
            base(x, y + e, seed) + base(x, y - e, seed) - 4.0f * c) / (e * e);
}

MU_INLINE float mu_noise_laplacian3_fd(mu_noise_fn3 base, float x, float y, float z,
                                       uint32_t seed, float eps)
{
    float e = (eps > 0.0f) ? eps : 1.0e-3f;
    float c = base(x, y, z, seed);
    return (base(x + e, y, z, seed) + base(x - e, y, z, seed) +
            base(x, y + e, z, seed) + base(x, y - e, z, seed) +
            base(x, y, z + e, seed) + base(x, y, z - e, seed) - 6.0f * c) / (e * e);
}

MU_INLINE mu_noise_sample4 mu_noise_sample4_fd(mu_noise_fn4 base, float x, float y,
                                               float z, float w, uint32_t seed, float eps)
{
    float e = (eps > 0.0f) ? eps : 1.0e-3f;
    mu_noise_sample4 out;

    out.value = base(x, y, z, w, seed);
    out.gradient.x = (base(x + e, y, z, w, seed) - base(x - e, y, z, w, seed)) / (2.0f * e);
    out.gradient.y = (base(x, y + e, z, w, seed) - base(x, y - e, z, w, seed)) / (2.0f * e);
    out.gradient.z = (base(x, y, z + e, w, seed) - base(x, y, z - e, w, seed)) / (2.0f * e);
    out.gradient.w = (base(x, y, z, w + e, seed) - base(x, y, z, w - e, seed)) / (2.0f * e);
    return out;
}

MU_INLINE float mu_noise_laplacian4_fd(mu_noise_fn4 base, float x, float y, float z, float w,
                                       uint32_t seed, float eps)
{
    float e = (eps > 0.0f) ? eps : 1.0e-3f;
    float c = base(x, y, z, w, seed);
    return (base(x + e, y, z, w, seed) + base(x - e, y, z, w, seed) +
            base(x, y + e, z, w, seed) + base(x, y - e, z, w, seed) +
            base(x, y, z + e, w, seed) + base(x, y, z - e, w, seed) +
            base(x, y, z, w + e, seed) + base(x, y, z, w - e, seed) - 8.0f * c) / (e * e);
}

/* ------------------------------------------------------------------------- */
/* 5) Worley / cellular noise                                                */
/* ------------------------------------------------------------------------- */

/*
    Worley noise stores random feature points inside each integer cell.

    For sample p, search neighboring cells and compute distances to features.

      F1 = nearest distance
      F2 = second nearest distance

    Typical textures:
      - veins/cracks: F2 - F1
      - cells: F1
      - borders: step(F2 - F1)
*/

MU_INLINE float mu_noise_distance2(mu_noise_vec2 d, mu_noise_distance_metric metric)
{
    if (metric == MU_NOISE_DIST_MANHATTAN)
    {
        return mu_noise_abs(d.x) + mu_noise_abs(d.y);
    }
    if (metric == MU_NOISE_DIST_CHEBYSHEV)
    {
        float ax = mu_noise_abs(d.x);
        float ay = mu_noise_abs(d.y);
        return (ax > ay) ? ax : ay;
    }
    return mu_noise_v2_len(d);
}

MU_INLINE float mu_noise_distance3(mu_noise_vec3 d, mu_noise_distance_metric metric)
{
    if (metric == MU_NOISE_DIST_MANHATTAN)
    {
        return mu_noise_abs(d.x) + mu_noise_abs(d.y) + mu_noise_abs(d.z);
    }
    if (metric == MU_NOISE_DIST_CHEBYSHEV)
    {
        float ax = mu_noise_abs(d.x);
        float ay = mu_noise_abs(d.y);
        float az = mu_noise_abs(d.z);
        float m = (ax > ay) ? ax : ay;
        return (m > az) ? m : az;
    }
    return mu_noise_v3_len(d);
}

MU_INLINE float mu_noise_distance4(mu_noise_vec4 d, mu_noise_distance_metric metric)
{
    if (metric == MU_NOISE_DIST_MANHATTAN)
    {
        return mu_noise_abs(d.x) + mu_noise_abs(d.y) + mu_noise_abs(d.z) + mu_noise_abs(d.w);
    }
    if (metric == MU_NOISE_DIST_CHEBYSHEV)
    {
        float ax = mu_noise_abs(d.x);
        float ay = mu_noise_abs(d.y);
        float az = mu_noise_abs(d.z);
        float aw = mu_noise_abs(d.w);
        float m = (ax > ay) ? ax : ay;
        if (az > m) m = az;
        return (aw > m) ? aw : m;
    }
    return mu_noise_v4_len(d);
}

/*
    Internal 2D cellular scan. `periodic` is true when both periods are > 0.
    The cell coordinates stay unwrapped so feature points keep their absolute
    positions; only the hash lookup wraps. This makes points across the tile
    seam line up exactly.
*/
MU_INLINE mu_noise_worley2_result mu_noise_worley2_impl(float x, float y,
                                                        uint32_t seed,
                                                        mu_noise_distance_metric metric,
                                                        int period_x, int period_y)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    int periodic = (period_x > 0 && period_y > 0);

    mu_noise_worley2_result out;
    out.f1 = FLT_MAX;
    out.f2 = FLT_MAX;
    out.cell_id1 = 0;
    out.cell_id2 = 0;
    out.center1 = mu_noise_v2(0.0f, 0.0f);
    out.center2 = mu_noise_v2(0.0f, 0.0f);

    for (int oy = -1; oy <= 1; ++oy)
    {
        for (int ox = -1; ox <= 1; ++ox)
        {
            int cx = ix + ox;
            int cy = iy + oy;
            uint32_t h = periodic
                ? mu_noise_hash_2i_periodic(cx, cy, period_x, period_y, seed)
                : mu_noise_hash_2i(cx, cy, seed);
            float fx = mu_noise_hash_to_unit_float(h);
            float fy = mu_noise_hash_to_unit_float(mu_noise_hash_u32(h ^ 0x68bc21ebu));
            mu_noise_vec2 center = mu_noise_v2((float)cx + fx, (float)cy + fy);
            mu_noise_vec2 d = mu_noise_v2_sub(center, mu_noise_v2(x, y));
            float dist = mu_noise_distance2(d, metric);
            int cell_id = (int)mu_noise_hash_u32(h ^ 0x9e3779b9u);

            if (dist < out.f1)
            {
                out.f2 = out.f1;
                out.cell_id2 = out.cell_id1;
                out.center2 = out.center1;
                out.f1 = dist;
                out.cell_id1 = cell_id;
                out.center1 = center;
            }
            else if (dist < out.f2)
            {
                out.f2 = dist;
                out.cell_id2 = cell_id;
                out.center2 = center;
            }
        }
    }

    return out;
}

MU_INLINE mu_noise_worley3_result mu_noise_worley3_impl(float x, float y, float z,
                                                        uint32_t seed,
                                                        mu_noise_distance_metric metric,
                                                        int period_x, int period_y,
                                                        int period_z)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    int iz = (int)floorf(z);
    int periodic = (period_x > 0 && period_y > 0 && period_z > 0);

    mu_noise_worley3_result out;
    out.f1 = FLT_MAX;
    out.f2 = FLT_MAX;
    out.cell_id1 = 0;
    out.cell_id2 = 0;
    out.center1 = mu_noise_v3(0.0f, 0.0f, 0.0f);
    out.center2 = mu_noise_v3(0.0f, 0.0f, 0.0f);

    for (int oz = -1; oz <= 1; ++oz)
    {
        for (int oy = -1; oy <= 1; ++oy)
        {
            for (int ox = -1; ox <= 1; ++ox)
            {
                int cx = ix + ox;
                int cy = iy + oy;
                int cz = iz + oz;
                uint32_t h = periodic
                    ? mu_noise_hash_3i_periodic(cx, cy, cz, period_x, period_y, period_z, seed)
                    : mu_noise_hash_3i(cx, cy, cz, seed);
                float fx = mu_noise_hash_to_unit_float(h);
                float fy = mu_noise_hash_to_unit_float(mu_noise_hash_u32(h ^ 0x68bc21ebu));
                float fz = mu_noise_hash_to_unit_float(mu_noise_hash_u32(h ^ 0x02e5be93u));
                mu_noise_vec3 center = mu_noise_v3((float)cx + fx, (float)cy + fy, (float)cz + fz);
                mu_noise_vec3 d = mu_noise_v3_sub(center, mu_noise_v3(x, y, z));
                float dist = mu_noise_distance3(d, metric);
                int cell_id = (int)mu_noise_hash_u32(h ^ 0x85ebca6bu);

                if (dist < out.f1)
                {
                    out.f2 = out.f1;
                    out.cell_id2 = out.cell_id1;
                    out.center2 = out.center1;
                    out.f1 = dist;
                    out.cell_id1 = cell_id;
                    out.center1 = center;
                }
                else if (dist < out.f2)
                {
                    out.f2 = dist;
                    out.cell_id2 = cell_id;
                    out.center2 = center;
                }
            }
        }
    }

    return out;
}

MU_INLINE mu_noise_worley4_result mu_noise_worley4_impl(float x, float y, float z, float w,
                                                        uint32_t seed,
                                                        mu_noise_distance_metric metric,
                                                        int period_x, int period_y,
                                                        int period_z, int period_w)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    int iz = (int)floorf(z);
    int iw = (int)floorf(w);
    int periodic = (period_x > 0 && period_y > 0 && period_z > 0 && period_w > 0);

    mu_noise_worley4_result out;
    out.f1 = FLT_MAX;
    out.f2 = FLT_MAX;
    out.cell_id1 = 0;
    out.cell_id2 = 0;
    out.center1 = mu_noise_v4(0.0f, 0.0f, 0.0f, 0.0f);
    out.center2 = mu_noise_v4(0.0f, 0.0f, 0.0f, 0.0f);

    for (int ow = -1; ow <= 1; ++ow)
    {
        for (int oz = -1; oz <= 1; ++oz)
        {
            for (int oy = -1; oy <= 1; ++oy)
            {
                for (int ox = -1; ox <= 1; ++ox)
                {
                    int cx = ix + ox;
                    int cy = iy + oy;
                    int cz = iz + oz;
                    int cw = iw + ow;
                    uint32_t h = periodic
                        ? mu_noise_hash_4i_periodic(cx, cy, cz, cw,
                                                    period_x, period_y, period_z, period_w,
                                                    seed)
                        : mu_noise_hash_4i(cx, cy, cz, cw, seed);
                    float fx = mu_noise_hash_to_unit_float(h);
                    float fy = mu_noise_hash_to_unit_float(mu_noise_hash_u32(h ^ 0x68bc21ebu));
                    float fz = mu_noise_hash_to_unit_float(mu_noise_hash_u32(h ^ 0x02e5be93u));
                    float fw = mu_noise_hash_to_unit_float(mu_noise_hash_u32(h ^ 0x7f4a7c15u));
                    mu_noise_vec4 center = mu_noise_v4((float)cx + fx, (float)cy + fy,
                                                       (float)cz + fz, (float)cw + fw);
                    mu_noise_vec4 d = mu_noise_v4_sub(center, mu_noise_v4(x, y, z, w));
                    float dist = mu_noise_distance4(d, metric);
                    int cell_id = (int)mu_noise_hash_u32(h ^ 0x85ebca6bu);

                    if (dist < out.f1)
                    {
                        out.f2 = out.f1;
                        out.cell_id2 = out.cell_id1;
                        out.center2 = out.center1;
                        out.f1 = dist;
                        out.cell_id1 = cell_id;
                        out.center1 = center;
                    }
                    else if (dist < out.f2)
                    {
                        out.f2 = dist;
                        out.cell_id2 = cell_id;
                        out.center2 = center;
                    }
                }
            }
        }
    }

    return out;
}

MU_INLINE mu_noise_worley2_result mu_noise_worley2(float x, float y, uint32_t seed, mu_noise_distance_metric metric)
{
    return mu_noise_worley2_impl(x, y, seed, metric, 0, 0);
}

MU_INLINE mu_noise_worley3_result mu_noise_worley3(float x, float y, float z, uint32_t seed, mu_noise_distance_metric metric)
{
    return mu_noise_worley3_impl(x, y, z, seed, metric, 0, 0, 0);
}

MU_INLINE mu_noise_worley2_result mu_noise_worley2_periodic(float x, float y,
                                                            uint32_t seed,
                                                            mu_noise_distance_metric metric,
                                                            int period_x, int period_y)
{
    if (period_x <= 0 || period_y <= 0)
        return mu_noise_worley2(x, y, seed, metric);
    return mu_noise_worley2_impl(x, y, seed, metric, period_x, period_y);
}

MU_INLINE mu_noise_worley3_result mu_noise_worley3_periodic(float x, float y, float z,
                                                            uint32_t seed,
                                                            mu_noise_distance_metric metric,
                                                            int period_x, int period_y,
                                                            int period_z)
{
    if (period_x <= 0 || period_y <= 0 || period_z <= 0)
        return mu_noise_worley3(x, y, z, seed, metric);
    return mu_noise_worley3_impl(x, y, z, seed, metric, period_x, period_y, period_z);
}

MU_INLINE mu_noise_worley4_result mu_noise_worley4(float x, float y, float z, float w,
                                                    uint32_t seed,
                                                    mu_noise_distance_metric metric)
{
    return mu_noise_worley4_impl(x, y, z, w, seed, metric, 0, 0, 0, 0);
}

MU_INLINE mu_noise_worley4_result mu_noise_worley4_periodic(float x, float y, float z, float w,
                                                            uint32_t seed,
                                                            mu_noise_distance_metric metric,
                                                            int period_x, int period_y,
                                                            int period_z, int period_w)
{
    if (period_x <= 0 || period_y <= 0 || period_z <= 0 || period_w <= 0)
        return mu_noise_worley4(x, y, z, w, seed, metric);
    return mu_noise_worley4_impl(x, y, z, w, seed, metric,
                                 period_x, period_y, period_z, period_w);
}

/* Small focused helpers; they reuse the full result above. */
MU_INLINE float mu_noise_worley2_f1(float x, float y, uint32_t seed,
                                    mu_noise_distance_metric metric)
{
    return mu_noise_worley2(x, y, seed, metric).f1;
}

MU_INLINE float mu_noise_worley2_f2(float x, float y, uint32_t seed,
                                    mu_noise_distance_metric metric)
{
    return mu_noise_worley2(x, y, seed, metric).f2;
}

MU_INLINE float mu_noise_worley2_edge(float x, float y, uint32_t seed,
                                      mu_noise_distance_metric metric)
{
    mu_noise_worley2_result r = mu_noise_worley2(x, y, seed, metric);
    return r.f2 - r.f1;
}

MU_INLINE int mu_noise_worley2_cell_id(float x, float y, uint32_t seed)
{
    return mu_noise_worley2(x, y, seed, MU_NOISE_DIST_EUCLIDEAN).cell_id1;
}

MU_INLINE mu_noise_vec2 mu_noise_worley2_cell_center(float x, float y, uint32_t seed)
{
    return mu_noise_worley2(x, y, seed, MU_NOISE_DIST_EUCLIDEAN).center1;
}

MU_INLINE float mu_noise_worley3_f1(float x, float y, float z, uint32_t seed,
                                    mu_noise_distance_metric metric)
{
    return mu_noise_worley3(x, y, z, seed, metric).f1;
}

MU_INLINE float mu_noise_worley3_f2(float x, float y, float z, uint32_t seed,
                                    mu_noise_distance_metric metric)
{
    return mu_noise_worley3(x, y, z, seed, metric).f2;
}

MU_INLINE float mu_noise_worley3_edge(float x, float y, float z, uint32_t seed,
                                      mu_noise_distance_metric metric)
{
    mu_noise_worley3_result r = mu_noise_worley3(x, y, z, seed, metric);
    return r.f2 - r.f1;
}

MU_INLINE int mu_noise_worley3_cell_id(float x, float y, float z, uint32_t seed)
{
    return mu_noise_worley3(x, y, z, seed, MU_NOISE_DIST_EUCLIDEAN).cell_id1;
}

MU_INLINE mu_noise_vec3 mu_noise_worley3_cell_center(float x, float y, float z,
                                                     uint32_t seed)
{
    return mu_noise_worley3(x, y, z, seed, MU_NOISE_DIST_EUCLIDEAN).center1;
}

MU_INLINE float mu_noise_worley4_f1(float x, float y, float z, float w,
                                    uint32_t seed, mu_noise_distance_metric metric)
{
    return mu_noise_worley4(x, y, z, w, seed, metric).f1;
}

MU_INLINE float mu_noise_worley4_f2(float x, float y, float z, float w,
                                    uint32_t seed, mu_noise_distance_metric metric)
{
    return mu_noise_worley4(x, y, z, w, seed, metric).f2;
}

MU_INLINE float mu_noise_worley4_edge(float x, float y, float z, float w,
                                      uint32_t seed, mu_noise_distance_metric metric)
{
    mu_noise_worley4_result r = mu_noise_worley4(x, y, z, w, seed, metric);
    return r.f2 - r.f1;
}

MU_INLINE int mu_noise_worley4_cell_id(float x, float y, float z, float w, uint32_t seed)
{
    return mu_noise_worley4(x, y, z, w, seed, MU_NOISE_DIST_EUCLIDEAN).cell_id1;
}

MU_INLINE mu_noise_vec4 mu_noise_worley4_cell_center(float x, float y, float z, float w,
                                                     uint32_t seed)
{
    return mu_noise_worley4(x, y, z, w, seed, MU_NOISE_DIST_EUCLIDEAN).center1;
}

/* ------------------------------------------------------------------------- */
/* 6) Fractal combinations                                                   */
/* ------------------------------------------------------------------------- */

/*
    fBm (fractal Brownian motion):

      result = Σ_{i=0..oct-1} amp_i * base(freq_i * p)
      freq_{i+1} = freq_i * lacunarity
      amp_{i+1}  = amp_i  * gain

    Typical defaults:
      lacunarity = 2.0
      gain       = 0.5
      octaves    = 4..8
*/

MU_INLINE float mu_noise_fbm1(mu_noise_fn1 base, float x, uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        sum += amp * base(x * freq, seed + (uint32_t)i * 1013u);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_turbulence1(mu_noise_fn1 base, float x, uint32_t seed,
                                     int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, seed + (uint32_t)i * 1013u);
        sum += amp * mu_noise_abs(n);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_billow1(mu_noise_fn1 base, float x, uint32_t seed,
                                 int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, seed + (uint32_t)i * 1013u);
        n = 2.0f * mu_noise_abs(n) - 1.0f;
        sum += amp * n;
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_ridged1(mu_noise_fn1 base, float x, uint32_t seed,
                                 int octaves, float lacunarity, float gain,
                                 float offset)
{
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    float weight = 1.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, seed + (uint32_t)i * 1013u);
        float signal = offset - mu_noise_abs(n);
        signal *= signal;
        signal *= weight;
        weight = mu_noise_clamp(signal * gain, 0.0f, 1.0f);
        sum += signal * amp;
        freq *= lacunarity;
        amp *= 0.5f;
    }

    return mu_noise_clamp(sum * 2.0f - 1.0f, -1.0f, 1.0f);
}

MU_INLINE float mu_noise_fbm2(mu_noise_fn2 base, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        sum += amp * base(x * freq, y * freq, seed + (uint32_t)i * 1013u);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_fbm3(mu_noise_fn3 base, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        sum += amp * base(x * freq, y * freq, z * freq, seed + (uint32_t)i * 1013u);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

/*
    Turbulence:
      like fBm, but uses absolute value of base signal.

      turbulence = Σ amp_i * |noise(freq_i * p)|
*/
MU_INLINE float mu_noise_turbulence2(mu_noise_fn2 base, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, seed + (uint32_t)i * 1013u);
        sum += amp * mu_noise_abs(n);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_turbulence3(mu_noise_fn3 base, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, z * freq, seed + (uint32_t)i * 1013u);
        sum += amp * mu_noise_abs(n);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

/*
    Billow noise:
      billow = 2*|noise| - 1

    It makes rounded "cloud/pillow" structures.
*/
MU_INLINE float mu_noise_billow2(mu_noise_fn2 base, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, seed + (uint32_t)i * 1013u);
        n = 2.0f * mu_noise_abs(n) - 1.0f;
        sum += amp * n;
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_billow3(mu_noise_fn3 base, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, z * freq, seed + (uint32_t)i * 1013u);
        n = 2.0f * mu_noise_abs(n) - 1.0f;
        sum += amp * n;
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

/*
    Ridged multifractal (Musgrave style flavor):

      signal = offset - |noise|
      signal = signal^2
      signal *= weight
      weight = clamp(signal * gain, 0, 1)
      sum += signal * amp

    Good for sharp mountain ridges.
*/
MU_INLINE float mu_noise_ridged2(mu_noise_fn2 base, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain, float offset)
{
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    float weight = 1.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, seed + (uint32_t)i * 1013u);
        float signal = offset - mu_noise_abs(n);
        signal *= signal;
        signal *= weight;

        weight = mu_noise_clamp(signal * gain, 0.0f, 1.0f);
        sum += signal * amp;

        freq *= lacunarity;
        amp *= 0.5f;
    }

    return mu_noise_clamp(sum * 2.0f - 1.0f, -1.0f, 1.0f);
}

MU_INLINE float mu_noise_ridged3(mu_noise_fn3 base, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain, float offset)
{
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    float weight = 1.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, z * freq, seed + (uint32_t)i * 1013u);
        float signal = offset - mu_noise_abs(n);
        signal *= signal;
        signal *= weight;

        weight = mu_noise_clamp(signal * gain, 0.0f, 1.0f);
        sum += signal * amp;

        freq *= lacunarity;
        amp *= 0.5f;
    }

    return mu_noise_clamp(sum * 2.0f - 1.0f, -1.0f, 1.0f);
}

/*
    Multifractal (multiplicative) composition.

    Unlike fBm, where every octave has independent amplitude, here the
    previous octave modulates how strongly the next one contributes:

        signal_i = clamp(gain * |noise_i| + offset, 0, 1)
        sum_i   += amp_i * signal_i
        amp_{i+1} = amp_i * signal_i

    High-activity regions stay detailed; quiet regions stay smooth. The result
    is remapped to roughly [-1, 1].
*/
MU_INLINE float mu_noise_multifractal2(mu_noise_fn2 base, float x, float y,
                                       uint32_t seed, int octaves,
                                       float lacunarity, float gain, float offset)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, seed + (uint32_t)i * 1013u);
        float signal = mu_noise_clamp(gain * mu_noise_abs(n) + offset, 0.0f, 1.0f);
        sum += amp * signal;
        norm += amp;
        amp *= signal;
        freq *= lacunarity;
    }

    return (norm > 0.0f) ? (2.0f * sum / norm - 1.0f) : 0.0f;
}

MU_INLINE float mu_noise_multifractal3(mu_noise_fn3 base, float x, float y, float z,
                                       uint32_t seed, int octaves,
                                       float lacunarity, float gain, float offset)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, z * freq, seed + (uint32_t)i * 1013u);
        float signal = mu_noise_clamp(gain * mu_noise_abs(n) + offset, 0.0f, 1.0f);
        sum += amp * signal;
        norm += amp;
        amp *= signal;
        freq *= lacunarity;
    }

    return (norm > 0.0f) ? (2.0f * sum / norm - 1.0f) : 0.0f;
}

/*
    Heterogeneous terrain: the previous octave controls the amplitude of the
    next one, producing valleys with fine detail and smooth mountain flanks.
*/
MU_INLINE float mu_noise_hetero_terrain2(mu_noise_fn2 base, float x, float y,
                                         uint32_t seed, int octaves,
                                         float lacunarity, float gain, float offset)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;
    float weight = 1.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, seed + (uint32_t)i * 1013u);
        float signal = n * weight;
        sum += amp * signal;
        norm += amp;
        weight = mu_noise_clamp(gain * signal + offset, 0.0f, 1.0f);
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_hetero_terrain3(mu_noise_fn3 base, float x, float y, float z,
                                         uint32_t seed, int octaves,
                                         float lacunarity, float gain, float offset)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;
    float weight = 1.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, z * freq, seed + (uint32_t)i * 1013u);
        float signal = n * weight;
        sum += amp * signal;
        norm += amp;
        weight = mu_noise_clamp(gain * signal + offset, 0.0f, 1.0f);
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

/* ------------------------------------------------------------------------- */
/* 7) Domain warp                                                            */
/* ------------------------------------------------------------------------- */

/*
    Domain warping injects complexity by perturbing coordinates first.

      p' = p + A * W(p * f)
      out = N(p')

    where W is often simplex/perlin and N can be any base noise.
*/
MU_INLINE mu_noise_vec2 mu_noise_domain_warp2(mu_noise_fn2 warp, float x, float y, uint32_t seed, float warp_amp, float warp_freq)
{
    float qx = warp(x * warp_freq, y * warp_freq, seed ^ 0xA511E9B3u);
    float qy = warp((x + 19.19f) * warp_freq, (y - 33.47f) * warp_freq, seed ^ 0x63D83595u);
    return mu_noise_v2(x + warp_amp * qx, y + warp_amp * qy);
}

MU_INLINE mu_noise_vec3 mu_noise_domain_warp3(mu_noise_fn3 warp, float x, float y, float z, uint32_t seed, float warp_amp, float warp_freq)
{
    float qx = warp(x * warp_freq, y * warp_freq, z * warp_freq, seed ^ 0xA511E9B3u);
    float qy = warp((x + 19.19f) * warp_freq, (y - 33.47f) * warp_freq, (z + 11.73f) * warp_freq, seed ^ 0x63D83595u);
    float qz = warp((x - 47.11f) * warp_freq, (y +  7.23f) * warp_freq, (z + 29.91f) * warp_freq, seed ^ 0xC2B2AE35u);
    return mu_noise_v3(x + warp_amp * qx, y + warp_amp * qy, z + warp_amp * qz);
}

MU_INLINE float mu_noise_domain_warped2(mu_noise_fn2 base, mu_noise_fn2 warp, float x, float y, uint32_t seed, float warp_amp, float warp_freq)
{
    mu_noise_vec2 p = mu_noise_domain_warp2(warp, x, y, seed, warp_amp, warp_freq);
    return base(p.x, p.y, seed);
}

MU_INLINE float mu_noise_domain_warped3(mu_noise_fn3 base, mu_noise_fn3 warp, float x, float y, float z, uint32_t seed, float warp_amp, float warp_freq)
{
    mu_noise_vec3 p = mu_noise_domain_warp3(warp, x, y, z, seed, warp_amp, warp_freq);
    return base(p.x, p.y, p.z, seed);
}

/*
    Anisotropic warp. Separate amplitudes and frequencies let you stretch
    noise along one axis (wind, water currents, brushed materials).
*/
MU_INLINE mu_noise_vec2 mu_noise_domain_warp2_ex(mu_noise_fn2 warp,
                                                 float x, float y, uint32_t seed,
                                                 float amp_x, float amp_y,
                                                 float freq_x, float freq_y)
{
    float qx = warp(x * freq_x, y * freq_y, seed ^ 0xA511E9B3u);
    float qy = warp((x + 19.19f) * freq_x, (y - 33.47f) * freq_y, seed ^ 0x63D83595u);
    return mu_noise_v2(x + amp_x * qx, y + amp_y * qy);
}

MU_INLINE mu_noise_vec3 mu_noise_domain_warp3_ex(mu_noise_fn3 warp,
                                                 float x, float y, float z,
                                                 uint32_t seed,
                                                 float amp_x, float amp_y, float amp_z,
                                                 float freq_x, float freq_y, float freq_z)
{
    float qx = warp(x * freq_x, y * freq_y, z * freq_z, seed ^ 0xA511E9B3u);
    float qy = warp((x + 19.19f) * freq_x, (y - 33.47f) * freq_y,
                    (z + 11.73f) * freq_z, seed ^ 0x63D83595u);
    float qz = warp((x - 47.11f) * freq_x, (y + 7.23f) * freq_y,
                    (z + 29.91f) * freq_z, seed ^ 0xC2B2AE35u);
    return mu_noise_v3(x + amp_x * qx, y + amp_y * qy, z + amp_z * qz);
}

/*
    Recursive (Inigo Quilez style) domain warp:

        p = warp(p)
        p = warp(p)
        ...
        result = base(p)

    Two or three levels are usually enough. More levels quickly turn into
    high-frequency smoke and are expensive.
*/
MU_INLINE mu_noise_vec2 mu_noise_domain_warp2_recursive(mu_noise_fn2 warp,
                                                        float x, float y,
                                                        uint32_t seed,
                                                        float warp_amp, float warp_freq,
                                                        int levels)
{
    mu_noise_vec2 p = mu_noise_v2(x, y);
    for (int i = 0; i < levels; ++i)
        p = mu_noise_domain_warp2(warp, p.x, p.y, seed + (uint32_t)i * 0x9E3779B9u,
                                  warp_amp, warp_freq);
    return p;
}

MU_INLINE mu_noise_vec3 mu_noise_domain_warp3_recursive(mu_noise_fn3 warp,
                                                        float x, float y, float z,
                                                        uint32_t seed,
                                                        float warp_amp, float warp_freq,
                                                        int levels)
{
    mu_noise_vec3 p = mu_noise_v3(x, y, z);
    for (int i = 0; i < levels; ++i)
        p = mu_noise_domain_warp3(warp, p.x, p.y, p.z, seed + (uint32_t)i * 0x9E3779B9u,
                                  warp_amp, warp_freq);
    return p;
}

MU_INLINE float mu_noise_domain_warped2_recursive(mu_noise_fn2 base, mu_noise_fn2 warp,
                                                  float x, float y, uint32_t seed,
                                                  float warp_amp, float warp_freq,
                                                  int levels)
{
    mu_noise_vec2 p = mu_noise_domain_warp2_recursive(warp, x, y, seed,
                                                      warp_amp, warp_freq, levels);
    return base(p.x, p.y, seed);
}

MU_INLINE float mu_noise_domain_warped3_recursive(mu_noise_fn3 base, mu_noise_fn3 warp,
                                                  float x, float y, float z,
                                                  uint32_t seed,
                                                  float warp_amp, float warp_freq,
                                                  int levels)
{
    mu_noise_vec3 p = mu_noise_domain_warp3_recursive(warp, x, y, z, seed,
                                                      warp_amp, warp_freq, levels);
    return base(p.x, p.y, p.z, seed);
}

/* ------------------------------------------------------------------------- */
/* 8) Curl noise                                                             */
/* ------------------------------------------------------------------------- */

/*
    Curl noise builds a divergence-free vector field.

    2D scalar potential Psi(x,y):
      v = ( dPsi/dy, -dPsi/dx )

    Finite difference approximation:
      dPsi/dx ≈ (Psi(x+e,y)-Psi(x-e,y))/(2e)
      dPsi/dy ≈ (Psi(x,y+e)-Psi(x,y-e))/(2e)
*/
MU_INLINE mu_noise_vec2 mu_noise_curl2(mu_noise_fn2 potential, float x, float y, uint32_t seed, float eps)
{
    float ex = (eps > 0.0f) ? eps : 1.0e-3f;

    float p_x1 = potential(x + ex, y, seed);
    float p_x0 = potential(x - ex, y, seed);
    float p_y1 = potential(x, y + ex, seed);
    float p_y0 = potential(x, y - ex, seed);

    float dpsi_dx = (p_x1 - p_x0) / (2.0f * ex);
    float dpsi_dy = (p_y1 - p_y0) / (2.0f * ex);

    return mu_noise_v2(dpsi_dy, -dpsi_dx);
}

/*
    3D curl from vector potential A = (Ax, Ay, Az):

      curl(A) = (
        dAz/dy - dAy/dz,
        dAx/dz - dAz/dx,
        dAy/dx - dAx/dy
      )

    We generate Ax,Ay,Az from the same scalar noise with different seeds.
*/
MU_INLINE mu_noise_vec3 mu_noise_curl3(mu_noise_fn3 scalar, float x, float y, float z, uint32_t seed, float eps)
{
    float ex = (eps > 0.0f) ? eps : 1.0e-3f;

    uint32_t sx = seed ^ 0xA511E9B3u;
    uint32_t sy = seed ^ 0x63D83595u;
    uint32_t sz = seed ^ 0xC2B2AE35u;

    /* dAz/dy and dAy/dz */
    float Az_y1 = scalar(x, y + ex, z, sz);
    float Az_y0 = scalar(x, y - ex, z, sz);
    float Ay_z1 = scalar(x, y, z + ex, sy);
    float Ay_z0 = scalar(x, y, z - ex, sy);

    /* dAx/dz and dAz/dx */
    float Ax_z1 = scalar(x, y, z + ex, sx);
    float Ax_z0 = scalar(x, y, z - ex, sx);
    float Az_x1 = scalar(x + ex, y, z, sz);
    float Az_x0 = scalar(x - ex, y, z, sz);

    /* dAy/dx and dAx/dy */
    float Ay_x1 = scalar(x + ex, y, z, sy);
    float Ay_x0 = scalar(x - ex, y, z, sy);
    float Ax_y1 = scalar(x, y + ex, z, sx);
    float Ax_y0 = scalar(x, y - ex, z, sx);

    float dAz_dy = (Az_y1 - Az_y0) / (2.0f * ex);
    float dAy_dz = (Ay_z1 - Ay_z0) / (2.0f * ex);

    float dAx_dz = (Ax_z1 - Ax_z0) / (2.0f * ex);
    float dAz_dx = (Az_x1 - Az_x0) / (2.0f * ex);

    float dAy_dx = (Ay_x1 - Ay_x0) / (2.0f * ex);
    float dAx_dy = (Ax_y1 - Ax_y0) / (2.0f * ex);

    return mu_noise_v3(
        dAz_dy - dAy_dz,
        dAx_dz - dAz_dx,
        dAy_dx - dAx_dy
    );
}

MU_INLINE mu_noise_vec2 mu_noise_curl2_normalized(mu_noise_fn2 potential,
                                                   float x, float y, uint32_t seed,
                                                   float eps)
{
    mu_noise_vec2 v = mu_noise_curl2(potential, x, y, seed, eps);
    return mu_noise_v2_normalize(v, 1.0e-8f);
}

MU_INLINE mu_noise_vec3 mu_noise_curl3_normalized(mu_noise_fn3 scalar,
                                                   float x, float y, float z,
                                                   uint32_t seed, float eps)
{
    mu_noise_vec3 v = mu_noise_curl3(scalar, x, y, z, seed, eps);
    return mu_noise_v3_normalize(v, 1.0e-8f);
}

MU_INLINE float mu_noise_curl2_magnitude(mu_noise_fn2 potential,
                                          float x, float y, uint32_t seed,
                                          float eps)
{
    return mu_noise_v2_len(mu_noise_curl2(potential, x, y, seed, eps));
}

/* Divergence of an arbitrary 2D vector field, by central differences. */
MU_INLINE float mu_noise_divergence2_fd(mu_noise_vfn2 field, float x, float y,
                                        uint32_t seed, float eps)
{
    float e = (eps > 0.0f) ? eps : 1.0e-3f;
    mu_noise_vec2 px1 = field(x + e, y, seed);
    mu_noise_vec2 px0 = field(x - e, y, seed);
    mu_noise_vec2 py1 = field(x, y + e, seed);
    mu_noise_vec2 py0 = field(x, y - e, seed);
    return (px1.x - px0.x) / (2.0f * e) + (py1.y - py0.y) / (2.0f * e);
}

/* Divergence of an arbitrary 3D vector field, by central differences. */
MU_INLINE float mu_noise_divergence3_fd(mu_noise_vfn3 field, float x, float y,
                                        float z, uint32_t seed, float eps)
{
    float e = (eps > 0.0f) ? eps : 1.0e-3f;
    mu_noise_vec3 px1 = field(x + e, y, z, seed);
    mu_noise_vec3 px0 = field(x - e, y, z, seed);
    mu_noise_vec3 py1 = field(x, y + e, z, seed);
    mu_noise_vec3 py0 = field(x, y - e, z, seed);
    mu_noise_vec3 pz1 = field(x, y, z + e, seed);
    mu_noise_vec3 pz0 = field(x, y, z - e, seed);
    return (px1.x - px0.x) / (2.0f * e) +
           (py1.y - py0.y) / (2.0f * e) +
           (pz1.z - pz0.z) / (2.0f * e);
}

/* ------------------------------------------------------------------------- */
/* Batch sampling                                                            */
/* ------------------------------------------------------------------------- */

/*
    Batch helpers use struct-of-arrays inputs and contiguous outputs:

      mu_noise_batch2(base, xs, ys, out, count, seed);

    The loops are written so compilers can auto-vectorize them. The callback
    form is convenient for switching noise families at runtime. For the
    tightest SIMD loops, call the concrete `mu_noise_value*`, `mu_noise_perlin*`
    or `mu_noise_simplex*` functions directly in your own batch loop, where the
    inliner can see the whole computation.

    `count` may be zero or negative; in that case the functions do nothing.
*/
MU_INLINE void mu_noise_batch1(mu_noise_fn1 base, const float* x, float* out,
                               int count, uint32_t seed)
{
    MU_NOISE_BATCH_SIMD
    for (int i = 0; i < count; ++i)
        out[i] = base(x[i], seed);
}

MU_INLINE void mu_noise_batch2(mu_noise_fn2 base, const float* x, const float* y,
                               float* out, int count, uint32_t seed)
{
    MU_NOISE_BATCH_SIMD
    for (int i = 0; i < count; ++i)
        out[i] = base(x[i], y[i], seed);
}

MU_INLINE void mu_noise_batch3(mu_noise_fn3 base, const float* x, const float* y,
                               const float* z, float* out, int count, uint32_t seed)
{
    MU_NOISE_BATCH_SIMD
    for (int i = 0; i < count; ++i)
        out[i] = base(x[i], y[i], z[i], seed);
}

MU_INLINE void mu_noise_batch4(mu_noise_fn4 base, const float* x, const float* y,
                               const float* z, const float* w, float* out,
                               int count, uint32_t seed)
{
    MU_NOISE_BATCH_SIMD
    for (int i = 0; i < count; ++i)
        out[i] = base(x[i], y[i], z[i], w[i], seed);
}

MU_INLINE void mu_noise_batch2_deriv(mu_noise_dfn2 base, const float* x, const float* y,
                                     mu_noise_sample2* out, int count, uint32_t seed)
{
    MU_NOISE_BATCH_SIMD
    for (int i = 0; i < count; ++i)
        out[i] = base(x[i], y[i], seed);
}

MU_INLINE void mu_noise_batch3_deriv(mu_noise_dfn3 base, const float* x, const float* y,
                                     const float* z, mu_noise_sample3* out,
                                     int count, uint32_t seed)
{
    MU_NOISE_BATCH_SIMD
    for (int i = 0; i < count; ++i)
        out[i] = base(x[i], y[i], z[i], seed);
}

MU_INLINE void mu_noise_batch4_deriv(mu_noise_dfn4 base, const float* x, const float* y,
                                     const float* z, const float* w,
                                     mu_noise_sample4* out, int count, uint32_t seed)
{
    MU_NOISE_BATCH_SIMD
    for (int i = 0; i < count; ++i)
        out[i] = base(x[i], y[i], z[i], w[i], seed);
}

/* ------------------------------------------------------------------------- */
/* Convenience recipes                                                       */
/* ------------------------------------------------------------------------- */

/*
    Useful one-liners for common terrain/material patterns.
*/
MU_INLINE float mu_noise_terrain_height(float x, float y, uint32_t seed)
{
    float n_continent = mu_noise_fbm2(mu_noise_simplex2, x * 0.0009f, y * 0.0009f, seed + 11u, 5, 2.0f, 0.5f);
    float n_detail    = mu_noise_ridged2(mu_noise_perlin2, x * 0.0070f, y * 0.0070f, seed + 29u, 4, 2.0f, 2.0f, 1.0f);
    return 0.75f * n_continent + 0.25f * n_detail;
}

MU_INLINE float mu_noise_marble(float x, float y, uint32_t seed)
{
    float warp = mu_noise_fbm2(mu_noise_simplex2, x * 1.5f, y * 1.5f, seed + 3u, 4, 2.0f, 0.5f);
    float t = x * 3.14159265f + 2.0f * warp;
    return sinf(t);
}

MU_INLINE float mu_noise_cells_edge(float x, float y, uint32_t seed)
{
    mu_noise_worley2_result w = mu_noise_worley2(x, y, seed, MU_NOISE_DIST_EUCLIDEAN);
    return w.f2 - w.f1;
}

MU_INLINE float mu_noise_clouds(float x, float y, uint32_t seed)
{
    float base = mu_noise_fbm2(mu_noise_perlin2, x, y, seed + 101u, 6, 2.0f, 0.55f);
    float detail = mu_noise_fbm2(mu_noise_simplex2, x * 3.7f, y * 3.7f, seed + 211u, 4, 2.1f, 0.5f);
    float density = mu_noise_remap_neg1_pos1_to_0_1(base);
    return mu_noise_clamp(density + 0.18f * detail, 0.0f, 1.0f);
}

MU_INLINE float mu_noise_wood(float x, float y, uint32_t seed)
{
    float warp = mu_noise_fbm2(mu_noise_simplex2, x * 1.7f, y * 1.7f, seed + 3u, 4, 2.0f, 0.5f);
    float rings = sinf((x + 0.35f * warp) * 18.0f) * 0.5f + 0.5f;
    float grain = mu_noise_fbm2(mu_noise_perlin2, x * 14.0f, y * 2.0f, seed + 17u, 3, 2.0f, 0.5f);
    return mu_noise_clamp(rings * 0.8f + grain * 0.2f, 0.0f, 1.0f);
}

MU_INLINE float mu_noise_rust(float x, float y, uint32_t seed)
{
    float patches = mu_noise_fbm2(mu_noise_simplex2, x * 2.0f, y * 2.0f, seed + 5u, 5, 2.0f, 0.55f);
    float pits = mu_noise_worley2_f1(x * 7.0f, y * 7.0f, seed + 9u, MU_NOISE_DIST_EUCLIDEAN);
    float mask = mu_noise_remap_neg1_pos1_to_0_1(patches);
    return mu_noise_clamp(mask * 0.7f + (1.0f - mu_noise_clamp(pits, 0.0f, 1.0f)) * 0.3f, 0.0f, 1.0f);
}

MU_INLINE float mu_noise_caustic(float x, float y, uint32_t seed)
{
    mu_noise_worley2_result c = mu_noise_worley2(x * 3.0f, y * 3.0f, seed + 31u,
                                                  MU_NOISE_DIST_EUCLIDEAN);
    float ridge = 1.0f - mu_noise_clamp(c.f2 - c.f1, 0.0f, 1.0f);
    float shimmer = mu_noise_fbm2(mu_noise_simplex2, x * 8.0f, y * 8.0f, seed + 47u,
                                  3, 2.0f, 0.5f);
    return mu_noise_clamp(ridge * ridge * 0.85f + shimmer * 0.15f, 0.0f, 1.0f);
}


/* =========================================================================
 * Adopted from SimplexNoise (Simon Geilfus, BSD-3), C99 port
 * =========================================================================
 *
 * What was missing here, checked function by function against
 * https://github.com/simongeilfus/SimplexNoise (its noise/dnoise/fBm/dfBm/
 * ridgedMF/iqfBm/iqMatfBm/flowNoise/dFlowNoise/worleyNoise-with-falloff set):
 *
 *   simplex1       1D simplex noise, and its analytic derivative.
 *                  We had 2D/3D/4D only. Gustavson's 1D case is the cheapest
 *                  way to get smooth 1-D variation (a 1D value noise has
 *                  visible lattice kinks at every integer).
 *   flow2/flow3    The gradient rotation trick: rotate every corner gradient
 *                  by a constant angle before the dot product. The lattice and
 *                  the hashes are unchanged, so the field is still continuous
 *                  and still tiles, but the noise is *sheared* along the angle
 *                  you ask for -- the direction-field look you want for cloth,
 *                  water streaks and brushed metal. Same cost as plain
 *                  simplex: it is the same code with sin/cos folded into the
 *                  gradient lookup, not a second noise evaluation.
 *   fbm4/turbulence4/billow4/ridged4
 *                  4D members of the fractal family (we had 1D-3D).
 *   dfbm1/2/3      fBm that carries the analytic derivative through the octave
 *                  sum. Needed any time you want a slope, not a height: a
 *                  terrain normal, a warp field, a distance-field gradient.
 *                  It is exact, not a finite difference of the sum.
 *   iq_fbm2/3      Inigo Quilez's variant: no explicit gain, the accumulated
 *                  derivative damps each octave by 1/(1+|d|^2). Flat regions
 *                  get more octaves, steep ones get fewer, which keeps detail
 *                  where the eye looks without the usual gain tuning.
 *   iq_rot_fbm2    Same sum, but every octave is multiplied by a rotation-scale
 *                  matrix, so the lattice axes never line up between octaves and
 *                  the classic 0/45/90 degree haze does not appear.
 *   worley2_falloff / worley3_falloff
 *                  Cellular noise with exponential falloff over the 27 (or 9)
 *                  neighbour cells: smooth blobs instead of hard F1 ridges.
 *                  Our mu_noise_worley* returns distances; this returns the
 *                  smoothed field.
 *
 * Not adopted, and why:
 *   - Their perm[512] table + global seed(): every function here takes a seed
 *     and hashes it, so there is no global state to seed and no cache-resident
 *     table to share between threads.
 *   - dFlowNoise: the analytic derivative of the rotated field is a second
 *     gradient rotation applied to the derivative tensor. Flow noise is used
 *     for advection, where the derivative is not needed; if you need one,
 *     mu_noise_sample2_fd(base_flow, x, y, seed, eps) gives a correct central
 *     difference of any scalar field.
 *   - curl(): we already have mu_noise_curl2/curl3 plus finite-difference
 *     divergence and Laplacian helpers, which take a function pointer to any
 *     scalar field.
 */

/*
    1D simplex noise.

    Same construction as 2D, in one dimension: the lattice cell [i, i+1] has a
    random +-1 gradient at each end, and each end's influence is weighted by
    (1 - x^2)^4, the 1D form of the (0.5 - |d|^2)^4 kernel.

        n_i = (1 - d_i^2)^4 * g_i * d_i        with d_i = x - i

    The two ends overlap, so the sum is scaled by the peak of one cell:
    max over x in [0,1] of |(1-x^2)^4 * x + (1-(x-1)^2)^4 * (x-1)| = 0.316406
    (verified numerically over 4 sign combinations and 200k samples), so the
    factor is 1/0.316406 = 3.160494 to land in [-1, 1]. Gustavson's published
    constant for this function is 0.395, which assumes the 2D kernel; using it
    here would leave the output in +-0.13 and quietly cost you 80% of the
    amplitude whenever the result is fed to a shader as if it were +-1.
    The kernel is C2 at the cell boundary, which is why the result has no
    lattice kinks even though the gradients are random.

    Cost: two hashes, two multiplies per term. This is the cheapest smooth
    1D noise in the header; mu_noise_value1 costs a hash pair and two lerps and
    has the same continuity, so prefer value1 unless you need the tapered
    kernel (simplex1 fades to exactly zero at the cell midpoint, value1 does
    not).
*/
MU_INLINE float mu_noise_simplex1(float x, uint32_t seed)
{
    int i0 = (int)floorf(x);
    int i1 = i0 + 1;

    float x0 = x - (float)i0;
    float x1 = x0 - 1.0f;

    float n0 = 0.0f;
    float n1 = 0.0f;

    float t0 = 1.0f - x0 * x0;
    if (t0 > 0.0f)
    {
        float g0 = (mu_noise_hash_1i(i0, seed) & 1U) ? -1.0f : 1.0f;
        t0 *= t0;
        n0 = t0 * t0 * g0 * x0;
    }

    float t1 = 1.0f - x1 * x1;
    if (t1 > 0.0f)
    {
        float g1 = (mu_noise_hash_1i(i1, seed) & 1U) ? -1.0f : 1.0f;
        t1 *= t1;
        n1 = t1 * t1 * g1 * x1;
    }

    return 3.1604938f * (n0 + n1);
}

/*
    1D simplex noise with its analytic derivative.

    d/dx of (1-x^2)^4 * g * x is g * (1-x^2)^3 * (1 - 9x^2), which is exact and
    costs two extra multiplies per corner -- much cheaper than the two extra
    evaluations a central difference would need, and it does not step off the
    lattice to get it.

    The gradient is returned in .gradient.x so the type matches the 2D/3D/4D
    derivative samplers; .gradient.y is always 0.
*/
MU_INLINE mu_noise_sample2 mu_noise_simplex1_deriv(float x, uint32_t seed)
{
    mu_noise_sample2 r;

    int i0 = (int)floorf(x);
    int i1 = i0 + 1;

    float x0 = x - (float)i0;
    float x1 = x0 - 1.0f;

    float n0 = 0.0f, d0 = 0.0f;
    float n1 = 0.0f, d1 = 0.0f;

    float t0 = 1.0f - x0 * x0;
    if (t0 > 0.0f)
    {
        float g0 = (mu_noise_hash_1i(i0, seed) & 1U) ? -1.0f : 1.0f;
        float t2 = t0 * t0;
        n0 = t2 * t2 * g0 * x0;
        d0 = g0 * t2 * t0 * (1.0f - 9.0f * x0 * x0);
    }

    float t1 = 1.0f - x1 * x1;
    if (t1 > 0.0f)
    {
        float g1 = (mu_noise_hash_1i(i1, seed) & 1U) ? -1.0f : 1.0f;
        float t2 = t1 * t1;
        n1 = t2 * t2 * g1 * x1;
        d1 = g1 * t2 * t1 * (1.0f - 9.0f * x1 * x1);
    }

    r.value = 3.1604938f * (n0 + n1);
    r.gradient = mu_noise_v2(3.1604938f * (d0 + d1), 0.0f);
    return r;
}

/*
    Flow noise, 2D: simplex noise whose corner gradients are rotated by `angle`.

    Plain simplex noise is isotropic on average, but a texture built from it
    still looks "random"; flow noise looks *directional*. The construction is
    one line of geometry: take the corner gradient g, rotate it by theta,

        g' = ( cos*t gx - sin*t gy , sin*t gx + cos*t gy )

    and dot that with the corner offset instead of g. Rotating every gradient by
    the same angle rotates the vector field of the noise, so the iso-lines run
    along theta. Nothing else changes: the lattice, hashes, kernel and 70x scale
    are the 2D simplex ones, so cost is identical to mu_noise_simplex2 plus one
    sin/cos pair that a caller in a loop should hoist (there is no state here,
    so hoisting means passing the angle pre-computed as sin/cos and calling
    mu_noise_flow2_sc).

    Thresholding flow noise along its angle gives streaks that stay parallel
    instead of blobbing, which is why it is the usual choice for wind streaks,
    brushed metal and water currents.
*/
MU_INLINE float mu_noise_flow2_sc(float x, float y, float sin_t, float cos_t, uint32_t seed)
{
    const float F2 = 0.36602540378443864676f;
    const float G2 = 0.21132486540518711775f;

    float s = (x + y) * F2;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);

    float t = (float)(i + j) * G2;
    float x0 = x - ((float)i - t);
    float y0 = y - ((float)j - t);

    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; }
    else         { i1 = 0; j1 = 1; }

    float x1 = x0 - (float)i1 + G2;
    float y1 = y0 - (float)j1 + G2;
    float x2 = x0 - 1.0f + 2.0f * G2;
    float y2 = y0 - 1.0f + 2.0f * G2;

    float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0.0f)
    {
        mu_noise_vec2 g = mu_noise_grad2_from_hash(mu_noise_hash_2i(i, j, seed));
        float gx = cos_t * g.x - sin_t * g.y;
        float gy = sin_t * g.x + cos_t * g.y;
        t0 *= t0;
        n0 = t0 * t0 * (gx * x0 + gy * y0);
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0.0f)
    {
        mu_noise_vec2 g = mu_noise_grad2_from_hash(mu_noise_hash_2i(i + i1, j + j1, seed));
        float gx = cos_t * g.x - sin_t * g.y;
        float gy = sin_t * g.x + cos_t * g.y;
        t1 *= t1;
        n1 = t1 * t1 * (gx * x1 + gy * y1);
    }

    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0.0f)
    {
        mu_noise_vec2 g = mu_noise_grad2_from_hash(mu_noise_hash_2i(i + 1, j + 1, seed));
        float gx = cos_t * g.x - sin_t * g.y;
        float gy = sin_t * g.x + cos_t * g.y;
        t2 *= t2;
        n2 = t2 * t2 * (gx * x2 + gy * y2);
    }

    return 70.0f * (n0 + n1 + n2);
}

MU_INLINE float mu_noise_flow2(float x, float y, float angle, uint32_t seed)
{
    return mu_noise_flow2_sc(x, y, sinf(angle), cosf(angle), seed);
}

/*
    Flow noise, 3D: the 3D analogue rotates each corner gradient around the
    Z axis by `angle`, so the noise is coherent in XY and streaks along the
    rotated direction. (A full 3D rotation needs an axis as well as an angle;
    the two extra parameters live in mu_noise_flow3_axis, and this is the
    common case.)
*/
MU_INLINE float mu_noise_flow3_axis(float x, float y, float z,
                                    float axis_x, float axis_y, float axis_z,
                                    float angle, uint32_t seed)
{
    mu_noise_vec3 axis = mu_noise_v3_normalize(mu_noise_v3(axis_x, axis_y, axis_z), 1e-6f);
    float c = cosf(angle);
    float s = sinf(angle);
    float k = 1.0f - c;

    float m00 = c + axis.x * axis.x * k;
    float m01 = axis.x * axis.y * k - axis.z * s;
    float m02 = axis.x * axis.z * k + axis.y * s;
    float m10 = axis.y * axis.x * k + axis.z * s;
    float m11 = c + axis.y * axis.y * k;
    float m12 = axis.y * axis.z * k - axis.x * s;
    float m20 = axis.z * axis.x * k - axis.y * s;
    float m21 = axis.z * axis.y * k + axis.x * s;
    float m22 = c + axis.z * axis.z * k;

    const float F3 = 1.0f / 3.0f;
    const float G3 = 1.0f / 6.0f;

    float sk = (x + y + z) * F3;
    int i = (int)floorf(x + sk);
    int j = (int)floorf(y + sk);
    int k3 = (int)floorf(z + sk);

    float t = (float)(i + j + k3) * G3;
    float x0 = x - ((float)i - t);
    float y0 = y - ((float)j - t);
    float z0 = z - ((float)k3 - t);

    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0)
    {
        if (y0 >= z0)      { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else               { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    }
    else
    {
        if (y0 < z0)       { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0)  { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else               { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }

    float x1 = x0 - (float)i1 + G3, y1 = y0 - (float)j1 + G3, z1 = z0 - (float)k1 + G3;
    float x2 = x0 - (float)i2 + 2.0f * G3, y2 = y0 - (float)j2 + 2.0f * G3, z2 = z0 - (float)k2 + 2.0f * G3;
    float x3 = x0 - 1.0f + 3.0f * G3, y3 = y0 - 1.0f + 3.0f * G3, z3 = z0 - 1.0f + 3.0f * G3;

    float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f, n3 = 0.0f;
    float tt;

    tt = 0.5f - x0 * x0 - y0 * y0 - z0 * z0;
    if (tt > 0.0f)
    {
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(i, j, k3, seed));
        float gx = m00 * g.x + m01 * g.y + m02 * g.z;
        float gy = m10 * g.x + m11 * g.y + m12 * g.z;
        float gz = m20 * g.x + m21 * g.y + m22 * g.z;
        tt *= tt;
        n0 = tt * tt * (gx * x0 + gy * y0 + gz * z0);
    }

    tt = 0.5f - x1 * x1 - y1 * y1 - z1 * z1;
    if (tt > 0.0f)
    {
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + i1, j + j1, k3 + k1, seed));
        float gx = m00 * g.x + m01 * g.y + m02 * g.z;
        float gy = m10 * g.x + m11 * g.y + m12 * g.z;
        float gz = m20 * g.x + m21 * g.y + m22 * g.z;
        tt *= tt;
        n1 = tt * tt * (gx * x1 + gy * y1 + gz * z1);
    }

    tt = 0.5f - x2 * x2 - y2 * y2 - z2 * z2;
    if (tt > 0.0f)
    {
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + i2, j + j2, k3 + k2, seed));
        float gx = m00 * g.x + m01 * g.y + m02 * g.z;
        float gy = m10 * g.x + m11 * g.y + m12 * g.z;
        float gz = m20 * g.x + m21 * g.y + m22 * g.z;
        tt *= tt;
        n2 = tt * tt * (gx * x2 + gy * y2 + gz * z2);
    }

    tt = 0.5f - x3 * x3 - y3 * y3 - z3 * z3;
    if (tt > 0.0f)
    {
        mu_noise_vec3 g = mu_noise_grad3_from_hash(mu_noise_hash_3i(i + 1, j + 1, k3 + 1, seed));
        float gx = m00 * g.x + m01 * g.y + m02 * g.z;
        float gy = m10 * g.x + m11 * g.y + m12 * g.z;
        float gz = m20 * g.x + m21 * g.y + m22 * g.z;
        tt *= tt;
        n3 = tt * tt * (gx * x3 + gy * y3 + gz * z3);
    }

    return 72.0f * (n0 + n1 + n2 + n3);
}

MU_INLINE float mu_noise_flow3(float x, float y, float z, float angle, uint32_t seed)
{
    return mu_noise_flow3_axis(x, y, z, 0.0f, 0.0f, 1.0f, angle, seed);
}

/*
    fBm with analytic derivatives: the octave sum of value+derivative samples.

    For an fBm  F(p) = sum_i a_i * n(f_i * p),  the derivative is
    dF/dp = sum_i a_i * f_i * dn/dp evaluated at f_i * p -- the chain rule
    multiplies each octave's gradient by its frequency, which is the part a
    naive "sum the values, finite-difference the sum" gets right only if the
    step is smaller than the highest octave's feature size. Carrying the
    analytic gradient keeps the slope exact at every scale.

    Returns value plus gradient (already frequency-weighted).
*/
MU_INLINE mu_noise_sample2 mu_noise_dfbm2(mu_noise_dfn2 base, float x, float y, uint32_t seed,
                                          int octaves, float lacunarity, float gain)
{
    mu_noise_sample2 r;
    float sum = 0.0f;
    float gx = 0.0f, gy = 0.0f;
    float amp = 1.0f, freq = 1.0f, norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        mu_noise_sample2 s = base(x * freq, y * freq, seed + (uint32_t)i * 1013u);
        float w = amp * freq;
        sum += amp * s.value;
        gx += w * s.gradient.x;
        gy += w * s.gradient.y;
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    if (norm > 0.0f)
    {
        sum /= norm;
        gx /= norm;
        gy /= norm;
    }

    r.value = sum;
    r.gradient = mu_noise_v2(gx, gy);
    return r;
}

MU_INLINE mu_noise_sample3 mu_noise_dfbm3(mu_noise_dfn3 base, float x, float y, float z, uint32_t seed,
                                          int octaves, float lacunarity, float gain)
{
    mu_noise_sample3 r;
    float sum = 0.0f;
    float gx = 0.0f, gy = 0.0f, gz = 0.0f;
    float amp = 1.0f, freq = 1.0f, norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        mu_noise_sample3 s = base(x * freq, y * freq, z * freq, seed + (uint32_t)i * 1013u);
        float w = amp * freq;
        sum += amp * s.value;
        gx += w * s.gradient.x;
        gy += w * s.gradient.y;
        gz += w * s.gradient.z;
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    if (norm > 0.0f)
    {
        sum /= norm;
        gx /= norm;
        gy /= norm;
        gz /= norm;
    }

    r.value = sum;
    r.gradient = mu_noise_v3(gx, gy, gz);
    return r;
}

MU_INLINE mu_noise_sample2 mu_noise_dfbm1(mu_noise_fn1 base, float x, uint32_t seed,
                                          int octaves, float lacunarity, float gain, float eps)
{
    /* 1D has no analytic derivative sampler in this header (the value/perlin/
     * simplex 1D families return floats), so the slope comes from a central
     * difference of the *sum*, which is exact for a C1 field and only costs two
     * extra evaluations, not 2*octaves. */
    mu_noise_sample2 r;
    r.value = mu_noise_fbm1(base, x, seed, octaves, lacunarity, gain);
    r.gradient = mu_noise_v2(
        (mu_noise_fbm1(base, x + eps, seed, octaves, lacunarity, gain) -
         mu_noise_fbm1(base, x - eps, seed, octaves, lacunarity, gain)) / (2.0f * eps),
        0.0f);
    return r;
}

/*
    Inigo Quilez's fBm.

    Ordinary fBm adds octaves with a fixed geometric amplitude. IQ's variant
    lets the noise decide: it accumulates the derivative of the octaves so far
    and damps the next octave by 1 / (1 + |d|^2). Where the field is flat the
    damping is ~1 and the octaves keep adding detail; where it is steep the
    damping shrinks and the sum stays smooth. The practical effect is strong,
    small-scale detail in flat areas and no crumple on slopes, without touching
    the gain.

        d        += dn_i
        f        += 0.5 * n_i / (1 + d.d)
        p        *= 2 * rot(0.5)

    Note there is no `gain` parameter on purpose: amplitude is fixed at 0.5 and
    the damping does the work. That is the whole point of the variant.
*/
MU_INLINE float mu_noise_iq_fbm2(mu_noise_dfn2 base, float x, float y, uint32_t seed, int octaves)
{
    float f = 0.0f;
    float dx = 0.0f, dy = 0.0f;
    float px = x, py = y;
    const float ca = 0.8f, sa = 0.6f; /* cos/sin of ~0.6435 rad: a 2x rotation */

    for (int i = 0; i < octaves; ++i)
    {
        mu_noise_sample2 s = base(px, py, seed + (uint32_t)i * 1013u);
        dx += s.gradient.x;
        dy += s.gradient.y;
        f += 0.5f * s.value / (1.0f + dx * dx + dy * dy);
        /* rotate and double: octaves never re-align with the lattice axes */
        {
            float nx = 2.0f * (ca * px - sa * py);
            float ny = 2.0f * (sa * px + ca * py);
            px = nx;
            py = ny;
        }
    }

    return f;
}

MU_INLINE float mu_noise_iq_fbm3(mu_noise_dfn3 base, float x, float y, float z, uint32_t seed, int octaves)
{
    float f = 0.0f;
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    float px = x, py = y, pz = z;
    const float ca = 0.8f, sa = 0.6f;

    for (int i = 0; i < octaves; ++i)
    {
        mu_noise_sample3 s = base(px, py, pz, seed + (uint32_t)i * 1013u);
        dx += s.gradient.x;
        dy += s.gradient.y;
        dz += s.gradient.z;
        f += 0.5f * s.value / (1.0f + dx * dx + dy * dy + dz * dz);
        {
            float nx = 2.0f * (ca * px - sa * py);
            float ny = 2.0f * (sa * px + ca * py);
            px = nx;
            py = ny;
            pz *= 2.0f;
        }
    }

    return f;
}

/*
    Rotated-lattice fBm, 2D: ordinary fBm with a rotation-scale between octaves.

    The reason to bother: with a pure scale, octave i's lattice lines up with
    octave i-1's at 0, 45 and 90 degrees, and the eye picks the resulting
    cross-hatch out of the sum. Multiplying by a rotation as well as a scale
    means each octave samples the lattice at a different angle, so there is no
    common axis. `angle` per octave in radians; `scale` is the lacunarity.
*/
MU_INLINE float mu_noise_iq_rot_fbm2(mu_noise_fn2 base, float x, float y, uint32_t seed,
                                     int octaves, float scale, float angle, float gain)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    float px = x, py = y;
    float ca = cosf(angle) * scale;
    float sa = sinf(angle) * scale;

    for (int i = 0; i < octaves; ++i)
    {
        sum += amp * base(px, py, seed + (uint32_t)i * 1013u);
        norm += amp;
        {
            float nx = ca * px - sa * py;
            float ny = sa * px + ca * py;
            px = nx;
            py = ny;
        }
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

/*
    4D members of the fractal family.

    The 4D bases already existed (mu_noise_value4, perlin4, simplex4, worley4);
    the compositions did not. Same shape as the 1D-3D ones, so a 4D noise
    volume animates through the same code path a 3D one does.
*/
MU_INLINE float mu_noise_fbm4(mu_noise_fn4 base, float x, float y, float z, float w,
                              uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        sum += amp * base(x * freq, y * freq, z * freq, w * freq, seed + (uint32_t)i * 1013u);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_turbulence4(mu_noise_fn4 base, float x, float y, float z, float w,
                                     uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, z * freq, w * freq, seed + (uint32_t)i * 1013u);
        sum += amp * mu_noise_abs(n);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_billow4(mu_noise_fn4 base, float x, float y, float z, float w,
                                 uint32_t seed, int octaves, float lacunarity, float gain)
{
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, z * freq, w * freq, seed + (uint32_t)i * 1013u);
        sum += amp * (2.0f * mu_noise_abs(n) - 1.0f);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

MU_INLINE float mu_noise_ridged4(mu_noise_fn4 base, float x, float y, float z, float w,
                                 uint32_t seed, int octaves, float lacunarity, float gain, float offset)
{
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, weight = 1.0f;

    for (int i = 0; i < octaves; ++i)
    {
        float n = base(x * freq, y * freq, z * freq, w * freq, seed + (uint32_t)i * 1013u);
        float signal = offset - mu_noise_abs(n);
        signal *= signal;
        signal *= weight;
        weight = mu_noise_clamp(signal * gain, 0.0f, 1.0f);
        sum += signal * amp;
        freq *= lacunarity;
        amp *= 0.5f;
    }

    return mu_noise_clamp(sum * 2.0f - 1.0f, -1.0f, 1.0f);
}

/*
    Cellular noise with exponential falloff ("smooth cellular").

    mu_noise_worley2 returns F1 (distance to the nearest feature point), which
    has a hard crease at every cell boundary: the field is C0, not C1, so
    bump-mapping or normals computed from it show the cell edges as lines.

    The falloff form replaces the hard minimum with a soft-max over the
    neighbourhood:

        v = -(1/k) * ln( (1/N) * sum_neighbours exp(-k * d_n) )

    which is a smooth lower envelope: as k grows it converges to F1 (hard
    cells), as k shrinks it converges to the average distance (blobs). The
    standard use is blobs/cell fields that still look cellular. Because the
    neighbours are hashed, this costs the full 3x3 (or 3x3x3) search -- it is
    the same loop the F1/F2 version already runs, so the cost is not the
    exponential, it is the neighbour scan.
*/
MU_INLINE float mu_noise_worley2_falloff(float x, float y, uint32_t seed, float falloff)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    float sum = 0.0f;

    if (falloff < 1e-4f) falloff = 1e-4f;

    for (int j = -1; j <= 1; ++j)
    {
        for (int i = -1; i <= 1; ++i)
        {
            int cx = ix + i;
            int cy = iy + j;

            uint32_t h = mu_noise_hash_2i(cx, cy, seed);
            float fx = (float)cx + mu_noise_hash_to_unit_float(h) - x;
            float fy = (float)cy + mu_noise_hash_to_unit_float(mu_noise_hash_combine(h, 0x9E3779B9u)) - y;

            float d = sqrtf(fx * fx + fy * fy);
            sum += expf(-falloff * d);
        }
    }

    /* Divide by the neighbour count: without it the result dives towards -inf
     * as the falloff shrinks (the log of a sum of nine terms), and the useful
     * range of k becomes tiny. With it, k -> 0 gives the mean distance and
     * k -> inf gives F1, so k interpolates between "blobs" and "cells". */
    return -(1.0f / falloff) * logf(sum * (1.0f / 9.0f));
}

MU_INLINE float mu_noise_worley3_falloff(float x, float y, float z, uint32_t seed, float falloff)
{
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    int iz = (int)floorf(z);
    float sum = 0.0f;

    if (falloff < 1e-4f) falloff = 1e-4f;

    for (int k = -1; k <= 1; ++k)
    {
        for (int j = -1; j <= 1; ++j)
        {
            for (int i = -1; i <= 1; ++i)
            {
                int cx = ix + i;
                int cy = iy + j;
                int cz = iz + k;

                uint32_t h = mu_noise_hash_3i(cx, cy, cz, seed);
                float fx = (float)cx + mu_noise_hash_to_unit_float(h) - x;
                float fy = (float)cy + mu_noise_hash_to_unit_float(mu_noise_hash_combine(h, 0x9E3779B9u)) - y;
                float fz = (float)cz + mu_noise_hash_to_unit_float(mu_noise_hash_combine(h, 0x85EBCA6Bu)) - z;

                float d = sqrtf(fx * fx + fy * fy + fz * fz);
                sum += expf(-falloff * d);
            }
        }
    }

    return -(1.0f / falloff) * logf(sum * (1.0f / 27.0f));
}

#ifdef __cplusplus
}
#endif

#endif /* MU_NOISE_MATH_H */
