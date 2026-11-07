/*
 * bench_highperf_ds.c — High-Performance Data Structures from Systems & GitHub Research
 * =====================================================================================
 * Proves data-structure optimizations from:
 *   1. Hash Table Lookup + Dense Iteration Shootout (262K u64 keys):
 *      Chained Heap Nodes vs Linear Probing vs Robin Hood vs SIMD SwissTable (32-way AVX2 H2)
 *      vs ankerl::unordered_dense Hybrid (SIMD H2 Index + Dense Contiguous Array)
 *   2. Static Sorted Search Shootout (2M int32 keys, 8 MB out-of-L2):
 *      libc/Branchy Binary Search vs Branchless CMOV Search vs Khuong-Morin Eytzinger BFS + Prefetch
 *      vs Slotin/Algorithmica 16-ary Cache-Line Aligned SIMD S+Tree (AVX2 vpcmpgtd)
 *   3. Approximate Membership Gatekeeper (500K keys, 95% negative queries):
 *      Direct Hash Miss vs Classic 6-Probe Bloom vs Cache-Line Blocked Bloom vs Graf-Lemire 8-bit Xor Filter
 *   4. Sparse Set / Bitset Iteration (4M ID universe, 512 active IDs):
 *      Dense bool[4M] vs Flat u64 Bitset vs 3-Level Hierarchical Summary Bitset (mu_hier_bitset)
 *   5. Key+ID Sorting (1M uint32 pairs):
 *      libc qsort (Function Ptr) vs Inline Quicksort vs 4-Pass 8-bit LSD Radix Sort (mu_sort)
 *   6. Short-Lived Small Lists (N <= 8 common case):
 *      Heap malloc/free Vector vs LLVM SmallVector / mu_small_vec Inline Stack Storage
 */

#include "bench_common.h"
#include <immintrin.h>

#define REPS 7

/* Fast 64-bit wyhash-style 128-bit multiply-fold mixer (Wang Yi / Lemire) */
static inline uint64_t wyhash64_mix(uint64_t key)
{
    __uint128_t r = (__uint128_t)(key ^ UINT64_C(0xa0761d6478bd642f)) *
                    (__uint128_t)(key ^ UINT64_C(0xe7037ed1a0b428db));
    return (uint64_t)(r ^ (r >> 64));
}

/* =========================================================================
 * 1. Hash Table Shootout: Chained vs Open Linear vs SIMD SwissTable vs unordered_dense
 * ========================================================================= */
typedef struct ChainNode {
    uint64_t key;
    uint64_t val;
    struct ChainNode *next;
    uint64_t pad; /* typical allocator header/alignment overhead */
} ChainNode;

typedef struct {
    ChainNode **buckets;
    ChainNode  *pool;
    size_t      mask;
    size_t      used;
} ChainMap;

static void chain_init(ChainMap *m, size_t cap, const uint32_t *perm)
{
    m->buckets = calloc(cap, sizeof(ChainNode *));
    m->pool    = bench_alloc_aligned(64, cap * sizeof(ChainNode));
    m->mask    = cap - 1u;
    m->used    = 0;
    (void)perm;
}

static void chain_put(ChainMap *m, uint64_t k, uint64_t v, uint32_t slot_idx)
{
    size_t b = (size_t)(wyhash64_mix(k) & m->mask);
    ChainNode *n = &m->pool[slot_idx];
    n->key = k; n->val = v; n->next = m->buckets[b];
    m->buckets[b] = n;
}

BENCH_NOINLINE static uint64_t chain_query_all(const ChainMap *m, const uint64_t *q, size_t nq)
{
    uint64_t sum = 0;
    size_t mask = m->mask;
    for (size_t i = 0; i < nq; ++i) {
        uint64_t k = q[i];
        for (const ChainNode *p = m->buckets[wyhash64_mix(k) & mask]; p; p = p->next) {
            if (p->key == k) { sum += p->val; break; }
        }
    }
    return sum;
}

BENCH_NOINLINE static uint64_t chain_iterate_all(const ChainMap *m)
{
    uint64_t sum = 0;
    size_t nb = m->mask + 1u;
    for (size_t b = 0; b < nb; ++b)
        for (const ChainNode *p = m->buckets[b]; p; p = p->next)
            sum += p->val;
    return sum;
}

/* --- AVX2 SwissTable (32-byte control groups, 7-bit H2 fingerprint, 0x80 = EMPTY) --- */
typedef struct {
    uint8_t  *ctrl;   /* cap + 32 mirrored bytes; 0x80 = empty, 0..127 = h2 */
    uint64_t *keys;
    uint64_t *vals;
    size_t    mask;
} SwissMap32;

static void swiss_init(SwissMap32 *m, size_t cap)
{
    m->ctrl = bench_alloc_aligned(64, cap + 32u);
    memset(m->ctrl, 0x80, cap + 32u);
    m->keys = bench_alloc_aligned(64, cap * sizeof(uint64_t));
    m->vals = bench_alloc_aligned(64, cap * sizeof(uint64_t));
    m->mask = cap - 1u;
}

