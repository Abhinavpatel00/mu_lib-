# `mu/` — an optimized Slang shader library

A header-only Slang library for writing shaders where the instruction count is
part of the design. Include root is `shaders/`, so a shader says

```slang
#include "mu/lighting.slang"
#include "mu/quantize.slang"
#include "mu/dispatch.slang"
```

Everything is a leaf function over values. The library binds no descriptors,
owns no passes, and never loops over your lights for you — it supplies the
per-light, per-tap and per-pixel math so the caller's loop can be unrolled,
clustered, tiled, or scheduled without being rewritten.

It coexists with the engine's existing shaders. Those include
`"common.slang"` (the root-level file with the binding-0..4 block); this
library lives under `mu/` and its own `mu/common.slang` is a different file.

**This revision applies `/home/user/skills/vulkan-opt` and
`/home/user/skills/vulkan-shader-opt` to the library.** Four new modules cover
launch shape, subgroups, explicit fp16 and format choice; the existing modules
were re-measured and edited where a skill finding predicted a real change. The
same two skills drive the new `src/vk_v2.h` (§16–§19) and the audit in
`docs/vk-v2-skills-audit.md`.

---

## 1. Module map

| Module | What it gives you | Depends on |
|---|---|---|
| `common.slang` | constants, saturate/remap, `mu_mad`/`mu_mads`/`mu_round_bias`, `mu_rcp`, sign tests/bit ops, safe division, multiply-form powers, fast `atan2`/`sincos`, sRGB, PCG hashes, IGN, 4×4 Bayer, Hammersley | — |
| `packing.slang` | bitfields, 4×8 / 2×16 / half2 words, snorm16, byte swap, aligned `ByteAddressBuffer` load | `common` |
| `quantize.slang` | unorm/snorm quantizers, RGBA8/RGB10A2, octahedral normals, RGBM/RGBD/RGBE/LogLuv, depth/velocity/yaw packing, cel bands, packed vertex + G-buffer recipes | `packing` |
| `brdf_util.slang` | perceptual↔alpha roughness, Duff branchless ONB, TBN (+ `mu_tbn_unit`), cosine/sphere/GGX/VNDF sampling, PDFs, golden-angle hemisphere, mip-from-solid-angle | `common` |
| `lighting.slang` | Burley + Frostbite diffuse, GGX/aniso/Charlie NDF, Smith visibility, Schlick Fresnel, split-sum DFG, multi-scatter compensation, direct/indirect PBR, clearcoat, sheen, translucency | `common`, `brdf_util` |
| `toon.slang` | half/wrap Lambert, soft cel bands, ramps, rims, banded Blinn, hair highlight, cross-hatch, outlines, one-call `mu_toon_shade` | `common`, `quantize` |
| `shadows.slang` | 3×3 PCF, bilinear PCF (4 taps), 16-tap Gather box PCF, rotated Poisson PCF, PCSS, VSM/ESM, cascade select/blend/fade, bias helpers | `common`, `brdf_util` |
| `noise.slang` | integer-hash value/gradient noise, simplex2D, Worley F1/F2, curl, fbm/ridged/turbulence, domain warp, marble | `common` |
| `sdf.slang` | 2D/3D primitives, boolean + smooth-min ops, repetition with hoisted inverse, generic tetrahedron normals, sphere tracing, **cone-tracing budget + analytic last step**, soft shadow, AO | `common` |
| `post.slang` | exposure, 8 tonemappers, dither, CAS/unsharp, colour grading, Karis bloom, depth linearize, position reconstruction, SSAO helpers | `common`, `brdf_util` |
| **`dispatch.slang`** | GFX12 occupancy/format constants, 32-B sector accounting, Little's-law bytes-in-flight, pass bandwidth in µs, halo economics, one fullscreen triangle, negated-Y viewport helpers, **Bavoil thread-group tiling** (general + pow2) | `common` |
| **`wave.slang`** | `mu_wave_size` (never hardcode 32), any/all/sum/max/min, **ballot compaction** (`mbcnt_amd`), broadcast/shuffle, fundamental-domain sample phases, `N_eff` noise budget | `common` |
| **`fp16.slang`** | explicit paired `half2`/`half4` math, colour/UV storage, the three fp16 ledgers and their device gates | `common`, `packing` |
| **`format.slang`** | `FMT_*` byte sizes (mirrors `src/vk_v2.h`), target/pass byte budgets, exact sRGB encode, snorm/gamma/depth notes | `common` |
| `tests/probe.slang` | instantiates every public function; the regression target | all 14 |
| `tests/measure.py` | recompiles tiny TUs, prints the measured SPIR-V table **and an A/B delta table** | all 14 |
| `tests/validate_math.py` | Python ports: error bounds, encode/decode pairs, SDF sign equivalence, tiling bijection, budgets | — |

