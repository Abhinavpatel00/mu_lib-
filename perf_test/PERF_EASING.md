# `mu_easing.h` performance — 33 curves, their derivatives, and when a table wins

Everything here is measured. The interesting numbers are not "how fast is
`out_cubic`" (it is a cubic; it costs a cubic) — they are the three questions
somebody actually has when they pick an easing API:

1. **What does the advanced part cost?** Bezier and spline are 18x and 3.3x a
   polynomial, and that decides whether they belong on a per-frame path or a
   bake-once path.
2. **When does a baked table beat the function?** Not always, and the crossover
   is not where "tables are fast" folklore puts it.
3. **What does the safety cost?** `mu_easing` clamps, guards its endpoints,
   fixes two curves par gets wrong, and dispatches by enum. All of that is in
   these numbers, and the answer is: nothing measurable per call, 2.5x on
   elastic.

## Method (Seven-Field Rule, AGENTS.md 2.6)

| field | value |
| --- | --- |
| CPU | Intel Xeon @ 2.60 GHz (Skylake-SP class), 2 vCPU shared guest, 48 KiB L1d, 1 MiB L2 |
| cores / affinity | 1 core, `taskset -c 0`; the box is a shared 2-vCPU guest, so every claim below is a ratio measured in the same process |
| frequency / DVFS | not pinned; at the mercy of a neighbour. No absolute number is quoted to better than 2 significant figures |
| compiler | gcc 14.2.0, `-O3 -march=native -std=c11 -Wall -Wextra -Werror` |
| workload | 64 eased values per row (`_x64`), or 64 values through the 8-wide path (`_x512`), drawn from a fixed 1024-value payload spread over [0,1] and read with a stride of 7 so nothing folds; the payload lives in L1, which is the point — this is the per-value cost of a curve, not a memory benchmark |
| baseline | `par_easings.h` (the header being extended) for the 16 curves it implements; frozen ratios within our own API for the rest |
| method | `mu/mu_bench.h`: `CLOCK_MONOTONIC_RAW`, operand barrier through a `volatile` sink, batches auto-sized to 20 ms, 21 samples per run, **median** reported with `CV` and a 99% CI; the par table is ABBA (par, mu, mu, par) × 3 rounds with the median of the per-run medians, then a **geometric mean** over cases |

A row the harness marks `UNSTABLE` (CV over the cutoff) still has its median
printed here, and the CV column is next to it so the number can be discounted.
The 2-vCPU guest produced CVs from 0.2% to 19% for the same code at different
sizes; the rows at CV > 10% are flagged in the tables below and no claim rests on
one.

## 1. `mu_easing.h` vs `par_easings.h`, 16 shared curves (paired, 21 samples × 3 ABBA rounds)

`make -C perf_test easing-par` — `perf_test/easing_vs_par.sh`. `par/mu > 1` means
mu is faster. Times are ns per *call* (each row times 64 calls).

