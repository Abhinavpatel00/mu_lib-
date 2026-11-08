/* bench_offset.c -- cost and behaviour of mu_offset_allocator.{h,c}
 *
 * Two different questions, because they need different instruments:
 *
 *   1. ns per operation, from mu_bench, for the plain path and for the three
 *      distinct aligned paths (exact fit, leading-gap fit, trailing-remainder
 *      fit). These are wall-clock numbers on a shared guest whose sustained
 *      clock moves between 0.64 and 1.07 GHz, so treat them as sizes, not as
 *      ratios: nothing here is claimed to be faster than anything else.
 *
 *   2. exact counts, from --counters: how many nodes a fragmenting workload
 *      consumes, how many nodes the aligned scan visits, and how many of those
 *      visits ended on a region that was already aligned. Counts do not care
 *      what the clock is doing, which is what makes the alignment-lookahead
 *      verdict in PERF_ALLOCATORS.md section 5 a fact rather than a hunch.
 *
 * This file is compiled twice by `make offset-ab`, once with
 * -DOA_ALIGN_LOOKAHEAD=0 (the shipped configuration) and once with 8, so both
 * arms are the same source with the same flags. The counters are compiled in
 * unconditionally with -DOA_COUNT_STEPS; the aligned timing rows therefore
 * include one increment per call, which is stated rather than hidden.
 *
 *   make -C perf_test offset-bench
 *   ./bench_offset --filter=offset --samples=31
 *   make -C perf_test offset-ab
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h" /* first: it sets _POSIX_C_SOURCE */

#include "mu_offset_allocator.h"

#include <string.h>

static volatile unsigned g_sink;

/* Big enough that a 1 MiB arena's node budget never binds during the timing
 * rows; the counters section deliberately uses a small one instead. */
#define ARENA     (1u << 20)
#define NODES     8192u
#define CELL      64u

static OA_Allocator g_oa;
static OA_Allocation g_cell[NODES];

/* ------------------------------------------------------------------ */
/* priming                                                            */
/* ------------------------------------------------------------------ */

/* A run of CELL-sized holes, all in one bin, separated by live keepers so they
 * cannot coalesce. `skew` bytes are burned before each keeper, which walks the
 * holes' offsets across the alignment grid: skew 0 leaves every hole on a
 * multiple of CELL, skew 8 makes every hole start 8 mod 64. */
static void prime_holes(uint32_t holes, uint32_t skew)
{
    uint32_t i, slot = 0u;

    oa_reset(&g_oa);
    for (i = 0u; i < holes; ++i) {
        OA_Allocation keeper, hole;

        if (skew) {
            OA_Allocation pad = oa_allocate(&g_oa, skew);
            (void)pad; /* leaked on purpose: it only exists to move the cursor */
        }
        keeper = oa_allocate(&g_oa, CELL);
        hole = oa_allocate(&g_oa, CELL);
        if (keeper.metadata == OA_NODE_UNUSED || hole.metadata == OA_NODE_UNUSED) break;
        g_cell[slot++] = keeper;
        g_cell[slot++] = hole;
    }
    /* Release the holes: they become the free regions the aligned path sees. */
    for (i = 1u; i < slot; i += 2u) oa_free(&g_oa, g_cell[i]);
}

/* ------------------------------------------------------------------ */
/* 1. ns per op                                                       */
/* ------------------------------------------------------------------ */

MU_BENCH(offset, alloc_free)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		OA_Allocation a = oa_allocate(&g_oa, CELL);
		g_sink += a.offset;
		oa_free(&g_oa, a);
	}
}

MU_BENCH_SETUP(offset, alloc_free)
{
	(void)state;
	oa_reset(&g_oa);
}

/* The aligned path that needs neither a leading nor a trailing node: the free
 * region is exactly CELL and already aligned. */
MU_BENCH(offset, aligned_exact)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		OA_Allocation a = oa_allocate_aligned(&g_oa, CELL, CELL);
		g_sink += a.offset;
		oa_free(&g_oa, a);
	}
}

MU_BENCH_SETUP(offset, aligned_exact)
{
	(void)state;
	prime_holes(64u, 0u);
}

/* The case the lookahead was for: the region is aligned for CELL but the free
 * region starts 8 bytes off a 64-byte boundary, so every allocation skips a
 * 56-byte leading gap, parks it as scrap, and splits off the remainder. */
MU_BENCH(offset, aligned_gap)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		OA_Allocation a = oa_allocate_aligned(&g_oa, CELL, CELL);
		g_sink += a.offset;
		oa_free(&g_oa, a);
	}
}

