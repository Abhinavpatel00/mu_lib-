---
name: dod-performance
description: >
  Write and refactor C (and C-like) code for maximum runtime performance using
  data-oriented design: table/SoA layout, existence-based processing, branch
  elimination, cache- and SIMD-friendly loops, explicit arenas/handles,
  ownership-based concurrency, LOD/mementos, and measure-first optimization.
  Use this whenever the task involves performance, games, engines, simulation,
  hot loops, memory layout, ECS, allocators, profiling, or when the user asks
  for "data-oriented", "high performance", "fast C", "DOD", or "cache friendly"
  code — even if they do not use those exact words.
---

# Skill: Data-Oriented Performance in C

This skill turns a request like "make this fast", "write a particle system",
"add an ECS", or "refactor this object-oriented C" into data-oriented C that is
measurably faster., and the repository
`AGENT.md` style contract.

**Before writing code, read `AGENT.md` and `dodnotes.txt` at the project root.**
They are the authority; this skill is the operating procedure.

---

## Step 0 — Load only what the situation needs

<!-- Routing architecture adapted from Intel's `performance-patterns` skill,
     (C) 2026 Intel Corporation, MIT. Content below is ours. -->

| You have | Read |
|---|---|
| A **symptom** ("this loop is slow", "line counts are wrong") and no profile | `triggers.md` §A — symptom -> rule -> `AGENTS.md` section |
| **Profiling output** (perf annotate / c2c / stat, VTune, flamegraph) | `triggers.md` §B |
| No PMU, and you need to know if a change is even worth attempting | `triggers.md` §C, then measure anyway |
| An idea you are about to propose | `triggers.md` §D first — it may already be a measured dead end |
| A **new** performance-sensitive design | Steps 1-6 below, in order |
| Code you are about to call **done** | §6 below and `AGENTS.md` §15 |
| A **text editor buffer / rope / piece table / gap buffer** | Load skill `text-buffer-dod` |

Match the row, read the cited section, then act. Do not fix from memory: most
of these rules have a documented losing case.

---

## 0. Core conviction (apply to every task)

> Program = **data + transformation**.
> Stop organizing memory around objects; organize it around the operations that
> consume the data. Represent state by **existence**, not by flags. Prefer
> **IDs and tables** over **pointers and objects**. Reduce **dependencies**.
> **Measure everything you claim.**

Do not apply the rules mechanically. Every technique here (SoA, SIMD,
branchless, non-temporal stores, fusion) has a losing case. That is why the
skill's last step is always a measurement, not an argument.

---

## 1. Procedure

Follow these steps in order. Do not skip step 1 or step 7.

### Step 1 — Frame the work as data

Answer, in writing if the task is non-trivial:

1. **What data exists?** List the values, their types/widths, their ranges.
2. **What transformations run?** Name each pass, its input, its output, and its
   frequency (per frame? per tick? on event? at load?).
3. **What is the access pattern per pass?** read-only / read-write / write-only;
   sequential / strided / random; how many elements; how big; hot or cold.
4. **What lifetime does each datum have?** per frame / per level / permanent.
5. **What can be derived, grouped, or regenerated** instead of stored?
6. **What does not need to exist individually** at the current level of detail?

### Step 2 — Choose the layout & data structure from the access pattern

