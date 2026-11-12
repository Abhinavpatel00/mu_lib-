#pragma once
#ifndef MU_SHADER_NOISE_H
#define MU_SHADER_NOISE_H

/* mu_shader_noise.h — the C mirror of mu/shaders/{common,noise}.slang.
 *
 * What problem it solves
 * ----------------------
 * A procedural world has to be evaluated on both sides of the bus. The GPU
 * evaluates it per pixel per frame because that is where the pixels are. The
 * CPU evaluates it once per entity per tick, for the questions a shader cannot
 * answer: what is the ground height under this aircraft, did that just hit a
 * hill, where is this weather cell allowed to spawn.
 *
 * If the two sides disagree, the terrain you crash into is not the terrain you
 * can see, and the only remaining fix is to bake one side into a texture and
 * upload it — which costs the memory, the bandwidth and the reload hitch that
 * an analytic field was chosen to avoid.
 *
 * mu/shaders builds every generator on the PCG3D integer hash from
 * common.slang. mu_noise_math.h builds its generators on mu_noise_hash_2i and
 * mu_noise_hash_3i, which is a different hash, so the two libraries produced
 * two different worlds from the same seed. Neither is wrong; they were simply
 * never the same function. This header is the missing half.
 *
 * The contract
 * ------------
 * Same names, same statement order, same wraparound. The integer parts then
 * agree bit for bit and the float parts agree to the last rounding step.
 *
 * Statement order IS the API here. mu_pcg3d updates x, then y from the new x,
 * then z from the new x and y. Slang runs those three statements in order and
 * so does this file. A "tidied" version that computes all three from the old
 * values is a different function and silently moves the whole world.
 *
 * One known and accepted difference: Slang's lerp() may lower to either
 * a + (b - a) t or a (1 - t) + b t depending on the target. These differ by at
 * most one ULP of the result, which on a 2 km height field is under a
 * micrometre. Callers comparing the two sides should compare with a tolerance,
 * not with ==. new_apiexp's tests/shader_noise.c measures the real gap by
 * running both sides over the same lattice and reporting the worst case.
 *
 * Scope: PCG3D, the cell hash, 2D value noise and the three 2D fractal
 * wrappers mu/shaders/noise.slang builds on it. The gradient, simplex, worley
 * and curl generators are not mirrored yet. When a workload needs one of them
 * on both sides, add it here — not in engine code, and not as a third hash.
 */

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

/* ------------------------------------------------------------------ PCG3D */

/* common.slang: uint3 mu_pcg3d(uint3 v)
 * Three independent streams from one call. Use one component; calling it three
 * times for one value is three times the work. */
MU_INLINE uint32_t mu_pcg3d(uint32_t x, uint32_t y, uint32_t z, uint32_t out[3])
{
    /* v = v * 1664525u + 1013904223u;  (componentwise, wrapping) */
    x = x * 1664525u + 1013904223u;
    y = y * 1664525u + 1013904223u;
    z = z * 1664525u + 1013904223u;

    /* Sequential, not parallel: y reads the x that was just written. */
    x += y * z;
    y += z * x;
    z += x * y;

    /* v ^= v >> 16u; */
    x ^= x >> 16u;
    y ^= y >> 16u;
    z ^= z >> 16u;

    x += y * z;
    y += z * x;
    z += x * y;

    out[0] = x;
    out[1] = y;
    out[2] = z;
    return x;
}

/* common.slang: float mu_hash01(uint3 v)
 * float(uint32) rounds to nearest in both languages. */
MU_INLINE float mu_hash01(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t h[3];
    mu_pcg3d(x, y, z, h);
    return (float)h[0] * (1.0f / 4294967296.0f);
}

/* ------------------------------------------------------------- cell hash */

/* noise.slang: float mu_cell_hash(int2 c)
 * The int2 -> uint2 cast is a two's-complement reinterpretation in Slang and a
 * (uint32_t) cast here, so negative cell coordinates land on the same lattice
 * point on both sides and there is no special case for them. */
MU_INLINE float mu_cell_hash(int cx, int cy)
{
    return mu_hash01((uint32_t)cx, (uint32_t)cy, 0u);
}

MU_INLINE void mu_cell_hash2(int cx, int cy, float out[2])
{
    uint32_t h[3];
    mu_pcg3d((uint32_t)cx, (uint32_t)cy, 0u, h);
    out[0] = (float)h[0] * (1.0f / 4294967296.0f);
    out[1] = (float)h[1] * (1.0f / 4294967296.0f);
}

