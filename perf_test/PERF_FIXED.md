# PERF_FIXED.md — `mu/mu_fixed.h`

Evidence for Q16.16 fixed point with no libm. The baseline is **glibc's libm**,
because every function here is a replacement for one: `sinf`, `cosf`, `atan2f`,
`sqrtf`. The headline is not "faster than libm" — it is the accuracy and
determinism tables, plus the one place where the first implementation was 8×
slower than libm and had to be thrown away.

All seven fields first, then the numbers, then what they do not show.

---

## 1. The seven fields

| # | field | value |
| --- | --- | --- |
| 1 | CPU / µarch | `Intel(R) Xeon(R) Processor @ 2.60GHz` (family 6 server part, AVX-512 present), one vCPU of a guest |
| 2 | Cores / affinity | 2 vCPU total, **1 thread**, every timed run wrapped in `taskset -c 0` |
| 3 | Measured frequency | as `PERF_TIME.md`: a 20 000 000-long dependent `add` chain measures **0.64–1.07 GHz** while ramping, against a nominal 2.60 GHz. Treat `cycles ≈ ns × 1.07` and a host at nominal as printing ≈41 % of these figures |
| 4 | Compiler / flags | `gcc (Debian 14.2.0-19) 14.2.0`, `-O3 -march=native -std=c11 -Wall -Wextra -Werror -lm`; the gate also builds at `-O0`, `-O2 -march=native` and `-O3` and compares the digest of all its output (§4), and runs clean under `-O1 -fsanitize=address,undefined -fno-sanitize-recover=all` — which caught a *left shift of a negative value* in `mu_q16_from_i32` (UB in C99, identical code on every compiler, now a multiply). Kernel `6.1.189+`, `glibc 2.41` |
| 5 | Workload / working set | per-call rows: a dependent angle chain (`a += 7919`, no foldable input) feeding a volatile accumulator. Batch rows: a 1024-entry LCG-generated angle array (4 KiB) walked sequentially. Table path touches 2 052 B of `.rodata` + the 80 B atan table, all L1-resident (**48 KiB L1d / 1.3 MiB L2 / 54 MiB L3**) |
| 6 | Baseline | (a) glibc `sinf`+`cosf`, `sinf`, `atan2f`, `sqrtf` on the identical loop shape with the identical dependency; (b) **the first implementation, `mu_q16_sincos_cordic`, kept in the header and in the bench** — so the algorithmic change is measured against the code it replaced, not asserted |
| 7 | Method | `mu_bench` with `MU_BENCH_DO_NOT_OPTIMIZE` barriers, auto-sized batches, **61 samples/row**, min (latency floor) / median / CV / CI99. Accuracy is measured separately and exhaustively against `double` (§3), which needs no clock at all |

---

## 2. Per-operation table (61 samples)

```
row                              min  |  median |  cv   | Mop/s
fixed.sincos                   8.780  |  9.440  |  4.9% |  104.9
fixed.sincos_cordic         119.491  | 129.627 |  1.8% |    7.7
libm.sincos                   14.743  | 15.307  |  1.1% |   65.2

fixed.sin                      4.632  |  4.766  |  1.1% |  209.5
libm.sin                      12.502  | 12.949  |  1.3% |   77.2

fixed.atan2                   35.310  | 36.221  |  1.6% |   27.6
libm.atan2                    22.208  | 22.751  |  1.5% |   43.9

fixed.sqrt                    81.492  | 83.648  |  1.6% |   11.9
fixed.sqrt_f32                 1.207  |  1.348  |  7.3% |  729.4
libm.sqrt                      1.813  |  1.897  |  1.9% |  525.4

batch.fixed_sincos_1024        3.229  |  3.768  | 20.4% |   0.25   (µs per 1024)
batch.fixed_sincos_cordic_1024 84.922 | 90.057  |  5.7% |   0.01
batch.libm_sincos_1024         9.925  | 15.000  |  5.5% |   0.07
batch.fixed_atan2_1024        99.169  | 103.558 |  1.6% |   0.01
batch.libm_atan2_1024         22.552  | 23.000  |  1.3% |   0.04
```

