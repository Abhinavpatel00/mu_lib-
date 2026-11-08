/* bench_roguelike.c -- what the roguelike algorithms cost, per operation
 *
 * The unit is one operation, not nanoseconds: one field of view, one distance
 * field over the whole map, one path, one dungeon. A per-cell or per-node number
 * is in the row name where it helps, and every row is a paired A/B in this same
 * binary, so the baseline is compiled with the same flags and measured by the
 * same clock:
 *
 *   fov.raycast            the previous FOV (mu_rog_fov_raycast)      baseline
 *   fov.shadowcast         mu_rog_fov_shadowcast                      the replacement
 *   dijkstra.spfa          the previous field, FIFO + re-push         baseline
 *   dijkstra.buckets       mu_rog_dijkstra_map8_ex (Dial + unlink)    the replacement
 *   astar.push_duplicates  the previous A* (duplicate heap pushes)    baseline
 *   astar.decrease_key     mu_rog_astar_find_path_ex                  the replacement
 *   astar.weighted_1_5     the same search at heuristic weight 1.5
 *   astar.plus_smooth      the same search plus mu_rog_path_smooth
 *   gen.rooms_rejection    mu_rog_generate_rooms_and_corridors        baseline
 *   gen.bsp                mu_rog_bsp_build + rooms + corridors
 *   gen.maze_perfect       mu_rog_generate_maze, braid 0
 *   gen.maze_braided       mu_rog_generate_maze, braid 40
 *   gen.autotile           mu_rog_wall_bitmask8 over the whole map
 *
 * The two baselines live in this file on purpose: the header no longer contains
 * them, and a comparison against a re-implementation in the same translation
 * unit is the only way to keep them honest - the compiler inlines both, the map
 * is the same bytes, and the only difference left is the algorithm.
 *
 *   make -C perf_test rogue-bench
 *   ./bench_roguelike --filter=dijkstra --samples=21
 *   ./bench_roguelike --list
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"          /* first: sets _POSIX_C_SOURCE */

#include "mu_roguelike.h"

#include <limits.h>

static volatile uint32_t g_sink;

/* ---- maps ---------------------------------------------------------------- */

#define MAP_W 129
#define MAP_H 97
#define FOV_W 61
#define FOV_H 61

static uint8_t g_map[MAP_W * MAP_H];
static uint8_t g_fov_map[FOV_W * FOV_H];
static int16_t g_dist[MAP_W * MAP_H];
static uint8_t g_vis[FOV_W * FOV_H];
static mu_rog_ivec2 g_path[MAP_W * MAP_H];
static mu_rog_astar_node g_nodes[MAP_W * MAP_H];
static mu_rog_astar_heap_entry g_heap[MAP_W * MAP_H];
static mu_rog_bsp_node g_bsp[512];
static mu_rog_rect g_rooms[256];
static int g_scratch[2 * MAP_W * MAP_H + 64];
static mu_rog_ivec2 g_goals[1];

static int map_blocked(int x, int y, void* user)
{
	(void)user;
	return g_map[y * MAP_W + x] != 0;
}

static int fov_blocked(int x, int y, void* user)
{
	(void)user;
	return g_fov_map[y * FOV_W + x] != 0;
}

/* A dungeon-ish map: rooms on a grid with rubble, so the searches do real work
 * instead of streaming across an empty rectangle. */
static void maps_init(void)
{
	static int done;
	int x;
	int y;
	uint32_t s = 0x1234567u;

	if (done) return;
	done = 1;

	for (y = 0; y < MAP_H; ++y) {
		for (x = 0; x < MAP_W; ++x) {
			int room = (x % 16 >= 3 && x % 16 <= 13) && (y % 12 >= 3 && y % 12 <= 9);
			g_map[y * MAP_W + x] = room ? 0 : 1;
		}
	}
	/* connect the rooms both ways: a corridor row through every room row and a
	   corridor column through every room column. Without both, the map is a set
	   of disconnected bands and every search finishes in a few hundred cells -
	   the first version of this benchmark did exactly that and measured the
	   setup cost of two searches, not the searches. */
	for (y = 6; y < MAP_H; y += 12) {
		for (x = 3; x < MAP_W; ++x) g_map[y * MAP_W + x] = 0;
	}
	for (x = 8; x < MAP_W; x += 16) {
		for (y = 3; y < MAP_H; ++y) g_map[y * MAP_W + x] = 0;
	}
	for (y = 0; y < MAP_H * MAP_W; ++y) {
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		if ((s & 31u) == 0) g_map[y] = 1;
	}
	/* the two endpoints sit in opposite corners of the room grid, clear of the
	   rubble: if either is solid the search exits immediately and the row
	   measures nothing at all */
	g_map[0 * MAP_W + 0] = 0;
	g_map[3 * MAP_W + 3] = 0;
	g_map[(MAP_H - 4) * MAP_W + (MAP_W - 4)] = 0;

	for (y = 0; y < FOV_H; ++y) {
		for (x = 0; x < FOV_W; ++x) {
			int pillar = (x % 6 == 2 && y % 6 == 2);
			g_fov_map[y * FOV_W + x] = pillar ? 1 : 0;
		}
	}
	g_goals[0].x = MAP_W - 4;
	g_goals[0].y = MAP_H - 4;
}

