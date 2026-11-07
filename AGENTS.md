 AGENT.md — Data-Oriented C for Maximum Performance

> **What this file is.** Operating instructions for any AI agent (or human)
> writing C in this repository. It encodes one person's C style, the
> Data-Oriented Design (DOD) doctrine, and the rules for using the bundled `mylib`
> (`mu`) library.
>
> **Prime directive:**
> **Lay data out for the loop you will run, remove every branch the layout
> makes unnecessary, own your memory explicitly, reduce dependencies, and
> measure everything you claim.**
>
> Sibling documents (read them, they are part of this contract):
> - `dodnotes.txt` — the human-readable DOD, hardware, SIMD & data-structure notes.
> - `perf_test/PERF_PROOFS.md` — runnable empirical C microbenchmark proofs (`make run-proofs` & `make run-mt-proofs`).
> - `.agents/skills/dod-performance/SKILL.md` — core single-threaded DOD & memory layout skill.
> - `.agents/skills/simd-hardware-c/SKILL.md` — desktop x86-64 microarchitecture, SWAR, BMI2, AVX2 & AVX-512 skill.
> - `.agents/skills/game-engine-dod/SKILL.md` — game/engine frame, ECS, culling, animation & rendering DOD skill.
> - `.agents/skills/multithreading-dod/SKILL.md` — **strictly separated** multithreading, lock-free, kernel sync & fiber skill.
> - `docs/making_a_demo.txt` — everything needed to add a graphics demo or
>   prototype (App/vk.h API, shader + push-constant rules, staging-ring uploads,
>   Makefile recipe, hot reload). Read this before creating or extending
>   anything under `demo/`.
>
> **Structural rule on Multithreading:**
> Single-threaded desktop hardware efficiency (cache lines, TLB, ILP, branchless
> execution, full SIMD/AVX2/AVX-512) ALWAYS comes first. Multithreading is
> deliberately isolated in **Part II (`§M.1–§M.12`)** at the end of this file and
> in `.agents/skills/multithreading-dod/` so concurrency is never mixed into
> single-core data-layout design.

> **Code examples in this file are illustrative fragments, not standalone
> compilable programs.** They show layout, control flow and API shape. When
> writing real code, compile it with the flags in §12.7 and measure it.

---

## 0. The one-paragraph summary

Programs are **data + transformation**. Stop organizing memory around objects;
organize it around the operations that consume the data. Design tables and
arrays whose *shape* makes the hot loop simple: contiguous, predictable,
branch-light, dependency-light. Represent state by **existence** (table
membership, bitsets, sparse sets) instead of by flags and `switch`es. Use
**IDs/handles** instead of long-lived pointers. Prefer **static, fixed-capacity,
or arena-backed** storage. When a reusable primitive is missing, **add it to
`mylib`** rather than hand-rolling a one-off. Then **measure** — the compiler
can eliminate instructions, but it cannot rescue a fundamentally poor data
layout.

---

## 1. The non-negotiables (read this first)

1. **Data layout before algorithms.** Choose the memory layout from the access
   pattern, then pick the algorithm. Not the other way round.
2. **IDs over pointers.** Long-lived references are `EntityId`, `MeshId`,
   `TextureId`, `Handle`, or an array index — never a raw pointer held across
   frames.
3. **Existence over flags.** If a thing needs processing, it lives in a table
   for that processing. Do not scan 100,000 entities to find the 1,000 that
   are hurt.
   Flags that introduce `switch`es/`if`s in a hot loop are a layout bug.
   Do not store "what am I" per row and branch on it — store *eligibility*
   as table membership and run one specialized transform per table:

   ```c
   /* WRONG: one mixed array, switch inside the loop */
   for (size_t i = 0; i < n; ++i)
       update_entity(&entities[i]);       /* switch inside */

   /* RIGHT: separate flat arrays, one specialized loop each */
   for (size_t i = 0; i < player_count; ++i)     update_player(&players[i]);
   for (size_t i = 0; i < enemy_count; ++i)      update_enemy(&enemies[i]);
   for (size_t i = 0; i < projectile_count; ++i) update_projectile(&projectiles[i]);
   ```

   A per-row field used only as a *load index* (mesh id, material id) is
   not a flag. A per-row field ever tested with `if`/`switch`/`?:` on a
   data-dependent condition inside a loop over rows is a flag: delete it,
   split the table, specialize the kernel. When in doubt between one
   mixed array plus a branch and separate flat arrays plus one
   specialized loop each, choose the separate arrays — moving rows
   between tables at insert/remove time is cheaper than branching on
   every row every frame.
4. **No pointless null checks.** Eliminate nulls by establishing *invariants*
   at boundaries, not by nesting `if (p && p->q && p->q->r)`.
5. **No data-dependent branches in hot loops.** Sort/bucket/partition the data,
   use predication (`x = c ? a : b`), masking, or `min`/`max`.
6. **Pointer chasing is the one unforgivable sin.** Each hop is a guaranteed
   cache miss with no prefetch to save you. Store graphs as arrays (CSR).
7. **One atomic value per cell (1NF).** No arrays-in-rows, no nullable members.
8. **Optional facts become membership in another table**, not a nullable field.
9. **Write contiguously, read contiguously.** Sequential or strided only.
10. **`restrict` on every non-aliasing pointer pair**, and make independence
    visible to the compiler.
11. **Own your memory.** One allocator strategy per program region; arenas,
    pools, handles. Global `malloc` sprinkled through hot code is a bug.
12. **Measure before and after.** No optimization without a number. Use the
    `mylib` timer (§9.6).
13. **Prefer `mylib`.** If `mylib` lacks a primitive you need, add a small
    reusable primitive to `mylib` — never invent a competing allocation or
    ownership model in engine code.
14. **Do not blindly apply any of this.** Every rule is conditional on the
    workload. SIMD, SoA, branchless code and non-temporal stores all have
    losing cases. That is why rule 12 exists.

---

## 2. Mindset and workflow

### 2.1 The two mindsets

```
OOP mindset                          DOD mindset
-----------                          -----------
WHAT is this object?                 WHAT DATA exists?
What does it own?                    What TRANSFORMS operate on it?
What methods does it have?           Which data actually needs each transform?
Who calls those methods?             How should that data be stored?
```

### 2.2 The transformation

```
MONOLITHIC OBJECT
      ↓  explode into elements
IDENTIFY DATA + DEPENDENCIES
      ↓
COMPONENTS / TABLES
      ↓
SYSTEMS / TRANSFORMS
      ↓
PROCESS ONLY ACTIVE DATA
```

### 2.3 The design questions to ask, in order

Ask these before writing a line of code:

- What data does this operation **actually need**?
- What is the **access pattern** (sequential? random? one-shot? read-only?
  read-write? write-only?).
- What is the **lifetime** (per frame? per level? permanent?).
- What **can be derived** instead of stored?
- What **can be grouped** (many identical things → one record + count)?
- What **can be regenerated** from a seed instead of persisted?
- What **does not need to exist individually** right now?

### 2.4 The optimization loop (always closed with a re-measure)

```
DEFINE   → what is slow? what is the target?
MEASURE  → get a number (MU_SCOPE_TIMER / perf / timings)
ANALYSE  → read the data shape; read the generated assembly of the hot 5%
IMPLEMENT→ change data layout / algorithm (not micro-tweaks first)
CONFIRM  → measure again; keep only if the real workload improved
```

Never skip the last step. "Our benchmark proved the clever fused-loop version
was slower, because of cache behaviour" is a real and common outcome.

### 2.5 What to prioritize when optimizing

```
1. Memory bandwidth
2. Cache locality
3. Contiguous data access
4. Reduced pointer chasing
5. Batch processing
6. Predictable control flow
7. Reduced branches
8. Instruction count
```

Optimize **data movement before instruction count**. The compiler can remove
instructions; it cannot fix a bad layout.

**Bytes moved is sometimes a design parameter, not a consequence.** When a
structure copies inside a fixed-size block, the copy volume per mutation is
`~block/2` regardless of how much data the structure holds, so total movement
scales with *edit count*, not with size. A 16 KiB segment in `mu_ted` moved
**1344.6 MB** to build a **102 KB** document from a real typing trace —
5176 B per one-byte insert. Making the fill target a function of document size
(`clamp(text_len/32, 2048, 16384)`) cut movement 7.5x and editing time 1.49x
with the index untouched. Before tuning an index, instrument the copy macro
and count bytes: "index too slow" and "block too big" look identical in wall
clock and have opposite fixes. Saturation matters — above `MIN_SEGS x
SEG_TEXT` the target pins to the old constant, so large documents take
provably identical decisions. See
`.agents/skills/text-buffer-dod/references/chunked-text-buffers.md`.

### 2.6 Quantitative Models (Roofline, ECM, Top-Down TMA) & The Seven-Field Evidence Rule

*(Sources: Williams Roofline CACM'09, Ilic CARM'13, Stengel/Hager ECM'15, Ahmad Yasin ISPASS'14, Intel `TMA_Metrics-full.xlsx` v5.2, `usamahz/cpu-performance-engineering`)*

1. **Roofline, Stream-Mix Roofs & ECM Overlap:**
   - Bound every hot loop before micro-tuning: $P \le \min(P_{\text{peak}},\, I \times BW_{\text{roof}})$, where $I = \text{FLOPs}/\text{Bytes}$ and $I_{\text{ridge}} = P_{\text{peak}} / BW_{\text{roof}}$.
   - Match the memory roof to the exact stream mix (`1R` read-only $\ne$ `1R+1W` copy $\ne$ `2R+1W` triad $\ne$ `1W` fill/RFO), use **Cache-Aware Roofline (CARM)** for L1/L2/L3-resident kernels, and use **ECM** ($T_{\text{ECM}} = \max(T_{\text{core\_OL}}, T_{\text{L1\_nOL}} + T_{\text{L2}} + T_{\text{L3}} + T_{\text{Mem}})$) when cache-to-cache transfers only partially overlap on x86. A point above a roof means a wrong roof or wrong byte count, never magic code.
2. **Top-Down Microarchitecture Analysis (TMA) Pipeline-Slot Thresholds:**
   Run `perf stat -M TopdownL1,TopdownL2` (`toplev` on Intel, `perf stat -M PipelineL1` on AMD Zen 4/5, `TopdownL1` on Arm; never mix `cpu_core` P-core and `cpu_atom` E-core counters, and verify counter running share is `100%` — not multiplexed!). Official Intel P-core Level-1 flagging thresholds:
   - `Frontend_Bound > 15%` → `Fetch_Latency` (I-cache/iTLB/BTB misses: outline cold code, run `BOLT`) vs `Fetch_Bandwidth` (DSB µop-cache misses, Skylake 32B JCC erratum).
   - `Bad_Speculation > 15%` → `Branch_Mispredicts` (sort, existence tables, SIMD blend) vs `Machine_Clears` (4K aliasing, memory-ordering disambiguation).
   - `Backend_Bound > 20%` → check Level 2: `Memory_Bound` (`L1/L2/L3/DRAM/Store` → SoA, hot/cold split, Eytzinger/S+Tree, 2MB hugepages) vs `Core_Bound` (8× FMA accumulators, kill `div`/`sqrtf`).
   - `Retiring > 70%` → pipeline slots are retiring, **now check SIMD width & uop count** (scalar loops can report 80% Retiring while running 8× slower than AVX2!).
   - Sample hot lines **without interrupt skid** (precise interrupts land on the oldest unretired ROB-head stall): use `perf record -e cycles:pp` (Intel PEBS), `ibs_op` (AMD IBS), or `perf arm-spe`.
3. **The Seven-Field Evidence Rule & Honest Microbenchmarking:**
   Never quote a performance number unless all **7 fields** are recorded: **(1)** CPU model & µarch (+ P-core vs E-core), **(2)** core/thread count & affinity, **(3)** measured core frequency (via `cycles/task-clock` or 1-cycle `add` chain — invariant `rdtsc` counts reference time, NOT core clocks under DVFS!), turbo/DVFS & SMT state, **(4)** compiler & exact flags, **(5)** workload & byte working set vs cache sizes (initialized with non-zero values to avoid hardware zero-store elimination!), **(6)** baseline, and **(7)** method (`CLOCK_MONOTONIC_RAW`, `SINK(res)` + `OPAQUE(ptr)` anti-hoist barriers, warmup + `>= 21` paired randomized round-robin reps, reporting **min** for latency floors, **median** for throughput, **geometric mean** for normalized ratios, plus `CV`).
4. **Structured `perf` Pre-Flight, `perf annotate` Assembly Pattern Scan & `.so` Version Audit (`intel/intel-performance-skills`):**
   - **Pre-flight:** Ensure `/proc/sys/kernel/perf_event_paranoid <= 1` and verify DWARF info (`objdump --dwarf=info bin | grep -c DW_TAG`). If stripped, rebuild adding **only `-g`** (never touch `-O2`/`-O3` or `-D` defines; discard any prior stale `perf.data`, and use `addr2line -e bin -f -i` for inline frames).
   - **7-Pattern `perf annotate --stdio -l <fn>` Checklist:** **(a) Scalar FP only** (`vaddsd`/`vmulsd` `>50%`, zero packed compute ops — ignore register-zeroing `vxorpd`/`vmovapd` moves!) → fix stride/`restrict` & vectorize; **(b) Narrow SIMD** (`xmm` on AVX2/512 host or `ymm` on AVX-512 host `>20%`) → widen via `-march`, dispatch, or Zipper (§7.16); **(c) Serial accumulator** (1 FP op `>40%` + `src == dst` reg + IPC `< 0.5` & LLC miss `< 2%`) → unroll 4–8 accumulators; **(d) In-loop horizontal reduction** (`shufps`/`unpckhps`/`vhaddps` within 3–5 ops of `vmulps` `>10%`) → keep vector accumulators vertical until loop exit; **(e) Aliasing preamble** (`lea` + `cmp` overlap guard jumping to scalar clone) → add `MU_RESTRICT`; **(f) Load wall** (`vmovups`/`movq` `>30%` + high LLC MPKI) → fix layout (SoA/CSR/S+Tree); **(g) Outdated system `.so` symbols** (e.g., `gcm_init_avx`, `aesni_xts_encrypt`, or pre-3.3 `ossl_aes_gcm_encrypt_avx512` in `libcrypto.so.3` → check `readlink -f` or `openssl version` / `strings libcrypto.so.3 | grep "^OpenSSL 3"`, upgrade to OpenSSL 3.3+).

---

## 3. Data layout — the core of the craft

### 3.1 Tables, not objects

```c
/* WRONG: a struct is a category box, and the CPU has to drag all of it around */
typedef struct Enemy {
    Vec3    position;
    Vec3    velocity;
    float   health;
    float   cooldown;
    char    name[64];
    struct Enemy *target;      /* pointer chasing */
    bool    dead;
    bool    visible;
} Enemy;

/* RIGHT: parallel arrays indexed by id; each system sees only its own data */
typedef uint32_t EntityId;

Vec3   positions[MAX_ENTITIES];
Vec3   velocities[MAX_ENTITIES];
float  health[MAX_ENTITIES];
```

A struct with 20 members is five systems pretending to be one type. **Split it
by consumer, not by category.**

### 3.2 SoA vs AoS — decide from the access pattern

- **SoA** (structure of arrays) when fields are processed **separately** and
  the loop is a stream. Enables SIMD, smaller working sets, fewer wasted bytes.
- **AoS** (array of structs) when fields are **always consumed together** as a
  unit (e.g. `Vec3 x,y,z` used as one vector; a config row; a file header).

```c
/* SoA: physics only touches position + velocity */
for (size_t i = 0; i < count; ++i)
    positions[i] += velocities[i] * dt;

/* AoS: the whole record is the unit of work */
for (size_t i = 0; i < header_count; ++i)
    parse_header(&headers[i]);
```

The animation-keyframe rule: **binary-search keys in a packed array; index the
payload once.** Searching `{time, x, y, z}` loads 3 fields you do not need.

```c
typedef struct { float x, y, z; } KeyData;

typedef struct {
    float   *times;    /* only this is searched  */
    KeyData *values;   /* touched once after the search lands */
    int      count;
} AnimStream;
```

### 3.3 Hot / cold split

Fields touched every frame and fields touched at spawn/save time belong in
different tables joined by id. This shrinks the hot working set.

```c
/* hot: 20 bytes/entity */
Vec3  positions[MAX_ENTITIES];
Vec3  velocities[MAX_ENTITIES];

/* cold: touched on spawn, save, UI */
char  names[MAX_ENTITIES][32];
uint32_t model_ids[MAX_ENTITIES];
```

### 3.4 One atomic value per cell (1NF)

Every field is a single atomic value; no array/list/set inside a row, no
nullable members. Optional facts become **membership in another table**.

### 3.5 Normalize the design, then denormalize the runtime

Start from one big table of everything, then normalize:

```
1NF  — atomic values only; no repeating groups.
2NF  — no partial dependencies on part of a composite key.
3NF  — no transitive dependencies (A→B→C means B belongs in its own table).
BCNF — every non-trivial dependency X→Y has X as a superkey.
```

Example: `weapon_id → weapon_type → type_description`.

```c
/* Weapon table holds only weapon-specific data */
typedef struct { uint32_t id; const char *name; uint16_t type_id; int damage; } Weapon;

/* Type table holds the shared description once */
typedef struct { uint16_t id; const char *name; const char *description; } WeaponType;
```

More tables can use **less** memory when the denormalized version has many
repeated/optional fields and nulls. But a game runtime also cares about cache
locality, branching, bandwidth, SIMD, parallelism and latency — so
**intentionally denormalize runtime data** where the hot loop needs the join
to be free. Normalization is a design tool, not a religion: let experimentation
and experience decide how far to push it.

Also: **do not store instance/state data next to definition data.** A quest's
name/description/reward belong to `Quest`; a player's progress belongs to
`PlayerQuest`. This removes an entire class of desync bugs.

### 3.6 Let structure provide identity and position

If the world already provides fixed locations — grids, cells, tiles, inventory
slots, voxels — **do not make an object for every location**. Store
`location → state`.

```c
/* 10,000 wheat tiles, zero objects, zero pointers, zero per-object lifetime */
uint8_t wheat_100x100[10000];   /* 0 = empty, 1..100 = growth */

int index = y * width + x;
int x = index % width;
int y = index / width;
```

> "The wheat doesn't have positions. The positions have wheat."

The storage representation does **not** have to match the logical geometry
(square, hex, triangle, irregular all map onto regular arrays + specialized
traversal functions).

### 3.7 If individual identity does not matter, use a count

```
N identical entities  →  1 record + count
```

```c
typedef struct { uint32_t type; uint32_t count; } ItemStack;
```

Applies to arrows, bullets, plates, resources, identical inventory items,
distant squadrons, crowds, swarms, forests.

Ask: **"How would a stranger describe this?"** They say "there is furniture,"
not "chair #1, chair #2, bookshelf #3." Match the player's mental model.

### 3.8 Separate read-only / read-write / write-only streams

Make the memory roles explicit. It documents the transform and gives the
compiler type-level independence.

```c
void integrate(/* read/write */ Vec3  *positions,
               /* read      */ const Vec3  *velocities,
               /* write     */ int   *out_dirty,
               size_t count, float dt);
```

### 3.9 Alignment, sizes, powers of two

- Prefer power-of-two capacities, then mask instead of modulo:
  `i & (cap - 1)` (a division-free, branch-free wrap).
- Give the compiler useful guarantees: aligned buffers (`MU_ALIGN`), regular
  counts (pad `count` to a multiple of 4/8). Padding a few unused elements is
  often cheaper than a scalar remainder loop.
- Keep struct sizes cache-line friendly; pad deliberately where false sharing
  or alignment matters.

### 3.10 Sorting data along the processing axis

Sort or bucket by the axis you process: position (spatial locality), update
rate (LOD buckets), type (homogeneous transforms), material (render batches).
A sorted table also makes branches predictable and joins mergeable.

### 3.11 Hybrid Tiled `AoSoA` (`SOA8` / `SOA16`) — Best of AoS Locality + SoA Unit-Stride SIMD

*(Sources: Intel ISPC, Intel Embree, DreamWorks MoonRay; **3.96× faster** in `perf_test/bench_memory_cache.c` Case 2)*

Pure `SoA` (`float *x, *y, *z, *nx, *ny, *nz, *u, *v`) opens **8 separate memory streams** — great for sequential full-array scans, but if you index random packets/buckets, 8 streams cost **8 separate cache-line & DTLB misses** per lookup! Pure `AoS` touches 1 cache line per point, but requires slow strided SIMD `vgatherdps` or scalar shuffles.

**Solution (`AoSoA8` for AVX2, `AoSoA16` for AVX-512):** Tile components in blocks matching the SIMD register width (`W = 8` floats = 32 bytes for AVX2; `W = 16` floats = 64 bytes = **exact 1 cache line per field** for AVX-512):

```c
typedef struct MU_ALIGN(64) {
    float x[8],  y[8],  z[8];
    float nx[8], ny[8], nz[8];
    float u[8],  v[8],  r[8];
} ShadeBlockSOA8; /* Every _mm256_load_ps is 32B-aligned unit-stride, AND 1 packet stays local in L1/DTLB! */
```

- **Use Pure `SoA`** (`mu_soa.h`) when streaming linearly over 1–6 hot columns.
- **Use `AoSoA8` / `AoSoA16`** when packets have 6–20 fields or are indexed semi-randomly by tile/material/ray-bucket.
- **Use Hot `AoS`** when scalar random point-lookups touch *every* field of one small (`≤ 32B`) record at once.

### 3.12 Space-Filling Curves (Morton Z-Order) & Fabian Giesen Dilated-Integer Stepping

*(Sources: Fabian Giesen 'Texture tiling and swizzling', id Software Virtual Textures, Christer Ericson; **1.37× faster** on 256 MB 4096² RGBA32F bilinear fetches in `perf_test/bench_memory_cache.c` Case 5)*

Row-major `grid[y * width + x]` has 1D horizontal locality (`+x`), **zero vertical locality (`+y` jumps by `width * sizeof(T)` — e.g. 64 KB = 16 OS pages on a 4096-wide RGBA32F texture!)**. Interleaving X and Y bits into a **Morton Z-order index** (`y15 x15 ... y1 x1 y0 x0`) maps every aligned `2×2`, `4×4`, or `8×8` 2D tile into contiguous memory on the **same cache line and 4 KB TLB page**:

