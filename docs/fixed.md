# `mu/mu_fixed.h` — Q16.16 fixed point and trig without libm

```c
#define MU_FIXED_WANT_FLOAT_SQRT 1      /* optional: lets sqrt use the FPU     */
#include "mu/mu_fixed.h"

mu_ang a = mu_ang_from_radians(MU_PI_Q16);   /* half a turn, in turns      */
mu_q16 s, c;
mu_q16_sincos(a, &s, &c);                    /* (-1, 0) in Q16.16          */
```

## Why it exists

Float is right almost everywhere in this library. It is the wrong tool when:

* **the answer must be identical everywhere** — replays, lockstep, a server
  that has to agree with a client. `sinf`/`cosf`/`atan2f` are allowed to differ
  by an ulp between libms and between versions of one libm, and the divergence
  compounds. Everything in this header is integer arithmetic, so the same
  inputs give the same bits on any machine with 32-bit two's complement ints.
* **there is no FPU**, so every float op is a soft-float call.
* **the value must be 4 bytes you can hash, compare and send**, which a float
  whose low bits you care about is not.

## The one design decision that matters

Angles are `mu_ang`: a Q16.16 count of **turns**, so a full turn is 65536 and
wraps for free. Range reduction is `>> 14` and `& 3`; there is no pi
multiplication and no branch in the hot path. A `mu_ang` is directly usable as a
phase accumulator.

## What is in it

| group | functions |
| --- | --- |
| types & conversion | `mu_q16`, `mu_ang`, `MU_Q16_ONE`, `MU_PI_Q16`, `mu_q16_from_i32`, `mu_q16_to_i32` (floors), `mu_q16_round_i32`, `mu_q16_from_f32[_round]`, `mu_q16_to_f32` |
| arithmetic | `mu_q16_add/sub` (wrap), `_sat` variants, `mul`, `div`, `mul_i32`, `div_i32`, `lerp`, `abs`, `neg`, `min`, `max`, `clamp`, `sign` |
| roots | `mu_u64_sqrt` (exact floor), `mu_q16_sqrt` (rounded), `mu_q16_rsqrt`, `mu_q16_sqrt_f32` (opt-in, FPU) |
| angles | `mu_ang_from_radians`, `mu_ang_to_radians`, `mu_ang_from_degrees_i32`, `mu_ang_to_degrees`, `mu_ang_wrap` |
| trig | `mu_q16_sin`, `mu_q16_cos`, `mu_q16_sincos`, `mu_q16_tan`, `mu_q16_atan2`, `mu_q16_asin`, `mu_q16_acos`, `*_rad` wrappers, and `mu_q16_sincos_cordic` (no table) |

## Accuracy (measured, not claimed)

| function | worst error | how measured |
| --- | --- | --- |
| `sin`, `cos` | **1 LSB** (1.5e-5) | every angle in ±1 turn, 131 072 points, against `double` |
| quadrant points | **exact** | sin(0)=0, cos(0)=1, sin(90°)=1, cos(180°)=-1 |
| `atan2` | **1 LSB** | 2 000 000 random pairs + a 41×41 grid |
| `asin`, `acos` | **1 LSB** | the whole domain, 65 537 inputs |
| `mul`, `div`, `sqrt` | correctly rounded / 1 LSB | 161×161 grid, `sqrt` exhaustively to 200 000 |

`sin`/`cos` come from a 513-entry quarter-wave table with linear interpolation
(2 KiB of `.rodata`, one multiply per call). The header also keeps a CORDIC
implementation, `mu_q16_sincos_cordic`, for targets that cannot spare the table:
it needs 80 bytes and is 13.7× slower. Both are gated.

## Limits, stated

* 4.5 decimal digits and a range of ±32768. Not a float replacement for
  physics; fine for animation, audio gains, sensors, tiles, game logic.
* **`mu_q16_atan2` is 1.6× slower than `atan2f`** (4.5× on a varied input
  stream, where its branches stop predicting). A table-based atan2 is possible
  and not written.
* **`mu_q16_sqrt` is 44× slower than `sqrtf`.** IEEE-754 makes `sqrtf`
  correctly rounded and therefore already deterministic — the integer loop is
  for FPU-less targets only. With an FPU, define `MU_FIXED_WANT_FLOAT_SQRT` and
  call `mu_q16_sqrt_f32` (1.35 ns).
* No `exp2`/`log2`, no matrix or vector fixed point, no Q32.32.

## Evidence

* `perf_test/PERF_FIXED.md` — seven fields, per-op table, the per-change
  decomposition (including the CORDIC→table reformation and its 13.7×), named
  losses.
* `make -C perf_test fixed-test` — 262 249 checks.
* `make -C perf_test fixed-nolibm` — links the whole API with no `-lm` and no
  `<math.h>`; fails if a libm symbol appears.
* `make -C perf_test fixed-determinism` — the digest of every output is
  identical at `-O0`, `-O2 -march=native` and `-O3`.
