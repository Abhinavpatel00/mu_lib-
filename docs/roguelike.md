# Roguelike toolkit: `mu_roguelike.h`

A self-contained grid-game toolkit: RNG, grids, map generation, field of
view, pathfinding, and a tick scheduler. All `static inline`
(`MU_ROG_INLINE`), C99, malloc-overridable (`MU_ROG_MALLOC`...), and no
external math dependency: the vector helpers are local.

## What changed, and what will break

Everything below is additive or a documented behaviour fix, but four calls
changed shape and one struct grew:

| change | effect |
|---|---|
| `mu_rog_dijkstra_map8` / `_ex` scratch | now `2n + max_step + 1` ints, not `n`. The old requirement was already too small for the old implementation. Use `mu_rog_dijkstra_scratch_ints(w, h, &opts)`. |
| `mu_rog_move_cost_fn` meaning | returns a **multiplier** for entering a cell (`1.0` floor, `2.0` mud, `<= 0` impassable), and the distance field now honours it at all - it used to ignore the callback. A\* uses the same meaning. |
| `mu_rog_astar_find_path` diagonals | no longer cut corners by default. A monster can no longer step diagonally between two walls. Pass `cut_corners = 1` in `mu_rog_search_opts` to get the old movement back via `_ex`. |
| `mu_rog_astar_node` | gained `heap_pos` and `stamp`; `mu_rog_astar_find_path_ex` takes a `stamp` argument. Source-compatible for the allocating wrapper, a recompile for anyone filling the struct themselves. |
| `mu_rog_has_line_of_sight` | symmetric by construction (two walks). `mu_rog_los_one_way` is the old single walk. |

---

## Small math helpers

```c
mu_rog_iabs / mu_rog_imax / mu_rog_imin      /* int   */
mu_rog_fmax / mu_rog_fmin                    /* float */

mu_rog_in_bounds_i(x, y, w, h)               /* bounds check  */
mu_rog_grid_index(x, y, w)                   /* y*w + x       */

mu_rog_euclidean2f(x0, y0, x1, y1)           /* float dist    */
mu_rog_chebyshev(dx, dy)                     /* 8-dir dist    */
mu_rog_manhattan(dx, dy)                     /* 4-dir dist    */
mu_rog_octile_heuristic(dx, dy)              /* 8-dir A* cost */
```

---

## RNG (`mu_rog_rng`)

SplitMix64-seeded PCG-style generator with typed draws:

```c
typedef struct mu_rog_rng { uint64_t state, inc; } mu_rog_rng;

mu_rog_rng rng;
mu_rog_rng_seed(&rng, 12345);

uint64_t u64 = mu_rog_rng_u64(&rng);
uint32_t u32 = mu_rog_rng_u32(&rng);
float    f   = mu_rog_rng_f01(&rng);                       /* [0,1)   */
int      i   = mu_rog_rng_range_i(&rng, lo, hi_inclusive);   /* span via hi-lo+1 */
float    g   = mu_rog_rng_range_f(&rng, lo, hi);

mu_rog_shuffle_i32(&rng, items, count);    /* Fisher-Yates            */
int roll = mu_rog_roll_dice_notation(&rng, "2d6+1");  /* dice parser  */
```

### Weighted picks

```c
typedef struct mu_rog_weighted_entry { int id; float weight; } mu_rog_weighted_entry;

mu_rog_weighted_entry table[] = { {GOBLIN, 5.0f}, {ORC, 2.0f}, {TROLL, 1.0f} };
int pick = mu_rog_weighted_pick(&rng, table, 3);
```

Weights are floats; zero/negative weights are skipped. Total weight is
computed per call (no caching), so mutating tables between picks is safe.

---

## Grids

Three element widths, all plain structs over caller-owned memory:

```c
typedef struct { int32_t w, h; uint8_t*  cells; } mu_rog_grid_u8;
typedef struct { int32_t w, h; int16_t*  cells; } mu_rog_grid_i16;
typedef struct { int32_t w, h; float*    cells; } mu_rog_grid_f32;
```

```c
mu_rog_grid_u8_get(g, x, y);   mu_rog_grid_u8_set(g, x, y, v);
mu_rog_grid_u8_fill(g, value); /* whole grid            */
```

---

## Map generation

### Rooms + corridors

```c
typedef struct mu_rog_roomgen_params
{
    /* room count range, room size range, corridor style, padding */
} mu_rog_roomgen_params;

int rooms = mu_rog_generate_rooms_and_corridors(&rng, map, &params, floor_tile, wall_tile);
```

Places non-overlapping rects (with padding), carves them, then connects
each room to the previous one with L-corridors
(`mu_rog_carve_l_corridor`) or straight h/v tunnels
(`mu_rog_carve_h_tunnel` / `mu_rog_carve_v_tunnel`).
Rect helpers: `mu_rog_rect_intersects(a, b, pad)`,
`mu_rog_rect_center(r)`, `mu_rog_carve_rect`.

