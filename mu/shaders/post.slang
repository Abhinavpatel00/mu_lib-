/* mu/post.slang — tonemapping, dithering, sharpening, bloom and screen utils.
 * ============================================================================
 *  All functions are leaf helpers over values: the caller owns the textures
 *  and the passes. That is deliberate — a "post-processing library" that binds
 *  its own descriptors is impossible to schedule into someone else's render
 *  graph, and a helper that takes 13 texture samples implicitly hides 13
 *  bandwidth costs from the person budgeting the pass.
 *
 *  Tonemapper cost, measured by mu/tests/measure.py (slangc 2026.21, -O3,
 *  net SPIR-V ops inside the entry point, runtime input):
 *
 *    mu_tonemap_reinhard         3 ops,  0 GLSL extended instructions
 *    mu_tonemap_aces_fitted     10 ops,  0 extended instructions
 *    mu_tonemap_hable           13 ops,  0 extended instructions
 *    mu_tonemap_aces            18 ops,  0 extended instructions (Hill fit)
 *    mu_tonemap_uchimura        26 ops,  Log2 + Exp2 + SmoothStep
 *    mu_tonemap_neutral         33 ops,  0 extended instructions
 *    mu_tonemap_agx             47 ops,  Log2 + Exp2 (6 matrix dot-triples)
 *
 *  No LUT, no branch, no texture fetch in any of them. Choose by how much
 *  highlight hue fidelity you need, not by which name sounds more modern.
 * ==========================================================================*/
#ifndef MU_POST_SLANG
#define MU_POST_SLANG

#include "common.slang"
#include "brdf_util.slang"

/* ------------------------------------------------------------- exposure */
float3 mu_exposure(float3 c, float ev) { return c * exp2(ev); }  /* 1 exp2 */

/* ----------------------------------------------------------- tonemapping */
float3 mu_tonemap_reinhard(float3 c) { return c / (1.0 + c); }
float3 mu_tonemap_reinhard_white(float3 c, float white)
{
    return (c * (1.0 + c / (white * white))) / (1.0 + c);
}

/* Narkowicz's ACES filmic fit. Cheapest good-looking curve: one rcp per
 * channel. Input must be exposure-scaled linear, output is display-linear. */
float3 mu_tonemap_aces_fitted(float3 x)
{
    return mu_saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

/* Stephen Hill's fit of the full ACES RRT+ODT, with the sRGB->AP1 and
 * AP1->sRGB transforms. Written as explicit dot() triples so there is no
 * row/column-major matrix ambiguity across SPIR-V backends. */
float3 mu_aces_input_transform(float3 c)
{
    return float3(dot(c, float3(0.59719, 0.35458, 0.04823)),
                  dot(c, float3(0.07600, 0.90834, 0.01566)),
                  dot(c, float3(0.02840, 0.13383, 0.83777)));
}
float3 mu_aces_output_transform(float3 c)
{
    return float3(dot(c, float3( 1.60475, -0.53108, -0.07367)),
                  dot(c, float3(-0.10208,  1.10813, -0.00605)),
                  dot(c, float3(-0.00327, -0.07276,  1.07602)));
}
float3 mu_tonemap_aces(float3 c)
{
    float3 v = mu_aces_input_transform(c);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return mu_saturate(mu_aces_output_transform(a / b));
}

/* Khronos PBR Neutral: preserves hue and in-gamut colours exactly, compresses
 * only the out-of-range peak. Branch-free version of the reference. */
float3 mu_tonemap_neutral(float3 c)
{
    const float start = 0.8 - 0.04;
    const float dsat  = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = select(x < 0.08, x - 6.25 * x * x, 0.04);   /* toe */
    c -= offset;
    float peak = mu_max3(c);
    float d    = 1.0 - start;
    float np   = 1.0 - d * d / max(peak + d - start, MU_EPS);
    float3 scaled = c * (np / max(peak, MU_EPS));
    float g = 1.0 - 1.0 / (dsat * (peak - np) + 1.0);
    return lerp(c, lerp(scaled, float3(np), g), step(start, peak));
}

/* Uchimura 2017 "HDR theory and practice": toe / linear / shoulder segments blended
 * by smoothstep weights. `P` max brightness, `a` contrast, `m` linear start,
 * `l` linear length, `c` black, `b` pedestal. Defaults are the paper's.
 * 1 log2 (toe pow) + 1 exp2 (shoulder) + 1 rcp. */
float3 mu_tonemap_uchimura(float3 x, float P, float a, float m,
                           float l, float c, float b)
{
    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;
    float3 w0 = float3(1.0) - smoothstep(float3(0.0), float3(m), x);
    float3 w2 = step(float3(m + l0), x);
    float3 w1 = float3(1.0) - w0 - w2;
    float3 T  = m * exp2(c * log2(max(x / m, float3(MU_EPS_SQ)))) + b;  /* pow(x/m,c) */
    float3 S  = P - (P - S1) * exp2((CP * (x - S0)) * MU_LOG2_E);
    float3 L  = m + a * (x - m);
    return mu_saturate(mu_mad(T, w0, mu_mad(L, w1, S * w2)));
}
float3 mu_tonemap_uchimura(float3 x)
{
    return mu_tonemap_uchimura(x, 1.0, 1.0, 0.22, 0.4, 1.33, 0.0);
}

/* Uncharted 2 / Hable filmic, normalized to a white point of 11.2. */
float3 mu_hable_curve(float3 x)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    /* REVERTED mad() form: mu_tonemap_hable also evaluates this curve at the
     * constant white point W=11.2, and slangc folds the original expression
     * into a constant but does NOT fold OpExtInst Fma. The mad() spelling cost
     * +3 net SPIR-V ops (measure.py: 13 -> 16). No change => reverted. */
    return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}
