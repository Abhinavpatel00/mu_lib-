/* mu/shadows.slang — filtering, soft shadows, moments and cascades.
 * ============================================================================
 *  Conventions (do not "fix" these by guessing):
 *
 *   - `depth_sign` is +1 for standard [0=near,1=far] depth and -1 for
 *     reverse-Z [1=near,0=far]. The comparison helper applies it once.
 *   - Comparison-sampler functions (mu_pcf3x3, mu_pcf_bilinear) use a
 *     SamplerComparisonState: configure its compare op for YOUR depth
 *     convention, then `ref` is the receiver depth as stored.
 *   - Raw-depth functions (Gather / PCSS / VSM) do their own compare so they
 *     work with a plain SamplerState and with reverse-Z.
 *   - MU_NDC_TO_UV matches README.md's coordinate table: the engine installs a
 *     negative-height viewport, so a clip-space y of +1 lands at uv.y = 0.
 *     Engines with a positive-height viewport pass float2(0.5, 0.5).
 *
 *  Texture instruction counts matter more than ALU here: one Gather is four
 *  texels for the price of one instruction. mu_pcf_gather_box() gives a 16-tap
 *  filter in FOUR texture ops.
 * ==========================================================================*/
#ifndef MU_SHADOWS_SLANG
#define MU_SHADOWS_SLANG

#include "common.slang"
#include "brdf_util.slang"

/* MU_NDC_TO_UV lives in common.slang so post.slang's reconstruction uses the
 * same convention as the shadow projection. */

/* ----------------------------------------------------------- projections */
float3 mu_clip_to_ndc(float4 clip) { return clip.xyz * mu_rcp(clip.w); }  /* 1 scalar rcp */
float2 mu_ndc_to_uv(float3 ndc, float2 ndc_scale)
{
    return mu_mad(ndc.xy, ndc_scale, float2(0.5));       /* 1 Fma */
}
float2 mu_shadow_uv(float4x4 light_vp, float3 world_pos, float2 ndc_scale)
{
    float4 clip = mul(light_vp, float4(world_pos, 1.0));
    return mu_ndc_to_uv(mu_clip_to_ndc(clip), ndc_scale);
}
/* Out-of-map guard, branch-free. */
float mu_shadow_in_bounds(float2 uv, float ref)
{
    float2 inside = step(float2(0.0), uv) * step(uv, float2(1.0));
    return inside.x * inside.y * step(0.0, ref) * step(ref, 1.0);
}

/* --------------------------------------------------------------- biases */
/* Slope-scaled depth bias. The (1-NoL) term is the cheap proxy for dz/dx. */
float mu_depth_bias(float NoL, float constant_bias, float slope_bias)
{
    return constant_bias + slope_bias * (1.0 - mu_saturate(NoL));
}
/* Normal-offset: move the receiver along its normal before projecting, so the
 * acne is removed geometrically instead of with a large depth bias (which
 * causes peter-panning). `texel_world` is the shadow texel footprint in world
 * units; scale ~1..3. */
float3 mu_normal_offset(float3 world_pos, float3 N, float3 L,
                        float texel_world, float scale)
{
    float k = (1.0 - mu_saturate(dot(N, L))) * texel_world * scale;
    return world_pos + N * k;
}