```c
/* Hardware BMI2 encode (3 cycles on Haswell+/Zen3+): */
static inline uint32_t morton2d_encode(uint32_t x, uint32_t y) {
    return _pdep_u32(x, 0x55555555u) | _pdep_u32(y, 0xAAAAAAAAu);
}

/* Fabian Giesen's Dilated-Integer Arithmetic Trick — step +1 in X or Y WITHOUT decoding!
 * Filling the other axis's bits with 1s lets carry propagate cleanly across interleaved bits: */
uint32_t mx_plus_1 = (mx - 0x55555555u) & 0x55555555u;   /* +1 along X in Morton space */
uint32_t my_plus_1 = (my - 0xAAAAAAAAu) & 0xAAAAAAAAu;   /* +1 along Y in Morton space */
uint32_t morton_neighbor = my_plus_1 | mx_plus_1;
```

> **Caveat (Rule 14):** On AMD Zen 1 / Zen 2, `_pdep_u32` / `_pext_u32` were microcoded (~18 cycles); use dilated-integer increments inside loops or the shift-and-mask magic-bits sequence when targeting pre-Zen3 AMD.

### 3.13 Struct Field Ordering, Implicit Padding & Quantized Domain Compression

*(Sources: Christer Ericson 'Real-Time Collision Detection', Valve Sergiy Migdalskiy, Mike Acton)*

1. **Order struct fields from largest alignment to smallest** (`double`/`uint64_t`/pointers -> `float`/`uint32_t` -> `uint16_t` -> `uint8_t`) and verify with `sizeof` / `MU_STATIC_ASSERT` or `pahole`. An accidental `{ uint8_t a; uint64_t b; uint8_t c; uint64_t d; }` wastes **14 bytes (44%)** to silent ABI padding! Never use `__attribute__((packed))` on hot structs — unaligned cross-line loads cost far more than sorting fields cleanly.
2. **Quantize floats & indices (`mu_bitpacking.h`):**
   - **24B float AABB -> 6B `uint16_t` quantized AABB** relative to parent node/chunk bounds (`4×` more BVH/grid boxes per 64B cache line!).
   - **Unit normals / quaternions:** Octahedral normal encoding (`2 × int16_t` = 4B instead of 12B `Vec3`) + smallest-three quaternion (`48–64 bits` instead of 16B).
   - **Narrow indices:** If `MAX_JOINTS <= 256` or `MAX_MATERIALS <= 65535`, store `uint8_t` / `uint16_t` instead of `size_t` / `void*`.

### 3.14 LLVM `PointerIntPair` Low-Bit Tagging & High-Bit Handle Packing

