/* bench_allocators.c -- what each allocator path costs, ns per operation
 *
 * Rows, all in one binary so both sides of every comparison are compiled with
 * the same flags and read by the same clock:
 *
 *   arena.bump32            mu_arena_alloc(32, 8) into a fresh cursor
 *   pool.alloc_free         mu_pool_alloc + mu_pool_free, the churn pair
 *   pool.free               mu_pool_free of a pre-filled pool, one per op
 *   buddy.alloc_lazy         mu_buddy_alloc(32) from an empty arena (split path)
 *   buddy.pop_park           pop one free block and park it again, sibling left
 *                            allocated so no merge can cascade (per op)
 *   buddy.mergeburst_lifo   ONE ITERATION = 4*CHUNK operations: park CHUNK
 *                           blocks, free their CHUNK partners in reverse (LIFO,
 *                           so every merge finds its sibling at the head), and
 *                           refill. The friendly order.
 *   buddy.mergeburst_fifo   the same, freeing the partners in forward order: the
 *                           wanted sibling is at the tail of the freelist, which
 *                           is the O(n) case for any sibling *scan*.
 *   ..._scan rows           THE BASELINE for both: identical work, but the
 *                           sibling is found by walking the level's freelist
 *                           instead of an O(1) unlink through free_prev. Same
 *                           level detection, same bits, same final state -- only
 *                           the unlink differs, which is what this file exists
 *                           to measure.
 *
 * Every free in every row is a free of a block this file allocated, exactly
 * once: rounds free a window of CHUNK blocks and immediately take the same
 * window back, so no double free can corrupt a freelist (a double free makes
 * the scan baseline spin forever -- that is how the first version of this file
 * died, and why the windows are disjoint by construction now).
 *
 *   make -C perf_test alloc-bench
 *   ./bench_allocators --filter=buddy --samples=21
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"          /* first: sets _POSIX_C_SOURCE */

#define MU_ALLOCATORS_IMPLEMENTATION 1
#include "allocators.h"

static volatile uint32_t g_sink;

#define N       (100000u)
/* Overridable so the same source can measure the merge at two freelist
 * lengths: the scan cost is O(list length), so the crossover is the point. */
#ifndef CHUNK
#  define CHUNK   256u
#endif
#define ARENA   (4u << 20)
#define LEAF    32u

static uint8_t  g_buf[ARENA + 4096];
static uint8_t *g_base;            /* 4096-aligned window into g_buf */
static uint8_t  g_meta[3u << 20];  /* 2 link words per node + bitsets */
static void    *g_ptr[N];

static mu_buddy g_b;
static mu_arena g_a;
static mu_pool  g_p;

static void buddy_fresh(void)
{
	if (!mu_buddy_init(&g_b, g_base, ARENA, LEAF, g_meta, (uint32_t)sizeof g_meta))
		fprintf(stderr, "  (buddy metadata buffer too small)\n");
}

/* ---- the baseline, kept here on purpose ---------------------------------- */

/* The merge path with an O(n) sibling lookup: same level detection, same bits,
 * same final state as mu_buddy_free. Only the unlink differs. */
static void buddy_free_scan(mu_buddy* b, void* ptr)
{
	uint32_t level, block_size, block;

	if (!b || !ptr) return;
	level = mu_buddy_level_from_ptr(b, ptr);
	block_size = mu_level_block_size(b, level);
	block = mu_block_index(b, level, (uint32_t)((uint8_t*)ptr - b->base) / block_size);

	while (level > 0u) {
		uint32_t pair = mu_buddy_pair(block);
		uint32_t buddy, prev, cur;

		if (mu_bit_get(b->merge_xor_map, pair) == 0u) {
			mu_bit_set(b->merge_xor_map, pair, 1u);
			mu_free_list_push(b, level, block);
			return;
		}

		buddy = mu_buddy_index(block);
		prev = 0xFFFFFFFFu;
		cur = b->free_head[level];
		while (cur != 0xFFFFFFFFu && cur != buddy) {
			prev = cur;
			cur = b->free_next[cur];
		}
		if (cur != buddy) {              /* never merge blind */
			mu_free_list_push(b, level, block);
			return;
		}
		if (prev == 0xFFFFFFFFu) b->free_head[level] = b->free_next[cur];
		else b->free_next[prev] = b->free_next[cur];
		if (b->free_next[cur] != 0xFFFFFFFFu) b->free_prev[b->free_next[cur]] = prev;
		b->free_next[cur] = 0xFFFFFFFFu;
		b->free_prev[cur] = 0xFFFFFFFFu;

		mu_bit_set(b->merge_xor_map, pair, 0u);
		block = mu_parent_index(block);
		--level;
	}
	mu_free_list_push(b, 0u, 0u);
}

