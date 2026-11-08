# `easy_memory` vs `mu/` — what we do not have, and whether it is worth having

Reference: [EasyMem/easy_memory](https://github.com/EasyMem/easy_memory) (MIT),
single-header C, read at `/home/user/easy_memory`. Per AGENTS.md it is an
algorithmic reference, not a vendored API: nothing here is being copied in, and
none of its `em_*` symbols should appear in `mu/`.

Short version: it is a better *general-purpose* allocator than anything we ship,
by a wide margin on API and by a real margin on fragmentation, and it costs
about the same per operation as `mu_buddy`. We should take three of its ideas
(alignment-gap recycling, alignment-quality tie-breaking, and the tail
scratchpad) and explicitly **not** take its `em_free(void*)` header scheme,
because the 32-byte header it buys is measurable and large.

## 1. The gap table

| capability | easy_memory | `mu/` today | verdict |
| --- | --- | --- | --- |
| free block selection | best-fit in a **triple-key LLRB** (size → alignment quality as CTZ → address), iterative, no recursion | `mu_buddy` = pow2, per-level LIFO list; `mu_offset_allocator` = size-class bins, first fit | **partial** — neither of ours has a size-ordered best fit *nor* an alignment-quality key |
| arbitrary sizes, exact fit | yes, splits the remainder | buddy rounds to pow2 (`docs/allocators.md`: up to ~2x internal waste); offset-allocator is waste-free per class | buddy: **no**; offset: **yes**, but not for arbitrary byte sizes |
| coalescing on free | yes, physically adjacent blocks merge into the LLRB | `mu_buddy` yes (the whole `merge_xor_map` design), offset-allocator yes within a block | **have** |
| per-allocation alignment | `em_alloc_aligned`, 1 B … `EMMAX_ALIGNMENT` = **2048** on 64-bit (measured; the README says 512 — the header computes `256 << 3`) | `mu_buddy_alloc(b, size, align)`, `mu_arena_alloc(a, size, align)`, `mu_pool` block size | **have**, with the caveat that buddy alignment is derived from the arena base |
| **alignment-gap recycling** | when an aligned request skips padding, the gap is split out and returned to the free tree if it is at least `EM_MIN_BUFFER_SIZE` | nothing: the skipped padding is lost until the block is freed | **GAP** — worth taking |
| **tail-end scratchpad** | `em_alloc_scratch` / `em_alloc_scratch_aligned`: reserve from the *highest* address in O(1) | nothing; `mu_arena` is bottom-up only, `mu_frame_allocator` is bottom-up | **GAP** — cheap, ~30 lines on the arena |
| nested scopes | `em_create_nested(parent, size)`: destroying the parent invalidates every child in O(1) | `mu_scope_stack` (mark/release), `mu_deferred_free`, `mu_frame_allocator` | **partial** — stack discipline and deferred frees, but no allocator-of-allocators with parent invalidation |
| sub-allocators built *inside* a parent | `em_bump_create(em, size)`, `em_slab_create(parent, …)`, `em_create_scratch` — zero-overhead creation (one standard allocation) | `mu_slab` / `mu_slab_manager` / `mu_frame_allocator` take a caller-supplied buffer | **partial** — same algorithms, one layer of plumbing missing |
| two-ended (inverted bi-directional) stack allocator | yes, with dynamically scaled metadata | nothing; ours are one-ended | **GAP**, low value for us |
| `free(void*)` with no allocator handle | yes: every block carries a 4-word header with an XOR-magic for underflow detection | every `mu_*_free` needs its allocator | **decline** — see §4 |
| poison bytes, contract vs defensive policy modes | `EM_POISON_BYTE`, `EM_POLICY_CONTRACT` / `EM_POLICY_DEFENSIVE` | `mu_debug_break.h`, ASan/UBSan in CI, and per-header gates | **decline** — we get the same coverage from the sanitizers without a runtime cost in release |
| bare metal / no libc | `EM_NO_MALLOC`, 16-bit targets | libc is assumed | out of scope |
| fuzzing fleet per allocator | `libFuzzer` targets for core/bump/slab/chaos | deterministic gates (`test_allocators.c`, 10 148 checks) | **note** — our gates are stronger on the specific bug class we had, theirs is broader on chaos |

What we have that they do not: `mu_deferred_free`, `mu_slab_manager`,
`mu_offset_allocator`, `mu_vm_arena`, thread/sync primitives, and the whole
non-allocator half of the library.

## 2. Measured: it is the same speed as our buddy, and better on fragmentation

Two harnesses, both in `/tmp/emb` (first-order: one `CLOCK_MONOTONIC` pass over
200 000 operations, `taskset -c 0`, no repetition or CI — treat the timings as
±20% and the counts as exact, since the counts are deterministic).

### Timings, 32-byte requests (`bench.c`)

| operation | easy_memory | `mu_arena` | `mu_pool` | `mu_buddy` |
| --- | --- | --- | --- | --- |
| alloc | 26-30 ns (71 on the first cold pass) | **1.7** | 8.3 | 49.4 |
| free, LIFO | 33.5 | — | **6.9** | 13.1 |
| free, alternating | 90-134 | — | 13.5 | 65.1 |
| alloc out of fragmentation | 72.2 | — | — | **34.5** |
| reset / free everything | 25.9 (alloc+reset) | **~0** (44 ns total for 200 000) | — | — |

Read honestly: a bump arena is 20-30x cheaper than either allocator and a fixed
pool is 4-5x cheaper, so the general-purpose discussion is between `em_alloc`
and `mu_buddy`, and there it is a draw on alloc (`~30` vs `~50` ns, same order,
different runs disagree) with `mu_buddy` ahead on LIFO free (13 vs 34 ns) and
`easy_memory` ahead on the mixed-order free and on fragmented alloc. Their
O(1) "alloc + reset" claim is real (26 ns/alloc including the reset). These are
also consistent in shape with the pinned, Seven-Field numbers in
`perf_test/PERF_ALLOCATORS.md`, which is the table to cite for anything that
matters.

### Fragmentation, 16 MiB arena, random 24-200 B blocks (`frag2.c`)

Fill to OOM with random small blocks, free every other one, then ask for 120 B —
a size that fits in most of the holes (`free(ptr)` reachable without an
allocator handle is `em_free`).

| | blocks before OOM | live payload / arena | holes reused at 120 B |
| --- | --- | --- | --- |
| easy_memory | 110 716 | **73.9%** | 27 524 / 55 358 (50%) |
| mu_buddy | 102 555 | 68.5% | 51 277 / 51 277 (**100%**) |

Both are "correct", and the two columns disagree in the interesting way: exact
fit packs 5.4 percentage points more payload into the same arena, while pow2
rounding makes every hole reusable by anything up to its block size. A 24-byte
`em_alloc` carries a 32-byte header — **100% overhead on that request**
(`sz.c`: `sizeof(Block) = 32`, 4 machine words), which is exactly the 5.4 pp.

## 3. What is worth adopting

1. **Alignment-gap recycling** (real gap, small change). In `mu_buddy` the
   padding before an aligned block is dead until the block is freed; in
   `mu_offset_allocator` it is handled per size class but not returned to a
   general free list. Splitting the ≥ `MU_MIN_GAP` skipped run and pushing it
   back is ~25 lines and a gate case (allocate aligned, then check the gap is
   allocatable at its own size).
2. **Alignment quality as a free-block key** (the genuinely good idea). Before
   splitting a fresh block to satisfy an alignment, prefer a free block whose
   address already satisfies it. In `mu_buddy` this is free (levels are aligned
   by construction); in `mu_offset_allocator` it is not, and that allocator is
   where it would pay.
3. **Tail scratchpad** for `mu_arena`: one `size_t` at the end, `mu_arena_alloc_top`,
   reset restores both cursors. O(1), no metadata, and it removes the classic
   "my temporary buffer fragmented my frame arena" failure.

## 4. What we are deliberately not taking

* **`em_free(void*)`.** It requires a 4-word header on every allocation — 32
  bytes, 100% overhead on a 32-byte request, and a cache line touched on every
  free that our handle-based API never touches. The price of our API is one
  extra argument at the call site. We keep the handle.
* **The LLRB.** It is the right structure for exact best-fit with a
  three-key order, and it is O(log n) with a 900-byte iterative stack. We already
  pay O(1) at the cost of pow2 rounding, and the measurement above says that
  trade is worth ~5 percentage points of density, not a tree.
* **Policy modes / poison bytes / XOR magic.** Our gates plus ASan/UBSan cover
  this class of bug in CI, and `test_allocators.c`'s byte-level owner map caught
  the aliasing bug that this whole thread started from. A release-mode poison
  byte would be a per-op cost for a debug-only benefit.
* **Two-ended stack.** No user of ours has asked for growth from both ends.

## 5. The bug this comparison actually found

Chasing the `em_alloc` numbers is what exposed that our `mu_buddy` parity bit was
toggled on free and never on alloc — correct only for strict LIFO free order,
and silently producing two live allocations from the same bytes otherwise
(16 396 duplicate pointers in a 32 768-block arena, measured, no crash). The fix
(doubly linked per-level freelists + a parent-indexed merge bit maintained on
alloc, split *and* free) is in `docs/allocators.md` and pinned by
`perf_test/test_allocators.c`. That is the honest return on reading someone
else's allocator: not the API, the invariant.

## Reproduce

```sh
cd /tmp/emb
gcc -O2 -std=c11 -I /home/user/easy_memory -I /home/user/mu_lib-/mu -o bench  bench.c
gcc -O2 -std=c11 -I /home/user/easy_memory -I /home/user/mu_lib-/mu -o frag2 frag2.c
taskset -c 0 ./bench && taskset -c 0 ./frag2
```

These harnesses are throwaway (`/tmp`, not in the tree): they exist to answer
"does the gap matter" once, and the numbers that survive into decisions are the
counts in §2 and the pinned table in `perf_test/PERF_ALLOCATORS.md`.