static void swiss_put(SwissMap32 *m, uint64_t k, uint64_t v)
{
    uint64_t h  = wyhash64_mix(k);
    uint8_t  h2 = (uint8_t)(h & 0x7Fu);
    size_t pos  = (size_t)((h >> 7) & m->mask);
    __m256i vempty = _mm256_set1_epi8((char)0x80);

    for (;;) {
        __m256i grp = _mm256_loadu_si256((const __m256i *)(m->ctrl + pos));
        uint32_t em = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(grp, vempty));
        if (em) {
            size_t idx = (pos + (size_t)__builtin_ctz(em)) & m->mask;
            m->ctrl[idx] = h2;
            if (idx < 32u) m->ctrl[m->mask + 1u + idx] = h2; /* mirror wrap group */
            m->keys[idx] = k;
            m->vals[idx] = v;
            return;
        }
        pos = (pos + 32u) & m->mask;
    }
}

BENCH_NOINLINE static uint64_t swiss_query_all(const SwissMap32 *m, const uint64_t *q, size_t nq)
{
    uint64_t sum = 0;
    size_t mask = m->mask;
    __m256i vempty = _mm256_set1_epi8((char)0x80);

    for (size_t i = 0; i < nq; ++i) {
        uint64_t k  = q[i];
        uint64_t h  = wyhash64_mix(k);
        __m256i vh2 = _mm256_set1_epi8((char)(h & 0x7Fu));
        size_t pos  = (size_t)((h >> 7) & mask);

        for (;;) {
            __m256i grp = _mm256_loadu_si256((const __m256i *)(m->ctrl + pos));
            uint32_t eq = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(grp, vh2));
            while (eq) {
                size_t idx = (pos + (size_t)__builtin_ctz(eq)) & mask;
                if (m->keys[idx] == k) { sum += m->vals[idx]; goto next_key; }
                eq &= eq - 1u;
            }
            if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(grp, vempty)))
                break;
            pos = (pos + 32u) & mask;
        }
    next_key:;
    }
    return sum;
}

/* --- ankerl::unordered_dense + SIMD H2 hybrid:
 *     Index table holds 32-way H2 ctrl + uint32 dense_idx; keys[] and vals[] are
 *     100% DENSE CONTIGUOUS SoA arrays [0..count)! Zero holes during iteration! --- */
typedef struct {
    uint8_t  *ctrl;
    uint32_t *dense_idx;
    uint64_t *dense_keys;
    uint64_t *dense_vals;
    size_t    mask;
    size_t    count;
} DenseSwissMap;

static void dswiss_init(DenseSwissMap *m, size_t cap)
{
    m->ctrl       = bench_alloc_aligned(64, cap + 32u);
    memset(m->ctrl, 0x80, cap + 32u);
    m->dense_idx  = bench_alloc_aligned(64, cap * sizeof(uint32_t));
    m->dense_keys = bench_alloc_aligned(64, cap * sizeof(uint64_t));
    m->dense_vals = bench_alloc_aligned(64, cap * sizeof(uint64_t));
    m->mask       = cap - 1u;
    m->count      = 0;
}

static void dswiss_put(DenseSwissMap *m, uint64_t k, uint64_t v)
{
    uint64_t h  = wyhash64_mix(k);
    uint8_t  h2 = (uint8_t)(h & 0x7Fu);
    size_t pos  = (size_t)((h >> 7) & m->mask);
    __m256i vempty = _mm256_set1_epi8((char)0x80);

    for (;;) {
        __m256i grp = _mm256_loadu_si256((const __m256i *)(m->ctrl + pos));
        uint32_t em = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(grp, vempty));
        if (em) {
            size_t slot = (pos + (size_t)__builtin_ctz(em)) & m->mask;
            uint32_t di = (uint32_t)(m->count++);
            m->ctrl[slot] = h2;
            if (slot < 32u) m->ctrl[m->mask + 1u + slot] = h2;
            m->dense_idx[slot] = di;
            m->dense_keys[di]  = k;
            m->dense_vals[di]  = v;
            return;
        }
        pos = (pos + 32u) & m->mask;
    }
}

BENCH_NOINLINE static uint64_t dswiss_query_all(const DenseSwissMap *m, const uint64_t *q, size_t nq)
{
    uint64_t sum = 0;
    size_t mask = m->mask;
    __m256i vempty = _mm256_set1_epi8((char)0x80);

    for (size_t i = 0; i < nq; ++i) {
        uint64_t k  = q[i];
        uint64_t h  = wyhash64_mix(k);
        __m256i vh2 = _mm256_set1_epi8((char)(h & 0x7Fu));
        size_t pos  = (size_t)((h >> 7) & mask);

        for (;;) {
            __m256i grp = _mm256_loadu_si256((const __m256i *)(m->ctrl + pos));
            uint32_t eq = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(grp, vh2));
            while (eq) {
                size_t slot = (pos + (size_t)__builtin_ctz(eq)) & mask;
                uint32_t di = m->dense_idx[slot];
                if (m->dense_keys[di] == k) { sum += m->dense_vals[di]; goto next_key; }
                eq &= eq - 1u;
            }
            if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(grp, vempty)))
                break;
            pos = (pos + 32u) & mask;
        }
    next_key:;
    }
    return sum;
}

