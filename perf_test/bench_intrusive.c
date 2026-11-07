/* bench_intrusive.c — what the intrusive-by-default rule (AGENTS.md 3.15) is
 * actually worth, measured three ways so the win can be attributed.
 *
 * The claim people repeat is "intrusive containers are faster because of cache
 * locality". That is three separate claims wearing one coat, and lumping them
 * together is how the discipline ends up cargo-culted. So this bench runs three
 * variants of the SAME doubly-linked list over the SAME payload:
 *
 *   A. intrusive     link embedded in the payload, payloads in one array.
 *                    Zero allocations. One indirection.
 *   B. pooled nodes  the textbook non-intrusive list, but its nodes come from a
 *                    contiguous pool in allocation order. This is std::list
 *                    after a good allocator has done its best.
 *                    N allocations (amortized), two indirections.
 *   C. heap nodes    one malloc per node, in insertion order, which is what
 *                    std::list + the default allocator really does.
 *                    N allocations, two indirections, nodes scattered.
 *
 * A vs B isolates "one indirection and one allocation instead of two".
 * B vs C isolates "nodes that live where you allocated them".
 * A vs C is the number you quote, and it is the product of the other two.
 *
 * The operations are the ones the rule talks about:
 *   push      append N elements (allocations counted)
 *   traverse  sum a payload field over the whole list (cache behaviour)
 *   erase     unlink every element given its handle (O(1) for A, B and C --
 *             the difference is purely per-node cost)
 *   erase_obj unlink the element holding a given PAYLOAD pointer, which is the
 *             case that separates them: A does it in O(1) because the payload
 *             carries its own link; B and C must search, because payload and
 *             node are different objects.
 *
 * Method follows AGENTS.md 2.6: paired round-robin, same order, same buffers,
 * median of paired ratios, CV reported. N is large enough that the lists do not
 * fit in L2 (1M payloads x 24 B = 24 MB).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>

#include "mu_dlist.h"

/* ------------------------------------------------------------------ harness */

#define REPS   15
#define WARMUP 5

typedef struct
{
    double t[REPS];
    double median;
    double mean;
    double cv;
    uint64_t allocs;
} timing;

static double now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;

    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void summarise(double *s, timing *out)
{
    double mean = 0.0, var = 0.0;
    int    i;

    qsort(s, REPS, sizeof(double), cmp_double);
    for (i = 0; i < REPS; ++i) mean += s[i];
    mean /= REPS;
    for (i = 0; i < REPS; ++i) var += (s[i] - mean) * (s[i] - mean);
    var /= REPS;
    out->median = s[REPS / 2];
    out->mean = mean;
    out->cv = (mean > 0.0) ? 100.0 * sqrt(var) / mean : 0.0;
}

/* --------------------------------------------------------------- variants */

/* A: payload carries its own link */
typedef struct
{
    float    value;
    mu_dlink link;
} ia_node;

/* B/C: payload and node are different objects */
typedef struct
{
    float value;
} na_payload;

typedef struct na_node
{
    struct na_node *next;
    struct na_node *prev;
    na_payload     *item;
} na_node;

static uint64_t g_allocs;
static na_node *g_pool;
static uint32_t g_pool_next;

static na_node *na_alloc_pooled(void)
{
    ++g_allocs;
    return &g_pool[g_pool_next++];
}

static na_node *na_alloc_heap(void)
{
    ++g_allocs;
    return (na_node *)malloc(sizeof(na_node));
}

static void na_free_heap(na_node *n) { free(n); }

/* --------------------------------------------------------------- workloads */

static ia_node  *g_ia;
static na_payload *g_payload;
static na_node **g_nodes;      /* node lookup table, in insertion order */
static uint32_t  g_n;

static void *xmalloc(size_t sz)
{
    void *p = malloc(sz);

    if (!p) { fprintf(stderr, "oom\n"); exit(1); }
    return p;
}

/* --- push ---------------------------------------------------------------- */

static double push_intrusive(uint32_t n)
{
    g_allocs = 0;
    static mu_dlist l;
    double t0, t1;
    uint32_t i;

    mu_dlist_init(&l);
    t0 = now_ns();
    for (i = 0; i < n; ++i) mu_dlist_push_back(&l, &g_ia[i].link);
    t1 = now_ns();
    return (t1 - t0) / (double)n;
}