/* ---- baseline A: the previous FOV, the raycast -------------------------- */

/* ---- baseline B: the previous Dijkstra map (FIFO + re-push, "SPFA") ----- */

static int g_spfa_queue[MAP_W * MAP_H * 8];

static void old_dijkstra_map8(int w, int h, const mu_rog_ivec2* goals, int goal_count,
							  mu_rog_cell_blocked_fn blocked, void* user,
							  int16_t* out_dist, int* queue)
{
	int i;
	int qh = 0;
	int qt = 0;
	int n = w * h;
	static const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};

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
		int d;
		for (d = 0; d < 8; ++d) {
			int nx = cx + dirs[d][0];
			int ny = cy + dirs[d][1];
			int nidx;
			int nd;
			if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
			if (blocked && blocked(nx, ny, user)) continue;
			nidx = mu_rog_grid_index(nx, ny, w);
			nd = (int)base + ((dirs[d][0] && dirs[d][1]) ? 14 : 10);
			if (nd < (int)out_dist[nidx]) {
				out_dist[nidx] = (int16_t)nd;
				if (qt < MAP_W * MAP_H * 8) queue[qt++] = nidx;
			}
		}
	}
}

/* The old field ignored cost_fn entirely. To compare like with like on a map
 * with terrain costs, this is the same FIFO field with the multiplier applied:
 * a re-push whenever a cheaper route is found. */
static float mud_mult(int fx, int fy, int tx, int ty, void* user)
{
	(void)fx;
	(void)fy;
	(void)user;
	/* a third of the map is expensive to enter */
	return ((tx * 7 + ty * 3) % 3 == 0) ? 3.0f : 1.0f;
}

static void old_dijkstra_weighted(int w, int h, const mu_rog_ivec2* goals, int goal_count,
								  mu_rog_cell_blocked_fn blocked, mu_rog_move_cost_fn cost_fn,
								  void* user, int16_t* out_dist, int* queue, int queue_cap)
{
	int i;
	int qh = 0;
	int qt = 0;
	int n = w * h;
	static const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};

	for (i = 0; i < n; ++i) out_dist[i] = (int16_t)32767;
	for (i = 0; i < goal_count; ++i) {
		int gx = goals[i].x;
		int gy = goals[i].y;
		if (!mu_rog_in_bounds_i(gx, gy, w, h)) continue;
		if (blocked && blocked(gx, gy, user)) continue;
		out_dist[mu_rog_grid_index(gx, gy, w)] = 0;
		queue[qt++] = mu_rog_grid_index(gx, gy, w);
	}
	while (qh < qt) {
		int cur = queue[qh++];
		int cx = cur % w;
		int cy = cur / w;
		int d;
		for (d = 0; d < 8; ++d) {
			int nx = cx + dirs[d][0];
			int ny = cy + dirs[d][1];
			int nidx;
			int step;
			int nd;
			if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
			if (blocked && blocked(nx, ny, user)) continue;
			step = (dirs[d][0] && dirs[d][1]) ? 14 : 10;
			if (cost_fn) {
				float m = cost_fn(cx, cy, nx, ny, user);
				if (!(m > 0.0f)) continue;
				step = (int)((float)step * m + 0.5f);
			}
			nidx = mu_rog_grid_index(nx, ny, w);
			nd = (int)out_dist[cur] + step;
			if (nd < (int)out_dist[nidx]) {
				out_dist[nidx] = (int16_t)nd;
				if (qt < queue_cap) queue[qt++] = nidx;
			}
		}
	}
}