### Caves (cellular automata)

```c
typedef struct mu_rog_cave_params
{
    /* fill %, sim steps, birth/survive neighbor thresholds */
} mu_rog_cave_params;

mu_rog_generate_cave(&rng, map, &params, floor_tile, wall_tile);
```

Pipeline: random fill (`mu_rog_generate_cave_random_fill`) → N smoothing
steps of `mu_rog_cave_step` (Moore neighborhood via
`mu_rog_count_wall_neighbors8`).

### Connectivity

```c
typedef struct mu_rog_component_result { int count; /* label per cell */ } mu_rog_component_result;

mu_rog_component_result r = mu_rog_label_components4(map, scratch, ...);  /* 4-dir flood fill */
mu_rog_keep_largest_component(map, scratch, ...);  /* prune islands        */
```

Standard post-gen cleanup: label floor regions, keep the biggest one so the
player can reach everything.

### BSP dungeon

```c
typedef struct mu_rog_bsp_node
{
    mu_rog_rect rect;    /* the area this node owns                          */
    mu_rog_rect room;    /* leaves: the carved room; internal: representative */
    int left, right, parent, depth;
} mu_rog_bsp_node;

mu_rog_bsp_node nodes[2 * max_rooms - 1];
int count = mu_rog_bsp_build(nodes, cap, bounds, min_leaf, max_depth, &rng);
int rooms = mu_rog_bsp_carve_rooms(nodes, count, &rng, map, w, h,
                                   wall, floor, min_room, out_rooms, out_cap);
mu_rog_bsp_carve_corridors(nodes, count, map, w, h, wall, floor);
```

Split the map recursively (always the longer side, at 40-60% of it, so rooms come
out roughly square instead of ribbon-shaped), put exactly one room in each leaf,
then join siblings up the tree. Because the tree covers the map and every leaf
has a room, the corridor pass connects everything by construction: no flood fill,
no "keep largest component" step, and the corridor count is exactly
`leaves - 1`. Rooms are inset by one cell, so two leaves can never produce
touching rooms - the gate checks that pairwise, plus that every room is inside its
leaf and that the result is one component.

Node storage is caller-owned and needs `2 * leaves - 1` entries.

### Maze

```c
mu_rog_generate_maze(w, h, &rng, map, wall, floor, braid_percent,
                     stack /* scratch, w*h ints */, stack_cap);
```

Recursive backtracker over the odd-indexed cells (iterative, explicit stack, each
cell visited once, O(w*h)). `braid_percent = 0` keeps a perfect maze - exactly one
path between any two cells, which the gate checks by counting edges against
cells. A perfect maze is the wrong dungeon, though: every dead end is a corridor
the player walks in and back out for nothing, so `braid_percent` knocks out the
wall of that share of dead ends. 30-50 gives a maze with a few cycles; 100 gives
loops everywhere.

### Doors

```c
int placed = mu_rog_place_doors(map, w, h, rooms, room_count, floor, door,
                                door_percent, &rng);
```

Scans the ring of wall cells immediately outside each room and turns the cells
that are a single-cell gap (floor on the room side, floor on the corridor side,
wall on both other sides) into `door`. `door_percent` decides how many of those
become doors, so "all doors" and "a few doors" are the same call with a different
number. Corner cells and two-wide corridor mouths are rejected, which is the whole
point: a double door on a room corner is what a naive "wall cell with floor on
both sides" scan produces.

### Autotiling

```c
typedef int (*mu_rog_is_wall_fn)(int x, int y, void* user);

uint8_t mask = mu_rog_wall_bitmask8(w, h, x, y, is_wall_fn, user);
int     idx  = mu_rog_autotile_index16(mask);         /* 16-tile atlas  */
const char* glyph = mu_rog_autotile_glyph8(mask);     /* UTF-8 box drawing */
```

Bits 0-3 are the cardinal walls (`MU_ROG_WALL_N/E/S/W`), bits 4-7 the diagonals
(`NE/SE/SW/NW`). A diagonal bit is only set when **both** of its cardinals are
walls - without that rule a diagonal touching two separate wall runs picks the
inner-corner tile in the wrong place, which is the single most common autotiling
bug. Cells outside the map count as wall, so the map edge closes up.
`mu_rog_autotile_glyph8` is a direct check on the mask: a cross corridor must come
out as the four-way glyph, an elbow as an elbow.

### Connecting regions

```c
int carved = mu_rog_connect_regions(map, floor, carve_tile, labels, queue, max_links);
```

Labels 4-connected floor, then carves an L corridor from each stray region to the
largest one and relabels, until there is a single component. BSP does not need it;
a cave generator does.

