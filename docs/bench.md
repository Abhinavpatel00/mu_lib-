# `mu_bench.h` - micro-benchmark harness

A single-header benchmark framework in the spirit of ubench.h, with the defaults
chosen to enforce AGENTS.md 2.6 rather than to look flattering.

```c
#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"          /* include FIRST: it sets _POSIX_C_SOURCE */

MU_BENCH(simd, vec3_normalize)
{
    uint32_t i;
    float acc = 0.0f;
    MU_BENCH_ITERATE(i) { acc += mu_vec3_length(mu_vec3_normalize(v[i])); }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);      /* acc must be a variable, not an expression */
}

MU_BENCH_MAIN()
```

```
./bench                          run everything
./bench --filter=simd            only matching suites/names
./bench --list                   names, one per line
./bench --csv=out.csv            machine-readable results
./bench --target=50              ms per batch (default 20)
./bench --samples=21             batches per benchmark (default 15)
./bench --confidence=5           UNSTABLE cutoff, percent (default 2.5)
```

Exit status is non-zero if any benchmark exceeded the confidence cutoff, so noise
fails a build instead of hiding in a table.

## What it does for you

* **Auto-sized batches.** It grows the iteration count until one batch takes at
  least `--target` ms, so a 3 ns operation is never timed with a ~20 ns clock
  resolution and you never hand-tune a repetition count.
* **Median of batch means**, plus the 99% Student-t confidence interval over
  batch means (the ubench construction) and the CV. Both are printed because a
  median without a dispersion measure is a rumour.
* **Warmup batches** (3) before sampling, so caches, branch predictors and the
  allocator reach steady state.
* **One pinned core** is *not* done for you - pin with `taskset` when it matters.

## The barrier, and why it is the whole file

`MU_BENCH_DO_NOT_OPTIMIZE(x)` is `__asm__ __volatile__("" : : "g"(x) : "memory")`.
It must take the value as an **input operand**. A memory-clobber-only barrier
(`"" ::: "memory"`) does not stop the compiler from deleting a store to a static
that nothing reads, and therefore from deleting the entire computation feeding
it: the first version of this header measured a `sinf` loop at 0.26 ns per call
(0.8 cycles) because gcc had removed every call. Verified with `objdump -d`, not
assumed.

The `"g"` constraint is deliberate: Google Benchmark's `"+r,m"` fails to compile
for `float` on gcc ("impossible constraint in 'asm'"), and `"g"` accepts
registers, memory and immediates, and rvalues.

**Consequences you must respect:**
- Work whose result never reaches the barrier **is deleted**. `test_bench.c`
  asserts this, because it is the failure mode the header exists to prevent.
- A benchmark whose loop bound is a compile-time constant can be folded to a
  closed form. Keep trip counts and inputs opaque (a `volatile` bound, or data
  filled at runtime).
- A static, zero-initialized input array is constant-foldable. Fill inputs at
  runtime.

## Hooks

```
MU_BENCH(suite, name) { ... }              body, auto-sized iterations
MU_BENCH_EX(suite, name, iterations)       body, fixed iterations
MU_BENCH_SETUP(suite, name) { ... }        untimed setup, attached to the benchmark above
MU_BENCH_STATE(suite, name) { ... }        per-benchmark state (state->user), runs at registration
```

## Gate

`perf_test/test_bench.c` - 12 checks. The central one is a **ratio**: one
benchmark does eight independent copies of a dependent-FMA chain, another does
one, and the harness must report ~8x. That single assertion catches a dead
barrier, a mis-scaled iteration count and a mis-placed clock read. Measured 7.6x
and 8.7x on two runs.