/* ---- baseline C: the previous A*, duplicate pushes, no decrease-key ----- */

static void old_heap_push(mu_rog_astar_heap_entry* heap, int* count, mu_rog_astar_heap_entry e)
{
	int i = (*count)++;
	heap[i] = e;
	while (i > 0) {
		int p = (i - 1) / 2;
		mu_rog_astar_heap_entry t;
		if (heap[p].f <= heap[i].f) break;
		t = heap[p];
		heap[p] = heap[i];
		heap[i] = t;
		i = p;
	}
}

static mu_rog_astar_heap_entry old_heap_pop(mu_rog_astar_heap_entry* heap, int* count)
{
	mu_rog_astar_heap_entry out = heap[0];
	int n = --(*count);
	heap[0] = heap[n];
	{
		int i = 0;
		for (;;) {
			int l = 2 * i + 1;
			int r = 2 * i + 2;
			int m = i;
			mu_rog_astar_heap_entry t;
			if (l < n && heap[l].f < heap[m].f) m = l;
			if (r < n && heap[r].f < heap[m].f) m = r;
			if (m == i) break;
			t = heap[i];
			heap[i] = heap[m];
			heap[m] = t;
			i = m;
		}
	}
	return out;
}

static int old_astar(int w, int h, mu_rog_ivec2 start, mu_rog_ivec2 goal,
					 mu_rog_cell_blocked_fn blocked, void* user,
					 float* out_cost, int* out_expanded)
{
	int n = w * h;
	int heap_count = 0;
	int found = 0;
	int expanded = 0;
	int start_idx;
	int goal_idx;
	static const int dirs8[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};
	int i;

	if (!mu_rog_in_bounds_i(start.x, start.y, w, h)) return 0;
	if (!mu_rog_in_bounds_i(goal.x, goal.y, w, h)) return 0;
	start_idx = mu_rog_grid_index(start.x, start.y, w);
	goal_idx = mu_rog_grid_index(goal.x, goal.y, w);
	for (i = 0; i < n; ++i) {
		g_nodes[i].g = 3.4e38f;
		g_nodes[i].came_from = -1;
		g_nodes[i].closed = 0;
	}
	g_nodes[start_idx].g = 0.0f;
	{
		mu_rog_astar_heap_entry e;
		e.idx = start_idx;
		e.f = mu_rog_octile_heuristic(goal.x - start.x, goal.y - start.y);
		old_heap_push(g_heap, &heap_count, e);
	}
	while (heap_count > 0) {
		mu_rog_astar_heap_entry e = old_heap_pop(g_heap, &heap_count);
		int cur = e.idx;
		int cx;
		int cy;
		int d;
		if (g_nodes[cur].closed) continue;
		g_nodes[cur].closed = 1;
		++expanded;
		if (cur == goal_idx) { found = 1; break; }
		cx = cur % w;
		cy = cur / w;
		for (d = 0; d < 8; ++d) {
			int nx = cx + dirs8[d][0];
			int ny = cy + dirs8[d][1];
			int nidx;
			float step;
			float ng;
			mu_rog_astar_heap_entry ne;
			if (!mu_rog_in_bounds_i(nx, ny, w, h)) continue;
			if (blocked && blocked(nx, ny, user)) continue;
			nidx = mu_rog_grid_index(nx, ny, w);
			if (g_nodes[nidx].closed) continue;
			step = (dirs8[d][0] && dirs8[d][1]) ? 1.41421356237f : 1.0f;
			ng = g_nodes[cur].g + step;
			if (ng >= g_nodes[nidx].g) continue;
			g_nodes[nidx].g = ng;
			g_nodes[nidx].came_from = cur;
			ne.idx = nidx;
			ne.f = ng + mu_rog_octile_heuristic(goal.x - nx, goal.y - ny);
			old_heap_push(g_heap, &heap_count, ne);
		}
	}
	if (out_expanded) *out_expanded = expanded;
	if (!found) { if (out_cost) *out_cost = 0.0f; return 0; }
	if (out_cost) *out_cost = g_nodes[goal_idx].g;
	return 1;
}

/* ---- benchmarks --------------------------------------------------------- */

MU_BENCH(fov, raycast)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		mu_rog_fov_raycast(FOV_W, FOV_H, FOV_W / 2, FOV_H / 2, 24, fov_blocked, NULL, g_vis);
		g_sink += g_vis[i % (FOV_W * FOV_H)];
	}
}

