# PERF_ROGUELIKE — what the new roguelike algorithms cost

`mu_roguelike.h` was reworked around four algorithmic changes (symmetric
shadowcasting FOV, a bucket-queue distance field with terrain costs, an indexed
A\* heap with a corner rule, and the generators the header was missing). This is
the evidence for them, measured as paired A/B inside one binary so that both
sides of every row are compiled with the same flags and read by the same clock.

The baselines are the *previous* implementations, kept in `bench_roguelike.c` on
purpose: the header no longer contains them, and a baseline next to the code it
is compared against is the only way a "the old one was slower" claim stays
honest.

```sh
make -C perf_test rogue-bench        # taskset -c 0 ./bench_roguelike --samples=21 --confidence=6
./bench_roguelike --filter=dijkstra --samples=21
./bench_roguelike --csv=perf_test/results_roguelike.csv
```

Host: Xeon @ 2.60 GHz, 2 vCPU shared guest, gcc 14.2.0, `-O3 -march=native`,
pinned with `taskset -c 0`, 21 samples, per-row CV and 99% CI reported by
`mu_bench.h`.

## The table

Two maps, fixed for every row so the numbers are comparable:

* **field / path map** — 129x97, rooms on a 16x12 grid with one-cell corridors
  through every room row *and* every room column, plus 3% rubble. 5,394 open
  cells, 5,385 reachable: one connected component, so a distance field and a path
  actually traverse the whole map. (The first version of this map had the
  corridors cut at the room boundaries; the searches finished inside 673 cells
  and the table measured the setup cost of two searches. Fixed, not tuned.)
* **FOV map** — 61x61, pillars every 6 cells, radius 24, origin at the centre.

| row | median | min | CV | vs. baseline |
|---|---:|---:|---:|---:|
| `fov.raycast` *(baseline)* | 21.87 us | 21.10 | 4.5% | — |
| `fov.shadowcast` r=24 | 24.04 us | 23.78 | 1.7% | **1.10x slower** |
| `fov.shadowcast_unlimited` | 36.76 us | 35.83 | 8.5% | n/a (no radius) |
| `dijkstra.spfa_previous` *(baseline, uniform cost)* | 218.8 us | 216.4 | 2.0% | — |
| `dijkstra.buckets_dial` | 450.1 us | 435.1 | 4.6% | **2.06x slower** |
| `dijkstra.spfa_weighted_previous` *(baseline, terrain cost)* | 734.5 us | 656.2 | 3.4% | — |
| `dijkstra.buckets_weighted` | 635.5 us | 606.8 | 5.1% | **1.16x faster** |
| `astar.push_duplicates_previous` *(baseline)* | 491.5 us | 405.8 | 7.2% | — |
| `astar.decrease_key` | 464.5 us | 455.4 | 1.2% | 1.06x faster (a wash) |
| `astar.weighted_1_5` | 35.7 us | 35.3 | 1.6% | **13.8x faster** |
| `astar.plus_smooth` | 478.8 us | 472.6 | 1.8% | +3% over exact, 3-4x fewer waypoints |
| `gen.rooms_rejection` *(baseline)* | 9.81 us | 9.49 | 16.2% ⚠ | — |
| `gen.bsp` | 15.17 us | 15.00 | 1.6% | 1.55x slower, whole map planned |
| `gen.maze_perfect` | 193.5 us | 186.4 | 2.2% | new capability |
| `gen.maze_braided_40` | 291.9 us | 283.4 | 1.2% | new capability |
| `gen.autotile_masks` | 74.7 us | 54.5 | 7.0% | 5.9 ns per cell, whole map |

⚠ `gen.rooms_rejection` exceeded the 6% stability cutoff in this run. It is
rejection sampling: the amount of work depends on how many room placements
collide, so its sample spread is a property of the algorithm, not of the
machine. Read that row as "about 10 us".

## What the numbers actually say

