# `mu_allocators.h` performance — the arena fast path, and where the buddy's O(1) unlink actually pays

`perf_test/bench_allocators.c` (`make -C perf_test alloc-bench`), 10 rows in one
binary, 21 samples each, pinned to one core.

Two things are worth reading here. The first is boring and should be the
headline anyway: **the buddy is not the fast allocator in this file** — a bump
arena is ~33x cheaper per operation and a fixed-size pool ~10x cheaper, so if a
hot path can use either, it should. The second is the measurement the rewrite in
`docs/allocators.md` was built for: the merge now unlinks the sibling through
`free_prev` instead of scanning the level's freelist, and this file answers
*when* that is worth the extra 4 bytes per leaf. The answer is not "always" — in
the friendly free order it is worth nothing at all, and it is worth 2x-5x only
once the freelist gets long and the free order stops matching the park order.

## Method (Seven-Field Rule, AGENTS.md §2.6)

| field | value |
| --- | --- |
| CPU | Intel Xeon @ 2.60 GHz nominal (Skylake-SP class), 2 vCPU shared guest |
| cores / affinity | 1 core, `taskset -c 0`; the second vCPU belongs to a neighbour, so only ratios measured in the same process at the same time are quoted as differences |
| frequency / DVFS | **measured**, not assumed: a 1-cycle dependent `add` chain run 7x through `CLOCK_MONOTONIC_RAW` gave 1.63, 1.62, 2.26, 2.94, 3.04, 3.06, 2.97 GHz. The guest free-runs between ~1.6 and 3.1 GHz with no pinning available to us. This is exactly why half the rows below come back `CV > 10%`, and why the A/B pair that matters is compiled into the same binary and read back-to-back |
| compiler | gcc 14.2.0, `-O3 -march=native -std=c11 -Wall -Wextra -Werror` |
| workload | arena `4 MiB` + **2 129 984 B of buddy metadata** (measured, by bisecting `mu_buddy_init`'s required size — i.e. 2.03 MiB, a 2.0:1 metadata:arena ratio, 16.3 B of bookkeeping per 32 B leaf) + `N = 100 000` live 32-byte leaves = 3.2 MiB of blocks touched in a scattered order. Everything is several times larger than the 1 MiB L2, so every row is a cache-miss row, which is the honest case for an allocator |
| baseline | the `_scan` rows: a local copy of the same merge that finds the sibling by walking `free_next` instead of unlinking it through `free_prev`. Same level detection, same `merge_xor_map` bits, same split/refill path, same binary. Pinned equivalent by `perf_test/test_allocators.c::test_buddy_scan_equivalence`, which runs 32 fragmented bursts both ways and demands an **identical live-pointer set** (not merely identical counts) |
| method | `mu/mu_bench.h`, `CLOCK_MONOTONIC_RAW`, `volatile` sink, auto-sized batches to a 20 ms target, 21 samples, median reported, `CV` + 99% CI per row; the two list lengths are two builds of the same source (`-DCHUNK=...`) run on the same core minutes apart |

## 1. The per-operation table (`CHUNK = 256`, 21 samples)

| row | median | min | CV | CI99 | what one iteration is |
| --- | --- | --- | --- | --- | --- |
| `arena.bump32` | **1.985 ns** | 1.857 | 23.2% | ±14.9% | one `mu_arena_alloc(32, 8)`, cursor wrapped when full |
| `arena.top32` | **2.128 ns** | 2.095 | 0.8% | ±0.5% | one `mu_arena_alloc_top(32, 8)` — same bump from the far end, added with the back cursor (§6 of `docs/easy_memory.md`) |
| `pool.alloc_free` | **6.274 ns** | 6.028 | 5.4% | ±3.5% | alloc + free pair (a full churn cycle) |
| `pool.free` | **2.358 ns** | 1.709 | 13.1% | ±8.4% | one `mu_pool_free` of a pre-filled pool |
| `buddy.alloc_lazy` | **45.21 ns** | 41.34 | 22.6% | ±14.5% | one `mu_buddy_alloc(32)` out of an empty arena — the pure split-down path, 17 levels |
| `buddy.pop_park` | **93.32 ns** | 62.76 | 15.2% | ±9.8% | pop a free leaf and park it again, sibling left allocated so nothing cascades |
| `buddy.mergeburst_lifo` | 67.44 µs | 45.85 | 24.4% | ±15.7% | 1024 ops (see below), merge order = LIFO |
| `buddy.mergeburst_lifo_scan` | 63.06 µs | 43.91 | 19.1% | ±12.3% | same, sibling found by scan |
| `buddy.mergeburst_fifo` | 67.33 µs | 45.55 | 16.8% | ±10.8% | 1024 ops, merge order = FIFO |
| `buddy.mergeburst_fifo_scan` | **135.85 µs** | 133.45 | 3.7% | ±2.4% | same, sibling found by scan |

The four `mergeburst` rows are per **burst**, not per op; dividing by `4*CHUNK =
1024` gives `65.7 ns/op` for `fifo` and `132.7 ns/op` for `fifo_scan`. Only
`buddy.mergeburst_fifo_scan` is stable enough (CV 3.7%, CI ±2.4%) to quote
without a caveat, and that is not an accident: it is the row that does the most
work per byte of state, so it is least sensitive to a stolen timeslice.

## 2. The A/B the rewrite exists for: unlink vs scan

A fragmented burst is the only pattern where the two implementations can differ.
One burst parks `CHUNK` leaves (their siblings stay allocated, so the leaf
freelist grows to `CHUNK` entries), then frees the `CHUNK` partners — every one
of which now meets a free sibling *somewhere in that list* — then refills. The
free order decides everything:

* **LIFO** (free the partners in reverse: the last-parked sibling is at the head,
  so it is found in 0 scan steps). A scope/frame allocator sees this.
* **FIFO** (free the partners in order: the wanted sibling is at the tail, so the
  scan walks `CHUNK, CHUNK-1, ...` steps, `CHUNK²/2` per burst). A streaming
  producer/consumer sees this.

| freelist length | order | unlink ns/op | scan ns/op | scan / unlink | median-based |
| --- | --- | --- | --- | --- | --- |
| 256 entries | LIFO | 65.9 | 61.6 | **0.93x** | no difference |
| 256 entries | FIFO | 65.7 | 132.7 | **2.02x** | 135.85 / 67.33 µs |
| 1024 entries | LIFO | 63.8 | 65.5 | **1.03x** | no difference |
| 1024 entries | FIFO | 65.8 | 338.6 | **5.14x** | 1387 / 269.7 µs |

The clean part is the unlink column: **65.9 → 63.8 ns/op while the freelist grows
4x**. That is O(1) doing what it says, measured, at two lengths. The scan column
is not: 132.7 → 338.6 ns/op.

Fitting `cost = base + c · steps` from the two FIFO rows, where the scan steps
per op are `CHUNK²/2 / (4·CHUNK) = CHUNK/8` (31.9 at 256, 127.9 at 1024):

| | steps/op | measured extra | implied cost/step |
| --- | --- | --- | --- |
| 256-entry list | 31.9 | +67.0 ns | 2.10 ns |
| 1024-entry list | 127.9 | +272.8 ns | 2.13 ns |

Same cost per step to 1.4%, and the fitted `base` is 65.7 ns at one length and
65.8 ns at the other. Two points do not make a law, but a model that predicts
both rows from one 2.1 ns/step constant (`≈ 5-6 cycles`, which is what a
dependent pointer chase into L2 on this µarch costs) is the kind of agreement
that means the mechanism is understood and not curve-fitted: the merge's unlink
is O(1) *and* the scan's penalty is exactly the list walk.

**The honest negative:** in LIFO order the doubly-linked list buys nothing, and
at 256 entries the scan even prints marginally *faster* (61.6 vs 65.9 ns/op) —
inside the CI, i.e. no difference, but certainly no win for `free_prev`. The
extra 4 bytes per leaf are paid on every push and every pop. So:

* the change was made for **correctness** — the parity bit was toggled on free
  and never on alloc, which produced 16 396 duplicate live pointers in a 32 768
  block arena (see `docs/allocators.md`), and a sound merge in any free order
  needs the sibling removable from wherever it sits. The O(1) unlink is what
  makes "wherever it sits" cheap;
* the speed argument for it is real only from the FIFO row onward: 2.0x at 256
  entries, 5.1x at 1024. If our workloads were all LIFO we should not have paid
  for it.

## 3. Where the buddy's 66 ns actually goes

Per-op, `mergeburst` does one free (park or merge), one unlink/scan, and one
refill-adjacent split, on an arena holding 100 000 live leaves. Its 65.7 ns is
`~1 ns` of address arithmetic and the rest is metadata traffic: `free_next` +
`free_prev` are 8 bytes per node over 262 143 nodes = 2.03 MiB, so a pop, a park
and a merge touch three different lines that no longer fit anywhere. The two
rows that isolate this:

* `buddy.alloc_lazy` (45 ns) is a 17-level split, all writes, all in the level
  and freelist-head arrays — cheap per level, expensive per op because it is 17
  dependent writes.
* `buddy.pop_park` (62-110 ns, CV 15%) touches the leaf's links, its parity bit
  and the head — that is 2-3 cold lines, and the row's spread is the memory
  system, not the code.

This is also why the pool is 10x faster: 32-byte blocks, one `free_head`, and
the next-pointer lives *in the free block itself*, so a pop is one line of
already-touched arena memory. The buddy buys arbitrary sizes, alignment and
coalescing with 2 MiB of side tables, and this table prices that.

## 5. The offset allocator: a gate, a bug, and a negative result

`mu/mu_offset_allocator.{h,c}` -- Aaltonen's two-level bin scheme, ~660 lines,
in the tree since before the gate policy -- had **no test and no benchmark**.
It now has both, and they were worth writing: the gate found a reporting bug on
its first run, and the benchmark settled an easy_memory idea in one afternoon.

### 5.1 The gate found a bug in the first minute

`oa_storage_report()` returned `total_free_space = 0` for an arena with free
bytes in it, because the whole function was gated on `free_offset > 0` and
`free_offset` is the *metadata* budget, not the storage. Fill a 64 KiB arena
with 64-byte blocks under a 1024-node budget and it stops at 1022 blocks with
128 bytes free -- and the report said 0 free, 0 largest. One-line fix (read the
storage fields; nothing in the body depends on `free_offset`), one gate case
("leftover bytes are still reported when the nodes run out"), and the same
workload is what shows *why* the node budget matters: it binds 128 bytes before
the byte budget does.

### 5.2 The A/B that said no

`easy_memory`'s alignment-quality idea -- prefer a free block whose address is
already aligned over one that needs a leading skip -- is a good idea in a
general heap. `OA_ALIGN_LOOKAHEAD=N` implements it in the aligned scan: keep the
first fit in hand, walk up to N more nodes *in the same bin*, take an
already-aligned one if it shows up. `make offset-ab` builds the same source
twice, once at 0 and once at 8, and prints exact counts.

Six fragmenting workloads (40 holes in one bin, holes 96..103 bytes, requests
96 bytes; the `walking` rows move the hole offsets across the alignment grid):

| workload | align | nodes @0 | nodes @8 | visits @0 | visits @8 | bytes (both) |
|---|---|---|---|---|---|---|
| uniform holes | 64 | 160 | 160 | 1600 | 1600 | 61 696 |
| uniform holes | 32 | 81 | 81 | 40 | 40 | 61 696 |
| uniform holes | 16 | 81 | 81 | 40 | 40 | 61 696 |
| walking holes | 64 | 160 | 160 | 1481 | 1521 | 61 696 |
| walking holes | 32 | 124 | 124 | 1281 | **1374** | 61 696 |
| walking holes | 16 | 126 | 126 | 948 | **1114** | 61 696 |

Every region granted is the same region, the node count is the same, the free
storage is the same, and the lookahead only visits 2.7-17.5 % more nodes. The
`zero-gap` counter explains it: in the `walking align32` row, **35 of 40**
requests already landed on an aligned region under plain first-fit, because
freed regions are pushed to the bin head and an aligned region that is freed
keeps its alignment. Where the counter is 0 (align 64 against holes that are
never 64-aligned) no amount of walking can conjure an aligned node.

So the knob ships at **0**. It is kept, with the counters, so the experiment is
reproducible rather than a claim -- the same reason the `_scan` baselines in §2
are kept.

### 5.3 Per-operation cost (shipped configuration, 31 samples)

| row | median | min | CV |
|---|---|---|---|
| `offset.alloc_free` -- `oa_allocate(64)` + `oa_free` | 41.4 ns | 39.9 | 22.4 % |
| `offset.aligned_exact` -- aligned alloc that needs no split | 33.5 ns | 24.0 | 15.1 % |
| `offset.aligned_gap` -- aligned alloc that skips a 56-byte gap | 33.8 ns | 25.7 | 12.5 % |
| `offset.free_merge` -- alloc 32 + free into a live neighbour | 43.5 ns | 36.4 | 20.6 % |

Each row is an alloc/free pair, so halve them for a per-operation figure of
merit. `aligned_gap` costing the same as `aligned_exact` is the interesting
one: parking a 56-byte scrap node and splitting off a remainder is two extra
bin inserts and it does not show up at this resolution. All four rows are
CV 12-22 % and `mu_bench` flags them unstable -- these are sizes, not ratios,
and nothing in 5.2 depends on them.

### 5.4 What this does not show

* Whether the offset allocator is fast *compared to anything*. There is no
  baseline here; §1-§3 cover our own arena/pool/buddy, and this section covers
  one library-internal question.
* Anything about 16-bit node indices (`USE_16_BIT_NODE_INDICES`): the gate and
  the bench are built with the default 32-bit `OA_NodeIndex`.
* Anything about a *multi-threaded* caller: `OA_Allocator` has no lock and the
  gate is single-threaded.

## 4. What this does not show

* **No cross-library claim.** There is no baseline allocator here that is not
  ours: this is a decomposition of our own paths plus one internal A/B, not a
  comparison against `dlmalloc`, `mimalloc`, `rpmalloc` or `tcmalloc`. Those are
  the numbers a reader would need to know whether the buddy is competitive, and
  we have not measured them.
* **One working-set size.** 4 MiB arena / 100 000 live leaves / 2 MiB metadata.
  A cache-resident buddy (e.g. a 64 KiB arena) would be a different allocator
  performance-wise and is not measured here; the 2.1 ns/step and the 66 ns base
  are properties of *this* size.
* **Absolute ns/op is ±20%** because the guest DVFS is unlocked and stolen time
  is visible in `CV`. Every difference quoted above is a ratio between rows read
  in the same process; do not compare these numbers to another machine's.
* **`arena.bump32` and `pool.alloc_free` are too cheap to time well** at this
  granularity (CV 23% and 5% respectively); they are here as the scale reference,
  not as a fine-grained result.
* The allocator does not zero memory, so no row here pays for that; a caller that
  wants zeroed blocks adds a `memset` on top of every number in this table.

## Reproduce

```sh
make -C perf_test alloc-bench        # 9 rows, 21 samples, taskset -c 0
make -C perf_test alloc-bench-big    # the same source at CHUNK=1024
make -C perf_test alloc-test         # the gate, incl. the unlink-vs-scan equivalence
make -C perf_test offset-test        # offset allocator gate (-DDEBUG, so the
make -C perf_test offset-test-la     # library validates itself after every op):
                                     # the second target is the rejected
                                     # lookahead arm, gated the same way
make -C perf_test offset-bench       # 4 rows, 31 samples
make -C perf_test offset-ab          # the lookahead A/B, exact counters
```

The frequency probe used for field 3 was a 100M-iteration dependent `add` chain
(7 runs, best case quoted); it needs no special privileges and is three lines of
inline asm if it should live in the tree later.