- Fields touched together every frame (`<= 32B`) → **Hot/Cold split AoS** (`7.07×` over bloated AoS).
- 1–6 numeric columns streamed linearly → **Aligned Pure SoA** (`mu_soa.h`, `11.50×` with AVX2).
- 6–20 fields accessed by random packet/tile/ray bucket → **Intel ISPC Tiled `AoSoA8` / `AoSoA16`** (`3.96×`).
- 2D/3D spatial neighborhood / bilinear texture sampling → **Morton Z-order + Fabian Giesen dilated `+1` stepping** (`(mx - 0x55555555u) & 0x55555555u`, `1.37×`).
- Static/semi-static graphs & meshes → **Contiguous CSR (`mu_csr.h`)**, NEVER heap node lists (`32.72×`).
- Point-lookup hash table → **SIMD SwissTable (`mu_swiss_map` in `mu_swiss_map.h`, 1-byte H2 SIMD control bytes, `6.44×`)**.
- Hash table that is ALSO iterated frequently → **`ankerl::unordered_dense` style contiguous SoA `mu_dense_map` (`mu_swiss_map.h`, `178.17×` iteration)**.
- Large static sorted search (`> 1 MB`) → **Eytzinger 1-based BFS + `MU_PREFETCH`** (`mu_eytzinger_u32` in `mu_eytzinger.h`, `5.92×`) or **16-ary Cache-Line AVX2 S+Tree** (`mu_stree16_i32` in `mu_eytzinger.h`, `9.79×`); NEVER plain CMOV binary search without prefetch (`1.00×` due to killed speculative loads!).
- Approximate membership pre-filter → **AVX2 Split-Block Bloom (`mu_bloom_simd256` in `mu_bloom.h`, 1 cache line/query, `8.31×`)** or **Static `Xor8`/`Binary Fuse 8` (`mu_xor_filter.h`, `5.37×`, ~9 bits/key)** for static sets.
- Boolean flags over `N` items → **Dense `mu_bitset` + `ctzll`** (`11.85×`) when `1%–50%` set; **4-Level `mu_hier_bitset`** (`318.68×`) when `< 0.1%` set over millions of IDs; **Existence Table / `mu_sparse_set`** (`121.72×`) when active entities drive a transform.
- Short-lived local scratch vectors (`<= 8..16` items common) → **LLVM `SmallVector` (`mu_small_vec.h`, `1.59×`)**.
- Priority queues / timers → **4-ary heap (`mu_four_heap.h`)** or **O(1) Hierarchical Timer Wheel (`mu_timer_wheel.h`)**.
- Sorting numeric arrays → **1-hist + pass-skipping LSD Radix (`mu_sort.h`, `FloatFlip` `18.67×` vs `qsort`)**, or **in-place SIMD Quicksort (`numpy/x86-simd-sort`, `3–8×` vs `std::sort`, `qselect`/`partial_qsort`/`argsort`)** when `O(N)` radix scratch is unwanted (watch `hasnan=true` — NaN with `hasnan=false` is UB, and `argsort` falls back to scalar on NaN!).
- Intrusive hash/timer buckets (when truly needed) → **Linux `hlist_node` `**pprev` (8B bucket, 0 head-branch, `1.59×`)** or **Mesa circular sentinel `exec_node`**.

### Step 3 — Remove branches, break serial chains, and kill divisions

- Delete dead/hurt/visible/idle entities from the hot table (swap-remove); `if (!dead)` disappears (`55.32×` over vtables when combined with SoA+AVX2!).
- Replace `enum + switch` dispatch with **separate homogeneous tables + specialized transforms**.
- Sort/bucket by the branch's deciding axis (`9.49×`) or use branchless masks (`13.96×`) / AVX2 predicates (`32.74×`).
- **Break FMA/ALU latency chains (Agner Fog rule):** `vfmadd231ps` has 4-cycle latency × 2/cycle throughput = **use 4–8 independent accumulators** (`20.20×` dot-product speedup) and **Estrin's binary tree** over Horner's rule (`1.74×`).
- **Eliminate integer `%` / `/`:** Power-of-2 `& (cap - 1)`; arbitrary hash bucket `Lemire fastrange32` (`(uint64_t)h * cap >> 32`, `11.48×`); exact runtime modulo `Lemire fastmod` / Mesa `fast_urem` (`2.98×`); batch float reciprocals via Montgomery's trick (`1 / (a*b)`).
- Outline cold error/logging/corrupt-input callees with `MU_UNLIKELY` + `__attribute__((cold))` (`[[gnu::cold]]`) placed on the **function definition** (declaration-only is a no-op; does not auto-propagate to sub-callees!). Confirm `< 0.1%` taken rate via Intel PMU `BR_INST_RETIRED.NEAR_TAKEN:upp,BR_INST_RETIRED.NOT_TAKEN:upp` (shifting macrofused `cmp/test` counts to the jump!) or workload-free on any arch via GCC `-g -fdump-tree-profile_estimate-lineno -dumpdir dump/`.

### Step 4 — Respect desktop x86-64 cache, STLF, TLB & SIMD rules

