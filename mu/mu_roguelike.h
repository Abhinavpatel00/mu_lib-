#ifndef MU_ROGUELIKE_H
#define MU_ROGUELIKE_H

/*
	mu_roguelike.h
	---------------------------------------------------------------------------
	Standalone C99 single-header toolbox for roguelike / roguelite gameplay.

	Design goals:
	1) Practical helpers you can call from gameplay code immediately.
	2) Keep ownership explicit (you pass buffers, or use small init/free APIs).
	3) Explain the algorithm math visually in comments near each function.
	4) Use cglm (when available) for basic vector math helpers.

	What is included:
	- RNG (xoroshiro128+) + weighted choices + dice notation parser.
	- Grid utilities and carving helpers (rooms, corridors, drunkard walk).
	- Cellular automata cave smoothing and connected-component cleanup.
	- Line of sight, raycast FOV, Bresenham line traversal.
	- Pathfinding helpers: BFS distance field, Dijkstra map, A*.
	- Turn scheduler min-heap for initiative / speed systems.

	---------------------------------------------------------------------------
	VISUAL QUICK INDEX
	---------------------------------------------------------------------------

	  Grid indexing:
		i = y * w + x

	  Room carve:
		###########
		##.......##
		##.......##
		###########

	  A* idea:
		f(n) = g(n) + h(n)
		g(n): exact cost start -> n
		h(n): heuristic estimate n -> goal

	  Dijkstra map:
		Seed goal cells with 0, then flood outward with +1/+cost.

	  Raycast FOV:
		cast many Bresenham rays from player to perimeter of radius square.

	  Scheduler heap:
		next actor = min(tick), stable by serial id

	---------------------------------------------------------------------------
	Usage:
	  - Include this header.
	  - Optionally define MU_ASSERT/MALLOC/FREE to route memory.
	  - Optionally define MU_ROG_CGLM_HEADER before include if cglm path custom.

	---------------------------------------------------------------------------
*/

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <assert.h>

#include "mu_macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* cglm detection (optional, used for simple vector math helpers)            */
/* ------------------------------------------------------------------------- */

#if defined(MU_ROG_CGLM_HEADER)
#include MU_ROG_CGLM_HEADER
#define MU_ROG_HAS_CGLM 1
#elif defined(__has_include)
#if __has_include(<cglm/cglm.h>)
#include <cglm/cglm.h>
#define MU_ROG_HAS_CGLM 1
#elif __has_include("../external/cglm/include/cglm/cglm.h")
#include "../external/cglm/include/cglm/cglm.h"
#define MU_ROG_HAS_CGLM 1
#elif __has_include("../../external/cglm/include/cglm/cglm.h")
#include "../../external/cglm/include/cglm/cglm.h"
#define MU_ROG_HAS_CGLM 1
#else
#define MU_ROG_HAS_CGLM 0
#endif
#else
#define MU_ROG_HAS_CGLM 0
#endif

/* ------------------------------------------------------------------------- */
/* Basic types                                                               */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_ivec2 {
	int x;
	int y;
} mu_rog_ivec2;

typedef struct mu_rog_rect {
	int x;
	int y;
	int w;
	int h;
} mu_rog_rect;

typedef struct mu_rog_grid_u8 {
	int w;
	int h;
	uint8_t* data;
} mu_rog_grid_u8;

typedef struct mu_rog_grid_i16 {
	int w;
	int h;
	int16_t* data;
} mu_rog_grid_i16;

typedef struct mu_rog_grid_f32 {
	int w;
	int h;
	float* data;
} mu_rog_grid_f32;

typedef int (*mu_rog_cell_blocked_fn)(int x, int y, void* user);
typedef float (*mu_rog_move_cost_fn)(int from_x, int from_y, int to_x, int to_y, void* user);
typedef int (*mu_rog_line_visit_fn)(int x, int y, void* user);

/* ------------------------------------------------------------------------- */
/* Scalar / vector helpers                                                   */
/* ------------------------------------------------------------------------- */

MU_INLINE int mu_rog_iabs(int v) { return (v < 0) ? -v : v; }
MU_INLINE int mu_rog_imax(int a, int b) { return (a > b) ? a : b; }
MU_INLINE int mu_rog_imin(int a, int b) { return (a < b) ? a : b; }
MU_INLINE float mu_rog_fmax(float a, float b) { return (a > b) ? a : b; }
MU_INLINE float mu_rog_fmin(float a, float b) { return (a < b) ? a : b; }

MU_INLINE int mu_rog_in_bounds_i(int x, int y, int w, int h)
{
	return (x >= 0 && y >= 0 && x < w && y < h);
}

MU_INLINE int mu_rog_grid_index(int x, int y, int w)
{
	return y * w + x;
}

MU_INLINE float mu_rog_euclidean2f(float x0, float y0, float x1, float y1)
{
#if MU_ROG_HAS_CGLM
	vec2 a = {x0, y0};
	vec2 b = {x1, y1};
	return glm_vec2_distance(a, b);
#else
	float dx = x1 - x0;
	float dy = y1 - y0;
	return sqrtf(dx * dx + dy * dy);
#endif
}

MU_INLINE int mu_rog_chebyshev(int dx, int dy)
{
	return mu_rog_imax(mu_rog_iabs(dx), mu_rog_iabs(dy));
}