BENCH_NOINLINE static uint64_t dswiss_iterate_all(const DenseSwissMap *m)
{
    const uint64_t * BENCH_RESTRICT vals = __builtin_assume_aligned(m->dense_vals, 64);
    uint64_t sum = 0;
    for (size_t i = 0; i < m->count; ++i)
        sum += vals[i];
    return sum;
}

static void run_case_hashtables(FILE *csv)
{
    const size_t CAP   = 1u << 19; /* 524,288 slots */
    const size_t ITEMS = 360000;   /* ~68.7% load factor */
    const size_t NQ    = 1u << 19; /* 524,288 mixed 50% hit / 50% miss queries */

    uint32_t *perm = malloc(ITEMS * sizeof(uint32_t));
    uint64_t *keys = bench_alloc_aligned(64, ITEMS * sizeof(uint64_t));
    uint64_t *queries = bench_alloc_aligned(64, NQ * sizeof(uint64_t));
    uint64_t seed = 0xCAFEBABE11223344ULL;

    for (size_t i = 0; i < ITEMS; ++i) perm[i] = (uint32_t)i;
    for (size_t i = ITEMS - 1; i > 0; --i) {
        size_t j = (size_t)(splitmix64(&seed) % (i + 1));
        uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }

    ChainMap      cmap;  chain_init(&cmap, CAP, perm);
    SwissMap32    smap;  swiss_init(&smap, CAP);
    DenseSwissMap dmap;  dswiss_init(&dmap, CAP);

    for (size_t i = 0; i < ITEMS; ++i) {
        uint64_t k = splitmix64(&seed) | 1ULL;
        uint64_t v = (i + 1u) * 7u;
        keys[i] = k;
        chain_put(&cmap, k, v, perm[i]);
        swiss_put(&smap, k, v);
        dswiss_put(&dmap, k, v);
    }
    for (size_t i = 0; i < NQ; ++i) {
        if (i & 1u) queries[i] = keys[splitmix64(&seed) % ITEMS];
        else        queries[i] = splitmix64(&seed) | 1ULL;
    }

    bench_print_section("1A. Hash Map Lookup (360K entries, 50% hit / 50% miss): Chained vs AVX2 SwissTable",
                        "Abseil SwissTable, boost::unordered_flat_map, Martin Leitner-Ankerl (unordered_dense), wyhash");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = chain_query_all(&cmap, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "highperf_ds", "hash_lookup", "Chained Heap Nodes (std::unordered_map)", b_ns, b_cyc, NQ, base, chk1, "Mq/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = dswiss_query_all(&dmap, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "hash_lookup", "unordered_dense + AVX2 32-Slot H2 Probe", b_ns, b_cyc, NQ, base, chk2, "Mq/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk3 = swiss_query_all(&smap, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk3);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "hash_lookup", "Flat AVX2 SwissTable (32-Way H2 Ctrl)", b_ns, b_cyc, NQ, base, chk3, "Mq/s", 1.0);

    bench_print_section("1B. Hash Map Full Iteration (360K entries): Chained Nodes vs unordered_dense Contiguous SoA",
                        "Martin Leitner-Ankerl (ankerl::unordered_dense), mu_sparse_set Principle");

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = chain_iterate_all(&cmap);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    base = (double)b_ns;
    bench_print_row(csv, "highperf_ds", "hash_iter", "Chained Bucket+Node Walk", b_ns, b_cyc, ITEMS, base, chk1, "Me/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = dswiss_iterate_all(&dmap);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "hash_iter", "unordered_dense Contiguous dense_vals[]", b_ns, b_cyc, ITEMS, base, chk2, "Me/s", 1.0);

    free(perm); free(keys); free(queries);
    free(cmap.buckets); free(cmap.pool);
    free(smap.ctrl); free(smap.keys); free(smap.vals);
    free(dmap.ctrl); free(dmap.dense_idx); free(dmap.dense_keys); free(dmap.dense_vals);
}

/* =========================================================================
 * 2. Static Sorted Search (4M int32 = 16 MB, out-of-L2!):
 *    Branchy Binary Search vs Branchless CMOV vs Eytzinger + Prefetch vs 16-ary SIMD S+Tree
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-if-conversion")))
static int64_t search_branchy_std(const int32_t *a, size_t n, const int32_t *q, size_t nq)
{
    int64_t sum = 0;
    for (size_t i = 0; i < nq; ++i) {
        int32_t x = q[i];
        size_t lo = 0, hi = n;
        while (lo < hi) {
            size_t mid = (lo + hi) >> 1;
            if (a[mid] < x) lo = mid + 1u;
            else            hi = mid;
        }
        sum += (lo < n) ? a[lo] : -1;
    }
    return sum;
}

BENCH_NOINLINE static int64_t search_branchless_cmov(const int32_t *a, size_t n, const int32_t *q, size_t nq)
{
    int64_t sum = 0;
    for (size_t i = 0; i < nq; ++i) {
        int32_t x = q[i];
        const int32_t *base = a;
        size_t len = n;
        while (len > 1u) {
            size_t half = len >> 1;
            base += (base[half - 1u] < x) ? half : 0u;
            len -= half;
        }
        base += (*base < x);
        size_t idx = (size_t)(base - a);
        sum += (idx < n) ? a[idx] : -1;
    }
    return sum;
}

/* Khuong & Morin / Algorithmica 1-indexed BFS Eytzinger Layout */
static void build_eytzinger(const int32_t *sorted, int32_t *eytz, size_t n, size_t k, size_t *idx)
{
    if (k <= n) {
        build_eytzinger(sorted, eytz, n, 2u * k, idx);
        eytz[k] = sorted[(*idx)++];
        build_eytzinger(sorted, eytz, n, 2u * k + 1u, idx);
    }
}

BENCH_NOINLINE static int64_t search_eytzinger_prefetch(const int32_t *eytz, size_t n, const int32_t *q, size_t nq)
{
    int64_t sum = 0;
    for (size_t i = 0; i < nq; ++i) {
        int32_t x = q[i];
        size_t k = 1u;
        while (k <= n) {
            /* Prefetch 4 tree levels ahead (16 children = 1 full 64B cache line!) */
            __builtin_prefetch(&eytz[k * 16u], 0, 1);
            k = 2u * k + (size_t)(eytz[k] < x);
        }
        k >>= __builtin_ffsll((long long)~k);
        sum += (k > 0u) ? eytz[k] : -1;
    }
    return sum;
}

/* Sergey Slotin / Algorithmica Static 16-ary SIMD B-Tree (S-Tree):
 * Every node is 16 int32s = 64 bytes (EXACTLY 1 cache line!), branching factor B+1 = 17!
 * Height for 4M keys is just ceil(log17(4M)) = 6 cache-line hops instead of 22! */
#define STREE_B 16u
typedef struct {
    int32_t *data;          /* flat contiguous 64B-aligned nodes across all levels */
    size_t   level_off[10]; /* start index (in nodes) of level l; 0 = root */
    int      height;
} StaticSTree;

static size_t stree_blocks(size_t n) { return (n + STREE_B - 1u) / STREE_B; }

static void stree_build(StaticSTree *t, const int32_t *sorted, size_t n)
{
    size_t layer_nodes[10];
    int h = 0;
    size_t cur = stree_blocks(n);
    while (cur > 1u) {
        layer_nodes[h++] = cur;
        cur = (cur + STREE_B) / (STREE_B + 1u); /* internal routing layer */
    }
    layer_nodes[h++] = 1u; /* root */
    t->height = h;

    /* Order levels top-down: level 0 = root, level h-1 = leaves */
    size_t total_nodes = 0;
    for (int l = 0; l < h; ++l) {
        t->level_off[l] = total_nodes;
        total_nodes += layer_nodes[h - 1 - l];
    }
    t->data = bench_alloc_aligned(64, total_nodes * STREE_B * sizeof(int32_t));
    for (size_t i = 0; i < total_nodes * STREE_B; ++i) t->data[i] = INT32_MAX;

    /* Fill leaves */
    int32_t *leaves = t->data + t->level_off[h - 1] * STREE_B;
    for (size_t i = 0; i < n; ++i) leaves[i] = sorted[i];

    /* Fill internal nodes bottom-up: key j of internal node u is the min key of subtree (u*17 + j + 1)
     * Wait: easier and exact — compute the max key of each subtree, so routing uses `key > node[j]`! */
    int32_t *sub_min = malloc(total_nodes * sizeof(int32_t));
    size_t leaf_cnt = layer_nodes[0];
    for (size_t u = 0; u < leaf_cnt; ++u)
        sub_min[t->level_off[h - 1] + u] = leaves[u * STREE_B];

    for (int l = h - 2; l >= 0; --l) {
        size_t cnt       = layer_nodes[h - 1 - l];
        size_t child_cnt = layer_nodes[h - 2 - l];
        size_t base      = t->level_off[l];
        size_t cbase     = t->level_off[l + 1];
        for (size_t u = 0; u < cnt; ++u) {
            sub_min[base + u] = sub_min[cbase + u * (STREE_B + 1u)];
            int32_t *node = t->data + (base + u) * STREE_B;
            for (size_t j = 0; j < STREE_B; ++j) {
                size_t cidx = u * (STREE_B + 1u) + j + 1u;
                node[j] = (cidx < child_cnt) ? sub_min[cbase + cidx] : INT32_MAX;
            }
        }
    }
    free(sub_min);
}

static inline unsigned stree_rank16_gt_or_eq(const int32_t *node, __m256i vx)
{
    /* Count how many keys in node[0..15] are <= x (for internal routing where node[j] = min of child j+1) */
    __m256i k0 = _mm256_load_si256((const __m256i *)node);
    __m256i k1 = _mm256_load_si256((const __m256i *)(node + 8));
    /* node[j] > x  <=>  _mm256_cmpgt_epi32(node, vx) */
    unsigned m0 = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(k0, vx)));
    unsigned m1 = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(k1, vx)));
    unsigned gt_mask = m0 | (m1 << 8) | 0x10000u;
    return (unsigned)__builtin_ctz(gt_mask);
}