- **Never trigger Store-to-Load Forwarding (STLF) stalls:** Never write narrow pieces (`2 × uint32_t`) and immediately read wide (`uint64_t` / `__m256`); pack in registers first (`4.29×`). Avoid 4K aliasing between `src` and `dst`.
- **Avoid power-of-2 2D row-stride set thrashing:** Pad row pitches by `+64B` (`+16` floats) so vertical accesses don't collide on the same L1d cache set.
- **Explicit MSHR prefetching (`MU_PREFETCH`)** on indirect streams `data[idx[i + 16]]`; advise `MADV_HUGEPAGE` (2 MB pages) on large arenas so hardware prefetchers don't stop at 4 KB page boundaries.
- **Prevent L2 cache pollution on write-only outputs:** Use `_mm256_stream_ps` + `_mm_sfence()` for multi-MB GPU/file staging buffers (`1.63×` faster hot-L2 survival).
- Load skill **`simd-hardware-c`** whenever writing explicit AVX2 / BMI2 / `PCLMULQDQ` / AVX-512 kernels.

### Step 5 — Own memory & virtual address space explicitly

- **Zero Is Initialization (ZII):** Design structs so all-bits-zero is valid; reserve slot `0` as a safe null sentinel object.
- **64-bit VM Reserve + Demand Commit (`mu_vm.h`):** Reserve gigabytes of virtual address space (`PROT_NONE`, 0 physical RAM) and commit pages monotonically — zero `realloc` copies or dangling pointers!
- **Double-Mapped Magic VM Ring (`mu_vm.h`):** Map the same `memfd` twice back-to-back so ring wrap-around reads/SIMD loads are 100% contiguous (`2.79×`).
- **Frame / Pass Bump Arenas (`mu_frame_allocator.h`, Mesa `ralloc`/`linear_alloc`):** Rewind cursor in O(1) (`6.12×` over `malloc`/`free`); alias non-overlapping stage buffers (Frostbite FrameGraph).
- **LLVM `PointerIntPair`:** Pack 2–3 tag bits into aligned low pointer bits instead of adding padded bool/enum fields.

### Step 6 — Multithreading is STRICTLY SEPARATE (load `multithreading-dod`)

- **Never mix thread synchronization into single-core design.** Finish Steps 1–5 on 1 core first!
- Only when single-core execution is cache-clean and SIMD-saturated, load `.agents/skills/multithreading-dod/SKILL.md` (`AGENTS.md` Part II `§M.1–§M.12`) for MESI padding, Linux `seqlock`, RCU/epoch retire, batched+shadow SPSC, SLUB per-thread slabs, NUMA first-touch, and Naughty Dog user-space fibers.

### Step 7 — Bound with Models, Triage with TMA, and Measure Under the Seven-Field Rule

- **Roofline & ECM first:** Check operational intensity $I = \text{FLOPs}/\text{Bytes}$ against stream-matched memory roofs (`1R` vs `1R+1W` vs `2R+1W`, CARM per cache level, ECM overlap).
- **Top-Down TMA (`perf stat -M TopdownL1,TopdownL2` / AMD `PipelineL1`):** Flag Intel P-core slots against `Frontend_Bound > 15%`, `Bad_Speculation > 15%`, `Backend_Bound > 20%`, `Retiring > 70%` (ensure `100%` non-multiplexed counter share and never divide `cpu_core` by `cpu_atom`!). Sample hot lines skid-free with `cycles:pp` (PEBS), `ibs_op` (IBS), or `arm-spe`.
- **Inspect the hot 5% assembly & run the 7-Pattern `perf annotate` Scan:** Ensure DWARF symbols exist (`objdump --dwarf=info bin | grep -c DW_TAG`; rebuild adding **only `-g`** and discard stale `perf.data`!). Check `perf annotate --stdio -l <fn>` for: **(1)** scalar-only FP (`vaddsd`/`vmulsd` `>50%`, ignoring zeroing `vxorpd`/`vmovapd`), **(2)** narrow `xmm`/`ymm` on wider CPU, **(3)** serial accumulator (`src == dst` reg `>40%` + IPC `< 0.5`), **(4)** in-loop horizontal `shufps`/`vhaddps` right after `vmulps`, **(5)** missing-`restrict` overlap check preamble (`5.29×` in-place `a == c` trap!), **(6)** `>30%` load wall + high LLC MPKI, and **(7)** outdated system `.so` symbols (`gcm_init_avx` / pre-3.3 `libcrypto.so.3` → check `readlink -f` or `openssl version`, upgrade to OpenSSL 3.3+).
- **Seven-Field Evidence Rule:** State (1) CPU & µarch (+ P/E core), (2) core count & affinity, (3) measured core GHz (`cycles/task-clock` or 1-cycle `add` chain, NOT `rdtsc`!), turbo/DVFS & SMT, (4) compiler & flags, (5) workload & byte working set (non-zero init!), (6) baseline, and (7) method (`CLOCK_MONOTONIC_RAW`, `SINK(res)` + `OPAQUE(ptr)`, `>= 21` paired round-robin reps, **min** for latency, **median** for throughput, **geomean** for ratios, + `CV`). Revert if no gain.