MU_INLINE int mu_rog_manhattan(int dx, int dy)
{
	return mu_rog_iabs(dx) + mu_rog_iabs(dy);
}

MU_INLINE float mu_rog_octile_heuristic(int dx, int dy)
{
	/*
	   Octile metric for 8-direction movement:

		 h = D*(dx+dy) + (D2-2D)*min(dx,dy)

	   with D=1 and D2=sqrt(2).
	*/
	int adx = mu_rog_iabs(dx);
	int ady = mu_rog_iabs(dy);
	int mn = mu_rog_imin(adx, ady);
	int mx = mu_rog_imax(adx, ady);
	return (float)(mx - mn) + 1.41421356237f * (float)mn;
}

/* ------------------------------------------------------------------------- */
/* RNG                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_rng {
	uint64_t s0;
	uint64_t s1;
} mu_rog_rng;

MU_INLINE uint64_t mu_rog_rotl64(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

MU_INLINE uint64_t mu_rog_splitmix64_next(uint64_t* state)
{
	uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

MU_INLINE void mu_rog_rng_seed(mu_rog_rng* rng, uint64_t seed)
{
	uint64_t sm = seed;
	rng->s0 = mu_rog_splitmix64_next(&sm);
	rng->s1 = mu_rog_splitmix64_next(&sm);
	if ((rng->s0 | rng->s1) == 0ULL) {
		rng->s0 = 0x0123456789abcdefULL;
		rng->s1 = 0xfedcba9876543210ULL;
	}
}

MU_INLINE uint64_t mu_rog_rng_u64(mu_rog_rng* rng)
{
	/* xoroshiro128+ */
	uint64_t s0 = rng->s0;
	uint64_t s1 = rng->s1;
	uint64_t result = s0 + s1;

	s1 ^= s0;
	rng->s0 = mu_rog_rotl64(s0, 55) ^ s1 ^ (s1 << 14);
	rng->s1 = mu_rog_rotl64(s1, 36);
	return result;
}

MU_INLINE uint32_t mu_rog_rng_u32(mu_rog_rng* rng)
{
	return (uint32_t)(mu_rog_rng_u64(rng) >> 32);
}

MU_INLINE float mu_rog_rng_f01(mu_rog_rng* rng)
{
	/* 24-bit precision float in [0, 1). */
	return (float)(mu_rog_rng_u32(rng) >> 8) * (1.0f / 16777216.0f);
}

MU_INLINE int mu_rog_rng_range_i(mu_rog_rng* rng, int lo_inclusive, int hi_inclusive)
{
	MU_ASSERT(hi_inclusive >= lo_inclusive);
	{
		uint32_t span = (uint32_t)(hi_inclusive - lo_inclusive + 1);
		uint32_t r = mu_rog_rng_u32(rng);
		return lo_inclusive + (int)(r % span);
	}
}

MU_INLINE float mu_rog_rng_range_f(mu_rog_rng* rng, float lo, float hi)
{
	return lo + (hi - lo) * mu_rog_rng_f01(rng);
}

MU_INLINE void mu_rog_shuffle_i32(mu_rog_rng* rng, int32_t* items, int count)
{
	int i;
	for (i = count - 1; i > 0; --i) {
		int j = mu_rog_rng_range_i(rng, 0, i);
		int32_t t = items[i];
		items[i] = items[j];
		items[j] = t;
	}
}

/* Dice parser: "3d6+2", "1d20", "2d8-1" */
MU_INLINE int mu_rog_roll_dice_notation(mu_rog_rng* rng, const char* text)
{
	int n = 0;
	int sides = 0;
	int sign = 1;
	int bonus = 0;
	int sum = 0;
	int i;

	if (!text) return 0;

	/* parse N */
	i = 0;
	while (text[i] >= '0' && text[i] <= '9') {
		n = n * 10 + (text[i] - '0');
		++i;
	}
	if (n <= 0) n = 1;

	if (text[i] != 'd' && text[i] != 'D') return 0;
	++i;

	while (text[i] >= '0' && text[i] <= '9') {
		sides = sides * 10 + (text[i] - '0');
		++i;
	}
	if (sides <= 0) return 0;

	if (text[i] == '+' || text[i] == '-') {
		sign = (text[i] == '-') ? -1 : 1;
		++i;
		while (text[i] >= '0' && text[i] <= '9') {
			bonus = bonus * 10 + (text[i] - '0');
			++i;
		}
	}

	for (i = 0; i < n; ++i) {
		sum += mu_rog_rng_range_i(rng, 1, sides);
	}
	sum += sign * bonus;
	return sum;
}

/* ------------------------------------------------------------------------- */
/* Weighted choice                                                           */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_weighted_entry {
	int id;
	float weight;
} mu_rog_weighted_entry;

MU_INLINE int mu_rog_weighted_pick(mu_rog_rng* rng, const mu_rog_weighted_entry* entries, int count)
{
	float total = 0.0f;
	int i;
	for (i = 0; i < count; ++i) {
		if (entries[i].weight > 0.0f) total += entries[i].weight;
	}
	if (total <= 0.0f || count <= 0) return -1;

	{
		float r = mu_rog_rng_range_f(rng, 0.0f, total);
		float acc = 0.0f;
		for (i = 0; i < count; ++i) {
			float w = entries[i].weight;
			if (w <= 0.0f) continue;
			acc += w;
			if (r <= acc) return entries[i].id;
		}
		return entries[count - 1].id;
	}
}