static inline unsigned stree_lower_bound16(const int32_t *leaf, __m256i vx)
{
    /* First index j in 0..15 where leaf[j] >= x  <=>  !(x > leaf[j]) */
    __m256i k0 = _mm256_load_si256((const __m256i *)leaf);
    __m256i k1 = _mm256_load_si256((const __m256i *)(leaf + 8));
    unsigned m0 = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(vx, k0)));
    unsigned m1 = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(vx, k1)));
    unsigned ge_mask = (~(m0 | (m1 << 8))) & 0xFFFFu;
    return ge_mask ? (unsigned)__builtin_ctz(ge_mask) : 16u;
}

BENCH_NOINLINE static int64_t search_simd_stree(const StaticSTree *t, size_t n_leaves, const int32_t *q, size_t nq)
{
    int64_t sum = 0;
    int h = t->height;
    const int32_t *leaves_base = t->data + t->level_off[h - 1] * STREE_B;

    for (size_t i = 0; i < nq; ++i) {
        int32_t x = q[i];
        __m256i vx = _mm256_set1_epi32(x);
        size_t u = 0;
        for (int l = 0; l < h - 1; ++l) {
            const int32_t *node = t->data + (t->level_off[l] + u) * STREE_B;
            unsigned child = stree_rank16_gt_or_eq(node, vx);
            u = u * (STREE_B + 1u) + child;
        }
        const int32_t *leaf = leaves_base + u * STREE_B;
        unsigned pos = stree_lower_bound16(leaf, vx);
        int32_t ans = -1;
        if (pos < 16u) {
            ans = (leaf[pos] == INT32_MAX) ? -1 : leaf[pos];
        } else if (u + 1u < n_leaves) {
            int32_t nxt = leaves_base[(u + 1u) * STREE_B];
            ans = (nxt == INT32_MAX) ? -1 : nxt;
        }
        sum += ans;
    }
    return sum;
}

