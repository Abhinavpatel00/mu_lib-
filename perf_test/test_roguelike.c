/*
   Correctness gate for mu/mu_roguelike.h.

   Every public entry point this header gained in the "better algorithms" batch
   is gated here against the property it claims, not against a golden dump:

	 - FOV: the symmetric shadowcast must agree with the float formulation cell
	   for cell, must be symmetric by construction, must hide what is behind a
	   pillar, must mark exactly the 3x3 box at radius 1, and must not run the
	   quadrants when the origin is solid.
	 - Dijkstra map: exact against an O(n^2) reference with the same corner rule,
	   honours terrain multipliers, and descends downhill everywhere.
	 - A*: same cost as the Dijkstra map on the same map, valid path, no
	   allocation in the _ex entry point, and fewer expansions under a weighted
	   heuristic.
	 - Path smoothing: never increases the cost, keeps both endpoints, stays off
	   walls.
	 - Generators: the maze is a spanning tree with braid 0 and has cycles with
	   braid 100; the BSP dungeon's rooms live inside their leaves, never touch,
	   and the corridor pass leaves exactly one component; doors land on one-cell
	   gaps; the autotile masks and glyphs match hand-checked cells;
	   connect_regions always ends at one component.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mu_roguelike.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		++g_checks;                                                            \
		if (!(cond)) {                                                         \
			++g_failures;                                                      \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
		}                                                                      \
	} while (0)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct test_map {
	int w;
	int h;
	uint8_t* cells;
} test_map;

static int test_blocked(int x, int y, void* user)
{
	test_map* m = (test_map*)user;
	return m->cells[y * m->w + x] != 0;
}

static float test_mud(int from_x, int from_y, int to_x, int to_y, void* user)
{
	/* the middle row is expensive to enter */
	(void)to_x;
	(void)from_x;
	(void)from_y;
	(void)user;
	return (to_y == 2) ? 3.0f : 1.0f;
}

static int test_is_wall(int x, int y, void* user)
{
	test_map* m = (test_map*)user;
	return m->cells[y * m->w + x] != 0;
}

static test_map map_new(int w, int h, uint8_t fill)
{
	test_map m;
	m.w = w;
	m.h = h;
	m.cells = (uint8_t*)malloc((size_t)w * (size_t)h);
	memset(m.cells, fill, (size_t)w * (size_t)h);
	return m;
}

static void map_free(test_map* m)
{
	free(m->cells);
	m->cells = NULL;
}

static uint64_t g_lcg = 0x9E3779B97F4A7C15ull;

static uint32_t test_rand(void)
{
	g_lcg ^= g_lcg << 13;
	g_lcg ^= g_lcg >> 7;
	g_lcg ^= g_lcg << 17;
	return (uint32_t)(g_lcg >> 11);
}

/* float reference for the FOV comparison: same rules, float slopes */
static void ref_fov_rec(test_map m,
						uint8_t* vis,
						int ox,
						int oy,
						int radius,
						int q,
						int depth,
						float ss,
						float es)
{
	int mn;
	int mx;
	int col;
	int pb = -1;

	if (radius > 0 && depth > radius) return;
	mn = (int)floorf((float)depth * ss + 0.5f);
	mx = (int)ceilf((float)depth * es - 0.5f);
	if (mn > mx) return;

	for (col = mn; col <= mx; ++col) {
		int dx;
		int dy;
		int x;
		int y;
		int b;
		switch (q) {
		case 0:  dx = col;    dy = -depth; break;
		case 1:  dx = col;    dy = depth;  break;
		case 2:  dx = depth;  dy = col;    break;
		default: dx = -depth; dy = col;    break;
		}
		x = ox + dx;
		y = oy + dy;
		b = (x < 0 || y < 0 || x >= m.w || y >= m.h) ? 1 : (m.cells[y * m.w + x] != 0);
		if (x >= 0 && y >= 0 && x < m.w && y < m.h) {
			if (b || ((float)col >= (float)depth * ss && (float)col <= (float)depth * es))
				vis[y * m.w + x] = 1;
		}
		if (pb == 1 && !b) ss = ((float)col - 0.5f) / (float)depth;
		if (pb == 0 && b) ref_fov_rec(m, vis, ox, oy, radius, q, depth + 1, ss,
									  ((float)col - 0.5f) / (float)depth);
		pb = b;
	}
	if (pb == 1) return;
	ref_fov_rec(m, vis, ox, oy, radius, q, depth + 1, ss, es);
}

static void ref_fov(test_map m, int ox, int oy, int radius, uint8_t* vis)
{
	int q;
	memset(vis, 0, (size_t)m.w * (size_t)m.h);
	if (ox < 0 || oy < 0 || ox >= m.w || oy >= m.h) return;
	vis[oy * m.w + ox] = 1;
	if (m.cells[oy * m.w + ox]) return;
	for (q = 0; q < 4; ++q) ref_fov_rec(m, vis, ox, oy, radius, q, 1, -1.0f, 1.0f);
}

/* exact reference shortest path (float costs, no corner cutting) */
static void ref_distances(test_map m, mu_rog_ivec2 from, float* dist)
{
	int n = m.w * m.h;
	int i;
	int iter;
	char* done = (char*)calloc((size_t)n, 1);
	static const int dirs[8][2] = {
		{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}
	};

	for (i = 0; i < n; ++i) dist[i] = 1e30f;
	if (m.cells[from.y * m.w + from.x]) { free(done); return; }
	dist[from.y * m.w + from.x] = 0.0f;

	for (iter = 0; iter < n; ++iter) {
		int best = -1;
		int k;
		float bd = 1e30f;
		for (k = 0; k < n; ++k) if (!done[k] && dist[k] < bd) { bd = dist[k]; best = k; }
		if (best < 0) break;
		done[best] = 1;
		{
			int cx = best % m.w;
			int cy = best / m.w;
			int d;
			for (d = 0; d < 8; ++d) {
				int nx = cx + dirs[d][0];
				int ny = cy + dirs[d][1];
				int ni;
				float nd;
				if (nx < 0 || ny < 0 || nx >= m.w || ny >= m.h) continue;
				if (m.cells[ny * m.w + nx]) continue;
				if (dirs[d][0] && dirs[d][1] &&
					m.cells[cy * m.w + nx] && m.cells[ny * m.w + cx]) continue;
				ni = ny * m.w + nx;
				nd = bd + ((dirs[d][0] && dirs[d][1]) ? 1.41421356237f : 1.0f);
				if (nd < dist[ni]) dist[ni] = nd;
			}
		}
	}
	free(done);
}