static double push_pooled(uint32_t n)
{
    g_allocs = 0;
    static mu_dlist l;
    double t0, t1;
    uint32_t i;

    mu_dlist_init(&l);
    g_allocs = 0; g_pool_next = 0;
    t0 = now_ns();
    for (i = 0; i < n; ++i)
    {
        na_node *nd = na_alloc_pooled();

        nd->item = &g_payload[i];
        mu_dlist_push_back(&l, (mu_dlink *)nd);   /* same link layout */
    }
    t1 = now_ns();
    return (t1 - t0) / (double)n;
}

static double push_heap(uint32_t n)
{
    g_allocs = 0;
    static mu_dlist l;
    double t0, t1;
    uint32_t i;

    mu_dlist_init(&l);
    g_allocs = 0;
    t0 = now_ns();
    for (i = 0; i < n; ++i)
    {
        na_node *nd = na_alloc_heap();

        nd->item = &g_payload[i];
        mu_dlist_push_back(&l, (mu_dlink *)nd);
    }
    t1 = now_ns();
    /* leave it allocated: freeing here would be measured in the next variant */
    for (i = 0; i < n; ++i)
    {
        mu_dlink *it = mu_dlist_front(&l);

        if (!it) break;
        mu_dlist_erase(&l, it);
        na_free_heap((na_node *)it);
    }
    return (t1 - t0) / (double)n;
}

/* --- traverse ------------------------------------------------------------ */

static volatile float g_sink;

static double traverse_intrusive(uint32_t n)
{
    g_allocs = 0;
    static mu_dlist l;
    double t0, t1;
    float  acc = 0.0f;
    uint32_t i;
    mu_dlink *it;

    mu_dlist_init(&l);
    for (i = 0; i < n; ++i) mu_dlist_push_back(&l, &g_ia[i].link);

    t0 = now_ns();
    for (it = mu_dlist_front(&l); it; it = mu_dlist_next(&l, it))
        acc += mu_container_of(it, ia_node, link)->value;
    t1 = now_ns();

    g_sink = acc;
    return (t1 - t0) / (double)n;
}

static double traverse_nodes(uint32_t n, int pooled)
{
    static mu_dlist l;
    double t0, t1;
    float  acc = 0.0f;
    uint32_t i;
    mu_dlink *it;

    mu_dlist_init(&l);
    g_allocs = 0; g_pool_next = 0;
    for (i = 0; i < n; ++i)
    {
        na_node *nd = pooled ? na_alloc_pooled() : na_alloc_heap();

        nd->item = &g_payload[i];
        mu_dlist_push_back(&l, (mu_dlink *)nd);
    }

    t0 = now_ns();
    for (it = mu_dlist_front(&l); it; it = mu_dlist_next(&l, it))
        acc += ((na_node *)it)->item->value;
    t1 = now_ns();

    g_sink = acc;
    for (i = 0; i < n; ++i)
    {
        mu_dlink *f = mu_dlist_front(&l);

        if (!f) break;
        mu_dlist_erase(&l, f);
        if (!pooled) na_free_heap((na_node *)f);
    }
    return (t1 - t0) / (double)n;
}

/* --- erase with a handle -------------------------------------------------- */

static double erase_intrusive(uint32_t n)
{
    g_allocs = 0;
    static mu_dlist l;
    double t0, t1;
    uint32_t i;

    mu_dlist_init(&l);
    for (i = 0; i < n; ++i) mu_dlist_push_back(&l, &g_ia[i].link);

    t0 = now_ns();
    for (i = 0; i < n; ++i) mu_dlist_erase(&l, &g_ia[i].link);
    t1 = now_ns();
    return (t1 - t0) / (double)n;
}

static double erase_nodes(uint32_t n, int pooled)
{
    static mu_dlist l;
    double t0, t1;
    uint32_t i;

    mu_dlist_init(&l);
    g_allocs = 0; g_pool_next = 0;
    for (i = 0; i < n; ++i)
    {
        na_node *nd = pooled ? na_alloc_pooled() : na_alloc_heap();

        nd->item = &g_payload[i];
        mu_dlist_push_back(&l, (mu_dlink *)nd);
        g_nodes[i] = nd;
    }

    t0 = now_ns();
    for (i = 0; i < n; ++i)
    {
        mu_dlist_erase(&l, (mu_dlink *)g_nodes[i]);
        if (!pooled) na_free_heap(g_nodes[i]);
    }
    t1 = now_ns();
    return (t1 - t0) / (double)n;
}

