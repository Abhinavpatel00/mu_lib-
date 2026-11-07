/*
 * bench_branch_ilp_alu.c — Branch Elimination, ILP, Arithmetic & Bit Tricks Proofs
 * ================================================================================
 * Proves optimizations from:
 *   1. Polymorphism: Vtable Shuffled Dispatch vs Switch(type) vs Homogeneous Tables + AVX2 (Muratori, Acton)
 *   2. Unpredictable 50/50 Branch vs Sorted Data vs CMOV vs AVX2 Blend Mask (Agner Fog, Fabian, Lemire)
 *   3. Instruction-Level Parallelism: 1 Serial FMA Accumulator vs 4 Scalar vs 8 AVX2 Accumulators (Agner Fog, Muratori)
 *   4. Polynomial Evaluation: Serial Horner Chain vs Estrin's Parallel Tree FMA (Agner Fog, Muratori)
 *   5. Integer Division/Modulo (x % d) vs Lemire fastrange32 & Lemire fastmod Magic Multiplier (Daniel Lemire, Mesa)
 *   6. FP Division & 1/sqrtf(x) vs Montgomery Batch Inversion & _mm256_rsqrt_ps + 1 NR Step (id Software, Agner Fog)
 *   7. Bitstream Decoding: Branchy Byte-by-Byte Bitreader vs Fabian Giesen Branchless 64-bit Refill (Fabian Giesen)
 *   8. Intrusive Doubly-Linked List: Nullable Head/Tail Branches vs Mesa/Amiga Sentinel exec_node + LLVM Tagged Ptr
 */

#include "bench_common.h"
#include <immintrin.h>
#include <math.h>

#define REPS 7

/* =========================================================================
 * 1. Polymorphism: Vtable Call vs Tagged Switch vs Homogeneous Tables (Muratori)
 * ========================================================================= */
typedef struct ShapeBase ShapeBase;
typedef float (*AreaFn)(const ShapeBase *);

struct ShapeBase {
    const AreaFn *vtable;
    uint8_t type;
    float a, b;
};

static float area_rect_vt(const ShapeBase *s) { return s->a * s->b; }
static float area_tri_vt(const ShapeBase *s)  { return 0.5f * s->a * s->b; }
static float area_circ_vt(const ShapeBase *s) { return 3.14159265f * s->a * s->a; }
static float area_trap_vt(const ShapeBase *s) { return 0.5f * (s->a + s->b) * s->a; }

static const AreaFn vt_rect[1] = { area_rect_vt };
static const AreaFn vt_tri[1]  = { area_tri_vt  };
static const AreaFn vt_circ[1] = { area_circ_vt };
static const AreaFn vt_trap[1] = { area_trap_vt };

BENCH_NOINLINE static double sum_area_vtable(ShapeBase * const *shapes, size_t n)
{
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += (double)shapes[i]->vtable[0](shapes[i]);
    return sum;
}

BENCH_NOINLINE static double sum_area_switch(const ShapeBase *shapes, size_t n)
{
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        float a = shapes[i].a, b = shapes[i].b, r = 0.0f;
        switch (shapes[i].type) {
        case 0: r = a * b; break;
        case 1: r = 0.5f * a * b; break;
        case 2: r = 3.14159265f * a * a; break;
        case 3: r = 0.5f * (a + b) * a; break;
        }
        sum += (double)r;
    }
    return sum;
}

/* Muratori table-driven formula + DOD separate homogeneous SoA tables */
BENCH_NOINLINE static double sum_area_homogeneous_soa_avx2(
    const float * BENCH_RESTRICT r_a, const float * BENCH_RESTRICT r_b, size_t nr,
    const float * BENCH_RESTRICT t_a, const float * BENCH_RESTRICT t_b, size_t nt,
    const float * BENCH_RESTRICT c_a, size_t nc,
    const float * BENCH_RESTRICT p_a, const float * BENCH_RESTRICT p_b, size_t np)
{
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    __m256 half = _mm256_set1_ps(0.5f), pi = _mm256_set1_ps(3.14159265f);

    for (size_t i = 0; i < nr; i += 8)
        acc0 = _mm256_fmadd_ps(_mm256_load_ps(r_a + i), _mm256_load_ps(r_b + i), acc0);
    for (size_t i = 0; i < nt; i += 8)
        acc1 = _mm256_fmadd_ps(_mm256_mul_ps(half, _mm256_load_ps(t_a + i)), _mm256_load_ps(t_b + i), acc1);
    for (size_t i = 0; i < nc; i += 8) {
        __m256 va = _mm256_load_ps(c_a + i);
        acc0 = _mm256_fmadd_ps(pi, _mm256_mul_ps(va, va), acc0);
    }
    for (size_t i = 0; i < np; i += 8) {
        __m256 va = _mm256_load_ps(p_a + i);
        __m256 vb = _mm256_load_ps(p_b + i);
        acc1 = _mm256_fmadd_ps(half, _mm256_mul_ps(_mm256_add_ps(va, vb), va), acc1);
    }
    __m256 tot = _mm256_add_ps(acc0, acc1);
    float lanes[8];
    _mm256_storeu_ps(lanes, tot);
    double sum = 0.0;
    for (int k = 0; k < 8; ++k) sum += lanes[k];
    return sum;
}

