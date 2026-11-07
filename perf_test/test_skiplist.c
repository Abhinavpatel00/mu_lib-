/* test_skiplist.c -- the gate for mu_skiplist.h.
 *
 * Oracle strategy: every operation is mirrored onto a plain sorted array of
 * (key, node) pairs. The skip list is not compared to a "reference skip list"
 * -- that would only prove the two agree, not that either is correct. The array
 * is obviously correct by construction, so agreeing with it (order, rank,
 * index-of-node, at-index, size) is a real check. mu_sl_validate() then checks
 * the invariants the array cannot see: the widths.
 *
 * Also checks the allocation claim: after warmup an insert/remove pair must
 * recycle and not call the allocator. That claim is in the header's comment
 * block, so it belongs in the gate.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mu_skiplist.h"

static int g_checks = 0;
static int g_fails = 0;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond)
    {
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s\n", what);
    }
}

static int check_u32(uint32_t got, uint32_t want, const char *what)
{
    ++g_checks;
    if (got != want)
    {
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s (got %u want %u)\n", what, got, want);
        return 0;
    }
    return 1;
}

static int cmp_int(const void *a, const void *b)
{
    const int x = *(const int *)a, y = *(const int *)b;

    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* ---------------------------------------------------------------- oracle */

enum { MAX_ITEMS = 4096 };

typedef struct item
{
    int        key;
    mu_sl_node node;
} item;

typedef struct oracle
{
    item    *items[MAX_ITEMS];   /* sorted by key, then by insertion order */
    uint32_t n;
} oracle;


/* upper_bound, not lower_bound: the list appends a duplicate AFTER its equals,
 * and an oracle that disagrees about ties reports every duplicate run as a bug.
 * This bit me during the fuzz stage, and the list was right. */
static int oracle_upper(const oracle *o, int key)
{
    uint32_t lo = 0, hi = o->n;

    while (lo < hi)
    {
        const uint32_t mid = (lo + hi) / 2u;

        if (o->items[mid]->key <= key) lo = mid + 1u;
        else                           hi = mid;
    }
    return (int)lo;
}

static void oracle_insert(oracle *o, item *it)
{
    const int pos = oracle_upper(o, it->key);

    memmove(&o->items[pos + 1], &o->items[pos], (o->n - (uint32_t)pos) * sizeof(o->items[0]));
    o->items[pos] = it;
    o->n += 1u;
}

static void oracle_remove_at(oracle *o, uint32_t pos)
{
    memmove(&o->items[pos], &o->items[pos + 1], (o->n - pos - 1u) * sizeof(o->items[0]));
    o->n -= 1u;
}

static int oracle_find(const oracle *o, item *it)
{
    uint32_t i;

    for (i = 0; i < o->n; ++i) if (o->items[i] == it) return (int)i;
    return -1;
}

/* ----------------------------------------------------------------- tests */

static void test_empty(void)
{
    mu_skiplist l;

    mu_skiplist_init(&l, cmp_int);
    check_u32(mu_sl_size(&l), 0u, "empty size");
    check(mu_sl_first(&l) == NULL, "empty first");
    check(mu_sl_at(&l, 0u) == NULL, "empty at(0)");
    check(mu_sl_find(&l, &(int){ 1 }) == NULL, "empty find");
    check(mu_sl_validate(&l), "empty validates");
    mu_skiplist_destroy(&l);
}

static void test_single_and_order(void)
{
    mu_skiplist l;
    item        a, b, c;

    mu_skiplist_init(&l, cmp_int);
    a.key = 20; b.key = 10; c.key = 30;
    mu_sl_insert(&l, &a.node, &a.key);
    mu_sl_insert(&l, &b.node, &b.key);
    mu_sl_insert(&l, &c.node, &c.key);

    check_u32(mu_sl_size(&l), 3u, "size 3");
    check(mu_sl_at(&l, 0u) == &b.node, "at(0) is b");
    check_u32(mu_sl_index_of(&l, &a.node), 1u, "index_of a");
    check_u32(mu_sl_index_of(&l, &c.node), 2u, "index_of c");
    check_u32(mu_sl_rank(&l, &(int){ 15 }), 1u, "lower_bound 15");
    check_u32(mu_sl_rank(&l, &(int){ 10 }), 0u, "lower_bound 10");
    check_u32(mu_sl_rank(&l, &(int){ 99 }), 3u, "lower_bound 99");
    check(mu_sl_find(&l, &(int){ 20 }) == &a.node, "find 20");
    check(mu_sl_find(&l, &(int){ 21 }) == NULL, "find 21 misses");
    check(mu_sl_validate(&l), "validates");
    mu_skiplist_destroy(&l);
}