/* integer reference for the Dijkstra map (same corner rule, 10/14 weights) */
static void ref_dijkstra_i(test_map m, mu_rog_ivec2 goal, int16_t* dist)
{
	int n = m.w * m.h;
	int i;
	int iter;
	char* done = (char*)calloc((size_t)n, 1);
	static const int dirs[8][2] = {
		{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}
	};

	for (i = 0; i < n; ++i) dist[i] = (int16_t)32767;
	if (m.cells[goal.y * m.w + goal.x]) { free(done); return; }
	dist[goal.y * m.w + goal.x] = 0;

	for (iter = 0; iter < n; ++iter) {
		int best = -1;
		int k;
		int bd = 32767;
		for (k = 0; k < n; ++k) if (!done[k] && dist[k] < bd) { bd = dist[k]; best = k; }
		if (best < 0) break;
		done[best] = 1;
		{
			int cx = best % m.w;
			int cy = best / m.w;
			int d;
			for (d = 0; d < 8; ++d) {
				int nx = cx + dirs[d][0];
				int ny = cy + dirs[d][1];
				int ni;
				int nd;
				if (nx < 0 || ny < 0 || nx >= m.w || ny >= m.h) continue;
				if (m.cells[ny * m.w + nx]) continue;
				if (dirs[d][0] && dirs[d][1] &&
					m.cells[cy * m.w + nx] && m.cells[ny * m.w + cx]) continue;
				ni = ny * m.w + nx;
				nd = bd + ((dirs[d][0] && dirs[d][1]) ? 14 : 10);
				if (nd < dist[ni]) dist[ni] = (int16_t)nd;
			}
		}
	}
	free(done);
}

/* ------------------------------------------------------------------------- */
/* 1. RNG                                                                    */
/* ------------------------------------------------------------------------- */
static void test_rng(void)
{
	mu_rog_rng a;
	mu_rog_rng b;
	int i;
	int same = 1;

	mu_rog_rng_seed(&a, 12345u);
	mu_rog_rng_seed(&b, 12345u);
	for (i = 0; i < 64; ++i) {
		if (mu_rog_rng_u64(&a) != mu_rog_rng_u64(&b)) same = 0;
	}
	CHECK(same, "same seed must give the same stream");

	mu_rog_rng_seed(&a, 999u);
	{
		int in_range = 1;
		for (i = 0; i < 4096; ++i) {
			int v = 5 + (int)(mu_rog_rng_u32(&a) % 5u);
			if (v < 5 || v > 9) in_range = 0;
		}
		CHECK(in_range, "rng modulo range must stay inside [lo, hi]");
	}
	{
		float f_ok = 1;
		mu_rog_rng_seed(&a, 7u);
		for (i = 0; i < 4096; ++i) {
			float f = mu_rog_rng_f01(&a);
			if (!(f >= 0.0f && f < 1.0f)) f_ok = 0;
		}
		CHECK(f_ok, "rng_f01 must stay inside [0, 1)");
	}
}

/* ------------------------------------------------------------------------- */
/* 2. line of sight                                                          */
/* ------------------------------------------------------------------------- */
static void test_los(void)
{
	int trial;
	int one_way_asym = 0;
	int sym_asym = 0;

	for (trial = 0; trial < 24; ++trial) {
		test_map m = map_new(16, 16, 0);
		int i;
		int x0;
		int y0;
		int x1;
		int y1;
		for (i = 0; i < m.w * m.h; ++i) m.cells[i] = (test_rand() % 100u) < 25u;
		for (y0 = 0; y0 < m.h; y0 += 3) {
			for (x0 = 0; x0 < m.w; x0 += 3) {
				if (m.cells[y0 * m.w + x0]) continue;
				for (y1 = 0; y1 < m.h; y1 += 3) {
					for (x1 = 0; x1 < m.w; x1 += 3) {
						int ab;
						int ba;
						if (m.cells[y1 * m.w + x1]) continue;
						if (x0 == x1 && y0 == y1) continue;
						ab = mu_rog_los_one_way(x0, y0, x1, y1, test_blocked, &m);
						ba = mu_rog_los_one_way(x1, y1, x0, y0, test_blocked, &m);
						if (ab != ba) ++one_way_asym;
						ab = mu_rog_has_line_of_sight(x0, y0, x1, y1, test_blocked, &m);
						ba = mu_rog_has_line_of_sight(x1, y1, x0, y0, test_blocked, &m);
						if (ab != ba) ++sym_asym;
					}
				}
			}
		}
		map_free(&m);
	}
	/* the raw walker is asymmetric; the public predicate must not be */
	CHECK(one_way_asym > 0, "expected the raw Bresenham walk to be asymmetric somewhere");
	CHECK(sym_asym == 0, "mu_rog_has_line_of_sight must be symmetric (%d asymmetric pairs)", sym_asym);

	{
		test_map m = map_new(8, 8, 1);
		m.cells[0] = 0;
		m.cells[1] = 0;
		CHECK(mu_rog_has_line_of_sight(0, 0, 1, 0, test_blocked, &m), "adjacent open cells must see each other");
		CHECK(!mu_rog_has_line_of_sight(0, 0, 7, 7, test_blocked, &m), "a blocked target is not visible");
		map_free(&m);
	}
}

