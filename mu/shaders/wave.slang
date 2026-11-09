/* mu/wave.slang — subgroup primitives that replace LDS round trips.
 * ============================================================================
 *  Grounded in vulkan-shader-opt 05 §3 and its Sabourin/GPC 2024 pricing,
 *  re-verified against Mesa 27.0.0-devel (ac271714f) for THIS driver:
 *
 *  | Need                     | Use                          | ISA on RADV      |
 *  |--------------------------|------------------------------|------------------|
 *  | stream-compaction offset | WavePrefixCountBits(pred)    | 2 (mbcnt_amd)    |
 *  | generic prefix sum       | WavePrefixSum(x)             | ~40 (!)          |
 *  | any/all across wave      | WaveActiveAnyTrue/AllTrue    | 1                |
 *  | sum/max/min              | WaveActiveSum/Max/Min        | log2(n) tree     |
 *  | broadcast first lane     | WaveReadLaneFirst            | scalar read      |
 *  | read a lane's value      | WaveReadLaneAt(v, lane)      | ds_bpermute_b32  |
 *
 *  SPIR-V op counts CANNOT price subgroup code: both prefix forms are 1-2
 *  OpGroupNonUniform* instructions, and the 40-vs-2 gap lives entirely in
 *  the backend. RADV sets `.lower_ballot_bit_count_to_mbcnt_amd = 1`
 *  (radv_shader.c:790) -> nir_intrinsic_mbcnt_amd -> v_mbcnt_lo_u32_b32.
 *  There is no hardware scan for a general prefix sum.
 *
 *  `WaveReadLaneAt` with a per-lane index is ONE ds_bpermute_b32 on RADV, NOT
 *  a waterfall loop: RADV does not enable `.lower_shuffle`, it enables
 *  `.lower_relative_shuffle` / `.lower_shuffle_to_swizzle_amd`, and ACO's
 *  nir_intrinsic_shuffle case has three paths and no loop
 *  (aco_select_nir_intrinsics.cpp:4170). AMD's own offline compiler DOES emit
 *  a waterfall loop — if you ship on Windows too, re-measure there.
 *
 *  Rules that are easy to break:
 *    - WAVE SIZE VARIES: 32 on NV, 32/64 on AMD, 8/16/32 on Intel. Never
 *      hardcode 32 or 64; use mu_wave_size() below.
 *    - Subgroup ops do NOT synchronise across the workgroup. Cross-wave
 *      exchange still needs groupshared + a barrier.
 *    - A per-light scalarisation/peel loop needs MAXIMAL reconvergence and a
 *      `>=` guard, or helper lanes hang the fragment shader (02 §6).
 *    - LDS traffic and ds_bpermute share the LDS crossbar (09 §3).
 * ==========================================================================*/
#ifndef MU_WAVE_SLANG
#define MU_WAVE_SLANG

#include "common.slang"

/* Never hardcode a subgroup size. WaveGetLaneCount() folds to the built-in
 * SubgroupSize; it also makes the shader correct under wave32 and wave64. */
uint mu_wave_size()  { return WaveGetLaneCount(); }
uint mu_wave_lane()  { return WaveGetLaneIndex(); }

/* Wave-uniform branch: the caller should prove uniformity with
 * `subgroupUniformControlFlow` (VK_KHR_shader_subgroup_uniform_control_flow).
 * Without it the compiler must assume divergence and scalarises less. */
bool mu_wave_any(bool p) { return WaveActiveAnyTrue(p); }
bool mu_wave_all(bool p) { return WaveActiveAllTrue(p); }

float mu_wave_sum(float v) { return WaveActiveSum(v); }
float mu_wave_max(float v) { return WaveActiveMax(v); }
float mu_wave_min(float v) { return WaveActiveMin(v); }
uint  mu_wave_sum(uint  v) { return WaveActiveSum(v); }
uint  mu_wave_max(uint  v) { return WaveActiveMax(v); }

/* Ballot -> exclusive prefix bit count = the compaction slot for this lane.
 * Sabourin measured WavePrefixSum at ~40 instructions vs 2 for the bit-count
 * spelling on RDNA. Use this one for cull/emit/compaction lists. */
uint  mu_wave_compact_offset(bool pred) { return WavePrefixCountBits(pred); }
uint  mu_wave_compact_count(bool pred)  { return countbits(WaveActiveBallot(pred).x); }
bool  mu_wave_first(bool pred)          { return mu_wave_compact_offset(pred) == 0u && pred; }

/* Deliberately NOT provided as a convenience wrapper: WavePrefixSum(x).
 * One SPIR-V ExclusiveScan, ~40 ISA instructions on AMD, no hardware scan.
 * If you actually need a general prefix sum, pay for it explicitly and put
 * the measurement in the report. */
uint mu_wave_prefix_sum_expensive(uint v)
{
    return WavePrefixSum(v);   /* ~40 ISA ops; prefer mu_wave_compact_offset */
}

float mu_wave_broadcast_first(float v) { return WaveReadLaneFirst(v); }
uint  mu_wave_broadcast_first(uint  v) { return WaveReadLaneFirst(v); }
float mu_wave_shuffle(float v, uint lane) { return WaveReadLaneAt(v, lane); }
uint  mu_wave_shuffle(uint  v, uint lane) { return WaveReadLaneAt(v, lane); }
/* XOR shuffle with a CONSTANT delta maps to a ds_swizzle-class swizzle on
 * RADV (masked_swizzle_amd), cheaper than the generic crossbar path. */
float mu_wave_shuffle_xor(float v, uint mask) { return WaveReadLaneAt(v, mu_wave_lane() ^ mask); }

/* A uniform per-wave trip count, for per-light or per-tile loops. Take the
 * max across the wave and guard the body with `>=`, not `!=` — an `!=` loop
 * lets helper lanes in a fragment shader run past the last real iteration
 * and hang (02 §6). */
uint mu_wave_loop_trip_count(uint local_count) { return mu_wave_max(local_count); }

/* Lane-coherent sample budgets (vulkan-opt 06 §8): if N samples are linked
 * by a deterministic symmetry, the free parameter must be drawn from the
 * FUNDAMENTAL DOMAIN. Two-sided/antipodal pairs drawn over [0,2pi) generate
 * each orbit twice: N_eff = N/2, 1.41x the noise or 2x the rays. */
float mu_antipodal_phase(float u)  { return u * MU_PI; }      /* NOT 2*PI */
float mu_reflected_pair(float u)   { return 1.0 - u; }        /* derive, never re-draw */
/* Report this next to every tap budget: noise ~ 1/sqrt(N_eff). */
float mu_noise_vs_samples(uint n_eff) { return rsqrt(max(float(n_eff), 1.0)); }

#endif /* MU_WAVE_SLANG */
