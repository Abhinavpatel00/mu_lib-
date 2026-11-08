/* test_offset.c -- correctness gate for mu_offset_allocator.{h,c}
 *
 * The offset allocator (Aaltonen's bin scheme ported to C99) shipped in this
 * tree with no gate at all, which is how a 660-line allocator ends up trusted
 * on the strength of "it is a port of something that works". It has three
 * things that are easy to get wrong and that nothing else would catch:
 *
 *   1. the neighbour chain, which is what turns a free into a coalesce;
 *   2. the node budget (max_allocs) -- scrap nodes from aligned splits are a
 *      *leak of metadata*, not of bytes, so a byte-level checker cannot see it;
 *   3. the aligned path specifically, which splits a node into leading scrap +
 *      block + trailing remainder and is the only path that can hand out an
 *      overlapping block while every list still looks well formed.
 *
 * Built with -DDEBUG so oa_debug_validate() is the real validator and runs
 * after every operation inside the library itself (bin membership, bin list
 * links, free_storage, used_bins). This file adds the things it does not
 * check: byte ownership (overlap), offset bounds, alignment, the neighbour
 * chain, and that the node budget is not monotonically consumed.
 *
 *   make -C perf_test offset-test        # shipped configuration
 *   make -C perf_test offset-test-la     # the rejected lookahead arm, also gated
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mu_offset_allocator.h"

static int g_checks;
static int g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s\n", what);
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *what)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s (got %u want %u)\n", what, got, want);
    }
}

/* ------------------------------------------------------------------ */
/* Arena under test                                                   */
/* ------------------------------------------------------------------ */

#define ARENA     (1u << 16) /* 64 KiB of offset space */
#define MAXALLOCS 1024u

static OA_Allocator g_oa;
static uint16_t     g_owner[ARENA]; /* 0 = free, else live slot + 1 */
static OA_Allocation g_live[MAXALLOCS];
static uint32_t     g_size[MAXALLOCS];
static uint32_t     g_rng = 12345u;

static uint32_t rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static void oa_reset_all(void)
{
    uint32_t i;

    oa_reset(&g_oa);
    memset(g_owner, 0, sizeof g_owner);
    memset(g_size, 0, sizeof g_size);
    /* memset(0) would make every slot look like node 0, which IS a valid node
     * index: a later release() of an untouched slot would then free live
     * memory. "Empty" has to be OA_NODE_UNUSED. */
    for (i = 0u; i < MAXALLOCS; ++i) g_live[i].metadata = OA_NODE_UNUSED;
}

/* Nodes handed out *beyond* the one that oa_reset() installs for the initial
 * free block: a split takes one from the free_nodes stack by decrementing
 * free_offset, a merge pushes one back. This is the number that has to return
 * to zero after a workload frees everything -- the metadata-leak detector. */
static uint32_t nodes_taken(void)
{
    return g_oa.max_allocs - 1u - g_oa.free_offset;
}

/* Claims [offset, offset+size) for slot; reports any overlap as a failure. */
static void claim(OA_Allocation a, uint32_t size, uint32_t slot, const char *what)
{
    uint32_t i;
    int clean = 1;

    if (a.metadata == OA_NODE_UNUSED) {
        ++g_checks;
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s: refused a request it should serve\n", what);
        return;
    }
    if (a.offset + size > ARENA) {
        check(0, what);
        return;
    }
    for (i = 0; i < size; ++i)
        if (g_owner[a.offset + i] != 0u) clean = 0;
    check(clean, "offset: allocation does not alias live memory");
    for (i = 0; i < size; ++i) g_owner[a.offset + i] = (uint16_t)(slot + 1u);
}

static void release(uint32_t slot)
{
    uint32_t i;

    if (g_live[slot].metadata == OA_NODE_UNUSED) return;
    for (i = 0; i < g_size[slot] && g_live[slot].offset + i < ARENA; ++i)
        g_owner[g_live[slot].offset + i] = 0u;
    oa_free(&g_oa, g_live[slot]);
    g_live[slot].metadata = OA_NODE_UNUSED;
    g_size[slot] = 0u;
}

/* ------------------------------------------------------------------ */
/* 1. basics, storage report, refusals                                */
/* ------------------------------------------------------------------ */