float3 mu_tonemap_hable(float3 c)
{
    const float W = 11.2;
    return mu_saturate(mu_hable_curve(c * 2.0) / mu_hable_curve(float3(W)));
}

/* AgX minimal (Wrensch / Filament / three.js), LUT-free. Matrices are spelled
 * as dot() triples to avoid column-major ambiguity. Costs 1 log2 + 1 exp2
 * plus ~30 FMAs; there is no cheap version that keeps the AgX hue behaviour,
 * so call this once per pixel, not once per light. */
float3 mu_agx_sigmoid(float3 c)
{
    float3 x2 = c * c;
    float3 x4 = x2 * x2;
    /* Horner in x2, with c kept as a scalar-first coefficient. Same
     * polynomial, 6 Fma instead of 6 mul + 6 add. */
    float3 p = mu_mads(x2, 15.5, float3(-40.14) * c + float3(31.96));
    p = mu_mad(p, x2, float3(-6.868) * c + float3(0.4298));
    return mu_mad(p, x2, float3(0.1191) * c - float3(0.00232));
}
float3 mu_tonemap_agx(float3 c)
{
    const float min_ev = -12.47393, max_ev = 4.026069;
    /* sRGB -> Rec2020 */
    c = float3(dot(c, float3(0.6274, 0.3293, 0.0433)),
               dot(c, float3(0.0691, 0.9195, 0.0113)),
               dot(c, float3(0.0164, 0.0880, 0.8956)));
    /* inset */
    c = float3(dot(c, float3(0.8566271533, 0.0951212405, 0.0482516061)),
               dot(c, float3(0.1373189729, 0.7612419906, 0.1014390365)),
               dot(c, float3(0.1118982130, 0.0767994186, 0.8113023684)));
    c = mu_saturate((clamp(log2(max(c, float3(1e-10))), min_ev, max_ev) - min_ev) / (max_ev - min_ev));
    c = mu_saturate(mu_agx_sigmoid(c));
    /* display encoding (exp2 + log2, not 3 Pow) */
    c = mu_srgb_to_linear_fast(c);
    /* outset */
    c = float3(dot(c, float3( 1.1271005818, -0.1106066431, -0.0164939387)),
               dot(c, float3(-0.1413297635,  1.1578237022, -0.0164939387)),
               dot(c, float3(-0.1413297635, -0.1106066431,  1.2519364066)));
    /* Rec2020 -> sRGB */
    c = float3(dot(c, float3( 1.6605, -0.5876, -0.0728)),
               dot(c, float3(-0.1246,  1.1329, -0.0083)),
               dot(c, float3(-0.0182, -0.1006,  1.1187)));
    return mu_saturate(c);
}