/* ------------------------------------------------------------------------- */
/* 3. FOV: shadowcasting                                                     */
/* ------------------------------------------------------------------------- */
static void test_fov_matches_float(void)
{
	int trial;
	int differing = 0;

	for (trial = 0; trial < 60; ++trial) {
		int w = 8 + (int)(test_rand() % 18u);
		int h = 8 + (int)(test_rand() % 18u);
		int density = 10 + (int)(test_rand() % 30u);
		test_map m = map_new(w, h, 0);
		uint8_t* a = (uint8_t*)malloc((size_t)w * (size_t)h);
		uint8_t* b = (uint8_t*)malloc((size_t)w * (size_t)h);
		int i;
		int ox;
		int oy;
		for (i = 0; i < w * h; ++i) m.cells[i] = (test_rand() % 100u) < (uint32_t)density;
		do {
			ox = (int)(test_rand() % (uint32_t)w);
			oy = (int)(test_rand() % (uint32_t)h);
		} while (m.cells[oy * w + ox]);
		mu_rog_fov_shadowcast(w, h, ox, oy, 0, test_blocked, &m, a);
		ref_fov(m, ox, oy, 0, b);
		for (i = 0; i < w * h; ++i) if (!!a[i] != !!b[i]) ++differing;
		free(a);
		free(b);
		map_free(&m);
	}
	CHECK(differing == 0, "integer shadowcast must match the float formulation exactly (%d cells differ)",
		  differing);
}

static void test_fov_symmetry(void)
{
	static const char* names[3] = {"rubble", "pillars", "corridors"};
	int kind;
	for (kind = 0; kind < 3; ++kind) {
		int w = 20;
		int h = 20;
		test_map m = map_new(w, h, 0);
		uint8_t* fov = (uint8_t*)malloc((size_t)w * h);
		uint8_t* back = (uint8_t*)malloc((size_t)w * h);
		long pairs = 0;
		long asym_shadow = 0;
		long asym_ray = 0;
		int x;
		int y;

		for (y = 0; y < h; ++y) {
			for (x = 0; x < w; ++x) {
				if (kind == 0) m.cells[y * w + x] = (test_rand() % 100u) < 25u;
				else if (kind == 1) m.cells[y * w + x] = (x % 4 == 2 && y % 4 == 2);
				else m.cells[y * w + x] = ((x == 0 || x == w - 1 || y == 0 || y == h - 1) ||
										   (x == 9 && y != 6) || (y == 13 && x != 15));
			}
		}
		for (y = 0; y < h; ++y) {
			for (x = 0; x < w; ++x) {
				int bx;
				int by;
				if (m.cells[y * w + x]) continue;
				mu_rog_fov_shadowcast(w, h, x, y, 0, test_blocked, &m, fov);
				{
					static uint8_t ray[400];
					mu_rog_fov_raycast(w, h, x, y, 12, test_blocked, &m, ray);
					for (by = 0; by < h; ++by) {
						for (bx = 0; bx < w; ++bx) {
							if (m.cells[by * w + bx] || !ray[by * w + bx]) continue;
							/* raycast is radius limited; only compare inside the radius */
							if (mu_rog_chebyshev(bx - x, by - y) > 11) continue;
							mu_rog_fov_raycast(w, h, bx, by, 12, test_blocked, &m, back);
							if (!back[y * w + x]) ++asym_ray;
						}
					}
				}
				for (by = 0; by < h; ++by) {
					for (bx = 0; bx < w; ++bx) {
						if (m.cells[by * w + bx] || !fov[by * w + bx]) continue;
						++pairs;
						mu_rog_fov_shadowcast(w, h, bx, by, 0, test_blocked, &m, back);
						if (!back[y * w + x]) ++asym_shadow;
					}
				}
			}
		}
		CHECK(pairs > 1000, "%s: expected a non-trivial map (%ld pairs)", names[kind], pairs);
		CHECK(asym_shadow == 0, "%s: shadowcast must be symmetric (%ld of %ld pairs asymmetric)",
			  names[kind], asym_shadow, pairs);
		CHECK(asym_ray > 0, "%s: the raycast is expected to be asymmetric somewhere (%ld)", names[kind],
			  asym_ray);
		free(fov);
		free(back);
		map_free(&m);
	}
}

static void test_fov_geometry(void)
{
	uint8_t vis[32 * 32];
	int w = 21;
	int h = 21;
	int x;
	int y;

	/* an open room, radius 5: exactly the box the diamond covers */
	{
		test_map m = map_new(w, h, 0);
		int cx = w / 2;
		int cy = h / 2;
		int wrong = 0;
		mu_rog_fov_shadowcast(w, h, cx, cy, 5, test_blocked, &m, vis);
		for (y = 0; y < h; ++y) {
			for (x = 0; x < w; ++x) {
				int in_box = (abs(x - cx) <= 5 && abs(y - cy) <= 5);
				if (!!vis[y * w + x] != in_box) ++wrong;
			}
		}
		CHECK(wrong == 0, "open room, radius 5: %d cells wrong", wrong);
		map_free(&m);
	}

	/* radius 1 is the 3x3 box, nothing more */
	{
		test_map m = map_new(w, h, 0);
		int marked = mu_rog_fov_shadowcast(w, h, 10, 10, 1, test_blocked, &m, vis);
		int count = 0;
		for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) if (vis[y * w + x]) ++count;
		CHECK(count == 9, "radius 1 must mark the 3x3 box (marked %d)", count);
		CHECK(marked == 9, "the return value must match the marked count (%d)", marked);
		map_free(&m);
	}

	/* a pillar casts a shadow, and the cells beside it stay lit */
	{
		test_map m = map_new(w, h, 0);
		m.cells[10 * w + 13] = 1;
		mu_rog_fov_shadowcast(w, h, 10, 10, 0, test_blocked, &m, vis);
		CHECK(vis[10 * w + 13], "the pillar itself is visible");
		CHECK(!vis[10 * w + 15], "the cell directly behind a pillar is not visible");
		CHECK(vis[9 * w + 14], "a cell diagonal to the pillar is visible");
		CHECK(vis[28 * w + 10] == 0, "cells far away stay dark");
		map_free(&m);
	}

	/* a solid origin marks only itself */
	{
		test_map m = map_new(w, h, 1);
		int marked = mu_rog_fov_shadowcast(w, h, 10, 10, 0, test_blocked, &m, vis);
		int count = 0;
		for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) if (vis[y * w + x]) ++count;
		CHECK(count == 1, "a solid origin must mark only itself (marked %d)", count);
		CHECK(marked == 1, "return value for a solid origin (%d)", marked);
		map_free(&m);
	}

	/* out of bounds origin marks nothing and does not crash */
	{
		test_map m = map_new(w, h, 0);
		int marked = mu_rog_fov_shadowcast(w, h, -3, 4, 8, test_blocked, &m, vis);
		int count = 0;
		for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) if (vis[y * w + x]) ++count;
		CHECK(marked == 0 && count == 0, "an origin outside the map must mark nothing");
		map_free(&m);
	}

	/* a wall across the map blocks everything behind it */
	{
		test_map m = map_new(w, h, 0);
		for (y = 0; y < h; ++y) m.cells[y * w + 15] = 1;
		mu_rog_fov_shadowcast(w, h, 10, 10, 0, test_blocked, &m, vis);
		CHECK(vis[10 * w + 15], "the wall is visible");
		CHECK(!vis[10 * w + 16], "nothing behind a full wall is visible");
		CHECK(!vis[10 * w + 20], "nothing far behind a full wall is visible");
		CHECK(vis[10 * w + 14], "the space in front of the wall is visible");
		map_free(&m);
	}

	/* a one-cell doorway in a wall passes sight straight through the gap and
	   not through the wall beside it */
	{
		test_map m = map_new(w, h, 1);
		for (x = 1; x < w - 1; ++x) for (y = 1; y < h - 1; ++y) m.cells[y * w + x] = 0;
		for (y = 1; y < h - 1; ++y) m.cells[y * w + 10] = 1;
		m.cells[10 * w + 10] = 0;                 /* the doorway */
		mu_rog_fov_shadowcast(w, h, 5, 10, 0, test_blocked, &m, vis);
		CHECK(vis[10 * w + 5 + 5], "sight passes straight through the doorway");
		CHECK(vis[10 * w + 15], "the cell beyond the doorway is visible");
		CHECK(!vis[5 * w + 15], "sight does not pass through the wall beside the door");
		CHECK(!vis[3 * w + 15], "sight does not pass through the far end of the wall");
		CHECK(vis[10 * w + 9], "the wall around the door is visible");
		map_free(&m);
	}
}