---

## Line of sight & FOV

```c
typedef int (*mu_rog_cell_blocked_fn)(int x, int y, void* user);
typedef int (*mu_rog_line_visit_fn)(int x, int y, void* user);

/* Bresenham line, visitor per cell, stops when the visitor returns 0 */
mu_rog_line_bresenham(x0, y0, x1, y1, visit_fn, user);

/* one walk (the old behaviour) and the symmetric predicate */
int mu_rog_los_one_way(x0, y0, x1, y1, blocked_fn, user);
int mu_rog_has_line_of_sight(x0, y0, x1, y1, blocked_fn, user);
```

`mu_rog_has_line_of_sight` is symmetric by construction: it walks the line in
both directions and accepts either. Bresenham is not symmetric (the cells it
visits from A to B are not the cells from B to A, so about 1 in 8 diagonal ties
disagree), and "the player sees a monster that cannot see the player" is a rules
bug, not a rendering preference. Two O(n) walks instead of one removes it; the
gate asserts the raw walk is asymmetric somewhere and the predicate never is.

```c
/* symmetric recursive shadowcasting: the FOV to use */
int marked = mu_rog_fov_shadowcast(w, h, ox, oy, radius,
                                   blocked_fn, user, out_visible /* w*h bytes */);

/* the previous FOV, kept for compatibility and for callers that want the rays */
void mu_rog_fov_raycast(w, h, cx, cy, radius,
                        blocked_fn, user, visible_out);
```

Shape of the call, and why it is the shape:

* One pass over the diamond, one quadrant at a time, walking rows outward. A row
  is a run of cells at a fixed depth; a sector is the angular range still
  visible. Opaque cells shorten the sector; an opaque-to-clear transition starts
  a tighter sector on the next row (recursion).
* A cell is marked when its **centre** is inside the sector. That is the
  symmetric variant, and it is what takes the asymmetry from 13-14% of visible
  pairs to zero. The price is one cell of sight through a diagonal gap, which is
  the standard behaviour of every shadowcaster in use.
* All slope arithmetic is integer: a slope is `num/den`, "is column `c` inside
  slope `s` at depth `d`" is `c*den >= d*num`, and the row bounds are the same
  comparison with integer division. No float, no FPU, and the gate checks the
  output is bit-identical to the float formulation over 60 random maps.
* `radius <= 0` means "limited only by the map". The origin is always marked; if
  the origin is solid, only the origin is marked.

Two behaviours worth knowing, both tested:

* A one-cell doorway passes sight straight through it, and the wall beside it
  does not leak.
* A diagonal gap between two walls (a corner) **does** pass sight. Both this
  implementation and the reference implementation it was measured against have
  that property; treat it as a documented rule of the algorithm.

Cost, measured: 1.10x the raycast at radius 24 on a 61x61 pillar map
(`perf_test/PERF_ROGUELIKE.md`). You are paying about 10% for symmetry,
exactness and no artifacts.

---

## Distance maps & pathfinding

### BFS (4-dir)

```c
mu_rog_bfs_distance4(w, h, sources, n_sources, dist_out, blocked_fn, user);
```

Unchanged: unit-cost flood, `-1` for unreached cells.

### Distance field (8-dir, terrain-aware)

```c
typedef struct mu_rog_search_opts
{
    int allow_diag;    /* 0 = 4-neighbour, 1 = 8-neighbour (default 1)          */
    int cut_corners;   /* 1 = a diagonal may squeeze between two walls (default 0) */
    int max_step;      /* largest integer step cost the queue may see           */
} mu_rog_search_opts;

mu_rog_search_opts opts = mu_rog_search_opts_default();   /* 8-dir, no corner cut */

int need = mu_rog_dijkstra_scratch_ints(w, h, &opts);     /* 2n + max_step + 1 ints */
mu_rog_dijkstra_map8_ex(w, h, goals, n_goals, blocked_fn, cost_fn, user,
                        &opts, dist_out /* int16 */, scratch);

/* the old signature, kept: uniform cost, 8-dir, no corner cutting */
mu_rog_dijkstra_map8(w, h, goals, n_goals, blocked_fn, user, dist_out, scratch);
```

- `dist_out[i]` is the cost to reach cell `i` in tenths: 10 per orthogonal step,
  14 per diagonal, so `1.0` and `sqrt(2)` to the caller. `32767` means
  unreachable. `0` is the goal.
- `cost_fn(from_x, from_y, to_x, to_y, user)` returns a **multiplier** for
  entering the cell: `1.0` floor, `2.0` mud, `0.5` road, `<= 0` impassable. This
  is a change from the old header: it used to be treated as an absolute cost and
  was in fact ignored entirely by the distance field. The same multiplier
  semantics apply to A\* below, so a route costs the same to both searches.