---

## 2. The rules, and why

1. **Integer powers are multiplies.** `slangc 2026.21 -O3` keeps
   `pow(x, 3.0)` as `OpExtInst Pow`; `mu_pow3(x)` is two `OpFMul`s. On RDNA a
   `Pow` is `exp2 + log2` — two quarter-rate ops.
2. **Every quarter-rate op is named in a comment** with its cheap replacement.
3. **Branch-free selects, honestly priced.** Scalar `?:` compiles to
   `OpBranchConditional` + `OpPhi`; the library uses `select()`/`step+lerp`.
   Leaf-only probe: **0 control-flow ops**. But `vulkan-shader-opt/05 §1`
   shows RADV flattens an `if` with ≤8 ALU ops per side anyway
   (`radv_shader.c:209-215`), so we do **not** claim this is fewer ISA
   instructions on RADV. It guarantees the front-end form and portable wave
   behaviour.
4. **FMA is explicit.** Measured: `a*b+c` stays `OpFMul + OpFAdd` in SPIR-V;
   `mad()` emits one `OpExtInst Fma`. NIR folds the pair back to `ffma` on RADV
   (`nir_opt_algebraic.py`, `ffma_weak`), so the ISA is one `v_fma_f32` either
   way — the win is a guaranteed contract on every backend and one fewer IR
   op. `mu_round_bias` is the single spelling for every `*scale + 0.5` encode.
   **Reverted** where `mad()` defeated constant folding (see §6).
5. **Reciprocals are hoisted, not spelled as divides.** `mu_rcp(x)` emits
   `OpFDiv %float 1.0 x` -> NIR `frcp` -> `v_rcp_f32`. A vector `p/c` is three
   reciprocal lanes; `p * mu_rcp(c)` is one. **Honest limit:** `ac_nir.c:60`
   sets `lower_fdiv = true` on RADV, so a plain `a/b` is *also* `fmul(a,
   frcp(b))`. `rcp()` is a portability/intent win, **not** a RADV ISA win; the
   hoist from `%v3float` to `%float` is the measured win.
6. **No `sign()`.** One `OpExtInst FSign` in SPIR-V, but ACO's `nir_op_fsign`
   is a multiply by +Inf plus a clamp. The three 2D SDFs needed only a sign
   test → `mu_sign_nz` (one `OpSelect`). `validate_math.py` verifies **0
   differing of 321,602 grid samples** (both forms evaluate to 0 on the
   surface, so `sign(0)=0` vs `+1` is not observable).
7. **Collapse scalars before touching a vector** (`02 §2`). `mu_pbr_direct`
   now computes `(NoL * active)` once instead of two vector multiplies.
8. **`Texture2D.Load` over `SampleLevel`** where no filtering is needed:
   `OpImageFetch` carries no sampler and no derivatives (`10 §3`).
9. **An `_SRGB` image view decodes for ZERO ALU** (`04 §3`). `format.slang`
   keeps the exact transfer for procedural colours and the swapchain write;
   `pow(c,2.2)` is not sRGB and costs two quarter-rate ops.