---

## 2. Hard rules (violations are bugs)

1. No long-lived raw pointers as identity — use IDs/handles.
2. No `bool` arrays as processing filters over large domains — use tables/bitsets.
3. No `switch (type)` dispatch in a hot loop — use table membership.
4. No nullable fields for optional data — use membership in another table.
5. No pointer-chased hot loops — use arrays / CSR / wide nodes (B-trees).
6. No unchecked null chains — establish invariants at boundaries.
7. No data-dependent branches on random data in inner loops.
8. No `%` for power-of-two wrapping — mask.
9. No per-frame heap churn — arena-rewind or reuse.
10. No `int` in serialized/on-disk/network structures — fixed-width ints.
11. No unmeasured "optimization".
12. No new container/allocator in engine code: **extend `mylib` instead**.

---

## 3. Use `mylib` (`mu`) — never reinvent it

- **One-Column / SoA SIMD & High-Perf Modules**: `mu_array.h`, `mu_soa.h`, `mu_small_vec.h`, `mu_swiss_map.h` (`mu_swiss_map`, `mu_dense_map`), `mu_eytzinger.h` (`mu_eytzinger_u32`, `mu_stree16_i32`), `mu_xor_filter.h` (`mu_xor8_filter`, `mu_bfuse8_filter`), `mu_four_heap.h`, `mu_timer_wheel.h`, `mu_fast_codec.h`, `mu_hash_table.h`, `mu_perfect_hash.h`, `mu_chunked_bitset.h`, `mu_sparse_bitset.h`, `mu_hier_bitset.h`, `mu_bloom.h` (`mu_bloom_simd256`), `mu_csr.h`, `mu_spatial_hash.h`, `mu_sort.h`, `mu_indexed_heap.h`, `mu_vm.h`, `mu_vm_arena.h`, `mu_frame_allocator.h`, `mu_offset_allocator.h`, `mu_intern.h`.

| Need | Use |
|------|-----|
| Dense hot iteration | `array` (`array_push/size/capacity/reserve/pop/clear/free`) |
| Non-owning view | `mu_span` / `mu_span_f32_make` / `mu_span_f32_from_array` |
| Stable handles, holes | `mu_bulk_storage_*` (+ `mu_weak_handle`, generations) |
| Dense active IDs in range | `mu_sparse_set_*` (O(1), dense iteration) |
| key → value | `mu_hash32_*` / `mu_hash64_*` (fixed capacity options) |
| key → many values | `mu_multi_index_*` |
| Packed booleans / set algebra | `mu_bitset_*` (`next_set_bit`, `for_each`) |
| Ranged/contiguous ID alloc | `mu_id_pool_*` |
| Many small variable lists | `mu_chunked_u32_array_*` |
| Packed strings | `mu_string_arena_*` |
| Intrusive lists | `mu_pool_link` / `mu_index_list_*` / `mu_freelist_*` |
| Cumulative offsets / rank | `mu_length_index_*` |
| Quantization / bit packing | `mu_bitpacking.h` (`mu_quantize_unorm/snorm/half`) |
| Linear/frame/ring arena | `mu_linear_allocator`, `mu_ring_allocator`, `mu_arena` |
| Fixed-block pool | `mu_pool_*` |
| Variable sizes + coalescing | `mu_buddy_*`, `offset_allocator.h` |
| Leak/corruption tracking | `mu_mmgr.h` |
| Deterministic RNG | `mu_pcg32_*` |
| ECS + transforms + prefabs | `mu_ecs_*` |
| Worker pool + atomics | `mu_ts_*` |
| Priorities/groups/parallel_for | `mu_sched_*` |
| Timing | `MU_SCOPE_TIMER("name")`, `mu_time_now()` |
| Noise / roguelike domain math | `mu_noise_math.h`, `mu_roguelike.h` |