/* ----------------------------------------------------------- value noise */

/* noise.slang: float mu_value_noise2(float2 p), result in [-1, 1].
 * Four hashes, a few FMAs, no quarter-rate op and no trig: the smoothstep is
 * the polynomial f*f*(3-2f). */
MU_INLINE float mu_value_noise2(float px, float py)
{
    const float ix = floorf(px);
    const float iy = floorf(py);
    const float fx = px - ix;
    const float fy = py - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const int   cx = (int)ix;
    const int   cy = (int)iy;
    const float a  = mu_cell_hash(cx, cy);
    const float b  = mu_cell_hash(cx + 1, cy);
    const float c  = mu_cell_hash(cx, cy + 1);
    const float d  = mu_cell_hash(cx + 1, cy + 1);
    return (a + (b - a) * ux + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy) * 2.0f - 1.0f;
}

/* ------------------------------------------------------- fractal wrappers */

/* common.slang: #define MU_FBM_ROT float2x2(0.8, 0.6, -0.6, 0.8)
 * A unit rotation between octaves, applied as mul(M, p), which is row-major
 * in HLSL: x' = 0.8x + 0.6y, y' = -0.6x + 0.8y. It kills the axis-aligned
 * look without a trig call. */
#define MU_FBM_ROT_X(x, y) (0.8f * (x) + 0.6f * (y))
#define MU_FBM_ROT_Y(x, y) (-0.6f * (x) + 0.8f * (y))

/* Advance one octave: rotate, scale by lacunarity, offset. The +11.7 is in
 * the shader and has to be here too or the lattices stop lining up. */
MU_INLINE void mu_fbm_step(float *x, float *y, float lacunarity)
{
    const float rx = MU_FBM_ROT_X(*x, *y);
    const float ry = MU_FBM_ROT_Y(*x, *y);
    *x = rx * lacunarity + 11.7f;
    *y = ry * lacunarity + 11.7f;
}

/* noise.slang: float mu_fbm4(float2 p, float lacunarity, float gain)
 * Four octaves, unrolled, a predictable 16 hashes. Normalised by the sum of
 * the amplitudes, so the result stays inside [-1, 1] for gain <= 1. */
MU_INLINE float mu_fbm4(float px, float py, float lacunarity, float gain)
{
    float a = 0.5f, sum = 0.0f, norm = 0.0f;
    int   i;
    for (i = 0; i < 4; ++i) {
        sum += a * mu_value_noise2(px, py);
        norm += a;
        mu_fbm_step(&px, &py, lacunarity);
        a *= gain;
    }
    return sum / norm;
}

/* noise.slang: float mu_ridged4(float2 p, float lacunarity, float gain)
 * abs() then inverted and squared: sharp creases, which is what terrain and
 * lightning both want. Result in [0, 1]. */
MU_INLINE float mu_ridged4(float px, float py, float lacunarity, float gain)
{
    float a = 0.5f, sum = 0.0f, norm = 0.0f;
    int   i;
    for (i = 0; i < 4; ++i) {
        const float n = 1.0f - fabsf(mu_value_noise2(px, py));
        sum += a * n * n;
        norm += a;
        mu_fbm_step(&px, &py, lacunarity);
        a *= gain;
    }
    return sum / norm;
}

/* noise.slang: float mu_turbulence4(float2 p, float lacunarity, float gain)
 * Sum of |noise|: the classic marble and fire-distortion field. */
MU_INLINE float mu_turbulence4(float px, float py, float lacunarity, float gain)
{
    float a = 0.5f, sum = 0.0f, norm = 0.0f;
    int   i;
    for (i = 0; i < 4; ++i) {
        sum += a * fabsf(mu_value_noise2(px, py));
        norm += a;
        mu_fbm_step(&px, &py, lacunarity);
        a *= gain;
    }
    return sum / norm;
}

/* noise.slang: float2 mu_domain_warp2(float2 p, float strength)
 * Two extra noise evaluations turn grid-like fbm into swirling flow. */
MU_INLINE void mu_domain_warp2(float px, float py, float strength, float out[2])
{
    out[0] = px + strength * mu_value_noise2(px, py);
    out[1] = py + strength * mu_value_noise2(px + 5.2f, py + 1.3f);
}

MU_END_EXTERN_C

#endif /* MU_SHADER_NOISE_H */