10. **Subgroup ops replace LDS round trips** (`05 §3`). Compaction uses
    `WavePrefixCountBits` (2 ISA ops via `mbcnt_amd` on RADV), **not**
    `WavePrefixSum` (~40). `mu_wave_size()` never hardcodes 32/64.
11. **fp16 only as explicit pairs** (`11`). `RelaxedPrecision`/`mediump` is a
    no-op on RADV; scalar `half` buys registers, not throughput; the Goldberg
    2022 RDNA2 measurement lost on every axis. `fp16.slang` documents all
    three ledgers and the `VK_KHR_shader_float16_int8` gates.
12. **No silent quality/correctness trades.** Every changed formula is checked
    in `validate_math.py`; every changed op count is in §5.

### Coordinate conventions

`MU_NDC_TO_UV` (in `common.slang`) is `float2(0.5, -0.5)`. That matches the
root `README.md` table: the engine installs a **negative-height viewport**, so
clip-space `y = +1` lands at `uv.y = 0`. `dispatch.slang`'s
`mu_fullscreen_triangle_pos` uses the same flip (`clip.y = 1 - 2*uv.y`), so a
one-triangle fullscreen pass and a shadow lookup agree. Do not "fix"
orientation by editing `vk_cmd_set_viewport_scissor`.

---

## 3. Measured, not asserted

Reproduce with:

```sh
mkdir -p /tmp/tools && cd /tmp/tools
curl -sL https://github.com/shader-slang/slang/releases/download/v2026.21/slang-2026.21-linux-x86_64.tar.gz | tar xz
export PATH=/tmp/tools/bin:$PATH
cd /home/user/mu_gfx_new
python3 shaders/mu/tests/measure.py
python3 shaders/mu/tests/validate_math.py
```

`net` = ops inside the entry point minus the baseline `b[0] = float4(x);`
(7 ops). Runtime input `x` defeats constant folding. Toolchain:
`slangc 2026.21`, `-target spirv -O3`, `spirv-dis 2025.1`.