/* ------------------------------------------------------ comparison PCF */
/* Uniform 3x3 PCF: 9 texture instructions, no branches. */
float mu_pcf3x3(Texture2D<float> shadow, SamplerComparisonState cmp,
                float2 uv, float ref, float2 inv_size)
{
    float2 o = inv_size;
    float s = 0.0;
    s += shadow.SampleCmpLevelZero(cmp, uv + float2(-o.x, -o.y), ref);
    s += shadow.SampleCmpLevelZero(cmp, uv + float2( 0.0, -o.y), ref);
    s += shadow.SampleCmpLevelZero(cmp, uv + float2( o.x, -o.y), ref);
    s += shadow.SampleCmpLevelZero(cmp, uv + float2(-o.x,  0.0), ref);
    s += shadow.SampleCmpLevelZero(cmp, uv,                        ref);
    s += shadow.SampleCmpLevelZero(cmp, uv + float2( o.x,  0.0), ref);
    s += shadow.SampleCmpLevelZero(cmp, uv + float2(-o.x,  o.y), ref);
    s += shadow.SampleCmpLevelZero(cmp, uv + float2( 0.0,  o.y), ref);
    s += shadow.SampleCmpLevelZero(cmp, uv + float2( o.x,  o.y), ref);
    return s * (1.0 / 9.0);
}

/* Bilinear 3x3 PCF: 4 texture instructions instead of 9. The four
 * comparison samples sit at the texel centres around uv; their weights are
 * the outer product of the bilinear fractions. This is the default choice. */
float mu_pcf_bilinear(Texture2D<float> shadow, SamplerComparisonState cmp,
                      float2 uv, float ref, float2 texel_size, float2 tex_size)
{
    float2 p    = uv * tex_size - 0.5;
    float2 f    = frac(p);
    float2 base = (floor(p) + 0.5) * texel_size;
    float2 w0   = 1.0 - f;
    float  s00  = shadow.SampleCmpLevelZero(cmp, base, ref);
    float  s10  = shadow.SampleCmpLevelZero(cmp, base + float2(texel_size.x, 0.0), ref);
    float  s01  = shadow.SampleCmpLevelZero(cmp, base + float2(0.0, texel_size.y), ref);
    float  s11  = shadow.SampleCmpLevelZero(cmp, base + texel_size, ref);
    return s00 * (w0.x * w0.y) + s10 * (f.x * w0.y)
         + s01 * (w0.x * f.y)  + s11 * (f.x * f.y);
}

/* ----------------------------------------------- raw-depth / Gather PCF */
/* Compare 4 gathered depths against ref, with depth_sign. */
float4 mu_shadow_compare4(float4 d, float ref, float depth_sign)
{
    return step(float4(0.0), (d - float4(ref)) * depth_sign);
}

/* 16-tap box PCF in FOUR texture instructions. Uniform weights deliberately:
 * they make the result independent of Gather's channel ordering, which differs
 * between GLSL/HLSL conventions. One Gather = one 2x2 quad. */
float mu_pcf_gather_box(Texture2D<float> shadow, SamplerState samp,
                        float2 uv, float ref, float2 inv_size, float depth_sign)
{
    float2 o = inv_size;
    float4 g0 = shadow.Gather(samp, uv + float2(-o.x, -o.y));
    float4 g1 = shadow.Gather(samp, uv + float2( o.x, -o.y));
    float4 g2 = shadow.Gather(samp, uv + float2(-o.x,  o.y));
    float4 g3 = shadow.Gather(samp, uv + float2( o.x,  o.y));
    float4 c = mu_shadow_compare4(g0, ref, depth_sign)
             + mu_shadow_compare4(g1, ref, depth_sign)
             + mu_shadow_compare4(g2, ref, depth_sign)
             + mu_shadow_compare4(g3, ref, depth_sign);
    return (c.x + c.y + c.z + c.w) * (1.0 / 16.0);
}

/* 16 Poisson-ish taps, rotated per pixel by IGN. One texture instruction each,
 * but the rotation kills the banding a fixed grid leaves on curved surfaces. */
static const float2 MU_POISSON16[16] = {
    float2(-0.94201624, -0.39906216), float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845), float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554), float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507), float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367), float2( 0.14383161, -0.14100790)
};

