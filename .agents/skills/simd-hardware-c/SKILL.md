---
name: simd-hardware-c
description: >
  Extract maximum single-core desktop x86-64 performance in C using cache-line
  engineering, store-to-load forwarding (STLF) & 4K-aliasing avoidance, TLB/2MB
  hugepages, MSHR software prefetching, OS virtual-memory tricks, SWAR, BMI1/2,
  PCLMULQDQ, AVX2, FMA, and AVX-512 intrinsics. Load this skill whenever writing
  hot numerical kernels, parsers, bitsets, culling, image/audio/mesh transforms,
  SIMD hash/tree probes, or micro-optimizing single-threaded C for desktop CPUs.
---

# Skill: Desktop x86-64 Hardware, SWAR & Full SIMD (AVX2 / AVX-512) in C

> **Separation Rule:** This skill covers **single-core hardware, cache/VM, ALU/ILP, SWAR, and SIMD execution ONLY**. Keep multithreading strictly separate (use `.agents/skills/multithreading-dod/SKILL.md` only after single-core throughput is saturated).
> Every pattern here is empirically proven in `perf_test/` (`perf_test/PERF_PROOFS.md`).

---

## 1. Desktop x86-64 Hardware Budget (Ice Lake / Alder Lake / Zen 4+)

| Resource | Typical Desktop Spec | Engineering Rule |
|---|---|---|
| **L1d Cache** | `32–48 KiB`, 8–12 way, `4–5 cyc` | Tile inner working sets & SIMD LUTs to `<= 32 KiB`; pad 2D pitches by `+64B` to avoid `4096B` set-conflict thrashing |
| **L2 Cache** | `1–2 MiB`, `12–14 cyc` | Stage AoS↔SoA transposes (`64 KB`) & radix histograms (`4 KB`) inside L2; protect hot L2 from write-only streams with `_mm256_stream_ps` (`1.63×`) |
| **L3 / DRAM** | `16–64 MiB` (`~40 cyc`) / DRAM (`~200+ cyc`) | Eliminate pointer chasing (`CSR = 32.72×`); overlap misses across `10–16` MSHRs via `MU_PREFETCH` (`Eytzinger = 5.92×`) |
| **DTLB & Pages** | `64` L1 DTLB entries (`256 KB` @ 4KB) | Advise `MADV_HUGEPAGE` (`2 MB` pages) on large arenas so hardware prefetchers don't stop at 4 KB boundaries |
| **Store Buffer** | `56–72` entries, strict STLF | Match store & load widths (`4.29×` speedup over `2×u32` store -> `u64` load); offset `dst` vs `src` away from `4096B` multiples |
| **FMA / SIMD** | `2 × 256b/512b` FMA/cyc, `4 cyc` lat | Use `Latency × Throughput = 8` independent AVX accumulators (`20.20×`) and Estrin's tree (`1.74×`) |
| **DSB (µop-cache)** | `~2.25K–4K` µops, `6–8` µops/cyc | Keep hot inner loop `< 256 bytes` of code; outline cold error paths (`MU_UNLIKELY` + `__attribute__((cold, noinline))`) |

---

## 2. Step-by-Step SIMD & Hardware Optimization Procedure

### Step 1 — Fix Memory Layout & Alignment Before Writing Intrinsics
1. Align hot arrays to **64 bytes** (`MU_ALIGN(64)` / `aligned_alloc(64, ...)`), and pad element counts up to a multiple of `8` (AVX2) or `16` (AVX-512) with harmless sentinel values (`0.0f`, `+FLT_MAX`) so the hot loop runs **zero scalar tail branches**.
2. Choose **SoA** for 1–6 linear streams (`11.50×`), **Tiled `AoSoA8`/`AoSoA16`** for 6–20 field packet access (`3.96×`), or **in-register 8×8 AVX2 shuffle transpose** (`2.94×`) when consuming external AoS buffers — NEVER `_mm256_i32gather_ps` on contiguous structs!
3. Mark all non-overlapping pointers `MU_RESTRICT`.

### Step 2 — Saturate Execution Ports & Eliminate Scalar Bottlenecks
1. **Break FMA/ALU chains:** Unroll reductions `4×` or `8×` into separate `__m256` accumulators (`acc0..acc7`), summing them once after the loop (`20.20×`).
2. **Kill `div` / `sqrtf`:**
   - Replace `1.0f / sqrtf(x)` with `_mm256_rsqrt_ps(x)` + 1 FMA Newton-Raphson step (`y * (1.5f - 0.5f * x * y * y)`, `6.70×` faster, ~23-bit precision; never use Quake `0x5f3759df` which stalls on XMM↔GPR moves!).
   - Replace runtime `x % d` with Lemire `fastmod` (`2.98×`) or hash-bucket `fastrange32` (`11.48×`).
   - Batch scalar divisions via Montgomery's trick (`inv_ab = 1/(a*b)`).
