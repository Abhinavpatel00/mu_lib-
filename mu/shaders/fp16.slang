/* mu/fp16.slang — EXPLICIT 16-bit float, used in pairs.
 * ============================================================================
 *  Grounded in vulkan-shader-opt 11 (MJP's structural insight, re-verified for
 *  Slang 2026.21 / Mesa 27):
 *
 *  Two INDEPENDENT paths to fp16:
 *    A. RelaxedPrecision / `mediump` — a decoration with NO contract. On RADV
 *       it is a NO-OP: searching src/amd/compiler + src/amd/common for
 *       "relaxed" yields only unrelated hits. Never build a strategy on it.
 *    B. Explicit `half`/`float16_t` — a real type with real device gates.
 *       In Slang `half` IS true fp16; there is no min16float dance.
 *
 *  Three separate ledgers, do not conflate them:
 *    REGISTERS  2 x 16-bit values per 32-bit VGPR — the RELIABLE win.
 *    BANDWIDTH  half4 = 8 B vs float4 = 16 B — needs a 16-bit format.
 *    THROUGHPUT requires the hardware packed op v_pk_* AND a PAIR.
 *
 *  THE TRAP (11 §4): ACO emits v_pk_add_f16 even for a SINGLE 16-bit value —
 *  the upper half carries nothing. Scalar `half` buys registers, NOT speed.
 *  Write the math so two halves travel together (half2/half4). Published
 *  RDNA2/RADV measurement (Goldberg, 2022) had f16vec4 LOSE on every axis
 *  vs vec4: VGPRs 40 vs 32, instructions 105 vs 91, waves/SIMD 24 vs 32 —
 *  conversions cost more than the register saving. Measure before shipping.
 *
 *  Device gates on RADV (radv_physical_device.c): shaderFloat16 (:1084,
 *  radv_shader_fp16_enabled :216), storageBuffer16BitAccess /
 *  uniformAndStorageBuffer16BitAccess (:1063-1064), storagePushConstant16
 *  (:1065), storageInputOutput16 (:1066), shaderFmaFloat16 (:1679). RADV's
 *  own comment at :218: GFX8 fp16 "can sometimes hurt perf", not enabled.
 *
 *  WHERE IT PAYS: colour, UV, 16-bit indices and interpolation-heavy data.
 *  WHERE IT DOES NOT: depth, world positions, and any value whose error is
 *  amplified by a derivative (04 §3, 10 §3).
 * ==========================================================================*/
#ifndef MU_FP16_SLANG
#define MU_FP16_SLANG

#include "common.slang"
#include "packing.slang"

/* Paired math: one OpFMul %v2half + one OpFAdd %v2half in SPIR-V. SPIR-V has
 * NO packed op — whether the two halves share a v_pk_* instruction is ACO's
 * decision, from the 16-bit vector types. Do not claim double throughput. */
half2 mu_half2(float2 v)                 { return half2(v); }
half4 mu_half4(float4 v)                 { return half4(v); }
float2 mu_float2(half2 v)                { return float2(v); }
float4 mu_float4(half4 v)                { return float4(v); }

half2 mu_half2_mad(half2 a, half2 b, half2 c) { return a * b + c; }
half2 mu_half2_saturate(half2 v)              { return clamp(v, half2(0.0), half2(1.0)); }
half4 mu_half4_saturate(half4 v)              { return clamp(v, half4(0.0), half4(1.0)); }
half2 mu_half2_lerp(half2 a, half2 b, half2 t){ return lerp(a, b, t); }
half4 mu_half4_lerp(half4 a, half4 b, half4 t){ return lerp(a, b, t); }
/* Luma in the pair: one half2 dot + one scalar. Prefer collapsing scalars
 * before touching a vector (02 §2). */
half mu_half3_luma(half3 c)
{
    return c.x * half(0.2126) + c.y * half(0.7152) + c.z * half(0.0722);
}

/* Colour round trip: fp32 -> paired fp16 -> fp32. Two OpFConvert instructions
 * for a half2; the pair is what makes the register ledger pay. */
half2 mu_half2_encode_colour(float2 c)  { return half2(mu_saturate(c)); }
float2 mu_half2_decode_colour(half2 c)  { return float2(c); }

/* 16-bit storage for UVs and other [0,1] pairs. This is the BANDWIDTH ledger:
 * one uint instead of two floats, with a defined 1/65535 error. */
uint  mu_half2_pack_unorm(float2 uv)
{
    return mu_pack_u16x2(uint2(mu_round_bias(mu_saturate(uv), 65535.0)));
}
float2 mu_half2_unpack_unorm(uint p)
{
    return float2(mu_unpack_u16x2(p)) * (1.0 / 65535.0);
}

/* fp16 in a push constant requires `storagePushConstant16`; in a varying it
 * requires `storageInputOutput16` (both true on RADV 2026). The helper is a
 * PACKED PAIR so the 16-bit storage is real, not a scalar-half decoration. */
uint mu_half2_pack_bits(half2 v)         { return mu_pack_half2(float2(v)); }
half2 mu_half2_unpack_bits(uint p)       { return half2(mu_unpack_half2(p)); }

#endif /* MU_FP16_SLANG */
