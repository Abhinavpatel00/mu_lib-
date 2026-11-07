# Roguelike toolkit: `mu_roguelike.h`

A self-contained grid-game toolkit: RNG, grids, map generation, field of
view, pathfinding, and a tick scheduler. All `static inline`
(`MU_ROG_INLINE`), C99, malloc-overridable (`MU_ROG_MALLOC`...). Uses cglm
when available (`MU_ROG_HAS_CGLM`) for vector helpers.

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

---

## Line of sight & FOV

```c
typedef struct mu_rog_los_ctx { /* visited flags, blocked fn */ } mu_rog_los_ctx;
typedef struct mu_rog_fov_ctx { /* radius, visited, opaque fn */ } mu_rog_fov_ctx;

typedef int (*mu_rog_cell_blocked_fn)(int x, int y, void* user);
typedef int (*mu_rog_line_visit_fn)(int x, int y, void* user);
```

```c
/* Bresenham line, visitor per cell, stops when visitor returns 0 */
mu_rog_line_bresenham(x0, y0, x1, y1, visit_fn, user);

/* symmetric Bresenham LOS test */
int visible = mu_rog_has_line_of_sight(x0, y0, x1, y1, blocked_fn, user);

/* raycast FOV: cast a ray to every border cell of the view box */
mu_rog_fov_raycast(w, h, cx, cy, radius, visible_out, opaque_fn, user);
```

`mu_rog_fov_raycast` marks `visible_out[y*w + x] = 1` for every cell any
ray touches. Raycast FOV is the cheap classic; expect thin wall-shadow
artifacts compared to recursive shadowcasting — acceptable for most games.

---

## Distance maps & pathfinding

### BFS (4-dir) and Dijkstra (8-dir)

```c
mu_rog_bfs_distance4(w, h, sources, n_sources, dist_out, blocked_fn, user);
mu_rog_dijkstra_map8(w, h, sources, n_sources, dist_out, blocked_fn, cost_fn, user);
```

- `dist_out` is a caller-provided `int*` (or similar) grid; unreached cells
  get a sentinel.
- Dijkstra takes a per-step `mu_rog_move_cost_fn(from_x, from_y, to_x, to_y, user)`
  so terrain costs shape the field. Great for "flow toward player/away from
  player" hordes: compute once per turn, then have every monster step
  downhill — no per-monster pathfinding.

### A* (8-dir, binary heap)

```c
typedef struct mu_rog_astar_node       { /* g, f, parent, state */ } mu_rog_astar_node;
typedef struct mu_rog_astar_heap_entry { /* f, node index      */ } mu_rog_astar_heap_entry;

int steps = mu_rog_astar_find_path(
    w, h, sx, sy, gx, gy,
    nodes,          /* scratch array [w*h]                */
    heap,           /* scratch array [w*h]                */
    out_path, out_path_cap,   /* reversed path indices    */
    blocked_fn, cost_fn, user);
```

- Octile heuristic, binary heap (`mu_rog_astar_heap_push` / `..._pop`).
- `out_path` receives cell indices from goal back toward start
  (reconstruct via `mu_rog_astar_reconstruct`); returns the step count or a
  failure sentinel. Caller owns all scratch memory — zero allocation.

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
