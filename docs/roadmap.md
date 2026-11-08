# What a game still needs from `mu`

A survey done the way the rest of this repo is done: **a gap is only a gap if
`grep` says the code is not there, a game needs it every day, and there is a
gate that could prove the result.** This is the list I would work down if the
target is shipping a game rather than growing a library.

Status of the evidence: `84` headers, `33` docs, all gates green. What follows
is measured against *that* tree, not against a general idea of a game engine.

---

## 1. The gap table

| game subsystem | closest thing in the tree | what is actually missing | verdict |
| --- | --- | --- | --- |
| **save / load / replay** | `mu_bitserial.h` (bit writer + varints), `mu_fast_codec.h` | no container, no versioning, no migration, no checksums, no atomic file path, no record/replay loop. `grep save\|replay mu_roguelike.h` → nothing: the roguelite cannot be saved | **gap** |
| **input** | nothing (`grep -l input` → none) | action maps, deadzones, device merge, rebinding, **buffering windows**, chords, hold/tap, tick-quantised sampling for deterministic replay | **gap** |
| **camera** | `mu_math.h` has `mu_frustum`, `mu_frustum_test_sphere`, `mu_frustum_cull_spheres_compact` | no camera at all: no follow, deadzone, lookahead, shake, bounds, screen↔world; `grep -in camera mu/*.h` → nothing | **gap** |
| **geometry queries** | `mu_shape_aabb()` returns a lo/hi pair; frustum spheres | no AABB *type*, no ray/segment, no sweep, no closest-point, no ray-sphere/capsule test. `grep -E "mu_ray\|intersect\|sweep" mu_math.h` → nothing | **gap** |
| **sequencing** | `mu_easing.h` (33 curves, velocities, cubic-bezier, steps) | curve *math* only. Nothing schedules anything: no tracks, keyframes, markers, loops, ping-pong, sub-tick events | **gap** |
| **audio** | nothing | no mixer, buses, gain ramps, voice pool, panning, sample-accurate scheduling | **gap (large)** |
| **animation** | `mu_math` quaternion `slerp`/`nlerp`, 3×4 skinning matrices | no clips, channels, blend spaces, state machine, additive layers, root motion, event markers | **gap (large)** |
| **text / fonts** | `mu_text_editor.h`, `mu_loom.h` (editing, not rendering) | no glyph atlas, no layout, no kerning, no text shaping | gap |
| **assets** | `mu_intern.h` (string interning), `mu_id_pool.h`, `mu_dag.h` | no handle registry, refcounting, async load, hot reload; also no file IO header at all | gap (blocked by IO) |
| **netcode / rollback** | `mu_bitserial.h`, `mu_spsc_queue.h`, `mu_sync.h`, `mu_fixed.h` | no snapshot ring, no input history, no rollback loop, no delta encoding against a baseline | gap (large) |
| **particles** | `mu_noise_math.h`, `mu_pcg.h` | no pools, curves, sorting, deterministic emission | gap |
| **tilemap** | `mu_roguelike.h` grids (private to it) | no general tile layer, autotiling, chunk streaming | gap (small) |
| **behaviour** | `mu_toposort.h`, `mu_vm.h` | no FSM, behaviour tree, utility scoring, steering | gap |
| **profiling** | `mu_perf.h`, `mu_profile.h`, `mu_stats.h`, `mu_bench.h` | already good: scopes, histograms/p99, benchmark harness | fine |
| **entities / hierarchy** | `mu_ecs.h` (generations, SoA pools, transform store with parent/child) | already good | fine |
| **paths / grids / FOV** | `mu_roguelike.h`: A*, Dijkstra, FOV, schedulers, WFC in `mu_wfc.h` | already good | fine |
| **randomness** | `mu_pcg.h`, `mu_rand.h`, `mu_hash_table.h` | deterministic, seedable, jumpable | fine |
| **determinism primitives** | `mu_fixed.h` (integer trig), `mu_time.h` (steady clock) | the *substrate* for replay exists; the *system* does not | fine as substrate |

The pattern: **the library is strong on data structures, math and tooling, and
thin exactly where a game spends its days** — saving, input, camera, the little
sequencer that drives everything, audio and animation.

---

## 2. The five I would build, in order

### 1. `mu_save.h` — versioned chunks, atomic files, replay

*Gap:* `mu_bitserial.h` writes bits; nothing above it knows what a *file* is.
The roguelite has 13 363 assertions of gameplay and no way to persist a run.

*Shape:*

