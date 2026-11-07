# PERF_MATH — `mu/mu_math.h` vs cglm, seven-field record

Every number here satisfies AGENTS.md §2.6.3: all seven fields recorded, paired
round-robin reps, `CV` reported next to the ratio, and losses left in the table.
If a row is missing, it is because it was not measured, not because it was bad.

Reproduce:

```sh
git clone --depth 1 https://github.com/recp/cglm ../../cglm      # header-only
make -C perf_test math-test      # correctness gate: exits non-zero on failure
make -C perf_test math-bench     # this table + results_math.csv
```

---

## 1. The seven fields

| # | Field | This run |
|---|-------|----------|
| 1 | CPU model & µarch | `Intel(R) Xeon(R) Processor @ 2.60GHz`, family 6 model 106 (Ice Lake server), KVM guest. Single µarch, no P/E core mix. |
| 2 | Cores / threads / affinity | 2 vCPU presented = **1 physical core + SMT2**. All measurements pinned to core 0 via `sched_setaffinity`; sibling left idle. No multithreaded cases are claimed. |
| 3 | Measured clock | **3.21 GHz**, measured per run by 100M dependent `add` chain over wall time (`measure_ghz`), not read from `/proc`. Nominal 2.60 GHz; DVFS moves it run to run, which is why `cyc/op` is derived from the *measured* clock and ratios are the only claimable output. |
| 4 | Compiler & flags | `gcc 14.2.0`, `-O3 -march=native -std=c11 -Wall -Wextra -Werror`. Backend selected by `-march` (`avx512f=1 avx2=1 fma=1` here). cglm built with the same flags in the same TU, so its `__AVX2__`/SSE paths are live. |
| 5 | Workload & working set | Per-call: 256 matrices (16 KB), 1024 vec3 (12 KB), 512 quats — all L1/L2 resident, initialised with non-zero random data (a zero matrix makes an inverse look free and is exactly the hardware zero-store elimination trap). Batch: 1M points ≈ 28 MB (DRAM), 64K points × 8 ≈ 1.5 MB (L2/L3), 256 bones = 48 KB. |
| 6 | Baseline | cglm at git `b9b570b`, its own kernels (`glm_mat4_mul` 16-FMA SSE, `glm_mat4_inv` SSSE3 cofactor, `glm_mat4_mul_avx` etc.). Not "a naive C loop". |
| 7 | Method | `CLOCK_MONOTONIC_RAW`, `SINK()` + `OPAQUE()` anti-hoist/anti-DCE, 3 warmup reps, **21 paired reps in randomized round-robin**, reporting **min** (latency floor), **median** (throughput), **geometric mean of paired ratios** (normalized ratio, per §2.6.3) and **CV**. The C-loop control case separates "SIMD" from "layout". |

Two statistics are printed for the ratio because they disagree informatively:
`min-ratio` is fragile (one descheduled rep collapses it — the `mat4_inverse`
row shows 0.12x with CV 110%), `geo-ratio` and `med-ratio` are stable. **When
`CV` is above ~10%, quote nothing but the median/geo ratio, and treat the row as
parity-or-worse.** The bench prints all three so you can see the disagreement
instead of trusting one number.

Host is a shared KVM guest on one physical core: absolute ns drift >20% between
runs on identical code. Pairing is what makes the ratios usable; do not compare
`ns/op` across runs, only ratios within a run.

---

## 2. Per-call: single object, L1/L2 resident

The regime cglm is already optimal in. Parity is the correct result here.