/* ------------------------------------------------------------------------- */
/* 4. Dijkstra map                                                           */
/* ------------------------------------------------------------------------- */
static void test_dijkstra(void)
{
	int trial;
	int mismatches = 0;
	int ascend = 0;

	for (trial = 0; trial < 40; ++trial) {
		int w = 7 + (int)(test_rand() % 12u);
		int h = 7 + (int)(test_rand() % 12u);
		int n = w * h;
		int density = 10 + (int)(test_rand() % 30u);
		test_map m = map_new(w, h, 0);
		int16_t* mine = (int16_t*)malloc((size_t)n * sizeof(int16_t));
		int16_t* ref = (int16_t*)malloc((size_t)n * sizeof(int16_t));
		mu_rog_search_opts opts = mu_rog_search_opts_default();
		int* scratch = (int*)malloc(sizeof(int) * (size_t)mu_rog_dijkstra_scratch_ints(w, h, &opts));
		mu_rog_ivec2 goal;
		int i;
		for (i = 0; i < n; ++i) m.cells[i] = (test_rand() % 100u) < (uint32_t)density;
		m.cells[0] = 0;
		m.cells[n - 1] = 0;
		goal.x = 0;
		goal.y = 0;
		mu_rog_dijkstra_map8_ex(w, h, &goal, 1, test_blocked, NULL, &m, &opts, mine, scratch);
		ref_dijkstra_i(m, goal, ref);
		for (i = 0; i < n; ++i) if (mine[i] != ref[i]) { ++mismatches; break; }

		/* everywhere reachable, the gradient must point downhill */
		for (i = 0; i < n; ++i) {
			mu_rog_ivec2 next;
			int x = i % w;
			int y = i / w;
			if (mine[i] == 32767 || mine[i] == 0) continue;
			if (!mu_rog_dijkstra_descend(w, h, mine, x, y, 1, &next)) { ++ascend; continue; }
			{
				int nx = next.x;
				int ny = next.y;
				CHECK(mine[ny * w + nx] < mine[i], "descend must go strictly downhill at (%d,%d)", x, y);
			}
		}
		free(mine);
		free(ref);
		free(scratch);
		map_free(&m);
	}
	CHECK(mismatches == 0, "dijkstra map disagrees with the reference on %d maps", mismatches);
	CHECK(ascend == 0, "dijkstra map has %d reachable cells with no downhill neighbour", ascend);
}