```c
mu_save_writer w; mu_save_begin(&w, buf, len, MAGIC, 3 /* version */);
mu_save_chunk_begin(&w, MU_SAVE_MAP, 1);
    mu_save_u32(&w, w_grid); mu_save_u32(&w, h_grid);
    mu_save_bytes(&w, tiles, w_grid * h_grid);
mu_save_chunk_end(&w);
mu_save_commit(&w);              /* directory, CRC32C, footer */

mu_save_reader r;                /* tolerant: unknown chunk ids are SKIPPED,  */
                                 /* which is what makes old saves load in a     */
                                 /* new binary at all                           */
if (mu_save_open(&r, buf, len, MAGIC) != MU_SAVE_OK) { /* clean failure */ }
while (mu_save_next(&r)) { ... }
```

*Hard parts worth doing properly:* a chunk directory so a reader can skip
data it does not understand (this is the whole reason old saves keep working);
per-chunk CRC so a corrupt region is reported instead of decoded; **migration
as a chain** (`v3 reader` upgrades a v1 chunk by running v1→v2→v3, each step
gated separately); and an atomic write (temp file → `fsync` → `rename`) so a
crash mid-save cannot eat the previous save, plus an alternating A/B pair so
even the rename cannot.

*Gate (and this one is objectively testable in a way most headers are not):*
round-trip every chunk type; **truncate the save at every byte offset and
require a clean `MU_SAVE_ERR_TRUNCATED`, never a crash or a half-loaded
state**; flip every bit in the payload and require detection; run the
migration chain from each historical version; and a property test that a save
written by the *last* version and read by the *current* one produces identical
state.

*Bench:* bytes/second for write and read against a naive `fwrite`-per-field
baseline, plus a comparison of "CRC32C with SSE4.2" vs the table version (the
library's first hardware-accelerated checksum, so it needs a scalar fallback
that matches bit for bit).

### 2. `mu_input.h` — actions, buffering, chords

*Gap:* nothing. Every game writes this badly, and the parts that make a game
*feel* good (buffer windows, coyote time, chords) are exactly the parts that
get left out.

*Shape:* a device state (raw), an action map (binding + deadzone per action),
and a **per-tick sampler** producing an immutable `mu_input_frame` that the
game reads for the whole tick — which is also what makes replay possible.

```c
mu_input_map map; mu_input_map_init(&map);
mu_input_bind_action(&map, ACT_JUMP, MU_INPUT_KEY_SPACE);
mu_input_bind_pad_axis(&map, ACT_MOVE_X, 0, MU_INPUT_AXIS_LEFT_X, MU_INPUT_DZ_RADIAL);
mu_input_frame f; mu_input_sample(&map, &devices, tick_dt, &f);
if (mu_input_pressed(&f, ACT_JUMP)) ...
if (mu_input_buffered(&f, ACT_JUMP, 0.12f)) ...   /* pressed within 120 ms  */
```

*Hard parts:* deadzone shapes worth having (radial, axial, and the sloped
`min + (1-min)*t^2` that keeps slow movement controllable); buffer windows and
their expiry in **ticks, not seconds**, so it replays; chord detection with a
resolution window; and *press/release ordering* within a tick — the "was down,
is down, went up and down again" case that naive `is_down` sampling loses.

*Gate:* synthetic device event streams → assert the sampled frame invariants
(pressed ⊆ down, edge counts, buffer expiry exactly at the tick boundary);
a **replay test**: record a raw event stream with tick timestamps, replay it
through the sampler, require bit-identical `mu_input_frame` sequences.

*Bench:* per-tick sampling cost vs per-device naive polling, and the buffered
query cost (should be O(1) per action, not a scan over history).

### 3. `mu_camera.h` + geometry queries in `mu_math.h` (or `mu_geom.h`)

*Gap:* `mu_math` culls spheres against a frustum but cannot answer "what did
the player click", "is this box inside", "sweep this AABB". There is no
camera, so every game writes its own follow/shake and gets the frame-rate
dependence wrong.

*Camera:* deadzone (a rectangle/ellipse the target can move inside without the
camera moving), velocity **lookahead** with a cap, exact critically-damped
smoothing (the closed-form discrete spring — frame-rate independent, unlike
`x += (target-x)*k*dt`), trauma-based shake (`trauma^2`, decaying, driven by
`mu_noise` so it is deterministic and replayable), bounds clamping, zoom, and
`mu_camera_view_proj()` feeding `mu_frustum_from_view_proj`.

*Geometry:* `mu_aabb`, `mu_ray`, `mu_sphere`, `mu_capsule`; slab ray-AABB
written so axis-parallel and zero-direction rays cannot produce NaN; ray-sphere
and swept-AABB (conservative advancement) for bullets and moving platforms;
closest-point segment-segment for capsule-vs-capsule; `mu_aabb_from_points`.

*Gate:* frame-rate independence — the same target trajectory sampled at 30,
60, 144 and 1000 Hz must produce camera positions within 0.1 % (this is where
`k*dt` smoothing fails and the closed form does not); "the camera never shows
outside the bounds" as an invariant over randomised trajectories; and for
geometry, 10⁶ randomised queries against a brute-force reference, with an
explicit "no NaN for any input" property that includes degenerate rays.