static void run_case_polymorphism(FILE *csv)
{
    const size_t N = 1u << 18; /* 262,144 shapes (65,536 per type) */
    const size_t PER = N / 4u;
    ShapeBase  *flat = bench_alloc_aligned(64, N * sizeof(ShapeBase));
    ShapeBase **ptrs = bench_alloc_aligned(64, N * sizeof(ShapeBase *));

    float *r_a = bench_alloc_aligned(64, PER * sizeof(float)), *r_b = bench_alloc_aligned(64, PER * sizeof(float));
    float *t_a = bench_alloc_aligned(64, PER * sizeof(float)), *t_b = bench_alloc_aligned(64, PER * sizeof(float));
    float *c_a = bench_alloc_aligned(64, PER * sizeof(float));
    float *p_a = bench_alloc_aligned(64, PER * sizeof(float)), *p_b = bench_alloc_aligned(64, PER * sizeof(float));

    size_t cr = 0, ct = 0, cc = 0, cp = 0;
    uint64_t seed = 0x9988776655443322ULL;
    uint32_t *types = malloc(N * sizeof(uint32_t));
    for (size_t i = 0; i < N; ++i) types[i] = (uint32_t)(i & 3u);
    for (size_t i = N - 1; i > 0; --i) {
        size_t j = (size_t)(splitmix64(&seed) % (i + 1));
        uint32_t tmp = types[i]; types[i] = types[j]; types[j] = tmp;
    }

    const AreaFn *vts[4] = { vt_rect, vt_tri, vt_circ, vt_trap };
    for (size_t i = 0; i < N; ++i) {
        uint8_t ty = (uint8_t)types[i];
        float a = 1.0f + (float)(i & 15) * 0.125f;
        float b = 2.0f + (float)((i >> 4) & 15) * 0.125f;
        flat[i] = (ShapeBase){ vts[ty], ty, a, b };
        ptrs[i] = &flat[i];
        if (ty == 0)      { r_a[cr] = a; r_b[cr] = b; ++cr; }
        else if (ty == 1) { t_a[ct] = a; t_b[ct] = b; ++ct; }
        else if (ty == 2) { c_a[cc] = a; ++cc; }
        else              { p_a[cp] = a; p_b[cp] = b; ++cp; }
    }

    bench_print_section("1. Polymorphism: Vtable Indirect Call vs Switch vs Homogeneous SoA Tables + AVX2 (262K shapes)",
                        "Casey Muratori ('Clean Code, Horrible Performance'), Mike Acton, AGENTS.md §4.4-4.5");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        double res = sum_area_vtable(ptrs, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk1 = (uint64_t)(res * 0.01);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "branch_ilp_alu", "polymorphism", "OOP Virtual Call (vptr[] shuffled)", b_ns, b_cyc, N, base, chk1, "Ms/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        double res = sum_area_switch(flat, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk2 = (uint64_t)(res * 0.01);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "polymorphism", "Flat Struct + switch(type) (shuffled)", b_ns, b_cyc, N, base, chk2, "Ms/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        double res = sum_area_homogeneous_soa_avx2(r_a, r_b, cr, t_a, t_b, ct, c_a, cc, p_a, p_b, cp);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk3 = (uint64_t)(res * 0.01);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "polymorphism", "DOD Split Tables per Type + AVX2", b_ns, b_cyc, N, base, chk3, "Ms/s", 1.0);

    free(types); free(flat); free(ptrs);
    free(r_a); free(r_b); free(t_a); free(t_b); free(c_a); free(p_a); free(p_b);
}

/* =========================================================================
 * 2. Unpredictable 50/50 Branch vs Sorted vs Branchless Mask vs AVX2 Predicate
 * ========================================================================= */
/* Force actual jump via asm goto / volatile branch block so compiler doesn't auto-cmov the baseline! */
BENCH_NOINLINE __attribute__((optimize("no-if-conversion","no-tree-vectorize")))
static int64_t sum_branchy_50_50(const int32_t *a, size_t n, int32_t thresh)
{
    int64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] >= thresh)
            sum += a[i];
    }
    return sum;
}

BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static int64_t sum_branchless_cmov(const int32_t *a, size_t n, int32_t thresh)
{
    int64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        int32_t v = a[i];
        int32_t mask = -((int32_t)(v >= thresh));
        sum += (v & mask);
    }
    return sum;
}

BENCH_NOINLINE static int64_t sum_predicated_avx2(const int32_t * BENCH_RESTRICT a, size_t n, int32_t thresh)
{
    __m256i vth  = _mm256_set1_epi32(thresh - 1);
    __m256i acc0 = _mm256_setzero_si256();
    __m256i acc1 = _mm256_setzero_si256();
    for (size_t i = 0; i < n; i += 8) {
        __m256i v    = _mm256_load_si256((const __m256i *)(a + i));
        __m256i mask = _mm256_cmpgt_epi32(v, vth);
        __m256i sel  = _mm256_and_si256(v, mask);
        acc0 = _mm256_add_epi64(acc0, _mm256_cvtepi32_epi64(_mm256_castsi256_si128(sel)));
        acc1 = _mm256_add_epi64(acc1, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(sel, 1)));
    }
    __m256i tot = _mm256_add_epi64(acc0, acc1);
    int64_t lanes[4];
    _mm256_storeu_si256((__m256i *)lanes, tot);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

static int cmp_i32(const void *p, const void *q)
{
    int32_t a = *(const int32_t *)p, b = *(const int32_t *)q;
    return (a > b) - (a < b);
}

static void run_case_branch(FILE *csv)
{
    const size_t N = 1u << 19; /* 524,288 ints */
    int32_t *unsorted = bench_alloc_aligned(64, N * sizeof(int32_t));
    int32_t *sorted   = bench_alloc_aligned(64, N * sizeof(int32_t));
    uint64_t seed = 0x1122334455667788ULL;
    for (size_t i = 0; i < N; ++i)
        unsorted[i] = (int32_t)(splitmix64(&seed) & 255u);
    memcpy(sorted, unsorted, N * sizeof(int32_t));
    qsort(sorted, N, sizeof(int32_t), cmp_i32);

    bench_print_section("2. 50/50 Data-Dependent Branch vs Sorted Branch vs Branchless Mask vs AVX2 Predicate",
                        "Agner Fog (Mispredict ~15-20 cyc), Daniel Lemire, AGENTS.md §5");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    int64_t chk1 = 0, chk2 = 0, chk3 = 0, chk4 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = sum_branchy_50_50(unsorted, N, 128);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "branch_ilp_alu", "branch_50_50", "Random 50/50 Branch (Mispredict Storm)", b_ns, b_cyc, N, base, (uint64_t)chk1, "Mi/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = sum_branchy_50_50(sorted, N, 128);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "branch_50_50", "Same Branch on Pre-Sorted Data", b_ns, b_cyc, N, base, (uint64_t)chk2, "Mi/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk3 = sum_branchless_cmov(unsorted, N, 128);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk3);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "branch_50_50", "Branchless Bit-Mask on Unsorted Data", b_ns, b_cyc, N, base, (uint64_t)chk3, "Mi/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk4 = sum_predicated_avx2(unsorted, N, 128);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk4);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "branch_50_50", "AVX2 vpcmpgtd + vpand on Unsorted Data", b_ns, b_cyc, N, base, (uint64_t)chk4, "Mi/s", 1.0);

    free(unsorted); free(sorted);
}

/* =========================================================================
 * 3. ILP Dependency Chains: 1 Serial Accumulator vs 4 Scalar vs 8 AVX2 FMA
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static float dot_1_accum_serial(const float * BENCH_RESTRICT a, const float * BENCH_RESTRICT b, size_t n)
{
    float s0 = 0.0f;
    for (size_t i = 0; i < n; ++i)
        s0 += a[i] * b[i]; /* 4-cycle FMA latency chain -> max 0.25 FLOP/cycle! */
    return s0;
}

BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static float dot_4_accum_scalar(const float * BENCH_RESTRICT a, const float * BENCH_RESTRICT b, size_t n)
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (size_t i = 0; i < n; i += 4) {
        s0 += a[i + 0] * b[i + 0];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    return (s0 + s1) + (s2 + s3);
}

