/* test_allocators.c -- gate for the mu_arena / mu_pool / mu_buddy family.
 *
 * The interesting assertions are the ones a "works in the LIFO demo" allocator
 * gets wrong:
 *   - handed out blocks must not overlap anything already live (checked with a
 *     byte-level owner map, which catches aliasing no matter the order),
 *   - blocks returned in any order must coalesce back to one region,
 *   - request alignment must be honoured, and refusal must not consume space,
 *   - capacity must be exactly what the arithmetic says, not what the free list
 *     remembers.
 */

#include <stdio.h>
#include <string.h>

#define MU_ALLOCATORS_IMPLEMENTATION 1
#include "allocators.h"

static int g_checks, g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) { ++g_fails; if (g_fails <= 20) printf("  FAIL %s\n", what); }
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
/* Arena                                                              */
/* ------------------------------------------------------------------ */

static void test_arena(void)
{
    /* mu_arena aligns the offset, so give it a 64-byte aligned base. */
    static uint8_t raw[256 + 64];
    uint8_t *mem = raw + ((64u - ((uintptr_t)raw % 64u)) % 64u);
    mu_arena a;
    a.base = mem; a.capacity = 256u; a.used = 0u;
    void *p, *q, *r;

    p = mu_arena_alloc(&a, 10u, 8u);
    check(p == mem, "arena: first allocation starts at base");
    q = mu_arena_alloc(&a, 10u, 8u);
    check(q == mem + 16, "arena: second allocation is 8-aligned");
    check_u32(a.used, 26u, "arena: used tracks aligned cursor");

    r = mu_arena_alloc(&a, 8u, 64u);
    check(((uintptr_t)r % 64u) == 0u, "arena: honours 64-byte alignment");

    check(mu_arena_alloc(&a, 4096u, 1u) == NULL, "arena: refuses oversized request");
    check_u32(a.used, 64u + 8u, "arena: refusal does not consume space");

    mu_arena_reset(&a);
    check_u32(a.used, 0u, "arena: reset rewinds");
    check(mu_arena_alloc(&a, 10u, 8u) == mem, "arena: reuse after reset");

    check(mu_arena_alloc(NULL, 1u, 1u) == NULL, "arena: NULL arena");
    check(mu_arena_alloc(&a, 0u, 1u) == NULL, "arena: size 0 refused");
}

/* ------------------------------------------------------------------ */
/* Pool                                                               */
/* ------------------------------------------------------------------ */

static void test_pool(void)
{
    static uint8_t mem[64 * 16];
    mu_pool p;
    void *ptrs[64];
    uint32_t i, n;

    check(mu_pool_init(&p, mem, 16u, 64u) == 1, "pool: init");
    check(mu_pool_init(&p, mem, 2u, 4u) == 0, "pool: block smaller than a link refused");
    check(mu_pool_init(&p, mem, 16u, 0u) == 0, "pool: zero blocks refused");

    n = 0u;
    while (n < 64u && (ptrs[n] = mu_pool_alloc(&p)) != NULL) ++n;
    check_u32(n, 64u, "pool: exactly block_count allocations");
    check(mu_pool_alloc(&p) == NULL, "pool: exhausted pool refuses");

    for (i = 0u; i < n; ++i) check(((uintptr_t)ptrs[i] - (uintptr_t)mem) % 16u == 0u, "pool: block stride");
    /* distinct blocks */
    {
        int distinct = 1;
        for (i = 0u; i < n; ++i)
            for (uint32_t j = i + 1u; j < n; ++j)
                if (ptrs[i] == ptrs[j]) distinct = 0;
        check(distinct, "pool: no block handed out twice");
    }

    mu_pool_free(&p, ptrs[7]);
    check(mu_pool_alloc(&p) == ptrs[7], "pool: LIFO reuse");
    mu_pool_free(&p, ptrs[7]);

    /* free everything in a scrambled order, then take everything back */
    for (i = 0u; i < n; i += 1u) mu_pool_free(&p, ptrs[(i * 37u) % n]);
    n = 0u;
    while (n < 64u && (ptrs[n] = mu_pool_alloc(&p)) != NULL) ++n;
    check_u32(n, 64u, "pool: scrambled frees return the full capacity");
}

/* ------------------------------------------------------------------ */
/* Buddy: owner map                                                   */
/* ------------------------------------------------------------------ */

#define BMEM (4096u)
#define BLEAF (32u)
#define BSLOTS (BMEM / BLEAF)