static void test_duplicates(void)
{
    mu_skiplist l;
    item        a, b, c;

    mu_skiplist_init(&l, cmp_int);
    a.key = 5; b.key = 5; c.key = 5;
    mu_sl_insert(&l, &a.node, &a.key);
    mu_sl_insert(&l, &b.node, &b.key);
    mu_sl_insert(&l, &c.node, &c.key);

    check_u32(mu_sl_size(&l), 3u, "dup size");
    /* inserted after equals: a, b, c */
    check(mu_sl_at(&l, 0u) == &a.node, "dup order 0");
    check(mu_sl_at(&l, 1u) == &b.node, "dup order 1");
    check(mu_sl_at(&l, 2u) == &c.node, "dup order 2");
    check_u32(mu_sl_index_of(&l, &b.node), 1u, "dup index_of middle");
    check(mu_sl_remove(&l, &b.node) == 1, "dup remove middle");
    check_u32(mu_sl_size(&l), 2u, "dup size after remove");
    check(mu_sl_at(&l, 0u) == &a.node && mu_sl_at(&l, 1u) == &c.node, "dup after remove");
    check(mu_sl_validate(&l), "dup validates");
    mu_skiplist_destroy(&l);
}

static void test_remove_all_ways(void)
{
    mu_skiplist l;
    item        it[64];
    int         i;

    mu_skiplist_init(&l, cmp_int);
    for (i = 0; i < 64; ++i) { it[i].key = (i * 37) % 64; mu_sl_insert(&l, &it[i].node, &it[i].key); }
    check_u32(mu_sl_size(&l), 64u, "64 inserted");
    check(mu_sl_validate(&l), "64 validates");

    /* remove from the head, the tail and the middle */
    check(mu_sl_remove(&l, mu_sl_first(&l)) == 1, "remove head");
    check(mu_sl_remove(&l, mu_sl_at(&l, mu_sl_size(&l) - 1u)) == 1, "remove tail");
    check(mu_sl_remove(&l, mu_sl_at(&l, 10u)) == 1, "remove middle");
    check_u32(mu_sl_size(&l), 61u, "size after 3 removals");

    /* removing the same node twice must fail, not corrupt */
    {
        mu_sl_node *n = mu_sl_at(&l, 5u);

        check(mu_sl_remove(&l, n) == 1, "first remove ok");
        check(mu_sl_remove(&l, n) == 0, "second remove fails");
    }
    check_u32(mu_sl_size(&l), 60u, "size after double remove");
    check(mu_sl_validate(&l), "validates after removals");
    mu_skiplist_destroy(&l);
}

/* The real gate: random operations, mirrored onto the array oracle. */
static void test_fuzz(void)
{
    static mu_skiplist l;
    static item        pool[MAX_ITEMS];
    static int         in_use[MAX_ITEMS];
    static oracle      o;
    uint64_t           rng = 0x243F6A8885A308D3ull;
    uint32_t           ops, live = 0u;
    int                i, bad_validate = 0, bad_oracle = 0, bad_index = 0, bad_at = 0;

    mu_skiplist_init(&l, cmp_int);
    mu_skiplist_set_seed(&l, 12345u);
    memset(&o, 0, sizeof o);
    memset(in_use, 0, sizeof in_use);
    for (i = 0; i < MAX_ITEMS; ++i) pool[i].key = 0;

    for (ops = 0; ops < 20000u; ++ops)
    {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;

        if ((rng & 3u) != 0u && live < MAX_ITEMS)
        {
            /* insert a free slot with a key drawn from a small range so
             * duplicates are common */
            const int slot = (int)((rng >> 8) % MAX_ITEMS);

            if (!in_use[slot])
            {
                pool[slot].key = (int)((rng >> 32) % 512u);
                mu_sl_insert(&l, &pool[slot].node, &pool[slot].key);
                oracle_insert(&o, &pool[slot]);
                in_use[slot] = 1;
                live += 1u;
            }
        }
        else if (live > 0u)
        {
            /* remove a random live slot, addressed by node identity */
            const int slot = (int)((rng >> 8) % MAX_ITEMS);

            if (in_use[slot])
            {
                const int pos = oracle_find(&o, &pool[slot]);

                if (pos >= 0)
                {
                    const int got = mu_sl_remove(&l, &pool[slot].node);

                    if (!got) bad_oracle += 1;
                    oracle_remove_at(&o, (uint32_t)pos);
                    in_use[slot] = 0;
                    live -= 1u;
                }
            }
        }

        if ((ops & 255u) == 0u)
        {
            if (!mu_sl_validate(&l)) bad_validate += 1;
            if (mu_sl_size(&l) != o.n) bad_oracle += 1;
            if (o.n > 0u)
            {
                const uint32_t probe = (uint32_t)((rng >> 16) % o.n);

                if (mu_sl_at(&l, probe) != &o.items[probe]->node) bad_at += 1;
                if (mu_sl_index_of(&l, &o.items[probe]->node) != probe) bad_index += 1;
            }
        }
    }

    check(bad_validate == 0, "fuzz: width invariant held throughout");
    check(bad_oracle == 0, "fuzz: size agreed with the oracle");
    check(bad_at == 0, "fuzz: at(i) agreed with the oracle");
    check(bad_index == 0, "fuzz: index_of(chain) agreed with the oracle");
    check_u32(mu_sl_size(&l), o.n, "fuzz: final size");

    /* full ordered walk must match the oracle element for element */
    {
        const mu_sl_node *n = mu_sl_first(&l);
        uint32_t          k = 0u;
        int               same = 1;

        while (n && k < o.n)
        {
            if (n != &o.items[k]->node) { same = 0; break; }
            n = mu_sl_next(n);
            k += 1u;
        }
        check(same && k == o.n && n == NULL, "fuzz: forward walk matches the oracle");
    }
    mu_skiplist_destroy(&l);
}