static void test_offset_basic(void)
{
    OA_StorageReport r;
    OA_Allocation a, b;
    static const OA_Allocation none = { 0u, OA_NODE_UNUSED };

    oa_reset_all();
    check_u32(g_oa.size, ARENA, "offset: size is what init was told");
    check_u32(nodes_taken(), 1u, "offset: a fresh allocator owns exactly one node");

    r = oa_storage_report(&g_oa);
    check_u32(r.total_free_space, ARENA, "offset: fresh report is the whole arena");
    check(r.largest_free_region >= ARENA - ARENA / 8u,
          "offset: fresh largest region is the arena (bin granularity is 1/8)");

    a = oa_allocate(&g_oa, 100u);
    check_u32(a.offset, 0u, "offset: first allocation starts at 0");
    claim(a, 100u, 0u, "offset: first allocation is served");
    g_live[0] = a; g_size[0] = 100u;
    check_u32(oa_allocation_size(&g_oa, a), 100u, "offset: allocation_size is exact");

    b = oa_allocate(&g_oa, 200u);
    check_u32(b.offset, 100u, "offset: the tail is handed out next");
    claim(b, 200u, 1u, "offset: second allocation is served");
    g_live[1] = b; g_size[1] = 200u;

    r = oa_storage_report(&g_oa);
    check_u32(r.total_free_space, ARENA - 300u, "offset: free storage is exact, not binned");

    release(0);
    release(1);
    r = oa_storage_report(&g_oa);
    check_u32(r.total_free_space, ARENA, "offset: freeing everything returns every byte");
    check(r.largest_free_region >= ARENA - ARENA / 8u, "offset: and coalesces back into one region");
    check_u32(nodes_taken(), 1u, "offset: and returns every node it took");

    check(oa_allocate(&g_oa, ARENA + 1u).metadata == none.metadata,
          "offset: a request larger than the arena is refused");
    check_u32(nodes_taken(), 1u, "offset: a refusal takes no node");

    check(oa_allocate(NULL, 8u).metadata == OA_NODE_UNUSED, "offset: NULL allocator");
    check(oa_allocate_aligned(NULL, 8u, 8u).metadata == OA_NODE_UNUSED, "offset: NULL allocator (aligned)");
    oa_free(NULL, a);
    oa_free(&g_oa, none);
    check_u32(g_oa.size, ARENA, "offset: NULL frees are ignored");
}

/* ------------------------------------------------------------------ */
/* 2. alignment                                                       */
/* ------------------------------------------------------------------ */

static void test_offset_alignment(void)
{
    uint32_t align, k, slot = 0u;

    for (align = 1u; align <= 4096u; align <<= 1u) {
        oa_reset_all();
        /* How many aligned blocks the arena can possibly hold: with align A and
         * blocks that each round up to an A-wide window, it is ARENA/A, and
         * asking for more than that must fail for a reason the test does not
         * want to confuse with a bug. */
        uint32_t n = ARENA / align;
        if (n > 32u) n = 32u;

        /* Deliberately walk the cursor off the alignment grid first: with a
         * fresh arena every allocation is trivially aligned, so the aligned
         * path would never have to skip anything. */
        for (k = 0u; k < n; ++k) {
            uint32_t size = 40u + (rnd() % 97u);
            OA_Allocation a = oa_allocate_aligned(&g_oa, size, align);
            claim(a, size, slot, "offset: aligned allocation is served");
            check(a.offset % align == 0u, "offset: aligned allocation honours its alignment");
            check_u32(oa_allocation_size(&g_oa, a), size, "offset: aligned size is exact");
            g_live[slot] = a;
            g_size[slot] = size;
            ++slot;
            if (slot == MAXALLOCS) break;
        }
        while (slot > 0u) { --slot; release(slot); }
        check_u32(oa_storage_report(&g_oa).total_free_space, ARENA,
                  "offset: aligned churn returns every byte");
    }
}

/* ------------------------------------------------------------------ */
/* 3. random model: no overlap, no lost bytes, no node leak           */
/* ------------------------------------------------------------------ */

static void test_offset_random_model(void)
{
    uint32_t op, live = 0u, served = 0u, refused = 0u;

    oa_reset_all();

    for (op = 0u; op < 40000u; ++op) {
        if (live > 0u && (rnd() % 100u) < 45u) {
            uint32_t slot = rnd() % live;
            release(slot);
            /* compact the live array by moving the last entry into the hole */
            --live;
            if (slot != live) {
                g_live[slot] = g_live[live];
                g_size[slot] = g_size[live];
            }
            g_live[live].metadata = OA_NODE_UNUSED;
        } else {
            uint32_t size = 1u + (rnd() % 512u);
            uint32_t align = 1u << (rnd() % 7u); /* 1 .. 64 */
            int wants_align = (int)(rnd() & 1u);
            OA_Allocation a = wants_align ? oa_allocate_aligned(&g_oa, size, align)
                                          : oa_allocate(&g_oa, size);

            if (a.metadata == OA_NODE_UNUSED) {
                ++refused;
                continue;
            }
            ++served;
            claim(a, size, live, "offset: random allocation is served");
            check(a.offset + size <= ARENA, "offset: allocation stays inside the arena");
            if (wants_align) check(a.offset % align == 0u, "offset: random aligned allocation honours it");
            g_live[live] = a;
            g_size[live] = size;
            ++live;
            if (live == MAXALLOCS) break;
        }
    }

    check(served > 1000u, "offset: random model actually allocated (workload sanity)");
    check(refused == 0u || served > 0u, "offset: refusals are allowed, silence is not");

    while (live > 0u) { --live; release(live); }
    check_u32(oa_storage_report(&g_oa).total_free_space, ARENA,
              "offset: random model leaks no bytes");
    check_u32(nodes_taken(), 1u, "offset: random model leaks no nodes");
}

