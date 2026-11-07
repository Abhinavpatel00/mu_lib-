# Data-structure performance record

Evidence behind AGENTS.md §3.15 ("Data Structures Are Intrusive by Default"),
recorded under the Seven-Field Evidence Rule (§2.6.3). Narrative version:
`docs/intrusive.md`, `docs/skiplist.md`.

Reproduce: `make -C perf_test bench-intrusive` (then `perf_test/bench_intrusive 1048576`).

---

## 1. The seven fields

| # | field | value |
| --- | --- | --- |
| 1 | CPU | Intel Xeon (Skylake-class server, no E-cores), `Intel(R) Xeon(R) Processor @ 2.60GHz` |
| 2 | cores / affinity | 2 vCPU (shared cloud guest); every timed run pinned with `taskset -c 0` |
| 3 | frequency | **nominal 2.60 GHz, not measured per run.** No `cycles/task-clock` ratio and no 1-cycle `add` chain was captured, so cycles-per-op below are derived from nominal frequency and are indicative only; the DVFS state of the host is unknown. Ratios (which is all that is claimed) do not depend on it. |
| 4 | compiler / flags | gcc 14.2.0, `-O3 -march=native -std=c11 -Wall -Wextra -Werror` |
| 5 | workload / working set | n = 1 048 576 nodes; payload 4 B + link 16 B; intrusive working set 24.0 MB (payload + embedded link), node variants 4 MB payload + 16 MB nodes = 20 MB in two streams. L1d 48 KiB, L2 1.25 MiB, L3 shared - the working set is **L3-resident at best, DRAM at worst**, so this measures memory behaviour, not ALU. Payloads are initialized with non-zero values before timing (no zero-store elimination) and pages are pre-touched. |
| 6 | baseline | both realistic alternatives: (a) pool allocator, i.e. an *ideal* node allocator that hands out same-size nodes in allocation order (the best case for the non-intrusive design), and (b) `malloc`/`free` per node |
| 7 | method | monotonic wall clock inside the timed region, 5 warmup + 15 paired randomized round-robin reps, **median** of per-rep means, **CV** per side, plus an optimization barrier on the traversed/summed value. Ratios only, never absolute ns as a claim. |

## 2. Intrusive vs node-owning, 6 independent runs

Speedup > 1 means the intrusive list wins.

| operation | 1 | 2 | 3 | 4 | 5 | 6 | claimable |
| --- | --- | --- | --- | --- | --- | --- | --- |
| push vs pooled nodes | 0.91 | 1.35 | 1.28 | 0.65 | 0.87 | 1.29 | **none - tie** |
| push vs heap nodes | 1.10 | 1.13 | 1.14 | 1.80 | 1.30 | 1.08 | 1.1x |
| traverse vs pooled nodes | 1.50 | 1.39 | 1.41 | 1.14 | 1.79 | 1.70 | 1.4x |
| traverse vs heap nodes | 1.30 | 1.28 | 1.29 | 2.13 | 1.98 | 1.41 | 1.3x |
| erase by handle vs pooled | 1.46 | 1.04 | 1.85 | 1.40 | 1.72 | 1.83 | 1.4x |
| erase by handle vs heap | 4.72 | 3.60 | 4.50 | 4.83 | 4.08 | 4.12 | **4.0x** |

Allocations per push: 1 048 576 (pooled), 1 048 576 (heap), **0** (intrusive).
CVs on this host range from 1% to 36% between runs; the table reports the spread
rather than a best-of.

## 3. The claim that is actually algorithmic

Erase given a **payload pointer**, in reverse insertion order (worst case for a
search), n = 8192, microseconds per pass:

| variant | us/pass | ratio vs intrusive |
| --- | --- | --- |
| pooled nodes -> intrusive | 44 411.5 -> **7.3** | 6.1e3x |
| heap nodes -> intrusive | 80 572.0 -> **7.2** | 1.1e4x |

Category: **algorithmic** (AGENTS.md §2.5), not cache. The node variants must map
payload -> node by searching, O(n) per erase, O(n^2) per pass. The intrusive
variant is O(1) per erase. The ratio therefore grows with n; it is not a
constant-factor speedup and must not be averaged with the rows above.

## 4. Losses / what not to claim

* **Push is not faster.** Against a pooled allocator it is a tie (0.65x-1.35x,
  i.e. noise around 1.0). Intrusiveness removes the allocation call, not the
  store; when the allocator is already ideal there is nothing left to win.
* **Traversal gains are modest** (1.3-1.8x) because the pooled baseline is
  already contiguous. The gain comes from touching one cache line per element
  instead of two, not from a layout revolution.
* **16 B per element is not free.** In `mu_skiplist.h` a height-4 node carries
  5 words of link (see `docs/skiplist.md`), which is why that header offers
  `mu_sl_use_embedded` for the small-object case rather than claiming embedded
  links are always better.

## 5. Skiplist and scope stack

Both are gated (`make -C perf_test ds-test`: `test_skiplist` 309 checks,
`test_scope_stack` 36 checks, 0 failures) but **no wall-clock claim is made for
them here** - their value is the API contract (O(log n) insert/erase/search plus
O(log n) *index* access via widths; O(1) bump allocation plus destructor
ordering), and the skiplist is benchmarked against a flat array only as a
sizing sanity check. A skip list loses to a sorted array below a few thousand
elements and to a hash map when order does not matter; it wins when you need
ordered iteration *and* order statistics *and* O(1)-ish concurrent-ish updates.
