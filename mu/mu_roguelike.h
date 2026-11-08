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
	4) No external math dependency: all vector math is local, so the header
	   compiles with nothing but the C standard library.

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
	float dx = x1 - x0;
	float dy = y1 - y0;
	return sqrtf(dx * dx + dy * dy);
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

MU_INLINE int mu_rog_los_one_way(int x0,
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

/*
   Line of sight, made symmetric by construction.

   Bresenham is not symmetric: the cells it visits from A to B are not the cells
   it visits from B to A, so "A can see B" and "B can see A" disagree for about
   1 in 8 of the ties on a diagonal. For a roguelike that is a rules bug - the
   player sees a monster that cannot see the player, or the reverse - and it is
   the same bug class as an asymmetric FOV.

   The fix is not a better line walker, it is to stop trusting one: a pair is
   visible if EITHER direction is clear, which makes the relation symmetric
   whatever the line walker does (both directions are evaluated with the same
   code, so the disjunction is commutative). The cost is two line walks instead
   of one, i.e. O(n) either way, and it is permissive: a corner shot that one
   walker allows is allowed.

   mu_rog_los_one_way is the raw single walk, for a caller that wants the old
   behaviour or is measuring the difference.
*/
MU_INLINE int mu_rog_has_line_of_sight(int x0,
									   int y0,
									   int x1,
									   int y1,
									   mu_rog_cell_blocked_fn blocked,
									   void* user)
{
	if (mu_rog_los_one_way(x0, y0, x1, y1, blocked, user)) return 1;
	return mu_rog_los_one_way(x1, y1, x0, y0, blocked, user);
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
/* FOV: symmetric recursive shadowcasting                                    */
/* ------------------------------------------------------------------------- */
/*
   The raycast above is the cheap classic and it has two defects that no amount
   of tuning fixes:

	 1) It is not symmetric. Raycasting from A and from B walk different cells,
		so A can see B while B cannot see A. Measured on a 25%-rubble 20x20 map:
		13-14% of visible open pairs disagree (perf_test/PERF_ROGUELIKE.md).

	 2) It has artifacts. Rays only pass through cells the line walker happens
		to step on, so a wall can show a false shadow one cell wide, and a
		one-cell pillar can leave a lit column behind it.

   Recursive shadowcasting fixes both, and the symmetric variant fixes the
   symmetry exactly (0 disagreements on every map in the gate, not "fewer"):

	 - Split the view into 4 quadrants (N/S/E/W). Inside a quadrant a row is a
	   horizontal line of cells at a fixed depth; a sector is the angular range
	   [start_slope, end_slope] still visible.
	 - Walk a row from the sector's left column to its right column. Opaque
	   cells shorten the sector; an opaque-to-clear transition starts a tighter
	   sector in the next row (recursion).
	 - The symmetric trick: a cell is marked when its CENTER is inside the
	   sector (a plain float comparison), instead of marking every cell the
	   rounded sector touches. That is what removes the asymmetry, at the price
	   of letting sight pass through a gap that is exactly one diagonal corner.

   All of the slope arithmetic here is integer. A slope is a fraction num/den
   with den > 0; "is column c inside slope s at depth d" is the cross-multiplied
   comparison c*den >= d*num, and the row bounds are the same comparison rounded
   by integer division. There is no float in this file's FOV path: the output is
   bit-identical to the float formulation (verified over 200 random maps in the
   gate) and it cannot round differently on an FPU-less target.

   Cost: one pass over the cells inside the diamond, O(r^2) with a small
   constant, against the raycast's 8r line walks of up to r steps each.
*/

typedef struct mu_rog_slope {
	int num;
	int den;   /* always > 0 */
} mu_rog_slope;

MU_INLINE int mu_rog__floor_div(int a, int b)   /* b > 0, floors toward -inf */
{
	int q = a / b;
	if ((a % b) != 0 && a < 0) --q;
	return q;
}

MU_INLINE int mu_rog__ceil_div(int a, int b)    /* b > 0 */
{
	return -mu_rog__floor_div(-a, b);
}

/* floor(d * s + 1/2) */
MU_INLINE int mu_rog__slope_ties_up(int d, mu_rog_slope s)
{
	return mu_rog__floor_div(2 * d * s.num + s.den, 2 * s.den);
}

/* ceil(d * s - 1/2) */
MU_INLINE int mu_rog__slope_ties_down(int d, mu_rog_slope s)
{
	return mu_rog__ceil_div(2 * d * s.num - s.den, 2 * s.den);
}

typedef struct mu_rog_fov_shadow_ctx {
	int w;
	int h;
	int ox;
	int oy;
	int radius;          /* <= 0 means limited only by the map */
	uint8_t* visible;
	mu_rog_cell_blocked_fn opaque;
	void* user;
	long marked;
} mu_rog_fov_shadow_ctx;

/*
   One sector of one quadrant of one row.

   Quadrant -> (dx, dy) for local (col, depth):
	 0: ( col, -depth)   north
	 1: ( col,  depth)   south
	 2: ( depth,  col)   east
	 3: (-depth,  col)   west
*/
MU_INLINE void mu_rog__fov_shadow_rec(mu_rog_fov_shadow_ctx* c,
									  int quadrant,
									  int depth,
									  mu_rog_slope start,
									  mu_rog_slope end)
{
	int min_col;
	int max_col;
	int col;
	int prev_blocked = -1;

	if (c->radius > 0 && depth > c->radius) return;
	if (depth <= 0) return;
	min_col = mu_rog__slope_ties_up(depth, start);
	max_col = mu_rog__slope_ties_down(depth, end);
	if (min_col > max_col) return;

	for (col = min_col; col <= max_col; ++col) {
		int dx;
		int dy;
		int x;
		int y;
		int blocked;

		switch (quadrant) {
		case 0:  dx = col;    dy = -depth; break;
		case 1:  dx = col;    dy = depth;  break;
		case 2:  dx = depth;  dy = col;    break;
		default: dx = -depth; dy = col;    break;
		}
		x = c->ox + dx;
		y = c->oy + dy;
		blocked = (x < 0 || y < 0 || x >= c->w || y >= c->h)
			? 1
			: (c->opaque ? (c->opaque(x, y, c->user) ? 1 : 0) : 0);

		if (x >= 0 && y >= 0 && x < c->w && y < c->h) {
			/* the symmetric rule: the cell's center must be inside the sector */
			if (blocked ||
				(col * start.den >= depth * start.num && col * end.den <= depth * end.num)) {
				uint8_t* cell = &c->visible[mu_rog_grid_index(x, y, c->w)];
				if (!*cell) { *cell = 1; ++c->marked; }
			}
		}

		if (prev_blocked == 1 && !blocked) {
			/* left edge of this cell is the right edge of the wall just passed */
			start.num = 2 * col - 1;
			start.den = 2 * depth;
		}
		if (prev_blocked == 0 && blocked) {
			mu_rog_slope tighter;
			tighter.num = 2 * col - 1;
			tighter.den = 2 * depth;
			mu_rog__fov_shadow_rec(c, quadrant, depth + 1, start, tighter);
		}
		prev_blocked = blocked;
	}

	if (prev_blocked == 1) return;
	mu_rog__fov_shadow_rec(c, quadrant, depth + 1, start, end);
}

/*
   Shadowcasting FOV.

   out_visible must be w*h bytes; it is cleared. The origin is always marked.
   If the origin itself is opaque, nothing else is marked (a degenerate case the
   raycast also degenerates on, kept for consistency).

   Opaque cells are marked visible when they bound a visible sector: you can see
   the wall you are looking at, which is what you want to draw.
*/
MU_INLINE int mu_rog_fov_shadowcast(int w,
									int h,
									int ox,
									int oy,
									int radius,
									mu_rog_cell_blocked_fn opaque,
									void* user,
									uint8_t* out_visible)
{
	mu_rog_fov_shadow_ctx c;
	int q;

	MU_MEMSET(out_visible, 0, (size_t)w * (size_t)h * sizeof(uint8_t));
	if (!mu_rog_in_bounds_i(ox, oy, w, h)) return 0;

	c.w = w;
	c.h = h;
	c.ox = ox;
	c.oy = oy;
	c.radius = radius;
	c.visible = out_visible;
	c.opaque = opaque;
	c.user = user;
	c.marked = 0;

	out_visible[mu_rog_grid_index(ox, oy, w)] = 1;
	if (opaque && opaque(ox, oy, user)) return 1;

	for (q = 0; q < 4; ++q) {
		mu_rog_slope start;
		mu_rog_slope end;
		start.num = -1; start.den = 1;
		end.num = 1;    end.den = 1;
		mu_rog__fov_shadow_rec(&c, q, 1, start, end);
	}
	return (int)c.marked + 1;
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

/* ------------------------------------------------------------------------- */
/* Dijkstra map: bucket queue (Dial), terrain costs, corner rule             */
/* ------------------------------------------------------------------------- */
/*
   The previous version of this function was a FIFO queue with 10/14 weights and
   a "re-push when improved" rule. That is not Dijkstra, it is SPFA: correct, but
   a cell can be expanded several times, and the scratch buffer it was documented
   to take (w*h ints) is smaller than the number of entries it can push, so a
   caller that did what the docs said got a buffer overflow.

   What is here instead:

	 - Dial's bucket queue. With integer step costs bounded by max_step there are
	   at most max_step+1 distinct distances "in flight", so a ring of max_step+1
	   FIFO buckets replaces the heap and every cell is expanded exactly once.
	   O(n + m) with no log factor, and the result is exact.
	 - Real terrain costs. cost_fn returns a MULTIPLIER for entering a cell
	   (1.0 = normal floor, 2.0 = mud, 0.5 = road); the step cost is the base
	   step (10 orthogonal, 14 diagonal, i.e. 1.0 and sqrt(2) in tenths) times
	   that multiplier. <= 0 means impassable.
	 - A corner rule. With allow_diag, cut_corners = 0 forbids a diagonal step
	   between two blocked orthogonal neighbours, which is what you want for
	   monster movement: otherwise a corridor corner is a legal diagonal
	   shortcut through solid wall.

   Scratch: 2n + max_step + 1 ints (a doubly linked list per bucket, so a cell
   that is improved while queued is unlinked from its old bucket in O(1) instead
   of being left in it - an intrusive single-link list cannot do that, because a
   cell can only be in one list at a time and relinking it silently truncates
   the bucket it came from). mu_rog_dijkstra_scratch_ints returns the size.
*/

#define MU_ROG_STEP_ORTHO 10
#define MU_ROG_STEP_DIAG  14

typedef struct mu_rog_search_opts {
	int allow_diag;    /* 0 = 4-neighbour, 1 = 8-neighbour (default 1) */
	int cut_corners;   /* 1 = diagonals may squeeze past a wall corner  */
	int max_step;      /* largest integer step cost the queue may see   */
} mu_rog_search_opts;

MU_INLINE mu_rog_search_opts mu_rog_search_opts_default(void)
{
	mu_rog_search_opts o;
	o.allow_diag = 1;
	o.cut_corners = 0;
	o.max_step = MU_ROG_STEP_DIAG;
	return o;
}

MU_INLINE int mu_rog_dijkstra_scratch_ints(int w, int h, const mu_rog_search_opts* opts)
{
	int max_step = (opts && opts->max_step > 0) ? opts->max_step : MU_ROG_STEP_DIAG;
	if (w <= 0 || h <= 0) return 0;
	return 2 * w * h + max_step + 1;
}

/* integer step cost for leaving (fx,fy) and entering (tx,ty), 0 = impassable */
MU_INLINE int mu_rog__step_cost(int fx, int fy, int tx, int ty,
								mu_rog_move_cost_fn cost_fn,
								void* user,
								const mu_rog_search_opts* opts,
								int* out_ok)
{
	int diagonal = (fx != tx) && (fy != ty);
	float base = diagonal ? (float)MU_ROG_STEP_DIAG : (float)MU_ROG_STEP_ORTHO;
	float mult = 1.0f;
	int cost;

	*out_ok = 1;
	if (cost_fn) {
		mult = cost_fn(fx, fy, tx, ty, user);
		if (!(mult > 0.0f)) { *out_ok = 0; return 0; }
	}
	cost = (int)(base * mult + 0.5f);
	if (cost <= 0) { *out_ok = 0; return 0; }
	if (opts && opts->max_step > 0 && cost > opts->max_step) cost = opts->max_step;
	return cost;
}

/* Can we step diagonally from (fx,fy) to (tx,ty) under the corner rule? */
MU_INLINE int mu_rog__diag_allowed(int fx, int fy, int tx, int ty,
								   mu_rog_cell_blocked_fn blocked,
								   void* user)
{
	if (!blocked) return 1;
	/* the two orthogonal neighbours the diagonal passes between */
	if (blocked(tx, fy, user) && blocked(fx, ty, user)) return 0;
	return 1;
}

MU_INLINE void mu_rog_dijkstra_map8_ex(int w,
									   int h,
									   const mu_rog_ivec2* goals,
									   int goal_count,
									   mu_rog_cell_blocked_fn blocked,
									   mu_rog_move_cost_fn cost_fn,
									   void* user,
									   const mu_rog_search_opts* opts_in,
									   int16_t* out_dist,
									   int* scratch)
{
	mu_rog_search_opts opts = opts_in ? *opts_in : mu_rog_search_opts_default();
	int max_step;
	int ring;
	int n = w * h;
	int* next;
	int* prev;
	int* head;
	int i;
	int pending = 0;
	int d = 0;      /* current distance ring; goals are pushed at 0 */
	int bucket = 0;
	int step_of[8];
	static const int dirs8[8][2] = {
		{1,0},{-1,0},{0,1},{0,-1},
		{1,1},{-1,1},{1,-1},{-1,-1}
	};

	if (!out_dist || !scratch || n <= 0) return;
	max_step = (opts.max_step > 0) ? opts.max_step : MU_ROG_STEP_DIAG;
	ring = max_step + 1;
	next = scratch;
	prev = scratch + n;
	head = scratch + 2 * n;

	for (i = 0; i < n; ++i) out_dist[i] = (int16_t)32767;
	for (i = 0; i < ring; ++i) head[i] = -1;
	/* the step costs do not depend on where we are on a uniform map: hoist them
	   out of the inner loop instead of rebuilding them per edge */
	for (i = 0; i < 8; ++i) {
		int diagonal = (dirs8[i][0] != 0 && dirs8[i][1] != 0);
		step_of[i] = diagonal ? MU_ROG_STEP_DIAG : MU_ROG_STEP_ORTHO;
	}

#define MU_ROG_DQ_PUSH(idx_, dist_) \
	do { \
		int off_ = (dist_) - d; \
		int b_ = bucket + off_; \
		while (b_ >= ring) b_ -= ring; \
		prev[idx_] = -1; \
		next[idx_] = head[b_]; \
		if (head[b_] >= 0) prev[head[b_]] = (idx_); \
		head[b_] = (idx_); \
		out_dist[idx_] = (int16_t)(dist_); \
		++pending; \
	} while (0)

#define MU_ROG_DQ_UNLINK(idx_, dist_) \
	do { \
		int b_ = (dist_) % ring; \
		if (prev[idx_] >= 0) next[prev[idx_]] = next[idx_]; \
		else head[b_] = next[idx_]; \
		if (next[idx_] >= 0) prev[next[idx_]] = prev[idx_]; \
		prev[idx_] = -2; \
		--pending; \
	} while (0)

	for (i = 0; i < goal_count; ++i) {
		int gx = goals[i].x;
		int gy = goals[i].y;
		int idx;
		if (!mu_rog_in_bounds_i(gx, gy, w, h)) continue;
		if (blocked && blocked(gx, gy, user)) continue;
		idx = mu_rog_grid_index(gx, gy, w);
		if (out_dist[idx] == 0) continue;
		MU_ROG_DQ_PUSH(idx, 0);
	}

	for (d = 0, bucket = 0; pending > 0; ++d) {
		while (head[bucket] != -1) {
			int cur = head[bucket];
			int cx;
			int cy;
			int dir_count;
			int k;

			MU_ROG_DQ_UNLINK(cur, d);
			if (out_dist[cur] != (int16_t)d) continue;
			cx = cur % w;
			cy = cur / w;
			dir_count = opts.allow_diag ? 8 : 4;

			for (k = 0; k < dir_count; ++k) {
				int nx = cx + dirs8[k][0];
				int ny = cy + dirs8[k][1];
				int nidx;
				int ok;
				int step;
				int nd;

				if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
				if (blocked && blocked(nx, ny, user)) continue;
				if (dirs8[k][0] != 0 && dirs8[k][1] != 0) {
					if (!opts.cut_corners && !mu_rog__diag_allowed(cx, cy, nx, ny, blocked, user))
						continue;
				}
				if (cost_fn) {
					step = mu_rog__step_cost(cx, cy, nx, ny, cost_fn, user, &opts, &ok);
					if (!ok) continue;
				} else {
					step = step_of[k];
				}
				nd = (int)out_dist[cur] + step;
				if (nd >= 32767) continue;
				nidx = mu_rog_grid_index(nx, ny, w);
				if (nd < (int)out_dist[nidx]) {
					if (out_dist[nidx] != (int16_t)32767 && prev[nidx] != -2)
						MU_ROG_DQ_UNLINK(nidx, (int)out_dist[nidx]);
					MU_ROG_DQ_PUSH(nidx, nd);
				}
			}
		}
		/* advance the ring without a division: every bucket we insert into is at
		   most max_step ahead of the one we are draining */
		++bucket;
		if (bucket == ring) bucket = 0;
	}
	(void)ring;

#undef MU_ROG_DQ_PUSH
#undef MU_ROG_DQ_UNLINK
}

/*
   The classic call, kept: uniform cost, 8-neighbour, no corner cutting.

   NOTE the scratch requirement changed: `scratch` must hold
   mu_rog_dijkstra_scratch_ints(w, h, NULL) ints, not w*h. The old requirement
   was too small for the old implementation anyway (see the comment above).
*/
MU_INLINE void mu_rog_dijkstra_map8(int w,
									int h,
									const mu_rog_ivec2* goals,
									int goal_count,
									mu_rog_cell_blocked_fn blocked,
									void* user,
									int16_t* out_dist,
									int* scratch)
{
	mu_rog_dijkstra_map8_ex(w, h, goals, goal_count, blocked, NULL, user, NULL, out_dist, scratch);
}

/* One step downhill: the neighbour of (x,y) with the lowest distance.
   Returns 0 when there is no strictly lower neighbour (local minimum / wall). */
MU_INLINE int mu_rog_dijkstra_descend(int w,
									  int h,
									  const int16_t* dist,
									  int x,
									  int y,
									  int allow_diag,
									  mu_rog_ivec2* out_next)
{
	static const int dirs8[8][2] = {
		{1,0},{-1,0},{0,1},{0,-1},
		{1,1},{-1,1},{1,-1},{-1,-1}
	};
	int best = -1;
	int best_d = 32767;
	int k;
	int dir_count = allow_diag ? 8 : 4;
	int cur_d;

	if (!dist || !mu_rog_in_bounds_i(x, y, w, h)) return 0;
	cur_d = dist[mu_rog_grid_index(x, y, w)];

	for (k = 0; k < dir_count; ++k) {
		int nx = x + dirs8[k][0];
		int ny = y + dirs8[k][1];
		int nd;
		if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
		nd = dist[mu_rog_grid_index(nx, ny, w)];
		if (nd < best_d) { best_d = nd; best = k; }
	}
	if (best < 0 || best_d >= cur_d) return 0;
	out_next->x = x + dirs8[best][0];
	out_next->y = y + dirs8[best][1];
	return 1;
}

/* ------------------------------------------------------------------------- */
/* A* pathfinding                                                            */
/* ------------------------------------------------------------------------- */

typedef struct mu_rog_astar_node {
	float g;
	int came_from;
	int heap_pos;     /* slot in the open heap, -1 popped, -2 never opened */
	uint32_t stamp;   /* generation this node's fields belong to */
	uint8_t closed;
} mu_rog_astar_node;

typedef struct mu_rog_astar_heap_entry {
	int idx;
	float f;
} mu_rog_astar_heap_entry;

MU_INLINE void mu_rog_astar_node_reset(mu_rog_astar_node* n)
{
	n->g = FLT_MAX;
	n->came_from = -1;
	n->heap_pos = -2;
	n->closed = 0;
	n->stamp = 0;
}

/*
   Lazily initialise one node.

   The previous version cleared all w*h nodes at the top of every search. On a
   129x97 map that is 12,513 nodes of stores per path - about 19 us of pure setup
   for a search that usually touches a fifth of the map, i.e. most of the time
   was spent resetting cells nobody was going to look at. Stamping makes the
   clear cost proportional to the cells the search actually reaches: a node whose
   stamp is not the current generation is treated as untouched and initialised on
   first access.
*/
MU_INLINE mu_rog_astar_node* mu_rog_astar_node_touch(mu_rog_astar_node* n, uint32_t stamp)
{
	if (n->stamp != stamp) {
		n->stamp = stamp;
		n->g = FLT_MAX;
		n->came_from = -1;
		n->heap_pos = -2;
		n->closed = 0;
	}
	return n;
}

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
/*
   ---------------------------------------------------------------------------
   Indexed binary heap: decrease-key instead of duplicate pushes
   ---------------------------------------------------------------------------

   The heap below keeps each node's slot in the node array (heap_pos), so
   improving an already-open node is an in-place O(log n) decrease-key rather
   than a duplicate push that the pop loop later throws away. On a 128x128 map
   with eight neighbours, the duplicate-push version can put an order of
   magnitude more entries through the heap than there are cells to settle.

   heap_pos: >= 0 in the open heap, -1 when popped, -2 when never opened.
*/
MU_INLINE void mu_rog_astar_heap_sift_up(mu_rog_astar_heap_entry* heap,
										 mu_rog_astar_node* nodes,
										 int i)
{
	while (i > 0) {
		int p = (i - 1) / 2;
		mu_rog_astar_heap_entry t;
		if (heap[p].f <= heap[i].f) break;
		t = heap[p];
		heap[p] = heap[i];
		heap[i] = t;
		nodes[heap[p].idx].heap_pos = p;
		nodes[heap[i].idx].heap_pos = i;
		i = p;
	}
}

MU_INLINE void mu_rog_astar_heap_sift_down(mu_rog_astar_heap_entry* heap,
										   mu_rog_astar_node* nodes,
										   int count,
										   int i)
{
	for (;;) {
		int l = 2 * i + 1;
		int r = 2 * i + 2;
		int m = i;
		mu_rog_astar_heap_entry t;
		if (l < count && heap[l].f < heap[m].f) m = l;
		if (r < count && heap[r].f < heap[m].f) m = r;
		if (m == i) break;
		t = heap[i];
		heap[i] = heap[m];
		heap[m] = t;
		nodes[heap[i].idx].heap_pos = i;
		nodes[heap[m].idx].heap_pos = m;
		i = m;
	}
}

/* insert or decrease-key idx to priority f */
MU_INLINE void mu_rog_astar_heap_update(mu_rog_astar_heap_entry* heap,
										int* heap_count,
										mu_rog_astar_node* nodes,
										int idx,
										float f)
{
	int i = nodes[idx].heap_pos;
	if (i < 0) {
		i = (*heap_count)++;
		heap[i].idx = idx;
		nodes[idx].heap_pos = i;
	}
	heap[i].f = f;
	mu_rog_astar_heap_sift_up(heap, nodes, i);
}

MU_INLINE mu_rog_astar_heap_entry mu_rog_astar_heap_take(mu_rog_astar_heap_entry* heap,
														 int* heap_count,
														 mu_rog_astar_node* nodes)
{
	mu_rog_astar_heap_entry out = heap[0];
	int n;
	nodes[out.idx].heap_pos = -1;
	n = --(*heap_count);
	if (n > 0) {
		heap[0] = heap[n];
		nodes[heap[0].idx].heap_pos = 0;
		mu_rog_astar_heap_sift_down(heap, nodes, n, 0);
	}
	return out;
}

/*
   ---------------------------------------------------------------------------
   A*: caller-owned scratch, corner rule, weighted heuristic
   ---------------------------------------------------------------------------

   Changes from the allocating version:

	 - No allocation. mu_rog_astar_find_path_ex takes the node array (w*h) and
	   the heap array (w*h) from the caller, so a monster can replan every turn
	   without touching the allocator.
	 - The corner rule is honoured (see mu_rog_search_opts).
	 - cost_fn is a multiplier for entering a cell (1.0 = floor), matching the
	   Dijkstra map, so the two searches agree on what a route costs.
	 - heuristic_weight > 1.0 is weighted A*: fewer expansions, a path up to
	   that factor longer. 1.0 keeps it exact. The bench measures both.
	 - out_expanded reports closed nodes, which is the number that actually
	   tracks cost on this kind of search - path length and node count barely
	   move between the two heap strategies; expansions do.
*/
MU_INLINE int mu_rog_astar_find_path_ex(int w,
										int h,
										mu_rog_ivec2 start,
										mu_rog_ivec2 goal,
										const mu_rog_search_opts* opts_in,
										mu_rog_cell_blocked_fn blocked,
										mu_rog_move_cost_fn cost_fn,
										void* user,
										float heuristic_weight,
										uint32_t stamp,
										mu_rog_astar_node* nodes,
										mu_rog_astar_heap_entry* heap,
										mu_rog_ivec2* out_path,
										int out_path_cap,
										float* out_cost,
										int* out_expanded)
{
	mu_rog_search_opts opts = opts_in ? *opts_in : mu_rog_search_opts_default();
	int n = w * h;
	int start_idx;
	int goal_idx;
	int heap_count = 0;
	int found = 0;
	int expanded = 0;
	static const int dirs8[8][2] = {
		{1,0},{-1,0},{0,1},{0,-1},
		{1,1},{-1,1},{1,-1},{-1,-1}
	};
	int dir_count;

	if (out_expanded) *out_expanded = 0;
	if (out_cost) *out_cost = 0.0f;
	if (!nodes || !heap || n <= 0) return 0;
	if (!mu_rog_in_bounds_i(start.x, start.y, w, h)) return 0;
	if (!mu_rog_in_bounds_i(goal.x, goal.y, w, h)) return 0;
	if (blocked && blocked(start.x, start.y, user)) return 0;
	if (blocked && blocked(goal.x, goal.y, user)) return 0;
	if (heuristic_weight <= 0.0f) heuristic_weight = 1.0f;

	start_idx = mu_rog_grid_index(start.x, start.y, w);
	goal_idx = mu_rog_grid_index(goal.x, goal.y, w);
	dir_count = opts.allow_diag ? 8 : 4;

	if (stamp == 0) {
		/* the caller asked for a full reset (a freshly malloc'd buffer) */
		int i;
		for (i = 0; i < n; ++i) mu_rog_astar_node_reset(&nodes[i]);
		stamp = 1;   /* stamp 0 set by reset() means "never touched" */
	}

	nodes[start_idx] = *mu_rog_astar_node_touch(&nodes[start_idx], stamp);
	nodes[start_idx].g = 0.0f;
	nodes[start_idx].heap_pos = -1;
	{
		float h0 = opts.allow_diag
			? mu_rog_octile_heuristic(goal.x - start.x, goal.y - start.y)
			: (float)mu_rog_manhattan(goal.x - start.x, goal.y - start.y);
		mu_rog_astar_heap_update(heap, &heap_count, nodes, start_idx, h0 * heuristic_weight);
	}

	while (heap_count > 0) {
		mu_rog_astar_heap_entry e = mu_rog_astar_heap_take(heap, &heap_count, nodes);
		int cur = e.idx;
		int cx;
		int cy;
		int d;

		if (nodes[cur].closed) continue;
		nodes[cur].closed = 1;
		++expanded;

		if (cur == goal_idx) { found = 1; break; }

		cx = cur % w;
		cy = cur / w;
		for (d = 0; d < dir_count; ++d) {
			int nx = cx + dirs8[d][0];
			int ny = cy + dirs8[d][1];
			int nidx;
			float mult;
			float base;
			float ng;
			float hcost;

			if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
			if (blocked && blocked(nx, ny, user)) continue;
			nidx = mu_rog_grid_index(nx, ny, w);
			mu_rog_astar_node_touch(&nodes[nidx], stamp);
			if (nodes[nidx].closed) continue;
			if (dirs8[d][0] != 0 && dirs8[d][1] != 0) {
				if (!opts.cut_corners && !mu_rog__diag_allowed(cx, cy, nx, ny, blocked, user))
					continue;
				base = 1.41421356237f;
			} else {
				base = 1.0f;
			}

			mult = 1.0f;
			if (cost_fn) {
				mult = cost_fn(cx, cy, nx, ny, user);
				if (!(mult > 0.0f)) continue;
			}
			ng = nodes[cur].g + base * mult;
			if (ng >= nodes[nidx].g) continue;

			nodes[nidx].g = ng;
			nodes[nidx].came_from = cur;
			hcost = opts.allow_diag
				? mu_rog_octile_heuristic(goal.x - nx, goal.y - ny)
				: (float)mu_rog_manhattan(goal.x - nx, goal.y - ny);
			mu_rog_astar_heap_update(heap, &heap_count, nodes, nidx,
									 ng + hcost * heuristic_weight);
		}
	}

	if (out_expanded) *out_expanded = expanded;
	if (!found) return 0;
	if (out_cost) *out_cost = nodes[goal_idx].g;
	return mu_rog_astar_reconstruct(nodes, start_idx, goal_idx, w, out_path, out_path_cap);
}

MU_INLINE int mu_rog_astar_scratch_nodes(int w, int h)
{
	return (w > 0 && h > 0) ? w * h : 0;
}

/*
   ---------------------------------------------------------------------------
   Path smoothing (string pulling)
   ---------------------------------------------------------------------------

   A grid path zig-zags along the diamond; the octile A* path is optimal in grid
   steps but visually it hugs every obstacle corner. Smoothing keeps the two
   endpoints, walks the waypoints in order, and from each anchor extends
   greedily to the furthest later waypoint that is still in line of sight of it.
   The result is a strictly shorter-or-equal path (it replaces a sub-path by its
   straight chord) that is still walkable, because the chord is validated cell by
   cell with the same blocked callback.

   Cost: O(k * window) line walks for a path of k waypoints. The window bounds
   the work per anchor; window = 0 means "as far as the buffer allows".

   This is the cheap, safe half of any-angle path planning. It does not make A*
   smarter, it makes its output look like a person drew it, and it cannot make a
   path worse - the gate asserts cost non-increase and endpoint preservation.
*/
MU_INLINE int mu_rog_path_cost(const mu_rog_ivec2* path,
							   int count,
							   mu_rog_move_cost_fn cost_fn,
							   void* user,
							   float* out_cost);

MU_INLINE int mu_rog_path_smooth(mu_rog_ivec2* path,
								 int count,
								 int window,
								 mu_rog_cell_blocked_fn blocked,
								 void* user)
{
	int anchor;
	int out = 0;

	if (!path || count <= 0) return 0;
	if (count == 1) return 1;

	anchor = 0;
	path[out++] = path[0];
	while (anchor < count - 1) {
		int far = anchor + 1;
		int best = anchor + 1;
		int limit = (window > 0) ? anchor + window : count - 1;

		if (limit > count - 1) limit = count - 1;
		for (far = anchor + 2; far <= limit; ++far) {
			if (!mu_rog_los_one_way(path[anchor].x, path[anchor].y,
									path[far].x, path[far].y, blocked, user))
				break;
			best = far;
		}
		path[out++] = path[best];
		anchor = best;
	}
	return out;
}

/* Total cost of a path under the same cost_fn the search used; returns the
   number of edges, writes the sum to out_cost. */
MU_INLINE int mu_rog_path_cost(const mu_rog_ivec2* path,
							   int count,
							   mu_rog_move_cost_fn cost_fn,
							   void* user,
							   float* out_cost)
{
	int i;
	float sum = 0.0f;

	if (out_cost) *out_cost = 0.0f;
	if (!path || count <= 1) return 0;
	for (i = 1; i < count; ++i) {
		/* octile length of the edge, so a smoothed chord costs what it spans
		   and not what a single grid step costs */
		int adx = mu_rog_iabs(path[i].x - path[i - 1].x);
		int ady = mu_rog_iabs(path[i].y - path[i - 1].y);
		int mx = mu_rog_imax(adx, ady);
		int mn = mu_rog_imin(adx, ady);
		float base = (float)mn * 1.41421356237f + (float)(mx - mn);
		float mult = 1.0f;
		if (cost_fn) mult = cost_fn(path[i - 1].x, path[i - 1].y, path[i].x, path[i].y, user);
		if (mult <= 0.0f) mult = 0.0f;
		sum += base * mult;
	}
	if (out_cost) *out_cost = sum;
	return count - 1;
}

/* Allocating convenience wrapper, unchanged signature. Note cost_fn is now a
   multiplier and diagonals no longer cut corners; see the comment above. */
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
	mu_rog_search_opts opts = mu_rog_search_opts_default();
	mu_rog_astar_node* nodes;
	mu_rog_astar_heap_entry* heap;
	int out = 0;
	int n = w * h;

	if (n <= 0) return 0;
	opts.allow_diag = allow_diag ? 1 : 0;
	nodes = (mu_rog_astar_node*)MU_MALLOC((size_t)n * sizeof(mu_rog_astar_node));
	heap = (mu_rog_astar_heap_entry*)MU_MALLOC((size_t)n * sizeof(mu_rog_astar_heap_entry));
	if (!nodes || !heap) {
		MU_FREE(nodes);
		MU_FREE(heap);
		return 0;
	}
	out = mu_rog_astar_find_path_ex(w, h, start, goal, &opts, blocked, cost_fn, user,
									1.0f, 0u, nodes, heap, out_path, out_path_cap, NULL, NULL);
	MU_FREE(nodes);
	MU_FREE(heap);
	return out;
}

/* ------------------------------------------------------------------------- */
/* Autotiling: wall connection masks                                         */
/* ------------------------------------------------------------------------- */
/*
   A tile renderer needs to know which neighbours of a wall are also walls, not
   what the wall "is". The mask below follows the usual convention:

	 bits 0..3  N, E, S, W     cardinal walls
	 bits 4..7  NE, SE, SW, NW diagonal walls, but only set when BOTH cardinal
			   neighbours of that corner are walls. Without that rule a diagonal
			   touches two separate wall runs and the atlas picks the inner
			   corner tile in the wrong place; this is the single most common
			   autotiling bug, so the rule lives here rather than in every
			   renderer.
*/
#define MU_ROG_WALL_N   (1u << 0)
#define MU_ROG_WALL_E   (1u << 1)
#define MU_ROG_WALL_S   (1u << 2)
#define MU_ROG_WALL_W   (1u << 3)
#define MU_ROG_WALL_NE  (1u << 4)
#define MU_ROG_WALL_SE  (1u << 5)
#define MU_ROG_WALL_SW  (1u << 6)
#define MU_ROG_WALL_NW  (1u << 7)

typedef int (*mu_rog_is_wall_fn)(int x, int y, void* user);

MU_INLINE int mu_rog__wall_at(int x, int y, int w, int h, mu_rog_is_wall_fn is_wall, void* user)
{
	/* out of bounds counts as wall: the map edge should close up */
	if (x < 0 || y < 0 || x >= w || y >= h) return 1;
	return is_wall ? (is_wall(x, y, user) ? 1 : 0) : 0;
}

MU_INLINE uint8_t mu_rog_wall_bitmask8(int w,
									   int h,
									   int x,
									   int y,
									   mu_rog_is_wall_fn is_wall,
									   void* user)
{
	uint8_t m = 0;
	int n = mu_rog__wall_at(x, y - 1, w, h, is_wall, user);
	int e = mu_rog__wall_at(x + 1, y, w, h, is_wall, user);
	int s = mu_rog__wall_at(x, y + 1, w, h, is_wall, user);
	int ww = mu_rog__wall_at(x - 1, y, w, h, is_wall, user);

	if (n) m |= MU_ROG_WALL_N;
	if (e) m |= MU_ROG_WALL_E;
	if (s) m |= MU_ROG_WALL_S;
	if (ww) m |= MU_ROG_WALL_W;
	if (n && e && mu_rog__wall_at(x + 1, y - 1, w, h, is_wall, user)) m |= MU_ROG_WALL_NE;
	if (s && e && mu_rog__wall_at(x + 1, y + 1, w, h, is_wall, user)) m |= MU_ROG_WALL_SE;
	if (s && ww && mu_rog__wall_at(x - 1, y + 1, w, h, is_wall, user)) m |= MU_ROG_WALL_SW;
	if (n && ww && mu_rog__wall_at(x - 1, y - 1, w, h, is_wall, user)) m |= MU_ROG_WALL_NW;
	return m;
}

/* 4-bit connection index (0..15) for a plain 16-tile atlas: N,E,S,W. */
MU_INLINE int mu_rog_autotile_index16(uint8_t mask)
{
	return (int)(mask & 0x0Fu);
}

/*
   Box-drawing glyph for a wall cell, as a UTF-8 string literal. Handy for an
   ASCII view, and a direct test that the mask is right: a cross corridor must
   come out as the four-way glyph, a dead end as an elbow, and so on.
*/
MU_INLINE const char* mu_rog_autotile_glyph8(uint8_t mask)
{
	int n = (mask & MU_ROG_WALL_N) != 0;
	int e = (mask & MU_ROG_WALL_E) != 0;
	int s = (mask & MU_ROG_WALL_S) != 0;
	int w = (mask & MU_ROG_WALL_W) != 0;
	int count = n + e + s + w;
	int straight_ns = n && s && !e && !w;
	int straight_ew = e && w && !n && !s;

	if (count == 0) return "\xE2\x94\xBC";            /* isolated: ┼ */
	if (count == 1) {
		if (n) return "\xE2\x95\xB5";                  /* ╵ */
		if (s) return "\xE2\x95\xB7";                  /* ╷ */
		if (e) return "\xE2\x95\xB8";                  /* ╸ */
		return "\xE2\x95\xB9";                         /* ╹ */
	}
	if (straight_ns) return "\xE2\x94\x82";            /* │ */
	if (straight_ew) return "\xE2\x94\x80";            /* ─ */
	if (count == 2) {
		if (n && e) return "\xE2\x94\x94";              /* └ */
		if (e && s) return "\xE2\x94\x8C";              /* ┌ */
		if (s && w) return "\xE2\x94\x90";              /* ┐ */
		return "\xE2\x94\x98";                          /* ┘ */
	}
	if (count == 3) {
		if (!w) return "\xE2\x94\x9C";                  /* ├ */
		if (!n) return "\xE2\x94\xAC";                  /* ┬ */
		if (!e) return "\xE2\x94\xA4";                  /* ┤ */
		return "\xE2\x94\xB4";                          /* ┴ */
	}
	return "\xE2\x94\xBC";                             /* ┼ */
}

/* ------------------------------------------------------------------------- */
/* Maze generation: recursive-backtracker, then braiding                     */
/* ------------------------------------------------------------------------- */
/*
   A perfect maze has exactly one path between any two cells. That is the right
   starting point and the wrong dungeon: every dead end is a corridor the player
   walks in and back out for nothing. So generation is two passes:

	 1) recursive backtracker over the odd-indexed cells, carving two cells at a
		time. Iterative with an explicit stack (no recursion, bounded by the
		number of cells), each cell visited once, so O(w*h).
	 2) braiding: with probability braid_percent, knock out the wall of a dead
		end. 0 keeps the maze perfect, ~100 gives loops everywhere, 30-50 gives
		a cave-ish maze with a few cycles - which is what you usually want for a
		dungeon, because it removes the "walk four rooms to get anywhere" tax
		while keeping the maze's readability.

   grid is 1 for wall and 0 for floor on entry; it is cleared to wall here.
*/
MU_INLINE void mu_rog_generate_maze(int w,
									int h,
									mu_rog_rng* rng,
									uint8_t* grid,
									uint8_t wall,
									uint8_t floor,
									int braid_percent,
									int* stack,
									int stack_cap)
{
	static const int dirs[4][2] = {{2,0},{-2,0},{0,2},{0,-2}};
	int n = w * h;
	int sp = 0;
	int i;
	int start_x;
	int start_y;

	if (!grid || !rng || w < 3 || h < 3) return;
	for (i = 0; i < n; ++i) grid[i] = wall;

	start_x = 1 + 2 * (int)(mu_rog_rng_u32(rng) % (uint32_t)((w - 1) / 2));
	start_y = 1 + 2 * (int)(mu_rog_rng_u32(rng) % (uint32_t)((h - 1) / 2));
	grid[start_y * w + start_x] = floor;

	if (!stack || stack_cap <= 0) return;
	stack[sp++] = start_y * w + start_x;

	while (sp > 0) {
		int cur = stack[sp - 1];
		int cx = cur % w;
		int cy = cur / w;
		int order[4] = {0, 1, 2, 3};
		int k;
		int carved = 0;

		/* shuffle the four directions */
		for (k = 3; k > 0; --k) {
			int j = (int)(mu_rog_rng_u32(rng) % (uint32_t)(k + 1));
			int t = order[k];
			order[k] = order[j];
			order[j] = t;
		}
		for (k = 0; k < 4; ++k) {
			int d = order[k];
			int nx = cx + dirs[d][0];
			int ny = cy + dirs[d][1];
			int mx;
			int my;
			if (nx < 1 || ny < 1 || nx > w - 2 || ny > h - 2) continue;
			if (grid[ny * w + nx] == floor) continue;
			mx = cx + dirs[d][0] / 2;
			my = cy + dirs[d][1] / 2;
			grid[my * w + mx] = floor;
			grid[ny * w + nx] = floor;
			if (sp < stack_cap) stack[sp++] = ny * w + nx;
			carved = 1;
			break;
		}
		if (!carved) --sp;
	}

	/* braid: open one wall of some dead ends so the maze gets cycles */
	if (braid_percent > 0) {
		int x;
		int y;
		for (y = 1; y < h - 1; ++y) {
			for (x = 1; x < w - 1; ++x) {
				int open = 0;
				int walls[4];
				int wc = 0;
				int d;
				if (grid[y * w + x] != floor) continue;
				for (d = 0; d < 4; ++d) {
					int nx = x + (d == 0) - (d == 1);
					int ny = y + (d == 2) - (d == 3);
					if (grid[ny * w + nx] == floor) ++open;
					else if (wc < 4) walls[wc++] = d;
				}
				if (open != 1) continue;
				if ((int)(mu_rog_rng_u32(rng) % 100u) >= braid_percent) continue;
				/* knock out a wall toward a cell that is not already the exit */
				for (d = 0; d < wc; ++d) {
					int pick = walls[(int)(mu_rog_rng_u32(rng) % (uint32_t)wc)];
					int nx = x + ((pick == 0) - (pick == 1));
					int ny = y + ((pick == 2) - (pick == 3));
					int nnx = x + 2 * ((pick == 0) - (pick == 1));
					int nny = y + 2 * ((pick == 2) - (pick == 3));
					if (nnx < 0 || nny < 0 || nnx >= w || nny >= h) continue;
					grid[ny * w + nx] = floor;
					break;
				}
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* BSP dungeon: split, carve rooms in the leaves, join the leaves            */
/* ------------------------------------------------------------------------- */
/*
   BSP is the other half of the generation story: instead of dropping rooms at
   random until they collide (the mu_rog_generate_rooms_and_corridors approach,
   which needs rejection sampling and leaves the map's empty space unplanned),
   you split the whole map into a tree of rectangles and put exactly one room in
   each leaf. Every room is separated by construction, the whole map is used, and
   connecting siblings up the tree connects everything with no component pass.

   Node storage is caller-owned: at most 2*max_rooms - 1 nodes, arranged so that
   children follow their parent (the split is a breadth-first-friendly layout but
   the link fields are what matter).
*/
typedef struct mu_rog_bsp_node {
	mu_rog_rect rect;     /* the area this node owns */
	mu_rog_rect room;     /* leaves: the carved room; internal: a representative room */
	int left;             /* child index or -1 */
	int right;
	int parent;
	int depth;
} mu_rog_bsp_node;

MU_INLINE int mu_rog__rect_inside(mu_rog_rect outer, mu_rog_rect inner)
{
	return inner.x >= outer.x && inner.y >= outer.y &&
		inner.x + inner.w <= outer.x + outer.w &&
		inner.y + inner.h <= outer.y + outer.h;
}

/*
   Split bounds recursively until every leaf is small enough. min_leaf is the
   smallest side a leaf may have; max_depth caps the tree. Returns the number of
   nodes used, or 0 if the node buffer was too small.

   Splitting rule: cut the longer side, at a position between 40% and 60% of it,
   so rooms come out roughly square instead of ribbon-shaped - the usual cause of
   a BSP dungeon that looks like a grid of shelves.
*/
MU_INLINE int mu_rog_bsp_build(mu_rog_bsp_node* nodes,
							   int cap,
							   mu_rog_rect bounds,
							   int min_leaf,
							   int max_depth,
							   mu_rog_rng* rng)
{
	int count = 1;

	if (!nodes || cap < 1) return 0;
	if (min_leaf < 2) min_leaf = 2;

	nodes[0].rect = bounds;
	nodes[0].left = -1;
	nodes[0].right = -1;
	nodes[0].parent = -1;
	nodes[0].depth = 0;
	nodes[0].room = bounds;

	{
		int i = 0;
		/* iterate as the array grows: children are appended, so this is a BFS */
		while (i < count) {
			mu_rog_rect r = nodes[i].rect;
			int can_split_h;
			int can_split_v;
			int split_h;
			int at;
			int a;
			int b;

			if (nodes[i].depth >= max_depth) { ++i; continue; }
			can_split_h = r.h >= 2 * min_leaf;   /* horizontal cut */
			can_split_v = r.w >= 2 * min_leaf;   /* vertical cut */
			if (!can_split_h && !can_split_v) { ++i; continue; }
			if (can_split_h && can_split_v) {
				if (r.w > r.h) split_h = 0;
				else if (r.h > r.w) split_h = 1;
				else split_h = (int)(mu_rog_rng_u32(rng) & 1u);
			} else {
				split_h = can_split_h ? 1 : 0;
			}

			if (count + 2 > cap) { ++i; continue; }
			if (split_h) {
				int lo = r.y + min_leaf;
				int hi = r.y + r.h - min_leaf;
				int span = hi - lo;
				if (span < 1) { ++i; continue; }
				at = lo + (int)(mu_rog_rng_u32(rng) % (uint32_t)(span + 1));
				a = count++;
				b = count++;
				nodes[a].rect.x = r.x;
				nodes[a].rect.y = r.y;
				nodes[a].rect.w = r.w;
				nodes[a].rect.h = at - r.y;
				nodes[b].rect.x = r.x;
				nodes[b].rect.y = at;
				nodes[b].rect.w = r.w;
				nodes[b].rect.h = r.y + r.h - at;
			} else {
				int lo = r.x + min_leaf;
				int hi = r.x + r.w - min_leaf;
				int span = hi - lo;
				if (span < 1) { ++i; continue; }
				at = lo + (int)(mu_rog_rng_u32(rng) % (uint32_t)(span + 1));
				a = count++;
				b = count++;
				nodes[a].rect.x = r.x;
				nodes[a].rect.y = r.y;
				nodes[a].rect.w = at - r.x;
				nodes[a].rect.h = r.h;
				nodes[b].rect.x = at;
				nodes[b].rect.y = r.y;
				nodes[b].rect.w = r.x + r.w - at;
				nodes[b].rect.h = r.h;
			}
			nodes[a].parent = i;
			nodes[b].parent = i;
			nodes[a].depth = nodes[i].depth + 1;
			nodes[b].depth = nodes[i].depth + 1;
			nodes[a].left = -1;
			nodes[a].right = -1;
			nodes[b].left = -1;
			nodes[b].right = -1;
			nodes[a].room = nodes[a].rect;
			nodes[b].room = nodes[b].rect;
			nodes[i].left = a;
			nodes[i].right = b;
			++i;
		}
	}
	return count;
}

/*
   Carve one room per leaf, inset by one so neighbouring rooms never share a wall
   cell. out_rooms receives the leaf rooms (up to out_cap) and the count is
   returned; internal nodes then inherit a representative room for corridors.
*/
MU_INLINE int mu_rog_bsp_carve_rooms(mu_rog_bsp_node* nodes,
									 int count,
									 mu_rog_rng* rng,
									 uint8_t* grid,
									 int w,
									 int h,
									 uint8_t wall,
									 uint8_t floor,
									 int min_room,
									 mu_rog_rect* out_rooms,
									 int out_cap)
{
	int i;
	int room_count = 0;

	if (!nodes || !grid || count <= 0) return 0;
	if (min_room < 2) min_room = 2;

	for (i = 0; i < count; ++i) {
		int x;
		int y;
		if (nodes[i].left >= 0 || nodes[i].right >= 0) continue;   /* internal */
		{
			mu_rog_rect r = nodes[i].rect;
			int rw;
			int rh;
			int rx;
			int ry;

			/* leave a one-cell wall between the room and the leaf edge, so two
			   neighbouring leaves can never produce touching rooms */
			int inner_w = r.w - 2;
			int inner_h = r.h - 2;
			if (inner_w < min_room || inner_h < min_room) {
				rw = r.w;
				rh = r.h;
			} else {
				rw = min_room + (int)(mu_rog_rng_u32(rng) % (uint32_t)(inner_w - min_room + 1));
				rh = min_room + (int)(mu_rog_rng_u32(rng) % (uint32_t)(inner_h - min_room + 1));
			}
			/* centre it in the leaf */
			rx = r.x + (r.w - rw) / 2;
			ry = r.y + (r.h - rh) / 2;
			nodes[i].room.x = rx;
			nodes[i].room.y = ry;
			nodes[i].room.w = rw;
			nodes[i].room.h = rh;

			for (y = ry; y < ry + rh; ++y) {
				for (x = rx; x < rx + rw; ++x) {
					if (mu_rog_in_bounds_i(x, y, w, h) && grid[y * w + x] == wall)
						grid[y * w + x] = floor;
				}
			}
			if (out_rooms && room_count < out_cap) out_rooms[room_count] = nodes[i].room;
			++room_count;
		}
	}

	/* internal nodes need a centre to route corridors; use a child's room */
	for (i = count - 1; i >= 0; --i) {
		if (nodes[i].left >= 0 && nodes[i].right >= 0) {
			nodes[i].room = nodes[nodes[i].left].room;
		}
	}
	return room_count;
}

/*
   Join every internal node's two children with an L-shaped corridor. Because
   the tree covers the map and every leaf got a room, this connects all rooms in
   one pass: no flood fill, no "keep largest component" step, and the corridor
   count is exactly leaves - 1.
*/
MU_INLINE void mu_rog_bsp_carve_corridors(const mu_rog_bsp_node* nodes,
										  int count,
										  uint8_t* grid,
										  int w,
										  int h,
										  uint8_t wall,
										  uint8_t floor)
{
	int i;
	(void)wall;
	if (!nodes || !grid) return;
	for (i = 0; i < count; ++i) {
		mu_rog_ivec2 a;
		mu_rog_ivec2 b;
		int x;
		int y;
		if (nodes[i].left < 0 || nodes[i].right < 0) continue;
		a = mu_rog_rect_center(nodes[nodes[i].left].room);
		b = mu_rog_rect_center(nodes[nodes[i].right].room);
		if (i & 1) {
			for (x = mu_rog_imin(a.x, b.x); x <= mu_rog_imax(a.x, b.x); ++x)
				if (mu_rog_in_bounds_i(x, a.y, w, h)) grid[a.y * w + x] = floor;
			for (y = mu_rog_imin(a.y, b.y); y <= mu_rog_imax(a.y, b.y); ++y)
				if (mu_rog_in_bounds_i(b.x, y, w, h)) grid[y * w + b.x] = floor;
		} else {
			for (y = mu_rog_imin(a.y, b.y); y <= mu_rog_imax(a.y, b.y); ++y)
				if (mu_rog_in_bounds_i(a.x, y, w, h)) grid[y * w + a.x] = floor;
			for (x = mu_rog_imin(a.x, b.x); x <= mu_rog_imax(a.x, b.x); ++x)
				if (mu_rog_in_bounds_i(x, b.y, w, h)) grid[b.y * w + x] = floor;
		}
	}
}

/*
   ---------------------------------------------------------------------------
   Doors
   ---------------------------------------------------------------------------

   A door is a floor cell on a room's wall line with floor on exactly one side
   inside the room and floor on the opposite side outside it. Scanning the room
   perimeters finds every corridor mouth; door_percent decides how many of them
   become doors, so a dungeon can be "all doors" or "a few doors" without a
   second generator. Returns the number of doors placed.
*/
MU_INLINE int mu_rog_place_doors(uint8_t* grid,
								 int w,
								 int h,
								 const mu_rog_rect* rooms,
								 int room_count,
								 uint8_t floor,
								 uint8_t door,
								 int door_percent,
								 mu_rog_rng* rng)
{
	int placed = 0;
	int r;

	if (!grid || !rooms || w <= 0 || h <= 0) return 0;

#define MU_ROG_IS_FLOOR(x_, y_) (((x_) >= 0 && (y_) >= 0 && (x_) < w && (y_) < h) \
		? (grid[(y_) * w + (x_)] == floor) : 0)

	/*
	   Scan the ring of wall cells immediately OUTSIDE each room, not the room's
	   own edge: the room is a solid rectangle, so its edge cells are floor and a
	   corridor cuts the wall ring around it. A doorway is a ring cell with floor
	   on the room side, floor on the corridor side, and wall on both of the other
	   sides - one cell wide, no double doors, no doors where a corridor runs
	   along the wall.
	*/
	for (r = 0; r < room_count; ++r) {
		mu_rog_rect room = rooms[r];
		int x;
		int y;
		for (y = room.y - 1; y <= room.y + room.h; ++y) {
			for (x = room.x - 1; x <= room.x + room.w; ++x) {
				int on_ring = (x == room.x - 1 || x == room.x + room.w ||
							   y == room.y - 1 || y == room.y + room.h);
				int horiz;
				int vert;
				if (!on_ring) continue;
				if (x < 0 || y < 0 || x >= w || y >= h) continue;
				if (grid[y * w + x] == door) continue;
				/* the cell may already be floor: a corridor that crosses the wall
				   ring carves it, and that carved cell IS the doorway */

				horiz = MU_ROG_IS_FLOOR(x - 1, y) && MU_ROG_IS_FLOOR(x + 1, y) &&
					!MU_ROG_IS_FLOOR(x, y - 1) && !MU_ROG_IS_FLOOR(x, y + 1);
				vert = MU_ROG_IS_FLOOR(x, y - 1) && MU_ROG_IS_FLOOR(x, y + 1) &&
					!MU_ROG_IS_FLOOR(x - 1, y) && !MU_ROG_IS_FLOOR(x + 1, y);
				if (!horiz && !vert) continue;
				if (rng && door_percent > 0 &&
					(int)(mu_rog_rng_u32(rng) % 100u) >= door_percent) continue;
				grid[y * w + x] = door;
				++placed;
			}
		}
	}
#undef MU_ROG_IS_FLOOR
	return placed;
}

/*
   ---------------------------------------------------------------------------
   Connecting regions
   ---------------------------------------------------------------------------

   Whatever generator ran, the map may end up with disconnected floor. This
   labels 4-connected regions and carves an L corridor from each region to the
   largest one, so the level is always traversable. It is the general safety net;
   BSP does not need it, a cave generator does.
*/
MU_INLINE int mu_rog_connect_regions(mu_rog_grid_u8 map,
									 uint8_t passable_tile,
									 uint8_t carve_tile,
									 int* label,
									 int* queue,
									 int max_links)
{
	int carved = 0;
	int links = 0;
	mu_rog_component_result res;

	if (!map.data || !label || !queue || map.w <= 0 || map.h <= 0) return 0;
	if (max_links <= 0) max_links = map.w + map.h;

	res = mu_rog_label_components4(map, passable_tile, label, queue);
	while (res.component_count > 1 && links < max_links) {
		int anchor = -1;
		int target = -1;
		int i;
		int ax;
		int ay;
		int tx;
		int ty;
		int step_x;
		int step_y;

		for (i = 0; i < map.w * map.h; ++i) {
			if (label[i] == res.largest_component_id) { anchor = i; break; }
		}
		for (i = 0; i < map.w * map.h; ++i) {
			if (label[i] > 0 && label[i] != res.largest_component_id) { target = i; break; }
		}
		if (anchor < 0 || target < 0) break;

		ax = anchor % map.w; ay = anchor / map.w;
		tx = target % map.w; ty = target / map.w;
		step_x = (ax > tx) - (ax < tx);
		step_y = (ay > ty) - (ay < ty);

		/* carve an L from the stray region to the largest one */
		for (;;) {
			int idx = mu_rog_grid_index(tx, ty, map.w);
			if (map.data[idx] != passable_tile) {
				map.data[idx] = carve_tile;
				++carved;
			}
			if (tx == ax && ty == ay) break;
			if (tx != ax) tx += step_x;
			else ty += step_y;
		}
		++links;
		res = mu_rog_label_components4(map, passable_tile, label, queue);
	}
	return carved;
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