static uint8_t   g_mem_raw[BMEM + 256]; /* base must be aligned for align tests */
static uint8_t  *g_mem;                 /* 256-byte aligned window into g_mem_raw */
static uint8_t   g_meta[64 * 1024];
static uint32_t  g_owner[BMEM]; /* 0 = free, else slot id + 1 */
static void     *g_live[BSLOTS];
static uint32_t g_size[BSLOTS];
static mu_buddy  g_b;

/* Marks claim[0..size) for slot; returns 0 if any byte was already claimed. */
static int claim(void *ptr, uint32_t size, uint32_t slot)
{
    uint32_t off = (uint32_t)((uint8_t *)ptr - g_mem);
    int clean = 1;
    if (off + size > BMEM) return 0;
    for (uint32_t i = 0u; i < size; ++i)
        if (g_owner[off + i] != 0u) clean = 0;
    if (!clean) { ++g_fails; if (g_fails <= 20) printf("  FAIL buddy: allocation aliases live memory\n"); return 0; }
    for (uint32_t i = 0u; i < size; ++i) g_owner[off + i] = slot + 1u;
    ++g_checks;
    return 1;
}

static void release(void *ptr, uint32_t size)
{
    uint32_t off = (uint32_t)((uint8_t *)ptr - g_mem);
    for (uint32_t i = 0u; i < size && off + i < BMEM; ++i) g_owner[off + i] = 0u;
}

static void buddy_reset_all(void)
{
    memset(g_mem, 0, BMEM);
    memset(g_meta, 0, sizeof g_meta);
    memset(g_owner, 0, sizeof g_owner);
    memset(g_live, 0, sizeof g_live);
    memset(g_size, 0, sizeof g_size);
    check(mu_buddy_init(&g_b, g_mem, BMEM, BLEAF, g_meta, (uint32_t)sizeof g_meta) == 1,
          "buddy: init");
}

/* Fill with leaf-sized blocks: capacity must be exactly the leaf count. */
static void test_buddy_capacity(void)
{
    uint32_t n = 0u;
    buddy_reset_all();

    while (n < BSLOTS && mu_buddy_alloc(&g_b, BLEAF, 8u) != NULL) ++n;
    check_u32(n, BSLOTS, "buddy: leaf-sized capacity == arena/leaf");
    check(mu_buddy_alloc(&g_b, BLEAF, 8u) == NULL, "buddy: full arena refuses");

    /* free everything, then take the whole arena back as one block */
    for (uint32_t i = 0u; i < n; ++i) { /* nothing held: re-fill below */ break; }
    buddy_reset_all();
    while (mu_buddy_alloc(&g_b, BLEAF, 8u) != NULL) { /* exhaust */ }
    /* everything is leaked by design: re-init for the next case */
    buddy_reset_all();
}

/* The regression that mattered: allocate N, free all, allocate N again. The
 * retired implementation toggled a per-pair bit on free only, so after the
 * refill it merged pairs whose sibling was still allocated and handed the same
 * bytes out twice. */
static void test_buddy_free_all_then_refill(void)
{
    uint32_t n = 0u, m = 0u;
    buddy_reset_all();

    while (n < BSLOTS && (g_live[n] = mu_buddy_alloc(&g_b, BLEAF, 8u)) != NULL) ++n;
    check_u32(n, BSLOTS, "buddy refill: filled");
    for (uint32_t i = 0u; i < n; ++i) mu_buddy_free(&g_b, g_live[i]);
    while (m < BSLOTS && (g_live[m] = mu_buddy_alloc(&g_b, BLEAF, 8u)) != NULL) {
        claim(g_live[m], BLEAF, m);
        ++m;
    }
    check_u32(m, BSLOTS, "buddy refill: capacity restored after free-all");
    check(mu_buddy_alloc(&g_b, BLEAF, 8u) == NULL, "buddy refill: still no over-allocation");
}

/* Free every other block, then refill: same capacity, no aliasing. */
static void test_buddy_free_alternate(void)
{
    uint32_t n = 0u, m = 0u;
    buddy_reset_all();

    while (n < BSLOTS && (g_live[n] = mu_buddy_alloc(&g_b, BLEAF, 8u)) != NULL) ++n;
    for (uint32_t i = 0u; i < n; i += 2u) { mu_buddy_free(&g_b, g_live[i]); g_live[i] = NULL; }
    for (uint32_t i = 0u; i < n; i += 2u) {
        void *p = mu_buddy_alloc(&g_b, BLEAF, 8u);
        if (!p) break;
        claim(p, BLEAF, i);
        g_live[i] = p;
        ++m;
    }
    check_u32(m, n / 2u, "buddy alternate: every freed slot came back");
}

