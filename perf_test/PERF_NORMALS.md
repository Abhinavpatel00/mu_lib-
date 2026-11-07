# Normal-transform performance record

Evidence behind AGENTS.md §3.16 ("Transform normals with the cofactor matrix").
Reproduce: `make -C perf_test bench-normals`
(`taskset -c 0 ./bench_normals --samples=21 --confidence=6`).

## 1. The seven fields

| # | field | value |
| --- | --- | --- |
| 1 | CPU | Intel Xeon (Skylake-class server), no E-cores |
| 2 | cores / affinity | 2 vCPU shared guest; run pinned with `taskset -c 0`, single-threaded |
| 3 | frequency | nominal 2.60 GHz, **not measured** per run (no `cycles/task-clock`). Ratios do not depend on it; the ns column does, so it is reported with a CV instead of as a fact. |
| 4 | compiler / flags | gcc 14.2.0, `-O3 -march=native -std=c11 -Wall -Wextra -Werror`, `-lm` |
| 5 | workload / working set | 64 matrices + 64 normals, 4 KiB each array (L1-resident): this is a **compute** benchmark, not a memory one. Matrices are non-uniformly scaled, skewed, and every third one reflects in z (`det < 0`), because that is the case that separates the two operators. Inputs are filled at runtime by an LCG - a static initializer would be constant-folded. |
| 6 | baseline | `mu_mat4_inverse` / `mu_mat3_inverse` **that we already ship**, followed by the actual transpose and multiply. Not a strawman: the question is whether the cofactor path beats the best inverse in the library. |
| 7 | method | `mu_bench.h`: auto-sized batches (>= 20 ms), 3 warmup batches, **21** sampled batches, **median** of batch means, 99% Student-t CI and CV, accumulator passed through an input-operand asm barrier at the end of every batch. Confidence cutoff 6% - a benchmark above it fails the run. |

## 2. Result

| path | median ns/op | min | cv | speedup |
| --- | --- | --- | --- | --- |
| `mat4_inverse_transpose` then multiply | 11.027 | 10.684 | 1.2% | 1.00x |
| **`mat4_cofactor` then multiply** | **3.579** | 3.552 | **1.1%** | **3.08x** |
| `mat3_inverse_transpose` then multiply | 3.827 | 3.706 | 1.7% | 1.00x |
| `mat3_cofactor` then multiply | 3.386 | 3.335 | 4.5% | 1.13x |

Geometric mean over the two: **1.87x**.

## 3. The earlier unstable row, kept on the record

An 11-sample run reported `mat4_cofactor` at 3.821 ns with **cv 13.6%** and was
flagged UNSTABLE, so it was not quoted. Re-running with 21 samples moved it to
3.579 ns at cv 1.1% - i.e. the unstable run was both *slower* and *noisier* than
the truth. This is the whole argument for the confidence cutoff: on a shared
2 vCPU guest, a single reading is not evidence, and the fix is samples, not
wishful rounding.

`mat3_cofactor` at cv 4.5% is inside the 6% cutoff but is still the noisiest row;
only the 1.13x *direction* is claimed for it, and the 3x3 case is argued from
correctness rather than speed.

## 4. Losses / scope

* **No win on 3x3.** 1.13x is a coefficient; a 3x3 adjugate is already cheap.
* **The output is unnormalized and scaled by `det(A)`** - if the caller needs a
  unit normal it pays one `rsqrt`/normalize, which the 4x4 numbers above already
  include for neither side (both rows multiply only). Adding normalization to
  both would compress the ratio; that is not hidden, it is why the benchmark
  measures the transform and not the whole shading pipeline.
* **Not applicable to a pure rotation** (`C == A`) - the right answer there is to
  transform with the matrix and skip the cofactor entirely.
