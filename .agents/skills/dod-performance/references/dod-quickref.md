# DOD Quick Reference

Companion to `.agents/skills/dod-performance/SKILL.md`. Condensed rules,
defaults, formulas and diagrams. Full narrative: project-root `dodnotes.txt`.

---

## 1. Core transformations

```
struct Someone { something }        →   something[someone_id]

Monolithic object
    ↓ explode
Data + dependencies
    ↓
Components / tables
    ↓
Systems / transforms
    ↓
Process only active data
```

```
enum + switch           →   table membership + specialized transform
"what am I?"            →   "what processing am I currently eligible for?"
object → vptr → method  →   id → table → data
instance → components   →   component → instances
```

```
many identical entities  →  1 record + count
fixed locations          →  location → state   ("positions have wheat")
full entity → simplified → collective → aggregate/count → procedural/implicit
```

---

## 2. Layout defaults

| Situation | Layout |
|---|---|
| Fields used by different passes | SoA (parallel arrays, padded) |
| Fields always used together | AoS (single struct) |
| Same-size objects, churn, need handles | pool + freelist + generations |
| Optional per-entity data | sparse set / side table keyed by id |
| 2-valued flag over large domain | bitset (1 bit each) |
| Fixed grid / slots | index arithmetic, no stored position |
| Search keys only | packed key array + payload array (index after search) |
| Hot + cold fields | split into two tables joined by id |

Do not mechanically apply SoA. Related data consumed together stays together.

---

## 3. Existence-based processing

```
Don't ask "does this object do something?"
Store    "these objects do something."
```

- Hurt entities live in `Damaged[]`, not behind `if (health < max)`.
- Visible entities live in a bitset, not behind `bool visible[]`.
- Subscribed listeners live in the event table; dispatch processes the table.
- `dirty == true` becomes `entity ∈ DirtyTable`.
- State `Running` becomes `entity ∈ Running[]`.

Row-of-truth: `existence / membership IS the state`.

Replacement map:

```
bool flag over many rows   →  bitset
optional component         →  sparse set / side table
enum as flow               →  separate tables
dirty flag                 →  list of things needing work
object hierarchy           →  independent transforms
```

---

## 4. Branch rules

```
branch exists  ≠  branch is unpredictable
unpredictable branch  →  sort/bucket/group data, then re-check
unpredictable select  →  x = c ? a : b  (CMOV / select / min / max)
```

- Cheap predictable work can beat an expensive check that avoids it.
- `if (dirty) update()` may lose to `update()` if the update is cheap.
- Hints (`MU_LIKELY`) only on cold error/init paths.
- Process in homogeneous bursts (A A A B B B C C C) to keep code/data hot.
- Sorting networks: fixed compare-exchange sequence, no data-dependent control.

---

## 5. Memory model

```
L1 ~4 cyc · L2 ~12 cyc · L3 ~40 cyc · DRAM ~80–300 cyc
cache line = 64 B = 16 ints = 8 doubles = 16 floats
memory request  →  whole line  →  use the rest of the line
```

- `address(i) = base + i*stride` — predictable, prefetchable, vectorizable.
- `p = p->next` — next address depends on previous load; no MLP; guaranteed miss.
- Graph → CSR. Tree → wide nodes (B-tree). Fewer hops, fewer stalls.
- Separate read-only / read-write / write-only streams.
- Write contiguously, in large amounts, using the bytes you load.
- Non-temporal stores for large one-shot output (avoid cache pollution).
- False sharing: pad per-thread hot data to 64 B; use local accumulators.
- `possible alias → reload`; `known non-alias → registers, reorder, vectorize`.
  `const` does not prove non-aliasing; `restrict` does.
- Return small structs by value (RVO) instead of out-parameters.

---

## 6. SIMD / auto-vectorization

Vectorizer-friendly loop:

```
contiguous · no early exit · no unknown calls · aligned · no cross-iteration dep
input and output non-aliasing · regular trip count · whole output stream written
```