**If a primitive is missing:** add a small, reusable, header-only primitive to
`mylib/mu/` with an `MU_API`/`MU_STATIC` linkage model, `mu_malloc`-style macro
overrides, and `MU_ASSERT` on contract violations. Then use it. Do not add a
second competing container or allocator family to engine code.

Macros from `mu_common.h`: `MU_ASSERT`, `MU_INLINE`, `MU_RESTRICT`,
`MU_ALIGN(N)`, `MU_LIKELY/UNLIKELY`, `MU_PREFETCH`, `MU_MIN/MAX/CLAMP/SWAP`,
`MU_ALIGN_UP/DOWN`, `MU_IS_POW2`, `MU_STATIC_ASSERT`,
`mu_popcount_u64`, `mu_trailing_zeroes_u64`, `mu_leading_zeroes_u64`.

---

## 4. Design patterns to reach for

```
existence        : state → table membership (Damaged[], Visible[], Walking[])
optional data    : sparse component / sparse set, never a nullable field
scheduled work   : sort by ready_time, process while ready_time <= now
implicit entity  : Entity = id; components attached by id define it
composition      : create_entity() inserts into relevant tables (factory)
transform FSM    : evaluate conditions → transition buffer → single commit
LOD              : representation = f(relevance), not f(distance)
collective LOD   : N identical entities → 1 record + count
memento          : keep compressed history for future refinement
JIT memento      : regenerate from Hash(entity_id) instead of storing
derived state    : state = f(axis); avoid independent booleans
index on demand  : linear search until a query is hot, then build+maintain an index
bloom filter     : cheap "definitely absent" test before expensive lookup
prefix sum       : compaction/offsets; parallel allocation of output slots
```

---

## 5. Anti-pattern detector (rewrite triggers)

If you see any of these, stop and restructure before adding features:

- a struct with many fields used by different functions;
- `for (e : entities) if (e.flag) ...` where `flag` is usually false;
- `switch (e.type)` in a per-frame loop;
- `->next` walking in a hot path;
- an index built before it is needed;
- alloc/free paired inside a loop;
- a shared counter written by every thread;
- sorting in a hot path when only a top-K or an Nth element is needed;
- a scene graph traversal rebuilding a render list each frame;
- emulating every entity individually when a statistic would be believable.

---

## 6. Review checklist before reporting done

```
[ ] Data framed: passes, access patterns, lifetimes written down.
[ ] Layout matches access pattern (SoA/AoS/pool/grid/bitset chosen, justified).
[ ] Hot rows hold no pointers; ids/handles only.
[ ] State is existence; optional data is membership; no nullable members.
[ ] No enum-as-flow dispatch in hot paths (or justified).
[ ] No data-dependent branches on random data.
[ ] No pointer chasing; restrict/ownership explicit.
[ ] One allocator strategy per region; no hot malloc; stack bounded.
[ ] Concurrency: one writer, local reduce, no false sharing.
[ ] mylib used for every container/allocator; missing primitive added to mylib.
[ ] Before/after measurement recorded; revert if no gain.
[ ] Compiles warning-clean with -Wall -Wextra -Werror.
```

---

## 7. Reference material

- Skill `simd-hardware-c` — desktop x86-64 microarchitecture, STLF/TLB, SWAR, BMI2, `PCLMULQDQ`, AVX2 & AVX-512 intrinsics cookbook.
- Skill `game-engine-dod` — composes these rules into a whole game/engine (frame loop, ECS, Frostbite SIMD culling & FrameGraph, AoSoA8 shading, animation, LOD, renderer batching).
- Skill `multithreading-dod` — **strictly separated** multithreading, MESI coherency, Linux `seqlock`, RCU/epoch, shadow/batched SPSC, SLUB per-thread slab, and Naughty Dog user-space fibers.
- `perf_test/PERF_PROOFS.md` — runnable C11 empirical proof benchmarks (`results_proofs.csv` & `results_mt_proofs.csv`).
- `triggers.md` — symptom -> rule -> file index, from-source and from-profile tables, the no-PMU attempt list, and measured dead ends.
- Skill `text-buffer-dod` — chunked text buffers: chunk sizing, line-start boundary invariant, differential-test flavours.
- `references/dod-quickref.md` — condensed doctrine, formulas, and defaults.
- `references/mu-map.md` — fuller `mylib` API map with call shapes.
- Project root `AGENTS.md` — the full style contract and code examples.
- Project root `dodnotes.txt` — the human-readable DOD notes.
- `docs/` — per-module documentation.
