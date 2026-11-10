# `mu_easing.h` — 33 easing curves as data, with derivatives and design-tool curves

A single-header C99 extension of [`par_easings.h`](https://github.com/prideout/par)
(Robert Penner's easing functions). Same curves, same feel, prefix rename only —
plus the five things that make them hard to actually ship: the curves par
implements but does not declare, the two it gets wrong, the endpoints it does not
reach, the input it does not clamp, and the derivatives it does not have.

```c
#include "mu/mu_easing.h"          /* needs mu_macros.h, mu_math.h, mu_simd.h */

float a = mu_ease_out_cubic(t);                     /* the par call, renamed       */
mu_vec3 p; mu_ease_vec3(p, MU_EASE_OUT_BACK, p0, p1, t);   /* out-param, like mu_math.h */
float v = mu_ease_velocity(MU_EASE_OUT_ELASTIC, t); /* exact f'(t), not a difference */
```

A port is a prefix swap, family tails unchanged:

```
par_easings_in_out_cubic(t)  ->  mu_ease_in_out_cubic(t)
par_easings_out_bounce(t)    ->  mu_ease_out_bounce(t)
```

`mu_ease_from_name` accepts all three spellings (`in_out_cubic`,
`mu_ease_in_out_cubic`, `par_easings_in_out_cubic`), so a config file written
against the old header keeps loading.

No allocation, no global state, no `IMPLEMENTATION` macro. Every public symbol is
`MU_INLINE`, so `MU_STATIC` / `MU_API` apply as everywhere else in `mu/`.

## 1. What is different from `par_easings.h`, and why each difference is a fix

| | `par_easings.h` | `mu_easing.h` |
| --- | --- | --- |
| curves you can call | 16 | **33** (`linear`, ten families × in/out/in_out, `smoothstep`, `smootherstep`) |
| quart, quint, sine, expo, circ | implemented as `par_easings__in_circ` & co. and **never declared** | public: `mu_ease_in_out_circ` etc. |
| `in_out_back(0.5)` | **1.500000** | 0.5 |
| `in_out_back(1.0)` | **3.978619** | 1.0 |
| `in_out_elastic(1.0)` | **0.500000** | 1.0 |
| `in_elastic(0)` | -0.000488 | 0.0 |
| `out_elastic(1)` | 1.000488 | 1.0 |
| `in_quad(NaN)` | NaN | 0.0 |
| `in_bounce(1.5)` | -0.890625 | 1.0 (clamped) |
| derivatives | none | `mu_ease_velocity` for all 33 kinds |
| curves as data | none | `mu_ease_kind`, `mu_ease_get`, `mu_ease_name`, `mu_ease_from_name` |
| parametric | none | `mu_ease_{in,out,in_out}_back_amt(t, s)`, `..._elastic_p(t, period)` |
| design-tool curves | none | cubic Bezier (CSS `cubic-bezier`), monotone spline |
| discrete | none | `mu_ease_steps` |
| baked tables | none | `mu_ease_lut` (caller-owned buffer) |
| batching | none | `mu_ease_lut_eval8` |
| transcendentals | `pow`, `cos`, `sin`, `sqrt` (double, per call) | `exp2f`, `cosf`, `sinf`, `sqrtf` |

The three upstream bugs are measured, not suspected, and the gate asserts both
sides of each so that "simplifying" our copy back to par's numbers fails a test:
`in_out_back` multiplies `s` by 1.525 twice and drops a `t²`;
`in_out_elastic` forgets to halve the time axis, so its two halves are only ever
visited one way.

### The construction that makes the invariants structural

Nothing above is patched with a special case. Every `out` and `in_out` curve is
**defined** as a mirror of its `in` half:

```
out(t)    = 1 - in(1-t)
in_out(t) = t < 0.5 ? 0.5*in(2t) : 1 - 0.5*in(2-2t)
```

so `out(t) == 1 - in(1-t)` and `in_out(t) + in_out(1-t) == 1` hold to within a
couple of ULPs *by construction*, and `in_out(0.5) == 0.5` because both branches
agree there. The catch, stated plainly: our `in_out_back` overshoots like its `in`
half (5%), not like Penner's intent (10%). If you want the old look, pass a bigger
`s` to `mu_ease_in_out_back_amt`.

The endpoints are forced by one helper, `mu_ease__exact(t, v)`: if `t <= 0` return
0, if `t >= 1` return 1, else `v`. That is what makes elastic and expo *land*
instead of arriving 0.05% short, and it costs two compares (`in_back` and
`in_out_back` return `-0.0f` at `t == 0`, which compares equal to zero; the closed
form produces it and the gate checks it with `fabs`).

## 2. The 33 kinds, and the contract each one keeps

`mu_ease_kind` is laid out family-major (`MU_EASE_IN_QUAD`, `OUT_QUAD`,
`IN_OUT_QUAD`, `IN_CUBIC`, ...) so `mu_ease__family == (k-1)/3 + 1` and
`mu_ease__phase == (k-1)%3` are cheap index math rather than a lookup. `linear`
is kind 0; `smoothstep` / `smootherstep` are the last two; `MU_EASE_COUNT`
iterates.

Every curve, asserted by `perf_test/test_easing.c` (586 checks):

- `f(0) == 0`, `f(1) == 1` (within 1e-7, and bit-exact for 31 of 33)
- `f(NaN)` is finite; `f(t<0) == f(0)`; `f(t>1) == f(1)`
- stays inside `[-0.40, 1.40]` — only elastic goes past `[0,1]`, and only to
  `[-0.3731, 1.3731]`
- the ten monotone families are monotone over 2000 samples
- every `in_out` satisfies `f(t) + f(1-t) = 1` (2e-6) and `f(0.5) = 0.5`
- every `out` satisfies `out(t) = 1 - in(1-t)` (2e-6)

`back`, `elastic` and `bounce` are the only curves that leave `[0,1]`, by design.

## 3. Velocity — the exact derivative, not a difference

```c
float v = mu_ease_velocity(MU_EASE_OUT_ELASTIC, t);   /* f'(t), analytic */
```

One function, a switch over `(family, phase)`, with the `in` derivative for each
family hand-derived and the `out` / `in_out` phases composed from it by the same
mirror rules as the values (`out'(t) = in'(1-t)`; `in_out'(t) = in'(2t)` — the
factors of two cancel against the halving). `in_circ`'s vertical tangent at `t=1`
is reported as `1e12`, a large finite number, because `INFINITY` in a spring or a
trail is worse than a clamp.

The gate cross-checks all 33 against a Richardson-extrapolated central difference
(≤ 1e-3 relative; ≤ 4e-5 for 30 of them) and then checks the `in_out` seam
*analytically* — `in_out_quad'(0.5) == 2`, `in_out_cubic'(0.5) == 3`,
`in_out_sine'(0.5) == π/2` — because that is exactly where a missing chain rule
hides and where a difference quotient is useless.

Why it is worth having: a spring, a velocity-matched transition, a motion-blur
shutter angle and a trail all want `f'(t)`. Finite-differencing a UI curve to get
it is where the jitter comes from. It is also cheap: `velocity_out_elastic` is
13.7 ns against 11.4 ns for the value (1.2x), because it shares the `exp2f` and
adds one `cosf`, where a difference would be two value calls.

## 4. Curves as data (§4)

```c
mu_ease_kind k = mu_ease_from_name("out_bounce");    /* MU_EASE_COUNT if unknown */
const char  *n = mu_ease_name(k);                    /* "out_bounce"             */
mu_ease_fn   f = mu_ease_get(k);                     /* float (*)(float)         */
float        v = mu_ease_apply(k, t);                /* same, without the pointer */
```

That is what a saved curve, a `--ease=` CLI flag, a UI dropdown and a
`for (k = 0; k < MU_EASE_COUNT; ++k)` test all need. Names are unique and
round-trip (asserted); lookup degrades to `linear` for an out-of-range enum, so a
corrupt value means "no animation" instead of a null call.

Dispatch is free — 2.12 ns against 2.04 ns for a direct call. `mu_ease_from_name`
is a `strcmp` chain and costs 96–118 ns: call it when you load the curve.

## 5. Cubic Bezier (§6) — the curve a designer hands you

```c
mu_ease_bezier b = mu_ease_bezier_make(0.25f, 0.1f, 0.25f, 1.0f);  /* CSS "ease" */
float v = mu_ease_bezier_eval(&b, t);
float d = mu_ease_bezier_velocity(&b, t);

mu_ease_bezier css;
if (mu_ease_bezier_css("ease-in-out", &css)) { /* the five CSS keywords */ }
```

The parameter is the x axis, so evaluating means inverting a cubic: Newton from
`s = t`, exit when the residual is under 1e-6 (3–4 iterations for the curves tools
emit), and a 24-step bisection if Newton ever leaves `[0,1]` or the slope
vanishes. That fallback is why a degenerate control polygon cannot hang the
solver or return a wrong answer — and why a flat-segment curve costs ~90 ns
instead of 33.

`x1`/`x2` are clamped into `[0,1]` at construction: outside that range `x(s)` is
not monotone and there is no function to return. `y1`/`y2` are free, which is how
overshoot is expressed. Solved value is checked against an independent bisection
in the gate; CSS `ease(0.5)` is checked against the published 0.8024.

33 ns a call. Bake it if you evaluate more than ~300 times (see §9).

## 6. Monotone spline (§7) — "here is my curve"

```c
mu_ease_key keys[] = { {0.0f, 0.0f}, {0.3f, 0.8f}, {1.0f, 1.0f} };
float slopes[3];                                    /* caller-owned scratch */
mu_ease_spline sp;
mu_ease_spline_build(&sp, keys, 3, slopes);

float y = mu_ease_spline_eval(&sp, 0.5f);
float d = mu_ease_spline_velocity(&sp, 0.5f);
```

Fritsch–Carlson tangents: the secants are averaged at the interior keys, then
limited so that `(α, β)` stays inside the monotonicity circle (`m_i = τ·α·δ_i`
with `τ = 3/sqrt(α²+β²)` when `α²+β² > 9`), and a zero secant forces both of its
tangents to zero. A Catmull-Rom through the same keys would be shorter to write
and would **overshoot** — a UI curve would fly off the edge of the panel — which
is the whole reason for the limiter. The gate asserts that monotone data gives a
monotone interpolant with no overshoot, that the keys are interpolated exactly,
that the velocity at a key is the stored slope, and that flat data stays flat.

- `count >= 2`, times strictly increasing, or `build` returns 0 and leaves an
  **empty** spline (every consumer can still call `eval` and gets the clamped
  identity rather than reading stack).
- Outside the key range the value clamps and the velocity is 0: no extrapolation.
- Linear search below 8 keys, binary above.
- 6.0 ns a call for 8 keys; a 257-point bake costs 2.0 µs.

## 7. Steps (§8)

```c
float v = mu_ease_steps(t, 8, 0);   /* 8 discrete levels, CSS jump-end */
```

`count < 1` returns the clamped identity; `t == 1` is exactly 1.0; exactly `count`
jumps happen in `[0,1)` (asserted). `jump_at_start` is CSS's `jump-start`.

## 8. Baked tables (§9) — and when NOT to use one

```c
float table[257];
mu_ease_lut lut;
mu_ease_lut_bake_kind(&lut, table, 257, MU_EASE_OUT_ELASTIC);
float v = mu_ease_lut_eval(&lut, t);          /* multiply, truncate, lerp */
float s = mu_ease_lut_velocity(&lut, t);      /* the segment's slope      */
float e = mu_ease_lut_error(&lut, 4001);      /* worst |table - curve|    */
```

`samples` counts **entries**, so `t=1` is `table[samples-1]`; 257 is the usual
choice (256 spans plus a top entry, no special case at the boundary). The buffer
is yours, the `mu_ease_lut` is a POD pointer + size + source function, and a
failed bake leaves it empty and safe.

Measured crossover (`perf_test/PERF_EASING.md` §3):

| curve | direct | table (257) | verdict |
| --- | --- | --- | --- |
| `out_quad` | 1.82 ns | 3.08 ns | **baking it makes it slower** |
| `out_bounce` | 1.87 ns | 3.08 ns | **slower** |
| `out_circ` | 2.44 ns | 3.08 ns | slower |
| `out_sine` | 6.45 ns | 3.08 ns | 2.1x, break-even ~505 calls |
| `out_expo` | 6.12 ns | 3.08 ns | 2.0x, break-even ~576 |
| 8-key spline | 5.98 ns | 3.08 ns | 1.9x, break-even ~704 |
| `out_elastic` | 11.37 ns | 3.08 ns | **3.7x, break-even ~410** |
| cubic Bezier | 33.0 ns | 3.08 ns | **10.7x, break-even ~300** |

Bake elastic, Bezier and splines. Do not bake polynomials, circ or bounce. A UI
panel that eases four values a frame should call the function; a particle system
that eases 100 000 should bake (elastic 1.14 ms → 0.31 ms per frame).

Accuracy: for smooth curves the error falls ~16x per 4x of samples (65 → 257 →
1025 gives 3.9e-6 for `out_quad`, 4.8e-6 for `out_sine`), which is the `O(h²)` of
linear interpolation. Two curves **floor** and no table size fixes them:
`out_elastic` at ~3.6e-4 and `out_expo` at ~7.3e-4, because the last span is
where the curve meets the endpoint guard. `out_circ` needs 1025 samples for
1.1e-2 and is never worth baking — it is vertical at `t=1`, and no uniform table
represents a vertical tangent.

`mu_ease_lut_eval8` takes and returns `mu_f32x8`: **1.09x** the scalar call,
measured. It is eight scalar lookups on purpose — an AVX2 `i32gather` version was
written and timed at the same speed. It buys call overhead and ILP, not width.

## 9. Time helpers and application (§10)

```c
float l  = mu_ease_loop(t);              /* wrap into [0,1)        */
float pp = mu_ease_pingpong(t);          /* 0 -> 1 -> 0 per period */
float u  = mu_ease_remap(t, 0.2f, 0.8f); /* -> [0,1]               */
float a  = mu_ease_lerp(kind, 3.0f, 9.0f, t);
mu_vec3 v; mu_ease_vec3(v, kind, from, to, t);   /* what most call sites want */
```

`remap` with an empty range returns 0; `loop`/`pingpong` handle negative and
NaN input.

## 10. Honest limits

- **`in_out_back` overshoots by 5.0%, not 10%**, because it is the mirror of a
  correct `in` half (measured peak 1.0500020). Pass `s = 2.6016f` to
  `mu_ease_in_out_back_amt` for a 10% peak if the classic look matters more than
  the invariant.
- **`out_elastic` overshoots to exactly the same peak par does** — both measured
  at 1.3730980 — but it now *ends* at 1.0 instead of par's 1.000488, and
  `in_elastic` starts at 0 instead of -0.000488. The deviation is the endpoints,
  not the shape.
- **Bezier costs 33 ns and the gate only guarantees 1e-3 against an independent
  bisection.** If you need a designer curve at 60 Hz on every vertex, bake it.
- **The spline does not extrapolate**, in value or velocity. Clamping is the
  documented behaviour; if you need an extension, append a key.
- **`mu_ease_velocity(in_circ, 1.0)` is 1e12**, not infinity. It is a sentinel, and
  a consumer that multiplies by a large dt will notice.
- **The 8-wide path is 1.09x**, and the header says so twice.
- The t table in `PERF_EASING.md` was measured on a shared 2-vCPU guest with CVs
  up to 19%; the ratios are the trustworthy part, not the absolute nanoseconds.

## 11. Testing

```sh
make -C perf_test easing-test      # 586 checks; par parity included if ../par_easings.h exists
make -C perf_test easing-bench     # per-call costs, 21 samples, pinned
make -C perf_test easing-par       # paired ABBA vs par_easings.h, geomean 1.27x
```