| case | ns/op | cyc/op | min-ratio | med-ratio | geo-ratio | CV |
|---|---|---|---|---|---|---|
| vec3_dot | 0.67 | 2.15 | 2.94x | 1.00x | 1.06x | 1.0% |
| vec3_cross | 0.62 | 2.01 | 1.00x | 1.00x | 1.00x | 0.4% |
| vec3_add | 0.62 | 2.00 | 1.00x | 1.00x | 1.00x | 0.3% |
| vec3_normalize | 0.63 | 2.04 | 1.04x | 1.02x | 1.02x | 0.5% |
| **vec3_normalize_fast** | 1.55 | 4.99 | **0.42x** | **0.42x** | **0.42x** | 0.3% |
| vec3_normalize (dependent chain) | 13.19 | 42.40 | 0.81x | 1.00x | 0.99x | 8.6% |
| vec3_normalize_fast (chain) | 12.20 | 39.21 | 0.88x | 1.08x | 1.06x | 15.4% |
| mat4_mul | 2.18 | 7.02 | 1.69x | 1.69x | 1.69x | 0.5% |
| mat4_mul_affine | 1.46 | 4.70 | 2.34x | 2.58x | 2.57x | 2.8% |
| mat4_mulv | 0.68 | 2.19 | 2.82x | 2.89x | 2.89x | 0.8% |
| mat4_point (hot matrix) | 0.62 | 2.06 | 2.86x | 2.90x | 2.90x | 0.6% |
| mat4_transpose | 1.15 | 3.81 | 1.88x | 1.89x | 1.89x | 0.9% |
| **mat4_inverse (general)** | 6.27 | 20.79 | 0.12x* | **0.93x** | **0.84x** | 110.6% |
| inverse_rigid (rigid) | 0.71 | 2.36 | 1.01x | 0.98x | 0.98x | 1.0% |
| inverse_affine (rigid) | 1.68 | 5.56 | 0.43x | 0.41x | 0.42x | 0.6% |
| inverse_affine vs general inv | 1.68 | 5.56 | 3.38x | 3.44x | 3.44x | 0.5% |
| inverse_affine (scaled) | 1.68 | 5.56 | 3.40x | 3.48x | 3.48x | 0.6% |
| mat3_inverse_transpose | 2.19 | 7.25 | 0.97x | 0.96x | 0.96x | 1.9% |
| quat_mul | 0.84 | 2.79 | 2.49x | 2.52x | 2.52x | 0.7% |
| quat_rotate_vec3 | 3.64 | 12.01 | 2.53x | 2.53x | 2.53x | 0.4% |
| quat_slerp | 17.75 | 58.87 | 1.47x | 1.08x | 1.09x | 1.4% (24% on noisy runs — treat as parity) |
| mat4_from_quat † | 0.66 | 2.19 | 3.98x | 4.02x | 4.02x | 0.7% |
| mat4_from_quat +normalize † | 2.83 | 9.38 | 0.94x | 0.94x | 0.95x | 1.3% |
| perspective | 1.00 | 3.32 | 0.99x | 0.98x | 0.99x | 3.8% |

\* min-ratio is a measurement artifact: one rep measured near-zero for mu.
† `glm_quat_mat4` normalizes the quaternion internally (`glm_quat_normalize_to`);
`mu_mat4_from_quat` documents that it does not. The 4.0x row is therefore
**not** apples-to-apples — the `+normalize` row (0.94x) is the honest one.

## 3. Batch: many objects — the actual thesis

| case | ns/op | cyc/op | min-ratio | med-ratio | geo-ratio | CV |
|---|---|---|---|---|---|---|
| xform 1M pts (cglm doing the work) | 0.81 | 2.70 | 2.66x | 2.65x | 2.65x | 3.4% |
| xform 1M pts (same layout, C loop) | 0.82 | 2.73 | 2.99x | 2.95x | 2.98x | 3.0% |
| xform 64K pts x8 | 0.27 | 0.89 | 8.96x | 9.10x | 9.01x | 4.8% |
| normalize 1M vec3 | 0.79 | 2.62 | 2.58x | 2.53x | 2.53x | 3.0% |
| rotate 1M pts by 1 quat (SoA) | 0.89 | 2.61 | 4.22x | 4.47x | 4.20–4.58x | 2.5–7% |
| rotate 1M (mu per-call loop) | 3.39 | 9.91 | 0.99x | 1.00x | 1.00x | 1.6–5.7% |
| frustum cull 1M spheres | 0.43 | 1.44 | 29.60x | 28.71x | 29.58x | 3.9% |
| mat4 palette 256 (pack+unpack) | 3.11 | 10.31 | 1.04x | 0.99x | 1.00x | 0.6% |
| mat4 palette 256 (pack each iter) | 3.79 | 12.56 | 0.81x | 0.82x | 0.82x | 2.3% |
| mat4 palette 256 (kernel only) | 1.80 | 5.98 | 1.70x | 1.71x | 1.73x | 2.3% |