| case | net ops | image | branch | GLSL extended instructions |
|---|---:|---:|---:|---|
| `pow(x,3.0)` native | 1 | 0 | 0 | `Pow` |
| `mu_pow3(x)` | 2 | 0 | 0 | — |
| `atan2(x,0.7)` native | 1 | 0 | 0 | `Atan2` |
| `mu_atan2_fast(x,0.7)` | **17** | 0 | 0 | `FAbs, Fma, NMax, NMin` |
| `1/sqrt(x+1)` | 3 | 0 | 0 | `Sqrt` |
| `mu_rsqrt(x+1)` | 3 | 0 | 0 | `InverseSqrt, NMax` |
| `mu_srgb_to_linear` | 7 | 0 | 0 | `FMix, Pow, Step` |
| `mu_srgb_to_linear_approx` | **4** | 0 | 0 | `Fma` |
| `mu_bayer4` | 21 | 0 | 0 | — |
| `mu_pack_4x8` (uint4 from buffer) | 11 | 0 | 0 | — |
| `mu_oct_encode` | 23 | 0 | 0 | `FAbs, FMix, NMax, Step` |
| `mu_oct_decode` | 19 | 0 | 0 | `FAbs, FMix, Normalize, Step` |
| half2 pack + unpack | 15 | 0 | 0 | — |
| `mu_f_schlick` | 7 | 0 | 0 | `FClamp` |
| `mu_d_ggx` | 6 | 0 | 0 | `NMax` |
| `mu_v_smith_ggx` | 10 | 0 | 0 | `NMax, Sqrt` |
| `mu_env_brdf_approx` | 8 | 0 | 0 | `Exp2, NMin` |
| `mu_tonemap_reinhard` | 3 | 0 | 0 | — |
| `mu_tonemap_aces_fitted` | 10 | 0 | 0 | `FClamp` |
| `mu_tonemap_hable` | 13 | 0 | 0 | `FClamp` |
| `mu_tonemap_aces` (Hill) | 18 | 0 | 0 | `FClamp` |
| `mu_tonemap_uchimura` | **24** | 0 | 0 | `Exp2, FClamp, Fma, Log2, NMax, SmoothStep, Step` |
| `mu_tonemap_neutral` | 33 | 0 | 0 | `FMix, NMax, NMin, Step` |
| `mu_tonemap_agx` | **40** | 0 | 0 | `Exp2, FClamp, Fma, Log2, NMax` |
| `mu_bayer4` table (not shipped) | 12 | 0 | 0 | — |
| `mu_pcf3x3` | 46 | **9** | 0 | — |
| `mu_pcf_bilinear` | 39 | **4** | 0 | `Floor, Fract` |
| `mu_pcf_gather_box` | 41 | **4** | 0 | `Step` |
| `x*y+z` (no FMA in IR) | 2 | 0 | 0 | — |
| `mu_mad(x,0.7,0.25)` | 1 | 0 | 0 | `Fma` |
| `sign(x)` | 3 | 0 | 0 | `FSign` |
| `mu_sign_nz(x)` | 2 | 0 | 0 | — |
| `p/c` (3 vector FDiv) | 4 | 0 | 0 | — |
| `p*mu_rcp(c)` (1 scalar FDiv) | 4 | 0 | 0 | — |
| `mu_sd_repeat3(p,c)` | 14 | 0 | 0 | `Fma, Fract` |
| `mu_sd_repeat3_inv(p,inv,c)` | 12 | 0 | 0 | `Fma, Fract` |
| `mu_tiled_group_id` (2 UDiv) | 14 | 0 | 0 | `UMin` |
| `mu_tiled_group_id_pow2` (1 UDiv) | 13 | 0 | 0 | `UMin` |
| `Texture2D.Load` | 4 | 1 | 0 | — |
| `Texture2D.SampleLevel` | 4 | 1 | 0 | — |
| `WavePrefixSum` (~40 ISA ops) | 4 | 0 | 0 | — |
| `mu_wave_compact_offset` (2 ISA ops) | 4 | 0 | 0 | — |
| `half2` paired mul+add | 5 | 0 | 0 | — |
| `mu_linear_to_srgb` (exact) | 11 | 0 | 0 | `Exp2, FClamp, FMix, Log2, NMax, Step` |

Reading the table:

- **SPIR-V op counts understate hardware cost.** `atan2` is one
  `OpExtInst Atan2` but ~109 driver instructions; `WavePrefixSum` is one
  `OpGroupNonUniformIAdd` but ~40 ISA ops, while `WavePrefixCountBits` is two
  SPIR-V ops *and* 2 ISA ops via `v_mbcnt_lo_u32_b32`. Count classes, not lines.
- `1/sqrt` and `mu_rsqrt` are both 3 net ops, but the former is `Sqrt` + a
  divide and the latter is one `InverseSqrt` + a `max`. The opcode matters.
- `mu_pack_4x8` is **3 shifts + 3 ors = 6 ops**; the 11 net include the buffer
  read. The static Bayer table is 12 net but spills to `OpVariable`/`OpLoad`;
  the shipped arithmetic form is 21 pure ALU ops.
- `probe.slang` compiles to **7076 SPIR-V ops with 0 warnings**; **66
  control-flow ops come only from the four SDF marchers**, and the leaf-only
  variant is **6756 ops and 0 control-flow ops**. The absolute number grew from
  6861 because four new modules and ~90 new public helpers were added; the
  per-helper savings are below.
- `shader_scan.py shaders/mu`: **54 findings, all `info`; 0 medium / low /
  high / critical** (was 67 with 3 medium `sign-call` and 6 low). `vk_scan.py
  src`: 0 findings.

### A/B deltas from this revision

`measure.py` prints this table itself. `vecFD` = `OpFDiv %vNfloat` (N
reciprocal lanes), `sclFD` = `OpFDiv %float` (one), `UDiv` = integer divide.