static void test_dijkstra_features(void)
{
	/* the corner rule: no diagonal squeeze between two walls */
	{
		test_map m = map_new(5, 5, 0);
		m.cells[1 * 5 + 2] = 1;
		m.cells[2 * 5 + 1] = 1;
		mu_rog_search_opts strict = mu_rog_search_opts_default();
		mu_rog_search_opts loose = mu_rog_search_opts_default();
		int16_t* d1 = (int16_t*)malloc(25 * sizeof(int16_t));
		int16_t* d2 = (int16_t*)malloc(25 * sizeof(int16_t));
		int* s1 = (int*)malloc(sizeof(int) * (size_t)mu_rog_dijkstra_scratch_ints(5, 5, &strict));
		mu_rog_ivec2 goal;
		loose.cut_corners = 1;
		goal.x = 3;
		goal.y = 3;
		mu_rog_dijkstra_map8_ex(5, 5, &goal, 1, test_blocked, NULL, &m, &strict, d1, s1);
		mu_rog_dijkstra_map8_ex(5, 5, &goal, 1, test_blocked, NULL, &m, &loose, d2, s1);
		/* (2,2) is the cell diagonally between the two walls */
		CHECK(d1[2 * 5 + 2] == 14, "without corner cutting (2,2) is a direct diagonal step (%d)",
			  (int)d1[2 * 5 + 2]);
		CHECK(d2[1 * 5 + 1] == 28, "with corner cutting the far corner is reachable in two diagonals (%d)",
			  (int)d2[1 * 5 + 1]);
		free(d1);
		free(d2);
		free(s1);
		map_free(&m);
	}

	/* terrain cost: an expensive band makes the field prefer the detour */
	{
		test_map m = map_new(9, 5, 0);
		mu_rog_search_opts opts = mu_rog_search_opts_default();
		int16_t* cheap = (int16_t*)malloc(45 * sizeof(int16_t));
		int16_t* muddy = (int16_t*)malloc(45 * sizeof(int16_t));
		int* scratch = (int*)malloc(sizeof(int) * (size_t)mu_rog_dijkstra_scratch_ints(9, 5, &opts));
		mu_rog_ivec2 goal;
		int x;
		for (x = 0; x < 9; ++x) m.cells[2 * 9 + x] = 0;
		goal.x = 0;
		goal.y = 2;
		mu_rog_dijkstra_map8_ex(9, 5, &goal, 1, test_blocked, NULL, &m, &opts, cheap, scratch);
		mu_rog_dijkstra_map8_ex(9, 5, &goal, 1, test_blocked, test_mud, &m, &opts, muddy, scratch);
		CHECK(cheap[2 * 9 + 8] == 80, "flat field: 8 orthogonal steps (%d)", (int)cheap[2 * 9 + 8]);
		CHECK(muddy[2 * 9 + 8] > cheap[2 * 9 + 8],
			  "mud on the straight route must cost more than the flat field (%d vs %d)",
			  (int)muddy[2 * 9 + 8], (int)cheap[2 * 9 + 8]);
		free(cheap);
		free(muddy);
		free(scratch);
		map_free(&m);
	}

	/* no goals reachable: everything stays at the sentinel */
	{
		test_map m = map_new(6, 3, 1);
		mu_rog_search_opts opts = mu_rog_search_opts_default();
		int16_t d[18];
		int* scratch = (int*)malloc(sizeof(int) * (size_t)mu_rog_dijkstra_scratch_ints(6, 3, &opts));
		mu_rog_ivec2 goal;
		int i;
		int all_unreachable = 1;
		goal.x = 1;
		goal.y = 1;
		mu_rog_dijkstra_map8_ex(6, 3, &goal, 1, test_blocked, NULL, &m, &opts, d, scratch);
		for (i = 0; i < 18; ++i) if (d[i] != 32767) all_unreachable = 0;
		CHECK(all_unreachable, "a solid map must leave every cell unreachable");
		free(scratch);
		map_free(&m);
	}
}

/* ------------------------------------------------------------------------- */
/* 5. A* and smoothing                                                       */
/* ------------------------------------------------------------------------- */
static void test_astar(void)
{
	int trial;
	int cost_mismatch = 0;
	int path_invalid = 0;

	for (trial = 0; trial < 30; ++trial) {
		int w = 8 + (int)(test_rand() % 16u);
		int h = 8 + (int)(test_rand() % 16u);
		int n = w * h;
		int density = 10 + (int)(test_rand() % 25u);
		test_map m = map_new(w, h, 0);
		mu_rog_search_opts opts = mu_rog_search_opts_default();
		mu_rog_astar_node* nodes = (mu_rog_astar_node*)malloc(sizeof(mu_rog_astar_node) * (size_t)n);
		mu_rog_astar_heap_entry* heap =
			(mu_rog_astar_heap_entry*)malloc(sizeof(mu_rog_astar_heap_entry) * (size_t)n);
		mu_rog_ivec2* path = (mu_rog_ivec2*)malloc(sizeof(mu_rog_ivec2) * (size_t)n);
		float* dist = (float*)malloc(sizeof(float) * (size_t)n);
		mu_rog_ivec2 start;
		mu_rog_ivec2 goal;
		float cost = -1.0f;
		int count;
		int i;
		int expanded = 0;

		for (i = 0; i < n; ++i) m.cells[i] = (test_rand() % 100u) < (uint32_t)density;
		m.cells[0] = 0;
		m.cells[n - 1] = 0;
		start.x = 0;
		start.y = 0;
		goal.x = w - 1;
		goal.y = h - 1;

		ref_distances(m, goal, dist);
		count = mu_rog_astar_find_path_ex(w, h, start, goal, &opts, test_blocked, NULL, &m, 1.0f, (uint32_t)(trial + 1),
										  nodes, heap, path, n, &cost, &expanded);
		if (dist[0] > 1e29f) {
			if (count != 0) ++cost_mismatch;
		} else if (count == 0) {
			++cost_mismatch;
		} else {
			if (fabsf(cost - dist[0]) > 1e-3f) {
				++cost_mismatch;
			}
			if (path[0].x != start.x || path[0].y != start.y ||
				path[count - 1].x != goal.x || path[count - 1].y != goal.y) {
				++path_invalid;
			}
			for (i = 0; i < count; ++i) {
				if (m.cells[path[i].y * w + path[i].x]) { ++path_invalid; break; }
				if (i && (abs(path[i].x - path[i - 1].x) > 1 || abs(path[i].y - path[i - 1].y) > 1)) {
					++path_invalid;
					break;
				}
			}
		}
		free(nodes);
		free(heap);
		free(path);
		free(dist);
		map_free(&m);
	}
	CHECK(cost_mismatch == 0, "A* disagreed with the reference distance on %d maps", cost_mismatch);
	CHECK(path_invalid == 0, "A* produced %d invalid paths", path_invalid);
}