Breaks vectorization:

```
if (a[i] < 0) break;         data-dependent exit
a[i] = f(a[i]);              opaque call / side effects
a[i] += a[i + 1];            cross-iteration dependency
float *a, *b (may alias)     aliasing
```

A branch where every iteration completes is fine — it becomes a select.

Order of attack: simple scalar loop → check `-fopt-info-vec` → restructure →
intrinsics only for structural lane work the compiler cannot infer.

SIMD wins on: enough data · independent ops · suitable layout · vector-friendly
instructions. It loses on tiny workloads with setup/remainder overhead.

---

## 7. Allocation & lifetime

```
frame arena     → alloc per frame, free by pointer reset
scope arena     → bulk alloc, bulk free
fixed pool      → same-size objects, freelist
buddy / offset  → variable sizes with coalescing
static array    → zero allocation, fixed capacity
```

- One allocator strategy per region. No global malloc sprinkled in hot code.
- Stack is limited: no huge locals, no deep call chains.
- Handles: `{ index, generation }`; stale handles fail a compare instead of
  corrupting memory.
- Deletion: `table[i] = table[--count]` (swap-remove) when order is free.
- Inactive ≠ deleted: keep memory, stop processing.
- Offsets over pointers where identity must survive serialization/realloc.

---

## 8. Concurrency

```
don't make shared memory safe — design so sharing isn't required
T0 → data A · T1 → data B · T2 → data C   →   reduce
```

- One writer per datum. Local accumulators, one final shared update.
- Thread-local output buffers; gather with prefix sums.
- Keep worker data on separate cache lines.
- Map / filter / variable-output / multi-stage patterns.
- Prefix sum = parallel compaction / output offsets / parallel allocation.
- Thread-local ownership beats reference counting; assert owning thread in debug.

---

## 9. Search & sort

```
do not search if derivable · index only hot repeated queries
O(log N) poor locality  can lose to  O(N) excellent locality
bloom filter: definitely absent | possibly present (no false negatives)
maintain top-K incrementally instead of sorting every frame
```

Sort only as much as necessary:

```
full sort: 1e6 ordered | partial sort: top K | quickselect: Nth element
counting/radix sort for known discrete ranges (material/shader/buffer ids)
insertion sort for small/nearly-sorted · merge sort for parallel large
sorting networks for tiny fixed N
maintain sorted state incrementally when updates are small
```

Render order:

```
opaque       → front-to-back (depth rejection kills overdraw)
transparent  → back-to-front (blending depends on order)
cost ≈ texture + mesh + state_change + overdraw   → measure, then choose key
```

---

## 10. LOD, mementos, generation

```
LOD = f(relevance)
relevance = f(distance, screen size, visibility, attention,
              time_since_seen, interaction, progression, expertise, importance)
```

```
LOD0 full → LOD1 simplified → LOD2 approximate → LOD3 collective →
LOD4 aggregate/implicit
far+irrelevant low · far+important high · near+irrelevant medium · near+important high
```