| change | before | after | delta | vecFD | sclFD | UDiv | mechanism |
|---|---:|---:|---:|---|---|---|---|
| `x*y+z` → `mu_mad` | 2 | 1 | −1 | 0→0 | 0→0 | 0→0 | `OpFMul+OpFAdd` → 1 `OpExtInst Fma` |
| `sign(x)` → `mu_sign_nz` | 3 | 2 | −1 | 0→0 | 0→0 | 0→0 | `FSign` (+Inf mul + clamp) → 1 `v_cndmask_b32` |
| `p/c` → `p*mu_rcp(c)` | 4 | 4 | +0 | **1→0** | 0→1 | 0→0 | 3 `v_rcp_f32` lanes → 1; op count hides it |
| `repeat3` → `repeat3_inv` | 14 | 12 | −2 | 0→0 | **3→0** | 0→0 | caller hoists the uniform inverse |
| tiling → pow2 tiling | 14 | 13 | −1 | 0→0 | 0→0 | **2→1** | uniform scalar-ALU division |
| `PrefixSum` → `PrefixCountBits` | 4 | 4 | +0 | 0→0 | 0→0 | 0→0 | ~40 ISA ops → 2 (`mbcnt_amd`); SPIR-V hides it |

Other measured module deltas (before → after, same harness):
`mu_atan2_fast` **19 → 17**, `mu_srgb_to_linear_approx` **6 → 4**,
`mu_tonemap_uchimura` **26 → 24**, `mu_tonemap_agx` **47 → 40**.

**Scope.** These are net SPIR-V body ops. SPIR-V cannot price registers or the
final ISA: `OpExtInst` and `OpGroupNonUniform*` hide multi-instruction
expansions. The VGPR/wave count and ACO listing still require
`RADV_DEBUG=shaderstats` or RGP on the RX 9070 XT target — that is the **next
measurement**, not a claim made here. No speedup percentages are claimed.

### Regression check

```sh
cd shaders
slangc mu/tests/probe.slang -target spirv -entry cs -stage compute -O3 -I . -o /tmp/mu_probe.spv
spirv-dis /tmp/mu_probe.spv | grep -E 'OpBranch|OpPhi|OpSelectionMerge|OpLoopMerge'
```

Only the SDF marchers should appear. Any `slangc` warning is a failure.

### Numerical validation

`python3 shaders/mu/tests/validate_math.py` checks the math independently of
the compiler (no `slangc` needed). Measured:

| check | result |
|---|---|
| `mu_atan2_fast` max error, full circle | **0.000204 rad (0.0117°)** |
| `mu_bayer4` vs the standard 4×4 table | **MATCH**, 16/16 |
| `mu_srgb_to_linear_approx` max abs error | **0.00167** at c = 0.082 |
| oct encode/decode, 40k directions | **2.6e-8 rad** worst |
| RGBM / RGBD / RGBE round trip (8-bit) | 7.14% / 0.35% / 0.39% of pixel peak |
| depth24 round trip | **3.0e-8** |
| cascade index samples | [0, 1, 2, 3, 3] |
| `sign()` → `mu_sign_nz()` in 2D SDFs | **0 differing of 321,602 samples** |
| group-ID tiling | bijection on exact-divisor grids; clamps on 6 non-multiple grids |
| 1440p RGBA16F read+write | **91.5 µs** @ 644.6 GB/s |
| halo border/interior, 8/16/32 tile | **56.2 / 26.6 / 12.9 %** |
| `mu_sdf_last_step` closed form | geometric-tail identity, q = 0.1…0.95 |
| exact sRGB encode/decode round trip | **4.4e-16** |
| fullscreen triangle vertices | (0,0),(2,0),(0,2) → clip (−1,1),(3,1),(−1,−3) |
| post `mad()`/Horner rewrites | Hable exact; AgX polynomial identity **9.2e-15** |