/* ------------------------------------------------------------------------- */
/* Grid helpers and carving                                                  */
/* ------------------------------------------------------------------------- */

MU_INLINE void mu_rog_grid_u8_fill(mu_rog_grid_u8 g, uint8_t value)
{
	MU_MEMSET(g.data, value, (size_t)g.w * (size_t)g.h * sizeof(uint8_t));
}

MU_INLINE uint8_t mu_rog_grid_u8_get(mu_rog_grid_u8 g, int x, int y)
{
	if (!mu_rog_in_bounds_i(x, y, g.w, g.h)) return 0;
	return g.data[mu_rog_grid_index(x, y, g.w)];
}

MU_INLINE void mu_rog_grid_u8_set(mu_rog_grid_u8 g, int x, int y, uint8_t v)
{
	if (!mu_rog_in_bounds_i(x, y, g.w, g.h)) return;
	g.data[mu_rog_grid_index(x, y, g.w)] = v;
}

MU_INLINE int mu_rog_rect_intersects(mu_rog_rect a, mu_rog_rect b, int pad)
{
	return !(a.x + a.w + pad <= b.x ||
			 b.x + b.w + pad <= a.x ||
			 a.y + a.h + pad <= b.y ||
			 b.y + b.h + pad <= a.y);
}

MU_INLINE mu_rog_ivec2 mu_rog_rect_center(mu_rog_rect r)
{
	mu_rog_ivec2 c;
	c.x = r.x + r.w / 2;
	c.y = r.y + r.h / 2;
	return c;
}

MU_INLINE void mu_rog_carve_rect(mu_rog_grid_u8 g, mu_rog_rect r, uint8_t floor_tile)
{
	int y, x;
	for (y = r.y; y < r.y + r.h; ++y) {
		for (x = r.x; x < r.x + r.w; ++x) {
			mu_rog_grid_u8_set(g, x, y, floor_tile);
		}
	}
}

MU_INLINE void mu_rog_carve_h_tunnel(mu_rog_grid_u8 g, int x0, int x1, int y, uint8_t floor_tile)
{
	int x;
	if (x1 < x0) {
		int t = x0;
		x0 = x1;
		x1 = t;
	}
	for (x = x0; x <= x1; ++x) {
		mu_rog_grid_u8_set(g, x, y, floor_tile);
	}
}

MU_INLINE void mu_rog_carve_v_tunnel(mu_rog_grid_u8 g, int y0, int y1, int x, uint8_t floor_tile)
{
	int y;
	if (y1 < y0) {
		int t = y0;
		y0 = y1;
		y1 = t;
	}
	for (y = y0; y <= y1; ++y) {
		mu_rog_grid_u8_set(g, x, y, floor_tile);
	}
}

MU_INLINE void mu_rog_carve_l_corridor(mu_rog_grid_u8 g,
											mu_rog_ivec2 a,
											mu_rog_ivec2 b,
											uint8_t floor_tile,
											mu_rog_rng* rng)
{
	/*
	   L-corridor variants:
		 A-----+
			   |
			   B

	   or
		 A
		 |
		 +-----B
	*/
	if (mu_rog_rng_u32(rng) & 1u) {
		mu_rog_carve_h_tunnel(g, a.x, b.x, a.y, floor_tile);
		mu_rog_carve_v_tunnel(g, a.y, b.y, b.x, floor_tile);
	} else {
		mu_rog_carve_v_tunnel(g, a.y, b.y, a.x, floor_tile);
		mu_rog_carve_h_tunnel(g, a.x, b.x, b.y, floor_tile);
	}
}

typedef struct mu_rog_roomgen_params {
	int map_w;
	int map_h;
	int room_count_max;
	int room_w_min;
	int room_w_max;
	int room_h_min;
	int room_h_max;
	int room_padding;
	uint8_t wall_tile;
	uint8_t floor_tile;
} mu_rog_roomgen_params;

/*
	Room-and-corridor dungeon generation (classic baseline):

	  1) Fill map with walls.
	  2) Sample random room rectangles.
	  3) Keep only non-overlapping rooms.
	  4) Connect accepted room centers in insertion order.
*/
MU_INLINE int mu_rog_generate_rooms_and_corridors(mu_rog_rng* rng,
													   mu_rog_grid_u8 map,
													   const mu_rog_roomgen_params* p,
													   mu_rog_rect* out_rooms,
													   int out_rooms_cap)
{
	int accepted = 0;
	int try_i;

	if (!p || map.w != p->map_w || map.h != p->map_h) return 0;
	if (p->room_w_min <= 0 || p->room_h_min <= 0) return 0;

	mu_rog_grid_u8_fill(map, p->wall_tile);

	for (try_i = 0; try_i < p->room_count_max; ++try_i) {
		mu_rog_rect r;
		int collide = 0;
		int i;

		r.w = mu_rog_rng_range_i(rng, p->room_w_min, p->room_w_max);
		r.h = mu_rog_rng_range_i(rng, p->room_h_min, p->room_h_max);
		if (r.w >= map.w - 2 || r.h >= map.h - 2) continue;

		r.x = mu_rog_rng_range_i(rng, 1, map.w - r.w - 1);
		r.y = mu_rog_rng_range_i(rng, 1, map.h - r.h - 1);

		for (i = 0; i < accepted; ++i) {
			if (mu_rog_rect_intersects(r, out_rooms[i], p->room_padding)) {
				collide = 1;
				break;
			}
		}
		if (collide) continue;
		if (accepted >= out_rooms_cap) break;

		out_rooms[accepted] = r;
		mu_rog_carve_rect(map, r, p->floor_tile);

		if (accepted > 0) {
			mu_rog_ivec2 a = mu_rog_rect_center(out_rooms[accepted - 1]);
			mu_rog_ivec2 b = mu_rog_rect_center(out_rooms[accepted]);
			mu_rog_carve_l_corridor(map, a, b, p->floor_tile, rng);
		}
		++accepted;
	}

	return accepted;
}