MU_BENCH(fov, shadowcast)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		mu_rog_fov_shadowcast(FOV_W, FOV_H, FOV_W / 2, FOV_H / 2, 24, fov_blocked, NULL, g_vis);
		g_sink += g_vis[i % (FOV_W * FOV_H)];
	}
}

MU_BENCH(fov, shadowcast_unlimited)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		mu_rog_fov_shadowcast(FOV_W, FOV_H, FOV_W / 2, FOV_H / 2, 0, fov_blocked, NULL, g_vis);
		g_sink += g_vis[i % (FOV_W * FOV_H)];
	}
}

MU_BENCH(dijkstra, spfa_previous)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		old_dijkstra_map8(MAP_W, MAP_H, g_goals, 1, map_blocked, NULL, g_dist, g_spfa_queue);
		g_sink += (uint32_t)g_dist[i % (MAP_W * MAP_H)];
	}
}

MU_BENCH(dijkstra, buckets_dial)
{
	uint32_t i;
	mu_rog_search_opts opts = mu_rog_search_opts_default();
	MU_BENCH_ITERATE(i)
	{
		mu_rog_dijkstra_map8_ex(MAP_W, MAP_H, g_goals, 1, map_blocked, NULL, NULL, &opts, g_dist,
								g_scratch);
		g_sink += (uint32_t)g_dist[i % (MAP_W * MAP_H)];
	}
}

MU_BENCH(dijkstra, spfa_weighted_previous)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		old_dijkstra_weighted(MAP_W, MAP_H, g_goals, 1, map_blocked, mud_mult, NULL, g_dist,
							  g_spfa_queue, (int)(sizeof g_spfa_queue / sizeof g_spfa_queue[0]));
		g_sink += (uint32_t)g_dist[i % (MAP_W * MAP_H)];
	}
}

MU_BENCH(dijkstra, buckets_weighted)
{
	uint32_t i;
	mu_rog_search_opts opts = mu_rog_search_opts_default();
	opts.max_step = 42;          /* 14 * 3, the largest step on this map */
	MU_BENCH_ITERATE(i)
	{
		mu_rog_dijkstra_map8_ex(MAP_W, MAP_H, g_goals, 1, map_blocked, mud_mult, NULL, &opts, g_dist,
								g_scratch);
		g_sink += (uint32_t)g_dist[i % (MAP_W * MAP_H)];
	}
}

MU_BENCH(astar, push_duplicates_previous)
{
	uint32_t i;
	mu_rog_ivec2 start;
	mu_rog_ivec2 goal;
	float cost = 0.0f;
	start.x = 3;
	start.y = 3;
	goal.x = MAP_W - 4;
	goal.y = MAP_H - 4;
	MU_BENCH_ITERATE(i)
	{
		int expanded = 0;
		(void)i;
		old_astar(MAP_W, MAP_H, start, goal, map_blocked, NULL, &cost, &expanded);
		g_sink += expanded + (uint32_t)(cost * 16.0f);
	}
}

MU_BENCH(astar, decrease_key)
{
	uint32_t i;
	mu_rog_ivec2 start;
	mu_rog_ivec2 goal;
	float cost = 0.0f;
	mu_rog_search_opts opts = mu_rog_search_opts_default();
	start.x = 3;
	start.y = 3;
	goal.x = MAP_W - 4;
	goal.y = MAP_H - 4;
	MU_BENCH_ITERATE(i)
	{
		int expanded = 0;
		(void)i;
		mu_rog_astar_find_path_ex(MAP_W, MAP_H, start, goal, &opts, map_blocked, NULL, NULL, 1.0f, (uint32_t)(i + 1),
								  g_nodes, g_heap, g_path, MAP_W * MAP_H, &cost, &expanded);
		g_sink += expanded + (uint32_t)(cost * 16.0f);
	}
}

MU_BENCH(astar, weighted_1_5)
{
	uint32_t i;
	mu_rog_ivec2 start;
	mu_rog_ivec2 goal;
	float cost = 0.0f;
	mu_rog_search_opts opts = mu_rog_search_opts_default();
	start.x = 3;
	start.y = 3;
	goal.x = MAP_W - 4;
	goal.y = MAP_H - 4;
	MU_BENCH_ITERATE(i)
	{
		int expanded = 0;
		(void)i;
		mu_rog_astar_find_path_ex(MAP_W, MAP_H, start, goal, &opts, map_blocked, NULL, NULL, 1.5f, (uint32_t)(i + 1),
								  g_nodes, g_heap, g_path, MAP_W * MAP_H, &cost, &expanded);
		g_sink += expanded + (uint32_t)(cost * 16.0f);
	}
}