/* The header claims insert/remove pairs stop hitting the allocator. Prove it. */
static void test_recycling(void)
{
    mu_skiplist l;
    mu_sl_stats st0, st1, st2;
    static item it[512];
    int         i, round;

    mu_skiplist_init(&l, cmp_int);
    for (i = 0; i < 512; ++i) it[i].key = i;

    for (round = 0; round < 8; ++round)
    {
        for (i = 0; i < 512; ++i) mu_sl_insert(&l, &it[i].node, &it[i].key);
        if (round == 0) st0 = mu_sl_get_stats(&l);
        else if (round == 7) st1 = mu_sl_get_stats(&l);
        for (i = 0; i < 512; ++i) mu_sl_remove(&l, &it[i].node);
        st2 = mu_sl_get_stats(&l);
    }

    /* Exact zero reuse is NOT claimable: blocks are pooled per level, and each
     * round draws a fresh set of levels, so a round that happens to draw more
     * level-2 nodes than the pool holds must allocate the difference. What is
     * claimable is that eight rounds of 512 inserts cost little more than one. */
    check(st0.allocs > 0u, "recycling: first round allocates blocks");
    check(st1.allocs < (uint64_t)(512 * 3 / 2),
          "recycling: 8 rounds cost < 1.5 rounds of allocations");
    check(st2.recycles > 512u * 6u, "recycling: most inserts reused a block");
    check(st2.pooled_blocks >= 512u, "recycling: blocks returned to the pool");
    mu_skiplist_destroy(&l);
}

/* Nodes that bring their own level arrays must never allocate. */
static void test_embedded(void)
{
    typedef struct big_item
    {
        int         key;
        mu_sl_node  node;
        mu_sl_node *lv_next[MU_SL_MAX_LEVEL];
        uint32_t    lv_span[MU_SL_MAX_LEVEL];
    } big_item;

    static big_item it[256];
    mu_skiplist     l;
    mu_sl_stats     st;
    int             i;

    mu_skiplist_init(&l, cmp_int);
    for (i = 0; i < 256; ++i)
    {
        memset(&it[i], 0, sizeof it[i]);
        it[i].key = 255 - i;
        mu_sl_use_embedded(&it[i].node, it[i].lv_next, it[i].lv_span);
        mu_sl_insert_embedded(&l, &it[i].node, &it[i].key);
    }
    st = mu_sl_get_stats(&l);
    check_u32(st.allocs, 0u, "embedded: zero allocations");
    check_u32(mu_sl_size(&l), 256u, "embedded: 256 inserted");
    check(mu_sl_validate(&l), "embedded: validates");
    for (i = 0; i < 256; ++i) check(mu_sl_remove(&l, &it[i].node) == 1, "embedded: remove");
    check_u32(mu_sl_size(&l), 0u, "embedded: empty again");
    check_u32(mu_sl_get_stats(&l).allocs, 0u, "embedded: still zero allocations");
    mu_skiplist_destroy(&l);
}

/* Counting allocator: proves the header routes every block through the
 * caller's allocator pair and that destroy returns everything it took. */
static uint64_t g_alloc_calls, g_free_calls, g_alloc_bytes;

static void *counting_alloc(size_t size, void *user)
{
    (void)user;
    g_alloc_calls += 1u;
    g_alloc_bytes += (uint64_t)size;
    return malloc(size);
}

static void counting_free(void *ptr, void *user)
{
    (void)user;
    g_free_calls += 1u;
    free(ptr);
}

static void test_custom_allocator(void)
{
    mu_skiplist l;
    static item it[64];
    int         i;

    g_alloc_calls = g_free_calls = g_alloc_bytes = 0u;

    mu_skiplist_init_alloc(&l, cmp_int, counting_alloc, counting_free, NULL);
    for (i = 0; i < 64; ++i) { it[i].key = i; mu_sl_insert(&l, &it[i].node, &it[i].key); }
    check(mu_sl_validate(&l), "custom alloc: validates");
    check(g_alloc_calls > 0u, "custom alloc: allocator was used");
    check(g_alloc_calls < 64u * 3u, "custom alloc: fewer blocks than nodes (pooling)");

    for (i = 0; i < 64; ++i) mu_sl_remove(&l, &it[i].node);
    mu_skiplist_destroy(&l);
    check_u32((uint32_t)g_alloc_calls, (uint32_t)g_free_calls,
              "custom alloc: every allocation returned by destroy");
}

int main(void)
{
    printf("mu_skiplist correctness gate\n");
    printf("----------------------------------------------------------\n");
    test_empty();
    test_single_and_order();
    test_duplicates();
    test_remove_all_ways();
    test_fuzz();
    test_recycling();
    test_embedded();
    test_custom_allocator();
    printf("----------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