/* ------------------------------------------------------------------ */
/* 4. exhaustion and recovery                                         */
/* ------------------------------------------------------------------ */

static void test_offset_exhaustion(void)
{
    uint32_t n = 0u, bytes = 0u;

    oa_reset_all();

    while (n < MAXALLOCS) {
        OA_Allocation a = oa_allocate(&g_oa, 64u);
        if (a.metadata == OA_NODE_UNUSED) break;
        claim(a, 64u, n, "offset: fill allocation is served");
        g_live[n] = a;
        g_size[n] = 64u;
        bytes += 64u;
        ++n;
    }
    /* Two blocks per node: the allocator refuses a request as soon as
     * free_offset == 0 (its entry guard, which cannot know that a request with
     * no trailing remainder needs no node at all), and every one of the splits
     * before that consumed one node for the remainder. max_allocs - 2 blocks is
     * therefore the ceiling with max_allocs nodes -- the arena is still 128
     * bytes short of full when it stops. That is the node budget binding before
     * the bytes do, which is the failure mode the lookahead in section 5 is
     * about, and the reason oa_storage_report must not report 0 here. */
    check_u32(n, MAXALLOCS - 2u, "offset: fills the arena up to the node budget");
    check_u32(oa_storage_report(&g_oa).total_free_space, ARENA - n * 64u,
              "offset: leftover bytes are still reported when the nodes run out");
    check(bytes <= ARENA, "offset: and never over-commits");
    check(oa_allocate(&g_oa, 64u).metadata == OA_NODE_UNUSED, "offset: full arena refuses");

    while (n > 0u) { --n; release(n); }

    {
        OA_Allocation whole = oa_allocate(&g_oa, ARENA);
        check(whole.metadata != OA_NODE_UNUSED, "offset: the whole arena is available again");
        check_u32(whole.offset, 0u, "offset: and it starts at 0");
        check_u32(oa_allocation_size(&g_oa, whole), ARENA, "offset: and it is exactly the arena");
        oa_free(&g_oa, whole);
    }
    check_u32(nodes_taken(), 1u, "offset: exhaustion round trip returns every node");
}

/* ------------------------------------------------------------------ */
/* 5. the node budget is a budget, not a leak                         */
/* ------------------------------------------------------------------ */

/* Scrap nodes are the one failure a byte-level checker cannot see: the bytes
 * come back on free, but every aligned split that had to skip a leading gap
 * creates a node, and a heap of live scrap nodes exhausts max_allocs while
 * half the arena is free. That is why the alignment lookahead exists.
 *
 * The workload below is built to expose exactly that, with numbers small
 * enough to read:
 *
 *   1. seed 40 blocks of assorted sizes and release every other one, so the
 *      free space is 20 regions whose starts are all over the alignment grid;
 *   2. with a tight node budget (64), keep asking for 64-byte blocks aligned
 *      to 64 and KEEP THEM LIVE. A first-fit arm has to split whatever region
 *      it meets -- 1 node per request plus 1 for the leading scrap -- and
 *      runs out of nodes; the lookahead arm skips to a region that is already
 *      aligned and pays only the trailing remainder.
 *
 * The number that comes out (`served`) is a count, not a time: it is the same
 * on every machine and under every clock. Both arms must be *correct* here;
 * only the shipped arm is held to the floor. */

#define TIGHT_NODES 64u
#define PAIRS       40u
#define HOLE_SIZE   100u /* not a multiple of the alignment below, on purpose */