**Measured discrepancy, recorded rather than hidden:** `vulkan-opt/09 §3A.3`
prose defines halo overhead as `(fetched − interior)/fetched` but its table
reports `border/interior` (8×8: 36/64 = 56%). The library ships both, named
`mu_halo_overhead` and `mu_halo_border_ratio`. Likewise the skill's claim that
the Bavoil remap "is a bijection for any grid" is only true when `grid.x` is a
multiple of `N`; otherwise the `min()` clamps keep outputs in range but
duplicate tiles. `dispatch.slang` states the precondition and `src/vk_v2.h`
§16 asserts it at frame-build time.

---

## 4. Usage recipes

### PBR

```slang
#include "mu/lighting.slang"

MuSurface s = mu_surface(albedo, N, V, roughness, metalness);
float3 L = mu_safe_normalize(light_pos - world_pos);
float3 radiance = float3(4.0) * mu_point_attenuation(distance2, radius) * shadow;
float3 direct = mu_pbr_direct(s, L, radiance);
float3 indirect = mu_pbr_indirect(s, irradiance, prefiltered_env, ao, spec_ao);
```

`mu_pbr_direct` returns `F*V*D + diffuse` already multiplied by
`radiance*NoL*active`; visibility includes `1/(4 NoL NoV)` — do not divide
again. Use `mu_tbn_unit` when the vertex tangent is MikkTSpace (already unit):
`mu_tbn` keeps a defensive `normalize()` for untrusted data.

### Compute post-pass with L2-friendly launch order

```slang
#include "mu/dispatch.slang"

#define TILE 8
[numthreads(MU_WG_64, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID, uint2 gid : SV_GroupID)
{
    // Only for a wide-kernel gather that is VRAM-bound with L2 hit < ~80%.
    uint2 grid = uint2(240u, 135u);            // 1440p / 8, 240 % 16 == 0
    uint2 tiled = mu_tiled_group_id_pow2(gid, grid, 4u);   // N = 16
    uint2 pixel = mu_dispatch_pixel(tiled, tid.xy, uint2(8u, 8u));
    // ...
}
```

`MU_WG_64/128/256/1024` come with the GFX12 occupancy table: 128/256 give
100% occupancy with slack, 1024 has **zero** slack (49 VGPRs or 33 KiB LDS →
50% occupancy). `MU_VGPR_FULL_OCC` is **96**, derived from 98,304 regs/CU ÷
1,024 threads — not the stale 48 from `vulkan-shader-opt/SKILL.md §1`.

### Stream compaction without an LDS round trip

```slang
#include "mu/wave.slang"

bool visible = ...;
uint slot = mu_wave_compact_offset(visible);      // 2 ISA ops via mbcnt_amd
uint total = mu_wave_compact_count(visible);
if (visible) { out_buf[base + slot] = item; }    // branch skips a MEMORY write
```

Never use `WavePrefixSum` for a 0/1 predicate (~40 ISA ops). Cross-wave
exchange still needs `groupshared` + a barrier. A per-light scalarisation loop
takes `mu_wave_loop_trip_count()` and guards with `>=`, never `!=`, or helper
lanes hang a fragment shader.

### Format choice is a bandwidth decision

```slang
#include "mu/format.slang"

float us = mu_pass_bandwidth_us(2560u, 1440u, MU_BPP_RGBA16F, 2u);  // 91.5
uint bytes = mu_pass_bytes(2560u, 1440u, MU_BPP_RGBA8_UNORM);
float3 linear = mu_srgb_view_equivalent(albedo_tex.Sample(samp, uv)); // or use FMT_RGBA8_SRGB and delete this
```

An `FMT_RGBA8_SRGB` view decodes in the sampler for zero ALU. `FMT_RGBA8_SNORM`
does `*2-1` in the fetch. `mu_linear_to_srgb` is only for the final write.

### SDF march with a bounded step budget