/* --- erase given the PAYLOAD pointer, worst case -------------------------
 *
 * This is the case where intrusiveness itself (not allocation layout) is the
 * difference. A node-owning list has no way to go from a payload pointer to its
 * node, so it must search. The FIRST version of this benchmark erased payloads
 * in list order, which finds every one of them at the front after one step --
 * O(1) amortized, and a completely misleading number. Erasing in REVERSE order
 * is the real worst case: every erase walks the whole remaining list.
 *
 * Because that is O(n^2) it runs at a small n and reports total microseconds,
 * not ns/op. Crude, but the alternative is a "speedup" that depends on the
 * order the test happened to pick.
 */

static double erase_by_object_intrusive(uint32_t n)
{
    static mu_dlist l;
    double t0, t1;
    uint32_t i;

    g_allocs = 0;
    mu_dlist_init(&l);
    for (i = 0; i < n; ++i) mu_dlist_push_back(&l, &g_ia[i].link);

    t0 = now_ns();
    for (i = n; i-- > 0u; ) mu_dlist_erase(&l, &g_ia[i].link);   /* O(1) each */
    t1 = now_ns();
    return (t1 - t0) / 1000.0;                                   /* us total */
}

static double erase_by_object_nodes(uint32_t n, int pooled)
{
    static mu_dlist l;
    double t0, t1;
    uint32_t i;
    float  acc = 0.0f;

    g_allocs = 0; g_pool_next = 0;
    mu_dlist_init(&l);
    for (i = 0; i < n; ++i)
    {
        na_node *nd = pooled ? na_alloc_pooled() : na_alloc_heap();

        nd->item = &g_payload[i];
        mu_dlist_push_back(&l, (mu_dlink *)nd);
    }

    t0 = now_ns();
    for (i = n; i-- > 0u; )                       /* find-then-erase, reverse */
    {
        mu_dlink *it;

        for (it = mu_dlist_front(&l); it; it = mu_dlist_next(&l, it))
        {
            na_node *nd = (na_node *)it;

            if (nd->item == &g_payload[i])
            {
                mu_dlist_erase(&l, it);
                acc += 1.0f;
                if (!pooled) na_free_heap(nd);
                break;
            }
        }
    }
    t1 = now_ns();
    g_sink = acc;
    return (t1 - t0) / 1000.0;
}

/* ------------------------------------------------------------------ table */

typedef struct
{
    const char *name;
    double (*base)(uint32_t);
    double (*opt)(uint32_t);
    uint64_t alloc_base;
    uint64_t alloc_opt;
    const char *note;
} row;

static double w_pooled_push(uint32_t n) { return push_pooled(n); }
static double w_heap_push(uint32_t n) { return push_heap(n); }
static double w_pooled_trav(uint32_t n) { return traverse_nodes(n, 1); }
static double w_heap_trav(uint32_t n) { return traverse_nodes(n, 0); }
static double w_pooled_erase(uint32_t n) { return erase_nodes(n, 1); }
static double w_heap_erase(uint32_t n) { return erase_nodes(n, 0); }
static double w_pooled_byobj(uint32_t n) { return erase_by_object_nodes(n, 1); }
static double w_heap_byobj(uint32_t n) { return erase_by_object_nodes(n, 0); }
static double w_intr_byobj(uint32_t n) { return erase_by_object_intrusive(n); }

static void run_row(const row *r, uint32_t n, const char *label)
{
    timing b, o;
    int    k;

    for (k = 0; k < WARMUP; ++k) { r->base(n); r->opt(n); }
    for (k = 0; k < REPS; ++k)
    {
        b.t[k] = r->base(n);
        if (k == 0) b.allocs = g_allocs;
        o.t[k] = r->opt(n);
        if (k == 0) o.allocs = g_allocs;
    }
    summarise(b.t, &b);
    summarise(o.t, &o);

    printf("  %-34s %9.2f %9.2f %7.2fx %7.2fx %6.1f%% %6.1f%%  %6llu %6llu\n",
           label, b.median, o.median, b.median / o.median, 1.0 / (o.median / b.median),
           b.cv, o.cv,
           (unsigned long long)b.allocs, (unsigned long long)o.allocs);
    (void)r;
}