The C-loop control (0.82 ns, 2.98x) says the win is the streamed SoA layout plus
SIMD, not "mu is better at multiplying": a hand-written C loop over the same
layout gets the same 3x. What the C loop *cannot* do is skip the 64 MB of matrix
re-reads per call, which is the point.

The 2.65x (1M points, DRAM) vs 9.01x (64K×8, L2/L3) split is a bandwidth roof,
not an inconsistency. Roofline: 12 FMA per 4 points = 24 FLOP per 16 B read;
at 1M points the stream is DRAM-bound and *both* implementations are waiting on
memory, so the ratio compresses to the byte-count ratio. Quote 2.65x for
DRAM-resident streams and ~9x only when you say "cache-resident".

---

## 4. Why `mat4_inverse` loses (and what to use instead)

- mu's SSE cofactor kernel: **76 instructions** (`objdump -d`, `-O3 -march=native`).
- cglm's `glm_mat4_inv`: **70 instructions**.
- Measured: **0.85–0.95x**. Parity at best.
- The *scalar* GLM transcription it replaced auto-vectorized into **99
  instructions with eight `vpermi2ps`**, all of which issue on port 5 — the same
  port that limits the shuffle-bound cofactor expansion. That is why the scalar
  version measured 0.66x: not because C is slow, but because GCC's vectorization
  of that particular arrangement is *worse* than the hand-written form.

The real win is not a faster 4×4 cofactor inverse; it is not running one:

| path | ns/op | vs `glm_mat4_inv` | vs `glm_inv_tr` |
|---|---|---|---|
| `mu_mat4_inverse` (general) | 6.27 | 0.93x | — |
| `mu_inverse_affine` (scale) | 1.68 | **3.44x** | 0.42x |
| `mu_inverse_rigid` (orthonormal) | 0.71 | **8.8x** | 0.98x |

`glm_inv_tr` is a dedicated transpose-only kernel and it beats
`mu_inverse_affine` (which pays three reciprocals for its scale case) by 2.4x.
That gap was found by this table, and `mu_inverse_rigid` was added in response:
it closes it to parity (0.98x) while keeping the affine/scale path for when
scale is real.

---

## 4b. Quaternion rotation: where the algorithmic win actually is

The two-cross-product form `c = u x v; v' = v + 2w*c + (2u x c)` is the right
way to rotate a vector by a unit quaternion — it replaces two full quaternion
multiplies (or a 3x3 build) with two crosses. mu already used it. The rows above
decompose what it is *worth*, and the answer depends on one thing: whether the
quaternion varies.

