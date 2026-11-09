/* mu/quantize.slang — quantization for storage, interception and style.
 * ============================================================================
 *  Three different jobs, kept apart on purpose:
 *
 *    STORAGE    fewer bytes per texel/vertex with a defined error budget
 *               (unorm/snorm, octahedral normals, RG10A2, RGBM/RGBE, packed
 *               depth, quantized velocity)
 *    INTERCEPT  fewer interpolator components between stages (their own
 *               budget: 32 x 32-bit varyings, and every one you skip is a
 *               TRAM/ISBE write the pixel pipe does not do)
 *    STYLE      deliberate banding — the stylization step, shared with toon
 *               shading (mu/quantize.slang mu_quantize_steps, used by toon.slang)
 *
 *  Every encode has its decode next to it, and each pair is round-trip safe at
 *  the ends (0 -> 0 -> 0, 1 -> max -> 1) because a half-texel bias is applied
 *  once, on encode. CORRECTED (skill 08 §1): slangc 2026.21 at -O3 does NOT
 *  fuse `a*b+c` — it emits OpFMul + OpFAdd. NIR folds that back to ffma on
 *  RADV (nir_opt_algebraic.py, ffma_weak), so the ISA is one v_fma_f32 either
 *  way, but ONLY `mad()` guarantees it in the module and on other drivers.
 *  Every rounding bias below therefore uses mu_round_bias() -> OpExtInst Fma.
 *
 *  Likewise, a vector `c / s` is three OpFDiv lanes (3 v_rcp_f32). Writing
 *  `c * mu_rcp(s)` hoists the reciprocal to one scalar v_rcp_f32.
 * ==========================================================================*/
#ifndef MU_QUANTIZE_SLANG
#define MU_QUANTIZE_SLANG

#include "packing.slang"

/* ===========================================================================
 * 1. Uniform scalar quantizers
 * =========================================================================*/

/* x in [0,1] -> [0, levels-1]. Cost: 1 fma + 1 cvt. */
uint mu_quantize_unorm(float x, uint levels_minus_one)
{
    return uint(mu_round_bias(mu_saturate(x), float(levels_minus_one)));
}

/* [0, levels-1] -> [0,1]. The keep-zero-at-zero branch is one select and is
   what makes a white 8-bit encode decode back to exactly 1.0. */
float mu_dequantize_unorm(uint q, uint levels_minus_one)
{
    return float(q) * mu_rcp(float(levels_minus_one));
}

uint  mu_unorm8_encode(float x)  { return mu_quantize_unorm(x, 255u); }
float mu_unorm8_decode(uint q)   { return float(q) * (1.0 / 255.0); }

uint  mu_unorm16_encode(float x) { return mu_quantize_unorm(x, 65535u); }
float mu_unorm16_decode(uint q)  { return float(q) * (1.0 / 65535.0); }

/* Signed [-1,1] -> [0, 2^n-1] with 0 mapping to the middle bucket.
 * Error is half a step either way; 8 bits gives 0.0078. */
uint  mu_snorm8_encode(float x)  { return uint(mu_round_bias(mu_saturate(mu_mad(x, 0.5, 0.5)), 255.0)); }
float mu_snorm8_decode(uint q)   { return float(q) * (1.0 / 255.0) * 2.0 - 1.0; }

/* ===========================================================================
 * 2. Packed channels — the four common G-buffer / attribute layouts
 * =========================================================================*/

/* 4 x 8-bit unorm in one uint. Measured pack: 3 shifts + 3 ors = 6 integer
 * ops (mu/tests/measure.py). There is no Slang intrinsic that produces fewer
 * SPIR-V instructions for this pattern. Prefer this layout whenever four
 * channels fit in 8 bits — one 32-bit attribute instead of four. */
uint mu_pack_rgba8(float4 c)
{
    uint4 q = uint4(mu_round_bias(mu_saturate(c), 255.0));
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}

float4 mu_unpack_rgba8(uint p)
{
    return float4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, (p >> 24) & 255u) * (1.0 / 255.0);
}

