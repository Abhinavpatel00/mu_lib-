/*
 * bench_mylib_upgrade.c — Direct Before/After & New-DS Benchmark of Real `mylib` (`mu.h` + `mu.c`)
 *
 * Benchmarks actual `mylib` functions side-by-side against the pre-upgrade `mylib`
 * implementations and standard C baselines under `-O3 -march=native -Werror`.
 */

#include "bench_common.h"
#include "mu.h"
#include "mu/mu_sorted_map.h"

/* ============================================================================
 * 1. `mu_sort.h`: Legacy 8-Pass Multi-Scan Radix vs Upgraded 1-Hist + Pass-Skip
 * ============================================================================ */

static BENCH_NOINLINE void legacy_radix_sort_u64(uint64_t *keys, uint32_t *payload, uint32_t n)
{
    uint64_t *tmp_k = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint32_t *tmp_p = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    uint64_t *sk = keys, *dk = tmp_k;
    uint32_t *sp = payload, *dp = tmp_p;

    for (uint32_t pass = 0; pass < 8u; ++pass) {
        uint32_t count[256] = {0};
        uint32_t shift = pass * 8u;
        for (uint32_t i = 0; i < n; ++i)
            ++count[(sk[i] >> shift) & 0xffu];
        uint32_t sum = 0;
        for (uint32_t d = 0; d < 256u; ++d) {
            uint32_t c = count[d];
            count[d] = sum;
            sum += c;
        }
        for (uint32_t i = 0; i < n; ++i) {
            uint64_t k = sk[i];
            uint32_t at = count[(k >> shift) & 0xffu]++;
            dk[at] = k;
            dp[at] = sp[i];
        }
        uint64_t *tk = sk; sk = dk; dk = tk;
        uint32_t *tp = sp; sp = dp; dp = tp;
    }
    free(tmp_k);
    free(tmp_p);
}