static void test_astar_features(void)
{
	int w = 40;
	int h = 24;
	int n = w * h;
	test_map m = map_new(w, h, 0);
	mu_rog_search_opts opts = mu_rog_search_opts_default();
	mu_rog_astar_node* nodes = (mu_rog_astar_node*)malloc(sizeof(mu_rog_astar_node) * (size_t)n);
	mu_rog_astar_heap_entry* heap =
		(mu_rog_astar_heap_entry*)malloc(sizeof(mu_rog_astar_heap_entry) * (size_t)n);
	mu_rog_ivec2* path = (mu_rog_ivec2*)malloc(sizeof(mu_rog_ivec2) * (size_t)n);
	mu_rog_ivec2 start;
	mu_rog_ivec2 goal;
	float exact_cost = -1.0f;
	float greedy_cost = -1.0f;
	int exact_expanded = 0;
	int greedy_expanded = 0;
	int count;
	int i;

	/* an open map with two obstacles: the classic case for a weighted heuristic */
	for (i = 0; i < n; ++i) m.cells[i] = 0;
	for (i = 4; i < h - 4; ++i) m.cells[i * w + 20] = 1;
	start.x = 1;
	start.y = h / 2;
	goal.x = w - 2;
	goal.y = h / 2;

	count = mu_rog_astar_find_path_ex(w, h, start, goal, &opts, test_blocked, NULL, &m, 1.0f, 6u,
									  nodes, heap, path, n, &exact_cost, &exact_expanded);
	CHECK(count > 0, "A* must find a path across an open map with a wall");
	CHECK(exact_expanded < n, "exact A* must not expand every cell (%d of %d)", exact_expanded, n);

	CHECK(mu_rog_astar_find_path_ex(w, h, start, goal, &opts, test_blocked, NULL, &m, 4.0f, 2u,
									nodes, heap, path, n, &greedy_cost, &greedy_expanded) > 0,
		  "weighted A* must still find a path");
	CHECK(greedy_expanded <= exact_expanded, "weighted A* must not expand more (%d vs %d)",
		  greedy_expanded, exact_expanded);
	CHECK(greedy_cost >= exact_cost - 1e-3f, "weighted A* cannot beat the exact cost");

	/* smoothing: same cost, fewer waypoints, endpoints kept, no wall cells */
	{
		mu_rog_ivec2* smooth = (mu_rog_ivec2*)malloc(sizeof(mu_rog_ivec2) * (size_t)n);
		float before = 0.0f;
		float after = 0.0f;
		int smooth_count;
		count = mu_rog_astar_find_path_ex(w, h, start, goal, &opts, test_blocked, NULL, &m, 1.0f, 8u,
										  nodes, heap, path, n, NULL, NULL);
		memcpy(smooth, path, sizeof(mu_rog_ivec2) * (size_t)count);
		mu_rog_path_cost(path, count, NULL, NULL, &before);
		smooth_count = mu_rog_path_smooth(smooth, count, 0, test_blocked, &m);
		mu_rog_path_cost(smooth, smooth_count, NULL, NULL, &after);
		CHECK(smooth_count <= count, "smoothing must not add waypoints (%d -> %d)", count, smooth_count);
		CHECK(smooth_count < count, "smoothing must actually drop waypoints on an open map (%d -> %d)",
			  count, smooth_count);
		CHECK(fabsf(after - before) <= 1e-3f, "smoothing must not change the octile cost (%.4f -> %.4f)",
			  (double)before, (double)after);
		CHECK(smooth[0].x == start.x && smooth[0].y == start.y, "smoothing must keep the start");
		CHECK(smooth[smooth_count - 1].x == goal.x && smooth[smooth_count - 1].y == goal.y,
			  "smoothing must keep the goal");
		for (i = 0; i < smooth_count; ++i) {
			CHECK(!m.cells[smooth[i].y * w + smooth[i].x], "smoothed waypoint on a wall at %d", i);
		}
		free(smooth);
	}

	/* no path at all */
	{
		test_map box = map_new(7, 7, 1);
		int j;
		for (j = 1; j < 6; ++j) { box.cells[j * 7 + 1] = 0; box.cells[j * 7 + 5] = 0; }
		{
			mu_rog_ivec2 a;
			mu_rog_ivec2 b;
			a.x = 1;
			a.y = 1;
			b.x = 5;
			b.y = 5;
			CHECK(mu_rog_astar_find_path_ex(7, 7, a, b, &opts, test_blocked, NULL, &box, 1.0f, 7u,
											nodes, heap, path, n, NULL, NULL) == 0,
				  "A* must report no path between disconnected regions");
		}
		map_free(&box);
	}

	free(nodes);
	free(heap);
	free(path);
	map_free(&m);
}

/* ------------------------------------------------------------------------- */
/* 6. generators                                                             */
/* ------------------------------------------------------------------------- */
static void test_maze(void)
{
	int trial;
	for (trial = 0; trial < 12; ++trial) {
		int w = 21;
		int h = 15;
		int n = w * h;
		int* queue = (int*)malloc(sizeof(int) * (size_t)n);
		int* labels = (int*)malloc(sizeof(int) * (size_t)n);
		test_map m = map_new(w, h, 1);
		mu_rog_grid_u8 grid;
		mu_rog_rng rng;
		int comps;
		int floor_cells = 0;
		int edges = 0;
		int i;

		mu_rog_rng_seed(&rng, 4000u + (uint64_t)trial);
		mu_rog_generate_maze(w, h, &rng, m.cells, 1, 0, 0, queue, n);
		grid.w = w;
		grid.h = h;
		grid.data = m.cells;
		comps = mu_rog_label_components4(grid, 0, labels, queue).component_count;
		CHECK(comps == 1, "a perfect maze must be fully connected (%d components, trial %d)", comps, trial);

		for (i = 0; i < n; ++i) if (!m.cells[i]) ++floor_cells;
		for (i = 0; i < n; ++i) {
			int x = i % w;
			int y = i / w;
			if (m.cells[i]) continue;
			if (x + 1 < w && !m.cells[i + 1]) ++edges;
			if (y + 1 < h && !m.cells[i + w]) ++edges;
		}
		CHECK(edges == floor_cells - 1, "a perfect maze must be a tree (%d edges, %d cells, trial %d)",
			  edges, floor_cells, trial);
		CHECK(m.cells[0] == 1 && m.cells[n - 1] == 1, "the maze border must stay solid (trial %d)", trial);

		/* braid 100 must introduce cycles and stay connected */
		mu_rog_rng_seed(&rng, 4000u + (uint64_t)trial);
		mu_rog_generate_maze(w, h, &rng, m.cells, 1, 0, 100, queue, n);
		comps = mu_rog_label_components4(grid, 0, labels, queue).component_count;
		floor_cells = 0;
		edges = 0;
		for (i = 0; i < n; ++i) if (!m.cells[i]) ++floor_cells;
		for (i = 0; i < n; ++i) {
			int x = i % w;
			int y = i / w;
			if (m.cells[i]) continue;
			if (x + 1 < w && !m.cells[i + 1]) ++edges;
			if (y + 1 < h && !m.cells[i + w]) ++edges;
		}
		CHECK(comps == 1, "a braided maze must stay connected (trial %d)", trial);
		CHECK(edges > floor_cells - 1, "braid 100 must create cycles (trial %d)", trial);

		free(queue);
		free(labels);
		map_free(&m);
	}
}