/* -------------------------------------------------------------- dithering */
/* Triangular-PDF dither removes both banding AND the fixed-pattern noise a
 * uniform dither leaves. IGN is 1 fma chain, no hash. */
float3 mu_dither_ign(float3 c, float2 pixel, float amount)
{
    float n = mu_ign(pixel) - 0.5;
    return c + float3(n) * amount;
}
/* 4x4 ordered dither: deterministic, TAA-friendly, exactly 16 levels. */
float3 mu_dither_bayer(float3 c, float2 pixel, float amount)
{
    return c + float3(mu_bayer4(pixel) - 0.5) * amount;
}
/* Apply dither BEFORE quantizing to `bits` per channel. */
float3 mu_dither_quantize(float3 c, float2 pixel, float bits)
{
    float scale = exp2(bits) - 1.0;               /* 1 exp2 */
    float3 n = float3(mu_ign(pixel), mu_ign(pixel + 17.3), mu_ign(pixel + 41.7)) - 0.5;
    return floor(mu_saturate(c) * scale + n + 0.5) / scale;
}

/* ------------------------------------------------------------ colour tools */
float3 mu_saturation(float3 c, float s)
{
    float l = mu_luma709(c);
    return lerp(float3(l), c, s);
}
float3 mu_contrast(float3 c, float contrast, float pivot)
{
    return (c - pivot) * contrast + pivot;
}
float3 mu_temperature(float3 c, float temp)  /* temp > 0 warm, < 0 cool */
{
    return mu_saturate(c * float3(1.0 + temp, 1.0, 1.0 - temp));
}
float3 mu_film_grain(float3 c, float2 pixel, float amount, float time)
{
    float n = mu_hash01(uint3(uint2(pixel), uint(time * 60.0))) - 0.5;
    return c + float3(n) * amount * (1.0 - mu_saturate(mu_luma709(c)));
}
float3 mu_vignette(float3 c, float2 uv, float radius, float softness)
{
    float2 d = uv - 0.5;
    float  r = sqrt(dot(d, d));                    /* 1 quarter-rate sqrt */
    float  v = 1.0 - mu_smooth01(mu_saturate((r - radius) / max(softness, MU_EPS)));
    return c * lerp(0.75, 1.0, v);
}
/* Chromatic aberration offset in UV. Feed it to three separate samples. */
float2 mu_chromatic_aberration(float2 uv, float2 center, float amount)
{
    return (uv - center) * amount;
}

/* ------------------------------------------------------------- sharpening */
/* Luma-weighted unsharp mask. `c` is the centre; n/s/e/w the 4 neighbours. */
float3 mu_cas_sharpen(float3 c, float3 n, float3 s, float3 e, float3 w, float sharpness)
{
    float3 blur = (n + s + e + w) * 0.25;
    float lc = mu_luma709(c);
    float lmin = min(min(mu_luma709(n), mu_luma709(s)),
                     min(mu_luma709(e), mu_luma709(w)));
    float lmax = max(max(mu_luma709(n), mu_luma709(s)),
                     max(mu_luma709(e), mu_luma709(w)));
    float range = lmax - lmin;
    float wgt = mu_saturate(range / max(lmax, MU_EPS)) * mu_saturate(1.0 - range);
    return c + (c - blur) * (wgt * sharpness);
}
float3 mu_unsharp(float3 c, float3 blurred, float amount)
{
    return c + (c - blurred) * amount;
}

/* ------------------------------------------------------------------ bloom */
/* Karis prefilter weight: suppress the single-pixel firefly before it gets
 * spread across the whole bloom chain. One rcp. */