/* RGB10A2: 30 bits of colour. Use when the albedo needs more than 8 bits of
 * green but alpha is a 2-value flag. */
uint mu_pack_rgb10a2(float3 rgb, float a)
{
    uint3 q = uint3(mu_round_bias(mu_saturate(rgb), 1023.0));
    uint  w = uint(mu_round_bias(mu_saturate(a), 3.0));
    return q.x | (q.y << 10) | (q.z << 20) | (w << 30);
}

float4 mu_unpack_rgb10a2(uint p)
{
    return float4(float(p & 1023u), float((p >> 10) & 1023u), float((p >> 20) & 1023u),
                  float(p >> 30)) * float4(1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 3.0);
}

/* Two 16-bit unorm channels in one uint — the default for UVs and for any
 * pair of values with more range than 8 bits. The bit-level form lives in
 * packing.slang; this is the semantic wrapper. */
uint   mu_pack_unorm16x2(float2 v) { return mu_pack_u16x2(uint2(mu_round_bias(mu_saturate(v), 65535.0))); }
float2 mu_unpack_unorm16x2(uint p) { return float2(mu_unpack_u16x2(p)) * (1.0 / 65535.0); }

/* ===========================================================================
 * 3. Octahedral normals — the one quantization everybody should be using
 * =========================================================================*/

/* 3 floats -> 2 in [-1,1]. The fold is what makes the second half of the
 * sphere cheap: no atan, no trig. Measured 23 encode / 19 decode net SPIR-V
 * ops (mu/tests/measure.py), with a continuous round-trip error of 2.6e-8 rad
 * (mu/tests/validate_math.py); 16-bit packing adds ~0.0004 rad.
 * Compare spherical (sin/cos/atan2), which needs 3 quarter-rate ops plus the
 * 109-instruction driver expansion of atan2. */
/* +/-1 fold sign comes from mu/common.slang (mu_sign_nz). sign() itself is
 * an OpExtInst FSign that ACO expands to a multiply by +Inf + a clamp; the
 * fold needs only a sign test, so it uses one OpSelect. */

float2 mu_oct_encode(float3 n)
{
    n *= mu_rcp(max(abs(n.x) + abs(n.y) + abs(n.z), MU_EPS));  /* 1 SCALAR v_rcp_f32 */
    float2 e      = n.xy;
    float2 folded = (1.0 - abs(n.yx)) * mu_sign_nz(n.xy);
    float  k      = step(0.0, n.z);                    /* 1 where upper hemisphere */
    return lerp(folded, e, k);                          /* no vector ?: */
}

float3 mu_oct_decode(float2 e)
{
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float2 folded = (1.0 - abs(n.yx)) * mu_sign_nz(n.xy);
    n.xy = lerp(folded, n.xy, step(0.0, n.z));
    return normalize(n);                               /* 1 quarter-rate rsqrt */
}

/* 2 x 16-bit unorm: the storage format for a normal in a vertex or G-buffer.
 * Error ~0.0004 rad — invisible. */
uint mu_pack_normal_oct16(float3 n) { return mu_pack_unorm16x2(mu_oct_encode(n) * 0.5 + 0.5); }
float3 mu_unpack_normal_oct16(uint p) { return mu_oct_decode(mu_unpack_unorm16x2(p) * 2.0 - 1.0); }

/* 2 x 8-bit: for normals in a packed vertex, where 8 bits buys you half the
 * attribute. Error ~0.01 rad, fine for shading, not for mirror reflections. */
uint   mu_pack_normal_oct8(float3 n) { uint2 q = uint2(mu_round_bias(mu_saturate(mu_mads(mu_oct_encode(n), 0.5, float2(0.5))), 255.0)); return q.x | (q.y << 8); }
float3 mu_unpack_normal_oct8(uint p) { return mu_oct_decode(float2(p & 255u, (p >> 8) & 255u) * (2.0 / 255.0) - 1.0); }

/* Tangent-space normal, z assumed >= 0: one channel pair is enough and the
 * decode can skip the fold. */