- `max_step` bounds the bucket ring: set it to `14 * largest multiplier` (its
  default covers the uniform case). A larger `cost_fn` result than `max_step` is
  clamped, not silently corrupting the queue.
- **Scratch is `2n + max_step + 1` ints, not `n`.** The old requirement was too
  small for the old implementation as well - it could push more entries than the
  buffer held.

The queue is Dial's: with integer steps bounded by `max_step` there are at most
`max_step+1` distances in flight, so a ring of that many FIFO buckets replaces
the heap and every cell is expanded exactly once. Each bucket is a doubly linked
list over the node array, so a cell that is improved while queued is unlinked
from its old bucket in O(1) - a single-link intrusive list cannot do that, and
the first version of this code silently truncated the bucket it came from (the
gate compared against an O(n^2) reference and caught it).

Honest note: for a plain uniform-cost map this is **2.06x slower** than the
tenths-weighted FIFO field it replaces, and 1.16x *faster* once terrain
multipliers are involved. The trade is a bounded queue and exact weights for
throughput on the easy case; `perf_test/PERF_ROGUELIKE.md` has both rows.

Walking a field:

```c
mu_rog_ivec2 next;
if (mu_rog_dijkstra_descend(w, h, dist, x, y, allow_diag, &next)) { /* step there */ }
```

### A* (8-dir)

```c
int steps = mu_rog_astar_find_path_ex(
    w, h, start, goal, &opts,
    blocked_fn, cost_fn, user,
    heuristic_weight,       /* 1.0 = exact, 1.5 = greedy                  */
    stamp,                  /* generation id; 0 forces a full reset        */
    nodes, heap,            /* scratch: w*h nodes, w*h heap entries        */
    out_path, out_path_cap, /* start..goal, contiguous cells               */
    &cost, &expanded);      /* optional: path cost, nodes closed           */
```

- No allocation. The old `mu_rog_astar_find_path` still exists and still
  allocates; use `_ex` in a game loop.
- `stamp` is why the search no longer clears `w*h` nodes per call: a node whose
  stamp is not the current generation is initialised on first touch, so setup
  costs what the search touches. Pass an increasing value, or `0` once.
- The heap is indexed (each node knows its slot, `heap_pos`), so improving an
  open node is a decrease-key, not a duplicate push. The heap therefore cannot
  hold more than `w*h` entries - the push-duplicate version could, which was a
  latent overflow of the array the caller passed in.
- `heuristic_weight > 1` is weighted A\*: on a 129x97 dungeon, weight 1.5 turns a
  464 us query into 36 us for a path up to 50% longer. The gate checks the
  weighted cost is never below the exact one.
- The corner rule (`opts.cut_corners == 0`, the default) stops a diagonal step
  between two blocked orthogonal neighbours, so monsters cannot cut a corridor
  corner through solid wall.

### Path smoothing

```c
float cost;
int edges = mu_rog_path_cost(path, count, cost_fn, user, &cost);   /* octile sum */
int kept  = mu_rog_path_smooth(path, count, window, blocked_fn, user);
```

Keeps both endpoints and replaces each usable sub-path with its straight chord
where the chord is clear. It cannot make a path worse (the gate asserts cost
non-increase and wall-freedom) and it turns a 25-waypoint zig-zag into 7
waypoints at the same cost, which is what a movement controller wants to consume.
`window = 0` means "as far as the buffer allows".

---

## Tick scheduler

Priority-free event timeline keyed by `(tick, actor_id)`:

```c
typedef struct mu_rog_sched_event { uint32_t actor_id; uint64_t tick; } mu_rog_sched_event;

typedef struct mu_rog_scheduler
{
    mu_rog_sched_event* events;   /* min-heap storage       */
    int count, capacity;
} mu_rog_scheduler;

mu_rog_scheduler_init(&s);
mu_rog_scheduler_reserve(&s, cap);

mu_rog_scheduler_push(&s, actor_id, tick);       /* schedule action  */
mu_rog_scheduler_peek(&s, &out);                 /* next event       */
mu_rog_scheduler_pop(&s, &out);                  /* consume it       */

mu_rog_scheduler_free(&s);
```

Classic energy/turn loop:

```c
for(;;)
{
    mu_rog_sched_event ev;
    if(!mu_rog_scheduler_peek(&sched, &ev)) break;
    if(ev.tick > world_tick) break;

    mu_rog_scheduler_pop(&sched, &ev);
    uint64_t cost = actor_act(ev.actor_id);      /* act returns cost */
    mu_rog_scheduler_push(&sched, ev.actor_id, world_tick + cost);
}
world_tick++;
```

Faster actors simply appear more often at the front of the heap — speed is
just a smaller action cost.