| situation | ratio | why |
|---|---|---|
| 512 rotations, quaternion varies per call | **2.53x** | cglm's `glm_quat_rotatev` re-normalizes on every call (it accepts unnormalized input by contract): a dot, an rsqrt and a scale, per vector. mu documents "q is normalized" and pays it never. |
| 512 rotations, quaternion loop-invariant | **1.00x** | GCC hoists cglm's normalize out of the loop, both sides do the same ~11 cycles. The algorithmic edge evaporates. |
| 1M points, one quaternion, SoA kernel | **4.2–4.6x** | layout + width + hoisted `u`, `2w`, `2u`. The control row (mu's scalar per-call call in the same loop) is **1.00x**, which pins the win on the layout, not the algebra. |

Two measurement traps this cost, both worth knowing:

1. **A rotating quaternion index (`qa[i % 512]`) defeated vectorization on both
   sides** and silently turned the per-call row into a scalar-latency test
   (2.44x instead of 2.53x, and a 4x shift in absolute ns). Walking the array
   contiguously is the honest per-call loop. Check the disassembly: if the hot
   loop has `vmulss` and no `vmulps`, you are measuring latency, not the kernel.
2. **An isolated harness reported 4.75x for the same code, and was wrong** —
   it stored only `d.x`, so the compiler deleted the `y` and `z` components.
   Measured work must be *all* the outputs; that is the entire reason the bench
   stores three arrays and accumulates a checksum.

The general lesson, which the whole matrix section already says in a different
way: an algorithmic improvement to a per-call kernel is worth ~2.5x here and
evaporates to 1.0x as soon as the optimizer can hoist it; the same improvement
inside a batch kernel with hoisted invariants is worth 4.2x that does not
evaporate, because it is changing the unit of work.

---

## 5. Correctness gate

`make -C perf_test math-test` → **949124 checks, 0 failures**, exit non-zero on
failure. Every kernel is compared against cglm (and, where cglm has no
counterpart, against an independent expectation: `M * M^-1 == I`, affine
`inv_tr` semantics, frustum membership of sampled points).

Backend matrix — the same 949124 checks, all green (the tail sweep on
`mu_quat_rotate_vec3_soa`, every n from 0 to 53, is most of the count):

| build | result |
|---|---|
| `-O3 -march=native` (AVX-512) | PASSED |
| `-O3 -mavx2 -mfma -mno-avx512f` | PASSED |
| `-O2 -msse2 -mno-sse3` (no `hadd`, no SSE4.1) | PASSED |
| `-O3 -DMU_MATH_SCALAR` (portable path) | PASSED |
| `-O1 -fsanitize=address,undefined` | PASSED |

`MU_MATH_SCALAR` exists precisely for this: it forces the portable path on a
machine that has AVX-512, so a SIMD bug shows up as a diff against the scalar
path instead of as a mystery. It found one: `mu_mat4_mul`'s scalar fallback
indexed A by row and produced the **transpose** of the product. It is silent for
symmetric test matrices and wrong everywhere else, and it had been in the tree
since the first version of the file because no test ever ran the scalar path.
That is the argument for the gate running all four builds, not one.

Tolerances: exact ops 1e-6, FMA-contracted 1e-5, fast/rsqrt paths 1e-5,
inverse 1e-4, composed transforms 1e-3. Near-singular inputs are filtered on
`|det| > 0.05` before comparison: mu returns `inf` and cglm `nan` there, and
neither is a contract.

---

## 6. Threats to validity — read before quoting anything

1. **One physical core, SMT2, shared KVM host.** Absolute times drift >20%
   between runs; only paired ratios within a run are claimable. No claim is made
   about multithreaded throughput.
2. **CV is reported, not hidden.** Rows above 10% CV are parity-or-worse by
   default; `mat4_inverse` at 110% means "the statistic is unstable, use the
   isolated harness" (20 reps × 256 matrices × 20 iterations → 0.87–0.93x,
   which agrees with the median here).
3. **`-march=native` on both sides.** cglm gets its AVX2 paths too. A
   distribution build of cglm without `-march` would be slower and the ratios
   would flatter mu. Not claimed.
4. **Per-call benchmarks are L1-resident by construction.** They measure the
   kernel, not a game loop. The batch section is the one that resembles real
   work.
5. **Frequency is measured, not assumed.** `cyc/op` uses the per-run measured
   clock; if the run reports 1.8 GHz the cycles column is wrong by that factor
   and the ratios are unaffected. The ns/op columns are stable across runs to
   ~5%; the cyc/op column is not, because the clock measurement itself has
   ±10% noise on this guest.
6. **Anti-DCE matters.** Every case accumulates a checksum into a volatile sink;
   without it the compiler deletes the loop and you measure 0.006 ns/op. The
   first version of the isolated inverse harness did exactly that.
