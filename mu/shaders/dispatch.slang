/* mu/dispatch.slang — launch shape, L2 locality and pass bandwidth.
 * ============================================================================
 *  The shader skill says clearly that for an effect pass the levers are, in
 *  order of size: (1) pass count / intermediates, (2) TOPOLOGY, (3) shader
 *  body (15 §1). Almost all optimisation effort goes into (3) and that is
 *  where it is least often the answer. This module holds the arithmetic for
 *  (1) and (2) so that the decision is made with a number, not a feeling.
 *
 *  Grounded in:
 *    vulkan-opt 09 §3A.2  workgroup size -> occupancy on GFX12/RDNA4
 *    vulkan-opt 09 §4     DRAM/L2 move 32-B SECTORS; traffic = sectors * 32
 *    vulkan-opt 09 §4.4   bytes_in_flight = bandwidth * latency (Little's law)
 *    vulkan-opt 03 §6     1440p RGBA16F read+write = 91 us @ 644.6 GB/s
 *    vulkan-shader-opt 13 §2  Bavoil thread-group ID tiling (L2 locality)
 *    vulkan-shader-opt 15 §2  one fullscreen triangle beats two (95% vs 87%
 *                              L1/L2 hit on the measured GCN post chain)
 *
 *  Everything here is integer/scalar work on UNIFORM values. On a GPU with a
 *  scalar ALU it is executed once per wave, not once per lane. That is the
 *  whole reason a dozen integer ops is affordable against a nine-tap gather.
 *
 *  Hard limits from this header are mirrored in src/vk_v2.h §16 so the CPU
 *  frame program and the shader agree on the same numbers.
 * ==========================================================================*/
#ifndef MU_DISPATCH_SLANG
#define MU_DISPATCH_SLANG

#include "common.slang"

/* ------------------------------------------------------------- hw budgets */
/* GFX12/RDNA4, wave64 (vulkan-opt 09 §3A.2). Register file 768 wave64 VGPRs
 * per SIMD x 2 SIMDs = 98,304 / 1,024 threads = 96 VGPRs per thread at full
 * occupancy. NOT 48: the 48 figure belongs to the GCN wave64/10-wave table. */
#define MU_VGPR_FULL_OCC       96
#define MU_WAVES_PER_CU        32
#define MU_LDS_BYTES_PER_CU    65536
#define MU_ICACHE_BYTES_WGP    32768
#define MU_SECTOR_BYTES        32
#define MU_L2_LINE_BYTES       128
#define MU_VISIBLE_VRAM_GBS    644.6
#define MU_VRAM_LATENCY_NS     600.0

/* Workgroup presets. 1024 has ZERO slack: 49 VGPRs or 33 KiB of LDS drops it
 * to one group per CU = 8 waves/SIMD = 50% occupancy. 128/256 leave slack. */
#define MU_WG_64               64
#define MU_WG_128              128
#define MU_WG_256              256
#define MU_WG_1024             1024
/* Separable filters: 8-wide groups keep the wave's rows contiguous; 8x32 was
 * a measured local maximum (15 §3). Narrow 4x16 / 1x64 groups improve the
 * occupancy number and destroy the cache hit rate — do not do it. */
#define MU_FILTER_TILE_X       8
#define MU_FILTER_TILE_Y       32

/* ------------------------------------------------------- sector accounting */
/* DRAM/L2 move 32-byte sectors; every distinct sector is paid in full,
 * independent of how many bytes the shader uses. An AoS row where the hot
 * loop reads one field pays 3-4x the sectors; a stride that is a power of two
 * puts every lane on the same sector / LDS bank. */
uint  mu_sector_count(uint bytes)          { return (bytes + MU_SECTOR_BYTES - 1u) >> 5u; }
uint  mu_sector_bytes(uint sectors)        { return sectors << 5u; }
float mu_sector_efficiency(uint useful_bytes, uint sectors)
{
    return float(useful_bytes) * mu_rcp(float(mu_sector_bytes(sectors)));
}
/* bytes_in_flight = bandwidth x latency; per-CU = that / CU count.
 * RX 9070 XT: 644.6e9 * 600e-9 ~= 387 KB, ~6 KB/CU, ~96 lines in flight,
 * i.e. ~1.5 independent loads per resident wave at full occupancy. */
float mu_bytes_in_flight(float gb_per_s, float latency_ns, uint cu_count)
{
    float total = gb_per_s * 1e9 * latency_ns * 1e-9;
    return total * mu_rcp(float(cu_count));
}
/* Full-screen read + write cost in microseconds. 1440p RGBA16F is 91 us
 * (vulkan-opt 03 §6); use this to price an extra intermediate target. */