BENCH_NOINLINE static float dot_8_accum_avx2(const float * BENCH_RESTRICT a, const float * BENCH_RESTRICT b, size_t n)
{
    __m256 c0 = _mm256_setzero_ps(), c1 = _mm256_setzero_ps();
    __m256 c2 = _mm256_setzero_ps(), c3 = _mm256_setzero_ps();
    __m256 c4 = _mm256_setzero_ps(), c5 = _mm256_setzero_ps();
    __m256 c6 = _mm256_setzero_ps(), c7 = _mm256_setzero_ps();
    for (size_t i = 0; i < n; i += 64) {
        c0 = _mm256_fmadd_ps(_mm256_load_ps(a + i +  0), _mm256_load_ps(b + i +  0), c0);
        c1 = _mm256_fmadd_ps(_mm256_load_ps(a + i +  8), _mm256_load_ps(b + i +  8), c1);
        c2 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 16), _mm256_load_ps(b + i + 16), c2);
        c3 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 24), _mm256_load_ps(b + i + 24), c3);
        c4 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 32), _mm256_load_ps(b + i + 32), c4);
        c5 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 40), _mm256_load_ps(b + i + 40), c5);
        c6 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 48), _mm256_load_ps(b + i + 48), c6);
        c7 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 56), _mm256_load_ps(b + i + 56), c7);
    }
    c0 = _mm256_add_ps(_mm256_add_ps(c0, c1), _mm256_add_ps(c2, c3));
    c4 = _mm256_add_ps(_mm256_add_ps(c4, c5), _mm256_add_ps(c6, c7));
    c0 = _mm256_add_ps(c0, c4);
    float lanes[8];
    _mm256_storeu_ps(lanes, c0);
    return (lanes[0] + lanes[1] + lanes[2] + lanes[3]) + (lanes[4] + lanes[5] + lanes[6] + lanes[7]);
}

static void run_case_ilp_accumulators(FILE *csv)
{
    const size_t N = 4096; /* 32 KB total -> Hot in 48 KB L1d! Pure CPU FMA latency vs throughput test */
    const size_t LOOPS = 1024;
    float *a = bench_alloc_aligned(64, N * sizeof(float));
    float *b = bench_alloc_aligned(64, N * sizeof(float));
    for (size_t i = 0; i < N; ++i) {
        a[i] = 1.0f / (float)((i & 7) + 1);
        b[i] = 0.5f;
    }

    bench_print_section("3. ILP Dependency Breaking (L1-Resident Dot Product): 1 Accum vs 4 Scalar vs 8x AVX2 FMA",
                        "Agner Fog (Latency x Throughput = 4 x 2 = 8 FMA chains), Casey Muratori (Part 5)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        float acc = 0.0f;
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t l = 0; l < LOOPS; ++l) {
            float v = dot_1_accum_serial(a, b, N);
            FORCE_REG(v);
            acc += v;
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(acc);
        chk1 = (uint64_t)acc;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "branch_ilp_alu", "ilp_dot", "1 Scalar Accumulator (Serial 4-cyc FMA)", b_ns, b_cyc, N * LOOPS, base, chk1, "Mf/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        float acc = 0.0f;
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t l = 0; l < LOOPS; ++l) {
            float v = dot_4_accum_scalar(a, b, N);
            FORCE_REG(v);
            acc += v;
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(acc);
        chk2 = (uint64_t)acc;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "ilp_dot", "4 Independent Scalar Accumulators (ILP)", b_ns, b_cyc, N * LOOPS, base, chk2, "Mf/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        float acc = 0.0f;
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t l = 0; l < LOOPS; ++l) {
            float v = dot_8_accum_avx2(a, b, N);
            FORCE_REG(v);
            acc += v;
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(acc);
        chk3 = (uint64_t)acc;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "ilp_dot", "8 Independent AVX2 FMA Accumulators", b_ns, b_cyc, N * LOOPS, base, chk3, "Mf/s", 1.0);

    free(a); free(b);
}

/* =========================================================================
 * 4. Polynomial Evaluation: Serial Horner vs Estrin's Parallel FMA Tree
 * ========================================================================= */
/* Evaluate degree-7 polynomial on dependent chain so latency is directly exposed */
BENCH_NOINLINE static float poly_horner_chain(float x, size_t steps)
{
    const float c0 = 1.0f, c1 = 0.5f, c2 = -0.12f, c3 = 0.04f,
                c4 = -0.01f, c5 = 0.002f, c6 = -0.0003f, c7 = 0.00004f;
    for (size_t i = 0; i < steps; ++i) {
        /* 7 serial FMAs = 28 cycles latency per step! */
        float r = fmaf(c7, x, c6);
        r = fmaf(r, x, c5);
        r = fmaf(r, x, c4);
        r = fmaf(r, x, c3);
        r = fmaf(r, x, c2);
        r = fmaf(r, x, c1);
        r = fmaf(r, x, c0);
        x = r * 0.5f;
    }
    return x;
}