/* ---- timed rounds -------------------------------------------------------- */

/* Park/pop round: one block is popped from the freelist and parked again, with
 * its sibling left allocated so no merge can cascade. This is the per-op fast
 * path of the allocator, the buddy equivalent of pool.alloc. */
static void buddy_pop_park(mu_buddy* b, uint32_t base)
{
	uint32_t slot = base % N;
	void* p;

	if (g_ptr[slot]) { mu_buddy_free(b, g_ptr[slot]); g_ptr[slot] = NULL; }
	p = mu_buddy_alloc(b, LEAF, 8u);
	g_ptr[slot] = p;
	g_sink += (uint32_t)(uintptr_t)p;
}

/* Fragmented merge burst: ONE ITERATION is
 *   - park CHUNK blocks (odd slots, in order): the sibling is still allocated,
 *     so these are plain parks and the leaf freelist grows to CHUNK entries;
 *   - free the CHUNK partners (even slots) in an order the caller picks: every
 *     one meets a free sibling that sits in that CHUNK-entry list, so the merge
 *     has to FIND and remove it;
 *   - refill all 2*CHUNK slots.
 * 4*CHUNK operations per iteration.
 *
 * The order parameter is the whole story of this benchmark. The parked list is
 * consumed from the head, so
 *   - merge in reverse (LIFO): the wanted sibling is always the head, a scan
 *     finds it in 0 steps, and the two implementations are the same speed;
 *   - merge forward (FIFO): the wanted sibling is at the tail, so a scan walks
 *     CHUNK, CHUNK-1, ... steps -- CHUNK^2/2 per burst, the O(n) case that the
 *     O(1) unlink exists to avoid.
 * Both are real patterns (LIFO is what a scope/frame allocator sees, FIFO what
 * a streaming producer/consumer sees), so the table reports both. */
static void buddy_frag_round(mu_buddy* b, uint32_t base, int scan, int fifo)
{
	uint32_t k;

	for (k = 0u; k < CHUNK; ++k) {
		uint32_t slot = (base + (k * 2u) + 1u) % N;
		void* p = g_ptr[slot];
		if (!p) continue;
		mu_buddy_free(b, p);           /* sibling allocated: plain park */
		g_ptr[slot] = NULL;
		g_sink += slot;
	}

	for (k = 0u; k < CHUNK; ++k) {
		uint32_t e = fifo ? k : (CHUNK - 1u - k);
		uint32_t slot = (base + (e * 2u)) % N;
		void* p = g_ptr[slot];
		if (!p) continue;
		if (scan) buddy_free_scan(b, p);
		else      mu_buddy_free(b, p);  /* merges with a parked sibling */
		g_ptr[slot] = NULL;
		g_sink += slot;
	}

	for (k = 0u; k < CHUNK * 2u; ++k) {
		uint32_t slot = (base + k) % N;
		g_ptr[slot] = mu_buddy_alloc(b, LEAF, 8u);
		g_sink += (uint32_t)(uintptr_t)g_ptr[slot];
	}
}

/* ---- arena / pool -------------------------------------------------------- */

MU_BENCH(arena, bump32)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		if (g_a.used + 32u > g_a.capacity) g_a.used = 0u;   /* wrap: rare */
		g_sink += (uint32_t)(uintptr_t)mu_arena_alloc(&g_a, 32u, 8u);
	}
}