3. **Kill branches inside lanes:**
   - Compare -> blend: `_mm256_blendv_ps(a, b, cmp)` or integer `_mm256_and_si256(cmp, val)` (`32.74×` over 50/50 branch).
   - Early-out across 8 lanes at once when uniform: `if (_mm256_movemask_ps(active) == 0) continue;` (Fabian Giesen lane-mask consolidation).

### Step 3 — Apply Domain-Specific Desktop SIMD Idioms
See `references/avx2-avx512-kernels.md` for copy-paste C kernels:
- **Multi-character byte classification (`15.59×`, 26.5 GB/s):** Lemire 2-nibble `_mm256_shuffle_epi8` (`vpshufb`) 16-byte register LUT + unsigned saturating-sub range checks.
- **Stateful quote/escape prefix parity (`24.19×`, 25.6 GB/s):** `simdjson` `_mm_clmulepi64_si128(quote_bits, -1ULL, 0)` carryless multiply prefix-XOR.
- **Stream compaction / filtering (`21.69×`, 5.2B ints/s):** `simdprune` 256-entry `_mm256_permutevar8x32_epi32` LUT on AVX2, or 1-instruction `_mm512_mask_compressstoreu_epi32` on AVX-512 (`20.64×`).
- **ASCII digit parsing (`5.03×`, 3.1B ints/s):** `_mm256_maddubs_epi16` (`*10+1`) -> `_mm256_madd_epi16` (`*100+1`) -> `_mm256_packus_epi32` -> `_mm256_madd_epi16` (`*10000+1`).
- **Large bitset popcount (`11.51×` AVX2 / `23.72×` AVX-512, 94.7 GB/s):** Muła-Kurz-Lemire `vpshufb`+`vpsadbw` CSA or `_mm512_popcnt_epi64`.
- **SIMD data-structure probes:** 32-way SwissTable H2 `_mm256_cmpeq_epi8` (`2.40×`), 16-ary cache-line AVX2 S+Tree (`9.79×`), and 256-bit Split-Block Bloom `_mm256_testc_si256` (`3.43×`).
- **Loop Interchange & `4×16` Register Micro-Kernels (`20.1×` / `44.1×`, 72.1 GFLOP/s):** Always put unit-stride `j` innermost (`i-k-j`) and accumulate `MR × NR` output blocks in YMM registers across L1-sized `KC` strips.
- **Special-purpose desktop silicon (`AES-NI`, `VPCLMULQDQ+CRC32C`, `GFNI`, `StreamVByte`, `pshufb` DFA) & Distance Kernels:**
  - Hash buffers/strings with 4-lane `_mm_aesenc_si128` (`76.6×`, 54.8 GB/s), 3-lane `_mm_crc32_u64` (`32.3×`, 23.1 GB/s), or **Corsix `fast-crc32` (`v3s1_s3` / `v4s5x3`)** fusing 512-bit `VPCLMULQDQ` folding on vector ports with concurrent scalar `crc32q` on integer ports (`64–97 GB/s`; compile TU with `-mavx512f -mavx512vl -mvpclmulqdq -mpclmul -msse4.2`).
  - Transform sub-byte bit layouts / reverse bits with 1-instruction `GFNI` `_mm256_gf2p8affine_epi64_epi8` (`93.2×`, 87.8 GB/s).
  - Decode variable-length integer deltas with `StreamVByte` (`_mm_shuffle_epi8` + 2-bit control stream, `43.9×`, 5.64B ints/s).
  - Parallelize `<= 16`-state finite state machines via associative function composition `_mm_shuffle_epi8(T_b, T_a)` (`8.84×`, 3.64 GB/s).
  - **Single-pass multi-accumulator distance kernels (`Cosine`, `Hamming`, `Jaccard`/`IoU`):** Fuse `dot_ab`, `dot_aa`, `dot_bb` in 1 memory pass (`12` YMM/ZMM FMA accumulators); interleave `AND`+`OR` popcounts in 1 pass (or derive `popcnt(A|B) = popA + popB - popcnt(A&B)` when cardinalities are cached).
- **10-Step "Zipper Algorithm" for SIMD Width Upconversion (`XMM -> YMM -> ZMM`):**
  1) 3-op VEX + tag integer flag liveness; 2) replicate narrow loop as fallback (prevents OOB on `n % wide != 0`; `vmovaps -> vmovups` unless aligned); 3) unroll wide loop `2×` & double count check; 4) rename 2nd-half dests `ymm0..15 -> ymm16..31`; 5) pairwise swap 2nd-half ops upward (adjusting memory offsets `+32` across `add $32, %ptr`); 6–7) merge integer bumps & vector pairs (checking `vpmovsx`/`zx` sign and intra-128b-lane shuffles vs `vpermd`); 8–10) promote constants (`vinsertf64x4`), fold accumulators (`_mm512_reduce_add_ps`), update clobbers/CPUID/`vzeroupper`, and use AVX-512 `{z}` masked tails (`__mmask16 k = (1u << rem) - 1`).
