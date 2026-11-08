# Noise, and the end of the cglm dependency

Two changes, one small and one structural.

The small one: `mu_noise_math.h` gained the members of the
[SimplexNoise](https://github.com/simongeilfus/SimplexNoise) feature set it was
missing, and got the correctness gate it never had.

The structural one: **nothing in `mu/` includes a third-party header any more.**
`mu_geometric_algebra.h` no longer requires cglm (it runs on `mu_math.h`),
`mu_roguelike.h` no longer probes for cglm, and `mu_noise_math.h` no longer
carries its own private copy of dot/add/cross. There is one math layer, and it
is ours.

---

## 1. What the library looks like now

| was | is |
|---|---|
| `mu_geometric_algebra.h` required cglm (`vec2s`/`vec3s`/`vec4s`, `glms_vec3_*`, `versors`, `CGLM_USE_ANONYMOUS_STRUCT` for C99) | uses `mu_vec2/3/4`, `mu_mat3`, `mu_mat4`, `mu_quat`, `mu_vec3_dot/cross/add/sub/scale` from `mu_math.h`. No external include, no config macro dance. |
| `mu_roguelike.h` did `__has_include(<cglm/cglm.h>)` and used `glm_vec2_distance` in one function | plain `sqrtf`; the include block is gone |
| `mu_noise_math.h` defined `mu_noise_vec2/3/4` **and** its own dot/add/sub/cross/normalize | those are now aliases of `mu_vec2/3/4`, and the helpers forward to `mu_math` |
| `mu_perf.h`, `mu_vm.h` needed `_POSIX_C_SOURCE` to be defined by the caller | `mu_macros.h` defines it once, before the first system header of the family (glibc locks its feature set at `<stddef.h>`, so anything later is ignored — this is why it moved from `mu_common.h`) |
| `mu_line_array.h` declared two API functions *outside* `MU_BEGIN_EXTERN_C` | declarations and definitions share C linkage (it broke as the first include of a C++14 TU); one dead local removed |

### The gate: `make -C perf_test headers-test`

`perf_test/check_headers.sh` does two things:

1. greps `mu/*.h` for `#include <cglm/…>` or `#include <glm/…>` and fails if it
   finds one. `perf_test/bench_math_vs_cglm.c` may still include cglm — it is a
   *benchmark*, not part of the library, and comparing against the baseline is
   its whole job.
2. compiles **every** header as the first include of an empty translation unit,
   in C99, C11 and C++14, with `-Wall -Wextra -Werror` and no cglm on the
   include path — and then the umbrella `mu.h` the same way. The umbrella is not
   redundant: it includes `mu_macros.h` first, so it reaches `<stddef.h>`
   *before* `mu_common.h` has a chance to define `_POSIX_C_SOURCE`, and glibc
   locks its feature set at that point. That is exactly how the umbrella build
   broke while every individual header passed.

```
check_headers: no cglm/glm include in mu/*.h
check_headers: every header stands alone (C99, C11, C++14)
check_headers: the umbrella mu.h, same three modes
check_headers: 82 headers, 0 failure(s)
PASSED: library has no cglm dependency and every header stands alone
```

That second check is worth more than it looks: it is what found the
`_POSIX_C_SOURCE` leaks and the `extern "C"` bug above — three headers that only
compiled because some *other* header had been included first.

---

## 2. Noise: what was adopted, and what was refused

Checked function by function against SimplexNoise (`noise`, `dnoise`,
`ridgedNoise`, `worleyNoise`, `flowNoise`, `dFlowNoise`, `curlNoise`, `fBm`,
`worleyfBm`, `dfBm`, `ridgedMF`, `iqfBm`, `iqMatfBm`, `curl`, `seed`).

### Added

| function | what it is, and why it was missing |
|---|---|
| `mu_noise_simplex1` | 1D simplex noise. We had 2D/3D/4D only. Normalised by `3.1604938` = 1/0.316406, the **measured peak** of one cell of `(1-x²)⁴·x + (1-(x-1)²)⁴·(x-1)` over all sign combinations. SimplexNoise (and Gustavson's notes) use `0.395`, which is the 2D constant: it leaves 1D output in ±0.13, i.e. 87% of the amplitude silently missing. |
| `mu_noise_simplex1_deriv` | value + exact derivative. For `(1-x²)⁴·g·x` the slope is `g·(1-x²)³·(1-9x²)`, two extra multiplies per corner — no second evaluation, no step size to tune. |
| `mu_noise_flow2`, `mu_noise_flow2_sc` | gradient-rotated simplex, 2D. Rotate every corner gradient by `angle` before the dot product; the lattice, hashes and kernel are unchanged, so it costs the same as `simplex2` (measured below) but the iso-lines run along the angle. `_sc` takes pre-computed sin/cos for callers in a loop. |
| `mu_noise_flow3`, `mu_noise_flow3_axis` | 3D flow noise: rotation about an arbitrary axis (SimplexNoise only rotates about Z; an axis is the honest general case). |
| `mu_noise_dfbm1/2/3` | fBm carrying the **analytic** derivative through the octave sum, `d/dp Σ aᵢ n(fᵢp) = Σ aᵢ fᵢ n'(fᵢp)`. Their `dfBm`. `dfbm1` falls back to a central difference of the sum (our 1D value/perlin/simplex samplers return floats, not gradients) and says so. |
| `mu_noise_iq_fbm2/3` | Quilez's variant: no `gain`, the accumulated derivative damps each octave by `1/(1+\|d\|²)`, with a 2× rotate-and-scale between octaves. Their `iqfBm` — which has a typo (`dx += d.y` for `dx += d.x`, and `d.z*d.z` where it means `dz*dz`); the version here is the formula, not the typo. |
| `mu_noise_iq_rot_fbm2` | fBm with a rotation-*and*-scale between octaves so the lattice axes never align. Their `iqMatfBm`, with the rotation an angle instead of a `mat2`. |
| `mu_noise_fbm4`, `turbulence4`, `billow4`, `ridged4` | the 4D members of the fractal family (bases already existed). |
| `mu_noise_worley2_falloff`, `worley3_falloff` | smooth cellular: `-(1/k)·ln((1/N)Σ exp(-k·dₙ))` over the 27/9 neighbours. `k→0` gives the mean neighbour distance (blobs), `k→∞` gives F1 (hard cells). Their version omits the `1/N`, which makes the result dive toward −∞ as `k` shrinks and confines the useful range of `k` to a narrow band. |

### Refused, with reasons

- **`perm[512]` + global `seed()`.** Every function here takes a seed and hashes
  it, so there is no global state to seed and no shared table to cache-coherency
  against in a multithreaded caller.
- **`dFlowNoise`.** The analytic derivative of the rotated field is the rotated
  field's derivative tensor — a second derivation for a function used for
  advection, where the derivative is not consumed. `mu_noise_sample2_fd` gives a
  central difference of any scalar field for the cases that need one.
- **`curl()`.** Already covered by `mu_noise_curl2/curl3` plus
  `mu_noise_divergence*_fd` / `laplacian*_fd`, which take any function pointer
  rather than a specific noise type.

---

## 3. What the gate checks (`make -C perf_test noise-test`)

`perf_test/test_noise.c`, 4594 checks. The interesting ones:

- **range and finiteness** for every family on a 37×37 grid — catches a
  normalisation constant that drifted and any division by zero;
- **determinism** and **seed sensitivity** (same seed ⇒ same float, different
  seed ⇒ different field);
- **continuity**: a step of `1e-4` may not change the value by more than `5e-3`,
  including explicitly across a lattice cell boundary where a bad hash shows up
  as a seam;
- **analytic derivatives vs central differences** for `simplex1_deriv`,
  `simplex2_deriv` and `dfbm2` — the check that proves the derivative is the
  derivative of *this* function;
- **identity cases**: `flow2(x,y,0) == simplex2(x,y)` **bit for bit**,
  `flow2(x,y,π) == -simplex2(x,y)` exactly, `flow3(…,0) == simplex3` within
  1e-5, `worley2_falloff` → F1 at high `k`, `iq_rot_fbm2` with one octave at
  scale 1 / angle 0 == the base noise;
- **interop**: `mu_vec2_length()` applied straight to a noise vector, and
  `sizeof` equality — if someone re-adds a private vector struct, this stops
  compiling.

One test I wrote first was wrong and is worth recording: "flow noise rotated by
90° equals the field sampled at the rotated coordinate" is **false**, because
rotating the gradients does not rotate the lattice. Rotating by π does negate
the field exactly, and that is the invariant the gate asserts.

---

## 4. Cost

`taskset -c 0 ./bench_noise --samples=21 --confidence=6`, `-O3 -march=native`,
Xeon @2.60 GHz, 2 vCPU sandbox. ns per sample, median of 21:

| row | median | CV |
|---|---:|---:|
| `simplex.s1` | 20.1 | 8.2% |
| `simplex.s2` | 57.3 | 29.2% (noisy core, see note) |
| `simplex.s3` | 103.2 | 5.3% |
| `simplex.s4` | 172.6 | 4.1% |
| `simplex.s1_deriv` | 24.2 | 9.1% |
| `flow.flow2` | 58.3 | 6.2% |
| `flow.flow2_precomputed_trig` | 48.5 | 9.1% |
| `flow.flow3` | 103.8 | 4.2% |
| `fbm.base_simplex2_1oct` | 58.2 | 6.7% |
| `fbm.fbm2_5oct` | 461.9 | 3.9% |
| `fbm.dfbm2_5oct` | 474.7 | 18.1% |
| `fbm.iq_fbm2_5oct` | 522.9 | 0.9% |
| `fbm.fbm4_5oct` | 1145 | 7.4% |
| `cellular.worley2_f1` | 147.8 | 12.0% |
| `cellular.worley2_falloff` | 312.0 | 27.0% |

Reading it honestly:

- **`flow2` costs 1.02× `simplex2`** (58.3 vs 57.3 ns) — the rotation is three
  multiplies and two adds per corner, not a second noise evaluation. Hoisting
  sin/cos with `_sc` is worth 17% if you are sampling a field.
- **`dfbm2` costs 1.03× `fbm2`** for five octaves (474.7 vs 461.9 ns): the
  derivative rides along for free because the octave loop was already the cost.
- **5 octaves is 7.9× one sample** — the fractal is the octave loop, not the
  noise kernel; if that ratio matters, lower the octave count before optimising
  the base.
- **`fbm4_5oct` is 2.5× `fbm2_5oct`**, which is the 4D kernel's corner count,
  not a defect.
- `worley2_falloff` is **2.1×** `worley2_f1` because the exponential and the
  `log` are real work; you pay it once per texel, not per octave.

Caveats, stated rather than buried: the sandbox has 2 vCPUs and no frequency
control, so CVs above ~10% (`s2`, `dfbm2`, `worley2_*`) mean the row is noisy on
this machine, median-of-21 is the honest number, and the *ratios between rows
inside one run* are more trustworthy than the absolute ns. There is no
cross-library claim here — SimplexNoise is C++/glm and is not vendored, so
nothing in this table is a "we beat them" number.

---

## 5. Migration notes

- `mu_noise_vec2/3/4` are now **the same types** as `mu_vec2/3/4`. Code that
  passed them to `mu_*` maths compiles unchanged; code that relied on
  `mu_noise_vec4` *not* being 16-byte aligned does not (same 16-byte alignment
  as `mu_vec4`).
- `mu_noise_v3_mul(a, s)` is still a scalar scale (it maps to
  `mu_vec3_scale`), unlike `mu_vec3_mul(a, b)`, which is component-wise.
- `mu_geometric_algebra.h`: no `MU_GA_CGLM_HEADER`, no
  `CGLM_USE_ANONYMOUS_STRUCT`. `vec3s` → `mu_vec3`, `mat4s` → `mu_mat4`,
  `versors` → `mu_quat`.
- `mu_roguelike.h`: `MU_ROG_CGLM_HEADER` and `MU_ROG_HAS_CGLM` are gone; nothing
  to define, nothing to guard.
- `mu_noise_simplex1` output is in [-1,1], not the ±0.13 you get from copying
  Gustavson's 2D constant into a 1D function.