*(Sources: LLVM Programmer's Manual `PointerIntPair`, Linux Kernel `maple_tree` / `rb_node`)*

Any pointer aligned to `2^K` bytes has its **lowest `K` bits guaranteed zero** (`K = 3` for 8B-aligned heap/arena allocations -> 3 free bits; `K = 6` for 64B cache-line-aligned nodes -> **6 free bits**!). Pack small discriminants, visited flags, or red/black colors directly into the pointer/handle word instead of adding an 8-byte padded struct field:

```c
#define PTR_TAG_MASK ((uintptr_t)7u)
static inline uintptr_t ptr_pack(void *p, unsigned tag) { return (uintptr_t)p | (tag & 7u); }
static inline void     *ptr_get(uintptr_t v)            { return (void *)(v & ~PTR_TAG_MASK); }
static inline unsigned  ptr_tag(uintptr_t v)            { return (unsigned)(v & PTR_TAG_MASK); }
```

### 3.15 Data Structures Are Intrusive by Default

*(Evidence: `perf_test/bench_intrusive.c`, `perf_test/PERF_DS.md`; implementations: `mu/mu_dlist.h`, `mu/mu_skiplist.h`, `mu/mu_rb_tree.h`)*

**The rule.** If an object has to be a member of a container, put the link **inside the object**. The container stores no nodes, owns no payload, and allocates nothing. Before writing a container that allocates one node per element, justify why the object cannot carry the link.

This is not a style preference. It changes three measurable things, and they are measured separately so nobody has to take them on faith:

| what it buys | measured (1M elements, 6 independent runs, median of paired ratios, `taskset -c 0`) |
| --- | --- |
| **Zero allocations** | 1 048 576 `malloc`/`free` pairs -> **0** |
| **One indirection, not two** | traverse **1.3x** vs heap nodes (up to 2.1x), **1.4x** vs pooled nodes (up to 1.8x) |
| **O(1) removal from the object** | erase by handle **4.0x** vs heap nodes (stable, 3.6-4.8x over 6 runs); erase given a payload pointer is **O(n) -> O(1)** (`6.1e3x` at n=8192, ratio scales with n) |
| push | **1.1x** vs heap nodes, and **no win at all** vs pooled nodes (0.65-1.35x across runs: a tie, sometimes a loss) |

Read that table honestly: the win is **not** "intrusive layouts are magic in cache". Traversal against *well-pooled* nodes is 1.41x, and push is a wash, because a bump-allocated node array is already contiguous. What intrusiveness actually buys is (a) that you are **not forced to allocate per element**, and (b) that **the object can find its own place in the structure**, which is what turns find-then-erase from O(n) into O(1) - a reformulation, and the only row in that table that grows with n. The cache-locality advantage only appears when the alternative is one `malloc` per node; against a good pooled allocator it is a 1.4x coefficient, not a structure of the world.

**Requirements for any intrusive container in `mu/`:**
1. The link is a small POD (`mu_dlink` is two pointers; `mu_rb_node` is parent+left+right) embedded by value in the user's struct.
2. Iteration returns the link; the caller recovers the payload with `mu_container_of`. The container never knows the payload type.
3. Removal takes the link, never a key. If you find yourself writing `container_erase(container, key)` on a list or tree, you have re-created the O(n) problem.
4. An object may be in **several** containers at once by embedding one link per container. Never multiplex a single link across containers.
5. Provide `*_is_linked()` so a double erase or a double push is a checkable bug, not corruption.
6. Document the memory cost: `mu_skiplist` nodes borrow pooled per-level arrays (recycled, so steady-state zero allocation), because a variable-height link cannot be embedded at fixed size without wasting the average case.

**When NOT to make it intrusive** (and say so in the header):
- The object must live in many containers and embedding a link per container is bigger than the container's own node would be.
- The object's lifetime is shorter than its membership (a temporary key in a lookup structure).
- The layout is a hot SoA stream where a link field would break the unit-stride access pattern (section 3.2 wins over 3.15).
- You need stable references while the container is copied or moved: an intrusive container must never be
  shallow-copied, because both copies would then share links.

**Checklist before shipping a container:**
- [ ] Can a caller erase an element in O(1) using only the element? If no, why not?
- [ ] Is there any allocation in the steady state? If yes, is it pooled and recycled, and does a test prove the recycling (`test_skiplist.c` asserts it)?
- [ ] Does the header state the link's size and what happens when the payload is memset to zero?
- [ ] Is there a structural validator (`mu_sl_validate`) for tests to call?
- [ ] Benchmarked against the node-owning alternative, with allocations counted, not just ns?

### 3.16 Transform Normals With the Cofactor Matrix, Not `inverse(M)^T`

*(Source: graphitemaster, `normals_revisited`; implementation `mu_mat3_cofactor` / `mu_mat4_normal` in `mu/mu_math.h`; evidence `perf_test/bench_normals.c` + `perf_test/PERF_NORMALS.md`; narrative `docs/normals.md`)*

**The rule.** A normal is transformed by the **cofactor matrix** `C = det(A) * inverse(A)^T`, which for columns `a0, a1, a2` is `[a1 x a2, a2 x a0, a0 x a1]`. Never write `transpose(inverse(M)) * n`.

Two reasons, and the second one is why this is a rule and not a micro-optimization:

1. **`inverse(M)^T` is the wrong operator.** It is derived from `n' · t' = 0`, which admits both orientations and silently picks the wrong one when `det(M) < 0`. A reflecting transform then produces a normal that points into the surface, every `max(0, dot(n, l))` evaluates to 0, and the object is lit inside-out. Correctness bug, not a performance bug - so no benchmark threshold would ever have caught it.
2. **It is 3.1x cheaper** on 4x4 (3.58 ns vs 11.03 ns, cv 1.1%/1.2%, 21 samples, 64 skewed matrices, one pinned core): three cross products and a 3x3 multiply instead of a full 4x4 adjugate plus two more passes. On 3x3 it is only 1.13x, but the reflection bug is identical.

Consequences for other code:
- The result of `C` is **not** normalized and scales by `det`; normalize at the call site if the consumer needs unit length.
- A rotation matrix needs no special path (`C == A`); a uniform scale gives `C == s^2 * A`; the translation column is never read.
- `test_math.c` asserts the reflection case in both directions: the cofactor normal keeps `-z`, and `inverse(M)^T` returns `+z`. That second assertion exists so that "simplifying" the function back to an inverse-transpose fails the build with an explanation instead of shipping.

### 3.17 A Correct Hash Table Can Still Be Quadratic - Hash the Key You Stored

*(Source: `mu/mu_shapes.h` subdivision and weld; evidence `perf_test/PERF_SHAPES.md` + the frozen `ref.*` rows in `perf_test/bench_shapes.c`; narrative `docs/shapes.md`)*

**The rule.** In an open-addressed table that stores `key + 1` so that `0` can mean "empty slot", the **lookup must hash the raw key and then offset it** - never hash the offset key. And before rewriting an inner loop for cache behaviour, verify its probe count.

The subdivision table stores `pair + 1`. Insert hashed the raw pair; lookup hashed the offset pair. Both correct: the linear probe walks from the wrong home bucket to the right slot. What nobody notices is the cost - the walk starts at a random dense position and scans until it hits the entry, so every lookup is `O(load factor)` and subdivision is `O(n^2)`. A 20 480-triangle mesh subdivides in **305 ms** instead of **0.8 ms**; icosphere level 5 goes from 175.9 ms to 5.1 ms (34.6x) once the probe hashes the raw key, and the probe counter in the same build drops from **16 081 678 to 3 840**.

The same lesson in the weld: a tolerance search that probes the textbook 3x3x3 neighbourhood costs 27 random accesses per vertex into a multi-megabyte table - 550 ns/vertex, 70 ms for a 132k-point sphere. With cells `2*eps` wide the eps ball spans one cell width, so `floor((p-eps)/w)` and `floor(peps/w)` differ by at most one: **eight** probes, same exact answer, 130 ns/vertex.

Consequences:
- A perf symptom that looks like cache misses may be an algorithmic `O(n^2)`. Count probes before you pad the struct.
- Table sizing is part of the fix: `3n + 8` slots is the worst case for an edge table (three unique edges per triangle) and cannot fill up, so the linear probe always terminates. `6n + 8` doubles the memset and the scan for nothing.
- Introspection does not need the subdivision payload: a count-only table (`(count, winding)` in 2 bytes, 8-byte key array) is 60 ms -> 12 ms on a 261k-triangle mesh. When two tables classify the same thing, the gate must compare them on a closed, an open, a non-manifold and an inconsistently wound mesh.

---

## 4. Existence-based processing

The central DOD idea: **don't repeatedly ask "does this object do something?"
— store "these objects do something."**

### 4.1 Don't use bools as a processing filter

```c
/* WRONG: inspect every entity every update, then branch */
for (size_t i = 0; i < entity_count; ++i) {
    bool hurt  = health[i] < MAX_HEALTH;
    bool dead  = health[i] <= 0.0f;
    bool ready = current_time - last_damage[i] > REGEN_DELAY;
    if (!dead && hurt && ready)
        health[i] = MU_MIN(MAX_HEALTH, health[i] + dt * regen_rate);
}

/* RIGHT: only hurt entities are in the table at all */
for (size_t i = 0; i < damaged_count; /* ... */) {
    EntityId id = damaged_ids[i];
    if (health[id] <= 0.0f) {
        dead_ids[dead_count++] = id;
        damaged_ids[i] = damaged_ids[--damaged_count];   /* swap-remove */
        continue;
    }
    if (current_time - last_damage[id] > REGEN_DELAY) {
        health[id] = MU_MIN(MAX_HEALTH, health[id] + dt * regen_rate);
        if (health[id] >= MAX_HEALTH) {
            damaged_ids[i] = damaged_ids[--damaged_count];
            continue;
        }
    }
    ++i;
}
```

### 4.2 Represent boolean streams as bitsets

For a 2-valued property over a large domain, one bit per element.

```c
/* 1 bit/instance instead of 1 byte, and 64 tests per instruction */
uint32_t casts_shadow[(MAX_INSTANCES + 31) / 32];
uint32_t visible_mask[(MAX_INSTANCES + 31) / 32];
```

Then set operations become whole-word operations:

```c
/* things that both cast shadows and are visible */
for (size_t w = 0; w < WORDS; ++w)
    xr[w] = casts_shadow[w] & visible_mask[w];
```

Use `mylib`'s `mu_bitset` when you need growth, set algebra and iteration
(§13). Use raw word arrays when the size is fixed and you want maximum control.

### 4.3 Represent sparse states by existence

```c
/* WRONG: a dense bool array over a huge range */
bool has_health[MAX_ENTITIES];

/* RIGHT: a dense list of just the entities that have health */
EntityId health_ids[MAX_ENTITIES];
size_t   health_count;
```

This is the "sparse component" pattern. `mu_sparse_set` gives O(1)
insert/remove/contains with dense packed iteration (§13).

### 4.4 Don't use enums as control flow

```
enum used as DATA  (Color, KeyBinding, CollisionResponse, table lookup)  → keep it.
enum used as FLOW  (forces switch/branch in every consumer)              → make it DATA/array.
```

Replace `enum + switch` with **table membership + specialized transform**:

```c
/* WRONG */
for (size_t i = 0; i < n; ++i)
    update_entity(&entities[i]);       /* switch inside */

/* RIGHT */
for (size_t i = 0; i < player_count; ++i)     update_player(&players[i]);
for (size_t i = 0; i < enemy_count; ++i)      update_enemy(&enemies[i]);
for (size_t i = 0; i < projectile_count; ++i) update_projectile(&projectiles[i]);
```

Rule of thumb: **moving an entity between tables has a cost, so rapidly
changing states may still be better as an enum.** Bucket by type once when it
is stable; keep the enum when it flips every tick.

### 4.5 Polymorphism lives in the data layout

```
object → type enum → switch → function          (still runtime dispatch)
object → vptr → vtable → virtual function       (still runtime dispatch)

DOD:  object → table membership → specialized transform
```

Type information becomes implicit in **table membership**; a polymorphic method
becomes a **specialized transform over a table**; a class constructor becomes a
**data-driven factory** that inserts rows into the relevant tables.

```c
EntityId create_enemy(/* ... */)
{
    EntityId id = new_entity();
    enemy_table_add(id);
    health_add(id, 100.0f);
    physics_add(id, pos);
    ai_add(id, AI_HOSTILE);
    return id;
}
```

An entity may belong to **many** tables at once — multidimensional composition,
similar to ECS. Because you reference entities by **ID**, they can move between
tables without changing logical identity. Event subscription uses the same
mechanism: subscribe = insert into an event table; unsubscribe = delete;
dispatch = process the subscribed dataset (and stop polling everything).

### 4.6 Entity = identity, Component = data, System = processing

```
Entity     = identity only
Component  = data
System     = processing
```

`systems` own the work. Rename thinking from `player_update()` to
`physics_manager_update()`, `animation_manager_update()`, `render_manager_update()`.

The best ECS model: **for each component, update its objects** — not for each
object, update its components.

```
Instance-oriented (weaker):
    Object 1 → {Transform, Physics}
    Object 2 → {Transform, Physics, Render}
    Object 3 → {Transform, Render}
    → processing order determined by instances

Type/system-oriented (stronger):
    Transform table [1,2,3]
    Physics   table [1,2]
    Render    table [2,3]
    → processing order determined by systems
```

Why it matters for profiling: with 500,000 entities but only 50,000 with
physics, we process 50,000 rows — and the profiler says
`PhysicsSystem = 2.4 ms` instead of mixing physics into one giant loop.

### 4.7 An entity is derived from its component data

```c
typedef uint32_t EntityId;

typedef struct { EntityId id; Vec3  position; } Position;
typedef struct { EntityId id; Vec3  velocity; } Velocity;
typedef struct { EntityId id; float health;   } Health;

/* Entity 42 is simply: 42 → Position + Velocity + Health */
```

Do not think of an entity as a class containing components. Think of it as the
**result** of the component data currently associated with its ID. No `Player`
struct is required.

### 4.8 Never represent state with more bits than it needs

- Do not use more bits/fields/precision/structure than the information requires.
- Pack small bounded values into bit fields when it improves bandwidth.
- Process at the granularity the representation supports:

```
bitset   → word/SIMD operations
packed   → vectorized processing
sparse   → affected elements only
events   → changed elements only
```

---

## 5. Branch discipline

### 5.1 A branch is only a problem when it is unpredictable

```
bad:      random branching (data-dependent, 50/50)
better:   predictable runs of true/false (sorted / bucketed / grouped data)
best:     no branch at all (predication, masking, table membership)
```

A mispredict is typically ~15–20 cycles of pipeline disruption. If the
condition is data-dependent (keys, hashes, random values), prefer sorted or
bucketed data, arithmetic selects, or masking.

### 5.2 Sorting data can make branches predictable

```c
for (size_t i = 0; i < n; ++i)
    if (a[i] > 128) sum += b[i];
```

`a[]` shuffled → `T F T F F T T F …` (hard). `a[]` sorted → `T T T T … F F F`
(easy). Same algorithm, different data order, different performance.
**Caveat:** the compiler may already have removed the branch (CMOV / SIMD
select). Inspect the assembly and measure before leaning on this.

### 5.3 Predication — compute both, select one

```c
/* branch */
if (a > b) lo = b, hi = a; else lo = a, hi = b;

/* branch-free */
hi = MU_MAX(a, b);
lo = MU_MIN(a, b);
```

`x = condition ? A : B` lets the compiler emit CMOV / SIMD select / min / max.
Sometimes doing a little extra arithmetic is faster than an unpredictable
branch. Sorting networks are the classic example: a **fixed** sequence of
compare-exchanges that does not depend on the input.

```
Sort 4:  (A,C) (B,D) → (A,B) (C,D) → (B,C)     ← fixed pattern, branch-free
```

### 5.4 Hint the cold path, never the hot one

```c
if (MU_UNLIKELY(!init_renderer()))     /* error handling, first-time init */
    return false;
```

Do not sprinkle `MU_LIKELY` on genuine 50/50 branches.

### 5.5 Sometimes computing everything is cheaper than deciding

```
if (dirty) update();          /* load flag, branch, possibly mispredict */
update();                     /* if update is cheap, this may win outright */
```

And existence-based replacement: `dirty == true` becomes `entity ∈ DirtyTable`,
and you update the table with no check at all. The recurring question is
**"is avoiding the work cheaper than doing the work?"** — sometimes it is not.

### 5.6 Group similar work into bursts

```
instead of:  A B C A B C A B C …
do:          A A A A A   B B B B B   C C C C C
```

Homogeneous bursts keep code and data hot and branches regular. Batch by type
(or by `vtable` pointer — a vtable pointer is data too, and a stream of them is
a predictable branch if batched).

### 5.7 Log decisions for debugging

When control flow is data-driven, keep a small log of *why* a decision was
taken (which condition matched, which transition fired). This turns "why is
this AI attacking?" into "here is the transition record."

### 5.8 The `CMOV` Trap on Out-of-Cache Data (Control vs Data Dependence)

*(Sources: Paul-Virak Khuong & Pat Morin, Linus Torvalds, Agner Fog; **empirically proven in `perf_test/bench_highperf_ds.c` Case 2: plain CMOV binary search = `1.00×` (`271.1 ns` vs `271.9 ns`), Eytzinger + explicit prefetch = `5.92×` (`45.9 ns`)!**)*

- On **in-cache L1/L2 streams**, replacing a 50/50 branch with branchless arithmetic or `CMOV` eliminates the ~15–20 cycle pipeline flush (**13.96× faster** scalar, **32.74× faster** with AVX2 `vpcmpgtd` in `bench_branch_ilp_alu.c` Case 2!).
- On **out-of-cache pointer/array hops** (like binary search on a 16 MB array), a conditional branch lets the CPU **speculatively predict and issue the next memory load** before the current load finishes. Converting that branch to `CMOV` turns a *control dependency* into a **strict data dependency** on the loaded value, **halting speculative memory prefetching**!
- **Rule:** Use `CMOV` / bitmasks when both inputs are already in registers or L1 cache, OR when paired with **explicit software prefetching (`MU_PREFETCH`)** (such as Eytzinger `2k`/`2k+1` prefetching `4` levels ahead). Keep a regular branch when the condition is **>98% predictable**!

### 5.9 Break Serial Dependency Chains (`Latency × Throughput` Accumulators & Estrin's Tree)

*(Sources: Agner Fog 'Optimizing Subroutines in Assembly', Casey Muratori Performance-Aware Programming Part 5; **2.43× scalar / 20.20× AVX2 speedup** in `bench_branch_ilp_alu.c` Case 3, **1.74× polynomial speedup** in Case 4)*

On modern Intel (Ice Lake / Alder Lake / Raptor Lake) and AMD Zen 4/5 desktops:
- `vfmadd231ps` has **Latency = 4 cycles**, **Reciprocal Throughput = 0.5 cycles** (2 FMA pipes/cycle).
- By Little's Law, saturating both FMA units requires **`4 ÷ 0.5 = 8` independent AVX registers in flight simultaneously**! A single `sum += a[i] * b[i]` loop achieves **only 1/8th (12.5%) of peak FPU compute** (`0.501 ns/op` vs `0.025 ns/op` = **20.20×**!).

```c
/* Unroll into 4–8 independent accumulators, then tree-reduce at the end: */
__m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps(),
       s2 = _mm256_setzero_ps(), s3 = _mm256_setzero_ps();
for (size_t i = 0; i + 32 <= n; i += 32) {
    s0 = _mm256_fmadd_ps(_mm256_load_ps(a + i +  0), _mm256_load_ps(b + i +  0), s0);
    s1 = _mm256_fmadd_ps(_mm256_load_ps(a + i +  8), _mm256_load_ps(b + i +  8), s1);
    s2 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 16), _mm256_load_ps(b + i + 16), s2);
    s3 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 24), _mm256_load_ps(b + i + 24), s3);
}
s0 = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
```

Similarly, for polynomial approximations (`sin`, `exp`, `atan2`, tonemapping curves), **Horner's rule** (`((c3*x + c2)*x + c1)*x + c0`) chains `N` serial FMAs (`4N` cycles latency), whereas **Estrin's binary tree** (`(c0 + c1*x) + x²*(c2 + c3*x)`) reduces FMA critical-path depth to `O(log₂ N)` (**1.74× faster** for degree-7 in `bench_branch_ilp_alu.c` Case 4).

### 5.10 Eliminate Integer & Float Divisions (`fastmod`, `fastrange32`, Montgomery Batching)

*(Sources: Daniel Lemire `fastmod`/`fastrange`, Mesa `src/util/fast_urem_by_const.h`, Agner Fog; **2.98× (`fastmod`) & 11.48× (`fastrange32`) faster** in `bench_branch_ilp_alu.c` Case 5)*

Hardware `div` / `idiv` is NOT pipelined like `imul`: even on Ice Lake/Zen 4, 32/64-bit integer division costs **10–18 cycles** with poor throughput (1 per 3–6 cycles), and there is **no integer vector `vdiv` instruction in SSE/AVX2/AVX-512**!

1. **Power-of-2 capacity:** Always prefer `x & (cap - 1)`.
2. **Arbitrary non-power-of-2 bucket mapping (`[0, d)`):** Use **Daniel Lemire's `fastrange32`** — maps a uniform 32-bit hash into `[0, d)` in **1 multiply + 1 shift (`0.28 cycles`, 11.48× faster than `%`)**:
   ```c
   static inline uint32_t fastrange32(uint32_t word, uint32_t p) {
       return (uint32_t)(((uint64_t)word * (uint64_t)p) >> 32);
   }
   ```
3. **Exact runtime-invariant modulo (`x % d`):** Precompute `uint64_t M = UINT64_C(0xFFFFFFFFFFFFFFFF) / d + 1;` once per table resize, then use **Lemire `fastmod_u32`** (used in Mesa's NIR/driver compiler via `fast_urem_by_const.h`, **2.98× faster**):
   ```c
   static inline uint32_t fastmod_u32(uint32_t x, uint64_t M, uint32_t d) {
       uint64_t lowbits = M * x;
       return (uint32_t)(((__uint128_t)lowbits * d) >> 64);
   }
   ```
4. **Agner Fog / Montgomery Batched Reciprocal Trick:** Need `1/a` and `1/b`? Compute `inv_ab = 1.0f / (a * b); inv_a = inv_ab * b; inv_b = inv_ab * a;` — generalizes to **`N` reciprocals using 1 division + `3(N-1)` multiplies**!

### 5.11 Branchless Bitstream Refill (Fabian Giesen / RAD Oodle Pattern)

*(Source: Fabian Giesen 'Reading bits in far too many ways'; **4.78× faster** in `perf_test/bench_branch_ilp_alu.c` Case 7)*

Never refill Huffman / mesh / animation bitstreams with `while (bitcnt < width) { bits |= (*p++) << bitcnt; bitcnt += 8; }` — that inner `while` mispredicts on every symbol! Unconditionally load an unaligned 64-bit word and advance the byte pointer by `(63 - bitpos) >> 3`:

```c
/* Invariant: at least 56 valid bits available before every symbol decode — ZERO branches! */
size_t advance = (63u - bitpos) >> 3;
byte_ptr += advance;
bitpos   |= (advance << 3);   /* same mod-8 bit offset after advancing whole bytes */
uint64_t window;
memcpy(&window, byte_ptr - 7, 8); /* compiles to a single unaligned `mov r64, [mem]` on x86-64 */
```

### 5.12 Cold-Path Outlining & Instruction-Cache Discipline

*(Sources: Linux Kernel `likely()`/`unlikely()` & `STATIC_BRANCH`, LLVM)*

Error handling, logging, `realloc` slow paths, and assertion dumps inside an inlined function bloat I-cache (`32 KB L1i`) and evict hot µops from the **Decoded Stream Buffer (DSB / µop-cache)**. Mark invariant error branches with `MU_UNLIKELY(cond)` and extract the cold body into a `MU_NOINLINE __attribute__((cold))` helper so GCC/Clang places it in `.text.unlikely` completely off the hot cache lines.

### 5.13 Bytecode / Interpreter Dispatch & Associative SIMD `pshufb` State Machines

*(Sources: LuaJIT, CPython 3.13+, Josh Haberman `upb`, Mytkowicz et al. ASPLOS, Geoff Langdale `Hyperscan`; **1.14× VM dispatch & 8.84× SIMD DFA (`3.64 GB/s` vs `412 MB/s`)** in `perf_test/bench_advanced_c_isa.c` Cases 4 & 7)*

1. **Direct-Threaded Computed `goto *` / `musttail` Dispatch:**
   - A `while (1) switch (*ip++)` loop compiles to **one single indirect `jmp`** shared by all opcodes, colliding in one Branch Target Buffer (BTB) slot.
   - Placing `goto *dispatch[*ip++];` at the tail of every opcode handler gives each opcode its own indirect `jmp` address so the BTB predicts common opcode pairs (`LOAD_IMM -> MUL -> ADD`) independently! **Crucial GCC trap:** Always pass `-fno-gcse -fno-crossjumping` (or `__attribute__((optimize("no-gcse,no-crossjumping")))`) on the interpreter function, otherwise GCC "optimizes" all `goto *` tails by merging them back into one shared jump! On Clang 13+ / GCC 15+, `__attribute__((musttail)) return handlers[*ip++](ip, sp, acc);` achieves tail-call register-pinned threading in pure standard C.
2. **Parallel Finite-State Machines (`<= 16` States) via `pshufb` Composition:**
   - A table-driven DFA (`state = dfa[state][byte]`) suffers a **4–5 cycle serial L1 load-to-address chain per byte** (`~412 MB/s`).
   - Any `<= 16`-state transition on input `c` is a mapping `T_c : {0..15} -> {0..15}` stored in **one 16-byte `__m128i`**, and **function composition `(T_b ∘ T_a)[s] = T_b[T_a[s]]` is literally `_mm_shuffle_epi8(T_b, T_a)` (`pshufb`)**! Because function composition is **associative**, split the input into 4 streams or tree-reduce pairs of transitions in parallel (`8.84×` faster, `3.64 GB/s`)!

---

## 6. Memory, cache and the machine

### 6.1 The memory hierarchy is the real medium

```
L1   ~4 cycles     ~32–48 KB
L2   ~12 cycles    ~256 KB–1 MB
L3   ~40 cycles    MBs
DRAM ~80–300 cycles
```

Every memory request brings in a whole **cache line (commonly 64 bytes)** —
that is 16 ints, 8 doubles, 16 floats, or 512 bits. **Use the rest of the
line.** Load useful metadata together with data you already had to fetch.
Store per-entity presence masks next to the entity so one line answers many
"does it have X?" questions.

Cache levels of caching, cheapest to best:

```
no cache → cache presence → cache partial query → cache complete result
```

### 6.2 Pointer chasing vs arrays

```c
/* BAD: next address depends on current data; no prefetch possible */
for (Node *p = head; p; p = p->next)
    process(p);

/* GOOD: addresses are predictable — address(i) = base + i * stride */
for (size_t i = 0; i < count; ++i)
    process(&nodes[i]);
```

The problem is not "pointers are slow." The problem is
**the next address is unknown until the previous load completes**:

```
previous result → next address → next load    (a serial chain, no MLP)
```

For graphs, use CSR (offsets + edge array). For trees, prefer **wide nodes**
(B-trees / B*-trees) — many keys per node, fewer hops, shallower tree, more
useful data per cache line. Reduce the number of dependent hops everywhere:

```
entity → component manager → component pointer → component → target   (BAD)
entity_id → direct table/index → data                                  (GOOD)
```

### 6.3 Memory-level parallelism (MLP)

Independent loads can be in flight simultaneously. Break dependency chains and
let several misses overlap (software prefetch, unrolled independent streams,
`MU_PREFETCH`). One accumulation must be serial; **reductions should use
multiple accumulators**.

### 6.4 Aliasing, `restrict`, and why the compiler goes conservative

```c
/* The compiler must assume p and q may be the same memory. */
void foo(int *p, const int *q, int n)
{
    for (int i = 0; i < n; ++i)
        p[i] = *q;            /* *q reloaded every iteration! */
}
```

`const` prevents modification *through that reference*; it does **not** prove
non-aliasing. Fix it with `restrict` and explicit ownership:

```c
void foo(int * MU_RESTRICT p, const int * MU_RESTRICT q, int n);
```

```
possible aliasing  → unknown side effects → fewer optimizations (or runtime checks)
known non-aliasing → register reuse, reordering, unrolling, clean vectorization
```

- **The Runtime Alias-Check In-Place (`a == c`) Trap:** On `a[i] = b[i] * s + c[i]` without `restrict`, Clang/GCC often vectorize anyway behind a **runtime pointer-overlap check**. Distinct arrays run at full SIMD speed, **but calling it in-place (`a == c`, i.e., `saxpy`) fails the overlap check and silently drops to the scalar fallback (`5.29×` slower in L1!)**, even though iteration `i` has zero cross-iteration dependency! Provide an explicit in-place API (`MU_RESTRICT` between `y` and `x`) or `#pragma clang loop vectorize(assume_safety)` / `#pragma GCC ivdep`.

### 6.5 Return by value — small structs are cheap

Modern compilers construct the return value directly in the caller's storage
(RVO). Prefer `Vec3 add(Vec3 a, Vec3 b)` over `void add(const Vec3*, const Vec3*, Vec3 *out)`.
Value-oriented code makes dependencies explicit and avoids aliasing and
out-parameter bugs. C caveat: returning a *large* struct by value on a
small/embedded ABI may still copy — check the assembly for big structs. (And in C++, **any** user-defined destructor or copy constructor makes even an 8-byte handle non-trivial under the Itanium C++ ABI, forcing stack passing by hidden pointer — keep handles trivial POD!)

### 6.6 False sharing

```
T0 → counter[0] ─┐
                  ├─ same cache line → cache-coherence traffic
T1 → counter[1] ─┘
```

Pad rapidly-modified per-thread data to cache lines:

```c
typedef struct { int64_t count; char _pad[56]; } MUCacheLineAligned;  /* 64B */
```

and aggregate locally, then do one shared update.

### 6.7 Non-temporal / streaming stores

Data written once and not read again soon does not deserve cache capacity.
Streaming stores bypass the cache to avoid **cache pollution** (evicting your
useful animation/AI/player data). Prefer normal stores for anything reused
soon. First write the transform in a simple, predictable form so the compiler
can spot the streaming opportunity; reach for intrinsics only when it cannot.

### 6.8 Prefetching and paging

- Software prefetch (`MU_PREFETCH`) works **only** on irregular/indirect hops
  (`nodes[idx[i + dist]]` or Eytzinger `a[k * 16]`) where hardware stream
  prefetchers are blind. **Never** sprinkle `__builtin_prefetch` on linear
  unit-stride arrays: it wastes load/AGU issue slots, evicts hot L1 lines when
  early, and can **mistrain hardware prefetchers** (Lee et al. TACO'12)!
- Random access over data larger than RAM thrashes both DTLB and Page-Walk
  Caches (`PWC`). Keep hot working sets page-friendly; block/tile algorithms
  (and copy conflict-prone tiles into scratch buffers — Lam et al. ASPLOS'91).

### 6.9 Keep hot code and data small

Hot code evicts hot code; hot data evicts other hot data. Keep the hot 5%
small by separating hot/cold data (§3.3) and batching work (§5.6).

### 6.10 Be aware of speculation

`if (has_component) load_component_data;` may speculatively load data that
turns out to be unnecessary — wasted bandwidth on top of an unpredictable
branch. Predicate caching ("does this query return anything?") is cheap and
prevents the wasted load.

### 6.11 Store-to-Load Forwarding (STLF) Stalls & 4K Aliasing

*(Sources: Intel 64 & IA-32 Architectures Optimization Reference Manual, Agner Fog, Fabian Giesen; **4.29× faster (`1.28 ns` vs `5.48 ns`, saves ~9 cycles/op)** in `perf_test/bench_memory_cache.c` Case 6)*

Recent stores wait in the CPU **Store Buffer** (~56–72 entries) before committing to L1d. If a subsequent load reads from the same address, the CPU forwards bytes directly from the Store Buffer in ~4–5 cycles — **ONLY if the load's address and width are wholly contained within a single prior store**!
- **STLF Stall (~11–15 cycle penalty):** Writing two `uint32_t`s (or four `uint16_t`s / `memcpy` byte pieces) to `[p]` and `[p+4]`, then immediately reading `*(uint64_t *)p` (or writing scalar floats into a stack array and immediately loading `__m256`) **cannot forward** — the load stalls until both stores drain to L1d!
- **Fix:** Pack narrow fields in integer/SIMD registers first (`((uint64_t)hi << 32) | lo` or `_mm256_setr_ps`) and store/load at **matched width and alignment**.
- **4K Aliasing Stall (~5–7 cycle false stall):** On Intel cores, initial store-buffer hazard checks compare only the low 12 bits (`addr & 0xFFF`). Reading `src[i]` while writing `dst[i]` when `(uintptr_t)dst - (uintptr_t)src` is an exact multiple of `4096` triggers false store-forwarding stalls on every iteration! Offset scratch buffers by `+64` or `+128` bytes when streaming `src -> dst` across page-aligned arrays.

### 6.12 Cache Set-Associativity & Power-of-2 Row-Stride Thrashing

*(Sources: Algorithmica, Agner Fog, Intel Opt Manual)*

A `48 KB` 12-way L1d cache with `64 B` lines has `48 KB / (12 × 64 B) = 64 sets` — meaning **bits `[11:6]` (`stride % 4096`) alone choose the L1d cache set**! Iterating vertically down a `1024 × 1024` `float` matrix (`row_stride = 4096 B`) maps *every single row* to the **exact same L1d set**, turning a `48 KB` cache into a `768-byte` (`12-line`) cache!
- **Fix:** Pad 2D row pitches by `+16 floats` (`+64 bytes`, one odd cache line: `pitch = width + 16`) or store 2D grids in **Morton Z-order / 8×8 tiles** (§3.12).

### 6.13 Hardware Prefetchers, 4 KB Page Boundaries, DTLB & 2 MB Hugepages

*(Sources: Intel Opt Manual, Linux Kernel `MADV_HUGEPAGE`, Valve Migdalskiy)*

1. Desktop x86-64 cores have four hardware prefetchers per core (L1d next-line, L1d IP-stride, L2 spatial pair-line, L2 streamer). They track up to **8–16 simultaneous monotonic streams** — **do not access >8–12 independent arrays in one loop**, or streams thrash the prefetcher table (loop-fission instead!).
2. **Hardware L1/L2 prefetchers STOP at 4 KB virtual page boundaries** because prefetcher hardware operates on physical addresses before consulting the page walker. On multi-hundred-MB ECS/BVH/graph arenas, call `madvise(ptr, bytes, MADV_HUGEPAGE)` on Linux (`MEM_LARGE_PAGES` on Windows) so **2 MB Transparent Huge Pages** expand DTLB reach by **512×** and let hardware prefetchers run uninterrupted across `32,768` cache lines per page!

### 6.14 MSHRs & Exact Software Prefetch Distance (`MU_PREFETCH`)

*(Sources: Sergiy Migdalskiy Valve GDC 2015, Khuong & Morin, Ulrich Drepper)*

Each desktop core has **10–16 L1 Fill Buffers / MSHRs** (max concurrent L1 misses in flight). When indirection `data[indices[i]]` hides future addresses from the hardware prefetcher, software prefetch overlaps DRAM latency (`~80 ns = ~240 cycles`) with loop work (`W` cycles/iter) at lookahead distance:
```
LOOKAHEAD = ceil(DRAM_CYCLES / LOOP_BODY_CYCLES)   /* typically 8..24 iterations ahead */
```
```c
for (size_t i = 0; i < count; ++i) {
    if (i + 16 < count) MU_PREFETCH(&nodes[indices[i + 16]]);
    process(&nodes[indices[i]]);
}
```
Never prefetch `indices[i + 1]` (arrives too late) or `indices[i + 256]` (evicts L1 before `i` reaches it).

### 6.15 Write-Allocate (RFO) Elimination & Non-Temporal Stores (`_mm256_stream_ps`)

*(Sources: Intel Opt Manual, Frostbite GPU Staging, `perf_test/bench_memory_cache.c` Case 8 — **1.63× faster subsequent hot-L2 query** by preventing L2 eviction!)*

A normal store to a cold destination line triggers **Read-For-Ownership (RFO)**: the CPU must first **read 64 bytes from DRAM** just so you can overwrite all 64 bytes — wasting **50% of memory bus bandwidth** AND **evicting your hot L2 working set**!
- When writing whole 64B cache lines to a large write-only buffer (GPU vertex/instance staging ring, baked audio/mesh output, frame snapshot) that will NOT be read back by the CPU soon:
  1. Ensure `dst` is `32B`/`64B`-aligned.
  2. Emit full cache lines sequentially with `_mm256_stream_ps` / `_mm256_stream_si256` (writes coalesce in Write-Combining buffers directly to memory, bypassing L1/L2/L3!).
  3. Issue `_mm_sfence()` once after the loop before making the buffer visible to GPU/other threads.

### 6.16 False Register Dependencies, `vzeroupper` & µop-Cache Loop Alignment

*(Sources: Agner Fog Microarchitecture Manual, LLVM X86TargetTransformInfo)*

1. **False Output Dependency Trap:** On Intel Skylake/Ice Lake (and older Zen), `popcnt`, `lzcnt`, `tzcnt`, `sqrtsd`, and `cvtsi2ss` have a **false architectural dependency on their destination register**! In tight unrolled loops, GCC/Clang may serialize iterations if they reuse the same output register unless broken (`xor eax, eax` or independent output variables across unrolled lanes).
2. **`vzeroupper` AVX↔SSE Transition Penalty (`other_assists.avx_to_sse`):** Writing `ymm0–15` or `zmm0–15` marks upper bits "dirty"; the **first non-VEX SSE instruction** afterward pays a multi-hundred-cycle state save/restore (blamed on that SSE instruction in `perf annotate`!). Verify `avx_to_sse == 0` via `perf stat -e other_assists.avx_to_sse,other_assists.sse_to_avx ./bin`. Call `_mm256_zeroupper()` (~1 cycle) before any return/callback that may reach non-VEX SSE code; in inline `asm`, `vzeroupper` clobbers **all** `ymm0–ymm15` (or `"zmm0".."zmm15"` — tracked separately by GCC/Clang!). Note: `zmm16–zmm31` (EVEX-only) have no YMM alias and need no `vzeroupper`; `__attribute__((target(...)))` / `target_clones` emit it automatically unless bypassed by inline `asm`.
3. **DSB (µop-Cache) Hot Loop Fit:** Keep innermost hot loops **under ~200–300 bytes of machine code** (`4–6 × 64B` windows) so the entire loop locks into the core's Loop Stream Detector (LSD) / Decoded Stream Buffer (DSB, up to 6–8 µops/cycle, bypassing legacy x86 decode!). Over-unrolling 16× can spill out of the LSD and make a loop slower than 4× unrolling.

### 6.17 Loop Interchange (`i-k-j`) & L1 + Register Micro-Kernel Tiling (GotoBLAS / BLIS Rule)

*(Sources: Goto & van de Geijn 'Anatomy of High-Performance Matrix Multiplication', BLIS, Algorithmica; **20.07× faster from swapping 2 lines (`i-k-j`) & 44.13× faster (`72.1 GFLOP/s` on 1 core!)** with L1+register tiling in `perf_test/bench_advanced_c_isa.c` Case 1)*

1. **Loop Order (`i-k-j` over `i-j-k`):** Never step the innermost loop variable along a non-unit row stride (`B[k * N + j]` with inner `k`). Swapping the two inner `for` headers (`for k` outside `for j`) broadcasts `A[i*N + k]` into a register and streams both `B[k*N + j]` and `C[i*N + j]` contiguously at unit stride (**20.07× speedup from swapping 2 lines**!).
2. **Register Micro-Kernel (`MR × NR`):** To eliminate repeated L1 load/store traffic on `C`, hold an **`MR × NR` (`4 × 16` = 8 `__m256` registers) block of outputs in YMM registers** across a `KC = 64` L1-resident strip of `B` (`64 × 16 × 4B = 4 KB`). That performs `4 × 16 × 2 = 128 FLOPs` for only `2` vector loads + `4` broadcasts (**16 FLOPs per vector load** -> **44.13× faster**)!

---

## 7. SIMD and auto-vectorization

### 7.1 Let the compiler vectorize first

```c
/* vectorizable: independent, contiguous, no early exit */
for (size_t i = 0; i < n; ++i)
    a[i] *= mult;
```

Check with `gcc -O3 -fopt-info-vec` / clang `-Rpass=loop-vectorize`. Only write
intrinsics for structural transforms the compiler cannot infer (lane masking,
pair folding, shuffles).

### 7.2 What breaks vectorization

```
- missing target ISA flags (-O3 without -march=x86-64-v3 / -march=native emits SSE2!)
- floating-point reductions without reassociation: float sum += a[i]*b[i] stays ONE
  serial scalar vaddss chain even when packed vmulps is emitted, unless unlocked via
  explicit 4-8x vector accumulators, #pragma omp simd reduction(+:sum), or
  -fassociative-math -fno-signed-zeros -fno-trapping-math!
- data-dependent loop exit:  if (a[i] < 0) break;      ← loop length depends on data
- unknown function calls in the loop (side effects)
- possible aliasing between input and output pointers (or failed runtime alias check)
- cross-iteration dependencies:  a[i] += a[i+1];
- unaligned pointers
- partial writes to the output stream
```

Note the asymmetry: a branch that lets every iteration **complete** can still
vectorize (the compiler emits a select):

```c
for (size_t i = 0; i < n; ++i) {
    float v = a[i] * mult;
    a[i] = (v > 0.0f) ? v : 0.0f;      /* select, not control flow */
}
```

### 7.3 Make scalar code vectorizer-friendly before writing intrinsics

Simple loop, no early exits, contiguous access, inlined callees, `restrict`,
regular counts. Write the whole output stream rather than partially updating it.

### 7.4 SIMD is not automatically faster

Vectorization wins when there is enough data, independent operations, a
suitable layout, and vector-friendly instructions. On tiny workloads, SIMD
setup and remainder handling can lose. Measure.

### 7.5 SoA enables SIMD because it makes streams explicit

```
positions[] + velocities[] → MovementSystem → positions[]
```

One vector instruction operating on `[x0 x1 x2 x3]` instead of four scalar ops.

### 7.6 Frostbite SoA + Prebound P-Vertex SIMD Frustum / Hi-Z Culling

*(Source: EA Frostbite 'Culling the Battlefield: Data Oriented Design in Practice'; **16.13× faster (`0.73 ns/box` = 1.37 Billion AABBs/sec vs `11.80 ns/box`)** in `perf_test/bench_simd_avx.c` Case 1)*

Scalar OOP culling stores `struct Entity { AABB box; ... }` and branches per plane (`if (outside) return false;`). Early-out branches **mispredict** on view boundaries and touch 1 entity at a time.
**Frostbite Pattern:**
1. Store `min_x[], min_y[], min_z[], max_x[], max_y[], max_z[]` in 32B-aligned SoA arrays.
2. **Hoist plane sign tests OUTSIDE the entity loop:** for each of the 6 frustum planes `(nx, ny, nz, d)`, prebind pointers `px = (nx >= 0.0f) ? max_x : min_x` before looping over entities — **zero sign selection inside the loop**!
3. Test 8 AABBs simultaneously across all 6 planes with `_mm256_fmadd_ps`, OR the outside sign-bits together, and emit a **compact 8-bit visibility bitmask** (`~(uint32_t)_mm256_movemask_ps(outside) & 0xFFu`) directly into `visible_bits[]`!

### 7.7 Fabian Giesen / ISPC In-Register 8×8 AoS↔SoA Float Transpose

*(Sources: Fabian Giesen 'SIMD Transposes', Intel ISPC; **2.94× faster** in `perf_test/bench_simd_avx.c` Case 7)*

Never use `_mm256_i32gather_ps` when reading contiguous AoS arrays (8 structs of 8 floats)! Load 8 contiguous `__m256` vectors at full L1 cache-line bandwidth and transpose `8×8` in registers in `3 × 8 = 24` shuffle instructions (`_mm256_unpacklo_ps`/`unpackhi_ps` -> `_mm256_shuffle_ps` -> `_mm256_permute2f128_ps`), process in pure SoA SIMD, and transpose back before storing. Stage in `32–64 KB` L1/L2-resident tiles so transposed buffers never spill to DRAM.

### 7.8 Daniel Lemire `vpshufb` 2-Nibble Register LUT Classification

*(Sources: Geoff Langdale & Daniel Lemire `simdjson`, Wojciech Muła; **15.59× faster (`26.5 GB/s` vs `1.70 GB/s`)** in `perf_test/bench_simd_avx.c` Case 2)*

Testing whether each byte in a stream belongs to an arbitrary set of characters (`' '`, `'\t'`, `'\n'`, `'\r'`, `'{'`, `'}'`, `'['`, `']'`, `':'`, `','`) via scalar branches or 10 separate `_mm256_cmpeq_epi8` masks wastes execution ports.
**`vpshufb` (`_mm256_shuffle_epi8`) IS a 32-way parallel 16-entry Lookup Table in 1 cycle!** Split each byte into `lo_nibble = v & 0x0F` and `hi_nibble = (v >> 4) & 0x0F`, assign 1 bit per character class (`<= 8` classes), and intersect `LUT_LO[lo] & LUT_HI[hi]`:

```c
__m256i lo = _mm256_and_si256(v, _mm256_set1_epi8(0x0F));
__m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), _mm256_set1_epi8(0x0F));
__m256i hit = _mm256_and_si256(_mm256_shuffle_epi8(lut_lo, lo),
                               _mm256_shuffle_epi8(lut_hi, hi));
uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpgt_epi8(hit, _mm256_setzero_si256()));
```

**Bonus Range Trick:** Unsigned byte range `[A, B]` in **2 instructions** using unsigned saturating subtraction: `_mm256_subs_epu8(_mm256_sub_epi8(v, set1(A)), set1(B - A)) == 0`!

### 7.9 `simdjson` `PCLMULQDQ` Carryless-Multiply Prefix-XOR (Eliminating Stateful Loops)

*(Source: Langdale & Lemire VLDB 2019; **24.19× faster (`25.6 GB/s` vs `1.06 GB/s`)** in `perf_test/bench_simd_avx.c` Case 3)*

A stateful loop like `if (c == '"') in_quote ^= 1;` looks impossible to vectorize because every byte depends on the parity of all preceding quotes. **Carryless multiplication (`_mm_clmulepi64_si128(quote_bits, all_ones, 0x00)`) computes the prefix-XOR of all 64 bits in a single 1-uop hardware instruction!** XOR that mask with `prev_in_string` (`(int64_t)in_quote >> 63`) and mask out every quoted delimiter at 25+ GB/s!

### 7.10 SIMD Stream Compaction / Left-Packing (`simdprune` `vpermd` & AVX-512 `vpcompressd`)

*(Sources: Daniel Lemire `simdprune`, Intel AVX-512; **21.69× faster (`5.21 Billion ints/s` vs `240 M/s`)** in `perf_test/bench_simd_avx.c` Case 4)*

Filtering active IDs, surviving particles, or visible draw commands (`if (keep) out[n++] = x;`) causes catastrophic branch mispredictions when survival is 20%–80%.
- **AVX2 (`vpermd` 256-entry LUT):** Compute 8-bit predicate mask `m = _mm256_movemask_ps(cmp)`, load precomputed 8-lane permutation indices `perm_lut[m]` (`256 × 32B = 8 KB`, stays hot in L1d!), permute matching lanes to the front via `_mm256_permutevar8x32_epi32(v, perm)`, store unaligned to `out + pos`, and advance `pos += _mm_popcnt_u32(m)`!
- **AVX-512 (`vpcompressd`):** Single hardware instruction `_mm512_mask_compressstoreu_epi32(out + pos, k, v)` + `pos += _mm_popcnt_u32(k)` — **zero lookup table needed** (`20.64×` faster than scalar!).

### 7.11 SWAR & AVX2 `vpmaddubsw` + `vpmaddwd` Digit Parsing

*(Sources: Daniel Lemire & Wojciech Muła; **5.03× faster (`3.12 Billion 8-digit ints/sec`)** in `perf_test/bench_simd_avx.c` Case 5)*

Replace `v = v * 10 + (s[i] - '0')` loops with horizontal pairwise multiply-accumulate:
- **64-bit SWAR (no SIMD needed):** Subtract `'0'` (`0x3030303030303030ULL`), then fold pairs/quads/octets via 3 integer multiplications (`* 2561 >> 8`, `* 6553601 >> 16`, `* 42949672960001 >> 32`).
- **AVX2 (4 × 8-digit integers per iteration):** Subtract `'0'`, fold adjacent byte pairs with `_mm256_maddubs_epi16(v, [10,1,...])`, fold 16-bit pairs to 0..9999 with `_mm256_madd_epi16(t1, [100,1,...])`, pack with `_mm256_packus_epi32`, and fold to 0..99999999 with `_mm256_madd_epi16(t3, [10000,1,...])`.

### 7.12 SIMD Bitset Popcount (`vpshufb+vpsadbw` & AVX-512 `vpopcntq`)

*(Sources: Muła, Kurz & Lemire 'Faster Population Counts Using AVX2 Instructions'; **11.51× (`45.9 GB/s` AVX2) & 23.72× (`94.7 GB/s` AVX-512)** in `perf_test/bench_simd_avx.c` Case 6)*

- For `< 512 bytes` (`< 64 words`): 4× unrolled scalar `__builtin_popcountll` (`30.2 GB/s`).
- For `>= 512 bytes` on AVX2: Muła-Kurz-Lemire nibble `vpshufb` popcount accumulated across 8–16 vectors before flushing via `_mm256_sad_epu8` (`45.9 GB/s`).
- On Ice Lake / Zen 4+ (`__AVX512VPOPCNTDQ__`): `_mm512_popcnt_epi64` processes 64 bytes/instruction at **L1 cache bandwidth limit (`94.7 GB/s`)**!

### 7.13 Hardware Reciprocal Sqrt (`_mm256_rsqrt_ps` + 1 FMA NR) vs `sqrtf` & Quake `0x5f3759df`

*(Sources: id Software Quake III historical comparison, Intel Opt Manual, Agner Fog; **6.70× faster (`0.23 ns/float` = 4.35 Billion normals/s)** in `perf_test/bench_branch_ilp_alu.c` Case 6)*

**Never copy-paste Quake III's `0x5f3759df` integer bit-hack on modern x86-64!** Moving a `float` from an XMM register to a GPR (`vmovd`), doing integer shift/sub, and moving back to XMM costs **6–8 cycles of domain-crossing latency** (`0.97 ns`), whereas hardware `_mm_rsqrt_ss` / `_mm256_rsqrt_ps` computes a 12-bit approximation in **4 cycles on 8 floats simultaneously**, and **one FMA Newton-Raphson refinement** restores ~23 bits of IEEE-754 mantissa accuracy at **6.70× the speed of `1.0f / sqrtf(x)`**:

```c
__m256 y = _mm256_rsqrt_ps(x); /* 12-bit hardware estimate, 8 lanes */
/* 1 Newton-Raphson step: y = y * (1.5 - 0.5 * x * y * y) -> ~23-bit mantissa precision */
__m256 half_x = _mm256_mul_ps(_mm256_set1_ps(-0.5f), x);
y = _mm256_mul_ps(y, _mm256_fmadd_ps(half_x, _mm256_mul_ps(y, y), _mm256_set1_ps(1.5f)));
```

### 7.14 The AVX2 128-Bit Lane Trap & Clean SIMD Tail Handling

1. **AVX2 128-Bit Lane Trap:** Most AVX2 byte/word shuffles and packs (`_mm256_shuffle_epi8`, `_mm256_packus_epi32`, `_mm256_unpacklo_epi32`, `_mm256_alignr_epi8`) operate on **two independent 128-bit lanes `[127:0]` and `[255:128]`** — bytes in the low 16B CANNOT cross into the high 16B via `vpshufb`! To cross the 128-bit lane boundary, use `_mm256_permutevar8x32_epi32` (`vpermd`), `_mm256_permute4x64_epi64` (`vpermq`), or `_mm256_permute2x128_si256`.
2. **Eliminate Scalar Cleanup Loops on Tails:**
   - **Best (DOD Arena Rule):** Pad all SoA/arena allocations to a **64-byte boundary** (`MU_ALIGN_UP(count, 16)`) with harmless identity sentinel values (`0.0f` velocity, `+FLT_MAX` AABB) so every SIMD loop runs `0..aligned_count` with **zero tail branch**!
   - **Overlapping Final Unaligned Vector:** For idempotent transforms/scans with `n >= 8`, process `i = 0, 8, ...` and handle the remainder with a single unaligned load/store at `ptr + (n - 8)`.
   - **Masked Load/Store:** Use `_mm256_maskload_ps` / `_mm256_maskstore_ps` (or AVX-512 `__mmask16 k = (1u << rem) - 1u`).

### 7.15 Desktop Special-Purpose ISA Accelerators: `AES-NI`/`CRC32C` Hashing, `GFNI`, `StreamVByte` & Bit-Serial SIMD

*(Empirically proven in `perf_test/bench_advanced_c_isa.c` Cases 2, 3 & 6)*

Modern desktop x86-64 CPUs include dedicated single-cycle silicon units far beyond float math — **use them for non-cryptographic systems work**:
1. **Hardware `AES-NI` (`_mm_aesenc_si128`), `CRC32C` (`_mm_crc32_u64`) & Corsix Fused `VPCLMULQDQ+CRC32C` (`76.59×`, `32.28×`, up to `97 GB/s`):**
   - Never hash buffers or asset blobs byte-by-byte with `FNV-1a` (`~700 MB/s`) or a single serial `crc32q` chain (`~3–8 GB/s`).
   - Three interleaved `_mm_crc32_u64` chains (+ `PCLMULQDQ` folding) hash **23.1 GB/s** (`32.28×`); `wyhash` hits **15.4 GB/s** (`20.75×`); **4 interleaved `_mm_aesenc_si128` rounds** (`mu_fast_codec.h`) hit **54.8 GB/s (`76.59×`)**; and for true `CRC32C` checksums, **Corsix `fast-crc32` (`v3s1_s3` / `v4s5x3` / `v3s2x4`)** fuses 512-bit `VPCLMULQDQ` folding on vector ports with concurrent scalar `crc32q` on integer ports to reach **64–97 GB/s** (`-mavx512f -mavx512vl -mvpclmulqdq -mpclmul -msse4.2`)!
2. **Hardware `GFNI` (`vgf2p8affineqb` — Ice Lake+ & Zen 4+, `93.20×` faster at `87.8 GB/s`):**
   - `_mm256_gf2p8affine_epi64_epi8(v, matrix64, imm8)` multiplies **every byte simultaneously by an arbitrary 8×8 bit matrix over GF(2)** in **1 instruction**! Replaces 12–18 shift/mask instructions for byte bit-reversal (`0x8040201008040201ULL`), sub-byte bit permutations, base64/UTF transcoding, and `8×8` bit-matrix transposes.
3. **Daniel Lemire `StreamVByte` SIMD Integer Compression (`43.93×` faster than Protobuf `LEB128`):**
   - Never use `0x80`-continuation-bit `LEB128` (`127M ints/s`) for mesh index deltas, animation frames, or netcode snapshots! Separate **2-bit length tags** (`1` control byte per `4` integers) from packed data bytes; `1` load + `_mm_shuffle_epi8(raw, svb_lut[ctrl])` decodes **4 variable-length `uint32_t`s in 1 cycle (`5.64 Billion ints/s`)**!
4. **Bit-Serial / Bit-Transposed SIMD (`mu_bitserial.h`):**
   - Storing small `K`-bit attributes vertically across `K` bit-planes evaluates comparisons/Popcounts across **256 entities per AVX2 bitwise instruction**!

### 7.16 Systematic SIMD Width Upconversion ("Zipper Algorithm") & Single-Pass Distance Kernels

1. **Intel 10-Step "Zipper Algorithm" (`XMM -> YMM` or `YMM -> ZMM`):**
   When widening inline asm or narrow intrinsics, naive doubling breaks flags or reads past array ends:
   - **(1)** Convert 2-op non-VEX SSE to 3-op VEX & annotate integer flag liveness (`produced`/`consumed`/`clobbered`); **(2)** Replicate the narrow loop unchanged as the fallback (prevents silent OOB reads when `n % wide != 0`!) and switch `vmovaps` → `vmovups` unless 32B/64B alignment is proven; **(3)** Unroll the primary loop `2×` and double its count guard; **(4)** Rename 2nd-half vector destinations `ymm0..15 -> ymm16..31`; **(5)** Pairwise "zipper" swap 2nd-half ops upward beneath their 1st-half counterparts (adding `+32` to load/store displacements whenever crossing `add $32, %ptr`!); **(6–7)** Merge paired integer bumps (`$32+$32 -> $64`) and vector ops (`ymm0 + ymm16 -> zmm0`), checking signed/unsigned (`vpmovsx` vs `vpmovzx`, `vpsrad` vs `vpsrld`) and intra-128b-lane shuffles (`vpshufb`/`vshufps`) vs cross-lane `vpermd`/`vpermps`; **(8–10)** Promote loop-invariant broadcasts (`vinsertf64x4 $1`), fold accumulators (`vextractf64x4 $1` / `_mm512_reduce_add_ps`), update clobbers/CPUID/`vzeroupper`, and use an AVX-512 `{z}` masked tail (`__mmask16 k = (1u << rem) - 1; _mm512_maskz_loadu_ps`) to replace scalar tail loops.
2. **Single-Pass Multi-Accumulator Distance Kernels (`Cosine`, `Hamming`, `Jaccard`/`IoU`):**
   - **Cosine similarity:** Compute `dot_ab`, `dot_aa`, and `dot_bb` in **one single memory pass** with 4 independent SIMD FMA accumulators per stream (`12` YMM/ZMM regs → `dot_ab / sqrtf(dot_aa * dot_bb)`, skipping norms when inputs are pre-normalized!).
   - **Hamming & Jaccard (`IoU`):** Interleave `AND` + `OR` popcounts in one pass (`4-acc popcntq` `~24 GB/s`, AVX2 Harley-Seal `27–38 GB/s`, AVX-512 `vpopcntq`/`bitalg` `65–91 GB/s` in `mu_bitset.h`). When `popcnt(A)` and `popcnt(B)` are cached, `popcnt(A|B) = popA + popB - popcnt(A&B)` halves the work!

---

## 8. Concurrency -> Strictly Separated into Part II (`§M.1–§M.12`)

> **Separation Principle:** Do **not** Reach for threads to hide cache misses, bloated structs, or scalar branches — 2 threads fighting over poorly laid-out AoS memory just saturate the memory bus twice as fast. Exhaust single-core memory layout (§3–§6), branch/ILP discipline (§5), SIMD (§7), and cache-conscious data structures (§10) **first**.
>
> All multithreading, MESI cache coherency, lock-free queues, Linux kernel `seqlock` / RCU / SLUB per-thread freelists, and Naughty Dog user-space fiber job-system rules are consolidated in **Part II (`§M.1–§M.12`)** at the end of this document and in `.agents/skills/multithreading-dod/SKILL.md`.

---

## 9. Allocation and lifetime

### 9.1 One allocator strategy per program region

Global `malloc`/`free` in hot code is a design failure mode. Make lifetime
questions visible.

- **Frame arena** — allocate per frame, free everything by resetting a pointer.
- **Level/scope arena** — bulk allocation, bulk free.
- **Fixed pool** — same-size objects, freelist recycling.
- **Buddy / offset allocator** — general sizes with coalescing.
- **Static / fixed-capacity arrays** — zero allocation, zero surprises.

> If your game frame produces garbage, don't free — rewind.

### 9.2 Prefer static memory, but do not blindly make everything static

Excessive static allocation wastes memory and creates arbitrary capacity
limits. Choose per-region. Use `enum { MAX_X = ... }` next to the table.

### 9.3 Treat the stack as limited

Avoid large local allocations, oversized temporary structs, and unnecessarily
deep call chains. Stack overflows are hard to trace. Big scratch buffers belong
in an arena or a static buffer, not on the stack.

### 9.4 Handles with generations

```c
typedef struct { uint32_t id; uint32_t generation; } Handle;
```

A stale reference becomes a **stale handle that fails a compare** instead of
corrupting memory. `mu_bulk_storage` implements this (§13). Offsets over
pointers wherever identity must survive serialization or realloc.

### 9.5 O(1) deletion and reuse

```c
/* swap-remove: order need not be stable inside a table */
table[i] = table[--count];
```

Keep order only where iteration order matters; otherwise swap-remove.

### 9.6 Measure allocation and timing

```c
#include "mu/mu_perf.h"

MU_SCOPE_TIMER("integrate");
```

or wrap a block with `mu_time_now()` before/after in nanoseconds. Timings live
next to the code they measure so re-measuring is one line.

### 9.7 Avoid hidden allocation

No allocation behind iterators, virtual calls, or "just a helper." Allocation
points must be visible in the calling code.

### 9.8 Casey Muratori Zero-Is-Initialization (ZII) & 64-Bit Virtual Memory Arenas (`mu_vm.h`)

*(Sources: Casey Muratori 'Handmade Hero' / 'Performance-Aware Programming', Ryan Fleury 'Untangling Lifetimes', `mu_vm.h`)*

1. **Zero Is Initialization (ZII):** Design every struct in C so `memset(p, 0, sizeof *p)` (or OS zero-filled demand pages / `.bss`) is a **100% valid, ready-to-use empty state**! Never make `0` mean "active entity #0" in a handle/slot table — reserve index `0` as the null sentinel object (e.g., default white texture, identity matrix, zero velocity) so reading `table[0]` on an uninitialized handle is safe without a null branch!
2. **64-Bit Virtual Address Space Reserve + Demand Commit (`mu_vm_arena`):** Desktop x86-64 gives 48–57 bits (**256 TB+**) of virtual address space. Dynamic `realloc()` doubling (`cap *= 2`) invalidates pointers, copies megabytes, and fragments heap. Instead, **reserve** a huge contiguous virtual range up front (`mmap(..., 4ULL<<30, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)` — costs **0 bytes of physical RAM**!), and **commit** physical pages in 64 KB / 2 MB chunks on demand (`mprotect(..., PROT_READ|PROT_WRITE)`):
   - Arrays grow monotonically to gigabytes with **zero `memcpy`, zero `realloc`, and permanent address stability**!

### 9.9 OS Double-Mapped "Magic" Virtual Memory Ring Buffer (`mu_vm_ring`)

*(Sources: Linux `memfd_create` + `mmap` `MAP_FIXED`, Windows `MapViewOfFile3`, Casey Muratori, `mu_vm.h`; **2.79× faster (`0.84 ns` vs `2.35 ns`)** in `perf_test/bench_memory_cache.c` Case 7)*

Ordinary ring buffers split reads/writes across the wrap boundary (`if (head + len > cap) { memcpy_part1; memcpy_part2; }`), breaking SIMD loads, network packet parsers, and zero-copy spans.
**Fix:** Create an anonymous shared file (`memfd_create`), reserve `2 × cap` virtual bytes, and map the **exact same physical file pages twice back-to-back** at `[base, base + cap)` and `[base + cap, base + 2*cap)`! Every read or AVX2 load starting anywhere in `[0, cap)` sees linear contiguous memory seamlessly across the wrap boundary with **zero branches and zero scratch copies**.

### 9.10 Mesa `ralloc` Hierarchical Ownership & `linear_alloc`

*(Sources: Mesa 3D `src/util/ralloc.c`, compiler IR / scene-load lifetimes; **6.12× faster** frame bump rewind vs `malloc`/`free` in `perf_test/bench_memory_cache.c` Case 9)*

Compiler passes (like Mesa NIR), asset importers, and UI trees allocate thousands of temporary nodes that die together when the pass finishes. Never free them individually: allocate from a **parent-owned linear bump buffer** (`mu_arena` / `mu_frame_allocator` / Mesa `linear_alloc_child`) and destroy the entire tree in **O(1)** (`cursor = 0` or `munmap` one slab).

### 9.11 EA Frostbite FrameGraph Transient Resource Memory Aliasing

*(Source: Yuriy O'Donnell GDC 2017 'FrameGraph: Extensible Rendering Architecture in Frostbite')*

Render passes and simulation stages declare `[first_use_stage, last_use_stage]` lifetimes for every scratch buffer/texture. Buffers with non-overlapping lifetimes (e.g., G-Buffer normals in Stage 1 and Post-FX bloom scratch in Stage 5) **alias the exact same physical memory offset** (`offset_allocator.h` / `mu_arena` checkpoints), cutting total memory footprint by **30%–50%** AND keeping L3 cache lines hot across consecutive stages!

### 9.12 LLVM `SmallVector<T, N>` Stack-First Inline Buffer (`mu_small_vec.h`)

*(Sources: LLVM Programmer's Manual `llvm::SmallVector`, `mu_small_vec.h`; **1.59× faster** in `perf_test/bench_highperf_ds.c` Case 6, up to **10×** under multi-list heap contention)*

95%+ of local adjacency queries, bone-influence lists, polygon clip vertices, and AST/IR operands contain `<= 8` items. Embed `T inline_buf[N]` directly inside the header (`data == inline_buf` initially) using 32-bit `count`/`cap`, spilling to heap/arena ONLY on the rare `< 3%` overflow!

---

## 10. Searching and sorting

### 10.1 Do not search when the answer can be derived

If a value is sorted, indexed, directly addressable, or in a fixed slot, a
separate search is unnecessary. **The biggest optimization is avoiding
unnecessary work.**

### 10.2 Build indexes for hot repeated queries, not speculatively

```
cold query → linear search
repeated query → detect → build index → maintain on insert/update/delete
```

Only index data that actually becomes a hotspot. Runtime telemetry tells you
which queries requested indexes — so indexing infrastructure is also profiling
infrastructure.

### 10.3 Complexity does not equal performance

```
O(log N) with poor locality  can lose to  O(N) with excellent locality
```

for small enough ranges. A binary search jumping across memory can lose to a
linear scan through cache. When you load one cache line, use as much of it as
possible.

### 10.4 Bloom filters for cheap existence

```
possibly present (may be a false positive) | definitely absent (never a false negative)
```

```c
if (!bloom_maybe_contains(&health_bloom, id))
    continue;                       /* skip the expensive lookup */
/* ...otherwise do the real lookup... */
```

Use the `mu` hashing helpers rather than rolling your own.

### 10.5 Repeated "find best" becomes a maintained subset

Need nearest 3? Maintain `best[3]` and update it when the data changes, instead
of sorting everything every frame.

### 10.6 Choose the data structure by workload

```
frequent modifications   → hash table
mostly static data       → tree
very static data         → perfect hash
```

### 10.7 Sorting: sort only as much as necessary

```
1.  Do not sort unless sorting is required.
2.  Partition data before sorting when groups need different orders.
3.  Sort only as much as necessary.
4.  Partial sort when only the top/bottom N matters.
5.  Quickselect when only the Nth boundary matters.
6.  Use unstable operations when order is irrelevant.
7.  Maintain sorted data incrementally when updates are small (insertion sort).
8.  Radix/counting sort for suitable discrete ranges.
9.  Merge sort / parallel-friendly algorithms for large data.
10. Sorting networks for small fixed-size datasets.
11. Prefer cache-friendly algorithms over comparison-minimal ones.
12. Measure the real workload.
13. More threads ≠ proportionally more performance (bucket contention).
14. Sometimes computing everything beats branching to decide.
```

Discrete, known-range keys (material IDs, shader IDs, buffer indices) suit
**counting/radix sort**; continuous/analogue ranges suit comparison sort.

```c
#define MAX_MATERIALS 256u
void counting_sort_material(RenderCall *src, RenderCall *dst, uint32_t n)
{
    uint32_t bucket[MAX_MATERIALS] = {0};
    uint32_t offset[MAX_MATERIALS];
    for (uint32_t i = 0; i < n; ++i) ++bucket[src[i].material_id];
    uint32_t sum = 0;
    for (uint32_t m = 0; m < MAX_MATERIALS; ++m) { offset[m] = sum; sum += bucket[m]; }
    for (uint32_t i = 0; i < n; ++i) dst[offset[src[i].material_id]++] = src[i];
}
```

### 10.8 Rendering sort order (a concrete example)

- Sort **opaque** front-to-back: depth rejection kills overdraw before the
  fragment shader runs (`A` writes depth; `B` is rejected if covered).
- Sort **transparent** back-to-front: blending depends on draw order.
- Opaque cost is roughly `texture_cost + mesh_cost + state_change_cost +
  overdraw_cost` — measure the profile and choose the sort key. Support
  multiple criteria and pick per scene.
- Sort sound channels by amplitude relative to sample position for priority.

### 10.9 Keep search data separate from result data

Index/payload split keeps the searched structure small. Then joins become
intersections of sorted/posted lists:

```
EntityRenderable { entityID }
EntityPosition   { entityID }
join: entityID in both → AddRenderCall()
```

Repeated joins justify an index or a cached join result.

### 10.10 Modern SIMD Hash Tables: Abseil AVX2 SwissTable vs `ankerl::unordered_dense`

*(Sources: Abseil `flat_hash_map` (Kulukundis CppCon 2017), Boost `unordered_flat_map`, Martin Leitner-Ankerl `unordered_dense`, `mu_swiss_map.h`; **2.40×–6.44× faster lookup (`5.17–5.76 ns`) & 60.19×–178.17× faster iteration** in `perf_test/bench_highperf_ds.c` & `bench_mylib_upgrade.c`)*

**Never use node-based chained hash maps (`std::unordered_map` style) in hot C!** Choose between the two modern flat designs in `mu_swiss_map.h` by access pattern:
1. **Point-Lookup Dominated -> AVX2 SwissTable (`mu_swiss_map` in `mu_swiss_map.h`):**
   - Stores a flat `ctrl[]` array of 1-byte metadata per slot (`0x80` = EMPTY, `0xFE` = TOMBSTONE, `0x00..0x7F` = 7-bit `H2 = hash >> 57` fingerprint).
   - One `_mm256_loadu_si256` + `_mm256_cmpeq_epi8` + `_mm256_movemask_epi8` probes **32 hash slots in parallel in 1 cache line**, filtering out 99.2% of hash collisions before ever touching `keys[]`!
   - Mirror the first 32 control bytes at `ctrl[cap .. cap + 31]` so group loads at `cap - 1` never need wrap branches.
   - Hash integers with a fast 64-bit mixer (`wyhash` / `mum`: `__uint128_t r = (u128)(k ^ C0) * C1; return (u64)r ^ (u64)(r >> 64);`), never identity hash!
2. **Iteration + Lookup Hybrid -> `ankerl::unordered_dense` / Sparse-Set Map:**
   - Stores keys & values in a **100% dense contiguous array** (`dense_keys[0..count]`, `dense_vals[0..count]`), while the SIMD/Robin-Hood bucket array stores only `{ h2_or_dist, uint32_t dense_idx }`.
   - Deletion is O(1) swap-remove on `dense_*`, and full-table iteration runs at pure sequential SoA/array speed (**60.19× faster** than chained nodes!).

### 10.11 Static Sorted Search: Eytzinger BFS + Prefetch & 16-Ary AVX2 S+Tree

*(Sources: Paul-Virak Khuong & Pat Morin 'Array Layouts for Comparison-Based Searching', Sergey Slotin Algorithmica S+Tree; **5.92× faster (`45.9 ns`) for Eytzinger & 9.79× faster (`27.8 ns`) for 16-Ary AVX2 S+Tree** vs `271.9 ns` `lower_bound` on 16 MB in `perf_test/bench_highperf_ds.c` Case 2)*

When searching sorted arrays larger than L2 (`> 1 MB`):
1. **Eytzinger (1-Based BFS Heap Order):** Root at `eytz[1]`, children of `k` at `2*k` and `2*k + 1`. Siblings & cousins live side-by-side in cache lines, AND future addresses are known arithmetically: `MU_PREFETCH(&eytz[k * 16])` overlaps 4 levels of DRAM misses at once (`k = 2*k + (eytz[k] < x)`), recovering the final index via `__builtin_ffs(~k)`.
2. **16-Ary Cache-Line AVX2 Static S+Tree (Ultimate Desktop Static Index):** Pack **16 `int32_t` keys = 64 bytes = 1 exact cache line per B-tree node** in implicit `16`-ary heap order (`child_node = node * 16 + 1 + rank`). Each hop loads ONE 64B line, compares all 16 keys in 2 AVX2 `vpcmpgtd` instructions, and advances via `_mm_popcnt_u32(mask)`. Cuts `4.2M`-key search depth from **22 dependent hops down to 6** (**9.79× faster**)!

### 10.12 Linux Kernel Cache-Aligned Range & Index Trees (Maple Tree & XArray)

*(Sources: Linux Kernel 6.1+ `lib/maple_tree.c` (Liam Howlett & Matthew Wilcox), `lib/xarray.c`, `mu_radix_tree.h`, `mu_interval_map.h`)*

Linux replaced `vm_area_struct` red-black trees (`rbtree`, 24B/node, pointer-chasing every level) with the **Maple Tree**:
- **256-byte (`4 × 64B` cache-line) nodes** with branching factor **10–16** (internal) and **16–31** (leaf), storing disjoint non-overlapping key ranges `[piv_i, piv_{i+1})` directly in flat pivot+slot arrays.
- Dense integer ID -> pointer lookup uses **XArray** (`64`-slot radix nodes = `6` bits per level, cache-line friendly, zero hash collisions) — implemented in `mylib` via `mu_radix_tree.h` and `mu_int_trie.h`.

### 10.13 Register-Blocked AVX2 Split-Block Bloom Filters & Static Xor8 / Binary Fuse Filters

*(Sources: Putze et al., Apache Parquet Split-Block Bloom Filter, Graf & Lemire 'Xor Filters' / 'Binary Fuse Filters', `mu_bloom.h`; **3.43× faster (`2.56 ns` = 391M queries/s vs `8.78 ns`)** in `perf_test/bench_highperf_ds.c` Case 3)*

- **Dynamic Membership (`mu_bloom.h` / AVX2 Split-Block):** A classic `k = 6` Bloom filter touches **6 random cache lines per query** (`6` L3/DRAM misses!). Instead, route `hash_hi` to **one single 32-byte (or 64-byte) block**, generate an 8-lane 1-bit-per-lane mask via odd-multiplier SIMD hashing (`_mm256_mullo_epi32(h, K) >> 27`), and test all 8 probes in **one cache line and one `_mm256_testc_si256` (`vptest`) instruction**!
- **Immutable Static Membership:** Use **Daniel Lemire's Xor8 or Binary Fuse8 Filter** — uses only `~8.7–9.8 bits/key` (~30% smaller than Classic Bloom at 0.39% FP rate) and tests `fp == (F[h0] ^ F[h1] ^ F[h2])` in **3 independent parallel loads** (full MLP, zero branches!).

### 10.14 4-Level 64-Ary Summary Hierarchical Bitset (`mu_hier_bitset.h`)

*(Sources: Rust `hibitset`, `mu_hier_bitset.h`; **318.68× faster (`0.38 µs` vs `122.0 µs`)** over 16.7M IDs in `perf_test/bench_highperf_ds.c` Case 4)*

When a bitset spans millions of IDs (`16.7M = 64⁴`) and active density drops below `0.1%`, even scanning `uint64_t` words wastes megabytes of L2 bandwidth on zeros. Maintain a **4-level 64-bit summary tree**:
- `L0` (1 `uint64_t` root word) -> `L1[64]` -> `L2[4096]` -> `L3[262144]` leaf words.
- `tzcnt` down 4 summary registers jumps directly across **262,144 empty bits per L1 bit** in **true `O(active_bits)` time**!

### 10.15 Intrusive Lists When Truly Needed: Linux `hlist_node` (`**pprev`) & Mesa Sentinel `exec_node`

*(Sources: Linux Kernel `include/linux/list.h`, Mesa NIR `src/compiler/nir/list.h`; **1.59× faster** + 50% smaller bucket array in `perf_test/bench_branch_ilp_alu.c` Case 8)*

Prefer flat arrays (`array_push` + swap-remove) or index lists (`mu_index_list`) 99% of the time. In the rare 1% where O(1) splice/LRU/timer-wheel bucket unlinking is required:
1. **Hash / Timer-Wheel Buckets -> Linux `hlist_head` (`{ first }` = 8 bytes, half of `{ head, tail }`!):** Store `struct hlist_node *next, **pprev;` where `pprev` points to the *pointer variable* referencing this node (`&bucket->first` OR `&prev->next`). Unlinking is `*n->pprev = n->next; if (n->next) n->next->pprev = n->pprev;` — **zero head-special-case branch** and you don't even need the bucket pointer to unlink!
2. **Compiler IR / Splice Lists -> Mesa Amiga-Style Sentinel / Linux Circular `list_head`:** Dummy head/tail sentinel nodes ensure `n->prev` and `n->next` are **never `NULL`**, turning unlink/insert into 4 unconditional stores with **zero `if`s**.

### 10.16 Single-Histogram 4-Pass LSD Radix Sort & IEEE-754 Float Key Flip (`mu_sort.h`)

*(Sources: Pierre Terdiman 'Radix Sort Revisited', Michael Herf `FloatFlip`, EA Frostbite; **9.53× faster (`10.0 ns/key` = 99.7M keys/s vs `95.6 ns/key` `qsort`)** in `perf_test/bench_highperf_ds.c` Case 5)*

1. **Compute ALL 4 byte-pass histograms (`hist[4][256] = 4 KB`, fits in L1d!) in ONE single scan** over `src[]` before scattering.
2. **Zero-Pass Skip:** If `hist[pass][byte_of_first_elem] == count`, all keys share identical byte `pass` (e.g. high byte `0x00`) — **skip that pass completely in O(1)**!
3. **Sort `float` depths with integer Radix Sort (`Michael Herf FloatFlip`):** IEEE-754 floats already sort lexicographically when positive; flip all bits if negative (`mask = -(int32_t)(u >> 31) | 0x80000000u; sortable = u ^ mask;`) to map `[-inf, +inf]` monotonically onto `[0, 0xFFFFFFFFu]`, sort with LSD Radix, and unflip (`u ^ (((u >> 31) - 1u) | 0x80000000u)` — **`18.67×` faster than `qsort`** in `mu_radix_sort_f32_scratch`!).
4. **In-Place SIMD Quicksort / Quickselect (`numpy/x86-simd-sort`, `3–8×` vs `std::sort`):** When `O(N)` radix scratch space is undesirable or `nth_element` (`qselect`), `partial_sort` (`partial_qsort`), or `argsort` is needed over numeric primitives (`f32/f64/i32/u32/i64/u64`), AVX-512 `vcompressps`/`vpcompressd` partitioning + bitonic networks avoid comparison branches. **Critical caveats:** not stable; passing `NaN` with `hasnan=false` is **Undefined Behaviour**; `qsort(..., hasnan=true)` canonicalizes NaN payloads to `quiet_NaN`, and `argsort` silently degrades to scalar `std::sort` when NaN is present.

### 10.17 4-Ary Cache-Friendly Heaps (`mu_four_heap.h`), Timer Wheels (`mu_timer_wheel.h`) & Wide BVH4/BVH8 (`mu_bvh.h`)

- **4-Ary Heap (`mu_four_heap.h`) over Binary Heap:** All 4 children `4i + 1 .. 4i + 4` sit contiguously on **1 cache line**, halving `sift_down` tree height (`log₄ N` vs `log₂ N`) and cache misses!
- **Hierarchical Timer Wheel (`mu_timer_wheel.h`) over Priority Queue:** Game/network timeouts (millions of timers) are **O(1) insert, O(1) cancel, O(1) amortized tick** using power-of-2 bitmasked wheel slots (Linux kernel `kernel/time/timer.c` pattern) instead of `O(log N)` heap churn.
- **Wide SIMD BVH4 / BVH8 (`Intel Embree / Valve`):** Store `4` (SSE) or `8` (AVX2) child AABBs in SoA layout inside each BVH node (`float min_x[8], max_x[8], ...`) so a single ray tests **8 child bounding boxes in 1 SIMD pass**, slashing BVH pointer depth by `3×`!

### 10.18 Roaring Bitmaps (`209.45×`), Static `Xor8` Filters (`3.42×`) & Perfect Hashing (`mu_perfect_hash.h`)

*(Empirically proven in `perf_test/bench_advanced_c_isa.c` Cases 5 & 8)*

1. **Roaring Bitmaps (`RoaringBitmap` — Lemire, Chambi et al.; `209.45×` faster than sorted `uint32[]` merge!):**
   - Partition 32-bit IDs by high 16 bits (`id >> 16`) into chunks of `65,536`:
     - `< 4,096` IDs in chunk -> **Sorted `uint16_t[]` array** (`<= 8 KB`, compact when sparse).
     - `>= 4,096` IDs in chunk -> **Fixed `uint64_t[1024]` bitset** (`8 KB`, fits in L1d, intersected via SIMD `vpand` + `popcnt`!).
     - Sparse × Dense chunk intersection simply tests `sparse_len` bits directly in the `8 KB` bitset in `O(|sparse|)`! (`mu_chunked_bitset.h` / `mu_sparse_bitset.h`).
2. **Static `Xor8` / Binary Fuse Filters (`3.42×` faster & `38%` smaller RAM than 8-probe Bloom):**
   - When a key set is immutable after load (asset manifests, dictionary words, SSTables), peel a 3-way hypergraph into an `Xor8` table (`1.23 × N` bytes = `9.84 bits/key`, `0.39%` FP). Membership `expect == (fp[i0] ^ fp[i1] ^ fp[i2])` dispatches **3 independent parallel loads** to MSHRs with **zero branches**!
3. **Compile-Time / Static Key Sets -> Minimal Perfect Hash (`mu_perfect_hash.h`):**
   - Zero collisions, deterministic `O(1)` lookup in `<= 2` array reads for fixed keyword/shader-parameter/command tables.

---

## 11. Simulation: state, LOD and memory of the world

### 11.1 State is data

```
Traditional:  state variable → branch → behaviour
DOD:          table membership → processing set → behaviour
```

```c
EntityId idle[MAX_ENTITIES];    size_t idle_count;
EntityId walking[MAX_ENTITIES]; size_t walking_count;
EntityId jumping[MAX_ENTITIES]; size_t jumping_count;
/* entity 42 is walking iff 42 ∈ walking[] */
```

Transition = remove from one table, insert into another.

### 11.2 Evaluate, then commit (transform-oriented FSM)

```
old_state + signals
      ↓
  condition tables
      ↓
 transition_requests
      ↓
   triggers / logging
      ↓
     commit
      ↓
  new_state
```

```c
typedef struct { EntityId entity; int next_state; } Transition;
Transition transitions[MAX_ENTITIES]; size_t transition_count;

/* evaluation: reads old state, writes requests */
for (size_t i = 0; i < walking_count; ++i) {
    EntityId id = walking[i];
    if (jump_pressed(id))
        transitions[transition_count++] = (Transition){ id, STATE_JUMPING };
}

/* commit: single mutation point */
for (size_t i = 0; i < transition_count; ++i)
    apply_transition(transitions[i].entity, transitions[i].next_state);
```

Benefits: deterministic update step, no signal-order dependence, easy
transition logging/telemetry, and state tables double as processing sets
(`Damaged[] → DamageSystem`, `Burning[] → FireSystem`,
`PotentiallyVisible[] → CullingSystem`).

Limitations: when ordering between signals is *fundamentally* part of the
problem (a lexer/parser), a traditional sequential FSM is correct. Use the
transform model when signals can be evaluated together and committed after.

### 11.3 LOD is a function of *relevance*, not distance

```
LOD = f(relevance)
relevance = f(distance, screen-space size, visibility, player attention,
              time since last seen, interaction history, progression,
              player expertise, gameplay importance)
```

LOD controls far more than polygons:

```
LOD 0: full physics, full AI, animation, weapons, high-res mesh
LOD 1: simplified physics/AI, low-res animation, medium mesh
LOD 2: approximate movement, 1 Hz AI, billboard
LOD 3: no individual aircraft — squadron data
LOD 4: no squadron entity — wave data
```

```
far  + irrelevant → low detail
far  + important  → high detail
near + irrelevant → medium detail
near + important  → high detail
```

Interaction history matters: an ambient car the player never touched may
regenerate from a seed; the player's parked car must retain its state.

### 11.4 Reduce instance count before optimizing instances

```
Collective LOD:  do these things need to exist as separate objects at all?
```

```
100 aircraft → 1 Squadron { count = 100, attack_probability }
```

```c
typedef struct { int aircraft_count; float attack_probability; } Squadron;
int simulate_attack(const Squadron *s)
{
    return (int)(s->aircraft_count * s->attack_probability);   /* believable, not exact */
}
```

The escalation chain:

```
full entity → simplified entity → collective → aggregate/count → implicit/procedural
```

### 11.5 Mementos — preserve what matters, discard precision

A memento is the **compressed historical information required for future
refinement**, not "save data":

```
High-detail state
   ├── important information ──► Memento
   ▼
Low-detail state

later:  Low-detail state + Memento → High-detail state
```

Lossy is allowed: `2/3 of windows are broken` instead of listing every window.
The player perceives consistency; they do not audit history.

### 11.6 JIT mementos — don't store derivable history

```
Ordinary memento: detail → store compressed memory → rebuild
JIT memento:      store a recipe (seed) → generate on demand
```

```c
static uint64_t hash64(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

uint64_t seed = hash64(car_id);
driver       = generate_driver(seed);
passenger[0] = generate_passenger(seed + 1);
```

The same seed gives the same result every visit, so the identity itself carries
the memory. Global RNG (`R_{t+1} = f(R_t)`) is wrong here — it depends on
everything that happened before. Use `R = f(seed)`.

### 11.7 Procedural generation wants bias, not uniform randomness

```
believable world = repetition + variation + imperfections
                   structured bias × randomness
```

```c
/* P(driver | convertible) = young-and-fashionable-biased,
   not "convertible MUST have a young driver" */
```

### 11.8 Derived state beats duplicated state

```c
/* WRONG: two booleans that can disagree */
bool in_options; bool in_volume_menu;

/* RIGHT: derive from one axis */
int menu_depth;
bool options_visible = menu_depth >= 1;
bool volume_visible  = menu_depth >= 2;
```

`state = f(other_state)` is safer than maintaining many independent flags.
Explicit state machines multiply transitions and inconsistent states; an
axis-based representation (`state = f(axis)`) shrinks the transition surface.

---

## 12. C style mechanics (my style)

### 12.1 Naming

```
Types:              PascalCase         (EntityId, RenderTarget)
Functions:          snake_case         (physics_update, array_push)
Variables/fields:   snake_case         (mesh_id, visible_count)
Constants/macros:   SCREAMING_CASE     (MAX_ENTITIES, MU_ALIGN_UP)
```

Align related declarations so the table scans vertically (Carmack style):

```c
typedef struct {
    TextureId  texture_id;
    MeshId     mesh_id;
    MaterialId material_id;

    uint32_t   width;
    uint32_t   height;
    uint32_t   stride;
} DrawItem;
```

### 12.2 Declarations at the top of the function

C89-style, block-visible, one place to see everything the function owns.

```c
static bool physics_step(Physics *p, const Input *in, float dt)
{
    EntityId id;
    Vec3      accel;
    size_t    i;

    /* ... */
    return true;
}
```

### 12.3 IDs, not long-lived pointers ("weak pointers")

```c
/* WRONG */
Texture *texture;  Mesh *mesh;  Material *material;

/* RIGHT */
TextureId  texture;
MeshId     mesh;
MaterialId material;
```

### 12.4 Static or arena-backed storage, with care

Prefer explicit, predictable allocation. Do not make everything static
(§9.2), and do not overfill the stack (§9.3).

### 12.5 Design out null checks

Do not eliminate null checks by ignoring errors — eliminate them with
**stronger invariants**.

```c
/* avoid this */
Texture *t = get_texture(id);
if (t) { if (t->image) { if (t->view) { /* ... */ } } }

/* prefer: resolve and validate at the boundary, then pass valid data in */
```

If an operation can genuinely fail, make failure **explicit** through a boolean
result, an invalid ID, an error value, or an output parameter. The goal is not
"fewer null checks," the goal is **stronger contracts**.

### 12.6 Functions and APIs

- Prefer meaningful, substantial functions. Do not split code into tiny
  functions for style alone. Extract a function when it gives meaningful reuse,
  a useful boundary, or easier testing.
- Prefer APIs that make misuse difficult and that state ownership, lifetime,
  validity and failure semantics.
- Resolve/validate resources at system boundaries, not in inner loops.
- Systems are free functions over tables:
  `void physics_update(PhysicsTable *p, const InputTable *in, float dt);`
- Pass state explicitly: `render_frame(Renderer *r, Frame *f);` — no hidden
  global access. Minimize global state and side effects.
- Separate preparation from execution; push validation to boundaries and keep
  inner loops clean.

### 12.7 Compiler flags and invariants

```sh
cc -std=c99 -O2 -Wall -Wextra -Werror -fno-strict-aliasing -march=native \
   -D_POSIX_C_SOURCE=199309L      # needed by mu_perf.h on glibc
```

- `-O3` / `-march=native` when you have measured; verify with `-fopt-info-vec`.
- `assert` / `MU_ASSERT` for table invariants in debug builds only.
- `MU_STATIC_ASSERT` for layout facts (struct sizes, power-of-two caps).
- No `int` in serialized structs — fixed-width integers everywhere.

### 12.8 Comments

Explain **why**, not what. A comment restating the code is noise; a comment
explaining an invariant, a tradeoff, or a hardware reason is gold.

### 12.9 Code quality checklist

```
- control flow obvious
- ownership obvious
- lifetimes predictable
- no hidden work, no hidden allocation
- no unnecessary indirection
- no unnecessary abstraction
- simple mechanisms with measurable cost
- debug-only assertions for invariants
```

### 12.10 Desktop x86-64 ABI Struct-By-Value Rules (SysV Linux vs Win64)

- **System V x86-64 ABI (Linux / macOS / BSD):** Structs **`<= 16 bytes`** (`2` eightbytes — e.g., `mu_span { ptr, len }`, `Vec2`, `Handle64`) pass & return **100% in two registers (`rdi/rsi` or `xmm0/xmm1`, returning in `rax/rdx` or `xmm0/xmm1`)** with zero stack memory traffic! Structs **`> 16 bytes`** (even `24` bytes like `{ ptr, size_t len, size_t cap }`!) are silently demoted to **`MEMORY` class** — spilled to the stack and returned via hidden pointer `sret`, risking STLF stalls! Keep hot value types `<= 16 bytes` (use `uint32_t len, cap` like LLVM `SmallVector`).
- **Windows x64 ABI:** Only structs of exact size **1, 2, 4, or 8 bytes** pass in GPRs; `12B`/`16B` structs pass via hidden stack pointer unless passed as `__m128` / `__vectorcall` or `const T * MU_RESTRICT`.

### 12.11 Compiler Hints, Assumptions & Attributes That Unlock Vectorization

1. **Alignment & Multiple-of-Width Proofs (Kills Prologue Peeling & Scalar Tail Epilogues!):**
   ```c
   float *x = __builtin_assume_aligned(raw_x, 64);
   if (count == 0 || (count & 15u) != 0) __builtin_unreachable(); /* proves 16-multiple! */
   ```
   Giving the compiler these two facts lets GCC/Clang emit **pure aligned `vmovaps` loops with zero scalar peel prologue and zero scalar remainder epilogue**!
2. **Function Purity & Call-Tree Flattening:**
   - `__attribute__((const))` (depends *only* on value arguments, reads no memory) & `__attribute__((pure))` (reads memory, zero side effects) let the compiler hoist calls out of loops (LICM) and common-subexpression eliminate them.
   - `__attribute__((flatten))` on a hot kernel entry point (`sys_cull`, `rasterize_tile`) force-inlines its entire internal call tree into one tight function while keeping helper source code modular (used in Mesa & `dav1d`).
3. **Strict-Aliasing-Safe Type Punning:** Always reinterpret float/integer bits or unaligned words via `memcpy(&u, &f, sizeof u)` — in C99/C11 under `-O2`/`-O3`, `memcpy` of `1, 2, 4, 8` bytes compiles to **zero function call (`vmovd` / single `mov`)** without UB!
4. **Cold-Path Callee Outlining & Branch-Probability Verification (`intel/performance-patterns`):**
   - Mark error/logging/corrupt-input callees `__attribute__((cold))` (or `[[gnu::cold]]`) at the **function definition** (annotating only the declaration is a no-op, and coldness does not propagate to sub-callees!). This moves cold code past the hot caller's `ret`, protecting L1i & DSB density.
   - Confirm `< 0.1%` branch taken probability on Intel via `perf record -c 1000 -e '{BR_INST_RETIRED.NEAR_TAKEN:upp,BR_INST_RETIRED.NOT_TAKEN:upp,cycles:u}'` (shifting macrofused `cmp/test` counts onto the adjacent conditional jump!), or statically on any arch via GCC `-g -fdump-tree-profile_estimate-lineno -dumpdir dump/`.
5. **Runtime CPU Multi-Versioning & Dispatch Traps (`target_clones` vs `__builtin_cpu_supports`):**
   - **Mechanism A (`__attribute__((target_clones("default","avx2,fma","avx512f")))`):** Works **only** on plain auto-vectorized C (does NOT widen `_mm256_*` intrinsics!), **must** be non-`static` AND `__attribute__((noinline))` so the IFUNC `.resolver` is emitted (`objdump -t app | grep .resolver`), and GCC rejects `"x86-64-v3"`/`"v4"` strings in `target_clones`. Avoid IFUNCs in `.so` shared libraries (early dynamic-relocation init-order hazard).
   - **Mechanism B (`__builtin_cpu_supports` + `static __attribute__((target(...)))` variants or separate `.c` TUs):** Required for explicit intrinsics and `.so` libraries (check widest first: `"x86-64-v4"` → `"x86-64-v3"` → `"x86-64-v2"` → scalar; on Arm Linux use `getauxval(AT_HWCAP)`). Watch exact ISA flags: `_mm512_popcnt_epi8/16` requires **`avx512bitalg` + `avx512bw`** (`avx512vpopcntdq` only covers 32/64-bit `epi32/64`!).

### 12.12 Whole-Program Build Flags: LTO, PGO, `-fno-plt` & Post-Link `BOLT`

For shipping desktop release builds:
- **`-O3 -march=native -flto=auto -fno-plt`:** Enables cross-translation-unit inlining, constant propagation across `.c` files, and direct indirect calls without PLT trampolines.
- **Profile-Guided Optimization (`-fprofile-generate` -> run benchmark/replay -> `-fprofile-use`):** Gives **+5%–15%** across branch-heavy compilers/engines by measuring real branch probabilities and hot/cold call edges.
- **Post-Link Binary Layout (`llvm-bolt` / `Propeller`):** Reorders basic blocks and functions using hardware LBR (Last Branch Record) samples to slash L1i and ITLB misses by **+8%–15%** on large codebases (used on Linux kernel, Clang, and Chrome).

---

## 13. `mylib` (`mu`) — the standard library. Use it.

**Rule:** prefer `mylib` for data structures and allocators. If `mylib` lacks a
primitive that the data-oriented design needs, **add a small reusable primitive
to `mylib`** (e.g. under `mylib/mu/`) rather than introducing an ad-hoc
container or a competing allocation/ownership model. External libraries should
not dictate engine architecture.

Include the core with the umbrella header:

```c
#include "mu.h"        /* containers, allocators, bitsets, spans, RNG, perf */
```

Subsystems are separate single headers:

```c
#include "mu_ecs.h"               /* entities, pools, transforms, prefabs */
#include "mu_thread_system.h"     /* worker pool + atomics                 */
#include "mu_task_scheduler.h"    /* priorities, groups, parallel_for      */
#include "allocators.h"           /* arena, pool, buddy                    */
#include "offset_allocator.h"     /* two-level bin free-space allocator    */
#include "mu_mmgr.h"              /* leak/corruption tracking malloc shim   */
#include "mu_min_containers.h"    /* standalone stretchy buffer + hash64    */
#include "mu_geometric_algebra.h" /* PGA / conformal math (needs cglm)      */
#include "mu_noise_math.h"        /* value/perlin/simplex/worley/fBm/warp   */
#include "mu_roguelike.h"         /* grid gen, FOV, pathing, tick scheduler  */
```

### 13.1 Pick by access pattern (Verified Exact `mu/*.h` Header Matrix)

| Need / Access Pattern | Exact `mylib` Header (`mu/*.h`) | Hardware Why |
|---|---|---|
| Dense hot dynamic array | `mu_array.h` / `mu_line_array.h` | Contiguous unit-stride L1/L2 prefetcher stream |
| Cache-line chunked stable array | `mu_chunked_array.h` | Zero-move growth across fixed power-of-2 chunks |
| Stack-first short scratch list (`<= N`) | `mu_small_vec.h` | LLVM `SmallVector`: zero heap calls for common small `N` (`1.59×`) |
| Non-owning typed slice (`<= 16B` ABI) | `mu_span.h` | LLVM `ArrayRef`: `{ ptr, count }` in 2 registers, zero copy |
| Stable handles + holes + generations | `mu_bulk_storage.h` / `mu_id_pool.h` / `mu_id_alloc.h` | Dense/slot pool + generation counter (zero ABA dangling ptrs) |
| Sparse active IDs in `[0, MAX)` | `mu_sparse_set.h` / `mu_sparse_array.h` | O(1) membership + packed dense iteration (`121.72×` at 1%) |
| Fast flat open-addressed hash table | `mu_swiss_map.h` / `mu_hash_table.h` / `mu_min_containers.h` | `mu_swiss_map` 16-way SIMD H2 ctrl (`6.44×`); `mu_dense_map` SoA iter (`178.2×`) |
| Zero-collision static key lookup | `mu_perfect_hash.h` | Deterministic O(1) minimal perfect hash for static keys |
| Static sorted search (> L2/L3 cache) | `mu_eytzinger.h` / `mu_sorted_map.h` / `mu_range_remap.h` | BFS `mu_eytzinger_u32/u64` (`2.52×`) & 16-ary AVX2 `mu_stree16_i32` (`4.99×`) |
| `key -> many values` | `mu_multi_index.h` | Flat pool-backed posting lists without per-node `malloc` |
| Dense boolean stream & set algebra | `mu_bitset.h` | SIMD `vpopcntq`/`vpshufb` (`91.1 GB/s`, `7.08×`) + CTZ set-bit skip (`11.85×`) |
| Stable-growth / adaptive sparse bitsets | `mu_chunked_bitset.h` / `mu_sparse_bitset.h` | 512-bit cache-line chunks; skips empty regions (Roaring style `209×`) |
| Ultra-sparse million-ID bitset | `mu_hier_bitset.h` | 4-level `64⁴` summary tree, `O(active)` scan (`318.68×`) |
| Vertical bit-plane SIMD filtering | `mu_bitserial.h` | Bit-transposed planes: 256 elements per AVX2 bitwise op |
| Cache-local / static set pre-filter | `mu_bloom.h` / `mu_xor_filter.h` | `mu_bloom_simd256` AVX2 (`8.31×`); static `mu_xor8_filter` (`9.84 b/key`, `5.37×`) |
| Static / immutable graph, DAG, toposort | `mu_csr.h` / `mu_dag.h` / `mu_toposort.h` | Flat `offsets[]` + `edges[]` CSR (`32.72×` over pointer graph) |
| Spatial broadphase & 2D/3D cells | `mu_spatial_hash.h` / `mu_spatial_array.h` / `mu_fast_codec.h` | 2-pass CSR grid + BMI2/dilated 2D/3D Morton codes (`mu_morton2d/3d`) |
| Balanced tree / interval queries | `mu_rb_tree.h` | Pool-backed node storage (no per-node `malloc`) |
| Sorting keys / floats / render packets | `mu_sort.h` | 1-hist + pass-skipping LSD radix (`u32`/`u64`) + `FloatFlip` `mu_radix_sort_f32` (`18.67×` vs `qsort`) |
| Priority queue / timers / handle heap | `mu_four_heap.h` / `mu_indexed_heap.h` / `mu_timer_wheel.h` | SoA 4-ary heap (`2.25×`), indexed decrease-key, O(1) bitmasked timer wheel (`15.42×`) |
| Deduplicated BFS / compiler worklist | `mu_worklist.h` | Mesa `u_worklist` style FIFO ring + bitset dedup |
| Connected components | `mu_union_find.h` | Flat path-halving + union-by-rank `uint32_t[]` |
| Virtual memory giant arena / magic ring | `mu_vm.h` / `mu_vm_arena.h` | `mmap` reserve+commit (`0` realloc) + double-mapped ring (`2.79×`) |
| Per-frame / general arenas & pools | `mu_frame_allocator.h` / `mu_allocators.h` / `allocators.h` | Bump pointer + O(1) cursor rewind (`6.12×` over `malloc`) |
| Sub-allocation / GPU buffer aliasing | `mu_offset_allocator.h` | O(1) two-level segregated fit (`ffs`/`fls` bitmasks, Frostbite style) |
| Interned / arena strings | `mu_intern.h` / `mu_string.h` | Intern strings to `uint32_t` IDs -> 1-cycle integer compare! |
| Portable SIMD, codecs, hashing & RNG | `mu_simd.h` / `mu_fast_codec.h` / `mu_bitpacking.h` / `mu_pcg.h` / `mu_perf.h` | `mu_f32x4` & 8-wide `mu_f32x8` + `transpose8`, AES-NI/wyhash (`54.3 GB/s`), `fastmod`, `StreamVByte` |
| *[Part II — MT]* SPSC lock-free ring | `mu_spsc_queue.h` | Acquire/release + shadow-cached + batched `mu_spsc_push_batch`/`pop_batch` (`127.12×`) |
| *[Part II — MT]* Atomics, Seqlock, RCU | `mu_atomic.h` / `mu_sync.h` / `mu_deferred_free.h` | Zero-RMW readers (`27.44×`), epoch retire |
| *[Part II — MT]* Per-thread Slab | `mu_slab.h` / `mu_slab_manager.h` | SLUB/Mesa lockless per-thread freelist (`932.84×`) |
| *[Part II — MT]* Task / parallel-for | `mu_task_scheduler.h` / `mu_thread_system.h` | Work-stealing tasks, counters, `parallel_for` |

### 13.2 The pieces you will actually use

```c
/* ---- arrays: header-prefixed stretchy buffer ------------------------- */
float *a = NULL;                 /* NULL is a valid empty array */
array_push(a, 1.0f);
array_size(a);  array_capacity(a);  array_reserve(a, n);
array_pop(a);   array_clear(a);     array_free(a);

/* ---- spans: non-owning views ---------------------------------------- */
mu_span_f32 s = mu_span_f32_make(v, 4);      /* typed alias: count = elements */
mu_span     b = mu_span_from_array(v);       /* plain mu_span: count = bytes  */
mu_span_f32_find(s, v);  mu_span_f32_sub(s, 1, 3);  mu_span_f32_copy_to(s, dst, n);

/* ---- bitsets: packed 64-bit words, full set algebra ------------------ */
mu_bitset *bs = mu_bitset_create_with_capacity(130);
mu_bitset_set(bs, 63);  mu_bitset_test(bs, 63);
mu_bitset_inplace_union(bs, other);
size_t i = 0; while (mu_bitset_next_set_bit(bs, &i)) { /* member */ ++i; }
mu_bitset_for_each(bs, visitor, user);

/* ---- sparse set: O(1) membership over [0, capacity) ------------------ */
mu_sparse_set_init(&set, MAX_ENTITIES);
mu_sparse_set_insert(&set, 42);
mu_sparse_set_contains(&set, 42);
mu_sparse_set_remove(&set, 42);          /* swap-delete, dense iteration */
for (uint32_t k = 0; k < set.count; ++k) { uint32_t v = mu_sparse_set_at(&set, k); }

/* ---- bulk storage: slots with holes + generational handles ---------- */
mu_bulk_storage_init(&st, sizeof(MyItem), 64);
uint32_t id = mu_bulk_storage_alloc(&st);
MyItem  *it = (MyItem *)mu_bulk_storage_ptr(&st, id);
mu_weak_handle h = mu_bulk_storage_make_handle(&st, id);
MyItem *live = (MyItem *)mu_bulk_storage_resolve_handle(&st, h);  /* NULL if stale */

/* ---- hashes: fixed-capacity, no hidden growth ----------------------- */
mu_hash32_t h; mu_hash32_init(&h, 1024);
uint32_t out; mu_hash32_get(&h, key, &out);      /* true/false, no magic 0 */
mu_hash32_set(&h, key, value);  mu_hash32_remove(&h, key);

/* ---- id pool: single ids or contiguous ranges ----------------------- */
mu_id_pool_init(&pool, 4096);
uint32_t first; mu_id_pool_create_range_id(&pool, &first, 16);

/* ---- multi index: one key → many values ----------------------------- */
mu_multi_index_init(&idx, node_cap, map_cap);
uint32_t first = mu_multi_index_first(&idx, key);
mu_multi_index_add(&idx, key, value);
for (uint32_t n = first;
     mu_multi_index_node_valid(&idx, n);
     n = mu_multi_index_next(&idx, first, n))
{ uint32_t v = mu_multi_index_value(&idx, n); }

/* ---- chunked u32 arrays: many small variable lists ------------------ */
mu_chunked_u32_array_push(&poolc, &arr, 42);

/* ---- allocators ----------------------------------------------------- */
mu_arena arena; mu_arena_alloc(&arena, size, align); mu_arena_reset(&arena);
mu_pool  p;     mu_pool_init(&p, buf, blk, n);  mu_pool_alloc(&p); mu_pool_free(&p, ptr);
mu_buddy bb;    mu_buddy_init(&bb, arena_mem, arena_size, leaf, meta, meta_size);

/* ---- RNG: deterministic, seedable (PCG) ----------------------------- */
mu_pcg32 rng; mu_pcg32_init(&rng, seed, seq);
uint32_t r = mu_pcg32_next_u32(&rng);

/* ---- perf ----------------------------------------------------------- */
MU_SCOPE_TIMER("physics");
```

### 13.3 `mu_common.h` macros worth knowing

```c
MU_ASSERT(cond);            MU_PANIC();
MU_INLINE;  MU_NOINLINE;    MU_RESTRICT;      MU_ALIGN(N);
MU_LIKELY(x); MU_UNLIKELY(x); MU_PREFETCH(addr);
MU_MIN(a,b); MU_MAX(a,b);   MU_CLAMP(x,lo,hi);  MU_SWAP(T,a,b);
MU_ARRAY_COUNT(a);  MU_IS_POW2(x);  MU_ALIGN_UP(x,a);  MU_ALIGN_DOWN(x,a);
MU_KB/MB/GB(x);  MU_CEIL(x,y);
MU_BIT(n); MU_HAS_FLAG/SET_FLAG/CLEAR_FLAG;
MU_STATIC_ASSERT(cond, msg);
mu_trailing_zeroes_u64(x);  mu_leading_zeroes_u64(x);  mu_popcount_u64(x);
```

### 13.4 `mu_ecs.h`

30-bit-ish entities (index + generation), component pools backed by dense
arrays + an entity→dense map, SoA transforms with **index**-based hierarchy
links, and prefabs/resource registry. Key calls: `mu_ecs_entity_manager_init(&em, min_free)`,
`mu_ecs_entity_create(&em)`, `mu_ecs_entity_destroy(&em, e)`,
`mu_ecs_pool_init(&p, elem_size)` / `mu_ecs_pool_add/get/remove(&p, e)`,
`mu_ecs_transform_init(&t)` / `mu_ecs_transform_add(&t, e)` /
`mu_ecs_transform_set_local(&t, inst, m16)` / `mu_ecs_transform_propagate(&t, root)`.
Use this before writing a new ECS.

### 13.5 `mu_thread_system.h` / `mu_task_scheduler.h`

```c
/* TaskFunc: void (*)(void *user, uint64_t thread_id) */
mu_ts_init_desc td = MU_TS_INIT_DESC_DEFAULT;
td.thread_count = 4;                       /* 0 = deterministic dummy mode */
mu_ts_handle ts; mu_ts_init(&ts, &td);
mu_ts_add_task(ts, fn, user);
mu_ts_add_tasks(ts, fn, count, sizeof(user_t), user_array);
mu_ts_wait_idle(ts);

/* TaskFunc: void (*)(void *user, uint32_t worker_index) */
mu_sched_init_desc sd = MU_SCHED_INIT_DESC_DEFAULT;
sd.worker_count = 4;
mu_task_scheduler *sched; mu_sched_init(&sched, &sd);
mu_sched_task_desc desc = { .fn = fn, .user = user,
                            .priority = MU_SCHED_PRIORITY_NORMAL,
                            .completion_counter = &counter };
mu_sched_submit(sched, &desc);
mu_sched_parallel_for(sched, begin, end, grain,
                      MU_SCHED_PRIORITY_NORMAL, fn, user);
mu_sched_counter_wait(&counter);
```

Customization is by macros, not vtables (`MU_TS_MALLOC`, `MU_SCHED_*`). Assert
on contract violations; return `NULL`/`false` for recoverable failures.

---

## 14. Anti-patterns — do not do these

```
✗ A struct with 20+ unrelated fields consumed by different systems.
✗ Pointers as long-lived identity (they break serialization, realloc, compaction).
✗ bool flags checked in a hot loop to decide whether work happens.
✗ enum + switch as runtime polymorphism in a hot loop.
✗ Linked lists / pointer trees in a hot path.
✗ Fixed structs kept as array-of-structs when every loop touches one field.
✗ Building every index up front instead of the hot ones.
✗ Freeing and reallocating per frame instead of arena-rewind / inactive reuse.
✗ Shared mutable accumulator written by all threads.
✗ Small per-thread data left unpadded on a shared cache line.
✗ Branching on random data in an inner loop.
✗ Searching when the answer is directly addressable.
✗ Emulating the real world exactly when a believable approximation is cheaper.
✗ Optimizing a hypothesis without a measurement.
✗ Assuming SIMD / SoA / branchless / non-temporal is always faster.
✗ Splitting code into tiny functions "for style" in the hot path.
✗ Hidden allocation inside helpers, iterators or virtual calls.
✗ Serving every subsystem's lifetime from the global allocator.
```

---

## 15. Review checklist (run before declaring code done)

```
LAYOUT
[ ] Does the hot loop touch only the data it needs?
[ ] SoA vs AoS chosen from the access pattern, not by habit.
[ ] Hot and cold data separated.
[ ] No pointers inside hot rows; ids/handles only.
[ ] One atomic value per cell; no nullable members.

CONTROL FLOW
[ ] No branches on random data in hot loops.
[ ] State is existence (tables/bitsets/sparse sets), not flags.
[ ] No enum-as-flow dispatch in hot paths (or justified if state flips often).
[ ] Unpredictable branches replaced by sorting/bucketing/predication.

MEMORY
[ ] Sequential or strided access; no pointer chasing.
[ ] restrict + ownership explicit enough for the compiler to vectorize.
[ ] Deletion is swap-remove when order is free.
[ ] Buffers/alignment regular enough for vectorization.

ALLOCATION
[ ] One allocator strategy per region; no stray malloc in hot code.
[ ] Stack usage bounded; large scratch in an arena/static buffer.
[ ] Handles carry generations where cross-table references live.

CONCURRENCY
[ ] One writer per datum; local accumulators + single reduce.
[ ] No false sharing; worker outputs kept separate.
[ ] Lifetime is inactive-reuse, not delete-while-referenced.

PROCESS
[ ] Before/after measurements exist (MU_SCOPE_TIMER or profiler).
[ ] Assembly of the hot 5% inspected.
[ ] Compiles warning-clean with -Wall -Wextra -Werror.
[ ] Missing primitive added to mylib, not reinvented locally.
```

---

## 16. Reference examples (illustrative, not compilable as-is)

### 16.1 The full shape of a system

```c
/* ------------------------------------------------------------------ *
 * Rendering: register on add/remove, do not walk a scene graph.       *
 * ------------------------------------------------------------------ */

enum { MAX_RENDERABLES = 1 << 16 };

typedef uint32_t RenderableId;

/* Packed per-renderable state consumed by the renderer, hot loop only. */
static float      positions[MAX_RENDERABLES][3];   /* SoA-friendly x,y,z */
static MeshId     meshes[MAX_RENDERABLES];
static MaterialId materials[MAX_RENDERABLES];
static uint32_t   flags[MAX_RENDERABLES];          /* bit 0 = opaque */
static uint32_t   renderable_count;

static RenderableId renderable_add(MeshId mesh, MaterialId mat, const float pos[3])
{
    RenderableId id = renderable_count++;
    MU_ASSERT(id < MAX_RENDERABLES);

    positions[id][0] = pos[0];
    positions[id][1] = pos[1];
    positions[id][2] = pos[2];
    meshes[id]       = mesh;
    materials[id]    = mat;
    flags[id]        = 1u;                          /* opaque by default */
    return id;
}

static void renderable_remove(RenderableId id)
{
    RenderableId last = --renderable_count;
    positions[id][0] = positions[last][0];
    positions[id][1] = positions[last][1];
    positions[id][2] = positions[last][2];
    meshes[id]       = meshes[last];
    materials[id]    = materials[last];
    flags[id]        = flags[last];
}

/* Stable sort by material id (counting sort, known range). */
static void sort_renderables(RenderableId *order, RenderableId *scratch)
{
    uint32_t bucket[256] = {0};
    for (uint32_t i = 0; i < renderable_count; ++i) ++bucket[materials[order[i]]];
    uint32_t sum = 0;
    for (uint32_t m = 0; m < 256u; ++m) {
        uint32_t n = bucket[m];
        bucket[m]  = sum;
        sum       += n;
    }
    for (uint32_t i = 0; i < renderable_count; ++i)
        scratch[bucket[materials[order[i]]]++] = order[i];
    memcpy(order, scratch, renderable_count * sizeof *order);
}
```

### 16.2 Existence-based damage system with bitset visibility

```c
enum { MAX_ENTITIES = 1 << 16, WORDS = (MAX_ENTITIES + 63) / 64 };

static float    health[MAX_ENTITIES];
static float    regen_delay_left[MAX_ENTITIES];
static uint64_t visible_bits[WORDS];
static uint64_t damaged_bits[WORDS];

static inline bool bit_test(const uint64_t *w, uint32_t i) { return (w[i >> 6] >> (i & 63)) & 1u; }
static inline void bit_set(uint64_t *w, uint32_t i)        { w[i >> 6] |=  UINT64_C(1) << (i & 63); }
static inline void bit_clear(uint64_t *w, uint32_t i)      { w[i >> 6] &= ~(UINT64_C(1) << (i & 63)); }

/* One pass: only iterate the damaged bits; one subtract per element. */
static void regen_update(float dt, float regen_rate)
{
    for (size_t wi = 0; wi < WORDS; ++wi) {
        uint64_t word = damaged_bits[wi];
        while (word) {
            uint32_t bit = (uint32_t)mu_trailing_zeroes_u64(word);
            uint32_t id  = (uint32_t)(wi * 64 + bit);
            word &= word - 1;                            /* clear lowest set bit */

            regen_delay_left[id] -= dt;
            if (regen_delay_left[id] > 0.0f) continue;

            health[id] = MU_MIN(100.0f, health[id] + dt * regen_rate);
            if (health[id] >= 100.0f)
                bit_clear(damaged_bits, id);            /* no longer needs work */
        }
    }
}

/* Visibility is a bitset, so culling output is word operations, not branches. */
static void cull(const Frustum *f, const float (*pos)[3])
{
    for (uint32_t i = 0; i < MAX_ENTITIES; ++i)
        if (in_frustum(f, pos[i])) bit_set(visible_bits, i);
}
```

### 16.3 Skeletal animation: parent indices + DFS order, no recursion

```c
/* Joints are stored in depth-first order, so parent[i] < i. */
typedef struct {
    uint16_t  parent[MAX_JOINTS];
    Matrix    local[MAX_JOINTS];
    Matrix    world[MAX_JOINTS];
    uint32_t  joint_count;
} Skeleton;

/* Recursion became a flat loop because the array encodes the traversal order. */
static void skeleton_local_to_world(Skeleton *s)
{
    s->world[0] = s->local[0];
    for (uint32_t i = 1; i < s->joint_count; ++i)
        s->world[i] = matrix_mul(s->world[s->parent[i]], s->local[i]);
}
```

Why it works: `parent[i] < i` guarantees `world[parent[i]]` is already computed.
The dependency is a **topological order baked into the array layout**. Indices
are 2 bytes instead of an 8-byte pointer, serialize cleanly, and need no pointer
fixups when the skeleton moves.

### 16.4 Handles + generations + arenas assembled

```c
typedef struct { uint32_t index; uint32_t generation; } Handle;

typedef struct {
    Particle *slots;        /* arena-backed pool of MAX_PARTICLES */
    uint32_t *generations;
    uint8_t  *live;
    uint32_t  capacity;
    uint32_t  live_count;
    uint32_t  free_head;
} ParticleStore;

static bool particle_alive(const ParticleStore *s, Handle h)
{
    return h.index < s->capacity
        && s->live[h.index]
        && s->generations[h.index] == h.generation;
}

static Particle *particle_get(ParticleStore *s, Handle h)
{
    if (!particle_alive(s, h)) return NULL;   /* boundary check, not inner-loop check */
    return &s->slots[h.index];
}
```

### 16.5 The transform signature convention

```c
/* read-only input, read/write state, write-only output, explicit count */
void movement_update(Vec3       * MU_RESTRICT positions,   /* rw  */
                     const Vec3 * MU_RESTRICT velocities,  /* r   */
                     size_t                    count,
                     float                     dt);
```

### 16.6 Loop fusion — carefully, and only after measuring

Merge passes over multiple streams when the same set of streams exists over a
range:

```
array1 ...............
array2 .......................................
array3 .........................

naive:
    for i in array1: update1
    for i in array2: update2
    for i in array3: update3

fused (ranges where the same streams exist):
    for i in [0, |a1|):        update1, update2, update3
    for i in [|a1|, |a2|):     update2, update3
```

**Measure it.** Fusion can hurt cache behaviour; the benchmark is the final
word. (This repo has a `demo/loop_fusion.c` benchmark for exactly this.)

---

## 17. Philosophy, condensed

```
Program = data + transformation.

Don't organize data because it belongs to the same noun;
organize it because it is processed together.

The player perceives the world, not the object graph — design
the representation around the relevant mental model.

The goal is not maximum simulation fidelity.
The goal is minimum necessary computation while preserving
the intended experience.

Don't build an object because the thing has a name.
Build the data required to represent the work the program
actually does.

Then measure.
```

> "The compiler can eliminate instructions. It cannot rescue a
> fundamentally poor data layout."

---

## 18. Industry & Pioneer Optimization Playbook (Distilled Reference)

Every major technique in this repository traces directly to battle-tested desktop systems engineering:

| Source / Codebase | Core Techniques Adopted Here & in `mylib` |
|---|---|
| **Mike Acton** *(Insomniac Games, CppCon 2014)* | Three Big Lies of OOP; Hot/Cold struct splitting (`7.07×`); cache-line utilization arithmetic; transforming arrays of homogeneous inputs; "where there is one, there are many." |
| **Richard Fabian** *('Data-Oriented Design')* | Existence-based processing (`121.72×` at 1% active); relational 1NF–BCNF table normalization; table-driven FSMs (evaluate-then-commit); hierarchical/collective LOD & JIT mementos. |
| **Casey Muratori** *('Clean Code, Horrible Performance', Handmade Hero)* | Eliminating vtables -> tagged switch -> split homogeneous SoA (`55.32×`); Zero-Is-Initialization (ZII); 64-bit VM reserve+commit (`mu_vm_arena`); double-mapped magic ring (`2.79×`); multi-accumulator ILP; repetition testing (`min` cycle wall). |
| **Agner Fog** *('Optimizing C++ / Microarchitecture')* | Breaking serial FMA chains via `Latency × Throughput = 8` accumulators (`20.20×`); Estrin's binary-tree polynomials (`1.74×`); Montgomery batched division (`1/(a*b)`); DSB µop-cache loop sizing; false register dependency traps (`popcnt`/`lzcnt`/`cvtsi2ss`). |
| **Fabian Giesen (ryg / RAD Game Tools)** | Branchless 64-bit unaligned bitstream refill (`4.78×`); Morton Z-order + dilated-integer stepping `(mx - 0x55555555) & 0x55555555` (`1.37×`); in-register 8×8 SIMD AoS↔SoA transpose (`2.94×`); STLF hazard prevention (`4.29×`); MESI cache-coherency discipline. |
| **Daniel Lemire** *(SIMD & Hashing Research)* | `fastmod` (`2.98×`) & `fastrange32` (`11.48×`); CTZ bitset decoding (`11.85×`); `vpshufb` 2-nibble 16B register LUT (`15.59×`); `simdjson` `PCLMULQDQ` quote prefix-XOR (`24.19×`); `simdprune` `vpermd` left-packing (`21.69×`); SWAR/AVX2 `vpmaddubsw` integer parsing (`5.03×`); Muła-Kurz-Lemire AVX2 popcount (`11.51×`); Split-Block Bloom (`3.43×`) & Xor/Binary Fuse filters. |
| **Mesa 3D** *(NIR Compiler & Gallium/RADV)* | `ralloc` & `linear_alloc` pass-scoped arenas (`6.12×`); `slab_child_pool` per-thread lockless object pools (`932.84×`); `fast_urem_by_const.h`; `u_vector` power-of-2 monotonic unmasked ring; `BITSET_FOREACH_SET`; dirty-bitmask state validation (`while (dirty)` + `tzcnt`); Amiga sentinel `exec_node`. |
| **Linux Kernel** *(mm / lib / include/linux)* | **Maple Tree** 256B cache-line RCU range B-tree & **XArray**; **SLUB** per-CPU lockless freelist (`932.84×`); **`seqlock`** writer-odd sequence counter (`27.44×` over `rwlock`); **`kfifo` / `io_uring`** power-of-2 monotonic batched rings (`127.12×`); **`hlist_node` `**pprev`** 8B bucket header (`1.59×`); `likely`/`unlikely` & `MADV_HUGEPAGE`. |
| **Naughty Dog** *(Christian Gyrling GDC 2015)* | M:N user-space fiber job system (`64 KB` fiber stacks, ~14-instruction x86-64 switch = **`8.58 ns`, 1,301.95× faster** than OS thread block/wake); atomic wait counters suspending fibers without blocking core threads; triple-buffered frame/GPU/CPU arenas. |
| **EA Frostbite** *(Battlefield / FrameGraph)* | Flat SoA AABB streams + prebound P-vertex SIMD frustum/Hi-Z culling outputting visibility bitsets (`16.13×`); FrameGraph transient memory aliasing (`30–50%` RAM saved); 64-bit packed sort keys (`shader|mat|depth`) + LSD radix sort (`9.53×`). |
| **Valve** *(Sergiy Migdalskiy GDC 2015 & Christer Ericson)* | Physics cache/TLB/SIMD discipline; explicit MSHR prefetch lookahead; quantized 6-byte `uint16_t` AABBs (`4×` cache density); flat CSR graphs (`32.72×`); Linear BVH. |
| **id Software** *(Quake -> id Tech 7)* | Modern `_mm256_rsqrt_ps` + 1 FMA NR (`6.70×` over `sqrtf`, replacing legacy GPR-stalling `0x5f3759df`); Morton swizzled virtual textures; render-world / game-world command snapshot decoupling; fiber jobs. |
| **Intel** *(ISPC, Embree, MoonRay, `intel-performance-skills`)* | Vector-width tiled `AoSoA8` / `AoSoA16` (`3.96×`); wide SIMD `BVH4`/`BVH8`; non-temporal `_mm256_stream_ps` (`1.63×` hot-L2 survival); AVX-512 `vpcompressd` & `vpopcntq` (`94.7 GB/s`); 10-step **Zipper** SIMD upconversion; Corsix `VPCLMULQDQ+CRC32C` (`64–97 GB/s`); `x86-simd-sort` (`3–8×`); `perf annotate` / `perf c2c --double-cl` / 1-vs-N dual-profile jumper workflows & CV thundering-herd fixes. |
| **LLVM** *(`llvm/ADT` Programmer's Manual)* | `SmallVector<T, N>` inline buffer (`mu_small_vec.h`, `1.59×`); `SmallPtrSet`; flat quadratic-probed `DenseMap`; hash-cached single-alloc `StringMap`; `PointerIntPair` low-bit pointer tagging; non-owning `ArrayRef`/`StringRef` (`mu_span.h`, `mu_str.h`); `BumpPtrAllocator`. |
| **GitHub High-Perf DS Research** | Abseil AVX2 **SwissTable** 1-byte H2 SIMD group probing (`2.40×` lookup); Martin Leitner-Ankerl **`unordered_dense`** (`60.19×` iteration); Khuong & Morin **Eytzinger** BFS + prefetch (`5.92×`); Sergey Slotin **16-ary AVX2 S+Tree** (`9.79×`); `wyhash` 128-bit multiply mixer; **4-Level `HierBitset`** (`318.68×`). |

---

## 19. Empirical Benchmark Proof Summary (`perf_test/`)

All additions are verified by standalone C11 benchmarks in `perf_test/` (`gcc -O3 -march=native -Wall -Wextra -Werror`, bit-exact checksums; full report in `perf_test/PERF_PROOFS.md`):

- **Single-Threaded Proofs (`make -C perf_test run-proofs` -> `results_proofs.csv`):**
  - `AoS (256B)` -> `Hot/Cold (24B)` (**7.07×**) -> `Aligned SoA + AVX2` (**11.50×**)
  - `AoS Packet Shading` -> `Intel ISPC Tiled AoSoA8 AVX2` (**3.96×**)
  - `Pointer-Chased Graph` -> `Contiguous Flat CSR (mu_csr)` (**32.72×**)
  - `Dense bool[1M] (1% active)` -> `CTZ Bitset` (**11.85×**) -> `Existence Table` (**121.72×**)
  - `STLF 2×u32->u64 Hazard` -> `Matched 64-bit Register Pack` (**4.29×**)
  - `Modulo Ring Split-Copy` -> `Double-Mapped Magic VM Ring (mu_vm)` (**2.79×**)
  - `Frame malloc/free Churn` -> `Frame Bump Arena Rewind` (**6.12×**)
  - `Vtable Polymorphism` -> `switch(type)` (**1.37×**) -> `Homogeneous SoA + AVX2` (**55.32×**)
  - `50/50 Random Branch` -> `Sorted` (**9.49×**) -> `Branchless Mask` (**13.96×**) -> `AVX2 Predicate` (**32.74×**)
  - `1 Scalar FMA Accum` -> `4 Scalar ILP` (**2.43×**) -> `8 AVX2 FMA Accumulators` (**20.20×**)
  - `x % d (divl)` -> `Lemire fastmod` (**2.98×**) -> `Lemire fastrange32` (**11.48×**)
  - `AoS Scalar Frustum Cull` -> `Frostbite SoA + Prebound P-Vertex AVX2` (**16.13×**)
  - `Scalar Multi-Char Scan` -> `Lemire AVX2 vpshufb 2-Nibble LUT (26.5 GB/s)` (**15.59×**)
  - `Scalar Quote State Loop` -> `simdjson PCLMULQDQ + AVX2 (25.6 GB/s)` (**24.19×**)
  - `Scalar Stream Filter` -> `AVX2 simdprune vpermd LUT` (**21.69×**) / `AVX-512 vpcompressd` (**20.64×**)
  - `Chained Hash Map` -> `AVX2 SwissTable Lookup` (**2.40×**) & `unordered_dense Iteration` (**60.19×**)
  - `16 MB std::lower_bound` -> `Plain CMOV` (`1.00×`!) -> `Eytzinger+Prefetch` (**5.92×**) -> `16-Ary AVX2 S+Tree` (**9.79×**)
  - `Classic 6-Probe Bloom` -> `AVX2 Split-Block Bloom` (**3.43×**)
  - `Flat 16.7M Bitset` -> `4-Level HierBitset` (**318.68×**)
  - `libc qsort()` -> `1-Scan Histogram + 4-Pass LSD Radix Sort` (**10.14×**)
  - `Naive i-j-k GEMM` -> `2-Line Loop-Interchanged i-k-j` (**20.07×**) -> `L1-Tiled + 4×16 AVX2 FMA Micro-Kernel` (**44.13×**, `72.1 GFLOP/s`)
  - `FNV-1a Byte Hash` -> `wyhash` (**20.75×**) -> `3-Lane CRC32C` (**32.28×**) -> `4-Lane AES-NI` (**76.59×**, `54.8 GB/s`)
  - `Protobuf LEB128 Varint` -> `Lemire SIMD StreamVByte (pshufb)` (**43.93×**, `5.64B ints/s`)
  - `Classic 8-Probe Bloom (2 MB)` -> `Lemire Static Xor8 Filter (1.23 MB)` (**3.42×** + `38%` smaller)
  - `Scalar Byte Bit-Reverse` -> `AVX2 vpshufb` (**42.84×**) -> `Hardware GFNI vgf2p8affineqb` (**93.20×**, `87.8 GB/s`)
  - `Table-Driven Scalar DFA` -> `4-Stream Associative pshufb SIMD DFA` (**8.84×**, `3.64 GB/s`)
  - `Sorted uint32[] Intersection` -> `Roaring Bitmap Hybrid (8KB Bitset + u16 Array)` (**209.45×**)

---

# PART II (SEPARATED) — MULTITHREADING, LOCK-FREE, KERNEL SYNC & USER-SPACE FIBERS

> **Why Part II is strictly separated:**
> Multithreading is a **throughput multiplier on already cache-clean, SIMD-vectorized data**, NEVER a substitute for single-core DOD. Parallelizing pointer-chased OOP structs simply saturates memory channels and triggers MESI coherence storms across cores. Apply Part I (`§1–§17`) first; enter Part II only when single-core execution is tight and work partitions cleanly across physical cores.
> Empirical proofs for Part II live separately in `perf_test/bench_multithreading_separate.c` (`make -C perf_test run-mt-proofs` -> `results_mt_proofs.csv`) and `tasks_not_threads/`.

## M.1 Don't Make Shared Memory Safe — Design So Sharing Isn't Required

```
SHARED MUTABLE STATE:            DATA-ORIENTED PARALLELISM:
T0 ─┐                            T0 → disjoint slice A (local regs/arena)
T1 ─┼→ same cache line → stall   T1 → disjoint slice B (local regs/arena)
T2 ─┘                            T2 → disjoint slice C (local regs/arena)
                                           ↓
                                    1 Stage-Boundary Reduce
```

**Core Rules:**
1. **One writer per cache line.** Partition output arrays into disjoint index ranges `[begin, end)` aligned to `64 B` (`16` floats) so two workers never write to the same cache line at boundary edges!
2. **Thread-local register/stack accumulators, one final combine (`71.76× faster` in `MT-1`):**
   ```c
   uint64_t local_sum = 0;
   for (size_t i = begin; i < end; ++i) local_sum += calc(i);
   atomic_fetch_add_explicit(&global_sum, local_sum, memory_order_relaxed);
   ```
3. **One worker per PHYSICAL core, NOT HyperThreads, for SIMD/memory-bound jobs:** Two HyperThreads on one physical core share the **exact same 48 KB L1d cache, store buffer, and AVX execution ports**, causing spin-wait starvation and cache thrashing on tight SIMD kernels (`tasks_not_threads/README.txt` Tip 2).

## M.2 MESI Cache Coherency & False Sharing Elimination

*(Sources: Fabian Giesen, Herb Sutter, Linux `__cacheline_aligned_in_smp`; **4.20× padded / 71.76× local-reduce speedup** in `bench_multithreading_separate.c` MT-1)*

A CPU core cannot modify a single byte without acquiring **Exclusive (`E`/`M`) ownership of the entire 64-byte cache line**, invalidating (`I`) that line in every other core's L1/L2 (~40–100 cycles per cross-core bounce!).
- **Pad every per-worker mutable struct, queue cursor, or status counter to `64 bytes` (`128 bytes` when Intel L2 spatial pair-line prefetcher is aggressive):**
  ```c
  typedef struct MU_ALIGN(64) {
      _Atomic uint64_t jobs_done;
      uint8_t _pad[64 - sizeof(_Atomic uint64_t)];
  } WorkerStats;
  ```
- **Never mix read-only configuration (`cap`, `mask`, `buffer_ptr`) on the same 64B cache line as a rapidly mutated `_Atomic head` or `lock` word!** Every atomic write to `head` invalidates the reader's cached `cap` and `mask`!
- **Advanced `perf c2c --double-cl` Access-Map Triage (`intel/linux-perf`):** Record via `perf c2c record -g -- app` and `perf c2c report --full-symbols --stdio --double-cl > c2c.txt` (`--double-cl` groups 128B hardware pair-line prefetcher units; prioritize top Pareto lines / `>5% Tot Hitm`). Check the byte **`Offset`** column first, watching **three traps**: **(1)** unpadded `slots[tid].done` shows the **same C field name** at **different cache-line offsets** (`0x00, 0x10, ...`) → **false sharing**; **(2)** two hot global counters on 1 line show **different offsets** (`0x00` vs `0x08`) hammered by all CPUs → **both true sharing (on each) and false sharing (between them)**; **(3)** offsets `0x00..0x3f` vs `0x40..0x7f` under `--double-cl` sit on adjacent 64B lines coupled by the L2 streamer!
- **Shared Statistics Counter vs Spinlock in `perf annotate` (`per-cpu-stats`):** `lock cmpxchg`/`lock xchg` in a loop = spinlock; `lock add`/`lock inc`/`lock xadd` with **flags/result ignored** (no branch after) = shared counter (`memory_order_relaxed` still bounces Exclusive `M` ownership at `100–300 ns`!). Fix in priority order: **(A) Local loop batching** into a stack `uint64_t` + 1 relaxed flush at exit (`<= 2` sites); **(B) `sched_getcpu() & 63` 64B-padded multi-counter `struct stats[64]`** (1 `sched_getcpu()` call + amortized padding; never call `flush_stats()` per request!); **(C) `__thread` TLS** fallback for `.so` libraries.

## M.3 C11 Memory Ordering Discipline, Hardware Instruction Cost, TTAS & MCS Locks

- **Cambridge C11-to-Hardware Mapping (Sewell; check subtle protocols with `herd7`/`litmus7`):**
  - `load(relaxed)` / `store(relaxed)` → `mov` (x86-TSO), `ldr` / `str` (ARMv8).
  - `load(acquire)` / `store(release)` → **plain `mov` on x86-64 TSO (`0` fence cost!)**; `ldar`/`ldapr` & `stlr` on ARMv8.
  - `store(seq_cst)` → `xchg` or `mov + mfence` on x86-64 (~15–30 cycles, drains the entire Store Buffer!); `stlr` (+ `dmb ish` on v8.0) on Arm. Reserve `seq_cst` strictly for Dekker-style Chase-Lev last-item steal races!
  - `fetch_add(acq_rel)` → `lock xadd` on x86-64; `ldaddal` on ARMv8.2-LSE (`ldxr/stlxr` loop on v8.0).
- **Spinlock Escalation (Never Bare TAS!):** A bare `while (!cmpxchg)` TAS loop acquires the line **exclusively on every failed spin** (`O(N^2)` traffic!). At minimum use **Try-First TTAS** (`if (cas != ok) do { while (load == LOCKED) _mm_pause(); } while (cas != ok);`) AND **pad the lock onto a separate 64B cache line from its protected payload**! At high core count, step up to **MCS queue locks** (Mellor-Crummey & Scott'91 — each waiter spins on its own local `qnode.locked` line) or Drepper's **3-state `futex`** (`0=unlocked, 1=locked, 2=contended`).

## M.4 Read-Mostly Escalation Ladder: Mutex (`osq_lock`) → RWLock → `seqlock` (`mu_sync.h`)

*(Sources: Linux Kernel `include/linux/seqlock.h`, `intel/performance-patterns`, `mu_sync.h`; **27.44× faster (`0.68 ns` vs `18.55 ns`)** in `bench_multithreading_separate.c` MT-2)*

1. **Diagnosing Read-Heavy Mutex Contention (`osq_lock`):** When `osq_lock` (Linux Optimistic Spin Queue), `__mutex_lock_slowpath`, or `__lll_lock_wait` dominates `perf report` on a critical section with `> 75%` reads, a mutex is serializing non-conflicting readers via `LOCK CMPXCHG`.
2. **Step 1 — `rwlock` (`1.8–14.6×` at moderate core counts):** Upgrading to `pthread_rwlock_t` / kernel `rwlock_t` (or sleepable `rw_semaphore`) lets readers overlap — **ALWAYS re-verify the condition (double-check) after dropping `rdlock` and acquiring `wrlock`!** (Avoid on `>50%`-write or tiny critical sections; watch writer starvation via `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`.)
3. **Step 2 — Why `pthread_rwlock_t` still hits a coherence wall → Linux `seqlock` (`27.44×`):** Calling `pthread_rwlock_rdlock()` still executes an **atomic RMW (`lock xadd`)** on `reader_count`, **dirtying the lock cache line into Modified (`M`) state on EVERY read**!
   - **Linux `seqlock` Pattern (for small POD snapshots: transforms, clock sync, bounding volumes):** Writer increments `seq` to **odd** (release), writes fields, increments `seq` to **even** (release); readers load `s1 = load_acquire(&seq)` (spin if `s1 & 1`), copy POD fields, fence acquire, and retry if `load_relaxed(&seq) != s1` — **ZERO reader writes**, keeping the line **MESI Shared (`S`)** across all cores (`1.42 cycles/read`)!

## M.5 Pointer-Swapped Configs & Tables: RCU / Epoch Reclamation (`mu_deferred_free.h`)

*(Sources: Linux Kernel RCU, Paul McKenney, `mu_deferred_free.h`)*

For larger read-mostly tables (asset registries, routing tables, spatial indexes, hot-reloaded shaders):
1. Readers load the current immutable table pointer via `atomic_load_explicit(&g_table, memory_order_acquire)` — **zero locks, zero reference-count RMWs**.
2. Writer builds a modified copy in the background, publishes via `atomic_exchange_explicit(&g_table, new_tbl, memory_order_acq_rel)`, and retires `old_tbl` to **epoch-based deferred reclamation (`mu_deferred_free.h`)** or frees it `2` frames later in a frame-based engine!

## M.6 Lock-Free Producer-Consumer Pipes: Shadow-Cached & Batched SPSC (`mu_spsc_queue.h`)

*(Sources: Linux `kfifo`, Erik Rigtorp `SPSCQueue`, Linux `io_uring`, DPDK `rte_ring`; **127.12× faster (`1.52 ns/msg` = 657.7M msgs/s vs `193.3 ns` mutex)** in `bench_multithreading_separate.c` MT-3)*

1. **Monotonic Unsigned Indices (`kfifo` / Mesa `u_vector` rule):** Store `uint32_t head, tail` monotonically (`0 .. UINT32_MAX`, wrapping naturally) and mask ONLY at array access (`buf[head & (cap - 1)]`). Full = `(tail - head == cap)`, Empty = `(tail == head)` — distinguishes full vs empty across all `cap` slots without wasting 1 slot!
2. **Shadow Index Caching (Rigtorp rule):** Put `head` and `tail` on **separate 64B cache lines**. Give the Producer a private `uint32_t head_shadow` on its `tail` line, reloading atomic `head` **ONLY when `tail - head_shadow == cap`**! Give the Consumer a private `tail_shadow`, reloading atomic `tail` **ONLY when `tail_shadow == head`**. Eliminates **99.9%+** of cross-core index coherence traffic!
3. **Batched Ring Commit (`io_uring` / DPDK rule):** Write `B` items (`e.g., 32–64`) into `buf[]` first, then publish **one single `atomic_store_release(&tail, tail + B)`**.

## M.7 Multi-Threaded Object Allocation: Linux SLUB / Mesa Per-Thread Slab (`mu_slab.h`)

*(Sources: Linux Kernel SLUB `mm/slub.c`, Mesa `src/util/slab.c` `slab_child_pool`, jemalloc/mimalloc, `mu_slab.h`; **932.84× faster (`0.20 ns` = 5.03B ops/s vs `185.4 ns` mutex pool)** in `bench_multithreading_separate.c` MT-4)*

Never guard a shared object freelist with a mutex! Give each worker thread:
- **Fast path (99.9%):** Thread-private singly-linked `local_free` list — **2 pointer moves, zero atomics, zero locks (`0.42 cycles/op`)**!
- **Cross-thread free path (Mesa / mimalloc pattern):** Each slab page has an `_Atomic FreeNode *remote_free` Treiber stack; foreign threads push freed blocks there via 1 CAS, and the owning worker sweeps the entire `remote_free` chain in **one `atomic_exchange(..., NULL)`** only when `local_free` empties!
- **Transient worker scratch:** Give every worker thread its own dedicated `mu_arena` (`worker_arena[worker_idx]`) rewound at frame end — **zero synchronization ever**.

## M.8 Parallel Filter / Stream Compaction (2-Pass Prefix Sum, Zero Contention)

```
Pass 1 (Parallel): Worker w counts survivors in slice w -> count[w] (local register!)
Sync 1 (Single):   Exclusive prefix sum: offset[w] = sum(count[0 .. w-1])
Pass 2 (Parallel): Worker w writes its survivors contiguously into out[offset[w] ..]
```
Zero atomic contention on `out_count`, deterministic output order, and 100% sequential cache-line writes!

## M.9 Work-Stealing Task Graphs (`mu_task_scheduler.h`) & Chase-Lev Deques

- Each worker owns a double-ended **Chase-Lev deque**: owner pushes/pops **LIFO** from the bottom (keeps just-spawned hot child tasks in L1/L2 cache with zero CAS in the common case); idle thieves steal **FIFO** from the top (steals the oldest, largest granularity sub-tree!).
- Choose `parallel_for` grain size so each task runs for **`>= 10–50 µs` (`>= 25,000` cycles)** (`e.g., grain = 1024..4096` entities) so scheduling overhead is `< 1%`.

## M.10 Naughty Dog M:N User-Space Fiber Job System

*(Source: Christian Gyrling GDC 2015 'Parallelizing the Naughty Dog Engine Using Fibers'; **1,301.95× faster (`8.58 ns` = 18 cycles vs `11,167 ns` OS kernel block/wake)** in `bench_multithreading_separate.c` MT-5)*

Why OS-thread blocking (`pthread_cond_wait` / `futex`) breaks nested task graphs: if Worker Thread 2 blocks waiting on a child job, either the OS context-switches threads (~10 µs + cache pollution) or you spin-execute recursive tasks on the same call stack (risking stack overflow and stalling parent continuations when children block).
**Naughty Dog Architecture:**
1. Spawn exactly **`N_physical_cores` pinned worker OS threads** (`pthread_setaffinity_np`). Worker threads **never block on mutexes during gameplay**.
2. Preallocate a fixed pool of **~128–160 `64 KiB` fiber stacks** (+ ~16–32 `512 KiB` stacks for deep I/O/middleware calls).
3. Every `KickJob(jobs, count, &counter)` sets an atomic wait counter. When `WaitForCounter(&counter, 0)` sees `counter > 0`, it pushes `{ current_fiber, &counter }` onto the wait list and executes a **14-instruction user-space x86-64 callee-saved register swap (`rsp, rbp, rbx, r12–r15`, `8.58 ns`)** into the next ready job fiber!
4. When the last child job decrements `counter` to `0`, the suspended parent fiber is pushed into the **high-priority ready queue** and resumed on any free core with its stack variables intact.
5. **Strict Fiber Rule:** Never hold an OS `pthread_mutex_t` across `WaitForCounter` (fibers migrate between OS threads, violating mutex thread-ownership!). Use spinlocks only for `< 20`-cycle queue operations with yields disabled.

## M.11 Scaling Laws (USL), Queueing Tails, NUMA First-Touch, OS/`resctrl` & Coordinated Omission

1. **Gunther's Universal Scalability Law (`USL`) & Kingman's Tail Formula:**
   - $C(N) = \frac{N}{1 + \alpha(N - 1) + \beta N(N - 1)}$: contention $\alpha$ flattens throughput, while pairwise cache-line coherency $\beta N(N - 1)$ makes throughput **drop** past $N_{\max} = \lfloor\sqrt{(1 - \alpha)/\beta}\rfloor$!
   - Kingman heavy-traffic wait: $E[W_q] \approx \frac{\rho}{1 - \rho}\frac{c_a^2 + c_s^2}{2}E[S]$ — tail latency explodes near $\rho \to 1$ and scales linearly with arrival/service jitter ($c_a^2, c_s^2$).
2. **NUMA First-Touch Page Placement & Controller Congestion:**
   - **Allocation $\ne$ placement:** `mmap`/`malloc` does not pick a NUMA node; the thread that **first writes** each page takes the minor page fault (`~670 ns` vs `4 ns` warm) and pins that physical frame to its local node! Always zero/initialize large buffers inside `parallel_for` with the exact same static slice partitioning as compute, and verify via `numastat` (`numa_hit` vs `numa_miss`) and BIOS node modes (Xeon `SNC2/4`, EPYC `NPS1/2/4`). Beware single-node memory-controller queue congestion (`Carrefour`, Dashti'13) and `kernel.numa_balancing` fault jitter.
3. **OS Kernel Boundary, `resctrl`/MPAM & Virtual-Memory Shootdowns:**
   - Syscalls pay a fixed ~`280 ns+` trap cost **plus** indirect L1/TLB/BTB pollution (`FlexSC`); keep clocks on `vDSO`, batch I/O via `io_uring` / poll-mode `SPDK`/`AF_XDP`, avoid container `cgroup v2` `cpu.max` throttling, isolate cores (`isolcpus`, `nohz_full`), and partition shared L3 ways & memory bandwidth via `/sys/fs/resctrl` (Intel RDT / AMD QoS / Arm MPAM — `Heracles`).
   - Never call `munmap` / `madvise(MADV_DONTNEED)` on hot paths: cross-core **TLB shootdown IPIs** freeze every thread in the process; pre-fault and reuse arenas (`mu_vm.h`), and trace kernel/IRQ jitter with Linux `osnoise`.
4. **Killer Microseconds & Gil Tene Coordinated Omission (`wrk2` / `HdrHistogram`):**
   - Stalls of `1–100 µs` exceed OoO ROB reach (~150 ns) yet are shorter than OS `futex` switches (~11 µs) — hide them with user-space fibers (`§M.10`).
   - Closed-loop load generators withhold requests during stalls, underreporting `p99` by **`1,000×+`** (`1,951×` in `12-coordinated-omission`)! Always measure latency open-loop from **intended `due_time`** and record `p50, p99, p99.9, max` in `HdrHistogram`.
5. **Core-Count Scaling Sweeps, Dual-Profile "Jumpers" & Condition-Variable Thundering Herds (`intel/intel-performance-skills`):**
   - **Bisecting Sweep & Dual-Profile Jumper Triage (`Flow D`):** Check `ldd bin` for threading libs, sweep `1, 2, nproc/4, nproc/2, nproc` (keeping throughput runs separate from `perf record`!), and flag the inflection point where marginal `Scaling_Factor = (Score(N) - Score(prev)) / (N - prev) / Score(1) < 0.80`. Compare top-15 `--no-children` functions at 1 core (`taskset -c 0 perf record -F 99 -g -- cmd` — omit `-a` or use `-C 0`!) vs N cores (or **N/2 vs N** if `> 7` functions jump from low-core noise). Flag **jumpers**: Rank rise `>= 3`, `%` ratio `>= 2×`, absolute gain `>= 3%`, or new entrant `>= 2%` (if inlined wrapper `<= 10` lines, fix the **caller**!).
   - **CV Thundering Herd vs Lock Contention (Invisible to `perf lock stat`!):** High `futex_wake` / `try_to_wake_up` / `__pthread_cond_broadcast` + normal `lock_stat` wait + sky-high `context-switches` (`FUTEX_WAKE val=INT_MAX`) means `notify_all()` is waking `N` waiters to serialize on 1 mutex (`~N × 5 µs = 800 µs` wasted at 160 cores!). Fix by wakeup topology: **(a)** wake exact `min(num_threads, num_jobs)` with early exit; **(b)** `O(sqrt(N))` **tree wakeup** (`K = sqrt(N)` helpers wake `N/K` peers in parallel); **(c)** **post-unlock `notify_one()` + baton hand-off** (unlock mutex *before* `notify_one()`, and consumer wakes next if `!queue.empty()`); or **(d)** **counting semaphore** (`sem_post`/`sem_wait`) / `std::latch` (zero mutex re-acquisition storm).

## M.12 Multithreading Review Checklist (Separate from Single-Core Checklist)

```
[ ] Single-core DOD layout + SIMD already optimized BEFORE parallelizing?
[ ] Workers pinned / sized to physical cores (not over-subscribed HyperThreads)?
[ ] Disjoint output slices aligned to 64B cache-line boundaries + parallel first-touch init?
[ ] Per-worker counters/cursors padded with MU_ALIGN(64) (verified via perf c2c --double-cl)?
[ ] Scaling walls triaged with 1-vs-N (or N/2-vs-N) jumper delta + no CV futex_wake herds?
[ ] Read-mostly shared POD uses Seqlock (0 reader RMWs), NOT mutex/pthread_rwlock_t?
[ ] Read-mostly tables use RCU pointer swap + frame/epoch deferred free?
[ ] Producer-consumer uses shadow-cached / batched SPSC (mu_spsc_queue)?
[ ] Multi-thread allocations use per-worker arenas or per-thread SLUB slabs (mu_slab), zero munmap TLB shootdowns?
[ ] Latency benchmarks use open-loop due_time + HdrHistogram (p99.9 + max)?
```

---

## 20. Where to go next

- `dodnotes.txt` — the full narrative notes with diagrams, formulas, hardware deep dives, quantitative models (`Part 21`), empirical scorecard (`Part 22`), and separated multithreading/NUMA/OS/tail-latency appendix (`Appendix M`).
- `perf_test/PERF_PROOFS.md` — runnable C11 empirical proof benchmarks & full speedup tables (`Suites 1–7`: `results_proofs.csv` and `results_mt_proofs.csv`).
- `.agents/skills/dod-performance/triggers.md` — **start here when you have a symptom**: symptom -> rule -> section index over this file, a from-profile table, a curated "worth attempting without PMU confirmation" list, and the measured dead ends not to retry.
- `.agents/skills/dod-performance/SKILL.md` — core single-threaded DOD, memory, quantitative models & high-perf data-structure procedure.
- `.agents/skills/text-buffer-dod/SKILL.md` — chunked text buffers (ropes, piece tables, gap buffers, segment arrays): chunk sizing, newline summaries, the boundary line-start invariant, differential-test flavours, and the long-line failure modes.
- `.agents/skills/simd-hardware-c/SKILL.md` — desktop x86-64 microarchitecture, SWAR, BMI2, PCLMULQDQ, AVX2 & AVX-512 kernel skill.
- `.agents/skills/game-engine-dod/SKILL.md` — end-to-end game/engine DOD skill (frame loop, ECS, culling, animation, Frostbite FrameGraph, batching).
- `.agents/skills/multithreading-dod/SKILL.md` — **strictly separated** multithreading, lock-free, NUMA/OS, Linux kernel sync & Naughty Dog fiber skill.
- Primary external reference paths: [`usamahz/cpu-performance-engineering`](https://github.com/usamahz/cpu-performance-engineering) (304 primary-source CPU performance papers/manuals, 14 reproducible C benchmarks, Seven-Field evidence standard) and [`intel/intel-performance-skills`](https://github.com/intel/intel-performance-skills) (structured `linux-perf` workflows, `perf annotate` assembly scans, Zipper SIMD upconversion, Corsix `VPCLMULQDQ+CRC32C`, runtime CPU dispatch, and HCC scaling patterns). Audited round 6: every pattern in Intel's catalog is already covered here, most in more depth, so what was adopted is their three-tier *routing architecture* (`triggers.md`) rather than their content.
- `docs/` — per-module `mu` documentation.