*Bench:* camera update per frame (trivial), and — the interesting one —
10⁶ ray-AABB queries, branchless slab vs the early-out version, to see whether
the early-out actually pays on mixed hit/miss data.

### 4. `mu_timeline.h` — a deterministic sequencer

*Gap:* `mu_easing.h` is curve math with no notion of *when*. Cutscenes, UI
transitions, tutorial prompts, screen shake curves, spawn schedules — all of it
is "tracks of values over time with events".

*Shape:* tracks (float/vec3/colour/callback), keyframes carrying the easing
curve per segment, loop / ping-pong / hold, and **marker events emitted with
sub-tick accuracy** so a 60 Hz tick can still fire something at 1.3742 s.

*Hard part worth doing properly:* the update must cost O(active tracks), not
O(all tracks), and it must be exact under a variable `dt` accumulated in a
fixed-step fashion — otherwise "the cutscene is 0.3 s longer on a slow frame".

*Gate:* exactness — a timeline stepped with a random `dt` sequence must agree
with a single large step at every marker time (within one tick), and a
recorded timeline play must replay identically; event ordering (multiple
markers inside one step fire in time order, once each).

*Bench:* O(active) update vs a naive full scan over 10 000 tracks.

### 5. `mu_audio.h` — mixer and voices

*Gap:* nothing at all. This is the largest missing subsystem and the one I
would defer until the four above are in, because it needs a device layer that
the library should not own (it can mix into a buffer and let the host feed the
OS).

*Scope I would take:* buses (a tiny graph, gains in dB or linear), gain ramps
that are **discontinuity-free** (no clicks: every gain change is a linear ramp
over at least one buffer), voice pool with priority-based stealing and a
fade-out instead of a hard cut, sample-accurate start/stop scheduling, spatial
panning and distance attenuation, and a deterministic integer mix path so
audio can be regression-tested.

*Gate:* no-clipping and no-discontinuity invariants over randomised automation
(enormous value, nobody tests it); ramp continuity: `|x[n] - x[n-1]|` bounded
across every voice start/stop; and a null test — silence in, exact silence out.

*Bench:* samples/second vs a naive per-sample virtual-call mixer; the
"denormal + voice count" sweep that shows where the per-voice cost lands.

---

## 3. Smaller ones (half a day each, same standards)

| header | what | why now |
| --- | --- | --- |
| `mu_particles.h` | SoA pool, `mu_pcg` per-particle streams, curves via `mu_easing`, stable sort by depth | deterministic, replayable particles; no engine dependency |
| `mu_atlas.h` | MaxRects/shelf packing for sprites and glyphs + a simple advance/kerning layout | fonts are the last "every game needs it" gap after audio |
| `mu_grid.h` | general tile layer (out of `mu_roguelike`), autotile rules, chunk streaming | the roguelite already has half of it privately |
| `mu_rollback.h` | snapshot ring over `mu_bitserial`, input history, `rollback(n)` — sits on `mu_fixed`/`mu_pcg` | the determinism substrate is ready; this is the payoff |
| `mu_behavior.h` | FSM + behaviour tree + utility scoring, on `mu_toposort` | gameplay AI is currently hand-rolled per project |
| `mu_files.h` | path join/normalise, atomic write, mmap read, watch-for-change | needed by `mu_save` and by hot reload; I declined it once, and the asset story is the reason to revisit |

## 4. What I would not build

* **A renderer.** Backend-specific, and the library's value is the data it
  hands to one (draw lists, batches, camera matrices).
* **A physics engine** beyond the query primitives above. A correct solver is
  its own project; ray/AABB/sweep queries are what 90 % of 2D games need.
* **A windowing/OS layer.** `mu_input` takes a device state struct; whoever
  owns the window fills it. That boundary is what keeps this testable.
* **An asset *importer*.** Formats churn; a handle registry with hot reload is
  worth having, a glTF loader is not this library's job.

## 5. Recommended order for this repo specifically

The roguelite is the only "game" in the tree, and it has zero persistence and a
hand-rolled input/loop story. So:

1. **`mu_save.h`** — unblocks the roguelite immediately, and the
   corruption/truncation gate is the kind of thing that gets *harder* to add
   later, once save formats exist in the wild.
2. **`mu_input.h`** — a turn-based roguelite needs it less than a real-time
   game, but the buffering/chord model is the same code and the replay hook
   (`mu_input_frame`) is what `mu_save` needs for demo recording.
3. **`mu_camera.h` + geometry queries** — needed the moment anything moves
   smoothly on screen, and the frame-rate-independence gate is a differentiator
   (most tutorials get this wrong).
4. **`mu_timeline.h`** — cheap, and it is what makes the rest *feel* sequenced.
5. **`mu_audio.h`** — biggest, most self-contained, best done when the four
   above have set the house style for gates and benches.
