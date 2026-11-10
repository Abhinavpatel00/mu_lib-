# `mu_shapes.h` — parametric shapes and mesh surgery

A single-header C99 replacement for [`par_shapes.h`](https://github.com/prideout/par),
built on `mu_math.h` instead of its own vector helpers, with the shapes and the
mesh operations that a `(u,v)` grid cannot give you.

```c
#include "mu/mu_shapes.h"          /* needs mu_macros.h, mu_math.h, mu_pcg.h */

mu_shape s = mu_shape_sphere(48, 24, 1.0f);   /* owns its arrays, returned by value */
if (s.npoints == 0) { /* out of memory; already cleaned up */ }
mu_shape_free(&s);
```

Build: it is a header. Nothing to link, no `#define MU_SHAPES_IMPLEMENTATION`,
no global state. Every generator and operation is `MU_INLINE`, so the usual
`MU_STATIC` / `MU_API` reach for single-TU or shared-library builds applies
exactly as it does for the rest of `mu/`.

## Migrated off `mu_math_legacy.h` (2026-11)

`mu_shapes.h` used to include `mu_math_legacy.h` — the struct-based
(`v.x`, return-by-value) front end kept for code that had not moved to
`mu_math.h` yet. That made `#include "mu.h"` fail outright: the umbrella pulls in
`mu_math.h` first, so the legacy `struct mu_vec3` collided with the array-based
`typedef float mu_vec3[3]`.

The header now builds on `mu_math.h` directly, in its own idiom:

```c
mu_vec3 d;                                     /* arrays, not structs        */
mu_vec3_sub(d, path[i + 1], path[i]);          /* output first               */
float l2 = mu_vec3_length2(d);                 /* scalars still return       */
mu_vec3 p0; mu_vec3_copy(p0, s->verts[0].p);   /* copies, not assignment     */
```

Three consequences worth knowing:

- **Callbacks return through an out-parameter.** `mu_shape_fn` is now
  `void (*)(mu_vec3 out, float u, float v, void *user)`, matching
  `mu_field_fn`-style callbacks elsewhere in `mu/`. Same for the internal
  `mu_shape__grid_pos` / `mu_shape__grid_cross` / `mu_lsystem__rotate`.
- **In-place algebra where the element is independent.** `mu_vec3_normalize(n, n)`
  or `mu_vec3_add(p, p, t)` is used deliberately: both read their inputs before
  writing the same index. Cross products through an alias are *not* safe and
  always go through a temporary.
- **`mu_ease_vec3` moved too** (`void mu_ease_vec3(mu_vec3 out, kind, a, b, t)`),
  because it was the last `mu_vec3` in `mu_easing.h` and the umbrella includes
  that header as well.

Verification that this is a port and not a rewrite:

| check | result |
|---|---|
| `perf_test/test_shapes.c` | **398 checks, 0 failures** (identical to the legacy build) |
| 26-shape geometry digest (every generator + transform/weld/normals, FNV over the vertex and index arrays) | **bit-identical** to the legacy build |
| `perf_test/bench_shapes.c` vs the legacy build, same machine | no change: tube 87.3 vs 87.7 µs, L-system 752 vs 752 µs, SDF sphere 9.43 vs 9.44 ms, Loop 2.481 vs 2.468 ms |
| `#include "mu.h"` | now compiles |

At the time of this port `mu_noise_math.h` and `mu_geometric_algebra.h` were
still on the legacy header; both moved over afterwards, so `mu_math_legacy.h` is
gone from the tree entirely. See [math.md](math.md) for that migration.

## What `par_shapes` does, and what this does instead

Same single-header style, same "return a mesh by value" feel, same OBJ export.
The differences are the point of the header:

| | `par_shapes.h` | `mu_shapes.h` |
| --- | --- | --- |
| index type | `uint16_t` by default | `MU_SHAPES_T` = `uint32_t`; nothing silently truncates at 65 536 points |
| epsilon | file-scope globals you set before calling (`PAR_SHAPES_EPSILON`) | every knob is a parameter, so two generators in one expression cannot interact |
| generators | `(u,v)` parametric grid, plus hardcoded tables for the 5 platonic solids | same, plus supershape, seashell, Möbius, Klein, lathe, RMF tube, spring, trefoil, extruded polygon, SDF→mesh, L-system |
| failure | generators return `NULL` | return an empty shape; `npoints == 0` after a failed reserve (OOM flag, cleaned up) |
| welding | sorts vertex pairs | spatial hash, one pass, 2 probes per axis-pair (see below), optional UV/normal awareness |
| topology | none | `mu_shape_stats` (boundary / non-manifold / inconsistent edges, Euler, genus), `mu_shape_validate` |
| subdivision | — | Loop, with edge tables |
| SDF | — | marching tetrahedra, watertight, orientation fixed by the sign of the volume |
| normals | `compute_welded_normals` rewrites the topology | seam-aware, `map`-based, topology untouched |

The remaining gaps are deliberate: `par_shapes_create_empty` is
`mu_shape_empty()` (it has to be — see the memory model), and the L-system
grammar is a *string rewriting* system rather than par's stack machine (below).

## Sections of the header

| § | Contents |
| --- | --- |
| 1 | lifecycle: `empty`, `free`, `reserve`, `add_vertex`, `add_triangle`, `add_quad`, `clone`, `flip`, `compact`, `remove_degenerate` |
| 2–3 | primitive builders and `mu_shape_parametric` (the `(u,v)` surface that every grid generator goes through, with pole collapse) |
| 4–5 | cube / rounded box, the five platonic solids |
| 6 | the shapes a `(u,v)` square cannot express: supershape, seashell, Möbius, Klein bottle |
| 7 | construction kit: `lathe`, `tube` (rotation-minimizing frames), `tube_radii`, `spring`, `trefoil_knot`, `extrude` (ear-clipped caps, helical twist) |
| 8 | mesh operations: `weld`, `unweld`, `compute_normals`, `aabb`, `area`, `volume`, `stats_compute`, `validate`, `loop_subdivide`, `transform`/`translate`/`rotate`/`scale`, `merge`, `export_obj` |
| 9 | signed distance fields: `mu_sdf_*` primitives and operators, `mu_shape_from_sdf` |
| 10 | L-systems: `mu_shape_lsystem` |

## The parts worth reading the code for

### Rotation-minimizing frames (`mu_shape_tube`)

Sweeping a circle along a path with a naive `up` vector spins the ribbon: the
normal flips as the path passes the reference axis. `mu_shape_tube` uses
double-reflection RMF (Wang et al., *Computation of rotation minimizing frames*,
2008): one reflection to transport the frame, one to remove the error, two cross
products per step. The gate checks a swept ribbon does not twist, and
`mu_shape_trefoil_knot` is a path whose torsion a naive frame gets wrong.

### Ear-clipped extrusion caps (`mu_shape_extrude`)

A twisted prism is a ring sweep: `segments = 1 + |twist| / (pi/32)`, capped at 32,
each ring an exact rotated copy of the polygon, bottom cap ear-clipped and the
top cap the same fan reversed. The side surface is ruled between rings, so the
area is `polygon_perimeter * height * (chord correction)`: a *deficit*, not an
error. It converges as the ring count grows — measured in the gate (twist 0 is
exact at 8.0000; twist 0.7 is 7.7568 against 8.0).

### SDF → mesh (`mu_shape_from_sdf`)

Marching tetrahedra, six tets per cell, no 256-case table and no ambiguous-face
cases. Crossings are computed once per grid edge and shared, so the output is
watertight by construction; the orientation of each triangle is decided locally
by the dot product of its normal with `p_out - p_in`, which removes any need to
reason about tet winding. The weld afterwards only has to catch
bit-identical crossings plus the slivers that appear when the surface passes
through a grid corner, and the result is flipped if the signed volume is
negative.

Cost, measured (`perf_test/bench_shapes.c`): a 32³ sphere bake is ~4.8 ms, a 24³
gyroid ~8.5 ms. Marching tets produces roughly 1.5–2x the triangles marching
*cubes* would for the same grid; that is the price of not having a case table,
and it is worth saying out loud before someone bakes a 200k-triangle level.

### Welding, and the 27-probe mistake

The weld is a spatial hash over cells of `2*eps`. Along one axis the eps ball
covers `[p-eps, p+eps]`, an interval of exactly one cell width, so it can
straddle at most two cells and those two are `base` and `base+1` where
`base = floor((p-eps)/w)`. Eight probes, not the textbook 27, with the same
answer — the candidate test is still the exact distance check, so this is not an
approximation.

The first version probed the full 3×3×3 neighbourhood, 27 random probes per
vertex into a table of several megabytes: 550 ns/vertex, i.e. 70 ms for a
132k-point sphere, and every one of them paid for it (`compute_normals` and
`stats` both call the map). Eight probes brought it to 130 ns/vertex. The
numbers are in [`perf_test/PERF_SHAPES.md`](../perf_test/PERF_SHAPES.md), next to
a frozen copy of the 27-probe version so the comparison is in one process.

### Topology: `mu_shape_stats` and `mu_shape_validate`

`stats_compute` reports point/triangle counts, unique positions after merging
coincident ones, degenerate triangles, boundary / non-manifold / inconsistently
wound edges, the Euler characteristic, the genus, area and signed volume.

Genus is only reported when it means something: watertight, consistently wound,
even Euler characteristic, `euler <= 2`. A Möbius strip and a Klein bottle are
non-orientable, so they get `genus == -1` and no volume claim — the gate asserts
that rather than pretending.

`validate` is the one call a test should make on any generated mesh: finite
positions, unit-ish normals, indices in range, no duplicate triangles, and no
degenerate triangles. Degenerate is *relative*: `area < 1e-12 * mean_area`, plus
an exact-zero test. An absolute epsilon rejects every small model; a relative one
that is too aggressive rejects honest bakes — a 24³ gyroid bottoms out at
2.8e-11 against a mean of 3.1e-3, a ratio of 9e-9.

There are two edge tables in the header on purpose. `mu_shape__edges_build` keeps
the per-edge payload (both opposite corners) that Loop subdivision needs and costs
24 bytes a slot; `mu_shape__edge_counts` keeps only (count, winding) in 2 bytes and
shares an 8-byte key array, which is what introspection needs. The gate compares
the two classifications on a closed mesh, an open mesh, a non-manifold mesh and an
inconsistently wound one, so they cannot drift apart.

### Loop subdivision, and an O(n²) that looked like a cache problem

`mu_shape_loop_subdivide` builds an edge table, computes edge points
(`3/8·(a+b) + 1/8·(c+d)`) and vertex points (`beta = 3/16` for valence 3, else
`3/(8n)`), then emits four triangles per triangle.

It used to store the key as `pair + 1` (so that `0` can mean "empty slot") and
then hash the *offset* key on lookup while hashing the *raw* key on insert. Not a
correctness bug — the linear probe still walks to the right slot — but every
lookup started in the wrong bucket and scanned until it found the right one.
Subdivision of a 20 480-triangle mesh took 305 ms instead of 0.8 ms, and the
cost grew quadratically with the mesh. Hashed on the raw key, level 5 of an
icosphere goes from 175.9 ms to 5.1 ms (34.6x); the probe counter in the same
build went from 16 081 678 to 3 840.

The lesson is in the gate as a comment, because it is not visible in the output:
a hash table that is *correct* can still be quadratic, and a profiler that only
shows "cache misses" hides it.

### L-systems (§10)

String rewriting with a 3D turtle, which is what
*The Algorithmic Beauty of Plants* actually describes and what `par_shapes`'s
stack-machine "lsystem" is not:

```c
static const char *const bush[] = { "X=F[+X][-X]FX", "F=FF" };
mu_lsystem_config c = { 0 };
c.axiom = "X"; c.iterations = 5; c.slices = 8;
c.radius = 0.02f; c.length = 0.2f; c.angle = 25.7f;
mu_shape tree = mu_shape_lsystem(&c, bush, 2);   /* 4992 points, 6752 triangles */
```

`F` draws a segment, `f` moves without drawing, `+ -` yaw, `& ^` pitch, `/ \` roll,
`|` turns around, `!` shrinks the radius, `[ ]` push/pop a branch. Any other
capital is a nonterminal: rewritten, never drawn. Several rules may share a
predecessor; with `seed == 0` the last one wins deterministically, with a seed one
of them is chosen uniformly from `mu_pcg32` — so a seed *reproduces* a tree, which
`par_shapes` cannot do because it calls `rand()`.

Two things to know before using it:

- A segment emits one ring, and consecutive segments share rings, so each tube run
  is manifold. A branch (`[`) starts a new run at the same position: runs meet
  visually, not topologically. Each run is manifold; the welded union of a
  bracketed grammar is not, and `mu_shape_stats` will say so.
- The result is an **open** surface (a plant has ends), so genus and volume are
  not meaningful. The gate asserts manifoldness per run, consistent winding, open
  ends and no degenerate triangles instead.

One real bug lived here: the turtle's up vector drifts toward its head after a
few hundred rotations, and `mu_vec3_normalize` guards its input (it returns zero
for a short vector). A projection that was "too short for the guard but not short
enough to trip my own epsilon test" collapsed the ring radius to zero and emitted
zero-area quads. `mu_lsystem__basis` normalises by hand and falls back to a cross
product instead.

## Honest limits

- Not an editor, not a half-edge structure, no CSG tree, no UV unwrapper, no
  decimation, no manifold arithmetic. `mu_shape_merge` concatenates; it does not
  boolean.
- Marching tetrahedra: more triangles than marching cubes, and thin features
  thinner than a cell vanish. That is aliasing, not a bug in the bake.
- The tube's ring count is driven by the path, not by the screen; a 256-point
  path with 12 sides is 6144 triangles before you have decided whether you wanted
  that many.
- `mu_shape_loop_subdivide` moves the surface *inward* (a Loop limit surface on a
  unit sphere is ~65% of the radius at level 2). The gate asserts the topological
  invariants and that the limit stays inside the sphere, not the radius.
- The platonic solids are 4–64x slower than `par_shapes`'s hardcoded tables
  (2.1 µs vs 33 ns for a dodecahedron) because they are *computed* — the dual of
  an icosahedron, with angular sorting around each vertex — instead of memcpy'd.
  In absolute terms it is 2 µs. That is a loss, and it is in the table.