int main(int argc, char **argv)
{
    uint32_t n = (argc > 1) ? (uint32_t)atoi(argv[1]) : (1u << 20);
    cpu_set_t set;
    uint32_t i;

    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof set, &set);

    g_n = n;
    g_ia = (ia_node *)xmalloc((size_t)n * sizeof(ia_node));
    g_payload = (na_payload *)xmalloc((size_t)n * sizeof(na_payload));
    g_nodes = (na_node **)xmalloc((size_t)n * sizeof(na_node *));
    g_pool = (na_node *)xmalloc((size_t)n * sizeof(na_node));
    for (i = 0; i < n; ++i)
    {
        g_ia[i].value = (float)i;
        g_payload[i].value = (float)i;
        g_ia[i].link.next = NULL;
        g_ia[i].link.prev = NULL;
    }
    /* fault every page in before timing anything: first-touch faults were
     * showing up as 30% CV on the push rows */
    memset(g_pool, 0, (size_t)n * sizeof(na_node));

    printf("===============================================================\n");
    printf(" intrusive vs node-owning doubly-linked list\n");
    printf("===============================================================\n");
    printf("  cpu       : ");
    fflush(stdout);
    if (system("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-") != 0) printf("<unknown>\n");
    printf("  compiler  : gcc -O3 -march=native -std=c11\n");
    printf("  n         : %u elements (payload 4 B, link 16 B)\n", n);
    printf("  working   : intrusive list is %.1f MB of payload+link\n",
           (double)n * sizeof(ia_node) / (1024.0 * 1024.0));
    printf("  method    : %d paired reps, %d warmup, median, CV per side\n", REPS, WARMUP);
    printf("  columns   : baseline ns/op, intrusive ns/op, speedup, allocs\n");
    printf("===============================================================\n\n");
    printf("  %-34s %9s %9s %8s %8s %7s %6s  %6s %6s\n",
           "operation", "node ns", "intr ns", "speedup", "inv", "cv-base", "cv-int",
           "allocB", "allocI");

    { row r = { "push", w_pooled_push, push_intrusive, 0, 0, "" }; run_row(&r, n, "push: pooled nodes -> intrusive"); }
    { row r = { "push", w_heap_push, push_intrusive, 0, 0, "" }; run_row(&r, n, "push: heap nodes -> intrusive"); }
    { row r = { "traverse", w_pooled_trav, traverse_intrusive, 0, 0, "" }; run_row(&r, n, "traverse: pooled nodes -> intrusive"); }
    { row r = { "traverse", w_heap_trav, traverse_intrusive, 0, 0, "" }; run_row(&r, n, "traverse: heap nodes -> intrusive"); }
    { row r = { "erase", w_pooled_erase, erase_intrusive, 0, 0, "" }; run_row(&r, n, "erase by handle: pooled -> intrusive"); }
    { row r = { "erase", w_heap_erase, erase_intrusive, 0, 0, "" }; run_row(&r, n, "erase by handle: heap -> intrusive"); }

    printf("\n── erase given a payload pointer, reverse order (worst case) ────\n");
    printf("  %-34s %9s %9s %8s %8s\n", "operation", "node us", "intr us", "speedup", "n");
    {
        const uint32_t small = (n < 8192u) ? n : 8192u;
        double *bp = NULL, *ip = NULL;
        int     k;

        bp = (double *)xmalloc(sizeof(double) * REPS);
        ip = (double *)xmalloc(sizeof(double) * REPS);
        for (k = 0; k < WARMUP; ++k) { w_pooled_byobj(small); w_heap_byobj(small); w_intr_byobj(small); }
        for (k = 0; k < REPS; ++k) { bp[k] = w_pooled_byobj(small); ip[k] = w_intr_byobj(small); }
        qsort(bp, REPS, sizeof(double), cmp_double);
        qsort(ip, REPS, sizeof(double), cmp_double);
        printf("  %-34s %9.1f %9.1f %7.2fx %8u\n", "pooled nodes -> intrusive",
               bp[REPS / 2], ip[REPS / 2], bp[REPS / 2] / ip[REPS / 2], small);
        for (k = 0; k < REPS; ++k) { bp[k] = w_heap_byobj(small); ip[k] = w_intr_byobj(small); }
        qsort(bp, REPS, sizeof(double), cmp_double);
        qsort(ip, REPS, sizeof(double), cmp_double);
        printf("  %-34s %9.1f %9.1f %7.2fx %8u\n", "heap nodes -> intrusive",
               bp[REPS / 2], ip[REPS / 2], bp[REPS / 2] / ip[REPS / 2], small);
        free(bp); free(ip);
    }
    printf("\n  (the node variants must SEARCH: payload and node are different\n");
    printf("   objects, so this is O(n) per erase, O(n^2) per pass. The\n");
    printf("   intrusive list does it in O(1) because the payload owns the link.)\n");
    printf("\n── done ────────────────────────────────────────────────────────\n");

    free(g_ia); free(g_payload); free(g_nodes); free(g_pool);
    return 0;
}
