/* mu/packing.slang — bit-level packing for attributes, G-buffers and arguments.
 * ============================================================================
 *  Where quantize.slang cares about *error*, this file cares about *bits* and
 *  the emitted instruction count. Every number below is measured by
 *  mu/tests/measure.py (slangc 2026.21, -O3, SPIR-V disassembly), not asserted.
 *
 *    4 x 8-bit pack (mu_pack_4x8)   3 OpShiftLeftLogical + 3 OpBitwiseOr
 *                                   = 6 integer ops for the pack itself
 *                                   (11 net ops in measure.py including the
 *                                    buffer read; no Slang intrinsic emits
 *                                    fewer, and the SPIR-V path has no
 *                                    byte-permute opcode)
 *    2 x 16-bit pack / unpack       1 shift + 1 or / 1 mask + 1 shift
 *    half2 round trip               15 net ops: 1 vector OpFConvert + OpBitcast
 *                                   + OpUConvert each way, plus shifts.
 *                                   `pack_f16x2`/`unpack_f16x2` do NOT exist in
 *                                   Slang 2026.21; f32tof16/f16tof32 do.
 *    RGBA8 pack/unpack round trip   measured in measure.py
 *
 *  Rule: prefer byte-aligned fields inside a 32-bit word. A 10-bit field costs
 *  extra shifts and masks that no permute unit can hide; a byte field is one
 *  shift and one or.
 * ==========================================================================*/
#ifndef MU_PACKING_SLANG
#define MU_PACKING_SLANG

#include "common.slang"

/* --------------------------------------------------------------- bitfields */
uint mu_bits_insert(uint base, uint value, uint offset, uint count)
{
    uint mask = ((1u << count) - 1u) << offset;
    return (base & ~mask) | ((value << offset) & mask);
}

uint mu_bits_extract(uint word, uint offset, uint count)
{
    return (word >> offset) & ((1u << count) - 1u);
}

/* Sign-extend the low `count` bits starting at `offset`. The trick every
 * packed-snorm decoder needs and most get wrong exactly once. */
int mu_bits_extract_signed(uint word, uint offset, uint count)
{
    uint v    = (word >> offset) & ((1u << count) - 1u);
    uint sign = 1u << (count - 1u);
    return int((v ^ sign) - sign);
}

/* ------------------------------------------------------------- 4 x 8-bit */
uint  mu_pack_4x8(uint4 v)  { return v.x | (v.y << 8) | (v.z << 16) | (v.w << 24); }
uint4 mu_unpack_4x8(uint p) { return uint4(p & 0xFFu, (p >> 8) & 0xFFu, (p >> 16) & 0xFFu, p >> 24); }

uint   mu_pack_4x8_unorm(float4 c) { return mu_pack_4x8(uint4(mu_saturate(c) * 255.0 + 0.5)); }
float4 mu_unpack_4x8_unorm(uint p) { return float4(mu_unpack_4x8(p)) * (1.0 / 255.0); }

/* Two 8-bit channels plus two flags — the cheap material word. */
uint mu_pack_2x8_2flags(float2 c, bool a, bool b)
{
    return mu_pack_4x8(uint4(uint2(mu_saturate(c) * 255.0 + 0.5), a ? 1u : 0u, b ? 1u : 0u));
}

/* ------------------------------------------------------------- 2 x 16-bit */
uint  mu_pack_u16x2(uint2 v)  { return v.x | (v.y << 16); }
uint2 mu_unpack_u16x2(uint p) { return uint2(p & 0xFFFFu, p >> 16); }

/* Half pair. Slang 2026.21 has no pack_f16x2/unpack_f16x2; f32tof16 and
 * f16tof32 are the intrinsics, and on a float2 they emit a *single* vector
 * conversion (checked in SPIR-V), not two scalar ones. Use for signed values
 * where unorm16's fixed range would clip: velocity, UV derivatives. */
uint   mu_pack_half2(float2 v) { uint2 h = f32tof16(v); return h.x | (h.y << 16); }
float2 mu_unpack_half2(uint p) { return f16tof32(uint2(p & 0xFFFFu, p >> 16)); }

/* -------------------------------------------------------------- snorm pack */
uint  mu_pack_snorm16(float x) { return uint(int(clamp(x, -1.0, 1.0) * 32767.0 + 0.5)) & 0xFFFFu; }
float mu_unpack_snorm16(uint q){ return float(mu_bits_extract_signed(q, 0u, 16u)) * (1.0 / 32767.0); }

/* ------------------------------------------------------------- multi-word */
/* Split a float across two 16-bit words and back — for packed structs whose
 * scale is only known at runtime (a model-space bound, a particle rate). */
uint2 mu_pack_float_split(float v) { uint u = asuint(v); return uint2(u & 0xFFFFu, u >> 16); }
float mu_unpack_float_split(uint2 w) { return asfloat(w.x | (w.y << 16)); }

/* ------------------------------------------------------------ byte helpers */
uint mu_byte_swap(uint v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

/* One 4-byte load from a packed attribute stream. ByteAddressBuffer.Load emits
 * `OpLoad ... Aligned 4`: 4 is the hardware floor
 * (minStorageBufferOffsetAlignment, radv_physical_device.c:1902) and the fast
 * path — an unaligned 4-byte load may straddle a cache line.
 *
 * Deliberately NOT a `uint` device address: that form silently adds the
 * SPV_KHR_physical_storage_buffer_addresses capability (OpConvertUToPtr) to
 * any module that touches it, which is a much bigger requirement than "read
 * four bytes". Pass a ByteAddressBuffer instead. */
uint mu_load_u32_le(ByteAddressBuffer buf, uint byte_offset)
{
    return buf.Load(byte_offset & ~3u);
}

#endif /* MU_PACKING_SLANG */