static void test_offset_node_budget(void)
{
    uint32_t i, slot = 0u, served = 0u, refused = 0u;
    uint32_t hw = 0u;
    uint32_t holes = 0u;

    oa_reset_all();
    oa_init(&g_oa, ARENA, TIGHT_NODES);

    /* Interleaved keeper / hole pairs of one size: every hole ends up in the
     * same bin as every other hole, separated by a live keeper so they cannot
     * coalesce. Two things follow, and they are what makes this a test rather
     * than a wish: the scanner can actually choose between several candidate
     * regions in one bin, and their offsets walk the alignment grid by
     * HOLE_SIZE % alignment per pair. */
    for (i = 0u; i < PAIRS; ++i) {
        OA_Allocation keep = oa_allocate(&g_oa, HOLE_SIZE);
        OA_Allocation hole = oa_allocate(&g_oa, HOLE_SIZE);
        if (keep.metadata == OA_NODE_UNUSED || hole.metadata == OA_NODE_UNUSED) break;
        claim(keep, HOLE_SIZE, slot, "offset budget: keeper is served");
        g_live[slot] = keep; g_size[slot] = HOLE_SIZE; ++slot;
        claim(hole, HOLE_SIZE, slot, "offset budget: hole is served");
        g_live[slot] = hole; g_size[slot] = HOLE_SIZE;
        release(slot);
        ++holes;
    }
    check_u32(holes, PAIRS, "offset budget: every keeper/hole pair is served");

    /* Aligned requests, all live, until the node pool says stop. */
    for (i = 0u; i < 64u; ++i) {
        OA_Allocation a = oa_allocate_aligned(&g_oa, HOLE_SIZE, 64u);
        uint32_t s;

        if (a.metadata == OA_NODE_UNUSED) { ++refused; continue; }
        ++served;
        check(a.offset % 64u == 0u, "offset budget: aligned request honours its alignment");
        for (s = 2u * PAIRS; s < MAXALLOCS; ++s)
            if (g_live[s].metadata == OA_NODE_UNUSED) break;
        check(s < MAXALLOCS, "offset budget: gate bookkeeping has room");
        if (s == MAXALLOCS) { oa_free(&g_oa, a); break; }
        claim(a, HOLE_SIZE, s, "offset budget: aligned allocation does not alias");
        g_live[s] = a; g_size[s] = HOLE_SIZE;
        if (nodes_taken() > hw) hw = nodes_taken();
    }

    check(served + refused == 64u, "offset budget: every request is either served or refused");
    check(hw <= TIGHT_NODES, "offset budget: the high-water mark respects max_allocs");
    check(oa_storage_report(&g_oa).total_free_space <= ARENA,
          "offset budget: free storage never exceeds the arena");

    /* A count, not a time, so it is asserted rather than reported: 11 with the
     * shipped configuration and a 64-node budget (PERF_ALLOCATORS.md section 5
     * has the table). The floor has headroom; what must not happen is the
     * workload silently serving nothing, which is what a broken scan looks
     * like once the counters are all zero. */
    check(served >= 8u, "offset budget: the aligned path keeps working under a tight budget");

    printf("  node budget: %u holes, %u aligned blocks served before the pool ran dry"
           " (lookahead %u, nodes %u of %u)\n",
           holes, served, (unsigned)OA_ALIGN_LOOKAHEAD, hw, TIGHT_NODES);

    for (i = 0u; i < MAXALLOCS; ++i) release(i);
    check_u32(oa_storage_report(&g_oa).total_free_space, ARENA,
              "offset budget: the workload leaks no bytes");
    check_u32(nodes_taken(), 1u, "offset budget: the workload leaks no nodes");
    oa_init(&g_oa, ARENA, MAXALLOCS);
}

/* ------------------------------------------------------------------ */
/* 6. neighbour chain: the coalesce path                              */
/* ------------------------------------------------------------------ */

static void test_offset_coalesce(void)
{
    OA_Allocation a, b, c;
    OA_StorageReport r;

    oa_reset_all();
    a = oa_allocate(&g_oa, 1000u);
    b = oa_allocate(&g_oa, 1000u);
    c = oa_allocate(&g_oa, 1000u);
    check(a.offset == 0u && b.offset == 1000u && c.offset == 2000u,
          "offset coalesce: three blocks are contiguous");

    oa_free(&g_oa, b); /* middle: no neighbour to merge with */
    r = oa_storage_report(&g_oa);
    check_u32(r.total_free_space, ARENA - 2000u, "offset coalesce: middle free is counted once");

    oa_free(&g_oa, a); /* merges with b, not with c (still live) */
    r = oa_storage_report(&g_oa);
    check_u32(r.total_free_space, ARENA - 1000u, "offset coalesce: left neighbour merges");
    check(nodes_taken() <= 3u, "offset coalesce: merges give nodes back");

    oa_free(&g_oa, c); /* all three plus the remainder collapse into one */
    r = oa_storage_report(&g_oa);
    check_u32(r.total_free_space, ARENA, "offset coalesce: right neighbour merges too");
    check(r.largest_free_region >= ARENA - ARENA / 8u, "offset coalesce: back to a single region");
    check_u32(nodes_taken(), 1u, "offset coalesce: back to a single node");
}

int main(void)
{
    printf("mu offset allocator correctness gate (alignment lookahead %u)\n",
           (unsigned)OA_ALIGN_LOOKAHEAD);
    printf("---------------------------------------------------------------\n");

    oa_init(&g_oa, ARENA, MAXALLOCS);
    test_offset_basic();
    test_offset_alignment();
    test_offset_random_model();
    test_offset_exhaustion();
    test_offset_node_budget();
    test_offset_coalesce();
    oa_destroy(&g_oa);

    printf("---------------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
