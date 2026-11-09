/* mu/format.slang — the format IS part of the shader's cost model.
 * ============================================================================
 *  Grounded in vulkan-shader-opt 04 §3 and vulkan-opt 03 §4/§6:
 *
 *    | Instead of                    | Use                    | Win           |
 *    |-------------------------------|------------------------|---------------|
 *    | RGBA8 normal + `*2-1` ALU     | RGBA8_SNORM / BC5      | 2 ALU + a bug |
 *    | `pow(color,2.2)`              | FMT_*_SRGB image view  | free decode   |
 *    | RGBA16F for a [0,1] value     | RGBA8_UNORM / R10G10B10A2 | half B/W  |
 *    | RGBA32F position buffer       | RGBA16_SFLOAT + recon  | half B/W      |
 *    | unorm stored as uint + /255   | FMT_*_UNORM            | fetch does it |
 *
 *  An `_SRGB` view decodes INSIDE THE SAMPLER for ZERO shader ALU. The exact
 *  transfer below exists only for procedural colours and for the final
 *  swapchain write, where there is no sampler to do it. `pow(c,2.2)` is NOT
 *  sRGB and costs two quarter-rate ops (log2 + exp2); do not use it.
 *
 *  Byte sizes here match the `Format` enum in src/vk_v2.h §3, so a shader
 *  budget and the CPU frame program price the same attachment identically.
 * ==========================================================================*/
#ifndef MU_FORMAT_SLANG
#define MU_FORMAT_SLANG

#include "common.slang"

/* Bytes per pixel of FMT_* in src/vk_v2.h. Keep the two lists in sync. */
#define MU_BPP_R8_UNORM        1
#define MU_BPP_RG8_UNORM       2
#define MU_BPP_RGBA8_UNORM     4
#define MU_BPP_RGBA8_SRGB      4
#define MU_BPP_RGBA8_SNORM     4
#define MU_BPP_RGB10A2_UNORM   4
#define MU_BPP_R11G11B10F      4
#define MU_BPP_RG16F           4
#define MU_BPP_RGBA16F         8
#define MU_BPP_RGBA32F         16
#define MU_BPP_D16_UNORM       2
#define MU_BPP_D24S8           4
#define MU_BPP_D32F            4

/* Full target size in bytes. At 1440p an RGBA8 target is ~14.7 MB per pass
 * (~23 us of bandwidth before shading), RGBA16F is ~29.5 MB. Adding a
 * CHANNEL to a word you already write is cheap; adding a TARGET is a whole
 * extra read+write (quantize.slang §8, vulkan-opt 03 §4). */
uint mu_target_bytes(uint width, uint height, uint bytes_per_pixel)
{
    return width * height * bytes_per_pixel;
}
/* Read + write of one full-screen pass, in bytes. */
uint mu_pass_bytes(uint width, uint height, uint bytes_per_pixel)
{
    return 2u * mu_target_bytes(width, height, bytes_per_pixel);
}

/* EXACT sRGB encode, branch-free (step + lerp, no bool3 conversion).
 * 1 vector Log2 + 1 vector Exp2 = two quarter-rate ops. If the destination
 * is an FMT_RGBA8_SRGB view, delete this call entirely. */
float3 mu_linear_to_srgb(float3 c)
{
    float3 v = mu_saturate(c);
    float3 lo = v * 12.92;
    float3 hi = 1.055 * exp2(log2(max(v, float3(MU_EPS_SQ))) * (1.0 / 2.4)) - 0.055;
    return lerp(lo, hi, step(float3(0.0031308), v));
}
/* EXACT sRGB decode (mu_srgb_to_linear in common.slang) is the inverse. An
 * FMT_RGBA8_SRGB view does this for free; mu_srgb_to_linear_approx is the
 * 4-op polynomial for the rare texture that cannot be viewed as sRGB. */
float3 mu_srgb_view_equivalent(float3 c) { return mu_srgb_to_linear(c); }

/* Signed normal: RGBA8_SNORM decodes `*2-1` in the FETCH. The helper below is
 * only for a procedural/CPU write into such a view; do not put it in a read
 * path — that is the "stop doing normal*2-1" bug (04 §3). */
float3 mu_normal_for_snorm_view(float3 n) { return mu_safe_normalize(n); }
float3 mu_normal_from_unorm8(float3 n)
{
    return mu_safe_normalize(n * 2.0 - 1.0);   /* prefer FMT_RGBA8_SNORM */
}

/* R11G11B10F has no sign bit and a shared 5-bit exponent per channel. It is
 * the right HDR intermediate when the value is non-negative and 16F's 8 B
 * per pixel is too expensive. Never store a signed normal or a world position
 * in it. */
float mu_r11g11b10_max()  { return 65024.0; }  /* largest finite half-like value */
float mu_r11g11b10_min()  { return 6.10352e-5; } /* 2^-14, smallest normal   */

/* A 2.2 gamma is a display approximation, not a transfer function of record.
 * Kept separate so nobody "simplifies" mu_srgb_to_linear into it. */
float3 mu_gamma22_to_linear(float3 c)
{
    return exp2(log2(max(c, float3(MU_EPS_SQ))) * 2.2);
}
float3 mu_linear_to_gamma22(float3 c)
{
    return exp2(log2(max(c, float3(MU_EPS_SQ))) * (1.0 / 2.2));
}

/* Depth: reverse-Z with FMT_D32F. A 24-bit packed depth (quantize.slang §5)
 * is for a stencil/soft-particle test, NOT for position reconstruction —
 * reconstructing from a quantized depth amplifies the error by dz/dx. */
float mu_reverse_z_ndc(float d) { return 1.0 - d; }

#endif /* MU_FORMAT_SLANG */