- **Compiler/ABI & Runtime Dispatch Unlockers:** Pass `__builtin_assume_aligned(p, 64)` + `if (n & 7) __builtin_unreachable()` to kill scalar peel/tail code; keep value-passed structs `<= 16B` on SysV Linux (`<= 8B` on Win64); build releases with `-O3 -march=native -flto=auto -fno-plt` + PGO. For portable multi-versioning:
  - `target_clones("default","avx2,fma","avx512f")` works **only** on plain auto-vectorized C (never widens `_mm256_*`!), **must** be non-`static` + `noinline` (`objdump -t bin | grep .resolver`), rejects `"x86-64-v3/v4"` in GCC, and should be avoided in `.so` libs (IFUNC init-order hazard).
  - Use `__builtin_cpu_supports` + `static __attribute__((target(...)))` variants for intrinsics & `.so` libs (Arm: `getauxval(AT_HWCAP)`). Watch ISA flags: `_mm512_popcnt_epi8/16` needs **`avx512bitalg` + `avx512bw`** (NOT `avx512vpopcntdq`, which is `epi32/64` only!).

---

## 3. Critical AVX2 / AVX-512 & Compiler Codegen Pitfalls to Avoid

1. **The AVX2 128-Bit Lane Trap:** `_mm256_shuffle_epi8`, `_mm256_packus_epi32`, `_mm256_unpacklo_ps`, and `_mm256_alignr_epi8` operate **independently within the low 128-bit lane and high 128-bit lane**! Use `_mm256_permutevar8x32_epi32` (`vpermd`), `_mm256_permute4x64_epi64` (`vpermq`), or `_mm256_permute2x128_si256` when lanes must cross `[127:0] <-> [255:128]`.
2. **The "Vectorized" Float-Reduction Serial Chain Trap:** Without `-fassociative-math` (or `#pragma omp simd reduction(+:sum)`), IEEE-754 forbids reassociating `(a + b) + c`. Clang/GCC may print `remark: vectorized loop` while emitting packed `vmulps` feeding a **serial chain of scalar `vaddss`** — running at scalar `fadd` latency! Always unroll 4–8 vector accumulators explicitly in C (deterministic & fast without `-ffast-math`).
3. **Runtime Alias Versioning & the In-Place (`a == c`) Fallback Trap:** Without `MU_RESTRICT`, Clang/GCC often vectorize `a[i] = b[i]*s + c[i]` behind a runtime pointer-overlap check. Distinct buffers run at full SIMD speed, **but calling in-place (`a == c`, i.e., `saxpy`) fails the check and silently drops to the scalar fallback (`5.29×` slower in L1!)** — use dedicated `MU_RESTRICT` in-place kernels or `vectorize(assume_safety)`.
4. **Never Sprinkle `__builtin_prefetch` on Linear Streams:** Software prefetch on unit-stride arrays wastes load/AGU ports, bloats uops, and **mistrains hardware stream prefetchers** (Lee et al. TACO'12). Reserve `MU_PREFETCH` strictly for irregular/tree/index-gather hops (`nodes[idx[i + dist]]`).
5. **Missing `vzeroupper` (`other_assists.avx_to_sse`) & Silicon Quirks:** Writing `ymm0–15` or `zmm0–15` dirties upper bits, causing a multi-hundred-cycle save/restore on the **first legacy non-VEX SSE instruction** afterward! Verify `avx_to_sse == 0` via `perf stat -e other_assists.avx_to_sse,other_assists.sse_to_avx ./bin`; emit `_mm256_zeroupper()` before exits/callbacks to SSE code, and in inline `asm` clobber **all** `ymm0–15` (or `"zmm0".."zmm15"` — tracked separately by GCC/Clang; `zmm16–31` need no `vzeroupper`). Beware Skylake AVX-512 license throttling phases, `tzcnt`/`lzcnt`/`popcnt` false output dependencies, and hardware **zero-store elimination** (always fill benchmark arrays with non-zero patterns like `(i & 7) * 0.25f`!).
6. **Quantized Dot Products (`VNNI` / `SDOT` / `AMX`):** For `int8` inference/DSP, `AVX-512 VNNI` (`vpdpbusd`) and Arm `SDOT` give `4×` MACs/instruction + `4×` less bandwidth (on pre-VNNI AVX2, `vpmaddubsw` saturates signed `int16` and requires `u8 * i8` + zero-point compensation).
7. **Verify Compiler Codegen & Pair `llvm-mca` with Counters:** `llvm-mca` ignores the front end and caches; check front-end/DSB loops with `uiCA` (note Skylake 32B JCC erratum `-mbranches-within-32B-boundaries`) and measure real timings with `SINK(res)` + `OPAQUE(ptr)` under the Seven-Field Rule.