MU_BENCH_SETUP(offset, aligned_gap)
{
	(void)state;
	prime_holes(64u, 8u);
}

/* Freeing into a live neighbour on both sides: the full coalesce path, twice. */
MU_BENCH(offset, free_merge)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		OA_Allocation a = oa_allocate(&g_oa, 32u);
		g_sink += a.offset;
		oa_free(&g_oa, a);
	}
}

MU_BENCH_SETUP(offset, free_merge)
{
	(void)state;
	/* Two live keepers with a hole between them: the hole is bounded on both
	 * sides, so a free that returns a block into it has to merge twice. */
	oa_reset(&g_oa);
	(void)oa_allocate(&g_oa, 32u);
	g_cell[0] = oa_allocate(&g_oa, 32u);
	oa_free(&g_oa, g_cell[0]);
}

/* ------------------------------------------------------------------ */
/* 2. exact counts (--counters)                                       */
/* ------------------------------------------------------------------ */

/* Hole sizes chosen so the free regions land in one bin (96..103 share the
 * [96,104) bin) while their starting offsets walk the alignment grid. `step`
 * controls how fast they walk: 0 -> every hole is congruent to the first. */
static void run_counters(const char *name, uint32_t holes, uint32_t align,
                         uint32_t req, uint32_t step)
{
    uint32_t i, served = 0u, nodes0;

    oa_reset(&g_oa);
    oa_stat_scan_steps = 0;
    oa_stat_aligned_calls = 0;
    oa_stat_zero_gap_fits = 0;
    oa_stat_gap_fits = 0;

    for (i = 0u; i < holes; ++i) {
        uint32_t hole = 96u + ((step == 0u) ? 0u : (i * step) % 8u);
        OA_Allocation keeper = oa_allocate(&g_oa, 96u);
        OA_Allocation h = oa_allocate(&g_oa, hole);

        if (keeper.metadata == OA_NODE_UNUSED || h.metadata == OA_NODE_UNUSED) break;
        g_cell[2u * i] = keeper;
        g_cell[2u * i + 1u] = h;
    }
    for (i = 0u; i < 2u * holes; i += 2u) oa_free(&g_oa, g_cell[i + 1u]);

    nodes0 = g_oa.max_allocs - 1u - g_oa.free_offset;

    for (i = 0u; i < holes; ++i) {
        OA_Allocation a = oa_allocate_aligned(&g_oa, req, align);
        if (a.metadata == OA_NODE_UNUSED) break;
        g_cell[i] = a;
        ++served;
    }

    printf("  %-26s align %4u req %3u  served %3u/%3u  nodes %5u -> %5u  visits %7llu"
           "  zero-gap %6llu  gap %6llu\n",
           name, align, req, served, holes, nodes0,
           g_oa.max_allocs - 1u - g_oa.free_offset,
           (unsigned long long)oa_stat_scan_steps,
           (unsigned long long)oa_stat_zero_gap_fits,
           (unsigned long long)oa_stat_gap_fits);

    for (i = 0u; i < holes; ++i) oa_free(&g_oa, g_cell[i]);
}

static int run_counters_main(void)
{
    OA_StorageReport r;

    oa_init(&g_oa, 65536u, NODES);

    printf("mu offset allocator, exact counts (alignment lookahead %u)\n",
           (unsigned)OA_ALIGN_LOOKAHEAD);
    printf("--------------------------------------------------------------------\n");
    run_counters("uniform holes",        40u, 64u, 96u, 0u);
    run_counters("uniform holes",        40u, 32u, 96u, 0u);
    run_counters("uniform holes",        40u, 16u, 96u, 0u);
    run_counters("walking holes",        40u, 64u, 96u, 1u);
    run_counters("walking holes",        40u, 32u, 96u, 3u);
    run_counters("walking holes",        40u, 16u, 96u, 1u);
    printf("--------------------------------------------------------------------\n");

    r = oa_storage_report(&g_oa);
    printf("  storage after the last workload: free %u of %u, largest region %u\n",
           r.total_free_space, g_oa.size, r.largest_free_region);
    oa_destroy(&g_oa);
    return 0;
}

static int mu_bench_main(int argc, char **argv);

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--counters") == 0) return run_counters_main();
    }
    oa_init(&g_oa, ARENA, NODES);
    i = mu_bench_main(argc, argv);
    oa_destroy(&g_oa);
    return i;
}