float mu_pcf_poisson(Texture2D<float> shadow, SamplerState samp,
                     float2 uv, float ref, float2 inv_size,
                     float radius, float2 pixel, float depth_sign)
{
    float a  = MU_2PI * mu_ign(pixel);             /* 1 fma chain, no trig */
    float2 sc = mu_sincos(a);                      /* 2 quarter-rate ops, ONCE */
    float2x2 rot = float2x2(sc.y, sc.x, -sc.x, sc.y);
    float sum = 0.0;
    [ForceUnroll]
    for (int i = 0; i < 16; ++i)
    {
        float2 o = mul(rot, MU_POISSON16[i]) * radius * inv_size;
        float  d = shadow.SampleLevel(samp, uv + o, 0.0);
        sum += step(0.0, (d - ref) * depth_sign);
    }
    return sum * (1.0 / 16.0);
}

/* ------------------------------------------------------------- PCSS */
/* Average blocker depth. Returns -1 when the search found no blocker; the
 * caller then uses a hard 1-tap shadow instead of a wide filter. */
float mu_pcss_blocker(Texture2D<float> shadow, SamplerState samp,
                      float2 uv, float ref, float2 inv_size,
                      float search_radius, float depth_sign)
{
    float sum = 0.0, count = 0.0;
    [ForceUnroll]
    for (int i = 0; i < 16; ++i)
    {
        float  d = shadow.SampleLevel(samp, uv + MU_POISSON16[i] * search_radius * inv_size, 0.0);
        float  w = step(0.0, (ref - d) * depth_sign);   /* d is closer than ref */
        sum   += d * w;
        count += w;
    }
    return select(count > 0.0, sum / max(count, 1.0), -1.0);
}

/* Penumbra width in texels. `receiver`/`blocker` are distances FROM THE LIGHT
 * (use 1-ref for standard depth, ref for reverse-Z), so their difference is
 * positive. This is the similar-triangles term from PCSS. */
float mu_pcss_penumbra(float receiver, float blocker, float light_size)
{
    return mu_saturate((receiver - blocker) * light_size / max(blocker, MU_EPS));
}

/* --------------------------------------------------------- moment shadows */
/* Chebyshev upper bound with light-bleed reduction. `moments` = (E[d], E[d^2]).
 * One rcp, no exp/log. */
float mu_vsm(float2 moments, float depth, float min_variance, float bleed)
{
    float mean = moments.x;
    float var  = max(moments.y - mean * mean, min_variance);
    float d    = depth - mean;
    float p    = var / (var + d * d);
    p = select(d <= 0.0, 1.0, p);                  /* fully lit in front */
    return mu_smooth01(mu_linearstep(bleed, 1.0, p));
}
/* Exponential shadow map: 1 quarter-rate exp2 on lookup. */
float mu_esm(float exp_occluder, float receiver, float c)
{
    return mu_saturate(exp_occluder * mu_exp_neg(c * receiver));
}
/* Variance for a 4-tap box blur of the moments (EVSM/MSM prefilter helper). */
float2 mu_moment_blur(float2 m0, float2 m1, float2 m2, float2 m3)
{
    return (m0 + m1 + m2 + m3) * 0.25;
}

/* --------------------------------------------------------------- cascades */
/* Which of 4 splits contains `view_depth` (a positive distance from the eye).
 * step + dot, no branch, no loop. */
float mu_cascade_index(float view_depth, float4 splits)
{
    return min(dot(step(splits, float4(view_depth)), float4(1.0)), 3.0);
}
/* Blend weight in [0,1] for the overlap band [split-width, split] between
 * cascade i and i+1. Zero outside the band. */
float mu_cascade_blend_weight(float view_depth, float split, float width)
{
    return mu_saturate((view_depth - (split - max(width, MU_EPS))) / max(width, MU_EPS));
}
/* Fade shadows out at the far plane so the last cascade has no hard edge. */
float mu_shadow_fade(float view_depth, float max_distance, float fade_width)
{
    return 1.0 - mu_saturate((view_depth - (max_distance - fade_width)) / max(fade_width, MU_EPS));
}

#endif /* MU_SHADOWS_SLANG */