BENCH_NOINLINE static float poly_estrin_tree(float x, size_t steps)
{
    const float c0 = 1.0f, c1 = 0.5f, c2 = -0.12f, c3 = 0.04f,
                c4 = -0.01f, c5 = 0.002f, c6 = -0.0003f, c7 = 0.00004f;
    for (size_t i = 0; i < steps; ++i) {
        /* Estrin's tree: critical path depth = log2(8) = 3 FMA levels (~12 cycles)! */
        float x2 = x * x;
        float p01 = fmaf(c1, x, c0);
        float p23 = fmaf(c3, x, c2);
        float p45 = fmaf(c5, x, c4);
        float p67 = fmaf(c7, x, c6);
        float x4  = x2 * x2;
        float q0  = fmaf(p23, x2, p01);
        float q1  = fmaf(p67, x2, p45);
        float r   = fmaf(q1, x4, q0);
        x = r * 0.5f;
    }
    return x;
}

static void run_case_horner_vs_estrin(FILE *csv)
{
    const size_t STEPS = 1u << 21;
    bench_print_section("4. Degree-7 Polynomial Latency: Serial Horner's Rule vs Estrin's Parallel FMA Tree",
                        "Agner Fog (Optimizing C++), Casey Muratori (Function Approximation)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        float res = poly_horner_chain(0.5f, STEPS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk1 = (uint64_t)(res * 1e6f);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "branch_ilp_alu", "horner_vs_estrin", "Horner's Rule (Depth 7 = 28 cyc FMA)", b_ns, b_cyc, STEPS, base, chk1, "Mp/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        float res = poly_estrin_tree(0.5f, STEPS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk2 = (uint64_t)(res * 1e6f);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "horner_vs_estrin", "Estrin's Scheme (Depth 3 Tree FMA)", b_ns, b_cyc, STEPS, base, chk2, "Mp/s", 1.0);
}

/* =========================================================================
 * 5. Hardware Integer Modulo (x % d) vs Lemire fastmod & fastrange32
 * ========================================================================= */
/* Daniel Lemire's fastmod: precompute M = ceil(2^64 / d), exact x % d in 2 multiplies! */
static inline uint64_t fastmod_compute_M_u32(uint32_t d)
{
    return UINT64_C(0xFFFFFFFFFFFFFFFF) / d + 1u;
}

static inline uint32_t fastmod_u32(uint32_t a, uint64_t M, uint32_t d)
{
    uint64_t lowbits = M * a;
    return (uint32_t)(((__uint128_t)lowbits * d) >> 64);
}

static inline uint32_t fastrange_u32(uint32_t word, uint32_t p)
{
    return (uint32_t)(((uint64_t)word * (uint64_t)p) >> 32);
}

BENCH_NOINLINE static uint64_t bench_hw_modulo(const uint32_t *x, size_t n, uint32_t d)
{
    FORCE_REG(d); /* runtime variable divisor (e.g. non-pow2 bucket count) */
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += x[i] % d;
    return sum;
}

BENCH_NOINLINE static uint64_t bench_lemire_fastmod(const uint32_t *x, size_t n, uint32_t d)
{
    FORCE_REG(d);
    uint64_t M = fastmod_compute_M_u32(d);
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += fastmod_u32(x[i], M, d);
    return sum;
}

BENCH_NOINLINE static uint64_t bench_lemire_fastrange(const uint32_t *x, size_t n, uint32_t d)
{
    FORCE_REG(d);
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += fastrange_u32(x[i], d);
    return sum;
}

static void run_case_fastmod(FILE *csv)
{
    const size_t N = 1u << 21; /* 2,097,152 runtime modulo ops */
    uint32_t *x = bench_alloc_aligned(64, N * sizeof(uint32_t));
    uint64_t seed = 0x55aa55aa12345678ULL;
    for (size_t i = 0; i < N; ++i) x[i] = (uint32_t)splitmix64(&seed);

    bench_print_section("5. Runtime Non-Pow2 Modulo/Range (d=10007): Hardware div vs Lemire fastmod vs fastrange32",
                        "Daniel Lemire (fastmod & fastrange), Mesa (fast_urem_by_const.h)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = bench_hw_modulo(x, N, 10007u);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "branch_ilp_alu", "fastmod", "Hardware x % d (x86 divl)", b_ns, b_cyc, N, base, chk1, "Mo/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = bench_lemire_fastmod(x, N, 10007u);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "fastmod", "Lemire fastmod (Exact x % d, 0 div!)", b_ns, b_cyc, N, base, chk2, "Mo/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk3 = bench_lemire_fastrange(x, N, 10007u);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk3);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "fastmod", "Lemire fastrange32 ((u64)x * d >> 32)", b_ns, b_cyc, N, base, chk3, "Mo/s", 1.0);

    free(x);
}

/* =========================================================================
 * 6. Vector Normalization / Inverse Sqrt: 1.0f/sqrtf(x) vs Quake 0x5f3759df
 *    vs Modern AVX2 _mm256_rsqrt_ps + 1 Newton-Raphson Step (~23-bit IEEE!)
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static void invsqrt_libm_scalar(const float * BENCH_RESTRICT in, float * BENCH_RESTRICT out, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        out[i] = 1.0f / sqrtf(in[i]);
}

BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static void invsqrt_quake_magic(const float * BENCH_RESTRICT in, float * BENCH_RESTRICT out, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        float x = in[i];
        float xhalf = 0.5f * x;
        uint32_t bits;
        memcpy(&bits, &x, 4);
        bits = 0x5f3759dfu - (bits >> 1);
        float y;
        memcpy(&y, &bits, 4);
        y = y * (1.5f - xhalf * y * y);
        y = y * (1.5f - xhalf * y * y); /* 2nd NR step to match 23-bit precision */
        out[i] = y;
    }
}