/* ------------------------------------------------------------------------- */
/* Cave generation (cellular automata)                                      */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_cave_params {
	int fill_percent;
	int birth_limit;
	int death_limit;
	int steps;
	uint8_t wall_tile;
	uint8_t floor_tile;
} mu_rog_cave_params;

/*
   CA rule intuition:

	 Count neighbors in 8-neighborhood.

	   ###
	   #X#   X = current cell
	   ###

	 - If wall and neighbors < death_limit => becomes floor.
	 - If floor and neighbors > birth_limit => becomes wall.

   This smooths random noise into cave-like blobs.
*/
MU_INLINE int mu_rog_count_wall_neighbors8(mu_rog_grid_u8 map, int x, int y, uint8_t wall_tile)
{
	int dy, dx;
	int n = 0;
	for (dy = -1; dy <= 1; ++dy) {
		for (dx = -1; dx <= 1; ++dx) {
			int nx = x + dx;
			int ny = y + dy;
			if (dx == 0 && dy == 0) continue;
			if (!mu_rog_in_bounds_i(nx, ny, map.w, map.h)) {
				++n;
			} else if (mu_rog_grid_u8_get(map, nx, ny) == wall_tile) {
				++n;
			}
		}
	}
	return n;
}

MU_INLINE void mu_rog_generate_cave_random_fill(mu_rog_rng* rng,
													 mu_rog_grid_u8 map,
													 uint8_t wall_tile,
													 uint8_t floor_tile,
													 int fill_percent)
{
	int y, x;
	for (y = 0; y < map.h; ++y) {
		for (x = 0; x < map.w; ++x) {
			int edge = (x == 0 || y == 0 || x == map.w - 1 || y == map.h - 1);
			if (edge || mu_rog_rng_range_i(rng, 0, 99) < fill_percent) {
				mu_rog_grid_u8_set(map, x, y, wall_tile);
			} else {
				mu_rog_grid_u8_set(map, x, y, floor_tile);
			}
		}
	}
}

MU_INLINE void mu_rog_cave_step(mu_rog_grid_u8 map,
									mu_rog_grid_u8 scratch,
									uint8_t wall_tile,
									uint8_t floor_tile,
									int birth_limit,
									int death_limit)
{
	int y, x;
	MU_ASSERT(scratch.w == map.w && scratch.h == map.h);

	for (y = 0; y < map.h; ++y) {
		for (x = 0; x < map.w; ++x) {
			uint8_t cur = mu_rog_grid_u8_get(map, x, y);
			int n = mu_rog_count_wall_neighbors8(map, x, y, wall_tile);
			uint8_t out = cur;
			if (cur == wall_tile) {
				if (n < death_limit) out = floor_tile;
			} else {
				if (n > birth_limit) out = wall_tile;
			}
			scratch.data[mu_rog_grid_index(x, y, map.w)] = out;
		}
	}

	MU_MEMCPY(map.data, scratch.data, (size_t)map.w * (size_t)map.h * sizeof(uint8_t));
}

MU_INLINE void mu_rog_generate_cave(mu_rog_rng* rng,
										mu_rog_grid_u8 map,
										mu_rog_grid_u8 scratch,
										const mu_rog_cave_params* p)
{
	int i;
	if (!p) return;
	mu_rog_generate_cave_random_fill(rng, map, p->wall_tile, p->floor_tile, p->fill_percent);
	for (i = 0; i < p->steps; ++i) {
		mu_rog_cave_step(map, scratch, p->wall_tile, p->floor_tile, p->birth_limit, p->death_limit);
	}
}

/* ------------------------------------------------------------------------- */
/* Connected components (for cave cleanup)                                   */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_component_result {
	int component_count;
	int largest_component_id;
	int largest_component_size;
} mu_rog_component_result;

MU_INLINE mu_rog_component_result mu_rog_label_components4(mu_rog_grid_u8 map,
																uint8_t passable_tile,
																int* labels,
																int* queue)
{
	int next_label = 1;
	int largest_id = 0;
	int largest_size = 0;
	int y, x;

	MU_MEMSET(labels, 0, (size_t)map.w * (size_t)map.h * sizeof(int));

	for (y = 0; y < map.h; ++y) {
		for (x = 0; x < map.w; ++x) {
			int idx = mu_rog_grid_index(x, y, map.w);
			int qh = 0;
			int qt = 0;
			int size = 0;
			if (labels[idx] != 0) continue;
			if (map.data[idx] != passable_tile) continue;

			labels[idx] = next_label;
			queue[qt++] = idx;

			while (qh < qt) {
				int cur = queue[qh++];
				int cx = cur % map.w;
				int cy = cur / map.w;
				static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
				int d;
				++size;

				for (d = 0; d < 4; ++d) {
					int nx = cx + dirs[d][0];
					int ny = cy + dirs[d][1];
					if (!mu_rog_in_bounds_i(nx, ny, map.w, map.h)) continue;
					{
						int nidx = mu_rog_grid_index(nx, ny, map.w);
						if (labels[nidx] != 0) continue;
						if (map.data[nidx] != passable_tile) continue;
						labels[nidx] = next_label;
						queue[qt++] = nidx;
					}
				}
			}

			if (size > largest_size) {
				largest_size = size;
				largest_id = next_label;
			}
			++next_label;
		}
	}

	{
		mu_rog_component_result r;
		r.component_count = next_label - 1;
		r.largest_component_id = largest_id;
		r.largest_component_size = largest_size;
		return r;
	}
}