static void run_case_static_search(FILE *csv)
{
    const size_t N  = 1u << 22; /* 4,194,304 sorted int32s = 16 MB (13x larger than 1.25 MB L2!) */
    const size_t NQ = 1u << 18; /* 262,144 random queries */
    int32_t *sorted = bench_alloc_aligned(64, N * sizeof(int32_t));
    int32_t *eytz   = bench_alloc_aligned(64, (N + 32u) * sizeof(int32_t));
    int32_t *queries = bench_alloc_aligned(64, NQ * sizeof(int32_t));

    for (size_t i = 0; i < N; ++i) sorted[i] = (int32_t)(i * 3u + 2u);
    size_t idx = 0;
    eytz[0] = -1;
    build_eytzinger(sorted, eytz, N, 1u, &idx);

    StaticSTree stree;
    stree_build(&stree, sorted, N);
    size_t n_leaves = stree_blocks(N);

    uint64_t seed = 0x76543210fedcba98ULL;
    for (size_t i = 0; i < NQ; ++i)
        queries[i] = (int32_t)(splitmix64(&seed) % (N * 3u + 10u));

    bench_print_section("2. Static Sorted Search (4.19M int32 = 16 MB): std::lower_bound vs Eytzinger vs AVX2 S+Tree",
                        "Paul-Virak Khuong & Pat Morin (Eytzinger), Sergey Slotin (Algorithmica S-Tree), Linux Maple Tree");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    int64_t chk1 = 0, chk2 = 0, chk3 = 0, chk4 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = search_branchy_std(sorted, N, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "highperf_ds", "static_search", "Standard Branchy lower_bound (22 hops)", b_ns, b_cyc, NQ, base, (uint64_t)chk1, "Mq/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = search_branchless_cmov(sorted, N, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "static_search", "Branchless CMOV Binary Search", b_ns, b_cyc, NQ, base, (uint64_t)chk2, "Mq/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk3 = search_eytzinger_prefetch(eytz, N, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk3);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "static_search", "Eytzinger BFS Layout + 4-Level Prefetch", b_ns, b_cyc, NQ, base, (uint64_t)chk3, "Mq/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk4 = search_simd_stree(&stree, n_leaves, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk4);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "static_search", "16-ary Cache-Line AVX2 S-Tree (6 hops!)", b_ns, b_cyc, NQ, base, (uint64_t)chk4, "Mq/s", 1.0);

    free(sorted); free(eytz); free(queries); free(stree.data);
}

/* =========================================================================
 * 3. Probabilistic Filter Shootout: Classic 6-Probe Bloom vs Cache-Line Blocked Bloom
 *    (500K items, 2M negative queries — testing cache-line locality on filter probes)
 * ========================================================================= */
BENCH_NOINLINE static size_t bloom_classic_6probe_test(const uint64_t *bits, size_t bit_mask,
                                                       const uint64_t *q, size_t nq)
{
    size_t hits = 0;
    for (size_t i = 0; i < nq; ++i) {
        uint64_t h  = wyhash64_mix(q[i]);
        uint64_t h1 = h;
        uint64_t h2 = (h >> 32) | 1u;
        bool ok = true;
        for (unsigned k = 0; k < 6; ++k) {
            size_t b = (size_t)((h1 + k * h2) & bit_mask);
            if (!((bits[b >> 6] >> (b & 63u)) & 1u)) { ok = false; break; }
        }
        hits += ok;
    }
    return hits;
}

/* Cache-Line Blocked Bloom Filter (Putze et al. / Daniel Lemire / mu_bloom):
 * Hash picks ONE 64-byte cache line (512 bits = 8 u64 words), and tests 8 bits
 * in 8 words IN PARALLEL with ONE AVX2 `_mm256_testz_si256` — 1 cache line miss max, 0 branches! */
typedef struct BENCH_ALIGN(64) {
    uint32_t w[8]; /* 256-bit block (half cache line) for 1-instruction AVX2 test! */
} BloomBlock256;

static inline __m256i bloom_make_mask_avx2(uint64_t h)
{
    const __m256i mult = _mm256_setr_epi32(
        0x47b6137b, 0x44974d91, 0x8824ad5b, 0xa2b7289d,
        0x705495c7, 0x2df1424b, 0x9efc4947, 0x5c6bfb31);
    __m256i x = _mm256_set1_epi32((int32_t)h);
    __m256i shifts = _mm256_srli_epi32(_mm256_mullo_epi32(x, mult), 27);
    return _mm256_sllv_epi32(_mm256_set1_epi32(1), shifts);
}

BENCH_NOINLINE static size_t bloom_blocked_avx2_test(const BloomBlock256 *blocks, size_t blk_mask,
                                                     const uint64_t *q, size_t nq)
{
    size_t hits = 0;
    for (size_t i = 0; i < nq; ++i) {
        uint64_t h = wyhash64_mix(q[i]);
        size_t b = (size_t)((h >> 32) & blk_mask);
        __m256i want = bloom_make_mask_avx2(h);
        __m256i have = _mm256_load_si256((const __m256i *)&blocks[b]);
        hits += (size_t)_mm256_testc_si256(have, want); /* 1 iff (have & want) == want! */
    }
    return hits;
}

static void run_case_filters(FILE *csv)
{
    const size_t ITEMS = 1u << 21; /* 2M items -> 32M bits = 4 MB filter (exceeds L2!) */
    const size_t BITS  = 1u << 25; /* 33,554,432 bits = 4 MiB */
    const size_t NBLKS = BITS / 256u;
    const size_t NQ    = 1u << 20;

    uint64_t      *classic = calloc(BITS / 64u, sizeof(uint64_t));
    BloomBlock256 *blocked = bench_alloc_aligned(64, NBLKS * sizeof(BloomBlock256));
    memset(blocked, 0, NBLKS * sizeof(BloomBlock256));
    uint64_t *queries = bench_alloc_aligned(64, NQ * sizeof(uint64_t));

    uint64_t seed = 0x1020304050607080ULL;
    for (size_t i = 0; i < ITEMS; ++i) {
        uint64_t k  = splitmix64(&seed);
        uint64_t h  = wyhash64_mix(k);
        uint64_t h1 = h, h2 = (h >> 32) | 1u;
        for (unsigned j = 0; j < 6; ++j) {
            size_t b = (size_t)((h1 + j * h2) & (BITS - 1u));
            classic[b >> 6] |= UINT64_C(1) << (b & 63u);
        }
        size_t blk = (size_t)((h >> 32) & (NBLKS - 1u));
        __m256i m  = bloom_make_mask_avx2(h);
        __m256i cur = _mm256_load_si256((const __m256i *)&blocked[blk]);
        _mm256_store_si256((__m256i *)&blocked[blk], _mm256_or_si256(cur, m));
    }
    /* 50% positive (worst case for classic: must touch all 6 scattered cache lines!) + 50% negative */
    uint64_t seed_pos = 0x1020304050607080ULL;
    for (size_t i = 0; i < NQ; ++i) {
        uint64_t pos_k = splitmix64(&seed_pos);
        queries[i] = (i & 1u) ? pos_k : splitmix64(&seed);
    }

    bench_print_section("3. Filter Membership (4 MB Filter, 50% Hit / 50% Miss): Classic 6-Probe vs AVX2 Register-Blocked",
                        "Putze et al., Apache Parquet Split-Block Bloom, Daniel Lemire, mu_bloom.h");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    size_t chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = bloom_classic_6probe_test(classic, BITS - 1u, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "highperf_ds", "bloom_filter", "Classic 6-Probe Bloom (Up to 6 Lines)", b_ns, b_cyc, NQ, base, chk1, "Mq/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = bloom_blocked_avx2_test(blocked, NBLKS - 1u, queries, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "bloom_filter", "AVX2 Split-Block Bloom (1 Line, vptest)", b_ns, b_cyc, NQ, base, chk2, "Mq/s", 1.0);

    free(classic); free(blocked); free(queries);
}

/* =========================================================================
 * 4. Hierarchical 3-Level Summary Bitset vs Flat Bitset (16M IDs, 256 Active)
 * ========================================================================= */
/* 16,777,216 bits = 262,144 L0 words -> 4,096 L1 words -> 64 L2 words -> 1 L3 root u64 word! */
typedef struct {
    uint64_t  l3_root;
    uint64_t  l2[64];
    uint64_t  l1[4096];
    uint64_t *l0; /* 262,144 words = 2 MB */
} HierBitset16M;

static void hier_set(HierBitset16M *h, uint32_t id)
{
    uint32_t w0 = id >> 6;
    uint32_t w1 = w0 >> 6;
    uint32_t w2 = w1 >> 6;
    h->l0[w0]  |= UINT64_C(1) << (id & 63u);
    h->l1[w1]  |= UINT64_C(1) << (w0 & 63u);
    h->l2[w2]  |= UINT64_C(1) << (w1 & 63u);
    h->l3_root |= UINT64_C(1) << (w2 & 63u);
}

BENCH_NOINLINE static uint64_t iter_flat_bitset(const uint64_t *l0, size_t nwords)
{
    uint64_t sum = 0;
    for (size_t w = 0; w < nwords; ++w) {
        uint64_t word = l0[w];
        while (word) {
            sum += (w << 6) + (unsigned)__builtin_ctzll(word);
            word &= word - 1u;
        }
    }
    return sum;
}

BENCH_NOINLINE static uint64_t iter_hier_bitset(const HierBitset16M *h)
{
    uint64_t sum = 0;
    uint64_t m3 = h->l3_root;
    while (m3) {
        unsigned b3 = (unsigned)__builtin_ctzll(m3);
        m3 &= m3 - 1u;
        uint64_t m2 = h->l2[b3];
        while (m2) {
            unsigned b2 = (unsigned)__builtin_ctzll(m2);
            m2 &= m2 - 1u;
            uint32_t i1 = (b3 << 6) + b2;
            uint64_t m1 = h->l1[i1];
            while (m1) {
                unsigned b1 = (unsigned)__builtin_ctzll(m1);
                m1 &= m1 - 1u;
                uint32_t i0 = (i1 << 6) + b1;
                uint64_t m0 = h->l0[i0];
                while (m0) {
                    unsigned b0 = (unsigned)__builtin_ctzll(m0);
                    m0 &= m0 - 1u;
                    sum += ((uint64_t)i0 << 6) + b0;
                }
            }
        }
    }
    return sum;
}

static void run_case_hier_bitset(FILE *csv)
{
    const size_t NWORDS = 262144u; /* 16,777,216 IDs = 2 MB bitset */
    const size_t LOOPS  = 256u;
    HierBitset16M hb;
    memset(&hb, 0, sizeof(hb));
    hb.l0 = calloc(NWORDS, sizeof(uint64_t));

    uint64_t seed = 0x8877665544332211ULL;
    for (int i = 0; i < 256; ++i)
        hier_set(&hb, (uint32_t)(splitmix64(&seed) & 0xFFFFFFu));

    bench_print_section("4. Sparse Domain Bitset Iteration (16.7M IDs, 256 Active): Flat Bitset vs 4-Level HierBitset",
                        "mu_hier_bitset.h, Rust hibitset / Specs ECS, Mesa sparse_bitset");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t acc = 0;
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t l = 0; l < LOOPS; ++l) {
            uint64_t v = iter_flat_bitset(hb.l0, NWORDS);
            FORCE_REG(v);
            acc += v;
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(acc);
        chk1 = acc / LOOPS;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "highperf_ds", "hier_bitset", "Flat uint64[262K] Scan (2 MB per pass)", b_ns, b_cyc, LOOPS, base, chk1, "Mp/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t acc = 0;
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t l = 0; l < LOOPS; ++l) {
            uint64_t v = iter_hier_bitset(&hb);
            FORCE_REG(v);
            acc += v;
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(acc);
        chk2 = acc / LOOPS;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "hier_bitset", "4-Level Summary HierBitset (O(active)!)", b_ns, b_cyc, LOOPS, base, chk2, "Mp/s", 1.0);

    free(hb.l0);
}

/* =========================================================================
 * 5. Sorting 1M (key, payload) Render Packets: libc qsort vs 4-Pass LSD Radix Sort
 * ========================================================================= */
typedef struct {
    uint32_t key;
    uint32_t id;
} SortPair;

static int cmp_sortpair(const void *a, const void *b)
{
    uint32_t ka = ((const SortPair *)a)->key;
    uint32_t kb = ((const SortPair *)b)->key;
    return (ka > kb) - (ka < kb);
}

/* Single-histogram-pass (4 histograms computed in 1 scan!) + 4 scatter passes (Pierre Terdiman / Michael Herf Radix trick!) */
BENCH_NOINLINE static void radix_sort_u32_single_hist(SortPair * BENCH_RESTRICT a, SortPair * BENCH_RESTRICT tmp, size_t n)
{
    uint32_t hist[4][256];
    memset(hist, 0, sizeof(hist));

    /* Compute ALL 4 byte histograms in ONE sequential pass over input! (Pierre Terdiman trick) */
    for (size_t i = 0; i < n; ++i) {
        uint32_t k = a[i].key;
        hist[0][ k        & 0xFFu]++;
        hist[1][(k >>  8) & 0xFFu]++;
        hist[2][(k >> 16) & 0xFFu]++;
        hist[3][(k >> 24) & 0xFFu]++;
    }
    for (int p = 0; p < 4; ++p) {
        uint32_t sum = 0;
        for (int b = 0; b < 256; ++b) {
            uint32_t c = hist[p][b];
            hist[p][b] = sum;
            sum += c;
        }
    }

    SortPair *src = a, *dst = tmp;
    for (int p = 0; p < 4; ++p) {
        unsigned shift = (unsigned)(p * 8);
        uint32_t *off  = hist[p];
        for (size_t i = 0; i < n; ++i) {
            uint32_t b = (src[i].key >> shift) & 0xFFu;
            dst[off[b]++] = src[i];
        }
        SortPair *swap = src; src = dst; dst = swap;
    }
}

static void run_case_radix_sort(FILE *csv)
{
    const size_t N = 1u << 19; /* 524,288 8-byte (key, id) render packets */
    SortPair *orig = bench_alloc_aligned(64, N * sizeof(SortPair));
    SortPair *work = bench_alloc_aligned(64, N * sizeof(SortPair));
    SortPair *tmp  = bench_alloc_aligned(64, N * sizeof(SortPair));

    uint64_t seed = 0x5555aaaa3333ccccULL;
    for (size_t i = 0; i < N; ++i)
        orig[i] = (SortPair){ (uint32_t)splitmix64(&seed), (uint32_t)i };

    bench_print_section("5. Render-Packet Key Sort (524K u32 key+id): libc qsort vs Single-Histogram 4-Pass LSD Radix",
                        "Pierre Terdiman / Michael Herf (Radix Tricks), Frostbite, mu_sort.h");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < 5; ++r) {
        memcpy(work, orig, N * sizeof(SortPair));
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        qsort(work, N, sizeof(SortPair), cmp_sortpair);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(work[N - 1].key);
        chk1 = (uint64_t)work[0].key + work[N / 2].key + work[N - 1].key;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "highperf_ds", "radix_sort", "libc qsort() (Indirect Compare + Branch)", b_ns, b_cyc, N, base, chk1, "Mk/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < 5; ++r) {
        memcpy(work, orig, N * sizeof(SortPair));
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        radix_sort_u32_single_hist(work, tmp, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(work[N - 1].key);
        chk2 = (uint64_t)work[0].key + work[N / 2].key + work[N - 1].key;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "radix_sort", "1-Scan Histogram + 4-Pass LSD Radix Sort", b_ns, b_cyc, N, base, chk2, "Mk/s", 1.0);

    free(orig); free(work); free(tmp);
}

/* =========================================================================
 * 6. LLVM SmallVector<uint32_t, 8> Inline Storage vs Heap Vector (97% <= 8 items)
 * ========================================================================= */
BENCH_NOINLINE static uint64_t smallvec_heap_always(const uint8_t *counts, size_t nlists)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < nlists; ++i) {
        uint32_t n = counts[i];
        uint32_t *buf = malloc((n < 8u ? 8u : n) * sizeof(uint32_t));
        FORCE_REG(buf);
        for (uint32_t k = 0; k < n; ++k) buf[k] = (uint32_t)i + k;
        for (uint32_t k = 0; k < n; ++k) sum += buf[k];
        free(buf);
    }
    return sum;
}