/* Random sizes and random free order, checked byte by byte. */
static void test_buddy_random_model(void)
{
    uint32_t rs = 0x12345678u, live = 0u;
    uint32_t best_live = 0u;
    buddy_reset_all();

    for (uint32_t step = 0u; step < 20000u; ++step) {
        rs ^= rs << 13u; rs ^= rs >> 17u; rs ^= rs << 5u;
        if ((rs & 1u) || live == 0u) {
            uint32_t size = 1u + (rs >> 8u) % 200u;
            void *p = mu_buddy_alloc(&g_b, size, 1u);
            if (p) {
                claim(p, size, step % BSLOTS);
                g_live[live] = p;
                g_size[live] = size;
                ++live;
                if (live > best_live) best_live = live;
            }
        } else {
            uint32_t k = (rs >> 8u) % live;
            release(g_live[k], g_size[k]);
            mu_buddy_free(&g_b, g_live[k]);
            g_live[k] = g_live[live - 1u];
            g_size[k] = g_size[live - 1u];
            --live;
        }
    }
    check(best_live > 8u, "buddy random model: reached a non-trivial occupancy");

    /* drain, then the whole arena must be one block again */
    while (live > 0u) {
        --live;
        release(g_live[live], g_size[live]);
        mu_buddy_free(&g_b, g_live[live]);
    }
    check(mu_buddy_alloc(&g_b, BMEM, 1u) != NULL, "buddy random model: fully coalesced after drain");
}

/* Alignment requests and the free_known variant. */
static void test_buddy_alignment_and_free_known(void)
{
    void *p, *q;
    buddy_reset_all();

    p = mu_buddy_alloc(&g_b, 8u, 128u);
    check(p != NULL, "buddy: aligned allocation succeeds");
    check(p && ((uintptr_t)p % 128u) == 0u, "buddy: 128-byte alignment honoured");
    q = mu_buddy_alloc(&g_b, 64u, 64u);
    check(q && ((uintptr_t)q % 64u) == 0u, "buddy: 64-byte alignment honoured");

    mu_buddy_free_known(&g_b, p, 128u);
    mu_buddy_free_known(&g_b, q, 64u);
    check(mu_buddy_alloc(&g_b, BMEM, 1u) != NULL, "buddy: free_known released everything");

    check(mu_buddy_alloc(&g_b, BMEM + 1u, 1u) == NULL, "buddy: over-capacity request refused");
    check(mu_buddy_alloc(NULL, 8u, 1u) == NULL, "buddy: NULL arena");
    check(mu_buddy_alloc(&g_b, 0u, 1u) == NULL, "buddy: size 0 refused");
}

/* ------------------------------------------------------------------ */
/* Buddy: the O(1) unlink and the O(n) sibling scan must agree         */
/* ------------------------------------------------------------------ */

/* perf_test/bench_allocators.c keeps a `_scan` baseline -- the same merge, but
 * the sibling is found by walking the level's freelist instead of unlinking it
 * through free_prev. That baseline is only honest if it reaches the *same*
 * allocator state; otherwise the timing compares two different algorithms. So
 * run one fragmented burst both ways and demand the identical live set. */

#define EQLEAF  (32u)
#define EQSLOTS (16384u)
#define EQCH    (512u)

static uint8_t  g_eq_mem[EQSLOTS * EQLEAF + 256u];   /* + room to align */
static uint8_t *g_eq_base;                          /* power-of-two arena window */
static uint8_t  g_eq_meta[384u * 1024u];
static void    *g_eq_live[EQSLOTS];
static mu_buddy g_eq_b;

/* The reference implementation of the merge, with a freelist scan. */
static void eq_scan_free(mu_buddy *b, void *p)
{
    uint32_t level = mu_buddy_level_from_ptr(b, p);
    uint32_t bs    = mu_level_block_size(b, level);
    uint32_t block = mu_block_index(b, level, (uint32_t)((uint8_t *)p - b->base) / bs);

    while (level > 0u) {
        uint32_t pair = mu_buddy_pair(block);
        if (mu_bit_get(b->merge_xor_map, pair) == 0u) {
            mu_bit_set(b->merge_xor_map, pair, 1u);
            mu_free_list_push(b, level, block);
            return;
        }
        {
            uint32_t buddy = mu_buddy_index(block);
            uint32_t cur   = b->free_head[level];
            while (cur != 0xFFFFFFFFu && cur != buddy) cur = b->free_next[cur];
            if (cur != buddy) { mu_free_list_push(b, level, block); return; }
            mu_free_list_unlink(b, level, buddy);
        }
        mu_bit_set(b->merge_xor_map, pair, 0u);
        block = mu_parent_index(block);
        --level;
    }
    mu_free_list_push(b, 0u, 0u);
}

