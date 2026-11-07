# Trigger index — symptom → rule → file

<!-- Architecture adapted from Intel's `performance-patterns` skill
     (https://github.com/intel/intel-performance-skills, (C) 2026 Intel
     Corporation, MIT). The three-tier routing idea and the quick-match table
     format are theirs. The rows below are ours: every pattern Intel catalogs
     was already documented in `AGENTS.md`, most in more depth, so this file
     imports the *index*, not the content. -->

`AGENTS.md` is 2500 lines and `dod-performance/SKILL.md` is a procedure. Neither
answers "I am looking at this symptom — which rule applies?" This file does.
Load it first, match a row, then read only the cited section.

**Do not fix from memory.** The cited section carries the mechanism, the
measured speedup, and the traps. Several of these have a losing case that is
only documented there.

---

## A. From source — no profiling data

Our normal case: this environment has **no hardware PMU** (`perf` unavailable,
no root). Source signals are the primary trigger, not a fallback.

| Signal in the code | Rule | Read |
|---|---|---|
| `for (rows) if (row.flag) ...`, `switch (row.type)` in a per-row loop | Flag / enum-as-flow — split tables, specialize the kernel | `AGENTS.md` §1 #3, §4.1, §4.4, §4.5 |
| `->next`, `->child`, `node->parent` walked in a hot loop | Pointer chasing — arrays, CSR, wide nodes | §6.2, §10.15, §10.17 |
| One FP accumulator: `sum += a[i]*b[i]`, running max/min | Serial dependency chain — 4–8 accumulators, Estrin | §5.9 |
| `_mm_*` / scalar float loop on an AVX2/AVX-512 host | Narrow SIMD — zipper upconversion | §7.16, §7.14 (lane trap) |
| Inline asm or intrinsics using `ymm`/`zmm0–15`, no `vzeroupper` before return or SSE call | Missing `vzeroupper` | §6.16 |
| Two+ pointer params, one written, no `restrict` | Missing `restrict` — overlap-check preamble kills vectorization | §6.4, §12.11 |
| `%` or `/` by a runtime value inside a loop | Division — mask, `fastrange32`, `fastmod`, Montgomery | §5.10 |
| Narrow store then wide load of the same bytes (`2×u32` → `u64`/`__m256`) | STLF stall / 4K aliasing | §6.11 |
| 2D array with power-of-2 row pitch, accessed vertically | Cache-set thrashing — pad the pitch | §6.12, §3.9 |
| `data[idx[i]]` gather with no prefetch | MLP / MSHR distance | §6.3, §6.14 |
| Write-only multi-MB output buffer | RFO / write-allocate — streaming stores | §6.7, §6.15 |
| `cmov` selecting between two *out-of-cache* addresses | CMOV trap — control dependence beats data dependence | §5.8 |
| Hot function calls an error reporter with no `cold` | Cold-path outlining (attribute on the **definition**) | §5.4, §5.12, §12.11 |
| `malloc`/`free` paired inside a loop | Per-frame churn — arena rewind, pool | §9.1, §9.7, §9.10 |
| Nullable field for optional data | Optional = membership in another table | §4.3, §12.5 |
| `bool` array used as a processing filter | Bitset / sparse set / existence table | §4.1, §4.2, §10.14 |
| Index built before any query needs it | Build indexes for hot repeated queries only | §10.2 |
| `qsort` / `std::sort` on numeric keys | Radix (`mu_sort.h`) or SIMD quicksort | §10.16, §10.7 |
| Function named `crc32c`, `cosine_similarity`, `hamming`, `jaccard`, `iou` | Known kernel — SIMD replacement exists | §7.15, §7.16 |
| Hand-written hash table | SwissTable / `unordered_dense` | §10.10, §13.1 |
| Linear/binary search over >1 MB static sorted data | Eytzinger + prefetch, or 16-ary S+Tree | §10.11 |
| Struct with fields consumed by different passes | Hot/cold split, SoA, or AoSoA | §3.2, §3.3, §3.11 |
| Chunked buffer with a **fixed** chunk size, editing a small document | Copy volume per edit scales with chunk size | `text-buffer-dod/references/chunked-text-buffers.md` |
| Fields written by different threads, no 64 B separation | False sharing | §6.6, §M.2 |
| `while (!cmpxchg(&lock,...))` with no prior read | TTAS | §M.3 |
| Mutex around a read-mostly lookup | Escalation ladder: mutex → rwlock → seqlock | §M.4 |
| `notify_all` / `pthread_cond_broadcast` to a pool | Thundering herd | §M.11, §18 |
| Global `count++` / atomic on a stats field | Per-CPU / per-thread stats, reduce at read | §M.2, §18 |
| Hot symbol lives in a system `.so` | Check the installed version before optimizing our code | §2.6 (the `libcrypto` pre-3.3 case) |

## B. From a profile — when a PMU is available