float3 mu_karis_weight(float3 c)
{
    return c * (1.0 / (1.0 + mu_luma709(c)));
}
/* Soft threshold with a knee (Jimenez, COD:Advanced Warfare). */
float3 mu_bloom_threshold(float3 c, float threshold, float knee)
{
    float l = mu_luma709(c);
    float soft = mu_saturate(l - threshold + knee);
    soft = soft * soft / max(4.0 * knee, MU_EPS);
    float contrib = max(soft, l - threshold) / max(l, MU_EPS);
    return c * mu_saturate(contrib);
}
/* 13-tap downsample (Jimenez): 5 boxes + centre, 13 fetches done by caller.
 * Firefly suppression is applied to each 2x2 box via mu_karis_weight. */
/* 13-tap downsample (Jimenez, COD: Advanced Warfare). Weights sum to 1:
 * centre 0.125, corners 4x0.03125, edges 4x0.0625, inner diagonals 4x0.125.
 * The caller applies mu_karis_weight() to each 2x2 box before averaging and
 * divides by the sum of those weights if fireflies are a problem. */
float3 mu_bloom_downsample13(float3 a, float3 b, float3 c, float3 d,
                             float3 e, float3 f, float3 g, float3 h,
                             float3 i, float3 j, float3 k, float3 l, float3 m)
{
    return e * 0.125
         + (a + c + g + i) * 0.03125
         + (b + d + f + h) * 0.0625
         + (j + k + l + m) * 0.125;
}
/* 3x3 tent upsample: outer product of [1,2,1]/4 with itself. */
float3 mu_bloom_upsample9(float3 s0, float3 s1, float3 s2,
                          float3 s3, float3 s4, float3 s5,
                          float3 s6, float3 s7, float3 s8)
{
    float3 w0 = s0 + 2.0 * s1 + s2;
    float3 w1 = s3 + 2.0 * s4 + s5;
    float3 w2 = s6 + 2.0 * s7 + s8;
    return (w0 + 2.0 * w1 + w2) * (1.0 / 16.0);
}

/* ------------------------------------------------------ screen-space utils */
float2 mu_uv_from_pixel(float2 pixel, float2 inv_resolution) { return (pixel + 0.5) * inv_resolution; }
float2 mu_pixel_from_uv(float2 uv, float2 resolution)        { return uv * resolution - 0.5; }
/* Invert mu_ndc_to_uv (shadows.slang) so reconstruction agrees with the
 * engine's negative-height viewport. */
float2 mu_uv_to_ndc(float2 uv, float2 ndc_scale)
{
    return (uv - 0.5) / ndc_scale;
}
/* Standard [0,1] depth -> eye-space linear distance. */
float mu_linear_depth(float depth, float near_plane, float far_plane, bool reverse_z)
{
    float d = select(reverse_z, 1.0 - depth, depth);
    return (near_plane * far_plane) / (far_plane - d * (far_plane - near_plane));
}
float3 mu_reconstruct_position(float2 uv, float depth, float4x4 inv_view_proj,
                               float2 ndc_scale)
{
    float2 ndc = mu_uv_to_ndc(uv, ndc_scale);
    float4 clip = float4(ndc, depth, 1.0);
    float4 w = mul(inv_view_proj, clip);
    return w.xyz / w.w;                            /* 1 rcp */
}
/* Luma edge for FXAA/SMAA-style blending, no derivative instructions. */
float mu_luma_edge(float3 a, float3 b) { return abs(mu_luma709(a) - mu_luma709(b)); }
/* SSAO sample position from a golden-angle hemisphere (brdf_util.slang). */
float3 mu_ssao_offset(uint i, uint count, float angle, float radius)
{
    return mu_golden_hemisphere(i, count, angle) * radius;
}
/* Range-checked horizon occlusion accumulator for one SSAO sample. */
float mu_ssao_sample_weight(float3 sample_delta, float3 normal, float radius)
{
    float dist2 = dot(sample_delta, sample_delta);
    float ndv   = dot(normal, sample_delta * rsqrt(max(dist2, MU_EPS_SQ)));
    return mu_saturate(ndv) * mu_saturate(1.0 - dist2 / (radius * radius));
}

#endif /* MU_POST_SLANG */