```slang
#include "mu/sdf.slang"

float eps = mu_sdf_cone_eps(t, cone_width);      // scale-relative, >= f32 ulp
float mip = mu_sdf_cone_mip(t, cone_width, voxel_size);
// ... break when D < cone_width*t; keep dprev; then:
float remaining = mu_sdf_last_step(d, dprev);    // one division, guards included
// verify ro + rd*(t+remaining) with ONE extra field fetch before accepting it
```

Sphere tracing is **fetch bound**: cost = steps/pixel × bytes/fetch. Build
volume mips from the distance field, not a box downsample (averaging distances
overestimates them). `mu_sdf_gradient_eps` returns ~0.5–1 voxel, the practical
tetrahedron-gradient epsilon.

---

## 5. Rejected ideas (measured, then reverted)

Both skills require "no change ⇒ reverted" and an explicit rejected list.

| Idea | Measurement | Verdict |
|---|---|---|
| `mad()` in `mu_hable_curve` | `mu_tonemap_hable` **13 → 16** net ops: `mu_tonemap_hable` also evaluates the curve at the constant white point W=11.2, and slangc folds the original expression but **not** `OpExtInst Fma` | **reverted** |
| `mu_mads()` in `mu_env_brdf_approx` | **8 → 13** net ops for the same reason: `roughness` is constant at many call sites | **reverted** |
| Shift/or rewrite of `mu_bayer4` | both forms **32 ops** end-to-end, identical over all 16 pixels | **no change, not shipped** |
| `RelaxedPrecision` / `mediump` fp16 | RADV AMD backend has no handling of relaxed precision; it is a decoration count, not a win | **rejected** (`11 §2`) |
| `rcp()` as a RADV ISA win | `ac_nir.c:60` sets `lower_fdiv = true`, so `a/b` is already `fmul(a, frcp(b))`; only the vector→scalar hoist is a win | **claim corrected**, helper kept for portability |
| `step()`/`lerp()` as an ISA saving | RADV flattens `if` with ≤8 ALU ops per side anyway | **claim corrected**, spelling kept |
| Morton/Z-order group tiling | Bavoil: plain horizontal tiling works better (fewer index ops) | **rejected** (`13 §2`) |
| 1024-invocation workgroups as a default | 49 VGPRs or 33 KiB LDS → 50% occupancy instantly | **rejected** (`09 §3A.2`) |
| Applying group tiling to a one-texel-per-pixel pass | no overlapping footprints ⇒ only adds arithmetic | **rejected** (`13 §2`) |

**Next measurement (not done here):** `RADV_DEBUG=shaderstats` for VGPR/wave
counts on `probe.slang` and the four new modules, then RGP pass-time + L2 hit
rate before/after `mu_tiled_group_id_pow2` on a real wide-kernel post pass.
SPIR-V alone cannot price either.

---

## 6. Deliberate constraints

- **`mu_load_u32_le` takes a `ByteAddressBuffer`**, not a `uint` device
  address: the pointer form adds `PhysicalStorageBufferAddresses` /
  `OpConvertUToPtr`.
- **No `fwidth`/derivatives**, so every helper works in compute post-passes.
- **No LUTs.** `mu_env_brdf_approx` replaces the split-sum texture; AgX and
  Uchimura are analytic.
- **No `pow(x, integer)`**; multiply forms are `mu_pow2`…`mu_pow8`.
- **`pack_f16x2` / `unpack_f16x2` do not exist** in Slang 2026.21; the library
  uses `f32tof16` / `f16tof32`.
- **No global `mediump`**, no hardcoded wave size, no `sign()`, no
  `pow(x,0.5)` / `pow(x,3.0)`, no `1/sqrt(x)` in a hot loop.
- `MU_WG_*`, `MU_VGPR_FULL_OCC`, `MU_BPP_*` and `MU_NDC_TO_UV` are mirrored in
  `src/vk_v2.h` §16–§17 so the CPU frame program and the shader agree on the
  same numbers. Keep the two lists in sync.
- No speedup percentages are claimed anywhere.