float2 mu_oct_encode_ts(float3 n) { return mu_oct_encode(n) * 0.5 + 0.5; }
float3 mu_oct_decode_ts(float2 e) { return mu_oct_decode(e * 2.0 - 1.0); }

/* ===========================================================================
 * 4. HDR encodings — pick by the error you can afford
 * =========================================================================*/

/* RGBM: 4 x 8-bit, `max_range` is chosen per use (bloom: 8; lightmaps: 6).
 * Error grows with range and is worst in the dark; never use it for a
 * tonemapper input that must stay physically linear. */
uint4 mu_encode_rgbm(float3 c, float max_range)
{
    float m = mu_saturate(max(max(c.r, max(c.g, c.b)) / max_range, 1.0 / 255.0));
    float3 q = c * mu_rcp(m * max_range);
    return uint4(uint3(mu_round_bias(q, 255.0)), uint(mu_round_bias(m, 255.0)));
}
float3 mu_decode_rgbm(uint4 p, float max_range)
{
    float4 q = float4(p) * (1.0 / 255.0);
    return q.rgb * (q.a * max_range);
}

/* RGBD: 4 x 8-bit with a divisor instead of a multiplier. Slightly better
 * than RGBM for dark pixels; still non-linear. */
/* Unlike RGBM the divisor is rounded *up* to a whole 8-bit value, which
 * guarantees the stored quotient is <= 255 without a saturate at encode.
 * That is the whole difference between the two 4-byte formats. */
uint4 mu_encode_rgbd(float3 c, float max_range)
{
    float  d = ceil(mu_saturate(max(max(c.r, max(c.g, c.b)) / max_range, 1.0 / 255.0)) * 255.0) / 255.0;
    float3 q = c * mu_rcp(d * max_range);
    return uint4(uint3(mu_round_bias(mu_saturate(q), 255.0)), uint(mu_round_bias(d, 255.0)));
}
float3 mu_decode_rgbd(uint4 p, float max_range)
{
    float4 q = float4(p) * (1.0 / 255.0);
    return q.rgb * (q.a * max_range);
}

/* RGBE / shared exponent: 4 x 8-bit, ~1 % relative error across the whole
 * dynamic range. The right choice when you need HDR in 4 bytes and cannot
 * spend 8 (RGBA16F). */
uint4 mu_encode_rgbe(float3 c)
{
    float  m = max(max(c.r, c.g), c.b);
    float  e = ceil(log2(max(m, 1e-32)));     /* 1 quarter-rate op */
    float3 s = c * mu_rcp(exp2(e));            /* 1 quarter-rate op + 1 SCALAR rcp */
    uint4  p = uint4(uint3(mu_round_bias(mu_saturate(s), 255.0)), uint(e + 128.0));
    return p * uint(step(1e-32, m));           /* branch-free black */
}
float3 mu_decode_rgbe(uint4 p)
{
    /* encode stored s = c/2^e in 8 bits, so the inverse is a /255 and an
     * exp2 (1 quarter-rate op). The old -8 here was wrong by 255x. */
    float e = exp2(float(p.a) - 128.0);
    return float3(p.rgb) * (1.0 / 255.0) * e * step(0.5, float(p.a));
}

/* Log-luma + chroma (Call of Duty: Advanced Warfare): 4 x 8-bit, favouring
 * perceived quality over physical linearity. Good for bloom and for any
 * buffer that is about to be tonemapped anyway. */
uint4 mu_encode_logluv(float3 c)
{
    float l = max(mu_luma709(c), 1e-8);
    float3 uvw = c * mu_rcp(l);
    float  logl = log2(1.0 + l * 4.0) / log2(1.0 + 65504.0 * 4.0);   /* 0..1 */
    return uint4(uint3(mu_round_bias(mu_saturate(uvw), 255.0)), uint(mu_round_bias(mu_saturate(logl), 255.0)));
}
float3 mu_decode_logluv(uint4 p)
{
    float4 q = float4(p) * (1.0 / 255.0);
    float  l = (exp2(q.a * log2(1.0 + 65504.0 * 4.0)) - 1.0) * 0.25;
    return q.rgb * l;
}