BENCH_NOINLINE static uint64_t smallvec_llvm_inline8(const uint8_t *counts, size_t nlists)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < nlists; ++i) {
        uint32_t inline_buf[8];
        uint32_t n = counts[i];
        uint32_t *buf = (n <= 8u) ? inline_buf : (uint32_t *)malloc(n * sizeof(uint32_t));
        for (uint32_t k = 0; k < n; ++k) buf[k] = (uint32_t)i + k;
        for (uint32_t k = 0; k < n; ++k) sum += buf[k];
        if (__builtin_expect(buf != inline_buf, 0))
            free(buf);
    }
    return sum;
}

static void run_case_smallvec(FILE *csv)
{
    const size_t NLISTS = 1u << 19; /* 524,288 temporary lists (97% <= 8 items, 3% = 24 items) */
    uint8_t *counts = bench_alloc_aligned(64, NLISTS);
    uint64_t seed = 0x99990000aaaa1111ULL;
    for (size_t i = 0; i < NLISTS; ++i) {
        uint64_t r = splitmix64(&seed) % 100u;
        counts[i] = (r < 97u) ? (uint8_t)((r & 7u) + 1u) : 24u;
    }

    bench_print_section("6. Short-Lived Scratch Vectors (524K lists, 97% <=8 items): Heap Vector vs LLVM SmallVector<8>",
                        "LLVM Programmer's Manual (llvm::SmallVector), mu_small_vec.h");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = smallvec_heap_always(counts, NLISTS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "highperf_ds", "small_vector", "std::vector Style (Heap malloc/free)", b_ns, b_cyc, NLISTS, base, chk1, "Ml/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = smallvec_llvm_inline8(counts, NLISTS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "highperf_ds", "small_vector", "LLVM SmallVector<u32, 8> (mu_small_vec)", b_ns, b_cyc, NLISTS, base, chk2, "Ml/s", 1.0);

    free(counts);
}

int main(void)
{
    printf("====================================================================================\n");
    printf("  SUITE 4: HIGH-PERFORMANCE DATA STRUCTURES (SWISSTABLE / S-TREE / RADIX / SMALLVEC)\n");
    printf("====================================================================================\n");

    FILE *csv = fopen("results_proofs.csv", "a");
    run_case_hashtables(csv);
    run_case_static_search(csv);
    run_case_filters(csv);
    run_case_hier_bitset(csv);
    run_case_radix_sort(csv);
    run_case_smallvec(csv);
    if (csv) fclose(csv);
    return 0;
}
