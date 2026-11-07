# `mu_shapes.h` performance — generators, bakes, and two algorithmic fixes

Everything here is measured, not asserted. The interesting numbers are not the
"how fast is a sphere" ones (a sphere is 20 µs either way); they are the two
places where the code was *algorithmically* wrong and looked like a cache
problem, and the honest losses against `par_shapes.h`.

## Method (Seven-Field Rule, AGENTS.md 2.6)

| field | value |
| --- | --- |
| CPU | Intel Xeon @ 2.60 GHz (Skylake-SP class), 2 vCPU shared guest, 48 KiB L1d, 1 MiB L2 |
| cores / affinity | 1 core, `taskset -c 0`; the box is a shared 2-vCPU guest, so *every* claim below is a ratio measured in the same process |
| frequency / DVFS | not pinned; the guest is at the mercy of a neighbour. This is why no absolute number here is quoted to better than 2 significant figures, and why the A/B pairs are in one binary |
| compiler | gcc 14.2.0, `-O3 -march=native -std=c11 -Wall -Wextra -Werror` |
| workload | listed per row with its point/triangle count; the surgery rows use one 130 817-point, 261 120-triangle UV sphere (4.2 MB of vertices, 3.1 MB of indices — several times L2) |
| baseline | `par_shapes.h` (the library being replaced) for the generator rows; the frozen pre-fix copies (`ref.*`) for the two fix rows |
| method | `mu/mu_bench.h`: `CLOCK_MONOTONIC_RAW`, operand barrier through a `volatile` sink, auto-sized batches to a 20 ms target, 21 samples per run, **median** reported, `CV` and a 99% CI per row; three independent runs per table; the par comparison runs ABBA (par, mu, mu, par) and takes the median of the per-run medians, then a **geometric mean** over cases |

A row marked `UNSTABLE` by the harness (CV above the cutoff) still has its median
reported, and the CV column is there so it can be discounted. Nothing is quoted
here from an unstable row without saying so.

## 1. `mu_shapes.h` vs `par_shapes.h`, same tessellation (paired, 3 ABBA rounds)

`make -C perf_test shapes-par` — `perf_test/shapes_vs_par.sh`, 21 samples × 3
rounds. `par/mu > 1` means mu is faster.

| case | par | mu | par/mu | unit | points (par / mu) | triangles (both) |
| --- | --- | --- | --- | --- | --- | --- |
| cone 64×32 | 315.65 | 36.50 | **8.65x** | µs | 2145 / 2145 | 4096 |
| cylinder 64×32 | 269.01 | 20.35 | **13.22x** | µs | 2145 / 2147 | 4096 / 4224 |
| torus 128×64 | 1848 | 90 | **20.64x** | µs | 8385 / 8385 | 16384 |
| sphere 64×32 | 297.63 | 21.32 | **13.96x** | µs | 2145 / 2017 | 3968 |
| sphere 64×32 + weld | 512.95 | 128.63 | **3.99x** | µs | 1986 / 1986 | 3968 |
| subdivided sphere 4 (icosphere level 4) | 2479 | 466 | **5.32x** | µs | 2562 / 2562 | 5120 |
| disk 64 | 1.087 | 0.641 | **1.71x** | µs | 65 / 65 | 64 |
| icosahedron | 26.9 | 73.1 | **0.37x** | ns | 12 / 12 | 20 |
| cube | 29.5 | 120.5 | **0.24x** | ns | 8 / 24 | 12 |
| dodecahedron | 33.0 | 2120 | **0.02x** | ns→µs | 20 / 20 | 36 |

**Geometric mean of `par/mu` over the 10 cases: 2.15x.**

Reading the loss rows honestly:

- `par_shapes` returns a *hardcoded table* for the platonic solids and the cube —
  33 ns is a `malloc` and a `memcpy` of 12–20 vertices, and it does no normal or
  seam work. `mu_shape_*` *computes* them: the dodecahedron is the dual of an
  icosahedron, which means 20 face centroids plus an angular sort around each of
  the 12 vertices (60 `atan2f` calls). 2.1 µs.
- In absolute terms all three losses are ≤ 2.1 µs, next to a 20 µs sphere and a
  4.5 ms SDF bake. They are losses and they are listed, but they are not the
  numbers that decide whether a frame fits.
- Point counts mostly match (par welds the sphere to 2145, our UV wall collapses
  its poles and welds the seam to 2017; par's cube is 8 points where ours is 24,
  because ours is already split for per-face normals). The ratio is per *shape*,
  not per vertex; the ns/point column below is the per-vertex view.

## 2. What generation and surgery cost, absolutely (3 runs, median of medians)

`make -C perf_test shapes-bench`

| case | points | triangles | median | per point | per triangle |
| --- | --- | --- | --- | --- | --- |
| `gen.uv_sphere_64x32` | 2 017 | 3 968 | 20.8 µs | 10.3 ns | 5.2 ns |
| `gen.uv_sphere_256x128` | 32 641 | 65 024 | 349.7 µs | 10.7 ns | 5.4 ns |
| `gen.icosphere_level4` | 2 562 | 5 120 | 460.9 µs | 179.9 ns | 90.0 ns |
| `gen.icosphere_level5` | 10 242 | 20 480 | 2.15 ms | 210.0 ns | 105.0 ns |
| `gen.supershape_48x24` | 1 223 | 2 208 | 98.6 µs | 80.7 ns | 44.7 ns |
| `gen.torus_128x64` | 8 385 | 16 384 | 90.3 µs | 10.8 ns | 5.5 ns |
| `gen.tube_256x12` | 3 074 | 6 144 | 54.5 µs | 17.7 ns | 8.9 ns |
| `gen.extrude_star_64` (twist 0) | 128 | 252 | 6.2 µs | 48.5 ns | 24.6 ns |
| `gen.lsystem_bush5` | 4 992 | 6 752 | 362.6 µs | 72.6 ns | 53.7 ns |
| `gen.platonic_dodecahedron` | 20 | 36 | 2.1 µs | 105.7 ns | 58.7 ns |
| `sdf.sphere_res32` | 7 406 | 14 808 | 4.49 ms | 606.8 ns | 303.5 ns |
| `sdf.gyroid_res24` | 16 222 | 31 201 | 8.50 ms | 523.9 ns | 272.4 ns |