MU_BENCH(astar, plus_smooth)
{
	uint32_t i;
	mu_rog_ivec2 start;
	mu_rog_ivec2 goal;
	mu_rog_search_opts opts = mu_rog_search_opts_default();
	start.x = 3;
	start.y = 3;
	goal.x = MAP_W - 4;
	goal.y = MAP_H - 4;
	MU_BENCH_ITERATE(i)
	{
		int count;
		(void)i;
		count = mu_rog_astar_find_path_ex(MAP_W, MAP_H, start, goal, &opts, map_blocked, NULL, NULL,
										  1.0f, (uint32_t)(i + 1), g_nodes, g_heap, g_path, MAP_W * MAP_H, NULL, NULL);
		count = mu_rog_path_smooth(g_path, count, 0, map_blocked, NULL);
		g_sink += (uint32_t)count;
	}
}

MU_BENCH(gen, rooms_rejection)
{
	uint32_t i;
	mu_rog_rng rng;
	mu_rog_roomgen_params params;
	mu_rog_grid_u8 grid;
	params.map_w = MAP_W;
	params.map_h = MAP_H;
	params.room_count_max = 48;
	params.room_w_min = 4;
	params.room_w_max = 10;
	params.room_h_min = 3;
	params.room_h_max = 7;
	params.room_padding = 1;
	params.wall_tile = 1;
	params.floor_tile = 0;
	grid.w = MAP_W;
	grid.h = MAP_H;
	grid.data = g_map;
	MU_BENCH_ITERATE(i)
	{
		mu_rog_rng_seed(&rng, 1u + i);
		mu_rog_generate_rooms_and_corridors(&rng, grid, &params, g_rooms, 256);
		g_sink += g_map[i % (MAP_W * MAP_H)];
	}
}

MU_BENCH(gen, bsp)
{
	uint32_t i;
	mu_rog_rng rng;
	mu_rog_rect bounds;
	bounds.x = 0;
	bounds.y = 0;
	bounds.w = MAP_W;
	bounds.h = MAP_H;
	MU_BENCH_ITERATE(i)
	{
		int count;
		mu_rog_rng_seed(&rng, 1u + i);
		MU_MEMSET(g_map, 1, (size_t)MAP_W * MAP_H);
		count = mu_rog_bsp_build(g_bsp, 512, bounds, 8, 6, &rng);
		mu_rog_bsp_carve_rooms(g_bsp, count, &rng, g_map, MAP_W, MAP_H, 1, 0, 4, g_rooms, 256);
		mu_rog_bsp_carve_corridors(g_bsp, count, g_map, MAP_W, MAP_H, 1, 0);
		g_sink += g_map[i % (MAP_W * MAP_H)];
	}
}

MU_BENCH(gen, maze_perfect)
{
	uint32_t i;
	mu_rog_rng rng;
	MU_BENCH_ITERATE(i)
	{
		mu_rog_rng_seed(&rng, 1u + i);
		mu_rog_generate_maze(MAP_W, MAP_H, &rng, g_map, 1, 0, 0, g_scratch, (int)(sizeof g_scratch / sizeof g_scratch[0]));
		g_sink += g_map[i % (MAP_W * MAP_H)];
	}
}

MU_BENCH(gen, maze_braided_40)
{
	uint32_t i;
	mu_rog_rng rng;
	MU_BENCH_ITERATE(i)
	{
		mu_rog_rng_seed(&rng, 1u + i);
		mu_rog_generate_maze(MAP_W, MAP_H, &rng, g_map, 1, 0, 40, g_scratch, (int)(sizeof g_scratch / sizeof g_scratch[0]));
		g_sink += g_map[i % (MAP_W * MAP_H)];
	}
}

MU_BENCH(gen, autotile_masks)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		int x;
		int y;
		uint32_t acc = 0;
		for (y = 1; y < MAP_H - 1; ++y)
			for (x = 1; x < MAP_W - 1; ++x)
				acc += mu_rog_wall_bitmask8(MAP_W, MAP_H, x, y, map_blocked, NULL);
		g_sink += acc + (uint32_t)i;
	}
}

int main(int argc, char** argv)
{
	maps_init();
	return mu_bench_main(argc, argv);
}