| Profile signal | Rule | Read |
|---|---|---|
| `vaddsd`/`vmulsd` >50 % of a hot loop (ignore zeroing `vxorpd`) | Scalar FP — vectorize | §7.1, §7.2, §7.3 |
| `src == dst` register on >40 % of samples + IPC < 0.5 | Serial accumulator | §5.9 |
| `xmm`/`ymm` on a wider CPU | Narrow SIMD | §7.16 |
| In-loop horizontal `shufps`/`vhaddps` right after `vmulps` | Layout — SoA, not in-loop transpose | §7.5, §7.7 |
| Overlap-check preamble before the vector body | Missing `restrict` | §6.4 |
| >30 % load wall, high LLC MPKI | Bandwidth/locality first — roofline, then layout | §2.5, §2.6, §6.1 |
| `HITM` / `perf c2c` cluster on one line | False sharing | §6.6, §M.2 |
| `other_assists.avx_to_sse` non-zero | Missing `vzeroupper` | §6.16 |
| `Frontend_Bound` > 15 % | I-cache / µop-cache — code size, cold outlining | §6.9, §5.12, §6.16 |
| `Bad_Speculation` > 15 % | Unpredictable branch — sort, predicate, or split | §5.1, §5.2, §5.3 |
| `Backend_Bound` > 20 %, memory-bound | Roofline / ECM before touching instructions | §2.6 |
| Hot symbol inside a system `.so` with a version gap | Library upgrade, not our code | §2.6 |
| `BR_INST_RETIRED.NEAR_TAKEN` high on an error path | Confirm <0.1 % taken rate, then `cold` | §5.12, §12.11 |

No PMU? Substitute `-g -fdump-tree-profile_estimate-lineno -dumpdir dump/` for
branch-probability questions (works on any arch, no counters), and the
cache-line LRU simulator in `editor-ds/loom/cache_sim.c` for locality questions.
Neither substitutes for a wall-clock A/B.

## C. Worth fixing without profiling confirmation

Structure alone predicts the problem strongly enough to justify the change.
**Still measure before/after** (§1 #12) — this list authorizes *attempting* the
fix, not skipping the measurement.

1. Missing `restrict` on non-aliased pointer params — §6.4
2. Missing `vzeroupper` after `ymm`/`zmm0–15` use — §6.16
3. Single FP accumulator in a hot loop — §5.9
4. `%`/`/` by a runtime value in a loop — §5.10
5. Per-row `if (flag)` / `switch (type)` — §4.1, §4.4
6. Pointer-chased hot loop — §6.2
7. `malloc`/`free` inside a loop — §9.7
8. Power-of-2 row pitch with vertical access — §6.12
9. Narrow store → wide load of the same bytes — §6.11
10. `cold` missing on an error path — §5.12
11. Fixed chunk size in a chunked buffer whose documents vary by 1000× —
    `text-buffer-dod`

## D. Measured dead ends — do not retry

This section has no counterpart in Intel's catalog. It exists because we
already paid for these measurements. Re-proposing them wastes a round.

| Tried | Result | Evidence |
|---|---|---|
| SoA hot-field mirror beside `mu_ted`'s AoS segments, 5 variants | **0.84–0.90×** on range_edit. The segment header shares a cache line with the `offs[]` the descent reads anyway. The maintain-but-never-read control still lost ⇒ cost is write-side rebuild, not read-side misses. | `docs/ROUND4_SOA_REJECTED.md` |
| `mu_loom` fanout 32 | Lost | `docs/ROUND3_LOCAL_SPLIT.md` |
| Whole-region balanced chunking in `mu_loom` | 8955 vs 4272 ns greedy. Balancing *only* the 2-leaf split case is a win. | `docs/ROUND3_LOCAL_SPLIT.md` |
| Branchless `__find64` | No effect | round-3 notes |
| Software prefetch on a unit-stride stream | Never helps; the hardware prefetcher already covers it | §6.14 |
| Deferring `trim_tail_start` to segment exit to fix the lost line start | Fixed **zero** failures. Real cause was the boundary start in `seg_insert`. | `docs/ROUND6_ADAPTIVE_SEG.md` |
| AVX-512 `unpacklo/hi_epi32` + `shuffle_i32x4` to interleave two arrays | Cannot produce `o0,l0,o1,l1,…`. Use two `permutex2var_epi32`. | round-3 notes |

## E. Where the write-time checklist lives

Intel splits review-time and write-time guidance into separate files. Ours is
already split, just not by filename:

* **Writing new code** → `dod-performance/SKILL.md` Steps 1–6 (in order).
* **Reviewing code you are about to call done** → `AGENTS.md` §15 and
  `SKILL.md` §6.
* **Choosing a container** → `AGENTS.md` §13.1, `references/mu-map.md`.
* **Writing SIMD** → load `simd-hardware-c`, then §7.14 and §7.16.
* **Anything multithreaded** → stop, finish single-core first, then load
  `multithreading-dod` and Part II.