**FOV: a 10% cost for exactness and symmetry.** The raycast is the cheap classic
and it is 13-14% asymmetric on a 25%-rubble map (`test_roguelike.c` asserts the
asymmetry is non-zero so the claim stays testable). Shadowcasting costs 10% more
wall-clock at radius 24 and buys: symmetry by construction (0 asymmetric pairs on
rubble, pillar and corridor maps), visibility that is a plain sector test instead
of a set of sampled rays, and no FPU involvement at all — the slopes are integer
fractions, and the gate checks the output is bit-identical to the float
formulation over 60 random maps. Worth it for a rules-visible quantity.

**Dijkstra: 2.06x slower on uniform cost, 1.16x faster with terrain.** This is
the honest loss. Dial's bucket queue is exact and its queue is bounded by
`2n + max_step + 1` ints, and it supports cost multipliers, which the old field
did not support at all — but on a plain 8-connected uniform map a FIFO field with
re-pushes is nearly a BFS and does less bookkeeping per cell (no unlink, no ring
arithmetic). If all you ever want is a uniform-cost flow field, the FIFO version
is still the faster tool; the reason it is no longer shipped is that its
documented scratch (`w*h` ints) is smaller than the number of entries it can
push, so a caller who did what the docs said got a buffer overflow, and it
silently ignored `mu_rog_move_cost_fn`. The bucket version trades 2x on the easy
case for a bounded queue, exactness under weights, and a 1.16x win on the case
that used to be impossible.

**A\*: a wash on exact search, 13.8x on the knob.** The indexed heap with
decrease-key costs roughly what the duplicate-push heap costs on an exact search
(run to run, between 0.94x and 1.02x — the two designs are within each other's
noise here), because this map rarely improves an open node. What it buys is a
heap that cannot exceed `w*h` entries, where the duplicate-push scheme can push
more entries than the array it was given — the same overflow class as the
Dijkstra queue. The real number in this suite is `astar.weighted_1_5`: a heuristic
weight of 1.5 takes the same path query from 464 us to 36 us, 13.8x, for a path
up to 50% longer. For a monster replanning every turn on a large map that is the
difference between 0.5 ms and 0.04 ms; the gate checks that the weighted result is
never cheaper than the exact one and never expands more nodes.

**Smoothing: free.** `mu_rog_path_smooth` costs 3% of a path query and returns the
same octile cost with 3-4x fewer waypoints, which is what a movement controller
consumes. The gate asserts the cost never increases, both endpoints survive, and
no waypoint lands on a wall.

**Generators.** BSP is 1.55x the cost of rejection-sampled rooms for a 129x97 map
and, unlike it, plans the whole map and needs no component pass afterwards: the
corridor pass over the tree connects everything by construction (the gate checks
one component, disjoint rooms, and rooms inside their leaves). The maze is a
recursive backtracker at ~194 us perfect and ~292 us braided — the braid pass is
half again because it rescans the whole map looking for dead ends. Autotiling is
6 ns per cell for the 8-bit mask with the corner rule, which is cheap enough to do
on every frame you redraw.

## Losses, plainly

1. **The distance field is slower than it was.** 2.06x on uniform-cost maps.
   Weighted maps are the only case where the new one wins (1.16x), and the
   weighted case did not exist before.
2. **Exact A\* did not get faster.** The corner rule adds up to eight predicate
   calls per expanded node (two per diagonal), and that is roughly the gain from
   the indexed heap and lazy stamping. The win in this suite comes from the
   heuristic weight, which is a knob, not an algorithm.
3. **Shadowcasting is 10% slower than the raycast.** Correctness, not speed.
4. **BSP is slower than rejection sampling** for the same map size. It buys
   structure (every leaf gets exactly one room, nothing can overlap, the corridor
   count is leaves-1), not throughput.
5. **`gen.rooms_rejection`'s spread is 16%**, above the gate's cutoff, because
   rejection sampling's work is data-dependent. The bench reports it as UNSTABLE
   rather than hiding it.

## Reproducing

```sh
make -C perf_test rogue-test         # 13363 checks, 0 failures
make -C perf_test rogue-bench        # the table above
./bench_roguelike --filter=astar --samples=41 --confidence=10   # a tighter run of one suite
```

Every row is a full operation (one field of view, one distance field, one path,
one dungeon), so `us` is `us per call`, not per cell. Per-cell figures are in the
prose above and are derived by dividing by the known cell counts (5,385 reachable
field cells, 61x61 FOV map).