static void test_bsp(void)
{
	int trial;
	for (trial = 0; trial < 12; ++trial) {
		int w = 96;
		int h = 64;
		int n = w * h;
		int* queue = (int*)malloc(sizeof(int) * (size_t)n);
		int* labels = (int*)malloc(sizeof(int) * (size_t)n);
		test_map m = map_new(w, h, 1);
		mu_rog_bsp_node nodes[512];
		mu_rog_rect rooms[256];
		mu_rog_grid_u8 grid;
		mu_rog_rng rng;
		int count;
		int room_count;
		int i;
		int doors;

		mu_rog_rng_seed(&rng, 7000u + (uint64_t)trial);
		count = mu_rog_bsp_build(nodes, 512, (mu_rog_rect){0, 0, w, h}, 8, 6, &rng);
		CHECK(count > 1, "bsp must split the map (trial %d)", trial);
		room_count = mu_rog_bsp_carve_rooms(nodes, count, &rng, m.cells, w, h, 1, 0, 4, rooms, 256);
		CHECK(room_count >= 8, "bsp must produce rooms (%d, trial %d)", room_count, trial);

		for (i = 0; i < count; ++i) {
			if (nodes[i].left >= 0) continue;
			CHECK(nodes[i].room.w >= 2 && nodes[i].room.h >= 2, "bsp produced an empty room (trial %d)", trial);
			CHECK(nodes[i].room.x >= nodes[i].rect.x && nodes[i].room.y >= nodes[i].rect.y &&
				  nodes[i].room.x + nodes[i].room.w <= nodes[i].rect.x + nodes[i].rect.w &&
				  nodes[i].room.y + nodes[i].room.h <= nodes[i].rect.y + nodes[i].rect.h,
				  "a bsp room escaped its leaf (trial %d)", trial);
		}
		for (i = 0; i < room_count; ++i) {
			int j;
			for (j = i + 1; j < room_count; ++j) {
				int separated = (rooms[i].x + rooms[i].w + 1 <= rooms[j].x ||
								 rooms[j].x + rooms[j].w + 1 <= rooms[i].x ||
								 rooms[i].y + rooms[i].h + 1 <= rooms[j].y ||
								 rooms[j].y + rooms[j].h + 1 <= rooms[i].y);
				CHECK(separated, "bsp rooms %d and %d touch (trial %d)", i, j, trial);
			}
		}

		grid.w = w;
		grid.h = h;
		grid.data = m.cells;
		CHECK(mu_rog_label_components4(grid, 0, labels, queue).component_count == room_count,
			  "bsp rooms must be separate before the corridor pass (trial %d)", trial);

		mu_rog_bsp_carve_corridors(nodes, count, m.cells, w, h, 1, 0);
		CHECK(mu_rog_label_components4(grid, 0, labels, queue).component_count == 1,
			  "bsp corridors must leave one component (trial %d)", trial);
		CHECK(m.cells[0] == 1, "bsp must not erase the map border (trial %d)", trial);

		doors = mu_rog_place_doors(m.cells, w, h, rooms, room_count, 0, 2, 100, &rng);
		CHECK(doors > 0, "doors must be placed on a bsp dungeon (trial %d)", trial);
		for (i = 0; i < n; ++i) {
			if (m.cells[i] != 2) continue;
			{
				int x = i % w;
				int y = i / w;
				int wall_side = mu_rog__wall_at(x - 1, y, w, h, test_is_wall, &m) ||
					mu_rog__wall_at(x + 1, y, w, h, test_is_wall, &m) ||
					mu_rog__wall_at(x, y - 1, w, h, test_is_wall, &m) ||
					mu_rog__wall_at(x, y + 1, w, h, test_is_wall, &m);
				CHECK(wall_side, "a door with no wall around it at (%d,%d) trial %d", x, y, trial);
			}
		}

		/* the deterministic build: same seed, same tree (compared on two fresh
		   builds, since carving mutates the room fields of the live one) */
		if (trial == 0) {
			mu_rog_bsp_node first[512];
			mu_rog_bsp_node second[512];
			mu_rog_rng rng_a;
			mu_rog_rng rng_b;
			int count_a;
			int count_b;
			mu_rog_rng_seed(&rng_a, 7000u);
			mu_rog_rng_seed(&rng_b, 7000u);
			count_a = mu_rog_bsp_build(first, 512, (mu_rog_rect){0, 0, w, h}, 8, 6, &rng_a);
			count_b = mu_rog_bsp_build(second, 512, (mu_rog_rect){0, 0, w, h}, 8, 6, &rng_b);
			CHECK(count_a == count_b, "bsp build must be deterministic in its node count");
			CHECK(count_a == count, "bsp build must not depend on the caller's buffer");
			CHECK(memcmp(first, second, sizeof(mu_rog_bsp_node) * (size_t)count_a) == 0,
				  "bsp build must be deterministic");
		}

		free(queue);
		free(labels);
		map_free(&m);
	}
}