Ratios of medians (ours / libm, below 1 is ours cheaper):

| comparison | ratio | reading |
| --- | --- | --- |
| `fixed.sin` vs `sinf` | **0.37** | **2.7× faster** |
| `fixed.sincos` vs `sinf`+`cosf` | **0.62** | **1.6× faster** |
| `batch.fixed_sincos_1024` vs libm | **0.25** | **4× faster** throughput, on a 1024-angle stream |
| `fixed.sqrt_f32` vs `sqrtf` | 0.71 | 1.4× faster; both are the hardware instruction, ours skips a call |
| `fixed.atan2` vs `atan2f` | 1.59 | **1.6× slower** (see §4) |
| `batch.fixed_atan2_1024` vs libm | 4.50 | **4.5× slower** on a varied stream — the branches, not the arithmetic |
| `fixed.sqrt` vs `sqrtf` | 44 | **44× slower**: digit-by-digit integer vs one `sqrtss` |
| `fixed.sincos_cordic` vs `fixed.sincos` | 13.7 | the reformation in §3, measured |

---

## 3. Where the performance came from, per change

**The first implementation (CORDIC) was 8× slower than libm, and that is the
most useful number in this file.** CORDIC is the textbook fixed-point trig
algorithm, so it was written first: 20 dependent shift–add steps, one
data-dependent branch each, `atan(2^-i)` needed per step. Measured against
`sinf`+`cosf`: `fixed.sincos_cordic` 129.6 ns vs libm 15.3 ns.

| change | effect | evidence |
| --- | --- | --- |
| CORDIC, 20 iterations, branchy | 129.6 ns/sincos — 8.5× *slower* than libm | `fixed.sincos_cordic` row |
| replace rotation with a **512-entry quarter-wave table + linear interpolation** | **9.44 ns/sincos**, 13.7× faster than CORDIC, and 1.6× faster than libm | `fixed.sincos` row |
| `cos(a) = sin(a + quarter turn)` instead of a second table | one table, one case analysis; the sincos pair costs 2 walks, not 2 tables | `fixed.sincos` 9.44 vs `fixed.sin` 4.77 × 2 |
| angle in **turns** (Q16.16) rather than radians | range reduction is `>> 14` and `& 3`, no pi multiplication, no branch | grep the header: no `MU_PI` in the hot path |
| `mu_q16_sqrt_f32` (opt-in) | 1.35 ns vs 83.6 ns for the integer loop — 62× | `fixed.sqrt` / `fixed.sqrt_f32` |

**Why the table is allowed to be 2 KiB when CORDIC is 80 bytes.** Q16.16 needs
16 bits of output; a quarter wave sampled 512 times and interpolated linearly
is accurate to 1 LSB *measured over every angle* (§4), and the interpolation is
one `imul`. CORDIC needs no table beyond 80 bytes of atan constants and is the
right choice on a target where 2 KiB of `.rodata` is not available — which is
why it is still in the header, not deleted.

---

## 4. Accuracy (no clock involved)

| property | bound | how measured |
| --- | --- | --- |
| `sin`, `cos` | **1 LSB** (1.5e-5 absolute) | every angle in ±1 turn, i.e. 131 072 points, against `double` and `llround` |
| quadrant points | **exact**: sin(0)=0, cos(0)=1, sin(90°)=1, cos(180°)=-1 | four assertions, both paths |
| `sin²+cos²` | 5 LSB | every 7th angle; the two interpolations are independent, so the error on the square is not 1 LSB |
| CORDIC path | 2 LSB (measured 1) | every third angle |
| `atan2` | **1 LSB** | 2 000 000 random pairs, plus a 41×41 grid, plus a round trip `atan2(sin a, cos a) == a` (0 LSB worst over 5 042 angles) |
| `asin`, `acos` | **1 LSB** | all 65 537 inputs of the domain |
| `mul`, `div`, `sqrt` | correctly rounded / within 1 LSB | 161×161 grid against `double`; `sqrt` exhaustively for 0..200 000 |