- Collective: `100 aircraft → Squadron{count, attack_probability}`.
- Memento: keep compressed history needed to refine later (lossy is fine).
- JIT memento: `state = Generate(Hash(entity_id))` — store the recipe, not the data.
- Bias, don't uniform-random: `believable = repetition + variation + imperfections`.
- Temporal LOD: 0–10 s full · 10–60 s simplified · 1–10 min compressed · long → seed.
- Interaction history raises importance (player's parked car ≫ ambient car).

---

## 11. FSM / state as data

```
state = table membership
transition = remove from one table + insert into another

old_state + signals → conditions → transition_requests → triggers/log → commit → new_state
```

Evaluate against one consistent snapshot, then commit. Use a sequential FSM
only when signal order is fundamentally the problem (lexer/parser).

---

## 12. Normalization (the relational link)

```
1NF  atomic values only (no lists/nullable members)
2NF  no partial dependency on part of a composite key
3NF  no transitive dependency (A→B→C)
BCNF every non-trivial dependency has a superkey
```

- Start with one big table, then normalize.
- Never store instance state next to definition data.
- More tables can use less memory (optional fields → no nulls).
- Then intentionally **denormalize runtime data** for cache/branch/bandwidth.
- Normalization removes coupling; DOD re-layouts what remains by access pattern.

---

## 13. DOD naming of the recurring patterns

```
PATTERN 1  state → table membership
PATTERN 2  optional state → sparse component
PATTERN 3  scheduled event → sorted time table
PATTERN 4  object identity → ID
PATTERN 5  object → component composition
PATTERN 6  full entity → collective representation
PATTERN 7  dirty flag → list of things that need work
PATTERN 8  object hierarchy → independent data transforms
```

---

## 14. Performance model

```
performance ≈ computation
            + memory traffic
            + cache behaviour
            + branch behaviour
            + synchronization
```

Most important principle:

```
REDUCE UNNECESSARY DEPENDENCIES
    fewer dependent instructions · fewer pointer hops
    fewer unpredictable branches · fewer random writes
    fewer cache-polluting accesses
```

Core question for every conditional optimization:

```
Is avoiding the work cheaper than doing the work?
```

Workflow:

```
MEASURE → identify bottleneck → change DATA/LAYOUT/ALGORITHM → MEASURE → keep or revert
```

---

## 15. Desktop Hardware, SIMD & High-Perf Data Structures Quick Card

```
LAYOUTS:
  Hot/Cold Split AoS (<=32B hot)        7.07x vs bloated OOP AoS
  Pure Aligned SoA + AVX2 (1-6 cols)   11.50x vs bloated OOP AoS
  Tiled AoSoA8 / AoSoA16 (6-20 cols)    3.96x vs strided AoS packets
  Morton Z-Order + ryg Dilated +1       1.37x on out-of-L3 2x2 bilinear fetch

MICROARCHITECTURE & VM:
  Matched 64b/256b Store->Load (no STLF) 4.29x vs narrow-write -> wide-read
  8x Independent FMA Accumulators       20.20x vs 1 serial FMA accumulator
  Lemire fastmod / fastrange32      2.98x / 11.48x vs hardware integer div
  Double-Mapped Magic VM Ring (mu_vm)    2.79x vs wrap split-memcpy
  _mm256_stream_ps (NT write-only)       1.63x faster hot-L2 survival

HIGH-PERF DATA STRUCTURES (`mu/*.h` Wave 3):
  Contiguous CSR Graph (mu_csr)         32.72x vs pointer-chased heap graph
  Flat SIMD SwissTable (mu_swiss_map)    6.44x lookup vs mu_hash32 (% cap)
  mu_dense_map Contiguous SoA Values   178.17x iteration vs sparse hash scan
  Eytzinger BFS + Prefetch (mu_eytzinger)5.92x vs std::lower_bound (16 MB)
  16-Ary Cache-Line AVX2 S+Tree          9.79x vs std::lower_bound (16 MB)
  AVX2 Split-Block Bloom (mu_bloom)      8.31x vs 6-probe scalar mu_bloom
  4-Level HierBitset (mu_hier_bitset)  318.68x vs flat 16.7M bitset (sparse)
  Roaring Hybrid (8KB Bitset + u16[])  209.45x vs sorted uint32[] merge
  Static Xor8 / BFuse8 (mu_xor_filter)   5.37x + 38% smaller vs scalar Bloom
  1-Hist LSD Radix + FloatFlip (mu_sort)18.67x vs libc qsort() on f32
  SoA 4-Heap / Timer Wheel              2.25x / 15.42x vs AoS binary heap

COMPILER, LOOP TILING & SPECIAL ISA:
  Loop Interchange i-k-j / 4x16 Tile    20.07x / 44.13x vs naive i-j-k GEMM
  3-Lane CRC32C / 4-Lane AES-NI Hash    32.28x / 76.59x vs FNV-1a (54.8 GB/s)
  Lemire SIMD StreamVByte (mu_fast_codec)43.93x vs Protobuf LEB128 (5.6B ints/s)
  GFNI vgf2p8affineqb 8x8 Bit-Matrix    93.20x vs scalar shift/mask (87.8 GB/s)
  Associative SIMD pshufb DFA           8.84x vs table DFA state=dfa[s][c]

SEPARATED MULTITHREADING (load skill `multithreading-dod`):
  Local Register Reduce / 64B Pad   71.76x / 4.20x vs MESI false sharing
  Linux Seqlock (mu_sync)               27.44x vs pthread_rwlock_t
  Batched+Shadow SPSC (mu_spsc_queue)  127.12x vs mutex ring buffer
  SLUB / Mesa Per-Thread Slab (mu_slab) 932.84x vs shared mutex pool
  Naughty Dog 64KB User-Space Fiber   1301.95x vs OS kernel thread wake
```

---

## 16. Quantitative Models, Top-Down TMA & Seven-Field Benchmark Rules

```
ROOFLINE & ECM:
  Intensity I = FLOPs / Bytes_moved ;  P <= min(P_peak, I * BW_stream_mix)
  Stream mixes differ: Read != Copy (1R+1W) != Triad (2R+1W) != Fill/RFO
  CARM plots L1/L2/L3 roofs; ECM models partial x86 cache-transfer overlap

INTEL P-CORE TOP-DOWN LEVEL-1 THRESHOLDS (TMA_Metrics-full.xlsx v5.2):
  Frontend_Bound  > 15%  -> I-cache/iTLB/BTB (BOLT/outline) or DSB/JCC 32B
  Bad_Speculation > 15%  -> Branch_Mispredicts or Machine_Clears (4K alias)
  Backend_Bound   > 20%  -> L2 split: Memory_Bound vs Core_Bound (chains/div)
  Retiring        > 70%  -> Check SIMD width! Scalar 4-wide still shows >75%!
  * Check counter running % == 100% (no multiplexing); keep cpu_core != cpu_atom
  * Zero-skid sampling: Intel `cycles:pp` (PEBS), AMD `ibs_op`, Arm `arm-spe`
  * Pre-flight: `perf_event_paranoid <= 1`, DWARF `-g`-only rebuild (drop stale perf.data)
  * 7-Pattern `perf annotate` scan: scalar FP (ignore zeroing `vxorpd`), narrow XMM/YMM,
    serial accum (`src==dst` + IPC<0.5), in-loop `shufps`/`vhaddps`, alias preamble
    (`restrict`), load wall, & outdated `.so` (`libcrypto.so.3` -> OpenSSL 3.3+)
  * Cold callee outlining: `__attribute__((cold))` on DEFINITION; verify <0.1% taken via
    PMU `BR_INST_RETIRED.NEAR_TAKEN:upp` (macrofusion fix) or GCC `-fdump-tree-profile_estimate-lineno`

SEVEN-FIELD EVIDENCE RULE (every benchmark number requires all 7):
  1. CPU model & uarch (+ P-core vs E-core)  5. Workload & byte working set
  2. Core count & affinity                   6. Explicit baseline
  3. Measured core GHz (NOT rdtsc!), DVFS/SMT7. Timer (`CLOCK_MONOTONIC_RAW`),
  4. Compiler, version & exact flags            `SINK`+`OPAQUE`, >=21 paired
     round-robin reps, min (latency) / median (throughput) / geomean (ratios)
```

---

## 17. One-liners worth memorizing

```
The wheat doesn't have positions; the positions have wheat.

Don't build an object because the thing has a name;
build the data required to represent the work the program actually does.

LOD is the resolution of existence, not polygon count.

Make data flow simple enough that the CPU and compiler can see
the independence you already know.

The compiler can eliminate instructions.
It cannot rescue a fundamentally poor data layout.
```