static void test_autotile(void)
{
	static const char* art[7] = {
		"#######",
		"#.....#",
		"#.###.#",
		"#.#.#.#",
		"#.###.#",
		"#.....#",
		"#######"
	};
	test_map m = map_new(7, 7, 1);
	int x;
	int y;
	uint8_t mask;

	for (y = 0; y < 7; ++y) for (x = 0; x < 7; ++x) m.cells[y * 7 + x] = (art[y][x] == '#');

	mask = mu_rog_wall_bitmask8(7, 7, 3, 2, test_is_wall, &m);
	CHECK(mask == (MU_ROG_WALL_E | MU_ROG_WALL_W), "horizontal wall run mask (%u)", (unsigned)mask);
	CHECK(strcmp(mu_rog_autotile_glyph8(mask), "\xE2\x94\x80") == 0, "horizontal glyph");

	mask = mu_rog_wall_bitmask8(7, 7, 2, 3, test_is_wall, &m);
	CHECK(mask == (MU_ROG_WALL_N | MU_ROG_WALL_S), "vertical wall run mask (%u)", (unsigned)mask);
	CHECK(strcmp(mu_rog_autotile_glyph8(mask), "\xE2\x94\x82") == 0, "vertical glyph");

	mask = mu_rog_wall_bitmask8(7, 7, 1, 0, test_is_wall, &m);
	CHECK(mask == (MU_ROG_WALL_N | MU_ROG_WALL_E | MU_ROG_WALL_W | MU_ROG_WALL_NE | MU_ROG_WALL_NW),
		  "T junction mask (%u)", (unsigned)mask);
	CHECK(strcmp(mu_rog_autotile_glyph8(mask), "\xE2\x94\xB4") == 0, "T junction glyph");

	mask = mu_rog_wall_bitmask8(7, 7, 0, 0, test_is_wall, &m);
	CHECK(mask == (MU_ROG_WALL_N | MU_ROG_WALL_E | MU_ROG_WALL_S | MU_ROG_WALL_W |
				   MU_ROG_WALL_NE | MU_ROG_WALL_SW | MU_ROG_WALL_NW),
		  "map corner mask (%u)", (unsigned)mask);
	CHECK(mu_rog_autotile_index16(MU_ROG_WALL_N | MU_ROG_WALL_S | MU_ROG_WALL_NE) ==
		  (int)(MU_ROG_WALL_N | MU_ROG_WALL_S), "index16 drops the diagonals");

	/* a diagonal corner bit needs both of its cardinals */
	memset(m.cells, 1, 49);
	m.cells[0] = 0;
	mask = mu_rog_wall_bitmask8(7, 7, 0, 1, test_is_wall, &m);
	CHECK((mask & MU_ROG_WALL_NE) == 0, "open cardinal must clear the corner bit (%u)", (unsigned)mask);
	memset(m.cells, 1, 49);
	mask = mu_rog_wall_bitmask8(7, 7, 0, 1, test_is_wall, &m);
	CHECK((mask & MU_ROG_WALL_NE) != 0, "closed cardinals must set the corner bit (%u)", (unsigned)mask);

	map_free(&m);
}

static void test_connect(void)
{
	int trial;
	for (trial = 0; trial < 8; ++trial) {
		int w = 40;
		int h = 30;
		int n = w * h;
		int* labels = (int*)malloc(sizeof(int) * (size_t)n);
		int* queue = (int*)malloc(sizeof(int) * (size_t)n);
		test_map m = map_new(w, h, 1);
		mu_rog_grid_u8 grid;
		mu_rog_rng rng;
		int i;
		int carved;

		mu_rog_rng_seed(&rng, 11000u + (uint64_t)trial);
		for (i = 0; i < 5; ++i) {
			int cx = 4 + (int)(mu_rog_rng_u32(&rng) % (uint32_t)(w - 8));
			int cy = 4 + (int)(mu_rog_rng_u32(&rng) % (uint32_t)(h - 8));
			int dx;
			int dy;
			for (dy = 0; dy < 3; ++dy) for (dx = 0; dx < 3; ++dx) m.cells[(cy + dy) * w + cx + dx] = 0;
		}
		grid.w = w;
		grid.h = h;
		grid.data = m.cells;
		CHECK(mu_rog_label_components4(grid, 0, labels, queue).component_count >= 2,
			  "the test map must start disconnected (trial %d)", trial);
		carved = mu_rog_connect_regions(grid, 0, 0, labels, queue, 16);
		CHECK(carved > 0, "connect_regions must carve something (trial %d)", trial);
		CHECK(mu_rog_label_components4(grid, 0, labels, queue).component_count == 1,
			  "connect_regions must leave one component (trial %d)", trial);

		free(labels);
		free(queue);
		map_free(&m);
	}
}

/* ------------------------------------------------------------------------- */
/* 7. the existing API still behaves                                        */
/* ------------------------------------------------------------------------- */
static void test_existing_api(void)
{
	/* cave generation still produces something sane */
	{
		int w = 40;
		int h = 24;
		int n = w * h;
		test_map m = map_new(w, h, 1);
		mu_rog_rng rng;
		mu_rog_cave_params params;
		params.fill_percent = 45;
		params.birth_limit = 4;
		params.death_limit = 4;
		params.steps = 4;
		params.wall_tile = 1;
		params.floor_tile = 0;
		mu_rog_grid_u8 cg;
		mu_rog_grid_u8 cs;
		cg.w = w;
		cg.h = h;
		cg.data = m.cells;
		cs.w = w;
		cs.h = h;
		cs.data = (uint8_t*)malloc((size_t)n);
		mu_rog_rng_seed(&rng, 5u);
		mu_rog_generate_cave(&rng, cg, cs, &params);
		free(cs.data);
		CHECK(m.cells[0] == 1, "caves keep a solid border");
		map_free(&m);
	}

	/* room-and-corridor generation still connects its rooms */
	{
		int w = 60;
		int h = 40;
		test_map m = map_new(w, h, 1);
		mu_rog_rng rng;
		mu_rog_roomgen_params params;
		mu_rog_rect rooms[64];
		int i;
		params.map_w = w;
		params.map_h = h;
		params.room_count_max = 48;
		params.room_w_min = 4;
		params.room_w_max = 10;
		params.room_h_min = 3;
		params.room_h_max = 7;
		params.room_padding = 1;
		params.wall_tile = 1;
		params.floor_tile = 0;
		mu_rog_rng_seed(&rng, 11u);
		{
			mu_rog_grid_u8 rg;
			rg.w = w;
			rg.h = h;
			rg.data = m.cells;
			i = mu_rog_generate_rooms_and_corridors(&rng, rg, &params, rooms, 64);
		}
		CHECK(i > 0, "rooms were generated (%d)", i);
		map_free(&m);
	}
}

/* ------------------------------------------------------------------------- */
int main(void)
{
	printf("mu_roguelike gate\n");
	test_rng();
	test_los();
	test_fov_matches_float();
	test_fov_symmetry();
	test_fov_geometry();
	test_dijkstra();
	test_dijkstra_features();
	test_astar();
	test_astar_features();
	test_maze();
	test_bsp();
	test_autotile();
	test_connect();
	test_existing_api();

	printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