An icosphere costs 90 ns/triangle against a UV sphere wall's 5.2 ns because it is
*built by Loop subdivision*, which allocates an edge table and rewrites every
vertex — not because the triangle loop is slow. If you only need a sphere, the UV
wall is 20x cheaper; if you need uniform triangles, pay it.

The L-system at 54 ns/triangle is the rewrite pass plus a ring per segment plus
the seam-aware normal pass; `F=FF` makes the segment count double per iteration,
so this row is the *grammar's* cost as much as the library's.

## 3. The two algorithmic fixes (same process, paired, 21 samples × 3 runs)

The pre-fix algorithms are kept in `bench_shapes.c` as `ref.*` rows — frozen
copies, not API — so the two versions run in the same binary, on the same mesh,
in the same conditions.

### 3.1 Subdivision: the hash of an offset key (`ref.subdivide_level4_offsetkey`)

| row | median | CV | note |
| --- | --- | --- | --- |
| `ref.subdivide_level4_offsetkey` | 79.71 ms | 5.4% | pre-fix: lookup hashes `key+1` while the table stores `key+1` |
| `op.loop_subdivide_icosphere4` | 1.258 ms | 5.7% | current: lookup hashes the raw key |

**63.4x**, and the current row is doing *more* work than the reference (it also
recomputes normals at the end), so 63x is conservative.

The structural evidence, which does not depend on the machine at all: the probe
counter in the instrumented build went from **16 081 678 probes** for 1 280
triangles (12 564 per triangle) to **3 840** (3 per triangle) — i.e. the loop was
`O(n²)` while printing correct meshes. Same bug, measured raw: an icosphere at
level 5 took 175.9 ms, and 5.1 ms after the one-line fix (34.6x), before any of
the other work in this table.

### 3.2 Weld: 27 probes per vertex vs 8 (`ref.weld_27probe`)

Both rows weld the same 130 817-point UV sphere with `eps = 1e-4`, on a
preallocated map, so the ratio measures the neighbourhood search and nothing else.

| row | median | CV | per vertex |
| --- | --- | --- | --- |
| `ref.weld_27probe` | 31.81 ms | 9.7% | 243 ns |
| `op.weld_map_512x256` | 11.55 ms | 10.1% | 88 ns |

**2.75x.** With cells `2*eps` wide the eps ball spans exactly one cell width per
axis, so `floor((p-eps)/w)` and `floor((p+eps)/w)` differ by at most one and eight
probes cover the ball exactly: 27 → 8 is 3.4x fewer random accesses into a
multi-megabyte table, and each of them was a cache miss.

## 4. Surgery on a 130 817-point / 261 120-triangle sphere

| case | median | note |
| --- | --- | --- |
| `op.clone_sphere_512x256` | 477 µs | 7.3 MB copied, so ~15 GB/s — memcpy-bound, as it should be |
| `op.weld_uv_sphere_512x256` | 13.1 ms | **includes the clone**; subtract 477 µs → ~12.6 ms of weld |
| `op.weld_map_512x256` | 11.6 ms | the weld proper, 88 ns/point |
| `op.compute_normals_sphere_512x256` | 13.2 ms | dominated by the weld map it needs at the start |
| `op.stats_sphere_512x256` | 36.5 ms | weld map + count-only edge table + area + volume |
| `op.validate_sphere_512x256` | 3.25 ms | 12 ns/point: two passes and an AABB, no hashing |

`stats` is the slow one, and it is honest to say why: 11.6 ms of it is the weld
map, and the rest is three hash probes per triangle into a 3n-slot key array
(8 MB at this size) plus a per-slot payload write that misses. The count-only
edge table already took it from 104 ms to 82 ms (the table used to be `6n` slots
of 24-byte entries, i.e. 33 MB, mostly zeroed and mostly scanned); an SoA
key/payload split is the next step and it is not done.

## 5. What this document does not claim

- No claim that `mu_shapes` is "faster than `par_shapes`" as a headline. The two
  libraries do not produce the same output (par does not weld, does not compute
  seam-aware normals, has no topology pass), and the geometric mean of 2.15x
  mixes 20x wins on parametric surfaces with 0.02x losses on a 36-triangle
  dodecahedron. The per-case table is the claim.
- No claim about the SDF bake vs anything: `par_shapes` has no SDF path, and
  marching tetrahedra has no apples-to-apples peer here.
- No `perf stat` / TMA breakdown yet. On a 2-vCPU shared guest the counters are
  multiplexed and the numbers would not be trustworthy; the ratios above are, and
  the two structural counters (probes per triangle, probes per vertex) are exact.
- Nothing on the `mu_noise_math` / WFC paths is re-measured here; this document is
  only about `mu/mu_shapes.h`.
