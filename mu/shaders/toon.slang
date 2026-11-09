/* mu/toon.slang — cel / stylized lighting, ramps, rims and hatching.
 * ============================================================================
 *  The visual difference between "toon" and "PBR with a low roughness" is
 *  QUANTIZATION, not a different BRDF. Every band here goes through
 *  mu_quantize_steps_soft() from quantize.slang, so the STYLE job and the
 *  STORAGE job share one implementation and one set of edge semantics.
 *
 *  Cost rules:
 *   - No atan2/sin/cos in the hot path. Fixed hatch directions are constants,
 *     not mu_sincos() calls (that would be 8 quarter-rate ops per pixel).
 *   - Rim/specular exponents are offered as multiply chains (mu_pow2/4/8) and
 *     as a generic mu_pow form; pick the multiply chain. 0 quarter-rate ops
 *     vs 2 for exp2(y*log2(x)).
 *   - All edges are step() or smoothstep(), never an `if`.
 * ==========================================================================*/
#ifndef MU_TOON_SLANG
#define MU_TOON_SLANG

#include "common.slang"
#include "quantize.slang"

/* ------------------------------------------------------------ N.L variants */
/* Valve's half-Lambert: maps [-1,1] to [0,1] with a squared falloff. One
 * multiply, and it keeps the terminator from crushing dark-side detail. */
float mu_half_lambert(float NoL) { float t = NoL * 0.5 + 0.5; return t * t; }

/* Wrapped diffuse, normalized so a fully lit surface still returns 1. */
float mu_wrap_lambert(float NoL, float wrap)
{
    return mu_saturate((NoL + wrap) / (1.0 + wrap));
}
/* Wrapped + half-Lambert hybrid, the common stylized-character look. */
float mu_wrap_half_lambert(float NoL, float wrap)
{
    return mu_pow2(mu_saturate((NoL + wrap) / (1.0 + wrap)));
}

/* --------------------------------------------------------------- cel bands */
/* The one band function to call. `softness` is the fraction of a step that is
 * blended: 0 = hard band (shimmers under TAA), 0.1..0.25 = stable in motion. */
float mu_cel_band(float x, float steps, float softness)
{
    return mu_quantize_steps_soft(x, steps, softness);
}

float mu_cel_diffuse(float NoL, float steps, float softness, float shadow_bias)
{
    return mu_cel_band(mu_saturate(NoL + shadow_bias), steps, softness);
}

/* --------------------------------------------------------------- ramps */
/* 3-stop gradient ramp, two lerps with a step mask (no branch). */
float3 mu_ramp3(float t, float3 shadow, float3 mid, float3 light)
{
    float x = mu_saturate(t);
    float3 lo = lerp(shadow, mid, mu_saturate(x * 2.0));
    float3 hi = lerp(mid, light, mu_saturate(x * 2.0 - 1.0));
    return lerp(lo, hi, step(0.5, x));
}

/* N-stop ramp evaluated from a packed colour word is a texture lookup in
 * practice; this is the analytic 4-stop version for procedural materials. */
float3 mu_ramp4(float t, float3 c0, float3 c1, float3 c2, float3 c3)
{
    float x = mu_saturate(t) * 3.0;
    float3 a = lerp(c0, c1, mu_saturate(x));
    float3 b = lerp(c1, c2, mu_saturate(x - 1.0));
    float3 c = lerp(c2, c3, mu_saturate(x - 2.0));
    float3 ab = lerp(a, b, step(1.0, x));
    return lerp(ab, c, step(2.0, x));
}

/* ------------------------------------------------------------------- rim */
/* Multiply-chain rims: 0 quarter-rate ops. */
float mu_rim2(float3 N, float3 V) { return mu_pow2(1.0 - mu_saturate(dot(N, V))); }
float mu_rim4(float3 N, float3 V) { return mu_pow4(1.0 - mu_saturate(dot(N, V))); }
float mu_rim8(float3 N, float3 V) { return mu_pow8(1.0 - mu_saturate(dot(N, V))); }

/* Arbitrary exponent: 2 quarter-rate ops (exp2 + log2). Prefer the three
 * fixed forms above; they are visually indistinguishable in a ramp. */
float mu_rim(float3 N, float3 V, float power)
{
    return mu_pow(1.0 - mu_saturate(dot(N, V)), power);
}

/* Rim gated by the key light so the silhouette only lights on the lit side —
 * the single most common "stylized" tweak, and it is two multiplies. */
float mu_rim_light(float3 N, float3 V, float3 L, float power, float bias)
{
    float f = mu_rim4(N, V);
    return mu_saturate((f - bias) * power) * mu_saturate(dot(N, L) * 0.5 + 0.5);
}

/* Fresnel in stylized form: biased, then banded. */
float mu_stylized_fresnel(float3 N, float3 V, float bias, float steps, float softness)
{
    return mu_cel_band(mu_saturate(mu_rim4(N, V) + bias), steps, softness);
}

/* -------------------------------------------------------------- specular */
/* Fixed-exponent Blinn-Phong with multiply chains. No pow, no rcp, no sqrt. */
float mu_blinn8(float3 N, float3 L, float3 V)
{
    float3 H = mu_safe_normalize(L + V);            /* 1 rsqrt */
    return mu_pow8(mu_saturate(dot(N, H)));
}
float mu_blinn16(float3 N, float3 L, float3 V)
{
    float3 H = mu_safe_normalize(L + V);            /* 1 rsqrt */
    return mu_pow4(mu_pow4(mu_saturate(dot(N, H))));
}
/* Generic exponent: 2 quarter-rate ops on top of the rsqrt. */
float mu_blinn(float3 N, float3 L, float3 V, float exponent)
{
    float3 H = mu_safe_normalize(L + V);            /* 1 rsqrt */
    return mu_pow(mu_saturate(dot(N, H)), exponent);
}