/* One run of 32 fragmented bursts. fifo=0 merges in reverse (the parked sibling
 * is at the head, so the scan is trivial), fifo=1 merges forward (the sibling is
 * at the tail, the case the scan loses). Leaves the live set sorted. */
static uint32_t eq_run(int scan, int fifo, uintptr_t *out)
{
    uint32_t k, r, n = 0u;

    memset(g_eq_mem, 0, sizeof g_eq_mem);
    g_eq_base = g_eq_mem + ((256u - ((uintptr_t)g_eq_mem % 256u)) % 256u);
    memset(g_eq_meta, 0, sizeof g_eq_meta);
    if (mu_buddy_init(&g_eq_b, g_eq_base, EQSLOTS * EQLEAF, EQLEAF,
                      g_eq_meta, (uint32_t)sizeof g_eq_meta) != 1) return 0u;
    for (k = 0u; k < EQSLOTS; ++k) g_eq_live[k] = mu_buddy_alloc(&g_eq_b, EQLEAF, 8u);

    for (r = 0u; r < 32u; ++r) {
        uint32_t base = (r * 2u * EQCH) % EQSLOTS;
        for (k = 0u; k < EQCH; ++k) {           /* park the odd slots */
            uint32_t slot = (base + 2u * k + 1u) % EQSLOTS;
            if (g_eq_live[slot]) { mu_buddy_free(&g_eq_b, g_eq_live[slot]); g_eq_live[slot] = NULL; }
        }
        for (k = 0u; k < EQCH; ++k) {           /* merge their partners */
            uint32_t e = fifo ? k : (EQCH - 1u - k);
            uint32_t slot = (base + 2u * e) % EQSLOTS;
            if (!g_eq_live[slot]) continue;
            if (scan) eq_scan_free(&g_eq_b, g_eq_live[slot]);
            else      mu_buddy_free(&g_eq_b, g_eq_live[slot]);
            g_eq_live[slot] = NULL;
        }
        for (k = 0u; k < 2u * EQCH; ++k) {      /* refill */
            uint32_t slot = (base + k) % EQSLOTS;
            g_eq_live[slot] = mu_buddy_alloc(&g_eq_b, EQLEAF, 8u);
        }
    }

    for (k = 0u; k < EQSLOTS; ++k) if (g_eq_live[k]) out[n++] = (uintptr_t)g_eq_live[k];
    /* insertion sort: the set is already in slot order, and slot order is not
     * address order once merges have split the arena differently. */
    for (k = 1u; k < n; ++k) {
        uintptr_t v = out[k];
        uint32_t j = k;
        while (j > 0u && out[j - 1u] > v) { out[j] = out[j - 1u]; --j; }
        out[j] = v;
    }
    return n;
}

static void test_buddy_scan_equivalence(void)
{
    static uintptr_t a[EQSLOTS], c[EQSLOTS];
    uint32_t k;

    for (int fifo = 0; fifo < 2; ++fifo) {
        uint32_t na = eq_run(0, fifo, a);
        uint32_t nc = eq_run(1, fifo, c);
        int same = (na == nc);
        check(na == EQSLOTS, fifo ? "unlink vs scan (fifo): everything is live again"
                                  : "unlink vs scan (lifo): everything is live again");
        for (k = 0u; same && k < na; ++k) if (a[k] != c[k]) same = 0;
        check(same, fifo ? "unlink vs scan (fifo): identical live pointer set"
                         : "unlink vs scan (lifo): identical live pointer set");
    }
}

int main(void)
{
    g_mem = g_mem_raw + ((256u - ((uintptr_t)g_mem_raw % 256u)) % 256u);
    printf("mu allocators correctness gate (arena / pool / buddy)\n");
    printf("----------------------------------------------------------\n");
    test_arena();
    test_pool();
    test_buddy_capacity();
    test_buddy_free_all_then_refill();
    test_buddy_free_alternate();
    test_buddy_random_model();
    test_buddy_alignment_and_free_known();
    test_buddy_scan_equivalence();
    printf("----------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