MU_INLINE void mu_rog_keep_largest_component(mu_rog_grid_u8 map,
												  uint8_t passable_tile,
												  uint8_t blocked_tile,
												  int* labels,
												  int* queue)
{
	int i;
	mu_rog_component_result r = mu_rog_label_components4(map, passable_tile, labels, queue);
	for (i = 0; i < map.w * map.h; ++i) {
		if (map.data[i] == passable_tile && labels[i] != r.largest_component_id) {
			map.data[i] = blocked_tile;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Bresenham line / LOS                                                      */
/* ------------------------------------------------------------------------- */

/*
   Bresenham line rasterization:

	  p0 *
		 **
		   **
			 * p1

   Visits integer cells approximating the continuous segment.
*/
MU_INLINE int mu_rog_line_bresenham(int x0,
										int y0,
										int x1,
										int y1,
										mu_rog_line_visit_fn visit,
										void* user)
{
	int dx = mu_rog_iabs(x1 - x0);
	int sx = (x0 < x1) ? 1 : -1;
	int dy = -mu_rog_iabs(y1 - y0);
	int sy = (y0 < y1) ? 1 : -1;
	int err = dx + dy;
	int count = 0;

	while (1) {
		++count;
		if (visit && !visit(x0, y0, user)) break;
		if (x0 == x1 && y0 == y1) break;
		{
			int e2 = 2 * err;
			if (e2 >= dy) {
				err += dy;
				x0 += sx;
			}
			if (e2 <= dx) {
				err += dx;
				y0 += sy;
			}
		}
	}
	return count;
}

typedef struct mu_rog_los_ctx {
	mu_rog_cell_blocked_fn blocked;
	void* user;
	int visible;
	int skip_first;
} mu_rog_los_ctx;

MU_INLINE int mu_rog_los_visit_cb(int x, int y, void* user)
{
	mu_rog_los_ctx* c = (mu_rog_los_ctx*)user;
	if (c->skip_first) {
		c->skip_first = 0;
		return 1;
	}
	if (c->blocked && c->blocked(x, y, c->user)) {
		c->visible = 0;
		return 0;
	}
	return 1;
}

MU_INLINE int mu_rog_has_line_of_sight(int x0,
											int y0,
											int x1,
											int y1,
											mu_rog_cell_blocked_fn blocked,
											void* user)
{
	mu_rog_los_ctx c;
	c.blocked = blocked;
	c.user = user;
	c.visible = 1;
	c.skip_first = 1;
	mu_rog_line_bresenham(x0, y0, x1, y1, mu_rog_los_visit_cb, &c);
	return c.visible;
}

/* ------------------------------------------------------------------------- */
/* FOV via perimeter ray casting                                             */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_fov_ctx {
	int w;
	int h;
	uint8_t* visible;
	mu_rog_cell_blocked_fn blocked;
	void* user;
	int ox;
	int oy;
	int radius;
	int stop_after_block;
} mu_rog_fov_ctx;

MU_INLINE int mu_rog_fov_visit(int x, int y, void* user)
{
	mu_rog_fov_ctx* c = (mu_rog_fov_ctx*)user;
	if (!mu_rog_in_bounds_i(x, y, c->w, c->h)) return 0;

	{
		int dx = x - c->ox;
		int dy = y - c->oy;
		if (mu_rog_chebyshev(dx, dy) > c->radius) return 0;
	}

	c->visible[mu_rog_grid_index(x, y, c->w)] = 1;
	if (c->stop_after_block && c->blocked && c->blocked(x, y, c->user)) {
		return 0;
	}
	return 1;
}

/*
   Raycast FOV:
	 - mark origin visible
	 - shoot rays to square perimeter at distance radius
	 - stop each ray when blocked

   This is simple and robust for many tile games.
*/
MU_INLINE void mu_rog_fov_raycast(int w,
									  int h,
									  int ox,
									  int oy,
									  int radius,
									  mu_rog_cell_blocked_fn blocked,
									  void* user,
									  uint8_t* out_visible)
{
	int x, y;
	mu_rog_fov_ctx c;
	MU_MEMSET(out_visible, 0, (size_t)w * (size_t)h * sizeof(uint8_t));
	if (!mu_rog_in_bounds_i(ox, oy, w, h)) return;

	c.w = w;
	c.h = h;
	c.visible = out_visible;
	c.blocked = blocked;
	c.user = user;
	c.ox = ox;
	c.oy = oy;
	c.radius = radius;
	c.stop_after_block = 1;

	out_visible[mu_rog_grid_index(ox, oy, w)] = 1;

	for (x = ox - radius; x <= ox + radius; ++x) {
		mu_rog_line_bresenham(ox, oy, x, oy - radius, mu_rog_fov_visit, &c);
		mu_rog_line_bresenham(ox, oy, x, oy + radius, mu_rog_fov_visit, &c);
	}
	for (y = oy - radius + 1; y <= oy + radius - 1; ++y) {
		mu_rog_line_bresenham(ox, oy, ox - radius, y, mu_rog_fov_visit, &c);
		mu_rog_line_bresenham(ox, oy, ox + radius, y, mu_rog_fov_visit, &c);
	}
}

/* ------------------------------------------------------------------------- */
/* BFS and Dijkstra maps                                                     */
/* ------------------------------------------------------------------------- */

MU_INLINE void mu_rog_bfs_distance4(int w,
										int h,
										const mu_rog_ivec2* starts,
										int start_count,
										mu_rog_cell_blocked_fn blocked,
										void* user,
										int16_t* out_dist,
										int* queue)
{
	int i;
	int qh = 0;
	int qt = 0;
	int n = w * h;
	for (i = 0; i < n; ++i) out_dist[i] = (int16_t)-1;

	for (i = 0; i < start_count; ++i) {
		int sx = starts[i].x;
		int sy = starts[i].y;
		if (!mu_rog_in_bounds_i(sx, sy, w, h)) continue;
		if (blocked && blocked(sx, sy, user)) continue;
		{
			int idx = mu_rog_grid_index(sx, sy, w);
			if (out_dist[idx] != -1) continue;
			out_dist[idx] = 0;
			queue[qt++] = idx;
		}
	}

	while (qh < qt) {
		int cur = queue[qh++];
		int cx = cur % w;
		int cy = cur / w;
		int16_t cd = out_dist[cur];
		static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
		int d;
		for (d = 0; d < 4; ++d) {
			int nx = cx + dirs[d][0];
			int ny = cy + dirs[d][1];
			int nidx;
			if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
			if (blocked && blocked(nx, ny, user)) continue;
			nidx = mu_rog_grid_index(nx, ny, w);
			if (out_dist[nidx] != -1) continue;
			out_dist[nidx] = (int16_t)(cd + 1);
			queue[qt++] = nidx;
		}
	}
}

MU_INLINE void mu_rog_dijkstra_map8(int w,
										int h,
										const mu_rog_ivec2* goals,
										int goal_count,
										mu_rog_cell_blocked_fn blocked,
										void* user,
										int16_t* out_dist,
										int* queue)
{
	int i;
	int qh = 0;
	int qt = 0;
	int n = w * h;

	for (i = 0; i < n; ++i) out_dist[i] = (int16_t)32767;

	for (i = 0; i < goal_count; ++i) {
		int gx = goals[i].x;
		int gy = goals[i].y;
		if (!mu_rog_in_bounds_i(gx, gy, w, h)) continue;
		if (blocked && blocked(gx, gy, user)) continue;
		{
			int idx = mu_rog_grid_index(gx, gy, w);
			out_dist[idx] = 0;
			queue[qt++] = idx;
		}
	}

	while (qh < qt) {
		int cur = queue[qh++];
		int cx = cur % w;
		int cy = cur / w;
		int16_t base = out_dist[cur];
		static const int dirs[8][2] = {
			{1,0},{-1,0},{0,1},{0,-1},
			{1,1},{-1,1},{1,-1},{-1,-1}
		};
		int d;

		for (d = 0; d < 8; ++d) {
			int nx = cx + dirs[d][0];
			int ny = cy + dirs[d][1];
			int step = (d < 4) ? 10 : 14; /* integer approx of 1 and sqrt(2). */
			int16_t nd;
			int nidx;

			if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
			if (blocked && blocked(nx, ny, user)) continue;
			nidx = mu_rog_grid_index(nx, ny, w);
			nd = (int16_t)(base + step);
			if (nd < out_dist[nidx]) {
				out_dist[nidx] = nd;
				queue[qt++] = nidx;
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* A* pathfinding                                                            */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_astar_node {
	float g;
	int came_from;
	uint8_t closed;
} mu_rog_astar_node;

typedef struct mu_rog_astar_heap_entry {
	int idx;
	float f;
} mu_rog_astar_heap_entry;

MU_INLINE void mu_rog_astar_heap_push(mu_rog_astar_heap_entry* heap,
										  int* heap_count,
										  mu_rog_astar_heap_entry e)
{
	int i = (*heap_count)++;
	heap[i] = e;
	while (i > 0) {
		int p = (i - 1) / 2;
		if (heap[p].f <= heap[i].f) break;
		{
			mu_rog_astar_heap_entry t = heap[p];
			heap[p] = heap[i];
			heap[i] = t;
		}
		i = p;
	}
}

MU_INLINE mu_rog_astar_heap_entry mu_rog_astar_heap_pop(mu_rog_astar_heap_entry* heap,
															 int* heap_count)
{
	mu_rog_astar_heap_entry out = heap[0];
	int n = --(*heap_count);
	heap[0] = heap[n];

	{
		int i = 0;
		for (;;) {
			int l = 2 * i + 1;
			int r = 2 * i + 2;
			int m = i;
			if (l < n && heap[l].f < heap[m].f) m = l;
			if (r < n && heap[r].f < heap[m].f) m = r;
			if (m == i) break;
			{
				mu_rog_astar_heap_entry t = heap[i];
				heap[i] = heap[m];
				heap[m] = t;
			}
			i = m;
		}
	}

	return out;
}

/*
   Reconstruct path from goal by following came_from links.
   Writes path from start->goal in out_path, returns count.
*/
MU_INLINE int mu_rog_astar_reconstruct(const mu_rog_astar_node* nodes,
										   int start_idx,
										   int goal_idx,
										   int w,
										   mu_rog_ivec2* out_path,
										   int out_path_cap)
{
	int temp_count = 0;
	int cur = goal_idx;
	while (cur >= 0 && temp_count < out_path_cap) {
		out_path[temp_count].x = cur % w;
		out_path[temp_count].y = cur / w;
		++temp_count;
		if (cur == start_idx) break;
		cur = nodes[cur].came_from;
	}

	if (temp_count == 0 || out_path[temp_count - 1].x != (start_idx % w) || out_path[temp_count - 1].y != (start_idx / w)) {
		return 0;
	}

	{
		int i;
		for (i = 0; i < temp_count / 2; ++i) {
			mu_rog_ivec2 t = out_path[i];
			out_path[i] = out_path[temp_count - 1 - i];
			out_path[temp_count - 1 - i] = t;
		}
	}
	return temp_count;
}

/*
   A* on tile map.

   Returns number of points in out_path (0 if no path).
   Neighbor policy:
	 allow_diag=0 -> 4-neighbors
	 allow_diag=1 -> 8-neighbors
*/
MU_INLINE int mu_rog_astar_find_path(int w,
										 int h,
										 mu_rog_ivec2 start,
										 mu_rog_ivec2 goal,
										 int allow_diag,
										 mu_rog_cell_blocked_fn blocked,
										 mu_rog_move_cost_fn cost_fn,
										 void* user,
										 mu_rog_ivec2* out_path,
										 int out_path_cap)
{
	int n = w * h;
	int start_idx;
	int goal_idx;
	mu_rog_astar_node* nodes;
	mu_rog_astar_heap_entry* heap;
	int heap_count = 0;
	int found = 0;

	if (!mu_rog_in_bounds_i(start.x, start.y, w, h)) return 0;
	if (!mu_rog_in_bounds_i(goal.x, goal.y, w, h)) return 0;
	if (blocked && blocked(start.x, start.y, user)) return 0;
	if (blocked && blocked(goal.x, goal.y, user)) return 0;

	start_idx = mu_rog_grid_index(start.x, start.y, w);
	goal_idx = mu_rog_grid_index(goal.x, goal.y, w);

	nodes = (mu_rog_astar_node*)MU_MALLOC((size_t)n * sizeof(mu_rog_astar_node));
	heap = (mu_rog_astar_heap_entry*)MU_MALLOC((size_t)n * sizeof(mu_rog_astar_heap_entry));
	if (!nodes || !heap) {
		MU_FREE(nodes);
		MU_FREE(heap);
		return 0;
	}

	{
		int i;
		for (i = 0; i < n; ++i) {
			nodes[i].g = FLT_MAX;
			nodes[i].came_from = -1;
			nodes[i].closed = 0;
		}
	}

	nodes[start_idx].g = 0.0f;
	{
		float h0 = allow_diag
			? mu_rog_octile_heuristic(goal.x - start.x, goal.y - start.y)
			: (float)mu_rog_manhattan(goal.x - start.x, goal.y - start.y);
		mu_rog_astar_heap_entry e;
		e.idx = start_idx;
		e.f = h0;
		mu_rog_astar_heap_push(heap, &heap_count, e);
	}

	while (heap_count > 0) {
		mu_rog_astar_heap_entry e = mu_rog_astar_heap_pop(heap, &heap_count);
		int cur = e.idx;
		int cx;
		int cy;
		int d;
		static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
		static const int dirs8[8][2] = {
			{1,0},{-1,0},{0,1},{0,-1},
			{1,1},{-1,1},{1,-1},{-1,-1}
		};
		const int (*dirs)[2] = allow_diag ? dirs8 : dirs4;
		int dir_count = allow_diag ? 8 : 4;

		if (nodes[cur].closed) continue;
		nodes[cur].closed = 1;

		if (cur == goal_idx) {
			found = 1;
			break;
		}

		cx = cur % w;
		cy = cur / w;

		for (d = 0; d < dir_count; ++d) {
			int nx = cx + dirs[d][0];
			int ny = cy + dirs[d][1];
			int nidx;
			float step_cost;
			float ng;
			float hcost;
			mu_rog_astar_heap_entry ne;

			if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
			if (blocked && blocked(nx, ny, user)) continue;
			nidx = mu_rog_grid_index(nx, ny, w);
			if (nodes[nidx].closed) continue;

			if (cost_fn) {
				step_cost = cost_fn(cx, cy, nx, ny, user);
			} else {
				step_cost = (dirs[d][0] != 0 && dirs[d][1] != 0) ? 1.41421356237f : 1.0f;
			}
			if (step_cost < 0.0f) continue;

			ng = nodes[cur].g + step_cost;
			if (ng >= nodes[nidx].g) continue;

			nodes[nidx].g = ng;
			nodes[nidx].came_from = cur;

			hcost = allow_diag
				? mu_rog_octile_heuristic(goal.x - nx, goal.y - ny)
				: (float)mu_rog_manhattan(goal.x - nx, goal.y - ny);

			ne.idx = nidx;
			ne.f = ng + hcost;
			mu_rog_astar_heap_push(heap, &heap_count, ne);
		}
	}

	{
		int out = 0;
		if (found) {
			out = mu_rog_astar_reconstruct(nodes, start_idx, goal_idx, w, out_path, out_path_cap);
		}
		MU_FREE(nodes);
		MU_FREE(heap);
		return out;
	}
}

/* ------------------------------------------------------------------------- */
/* Turn scheduler (min-heap by tick)                                         */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_sched_event {
	uint64_t tick;
	uint32_t actor_id;
	uint32_t serial;
} mu_rog_sched_event;

typedef struct mu_rog_scheduler {
	mu_rog_sched_event* heap;
	int count;
	int cap;
	uint32_t serial_counter;
} mu_rog_scheduler;

MU_INLINE int mu_rog_sched_less(mu_rog_sched_event a, mu_rog_sched_event b)
{
	if (a.tick != b.tick) return a.tick < b.tick;
	return a.serial < b.serial;
}

MU_INLINE void mu_rog_scheduler_init(mu_rog_scheduler* s)
{
	MU_MEMSET(s, 0, sizeof(*s));
}

MU_INLINE void mu_rog_scheduler_free(mu_rog_scheduler* s)
{
	MU_FREE(s->heap);
	MU_MEMSET(s, 0, sizeof(*s));
}

MU_INLINE int mu_rog_scheduler_reserve(mu_rog_scheduler* s, int cap)
{
	if (cap <= s->cap) return 1;
	{
		int new_cap = (s->cap > 0) ? s->cap : 32;
		mu_rog_sched_event* p;
		while (new_cap < cap) new_cap *= 2;
		p = (mu_rog_sched_event*)MU_REALLOC(s->heap, (size_t)new_cap * sizeof(mu_rog_sched_event));
		if (!p) return 0;
		s->heap = p;
		s->cap = new_cap;
		return 1;
	}
}

MU_INLINE int mu_rog_scheduler_push(mu_rog_scheduler* s, uint32_t actor_id, uint64_t tick)
{
	int i;
	mu_rog_sched_event e;
	if (!mu_rog_scheduler_reserve(s, s->count + 1)) return 0;

	e.tick = tick;
	e.actor_id = actor_id;
	e.serial = s->serial_counter++;

	i = s->count++;
	s->heap[i] = e;
	while (i > 0) {
		int p = (i - 1) / 2;
		if (mu_rog_sched_less(s->heap[p], s->heap[i])) break;
		{
			mu_rog_sched_event t = s->heap[p];
			s->heap[p] = s->heap[i];
			s->heap[i] = t;
		}
		i = p;
	}
	return 1;
}

MU_INLINE int mu_rog_scheduler_peek(const mu_rog_scheduler* s, mu_rog_sched_event* out)
{
	if (s->count <= 0) return 0;
	if (out) *out = s->heap[0];
	return 1;
}

MU_INLINE int mu_rog_scheduler_pop(mu_rog_scheduler* s, mu_rog_sched_event* out)
{
	int n;
	int i;
	if (s->count <= 0) return 0;
	if (out) *out = s->heap[0];

	n = --s->count;
	if (n <= 0) return 1;
	s->heap[0] = s->heap[n];

	i = 0;
	for (;;) {
		int l = 2 * i + 1;
		int r = 2 * i + 2;
		int m = i;
		if (l < n && mu_rog_sched_less(s->heap[l], s->heap[m])) m = l;
		if (r < n && mu_rog_sched_less(s->heap[r], s->heap[m])) m = r;
		if (m == i) break;
		{
			mu_rog_sched_event t = s->heap[i];
			s->heap[i] = s->heap[m];
			s->heap[m] = t;
		}
		i = m;
	}
	return 1;
}

/* ------------------------------------------------------------------------- */
/* Example usage (documentation-only, no symbols generated)                 */
/* ------------------------------------------------------------------------- */

/*
	Example skeleton:

	  // 1) Build map
	  mu_rog_rng rng;
	  mu_rog_rng_seed(&rng, 12345u);

	  uint8_t tiles[W * H];
	  mu_rog_grid_u8 map = {W, H, tiles};

	  mu_rog_rect rooms[128];
	  mu_rog_roomgen_params p = {...};
	  int room_count = mu_rog_generate_rooms_and_corridors(&rng, map, &p, rooms, 128);

	  // 2) FOV
	  uint8_t visible[W * H];
	  mu_rog_fov_raycast(W, H, player_x, player_y, 12, is_blocked_cb, &state, visible);

	  // 3) Path
	  mu_rog_ivec2 path[512];
	  int path_len = mu_rog_astar_find_path(
		  W, H,
		  (mu_rog_ivec2){player_x, player_y},
		  (mu_rog_ivec2){target_x, target_y},
		  1, is_blocked_cb, NULL, &state,
		  path, 512
	  );

	  // 4) Scheduler
	  mu_rog_scheduler sched;
	  mu_rog_scheduler_init(&sched);
	  mu_rog_scheduler_push(&sched, player_id, 100);
	  mu_rog_scheduler_push(&sched, goblin_id, 105);
*/

#ifdef __cplusplus
}
#endif

#endif /* MU_ROGUELIKE_H */