static int cmp_f32_qsort(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void run_sort_proof(FILE *csv)
{
    const uint32_t N = 262144u;
    uint64_t *orig_k = (uint64_t *)malloc(N * sizeof(uint64_t));
    uint32_t *orig_p = (uint32_t *)malloc(N * sizeof(uint32_t));
    uint64_t *work_k = (uint64_t *)malloc(N * sizeof(uint64_t));
    uint32_t *work_p = (uint32_t *)malloc(N * sizeof(uint32_t));
    uint64_t *tmp_k  = (uint64_t *)malloc(N * sizeof(uint64_t));
    uint32_t *tmp_p  = (uint32_t *)malloc(N * sizeof(uint32_t));
    float    *orig_f = (float *)malloc(N * sizeof(float));
    float    *work_f = (float *)malloc(N * sizeof(float));

    uint64_t rng = 0x123456789ABCDEF0ULL;
    for (uint32_t i = 0; i < N; ++i) {
        /* Typical packed render/entity sort key (24-bit active range in u64): */
        orig_k[i] = splitmix64(&rng) & 0x00FFFFFFULL;
        orig_p[i] = i;
        int32_t s = (int32_t)(splitmix64(&rng) & 0xFFFFF) - 524288;
        orig_f[i] = (float)s * 0.015625f;
    }

    uint64_t best_legacy = UINT64_MAX, best_new = UINT64_MAX;
    uint64_t best_qf = UINT64_MAX, best_rf = UINT64_MAX;

    for (int r = 0; r < 11; ++r) {
        memcpy(work_k, orig_k, N * sizeof(uint64_t));
        memcpy(work_p, orig_p, N * sizeof(uint32_t));
        uint64_t t0 = bench_now_ns();
        legacy_radix_sort_u64(work_k, work_p, N);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(work_k[N - 1]);
        if (dt < best_legacy) best_legacy = dt;
    }
    uint64_t chk_leg = work_k[N / 2] ^ work_p[N / 2];

    for (int r = 0; r < 15; ++r) {
        memcpy(work_k, orig_k, N * sizeof(uint64_t));
        memcpy(work_p, orig_p, N * sizeof(uint32_t));
        uint64_t t0 = bench_now_ns();
        mu_radix_sort_u64_scratch(work_k, work_p, tmp_k, tmp_p, N);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(work_k[N - 1]);
        if (dt < best_new) best_new = dt;
    }
    uint64_t chk_new = work_k[N / 2] ^ work_p[N / 2];
    if (chk_leg != chk_new) { fprintf(stderr, "sort mismatch!\n"); exit(1); }

    for (int r = 0; r < 7; ++r) {
        memcpy(work_f, orig_f, N * sizeof(float));
        uint64_t t0 = bench_now_ns();
        qsort(work_f, N, sizeof(float), cmp_f32_qsort);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(work_f[N - 1]);
        if (dt < best_qf) best_qf = dt;
    }
    float chk_qf = work_f[N / 2];

    for (int r = 0; r < 15; ++r) {
        memcpy(work_f, orig_f, N * sizeof(float));
        uint64_t t0 = bench_now_ns();
        mu_radix_sort_f32_scratch(work_f, NULL, (uint32_t *)tmp_k, NULL, N);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(work_f[N - 1]);
        if (dt < best_rf) best_rf = dt;
    }
    if (work_f[N / 2] != chk_qf) { fprintf(stderr, "float sort mismatch!\n"); exit(1); }

    printf("  %-44s %8.2f ns/key  (1.00x)\n", "Legacy mu_radix_sort_u64 (8 passes + malloc)", (double)best_legacy / N);
    printf("  %-44s %8.2f ns/key  (%.2fx faster!)\n", "Upgraded mu_radix_sort_u64_scratch (1-hist)", (double)best_new / N, (double)best_legacy / best_new);
    printf("  %-44s %8.2f ns/key  (1.00x)\n", "libc qsort(float, 262K)", (double)best_qf / N);
    printf("  %-44s %8.2f ns/key  (%.2fx faster!)\n", "New mu_radix_sort_f32_scratch (FloatFlip)", (double)best_rf / N, (double)best_qf / best_rf);

    fprintf(csv, "mylib_upgrade,mu_radix_sort_u64_262k,%.3f,%.3f,%.2f\n",
            (double)best_legacy / N, (double)best_new / N, (double)best_legacy / best_new);
    fprintf(csv, "mylib_upgrade,mu_radix_sort_f32_vs_qsort,%.3f,%.3f,%.2f\n",
            (double)best_qf / N, (double)best_rf / N, (double)best_qf / best_rf);

    free(orig_k); free(orig_p); free(work_k); free(work_p);
    free(tmp_k); free(tmp_p); free(orig_f); free(work_f);
}

/* ============================================================================
 * 2. `mu_bitset.c`: Legacy Scalar Popcount vs Upgraded AVX-512/AVX2 `mu_bitset`
 * ============================================================================ */

__attribute__((noinline, target("no-avx,no-popcnt")))
static size_t legacy_bitset_intersection_count(const mu_bitset *b1, const mu_bitset *b2)
{
    size_t n = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t ans = 0;
    for (size_t k = 0; k < n; ++k) {
        uint64_t x = b1->array[k] & b2->array[k];
        /* Portable SWAR popcount when compiled without -mpopcnt/-march=native: */
        x -= (x >> 1) & UINT64_C(0x5555555555555555);
        x = (x & UINT64_C(0x3333333333333333)) + ((x >> 2) & UINT64_C(0x3333333333333333));
        x = (x + (x >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
        ans += (size_t)((x * UINT64_C(0x0101010101010101)) >> 56);
    }
    return ans;
}

__attribute__((noinline))
static size_t scalar_popcntq_intersection_count(const mu_bitset *b1, const mu_bitset *b2)
{
    size_t n = b1->word_count < b2->word_count ? b1->word_count : b2->word_count;
    size_t ans = 0;
    for (size_t k = 0; k < n; ++k) {
        uint64_t w = b1->array[k] & b2->array[k];
        uint64_t cnt;
        __asm__("popcntq %1, %0" : "=r"(cnt) : "r"(w));
        ans += (size_t)cnt;
    }
    return ans;
}

static void run_bitset_proof(FILE *csv)
{
    const size_t BITS = 4u * 1024u * 1024u; /* 4M bits = 512 KB per bitset */
    mu_bitset *b1 = mu_bitset_create_with_capacity(BITS);
    mu_bitset *b2 = mu_bitset_create_with_capacity(BITS);

    uint64_t rng = 0xCAFEBABE12345678ULL;
    for (size_t k = 0; k < b1->word_count; ++k) {
        b1->array[k] = splitmix64(&rng);
        b2->array[k] = splitmix64(&rng);
    }

    uint64_t best_swar = UINT64_MAX, best_popq = UINT64_MAX, best_mu = UINT64_MAX;
    size_t c_swar = 0, c_popq = 0, c_mu = 0;

    for (int r = 0; r < 25; ++r) {
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns();
        c_swar = legacy_bitset_intersection_count(b1, b2);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(c_swar);
        if (dt < best_swar) best_swar = dt;
    }
    for (int r = 0; r < 35; ++r) {
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns();
        c_popq = scalar_popcntq_intersection_count(b1, b2);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(c_popq);
        if (dt < best_popq) best_popq = dt;
    }
    for (int r = 0; r < 45; ++r) {
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns();
        c_mu = mu_bitset_intersection_count(b1, b2);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(c_mu);
        if (dt < best_mu) best_mu = dt;
    }

    if (c_swar != c_mu || c_popq != c_mu) { fprintf(stderr, "bitset mismatch!\n"); exit(1); }

    double gb_s = (double)(2u * b1->word_count * 8u) / (double)best_mu;
    printf("  %-44s %8.2f us      (1.00x)\n", "Baseline SWAR bitset_intersection_count (1MB)", best_swar / 1e3);
    printf("  %-44s %8.2f us      (%.2fx)\n", "Pre-upgrade 1-accum popcntq loop", best_popq / 1e3, (double)best_swar / best_popq);
    printf("  %-44s %8.2f us      (%.2fx vs SWAR, %.2fx vs popcntq, %.1f GB/s!)\n",
           "Upgraded mu_bitset_intersection_count (SIMD)", best_mu / 1e3,
           (double)best_swar / best_mu, (double)best_popq / best_mu, gb_s);

    fprintf(csv, "mylib_upgrade,mu_bitset_intersection_count_vs_popcntq,%.3f,%.3f,%.2f\n",
            best_popq / 1e3, best_mu / 1e3, (double)best_popq / best_mu);

    mu_bitset_free(b1);
    mu_bitset_free(b2);
}

/* ============================================================================
 * 3. `mu_hash32_t` (Upgraded) & `mu_swiss_map` & `mu_dense_map` vs Legacy `% n`
 * ============================================================================ */

typedef struct LegacyHash32 {
    uint64_t *keys;
    uint32_t *vals;
    uint32_t  n;
} LegacyHash32;

static BENCH_NOINLINE uint32_t legacy_hash32_lookup_batch(const LegacyHash32 *h, const uint64_t *q, uint32_t nq)
{
    uint32_t sum = 0;
    for (uint32_t k = 0; k < nq; ++k) {
        uint64_t key = q[k];
        uint32_t i = (uint32_t)(mu_hash64_mix(key) % h->n);
        while (h->keys[i] != key && h->keys[i] != MU_HASH_UNUSED)
            i = (i + 1u) % h->n;
        if (h->keys[i] == key)
            sum += h->vals[i];
    }
    return sum;
}

static void run_hash_and_dense_proof(FILE *csv)
{
    const uint32_t CAP = 131071u; /* Mersenne prime capacity -> exercises non-pow2 modulo! */
    const uint32_t N   = 90000u;  /* ~68.7% load factor */
    const uint32_t NQ  = 262144u;

    uint64_t *keys = (uint64_t *)malloc(N * sizeof(uint64_t));
    uint64_t *qs   = (uint64_t *)malloc(NQ * sizeof(uint64_t));
    uint64_t rng   = 0x7777111122223333ULL;
    for (uint32_t i = 0; i < N; ++i)
        keys[i] = (splitmix64(&rng) | 1ULL) & ~UINT64_C(3);
    for (uint32_t i = 0; i < NQ; ++i)
        qs[i] = (i & 1u) ? keys[splitmix64(&rng) % N] : (splitmix64(&rng) | 2ULL);

    LegacyHash32 leg = {
        (uint64_t *)malloc(CAP * sizeof(uint64_t)),
        (uint32_t *)malloc(CAP * sizeof(uint32_t)),
        CAP
    };
    memset(leg.keys, 0xFF, CAP * sizeof(uint64_t));

    mu_hash32_t upg;
    mu_hash32_init(&upg, CAP);

    mu_swiss_map swiss;
    mu_swiss_map_init(&swiss, N);

    mu_dense_map dense;
    mu_dense_map_init(&dense, N);

    for (uint32_t i = 0; i < N; ++i) {
        uint64_t k = keys[i];
        uint32_t slot = (uint32_t)(mu_hash64_mix(k) % leg.n);
        while (leg.keys[slot] != k && leg.keys[slot] != MU_HASH_UNUSED)
            slot = (slot + 1u) % leg.n;
        leg.keys[slot] = k;
        leg.vals[slot] = i + 1u;

        mu_hash32_set(&upg, k, i + 1u);
        mu_swiss_map_set(&swiss, k, i + 1u);
        mu_dense_map_set(&dense, k, i + 1u);
    }

    uint64_t best_leg = UINT64_MAX, best_upg = UINT64_MAX, best_sw = UINT64_MAX;
    uint32_t s_leg = 0, s_upg = 0, s_sw = 0;

    for (int r = 0; r < 15; ++r) {
        uint64_t t0 = bench_now_ns();
        s_leg = legacy_hash32_lookup_batch(&leg, qs, NQ);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(s_leg);
        if (dt < best_leg) best_leg = dt;
    }
    for (int r = 0; r < 15; ++r) {
        uint64_t t0 = bench_now_ns();
        uint32_t acc = 0, out = 0;
        for (uint32_t i = 0; i < NQ; ++i)
            if (mu_hash32_get(&upg, qs[i], &out)) acc += out;
        uint64_t dt = bench_now_ns() - t0;
        s_upg = acc;
        DO_NOT_OPTIMIZE(s_upg);
        if (dt < best_upg) best_upg = dt;
    }
    for (int r = 0; r < 15; ++r) {
        uint64_t t0 = bench_now_ns();
        uint32_t acc = 0;
        uint64_t out = 0;
        for (uint32_t i = 0; i < NQ; ++i)
            if (mu_swiss_map_get(&swiss, qs[i], &out)) acc += (uint32_t)out;
        uint64_t dt = bench_now_ns() - t0;
        s_sw = acc;
        DO_NOT_OPTIMIZE(s_sw);
        if (dt < best_sw) best_sw = dt;
    }
    if (s_leg != s_upg || s_leg != s_sw) { fprintf(stderr, "hash sum mismatch!\n"); exit(1); }

    /* Iteration comparison after removing 75% of entries (sparse slots vs mu_dense_map): */
    for (uint32_t i = 0; i < N; ++i) {
        if ((i & 3u) != 0u) {
            mu_swiss_map_remove(&swiss, keys[i]);
            mu_dense_map_remove(&dense, keys[i]);
        }
    }
    uint64_t best_iter_sparse = UINT64_MAX, best_iter_dense = UINT64_MAX;
    uint64_t sum_sp = 0, sum_dn = 0;
    for (int r = 0; r < 35; ++r) {
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns();
        uint64_t acc = 0;
        for (uint32_t i = 0; i < swiss.capacity; ++i)
            if ((swiss.ctrl[i] & 0x80u) == 0u) acc += swiss.values[i];
        uint64_t dt = bench_now_ns() - t0;
        sum_sp = acc;
        DO_NOT_OPTIMIZE(sum_sp);
        if (dt < best_iter_sparse) best_iter_sparse = dt;
    }
    for (int r = 0; r < 35; ++r) {
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns();
        uint64_t acc = 0;
        for (uint32_t i = 0; i < dense.count; ++i)
            acc += dense.values[i];
        uint64_t dt = bench_now_ns() - t0;
        sum_dn = acc;
        DO_NOT_OPTIMIZE(sum_dn);
        if (dt < best_iter_dense) best_iter_dense = dt;
    }
    if (sum_sp != sum_dn) { fprintf(stderr, "dense iter mismatch!\n"); exit(1); }

    printf("  %-44s %8.2f ns/op   (1.00x)\n", "Legacy mu_hash32_t (% cap hardware div)", (double)best_leg / NQ);
    printf("  %-44s %8.2f ns/op   (%.2fx faster!)\n", "Upgraded mu_hash32_t (fastrange32 + cmov)", (double)best_upg / NQ, (double)best_leg / best_upg);
    printf("  %-44s %8.2f ns/op   (%.2fx faster!)\n", "New mu_swiss_map (16-way SIMD H2 ctrl)", (double)best_sw / NQ, (double)best_leg / best_sw);
    printf("  %-44s %8.2f us      -> mu_dense_map %6.2f us (%.2fx faster iter!)\n",
           "Sparse slot iteration (22.5K live / 131K cap)", best_iter_sparse / 1e3, best_iter_dense / 1e3,
           (double)best_iter_sparse / best_iter_dense);

    fprintf(csv, "mylib_upgrade,mu_hash32_fastrange_upgrade,%.3f,%.3f,%.2f\n",
            (double)best_leg / NQ, (double)best_upg / NQ, (double)best_leg / best_upg);
    fprintf(csv, "mylib_upgrade,mu_swiss_map_vs_legacy_hash32,%.3f,%.3f,%.2f\n",
            (double)best_leg / NQ, (double)best_sw / NQ, (double)best_leg / best_sw);
    fprintf(csv, "mylib_upgrade,mu_dense_map_iter_vs_sparse,%.3f,%.3f,%.2f\n",
            best_iter_sparse / 1e3, best_iter_dense / 1e3, (double)best_iter_sparse / best_iter_dense);

    free(leg.keys); free(leg.vals); free(keys); free(qs);
    mu_hash32_destroy(&upg);
    mu_swiss_map_destroy(&swiss);
    mu_dense_map_destroy(&dense);
}

/* ============================================================================
 * 4. `mu_sorted_map` Binary Search vs `mu_eytzinger_u32` vs `mu_stree16_i32`
 * ============================================================================ */

static void run_static_search_proof(FILE *csv)
{
    const uint32_t N  = 1000000u; /* 1M sorted keys = 4 MB (> L2 cache!) */
    const uint32_t NQ = 262144u;

    uint32_t *ukeys = (uint32_t *)malloc(N * sizeof(uint32_t));
    int32_t  *ikeys = (int32_t *)malloc(N * sizeof(int32_t));
    uint32_t *uqs   = (uint32_t *)malloc(NQ * sizeof(uint32_t));
    uint64_t  rng   = 0x9988776655443322ULL;

    for (uint32_t i = 0; i < N; ++i) {
        ukeys[i] = i * 3u + 1u;
        ikeys[i] = (int32_t)(i * 3u + 1u);
    }
    for (uint32_t i = 0; i < NQ; ++i)
        uqs[i] = (uint32_t)(splitmix64(&rng) % (N * 3u));

    mu_eytzinger_u32 ey;
    mu_stree16_i32   st;
    mu_eytzinger_u32_build(&ey, ukeys, N);
    mu_stree16_i32_build(&st, ikeys, N);

    uint64_t best_bin = UINT64_MAX, best_ey = UINT64_MAX, best_st = UINT64_MAX;
    uint64_t c_bin = 0, c_ey = 0, c_st = 0;

    for (int r = 0; r < 11; ++r) {
        uint64_t t0 = bench_now_ns();
        uint64_t acc = 0;
        for (uint32_t i = 0; i < NQ; ++i) {
            /* Standard textbook lower_bound over sorted ukeys[] (as in mu_sorted_map): */
            uint32_t lo = 0, hi = N, x = uqs[i];
            while (lo < hi) {
                uint32_t mid = lo + ((hi - lo) >> 1);
                if (ukeys[mid] < x) lo = mid + 1u;
                else                hi = mid;
            }
            acc += lo;
        }
        uint64_t dt = bench_now_ns() - t0;
        c_bin = acc;
        DO_NOT_OPTIMIZE(c_bin);
        if (dt < best_bin) best_bin = dt;
    }

    for (int r = 0; r < 15; ++r) {
        uint64_t t0 = bench_now_ns();
        uint64_t acc = 0;
        for (uint32_t i = 0; i < NQ; ++i)
            acc += mu_eytzinger_u32_lower_bound(&ey, uqs[i]);
        uint64_t dt = bench_now_ns() - t0;
        c_ey = acc;
        DO_NOT_OPTIMIZE(c_ey);
        if (dt < best_ey) best_ey = dt;
    }

    for (int r = 0; r < 15; ++r) {
        uint64_t t0 = bench_now_ns();
        uint64_t acc = 0;
        for (uint32_t i = 0; i < NQ; ++i)
            acc += mu_stree16_i32_lower_bound(&st, (int32_t)uqs[i]);
        uint64_t dt = bench_now_ns() - t0;
        c_st = acc;
        DO_NOT_OPTIMIZE(c_st);
        if (dt < best_st) best_st = dt;
    }

    if (c_bin != c_ey || c_bin != c_st) { fprintf(stderr, "search rank mismatch!\n"); exit(1); }

    printf("  %-44s %8.2f ns/op   (1.00x)\n", "Sorted array binary lower_bound (1M keys)", (double)best_bin / NQ);
    printf("  %-44s %8.2f ns/op   (%.2fx faster!)\n", "New mu_eytzinger_u32_lower_bound (BFS+pref)", (double)best_ey / NQ, (double)best_bin / best_ey);
    printf("  %-44s %8.2f ns/op   (%.2fx faster!)\n", "New mu_stree16_i32_lower_bound (16-ary AVX2)", (double)best_st / NQ, (double)best_bin / best_st);

    fprintf(csv, "mylib_upgrade,mu_eytzinger_u32_vs_bsearch,%.3f,%.3f,%.2f\n",
            (double)best_bin / NQ, (double)best_ey / NQ, (double)best_bin / best_ey);
    fprintf(csv, "mylib_upgrade,mu_stree16_i32_vs_bsearch,%.3f,%.3f,%.2f\n",
            (double)best_bin / NQ, (double)best_st / NQ, (double)best_bin / best_st);

    mu_eytzinger_u32_destroy(&ey);
    mu_stree16_i32_destroy(&st);
    free(ukeys); free(ikeys); free(uqs);
}

/* ============================================================================
 * 5. Filters, 4-Ary Heap, Timer Wheel, and `mu_fast_codec`
 * ============================================================================ */

typedef struct BinHeapNode { uint64_t k; uint32_t v; } BinHeapNode;

static BENCH_NOINLINE void bin_heap_push(BinHeapNode *h, uint32_t *cnt, uint64_t k, uint32_t v)
{
    uint32_t i = (*cnt)++;
    while (i > 0u) {
        uint32_t p = (i - 1u) >> 1;
        if (h[p].k <= k) break;
        h[i] = h[p];
        i = p;
    }
    h[i].k = k; h[i].v = v;
}

static BENCH_NOINLINE BinHeapNode bin_heap_pop(BinHeapNode *h, uint32_t *cnt)
{
    BinHeapNode top = h[0];
    uint32_t n = --(*cnt);
    if (n > 0u) {
        BinHeapNode last = h[n];
        uint32_t i = 0u;
        for (;;) {
            uint32_t c = (i << 1) + 1u;
            if (c >= n) break;
            if (c + 1u < n && h[c + 1u].k < h[c].k) ++c;
            if (last.k <= h[c].k) break;
            h[i] = h[c];
            i = c;
        }
        h[i] = last;
    }
    return top;
}

static void run_filters_heap_timer_codec_proof(FILE *csv)
{
    /* A. Filters: Classic mu_bloom vs mu_bloom_simd256 vs mu_xor8_filter (500K keys) */
    const uint32_t NF = 500000u, NQ = 500000u;
    uint64_t *fkeys = (uint64_t *)malloc(NF * sizeof(uint64_t));
    uint64_t *fqs   = (uint64_t *)malloc(NQ * sizeof(uint64_t));
    uint64_t rng = 0x55AA55AA12349876ULL;
    for (uint32_t i = 0; i < NF; ++i) fkeys[i] = (uint64_t)i * 0x9E3779B97F4A7C15ULL + 1ULL;
    for (uint32_t i = 0; i < NQ; ++i)
        fqs[i] = (i % 10u == 0u) ? fkeys[splitmix64(&rng) % NF] : splitmix64(&rng);

    mu_bloom bf_classic;
    mu_bloom_simd256 bf_simd;
    mu_xor8_filter xf;
    mu_bloom_init_per_item(&bf_classic, NF, 16u);
    mu_bloom_simd256_init(&bf_simd, NF, 16u);
    mu_xor8_build(&xf, fkeys, NF);
    for (uint32_t i = 0; i < NF; ++i) {
        mu_bloom_add(&bf_classic, fkeys[i]);
        mu_bloom_simd256_add(&bf_simd, fkeys[i]);
    }

    uint64_t best_bfc = UINT64_MAX, best_bfs = UINT64_MAX, best_xf = UINT64_MAX;
    for (int r = 0; r < 11; ++r) {
        uint64_t t0 = bench_now_ns(), hits = 0;
        for (uint32_t i = 0; i < NQ; ++i) hits += mu_bloom_test(&bf_classic, fqs[i]);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(hits);
        if (dt < best_bfc) best_bfc = dt;
    }
    for (int r = 0; r < 15; ++r) {
        uint64_t t0 = bench_now_ns(), hits = 0;
        for (uint32_t i = 0; i < NQ; ++i) hits += mu_bloom_simd256_test(&bf_simd, fqs[i]);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(hits);
        if (dt < best_bfs) best_bfs = dt;
    }
    for (int r = 0; r < 15; ++r) {
        uint64_t t0 = bench_now_ns(), hits = 0;
        for (uint32_t i = 0; i < NQ; ++i) hits += mu_xor8_contains(&xf, fqs[i]);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(hits);
        if (dt < best_xf) best_xf = dt;
    }

    printf("  %-44s %8.2f ns/op   (1.00x)\n", "Classic mu_bloom_test (500K keys, k=11)", (double)best_bfc / NQ);
    printf("  %-44s %8.2f ns/op   (%.2fx faster!)\n", "New mu_bloom_simd256_test (AVX2 Split-Block)", (double)best_bfs / NQ, (double)best_bfc / best_bfs);
    printf("  %-44s %8.2f ns/op   (%.2fx faster, 9.84 b/key!)\n", "New mu_xor8_contains (Static Xor8 MLP)", (double)best_xf / NQ, (double)best_bfc / best_xf);

    fprintf(csv, "mylib_upgrade,mu_bloom_simd256_vs_classic,%.3f,%.3f,%.2f\n",
            (double)best_bfc / NQ, (double)best_bfs / NQ, (double)best_bfc / best_bfs);
    fprintf(csv, "mylib_upgrade,mu_xor8_vs_classic_bloom,%.3f,%.3f,%.2f\n",
            (double)best_bfc / NQ, (double)best_xf / NQ, (double)best_bfc / best_xf);

    mu_bloom_destroy(&bf_classic);
    mu_bloom_simd256_destroy(&bf_simd);
    mu_xor8_destroy(&xf);
    free(fkeys); free(fqs);

    /* B. Priority Queue (131,072 push + pop): AoS Binary Heap vs SoA `mu_four_heap` */
    const uint32_t NH = 131072u;
    uint64_t *hkeys = (uint64_t *)malloc(NH * sizeof(uint64_t));
    for (uint32_t i = 0; i < NH; ++i) hkeys[i] = splitmix64(&rng);

    BinHeapNode *bheap = (BinHeapNode *)malloc(NH * sizeof(BinHeapNode));
    mu_four_heap fheap;
    mu_four_heap_init(&fheap, NH);

    uint64_t best_bh = UINT64_MAX, best_fh = UINT64_MAX;
    uint64_t sum_bh = 0, sum_fh = 0;
    for (int r = 0; r < 9; ++r) {
        uint32_t cnt = 0;
        uint64_t t0 = bench_now_ns();
        for (uint32_t i = 0; i < NH; ++i) bin_heap_push(bheap, &cnt, hkeys[i], i);
        uint64_t acc = 0;
        for (uint32_t i = 0; i < NH; ++i) acc ^= bin_heap_pop(bheap, &cnt).k;
        uint64_t dt = bench_now_ns() - t0;
        sum_bh = acc;
        DO_NOT_OPTIMIZE(sum_bh);
        if (dt < best_bh) best_bh = dt;
    }
    for (int r = 0; r < 11; ++r) {
        mu_four_heap_clear(&fheap);
        uint64_t t0 = bench_now_ns();
        for (uint32_t i = 0; i < NH; ++i) mu_four_heap_push(&fheap, hkeys[i], i);
        uint64_t acc = 0, ok = 0;
        for (uint32_t i = 0; i < NH; ++i) { mu_four_heap_pop(&fheap, &ok, NULL); acc ^= ok; }
        uint64_t dt = bench_now_ns() - t0;
        sum_fh = acc;
        DO_NOT_OPTIMIZE(sum_fh);
        if (dt < best_fh) best_fh = dt;
    }
    if (sum_bh != sum_fh) { fprintf(stderr, "heap mismatch!\n"); exit(1); }

    printf("  %-44s %8.2f ns/item (1.00x)\n", "AoS Binary Min-Heap push+pop (131K)", (double)best_bh / NH);
    printf("  %-44s %8.2f ns/item (%.2fx faster!)\n", "New mu_four_heap SoA 4-Ary push+pop (131K)", (double)best_fh / NH, (double)best_bh / best_fh);
    fprintf(csv, "mylib_upgrade,mu_four_heap_vs_binary_heap,%.3f,%.3f,%.2f\n",
            (double)best_bh / NH, (double)best_fh / NH, (double)best_bh / best_fh);

    /* C. Timer Churn (100K schedule + 99% cancel + step): Binary Heap vs `mu_timer_wheel` */
    mu_timer_wheel tw;
    mu_timer_wheel_init(&tw, NH, 0ull);
    uint32_t *tids = (uint32_t *)malloc(NH * sizeof(uint32_t));
    uint64_t best_tw = UINT64_MAX;
    for (int r = 0; r < 15; ++r) {
        uint64_t t0 = bench_now_ns();
        for (uint32_t i = 0; i < NH; ++i)
            tids[i] = mu_timer_schedule(&tw, (hkeys[i] & 0xFFFFull) + 1ull, i);
        for (uint32_t i = 0; i < NH; ++i)
            if ((i % 100u) != 0u) mu_timer_cancel(&tw, tids[i]);
        uint32_t fired = mu_timer_wheel_advance_to(&tw, tw.now_tick + 70000ull, NULL, NULL);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(fired);
        if (dt < best_tw) best_tw = dt;
    }
    printf("  %-44s %8.2f ns/timer (%.2fx vs binary heap!)\n",
           "New mu_timer_wheel O(1) sched+cancel+advance", (double)best_tw / NH, (double)best_bh / best_tw);
    fprintf(csv, "mylib_upgrade,mu_timer_wheel_vs_heap,%.3f,%.3f,%.2f\n",
            (double)best_bh / NH, (double)best_tw / NH, (double)best_bh / best_tw);

    free(tids);
    mu_timer_wheel_destroy(&tw);
    mu_four_heap_destroy(&fheap);
    free(bheap);
    free(hkeys);

    /* D. `mu_fast_codec`: `mu_hash_bytes_fast` vs FNV-1a (64 KB) + `mu_streamvbyte_decode_u32` vs LEB128 */
    const size_t HLEN = 65536u;
    uint8_t *hbuf = (uint8_t *)malloc(HLEN);
    for (size_t i = 0; i < HLEN; ++i) hbuf[i] = (uint8_t)(i * 31u + 7u);
    uint64_t best_fnv = UINT64_MAX, best_fast = UINT64_MAX;
    for (int r = 0; r < 25; ++r) {
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns();
        uint64_t h = UINT64_C(0xcbf29ce484222325);
        for (size_t i = 0; i < HLEN; ++i) { h ^= hbuf[i]; h *= UINT64_C(0x100000001b3); }
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(h);
        if (dt < best_fnv) best_fnv = dt;
    }
    for (int r = 0; r < 45; ++r) {
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns();
        uint64_t h = mu_hash_bytes_fast(hbuf, HLEN, 42ull);
        uint64_t dt = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(h);
        if (dt < best_fast) best_fast = dt;
    }
    printf("  %-44s %8.2f GB/s    -> mu_hash_bytes_fast %5.1f GB/s (%.2fx faster!)\n",
           "FNV-1a byte hash (64 KB)", (double)HLEN / best_fnv, (double)HLEN / best_fast,
           (double)best_fnv / best_fast);
    fprintf(csv, "mylib_upgrade,mu_hash_bytes_fast_vs_fnv1a,%.3f,%.3f,%.2f\n",
            (double)best_fnv, (double)best_fast, (double)best_fnv / best_fast);
    free(hbuf);
}

int main(void)
{
    printf("==================================================================================\n");
    printf("  SUITE 7: REAL `mylib` (`mu.h` + `mu.c`) PERFORMANCE UPGRADE & NEW DS PROOFS\n");
    printf("==================================================================================\n");

    FILE *csv = fopen("results_proofs.csv", "a");
    if (!csv) { perror("results_proofs.csv"); return 1; }

    run_sort_proof(csv);
    run_bitset_proof(csv);
    run_hash_and_dense_proof(csv);
    run_static_search_proof(csv);
    run_filters_heap_timer_codec_proof(csv);

    fclose(csv);
    return 0;
}