The gate is 262 249 checks, clean under ASan+UBSan. Two things it caught are
worth naming. `sin(0) == -1 LSB` was the first — a rounding formula of the form
`(x + half) >> 16`, which floors, so negative values rounded *away* from zero;
every rounding in the header is now sign-explicit. The second was UBSan's: three
`<< 16` / `<< 14` on signed values, undefined for negatives even though every
compiler emits the shift, now multiplies.

**Determinism.** Two mechanical proofs, both in `make -C perf_test fixed-*`:
the same source built at `-O0`, at `-O2 -march=native` and at `-O3` prints the
same 32-bit digest of all 131 072 sin/cos pairs, 200 000 sqrt values and the
atan2 grid; and `fixed-nolibm` links a program that calls every function in the
header **without `-lm` and without `<math.h>`**, so a stray `sinf` would be an
undefined symbol and `nm` finds no libm references at all.

---

## 5. Losses, named

* **`atan2` is 1.6× slower than libm per call and 4.5× slower on a varied
  stream.** The per-call row has a nearly constant input, so its branches
  predict; the batch row draws from an LCG and does not. A table-based atan2
  (ratio → angle) is possible and is not written. This is the honest cost of
  bit-identical output: glibc's `atan2f` is a polynomial whose last ulp is not
  specified anywhere.
* **`mu_q16_sqrt` is 44× slower than `sqrtf`.** IEEE-754 requires correctly
  rounded `sqrt`, so `sqrtf` is *already* deterministic — the integer loop
  exists only for targets without an FPU. Callers with an FPU should define
  `MU_FIXED_WANT_FLOAT_SQRT` and use `mu_q16_sqrt_f32` (1.35 ns, and 1.4×
  faster than calling `sqrtf` yourself because it skips the call).
* **Everything is 4.5 digits.** Q16.16 cannot represent 1e-6, and the range is
  ±32768. It is not a drop-in for a float pipeline that needs the precision.
* **The 2 KiB table is a real cost** on a memory-tight target; CORDIC is the
  answer there, at 13.7× the per-call cost.
* **`sinf` on this box is 12.9 ns, and part of that is the guest's 0.64–1.07 GHz
  clock.** On a host at nominal frequency both sides shrink by ~2.4×; the
  *ratios* are what this file claims, not the nanoseconds.

## 6. What this does not show

* Anything about other fixed-point formats (Q1.15, Q32.32, Q8.24): only Q16.16
  is implemented and measured.
* Anything about the audio/embedded workloads the header is aimed at: the
  numbers are microbenchmarks of single calls in a tight loop.
* Anything about an FPU-less machine. The integer paths are integer paths and
  would not change, but "no FPGA-bound soft-float comparison was run" is a
  fair description of the gap.
* A comparison against other C fixed-point libraries (`libfixmath`,
  `fpm`, `fixedptc`): the baseline here is libm, because that is what the
  callers are replacing.

## Reproduce

```sh
make -C perf_test fixed-test          # 262 249 checks, ~0.4 s
make -C perf_test fixed-nolibm        # link with no -lm, no <math.h>
make -C perf_test fixed-determinism   # same digest at -O0 / -O2 -march=native / -O3
make -C perf_test fixed-bench         # 15 rows, 61 samples, taskset -c 0
```

Both tables are generated constants with a one-line definition, so they are
reproducible by construction: the sine table is
`round(sin(i/512 * pi/2) * 65536)` for `i = 0..512`, and the CORDIC table is
`round(atan(2^-i) / (2*pi) * 2^30)` for `i = 0..19`. The scratch file used to
find that 512 segments is enough (and 256 is not: 2 LSB) was `/tmp/tf3.c`, whose
sweep is now `test_fixed.c` §5.