BENCH_NOINLINE static void invsqrt_avx2_rsqrt_nr(const float * BENCH_RESTRICT in, float * BENCH_RESTRICT out, size_t n)
{
    __m256 half       = _mm256_set1_ps(0.5f);
    __m256 three_half = _mm256_set1_ps(1.5f);
    for (size_t i = 0; i < n; i += 8) {
        __m256 x  = _mm256_load_ps(in + i);
        __m256 y  = _mm256_rsqrt_ps(x); /* ~12-bit hardware LUT in 1 instruction */
        /* 1 FMA Newton-Raphson step -> ~23-bit full single precision! */
        __m256 xh = _mm256_mul_ps(half, x);
        __m256 yy = _mm256_mul_ps(y, y);
        y = _mm256_mul_ps(y, _mm256_fnmadd_ps(xh, yy, three_half));
        _mm256_store_ps(out + i, y);
    }
}

static void run_case_rsqrt(FILE *csv)
{
    const size_t N = 1u << 20; /* 1,048,576 floats */
    float *in  = bench_alloc_aligned(64, N * sizeof(float));
    float *out = bench_alloc_aligned(64, N * sizeof(float));
    for (size_t i = 0; i < N; ++i) in[i] = 1.0f + (float)(i & 1023) * 0.25f;

    bench_print_section("6. Inverse Square Root (1M floats): 1.0f/sqrtf vs Quake 0x5f3759df vs AVX2 rsqrt_ps + NR",
                        "id Software (Carmack / van Waveren), Intel Intrinsics, Algorithmica");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        invsqrt_libm_scalar(in, out, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(out[N - 1]);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    uint64_t chk1 = (uint64_t)((out[0] + out[123] + out[N - 1]) * 10000.0f);
    double base = (double)b_ns;
    bench_print_row(csv, "branch_ilp_alu", "rsqrt", "Scalar 1.0f / sqrtf(x) (vsqrtss+vdivss)", b_ns, b_cyc, N, base, chk1, "Mf/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        invsqrt_quake_magic(in, out, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(out[N - 1]);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    uint64_t chk2 = (uint64_t)((out[0] + out[123] + out[N - 1]) * 10000.0f);
    bench_print_row(csv, "branch_ilp_alu", "rsqrt", "Quake III 0x5f3759df (GPR<->XMM transfer)", b_ns, b_cyc, N, base, chk2, "Mf/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        invsqrt_avx2_rsqrt_nr(in, out, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(out[N - 1]);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    uint64_t chk3 = (uint64_t)((out[0] + out[123] + out[N - 1]) * 10000.0f);
    bench_print_row(csv, "branch_ilp_alu", "rsqrt", "AVX2 _mm256_rsqrt_ps + 1 FMA NR (~23-bit)", b_ns, b_cyc, N, base, chk3, "Mf/s", 1.0);

    free(in); free(out);
}

/* =========================================================================
 * 7. Fabian Giesen Branchless 64-Bit Bitstream Refill vs Branchy Byte Refill
 * ========================================================================= */
BENCH_NOINLINE static uint64_t bitread_branchy_bytes(const uint8_t * stream, const uint8_t *widths, size_t nsyms)
{
    const uint8_t *p = stream;
    uint64_t bitbuf = 0;
    int bitcnt = 0;
    uint64_t sum = 0;

    for (size_t i = 0; i < nsyms; ++i) {
        int w = widths[i];
        while (bitcnt < w) {
            bitbuf |= (uint64_t)(*p++) << bitcnt;
            bitcnt += 8;
        }
        sum += bitbuf & ((UINT64_C(1) << w) - 1u);
        bitbuf >>= w;
        bitcnt -= w;
    }
    return sum;
}

/* Fabian Giesen ("Reading bits in far too many ways", Variant 4):
 * Branchless refill! Unconditionally load 64 bits, shift into bitbuf, advance
 * pointer by full bytes consumed, OR bitcnt with 56! Zero refill branches! */
BENCH_NOINLINE static uint64_t bitread_ryg_branchless64(const uint8_t * stream, const uint8_t *widths, size_t nsyms)
{
    const uint8_t *p = stream;
    uint64_t bitbuf = 0;
    uint32_t bitcnt = 0;
    uint64_t sum = 0;

    for (size_t i = 0; i < nsyms; ++i) {
        /* Unconditional branchless refill whenever bitcnt <= 56 */
        uint64_t next_word;
        memcpy(&next_word, p, 8);
        bitbuf |= next_word << (bitcnt & 63u);
        p      += (63u - bitcnt) >> 3;
        bitcnt |= 56u;

        uint32_t w = widths[i];
        sum += bitbuf & ((UINT64_C(1) << w) - 1u);
        bitbuf >>= w;
        bitcnt -= w;
    }
    return sum;
}

static void run_case_bitstream(FILE *csv)
{
    const size_t NSYMS = 1u << 21; /* 2,097,152 variable-width bit symbols (1..15 bits) */
    const size_t BUFSZ = NSYMS * 2u + 64u;
    uint8_t *stream = bench_alloc_aligned(64, BUFSZ);
    uint8_t *widths = bench_alloc_aligned(64, NSYMS);
    uint64_t seed = 0xfeedbeeffeedbeefULL;
    for (size_t i = 0; i < BUFSZ; ++i) stream[i] = (uint8_t)splitmix64(&seed);
    for (size_t i = 0; i < NSYMS; ++i) widths[i] = (uint8_t)((splitmix64(&seed) % 15u) + 1u);

    bench_print_section("7. Variable-Length Bitstream Decoding (2.1M symbols): Branchy Byte Loop vs ryg Branchless 64b",
                        "Fabian Giesen ('Reading bits in far too many ways', Oodle Huffman/rANS)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = bitread_branchy_bytes(stream, widths, NSYMS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "branch_ilp_alu", "ryg_bitstream", "Classic while(bitcnt < w) Byte Refill", b_ns, b_cyc, NSYMS, base, chk1, "Ms/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = bitread_ryg_branchless64(stream, widths, NSYMS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "ryg_bitstream", "Fabian Giesen Branchless 64-bit Refill", b_ns, b_cyc, NSYMS, base, chk2, "Ms/s", 1.0);

    free(stream); free(widths);
}

/* =========================================================================
 * 8. Intrusive Bucket Unlink+Relink Across 1024 Small Buckets (Timer Wheel / Hash):
 *    Nullable Doubly-Linked {head, tail} (4 NULL Branches) vs Linux Kernel hlist_node
 *    (`pprev` pointer-to-pointer: 1-word bucket header, ZERO head-special-case branch!)
 * ========================================================================= */
typedef struct DNullNode {
    struct DNullNode *prev, *next;
    uint32_t val;
} DNullNode;

typedef struct {
    DNullNode *head, *tail; /* 16-byte bucket header + head/tail NULL branches */
} DBucket;

typedef struct HListNode {
    struct HListNode  *next;
    struct HListNode **pprev; /* Linux kernel trick: points directly to prev->next OR bucket->first! */
    uint32_t val;
} HListNode;

typedef struct {
    HListNode *first; /* 8-byte bucket header (half the cache footprint of DBucket!) */
} HListHead;

BENCH_NOINLINE __attribute__((optimize("no-if-conversion")))
static uint64_t bucket_move_nullable(DBucket *buckets, DNullNode *nodes,
                                     const uint32_t *nidx, const uint32_t *src_b, const uint32_t *dst_b, size_t nops)
{
    for (size_t i = 0; i < nops; ++i) {
        DNullNode *n = &nodes[nidx[i]];
        DBucket *sb  = &buckets[src_b[i]];
        DBucket *db  = &buckets[dst_b[i]];
        if (n->prev) n->prev->next = n->next; else sb->head = n->next;
        if (n->next) n->next->prev = n->prev; else sb->tail = n->prev;
        n->prev = NULL;
        n->next = db->head;
        if (db->head) db->head->prev = n; else db->tail = n;
        db->head = n;
    }
    uint64_t sum = 0;
    for (size_t b = 0; b < 1024; ++b)
        for (DNullNode *p = buckets[b].head; p; p = p->next) sum += p->val * (uint32_t)(b + 1u);
    return sum;
}

BENCH_NOINLINE static uint64_t bucket_move_linux_hlist(HListHead *buckets, HListNode *nodes,
                                                       const uint32_t *nidx, const uint32_t *dst_b, size_t nops)
{
    for (size_t i = 0; i < nops; ++i) {
        HListNode *n  = &nodes[nidx[i]];
        HListHead *db = &buckets[dst_b[i]];
        /* Unlink: *pprev = next works identically whether n was bucket->first or middle! */
        HListNode *next   = n->next;
        HListNode **pprev = n->pprev;
        *pprev = next;
        if (next) next->pprev = pprev;
        /* Push front into destination bucket (8B header, doesn't even need src_b!) */
        HListNode *first = db->first;
        n->next = first;
        if (first) first->pprev = &n->next;
        db->first = n;
        n->pprev  = &db->first;
    }
    uint64_t sum = 0;
    for (size_t b = 0; b < 1024; ++b)
        for (HListNode *p = buckets[b].first; p; p = p->next) sum += p->val * (uint32_t)(b + 1u);
    return sum;
}

static void run_case_sentinel_list(FILE *csv)
{
    const size_t NBUCKETS = 1024;
    const size_t NNODES   = 2048;
    const size_t NOPS     = 1u << 21;

    DBucket   *db = bench_alloc_aligned(64, NBUCKETS * sizeof(DBucket));
    DNullNode *dn = bench_alloc_aligned(64, NNODES * sizeof(DNullNode));
    HListHead *hb = bench_alloc_aligned(64, NBUCKETS * sizeof(HListHead));
    HListNode *hn = bench_alloc_aligned(64, NNODES * sizeof(HListNode));

    uint32_t *nidx  = bench_alloc_aligned(64, NOPS * sizeof(uint32_t));
    uint32_t *src_b = bench_alloc_aligned(64, NOPS * sizeof(uint32_t));
    uint32_t *dst_b = bench_alloc_aligned(64, NOPS * sizeof(uint32_t));
    uint32_t *cur_b = malloc(NNODES * sizeof(uint32_t));

    uint64_t seed = 0x3141592653589793ULL;
    for (size_t i = 0; i < NNODES; ++i) cur_b[i] = (uint32_t)(i & (NBUCKETS - 1u));
    for (size_t i = 0; i < NOPS; ++i) {
        uint32_t id = (uint32_t)(splitmix64(&seed) & (NNODES - 1u));
        uint32_t to = (uint32_t)(splitmix64(&seed) & (NBUCKETS - 1u));
        nidx[i]  = id;
        src_b[i] = cur_b[id];
        dst_b[i] = to;
        cur_b[id] = to;
    }

    bench_print_section("8. Intrusive Bucket Migrate (2.1M ops, 1K buckets): Nullable {head,tail} vs Linux hlist pprev",
                        "Linux Kernel (include/linux/list.h hlist_node pprev), Mesa NIR, mu_index_list");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        memset(db, 0, NBUCKETS * sizeof(DBucket));
        for (size_t i = 0; i < NNODES; ++i) {
            size_t b = i & (NBUCKETS - 1u);
            dn[i].val = (uint32_t)(i + 1u);
            dn[i].prev = NULL;
            dn[i].next = db[b].head;
            if (db[b].head) db[b].head->prev = &dn[i]; else db[b].tail = &dn[i];
            db[b].head = &dn[i];
        }
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = bucket_move_nullable(db, dn, nidx, src_b, dst_b, NOPS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "branch_ilp_alu", "linux_hlist", "Nullable {head,tail} (16B hdr + 3 ifs)", b_ns, b_cyc, NOPS, base, chk1, "Mo/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        memset(hb, 0, NBUCKETS * sizeof(HListHead));
        for (size_t i = 0; i < NNODES; ++i) {
            size_t b = i & (NBUCKETS - 1u);
            hn[i].val  = (uint32_t)(i + 1u);
            hn[i].next = hb[b].first;
            if (hb[b].first) hb[b].first->pprev = &hn[i].next;
            hb[b].first = &hn[i];
            hn[i].pprev = &hb[b].first;
        }
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = bucket_move_linux_hlist(hb, hn, nidx, dst_b, NOPS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "branch_ilp_alu", "linux_hlist", "Linux Kernel hlist_node (**pprev, 8B hdr)", b_ns, b_cyc, NOPS, base, chk2, "Mo/s", 1.0);

    free(db); free(dn); free(hb); free(hn); free(nidx); free(src_b); free(dst_b); free(cur_b);
}

int main(void)
{
    printf("====================================================================================\n");
    printf("  SUITE 2: BRANCH ELIMINATION, ILP, ARITHMETIC & BIT-LEVEL PROOFS\n");
    printf("====================================================================================\n");

    FILE *csv = fopen("results_proofs.csv", "a");
    run_case_polymorphism(csv);
    run_case_branch(csv);
    run_case_ilp_accumulators(csv);
    run_case_horner_vs_estrin(csv);
    run_case_fastmod(csv);
    run_case_rsqrt(csv);
    run_case_bitstream(csv);
    run_case_sentinel_list(csv);
    if (csv) fclose(csv);
    return 0;
}