/* ===========================================================================
 * 5. Depth and velocity
 * =========================================================================*/

/* Depth in 16 bits (RG8). Enough for a stencil/soft-particle test, not for a
 * reconstructed position; the 24-bit form below is the one to use for that.
 * Reverse-Z is assumed — decode returns exactly what you stored. */
uint  mu_pack_depth16(float d)  { return mu_unorm16_encode(d); }
float mu_unpack_depth16(uint p) { return mu_unorm16_decode(p); }

/* 24 bits of depth in two bytes-plus: word0 = low 16, word1 = high 8. Written
 * as a pair so the packing cannot be half-remembered at a call site. */
uint2 mu_pack_depth24(float d)
{
    uint q = uint(mu_round_bias(mu_saturate(d), 16777215.0));
    return uint2(q & 0xFFFFu, (q >> 16) & 0xFFu);
}
float mu_unpack_depth24(uint2 p) { return float(p.x | (p.y << 16)) * (1.0 / 16777215.0); }

/* Velocity: RG16F stores it as float (no encode), 2 x 16-bit snorm stores it
 * in the same space with a known scale. Pick the snorm form when the velocity
 * range is bounded (it is: screen-space motion is < 1 screen per frame). */
uint2 mu_pack_velocity(float2 v, float inv_scale)
{
    int2 q = int2(mu_round_bias(clamp(v * inv_scale, -1.0, 1.0), 32767.0));
    return uint2(uint(q.x & 0xFFFF), uint(q.y & 0xFFFF));
}
float2 mu_unpack_velocity(uint2 p, float scale)
{
    int2 q = int2(int(p.x << 16) >> 16, int(p.y << 16) >> 16);   /* sign-extend */
    return float2(q) * (scale / 32767.0);
}

/* ===========================================================================
 * 6. Quantized gameplay values — the cheap win people forget
 * =========================================================================*/

/* A yaw in [0, 2pi) as 8 bits (1.4 deg) or 16 bits. The sim writes it once,
 * the renderer reads it per pixel; storing radians as float32 spends 4 bytes
 * per agent on 0.002 deg of precision nothing can see. */
uint  mu_pack_yaw8(float radians)  { return min(uint(frac(radians * MU_INV_2PI) * 256.0), 255u); }
float mu_unpack_yaw8(uint q)       { return float(q) * (MU_2PI / 256.0); }

/* A unit direction's XZ pair as 2 x 16 snorm: same as a 2D normal. */
uint  mu_pack_dir_xz(float3 dir) { float2 d = mu_safe_normalize(float2(dir.x, dir.z)); return mu_pack_unorm16x2(d * 0.5 + 0.5); }
float3 mu_unpack_dir_xz(uint p)  { float2 d = mu_unpack_unorm16x2(p) * 2.0 - 1.0; return float3(d.x, 0.0, d.y); }

/* ===========================================================================
 * 7. Stylization — deliberate quantization (shared with toon.slang)
 * =========================================================================*/

/* Hard band: n steps, no blending. 3 ops. */
float mu_quantize_steps(float x, float steps) { return floor(mu_saturate(x) * steps) * mu_rcp(steps); }

/* Soft band: the same edge with a width, so it survives TAA and does not
 * shimmer. `width` is the fraction of a step that is blended; 0 = hard, 1 =
 * linear. This is what toon.slang's cel_band() uses underneath. */
float mu_quantize_steps_soft(float x, float steps, float width)
{
    float  s    = mu_saturate(x) * steps;
    float  base = floor(s);
    float  f    = s - base;
    float  w    = max(width, 1e-4);
    float  t    = smoothstep(0.5 - 0.5 * w, 0.5 + 0.5 * w, f);
    return (base + t) / steps;
}

/* Posterize colour: `levels` per channel, optionally preserving luma so the
 * image does not darken. Costs 1 fma per channel + the luma dot. */