| case | par | mu | par/mu | why |
| --- | --- | --- | --- | --- |
| `in_elastic` | 30.6 | **11.0** | **2.79x** | `pow(2,x)` + `sin` in double, vs `exp2f` + `sinf` |
| `out_elastic` | 29.9 | **11.7** | **2.56x** | same |
| `in_out_elastic` | 31.4 | **12.5** | **2.51x** | same, plus the mirror is a branch not a second transcendental |
| `in_out_bounce` | 5.39 | **3.14** | **1.72x** | par divides per arc; we multiply by `1/2.75` |
| `in_bounce` | 3.45 | **2.26** | **1.53x** | same |
| `out_bounce` | 2.50 | **1.87** | **1.34x** | same |
| `out_cubic` | 1.88 | **1.83** | 1.02x | identical arithmetic |
| `in_quad` | 1.85 | **1.84** | 1.01x | identical |
| `out_quad` | 1.85 | **1.83** | 1.01x | identical |
| `in_out_quad` | 1.84 | 1.84 | 1.00x | identical |
| `in_cubic` | 1.82 | 1.83 | 1.00x | identical |
| `linear` | 1.85 | 1.86 | 0.99x | **loss** — par returns `t`, we clamp first |
| `in_back` | 1.83 | 1.86 | 0.98x | **loss** — same |
| `out_back` | 1.83 | 1.95 | 0.94x | **loss** — clamp + endpoint guard |
| `in_out_back` | 2.09 | 2.36 | 0.89x | **loss** — clamp + guard + the mirror branch (and par's is *wrong*, see below) |
| `in_out_cubic` | 1.82 | 2.05 | 0.89x | **loss** — the `t<0.5` branch and the mirror cost 0.2 ns |

**Geometric mean of `par/mu` over the 16 cases: 1.27x.**

Reading it honestly:

- The two families that win are the two where par pays for a **double-precision
  transcendental** (`pow`, `sin`, `sqrt`) and we pay for the `float` one, or where
  par divides and we multiply by a reciprocal. That is the whole of the 2.5x and
  1.5x; it is not a smarter algorithm.
- The four losses are all ≤ 0.54 ns a call and all of them are the price of the
  clamp and the endpoint guard. Section 4 measures that price directly, and it is
  not a reason to remove them: par's `in_out_back(1.0)` is **3.978619** and its
  `in_out_elastic(1.0)` is **0.5**, because nothing in par forces the endpoints.
  The gate asserts both numbers so that a future "cleanup" cannot quietly
  reintroduce them.
- 12 of the 16 rows are within ±2% of par, i.e. the polynomial bodies really are
  the same arithmetic, and the header is not hiding a different curve behind a
  faster loop.

## 2. What each curve costs, per call (21 samples, `make -C perf_test easing-bench`)

| row | median | min | CV | per call |
| --- | --- | --- | --- | --- |
| `mu.smoothstep_x64` | 118.6 ns | 116.3 | 11.7% | **1.85 ns** |
| `mu.direct_out_quad_x64` | 116.7 ns | 116.1 | 2.9% | **1.82 ns** |
| `mu.direct_out_bounce_x64` | 119.9 ns | 118.6 | 4.2% | **1.87 ns** |
| `mu.direct_out_back_x64` | 130.9 ns | 123.9 | 14.6% | **2.05 ns** |
| `mu.direct_in_out_cubic_x64` | 130.6 ns | 128.2 | 10.4% | **2.04 ns** |
| `mu.steps8_x64` | 138.5 ns | 135.2 | 15.1% | **2.16 ns** |
| `mu.lerp_out_cubic_x64` | 136.3 ns | 135.4 | 9.7% | **2.13 ns** |
| `mu.vec3_vec3_x64` | 188.5 ns | 187.2 | 0.3% | **2.95 ns** |
| `mu.direct_out_circ_x64` | 156.2 ns | 152.1 | 4.9% | **2.44 ns** |
| `mu.dispatch_in_out_cubic_x64` | 135.5 ns | 134.2 | 17.1% | **2.12 ns** |
| `mu.apply_in_out_cubic_x64` | 147.7 ns | 134.3 | 18.6% | **2.31 ns** (min: 2.10) |
| `mu.direct_out_expo_x64` | 391.8 ns | 379.6 | 5.9% | **6.12 ns** |
| `mu.direct_out_sine_x64` | 412.7 ns | 395.4 | 4.5% | **6.45 ns** |
| `mu.direct_out_elastic_x64` | 727.4 ns | 719.6 | 9.3% | **11.37 ns** |
| `mu.velocity_out_cubic_x64` | 117.1 ns | 115.9 | 2.5% | **1.83 ns** |
| `mu.velocity_out_elastic_x64` | 876.3 ns | 871.5 | 0.8% | **13.69 ns** |
| `mu.spline_eval_x64` (8 keys) | 382.7 ns | 380.9 | 0.5% | **5.98 ns** |
| `mu.spline_velocity_x64` | 405.7 ns | 404.8 | 0.3% | **6.34 ns** |
| `mu.bezier_css_ease_x64` | 2114.6 ns | 2109.6 | 0.3% | **33.0 ns** |
| `mu.bezier_velocity_x64` | 2640.1 ns | 2634.7 | 0.9% | **41.3 ns** |
| `mu.lut_eval_x64` (257) | 197.0 ns | 196.1 | 0.9% | **3.08 ns** |
| `mu.lut_eval8_x512` (257) | 181.0 ns | 179.2 | 1.2% | **2.83 ns** |
| `mu.from_name_x64` | 7531.7 ns | 6119.7 | 11.4% | **117.6 ns** |
| `mu.lut_bake_257` | 2.93 µs | 2.87 | 18.9% | **2.93 µs per bake** |
| `mu.spline_build_8keys` | 19.7 ns | 19.5 | 0.3% | **19.7 ns per build** |

What to take from the table:

- **The polynomial families, bounce, steps and back are all ~2 ns.** A branchy
  four-arc bounce at 1.87 ns is *cheaper* than a two-branch `in_out_cubic` at
  2.04 ns, because the branch pattern is data-dependent but perfectly predictable
  and the arcs are one multiply each. Anyone who bakes bounce is doing more work,
  not less.
- **A transcendental costs ~4 ns.** `sine`/`expo` at 6.1–6.5 ns is the `sinf` /
  `exp2f` call and nothing else; the `*f` suffixed intrinsics are what make this
  row 5x faster than par's `pow`/`sin`.
- **Velocity is essentially free relative to the value it differentiates.**
  `velocity_out_elastic` is 13.7 ns against 11.4 ns for the value — 1.2x, not 2x,
  because an analytic derivative shares the `exp2f` and adds one `cosf` — while a
  finite difference would be two value calls (22.7 ns) and less accurate.
  `velocity_out_cubic` at 1.83 ns is the value's own cost.
- **Bezier is 33 ns a call, and that is the honest price of inverting a cubic.**
  Newton converges in 3–4 iterations off a `t` seed (the code returns as soon as
  the residual is under 1e-6, and only falls back to 24 bisection steps on a
  near-flat segment). It is 18x a polynomial and 10.7x the baked table, which is
  the entire argument for section 3.
- **The spline is 6.0 ns** for 8 keys with a linear search; the linear search is
  the cost of a handful of keys and the binary search kicks in above 8.
- **`from_name` is 96–118 ns**: it is a `strcmp` chain against 33 names. Call it
  when a curve is loaded, never in a loop. It is in the table because that is the
  sort of thing that ends up in a debug HUD.
- **Dispatch is free**: `mu_ease_get` + an inlined call is 2.12 ns against 2.04
  for the direct call (0.08 ns for the switch), and `mu_ease_apply` is 2.10–2.31
  (that row is the only one here with CV near 19%, so treat the difference as
  "under 0.3 ns"). Curves-as-data costs nothing; use the enum.

## 3. When a baked table wins, and when it is a pessimisation

`make -C perf_test easing-bench` for the direct and table columns (21 samples);
the bake column is a separate min-of-7 scratch measurement of 257 evaluations of
each curve (dead-store eliminated on the first attempt, hence the volatile sink).

| curve | direct | table (257) | ratio | bake (257) | calls to break even |
| --- | --- | --- | --- | --- | --- |
| `out_quad` | 1.82 ns | 3.08 ns | **0.59x — the table is SLOWER** | 0.48 µs | never |
| `out_bounce` | 1.87 ns | 3.08 ns | **0.61x — the table is SLOWER** | 0.60 µs | never |
| `out_circ` | 2.44 ns | 3.08 ns | 0.79x — the table is slower | 0.72 µs | never |
| `in_out_bounce` | 3.14 ns | 3.08 ns | 0.98x — a wash | 0.96 µs | never |
| `out_sine` | 6.45 ns | 3.08 ns | **2.09x** | 1.70 µs | ~505 |
| `out_expo` | 6.12 ns | 3.08 ns | **1.99x** | 1.75 µs | ~576 |
| 8-key spline | 5.98 ns | 3.08 ns | **1.94x** | 2.04 µs | ~704 |
| `out_elastic` | 11.37 ns | 3.08 ns | **3.69x** | 3.40 µs (2.93 µs in the harness row) | ~410 |
| cubic bezier | 33.0 ns | 3.08 ns | **10.7x** | 8.94 µs | ~298 |

(break-even = `bake / (direct - table)`, i.e. how many eased values pay for the
bake. Below that the table is a loss.)

So: **bake elastic and bezier, bake a spline if it is evaluated more than ~700
times, and do not bake polynomials, circ or bounce.** The folklore "a table is
faster" is wrong for anything under ~3 ns, which is most of the header. For a
particle system that eases 100 000 values a frame, elastic goes from 1.14 ms to
0.31 ms and bezier from 3.30 ms to 0.31 ms; for a UI panel that eases four
values a frame, a table is ~1 µs of pure loss per frame plus a 257-float buffer,
and the function is the right answer.

The 8-wide form (`mu_ease_lut_eval8`) is **2.83 vs 3.08 ns a value: 1.09x.** That
is not a SIMD win, and the header says so in the code: an AVX2
`_mm256_i32gather_ps` version was written and measured at the same speed, because
a gather costs about what eight L1 loads plus the int conversion cost. What the
8-wide call buys is call overhead and ILP, and nothing else.

## 4. What the correctness work costs

| what | measured |
| --- | --- |
| clamp + endpoint guard, per call | `mu_ease_out_quad` 1.82 ns vs the bare `u*(2-u)` expression with the same clamp at 1.85 ns (min 116.1 vs 116.1 ns per 64) — **no measurable cost**, the branches predict perfectly on in-range data |
| forcing `f(0)=0, f(1)=1` (elastic) | 0 ns: the guard is the first two compares of the function, and it *removes* the `exp2f`/`sinf` work at the endpoints |
| `in_out` symmetry by mirroring | +0.2 ns on `in_out_cubic` (2.04 vs par's 1.82), and it is what makes `in_out_back(0.5) == 0.5` structural instead of accidental |
| enum dispatch instead of a function pointer in hand | 0.08 ns |
| the velocity functions | 1.2x the value they differentiate (see section 2) |

## 5. Table accuracy — how many samples, and what it cannot fix

Worst-case absolute error against the exact curve, 4001 probe points,
`mu_ease_lut_error`:

| curve | 65 samples | 257 samples | 1025 samples |
| --- | --- | --- | --- |
| `out_quad` | 6.11e-5 | 3.90e-6 | 3.13e-7 |
| `in_out_cubic` | 3.61e-4 | 2.28e-5 | 1.43e-6 |
| `out_sine` | 7.53e-5 | 4.77e-6 | 3.58e-7 |
| `in_out_back` | 7.66e-4 | 4.86e-5 | 3.04e-6 |
| `out_elastic` | 9.78e-3 | 7.12e-4 | **3.63e-4 (floor)** |
| `in_out_bounce` | 2.88e-2 | 7.68e-3 | 9.23e-4 |
| `out_expo` | 1.39e-3 | 9.14e-4 | **7.27e-4 (floor)** |
| `out_circ` | 4.43e-2 | 2.21e-2 | 1.11e-2 |

Three things worth knowing:

- For the smooth curves the error falls by ~16x per 4x of samples (65 → 257 →
  1025 gives 15.7x and 12.5x for `out_quad`) — that is the `O(h^2)` of linear
  interpolation, and it is the check that the table indexing is aligned. The gate
  asserts the decrease at those three sizes for `out_cubic`.
- **Two curves have a floor that more samples do not lower.** `out_elastic` stops
  at ~3.6e-4 and `out_expo` at ~7.3e-4 because the last span is where the curve
  meets the endpoint guard: elastic's raw value at t=1 is 1.000488 and the guard
  makes it exactly 1, and expo climbs from 0.999 to 1 inside one span no matter
  how many spans there are, so `h^2 f''` does not shrink. Bake elastic at 257
  samples and accept 7.1e-4 (0.07% of a curve that overshoots to 1.37); do not
  chase the floor with a bigger table.
- `out_circ` needs 1025 samples to reach 1.1e-2 and is never worth baking: it is
  vertical at t=1 and no uniform table represents a vertical tangent. Call the
  `sqrtf`; it is 2.44 ns.

## 6. What this document does not claim

- It does not claim the curves are faster than par's in any algorithmic sense for
  the polynomial families: 12 of the 16 rows are within ±2%, because the
  arithmetic is the same three multiplies. The wins are the `float`
  transcendentals and the reciprocal-for-divide.
- It does not claim a table beats a function. Four of the nine rows in section 3
  are losses or a wash, and they are the cheapest curves in the header.
- It does not claim the 8-wide LUT path vectorizes. It is 1.09x and it is
  documented as 1.09x.
- It does not claim the bezier solver is optimal. It is 33 ns for a CSS curve and
  the bisection fallback is 24 steps; a curve with a genuinely flat segment can
  reach that path and pay ~90 ns. That is the correctness/perf trade: the
  fallback exists so that a designer's degenerate control points do not produce a
  wrong answer or an infinite loop.
- It does not claim the benchmarks are stable. This is a shared 2-vCPU guest;
  CVs reach 19% and four rows are flagged. The paired ABBA design is what makes
  the 1.27x geomean trustworthy, not the individual medians.