float mu_pass_bandwidth_us(uint width, uint height, uint bytes_per_pixel, uint read_write)
{
    float bytes = float(width * height * bytes_per_pixel * read_write);
    return bytes * mu_rcp(MU_VISIBLE_VRAM_GBS * 1e3);   /* GB/s -> bytes/us */
}
/* Neighbourhood-filter halo economics (09 §3A.3). NOTE: the skill's PROSE
 * defines overhead as (fetched - interior)/fetched, but its TABLE reports
 * border/interior (8x8: 36 border / 64 interior = 56%; 16x16: 68/256 = 27%;
 * 32x32: 132/1024 = 13%). Both are shipped, explicitly named, so nobody
 * has to guess which one a budget line used. Pick the smallest tile whose
 * overhead you can afford AT AN OCCUPANCY YOUR VGPR COUNT ALLOWS — 32x32
 * costs 1024 invocations and all the occupancy slack (09 §3A.2). */
float mu_halo_overhead(uint2 tile, uint border)      /* border / FETCHED */
{
    uint2 fetched = tile + 2u * border;
    return 1.0 - float(tile.x * tile.y) * mu_rcp(float(fetched.x * fetched.y));
}
float mu_halo_border_ratio(uint2 tile, uint border)  /* border / INTERIOR */
{
    uint2 fetched = tile + 2u * border;
    return float(fetched.x * fetched.y - tile.x * tile.y)
         * mu_rcp(float(tile.x * tile.y));
}

/* --------------------------------------------------------- launch geometry */
uint2 mu_dispatch_pixel(uint2 group_id, uint2 thread_id, uint2 tile)
{
    return group_id * tile + thread_id;
}
/* One fullscreen triangle, no vertex buffer, no vertex fetch (15 §2):
 *   vkCmdDraw(cmd, 3, 1, 0, 0); OpEntryPoint declares only VertexIndex,
 *   which RADV reads from the draw packet.
 * uv = (0,0),(2,0),(0,2); clip y = 1 - 2*uv.y matches the engine's
 * NEGATIVE-height viewport (README.md), so uv.y = 0 is the TOP row.
 * Two triangles are traversed left-then-right and hand the cache a cold
 * working set at the seam; measured 87% vs 95% L1/L2 hit, 108% relative
 * performance on a cache-bound post chain. */
float2 mu_fullscreen_triangle_uv(uint vertex_id)
{
    return float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
}
float4 mu_fullscreen_triangle_pos(uint vertex_id)
{
    float2 uv = mu_fullscreen_triangle_uv(vertex_id);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
/* The negated-Y viewport convention from the repo README.md, in one place.
 *   fb_y = height*(1-ndc.y)/2 ; ndc = 2*fb_pixel/viewport - 1
 * MU_NDC_TO_UV is (0.5,-0.5); positive-height viewports use (0.5,0.5). */
float2 mu_ndc_to_uv(float2 ndc) { return mu_mad(ndc, MU_NDC_TO_UV, float2(0.5)); }
float2 mu_uv_to_ndc_flipped(float2 uv) { return float2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y); }

/* ------------------------------------------------- Bavoil group-ID tiling */
/* Horizontal thread-group ID swizzling (13 §2). Applies ONLY when all three
 * hold: VRAM/L2 is the top-throughput unit, L2 load hit rate is well below
 * ~80%, and adjacent groups have overlapping footprints (wide-kernel gather).
 * Published result on a DXR denoiser at 1440p, N=16: pass time -47%, L2 read
 * hit 63% -> 86%. Morton/Z-order LOSES in practice (fewer index ops wins).
 * Do NOT apply to a fragment shader, a one-texel-per-pixel pass, or N > ~32. */
uint2 mu_tiled_group_id(uint2 gid, uint2 grid, uint n)
{
    uint tile_id  = gid.x / n;                        /* which tile column   */
    uint within_x = gid.x - tile_id * n;              /* inside the tile     */
    uint linear   = (tile_id * grid.y + gid.y) * n + within_x;
    uint y        = linear / grid.x;
    uint x        = linear - y * grid.x;
    /* The clamps are NOT optional: dropping them is how the original GDC
     * snippet broke for grids whose x dimension is not a multiple of N. */
    return uint2(min(x, grid.x - 1u), min(y, grid.y - 1u));
}
/* Cheaper variant for N = 2^n_shift AND grid.x a multiple of N (the common
 * full-screen case: 1920/8 = 240 groups, 240 % 16 == 0). The skill's
 * measured remap costs 12 integer ops including TWO OpUDiv; this one has
 * ONE OpUDiv and no divide by N, because
 *   linear = A*N + within_x  =>  linear/(g*N) = A/g  (g = grid.x >> shift).
 * All inputs are uniform, so the remaining division is scalar work. */
uint2 mu_tiled_group_id_pow2(uint2 gid, uint2 grid, uint n_shift)
{
    uint n        = 1u << n_shift;
    uint tile_id  = gid.x >> n_shift;
    uint within_x = gid.x & (n - 1u);
    uint a        = tile_id * grid.y + gid.y;
    uint g        = grid.x >> n_shift;                /* requires grid.x to be a multiple of n */
    uint y        = a / g;
    uint x        = ((a - y * g) << n_shift) + within_x;
    return uint2(min(x, grid.x - 1u), min(y, grid.y - 1u));
}

#endif /* MU_DISPATCH_SLANG */