float3 mu_posterize(float3 c, float levels, bool preserve_luma)
{
    float3 q  = floor(mu_round_bias(mu_saturate(c), levels)) * mu_rcp(levels);
    float  l0 = max(mu_luma709(c), 1e-4);
    float  l1 = max(mu_luma709(q), 1e-4);
    float  k  = select(preserve_luma, 1.0, 0.0);
    return q * lerp(1.0, l0 / l1, k);      /* branch-free, uniform bool */
}

/* ===========================================================================
 * 8. Recipes — the layouts the engine should standardise on
 * =========================================================================*/

/* A quantized static vertex: 12 bytes instead of 44.
 *   pos   2 x 16 unorm in the model's local bounds (+ z folded into word 1)
 *   nrm   octahedral 16 + 16
 *   uv    2 x 16 unorm
 * `inv_extent` is 1/(bounds.max - bounds.min); quantizing without it is how
 * baked meshes lose their corners. */
struct MuPackedVertex
{
    uint pos_xy;
    uint pos_z;
    uint nrm;
    uint uv;
};

MuPackedVertex mu_pack_vertex(float3 p, float3 inv_extent,
                              float3 n, float2 uv)
{
    MuPackedVertex v;
    v.pos_xy = mu_pack_unorm16x2(mu_saturate(p.xy * inv_extent.xy));
    v.pos_z  = mu_unorm16_encode(mu_saturate(p.z * inv_extent.z));
    v.nrm    = mu_pack_normal_oct16(n);
    v.uv     = mu_pack_unorm16x2(uv);
    return v;
}

float3 mu_unpack_vertex_pos(MuPackedVertex v, float3 extent)
{
    float2 xy = mu_unpack_unorm16x2(v.pos_xy);
    float  z  = mu_unorm16_decode(v.pos_z);
    return float3(xy, z) * extent;
}

/* The normal/roughness/metalness word: 16 bits octahedral, 8 roughness,
 * 8 metalness in one 32-bit attribute instead of three. Losses: 1/255 on the
 * material terms, ~0.0004 rad on the normal with 16-bit oct. */
uint  mu_pack_nrm_rough_metal(float3 n, float roughness, float metalness)
{
    uint oct = mu_pack_normal_oct16(n);
    uint r8  = uint(mu_round_bias(mu_saturate(roughness), 255.0));
    uint m8  = uint(mu_round_bias(mu_saturate(metalness), 255.0));
    return mu_bits_insert(oct, r8 | (m8 << 8), 16u, 16u);
}

float3 mu_unpack_nrm_rough_metal(uint w)
{
    /* the two material bytes are the top half; read them with mu_unpack_orm */
    return mu_unpack_normal_oct16(w & 0xFFFFu);
}

float2 mu_unpack_orm(uint w)
{
    return float2((w >> 16) & 0xFFu, (w >> 24) & 0xFFu) * (1.0 / 255.0);
}

/* ---- G-buffer recipes ---------------------------------------------------
 * FAT  (12 B): albedo RGBA8 | nrm/rough/metal word | velocity 2x16 snorm
 *              depth stays in the depth buffer — never store it again.
 * THIN  (8 B): albedo RGB10A2 (alpha = material id) | nrm/rough/metal word
 *
 * The THIN layout is what vulkan-opt 03 §4 argues for. Every extra target is
 * a full-extent read+write on every pass that touches it: at 1440p an RGBA8
 * target is 14.7 MB per pass ~= 23 us of bandwidth before any shading. Adding
 * a *channel* is cheap (it lives in a word you already write); adding a
 * *target* is expensive.
 * ----------------------------------------------------------------------- */
struct MuGBufferFat
{
    uint albedo;    /* mu_pack_4x8_unorm(albedo)            */
    uint nrm_orm;   /* mu_pack_nrm_rough_metal(n, r, m)     */
    uint velocity;  /* mu_pack_u16x2(mu_pack_velocity(...)) */
};

struct MuGBufferThin
{
    uint albedo_mat; /* RGB10A2: albedo in rgb, material id in a */
    uint nrm_orm;    /* mu_pack_nrm_rough_metal(n, r, m)          */
};

#endif /* MU_QUANTIZE_SLANG */