MU_BENCH(pool, alloc_free)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		void* p = mu_pool_alloc(&g_p);
		g_sink += (uint32_t)(uintptr_t)p;
		mu_pool_free(&g_p, p);
	}
}

MU_BENCH_SETUP(pool, alloc_free)
{
	(void)state;
	mu_pool_init(&g_p, g_base, 32u, N);
}


MU_BENCH(pool, free)
{
	uint32_t i;
	MU_BENCH_ITERATE(i)
	{
		uint32_t k = (uint32_t)(i % N);
		void* p = g_ptr[k];
		if (!p) continue;
		mu_pool_free(&g_p, p);
		g_ptr[k] = NULL;
		g_sink += k;
	}
}

MU_BENCH_SETUP(pool, free)
{
	(void)state;
	mu_pool_init(&g_p, g_base, 32u, N);
	for (uint32_t k = 0u; k < N; ++k) g_ptr[k] = mu_pool_alloc(&g_p);
}


/* ---- buddy --------------------------------------------------------------- */

MU_BENCH_EX(buddy, alloc_lazy, 65536)
{
	uint32_t i;
	buddy_fresh();
	MU_BENCH_ITERATE(i)
	{
		void* p = mu_buddy_alloc(&g_b, LEAF, 8u);
		g_sink += (uint32_t)(uintptr_t)p;
	}
}

MU_BENCH(buddy, pop_park)
{
	uint32_t i;
	MU_BENCH_ITERATE(i) buddy_pop_park(&g_b, i * 7u);
}
MU_BENCH_SETUP(buddy, pop_park)
{
	(void)state;
	buddy_fresh();
	/* one live block whose sibling is allocated: the freelist has exactly one
	 * entry at the leaf level, so nothing can cascade. */
	g_ptr[0] = mu_buddy_alloc(&g_b, LEAF, 8u);
	mu_buddy_free(&g_b, g_ptr[0]);
}

MU_BENCH(buddy, mergeburst_lifo)
{
	uint32_t i;
	MU_BENCH_ITERATE(i) buddy_frag_round(&g_b, (uint32_t)((i * 2u * CHUNK) % N), 0, 0);
}
MU_BENCH_SETUP(buddy, mergeburst_lifo)
{
	(void)state;
	buddy_fresh();
	for (uint32_t k = 0u; k < N; ++k) g_ptr[k] = mu_buddy_alloc(&g_b, LEAF, 8u);
}

MU_BENCH(buddy, mergeburst_lifo_scan)
{
	uint32_t i;
	MU_BENCH_ITERATE(i) buddy_frag_round(&g_b, (uint32_t)((i * 2u * CHUNK) % N), 1, 0);
}
MU_BENCH_SETUP(buddy, mergeburst_lifo_scan)
{
	(void)state;
	buddy_fresh();
	for (uint32_t k = 0u; k < N; ++k) g_ptr[k] = mu_buddy_alloc(&g_b, LEAF, 8u);
}

MU_BENCH(buddy, mergeburst_fifo)
{
	uint32_t i;
	MU_BENCH_ITERATE(i) buddy_frag_round(&g_b, (uint32_t)((i * 2u * CHUNK) % N), 0, 1);
}
MU_BENCH_SETUP(buddy, mergeburst_fifo)
{
	(void)state;
	buddy_fresh();
	for (uint32_t k = 0u; k < N; ++k) g_ptr[k] = mu_buddy_alloc(&g_b, LEAF, 8u);
}

MU_BENCH(buddy, mergeburst_fifo_scan)
{
	uint32_t i;
	MU_BENCH_ITERATE(i) buddy_frag_round(&g_b, (uint32_t)((i * 2u * CHUNK) % N), 1, 1);
}
MU_BENCH_SETUP(buddy, mergeburst_fifo_scan)
{
	(void)state;
	buddy_fresh();
	for (uint32_t k = 0u; k < N; ++k) g_ptr[k] = mu_buddy_alloc(&g_b, LEAF, 8u);
}

int main(int argc, char** argv)
{
	g_base = g_buf + ((4096u - ((uintptr_t)g_buf % 4096u)) % 4096u);
	buddy_fresh();
	return mu_bench_main(argc, argv);
}