/* Banded specular highlight (anime "star" or hard shine). */
float mu_cel_specular(float3 N, float3 L, float3 V, float exponent,
                      float steps, float softness)
{
    return mu_cel_band(mu_blinn(N, L, V, exponent), steps, softness) *
           mu_cel_band(mu_saturate(dot(N, L)), 3.0, 0.15);   /* 2 quarter-rate ops from mu_blinn */
}
/* Anisotropic-ish stretched hair highlight: band along the tangent direction. */
float mu_hair_highlight(float NoH, float tangent_coord, float steps, float softness, float freq)
{
    float stripe = mu_bayer4(float2(tangent_coord * freq, 0.0));
    float wave   = mu_pow2(frac(tangent_coord * freq) * 2.0 - 1.0);
    return mu_cel_band(mu_saturate(NoH * (1.0 - wave) + stripe * 0.25), steps, softness);
}

/* -------------------------------------------------------------- hatching */
/* One hatch direction as a periodic line mask. `dir` is a UNIT, constant
 * vector — callers use mu_hatch4() below, which hard-codes the four angles
 * so no sin/cos is evaluated per pixel. */
float mu_hatch_line(float2 uv, float2 dir, float freq, float thickness)
{
    float v = dot(uv, dir) * freq;
    return step(frac(v), thickness);                /* 1 frac, 1 step */
}

/* 4-level cross-hatch from a scalar intensity (0 = darkest). Returns the ink
 * coverage in [0,1]; multiply your paper colour by (1-ink).
 * Costs 4 frac + 4 step + 3 lerps, zero quarter-rate ops. */
float mu_hatch4(float2 uv, float intensity, float scale)
{
    float t  = mu_saturate(intensity);
    float2 p = uv * scale;
    float l0 = mu_hatch_line(p, float2(0.70710678, 0.70710678), 1.0, 0.5);
    float l1 = mu_hatch_line(p, float2(0.0, 1.0),               1.0, 0.5);
    float l2 = mu_hatch_line(p, float2(-0.70710678, 0.70710678),1.0, 0.5);
    float l3 = mu_hatch_line(p, float2(1.0, 0.0),               1.0, 0.5);
    /* level 0 (dark) = all four line sets, level 4 (bright) = none. The
       cumulative max() is what makes the lines cross instead of alias. */
    float ink0 = max(max(l0, l1), max(l2, l3));
    float ink1 = max(max(l0, l1), l2);
    float ink2 = max(l0, l1);
    float ink3 = l0;
    float x = t * 4.0;
    float r = lerp(ink0, ink1, step(1.0, x));
    r = lerp(r, ink2, step(2.0, x));
    r = lerp(r, ink3, step(3.0, x));
    r = lerp(r, 0.0,  step(4.0, x));
    return r;
}

/* Screen-space paper grain, cheap and stable: IGN + a 4x4 Bayer offset. */
float mu_paper_grain(float2 pixel, float amount)
{
    return (mu_ign(pixel) - 0.5) * amount + (mu_bayer4(pixel) - 0.5) * amount;
}

/* --------------------------------------------------------------- outlines */
/* Depth/normal/id edge detectors. They take neighbour values the caller has
 * already fetched, so there are no implicit derivative instructions and the
 * same function works in a compute post-pass. */
float mu_edge_depth(float d0, float d1, float threshold)
{
    return step(threshold, abs(d1 - d0));
}
float mu_edge_normal(float3 n0, float3 n1, float threshold)
{
    return step(threshold, 1.0 - dot(n0, n1));
}
float mu_edge_id(uint a, uint b) { return float(select(a != b, 1u, 0u)); }
float mu_edge_combine(float depth_edge, float normal_edge, float id_edge)
{
    return mu_saturate(max(max(depth_edge, normal_edge), id_edge));
}
/* Depth-aware threshold: further edges need a larger depth delta to count. */
float mu_edge_depth_scale(float depth, float base, float scale)
{
    return base + scale * depth;
}

/* --------------------------------------------------------- one-call shade */
struct MuToonParams
{
    float3 albedo;
    float3 shadow_tint;
    float  steps;
    float  softness;
    float  rim_strength;
    float3 rim_color;
    float  spec_steps;
    float  spec_strength;
};

/* The complete stylized lighting model in one call. Inputs are normalized N,
 * L, V; radiance is the (already shadowed) light colour. */
float3 mu_toon_shade(MuToonParams p, float3 N, float3 L, float3 V, float3 radiance)
{
    float  NoL   = dot(N, L);
    float  band  = mu_cel_diffuse(NoL, p.steps, p.softness, 0.0);
    float3 lit   = p.albedo * radiance * band;
    float3 dark  = p.albedo * p.shadow_tint;
    float3 base  = lerp(dark, lit, step(0.5, band));
    float  rim   = mu_rim_light(N, V, L, p.rim_strength, 0.2) * p.rim_strength;
    float  spec  = mu_cel_specular(N, L, V, 16.0, p.spec_steps, p.softness) * p.spec_strength;
    return base + p.rim_color * rim + radiance * spec;
}

#endif /* MU_TOON_SLANG */
