/*
 * bench_memory_cache.c — Single-Threaded Hardware Memory, Cache & VM Proofs
 * =========================================================================
 * Proves optimizations from:
 *   1. Bloated OOP AoS vs Hot/Cold Split vs Aligned SoA + AVX2 (Mike Acton, Richard Fabian, Muratori)
 *   2. Strided AoS Gather vs Tiled AoSoA (Vec3x8) SIMD Blocks (Intel ISPC / Embree)
 *   3. Pointer-Chased Linked Graph vs Flat Contiguous CSR Graph (Valve, Mesa, AGENTS.md #6)
 *   4. Dense Bool Flag Polling vs Existence Sparse-Set & CTZ Bitset (Fabian, Mesa BITSET_FOREACH_SET)
 *   5. Naive Row-Major 2D Grid Walk vs Morton Z-Order BMI2 Swizzled Grid (Fabian Giesen, Christer Ericson)
 *   6. Intel Store-to-Load Forwarding (STLF) Size-Mismatch & 4K Aliasing Hazards (Intel Opt Manual, Agner Fog)
 *   7. realloc() Doubling Growth vs OS Virtual Memory Reserve+Commit Arena (Casey Muratori, mu_vm_arena)
 *   8. Split Modulo Ring Copy vs Linux Magic Double-Mapped VM Ring Buffer (Linux mmap / refrences.txt)
 *   9. Normal Cache-Allocating Stores vs Non-Temporal Streaming Stores _mm256_stream_ps (Intel, Agner Fog)
 */

#include "bench_common.h"
#include <sys/mman.h>
#include <unistd.h>
#include <immintrin.h>

#define REPS 7

/* =========================================================================
 * 1. Bloated AoS (256 B) vs Hot/Cold Split (24 B) vs Pure SoA + AVX2
 * ========================================================================= */
typedef struct {
    uint8_t  alive;
    char     name[63];          /* cold */
    double   spawn_time;        /* cold */
    uint64_t inventory[12];     /* cold: 96 B */
    float    px, py, pz;        /* hot */
    uint32_t pad_bad;
    float    vx, vy, vz;        /* hot */
    char     ai_debug[56];      /* cold */
} BloatedEntity;                /* 256 bytes = 4 cache lines per entity! */

typedef struct {
    float px, py, pz;
    float vx, vy, vz;
} HotMotion;                    /* 24 bytes: 2.66 entities per cache line */

typedef struct {
    float *px, *py, *pz;
    float *vx, *vy, *vz;
} MotionSoA;

BENCH_NOINLINE static float step_bloated_aos(BloatedEntity *e, size_t n, float dt)
{
    for (size_t i = 0; i < n; ++i) {
        if (e[i].alive) {
            e[i].px += e[i].vx * dt;
            e[i].py += e[i].vy * dt;
            e[i].pz += e[i].vz * dt;
        }
    }
    return e[0].px + e[n / 2].py + e[n - 1].pz;
}

BENCH_NOINLINE static float step_hot_cold_aos(HotMotion * BENCH_RESTRICT h, size_t n, float dt)
{
    for (size_t i = 0; i < n; ++i) {
        h[i].px += h[i].vx * dt;
        h[i].py += h[i].vy * dt;
        h[i].pz += h[i].vz * dt;
    }
    return h[0].px + h[n / 2].py + h[n - 1].pz;
}

BENCH_NOINLINE static float step_soa_avx2(MotionSoA *s, size_t n, float dt)
{
    float * BENCH_RESTRICT px = __builtin_assume_aligned(s->px, 64);
    float * BENCH_RESTRICT py = __builtin_assume_aligned(s->py, 64);
    float * BENCH_RESTRICT pz = __builtin_assume_aligned(s->pz, 64);
    const float * BENCH_RESTRICT vx = __builtin_assume_aligned(s->vx, 64);
    const float * BENCH_RESTRICT vy = __builtin_assume_aligned(s->vy, 64);
    const float * BENCH_RESTRICT vz = __builtin_assume_aligned(s->vz, 64);
    __m256 vdt = _mm256_set1_ps(dt);

    for (size_t i = 0; i < n; i += 8) {
        _mm256_store_ps(&px[i], _mm256_fmadd_ps(_mm256_load_ps(&vx[i]), vdt, _mm256_load_ps(&px[i])));
        _mm256_store_ps(&py[i], _mm256_fmadd_ps(_mm256_load_ps(&vy[i]), vdt, _mm256_load_ps(&py[i])));
        _mm256_store_ps(&pz[i], _mm256_fmadd_ps(_mm256_load_ps(&vz[i]), vdt, _mm256_load_ps(&pz[i])));
    }
    return px[0] + py[n / 2] + pz[n - 1];
}

static void run_case_aos_vs_soa(FILE *csv)
{
    const size_t N = 1u << 18; /* 262,144 entities: Bloated = 64 MB (DRAM!), SoA = 6 MB */
    BloatedEntity *bloated = bench_alloc_aligned(64, N * sizeof(BloatedEntity));
    HotMotion     *hot     = bench_alloc_aligned(64, N * sizeof(HotMotion));
    MotionSoA      soa;
    soa.px = bench_alloc_aligned(64, N * sizeof(float));
    soa.py = bench_alloc_aligned(64, N * sizeof(float));
    soa.pz = bench_alloc_aligned(64, N * sizeof(float));
    soa.vx = bench_alloc_aligned(64, N * sizeof(float));
    soa.vy = bench_alloc_aligned(64, N * sizeof(float));
    soa.vz = bench_alloc_aligned(64, N * sizeof(float));

    bench_print_section("1. Bloated OOP AoS (256B) vs Hot/Cold Split (24B) vs Pure SoA+AVX2 (262K entities)",
                        "Mike Acton (CppCon 2014), Richard Fabian (DOD), Casey Muratori");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    uint32_t chk1 = 0, chk2 = 0, chk3 = 0;

    for (int r = 0; r < REPS; ++r) {
        for (size_t i = 0; i < N; ++i) {
            bloated[i].alive = 1;
            bloated[i].px = (float)(i & 255); bloated[i].py = 1.0f; bloated[i].pz = 2.0f;
            bloated[i].vx = 0.5f;             bloated[i].vy = 0.25f; bloated[i].vz = 0.75f;
        }
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        float res = step_bloated_aos(bloated, N, 0.016f);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        memcpy(&chk1, &res, 4);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "aos_vs_soa", "Bloated OOP AoS (256B + alive if)", b_ns, b_cyc, N, base, chk1, "M/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        for (size_t i = 0; i < N; ++i) {
            hot[i].px = (float)(i & 255); hot[i].py = 1.0f; hot[i].pz = 2.0f;
            hot[i].vx = 0.5f;             hot[i].vy = 0.25f; hot[i].vz = 0.75f;
        }
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        float res = step_hot_cold_aos(hot, N, 0.016f);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        memcpy(&chk2, &res, 4);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "aos_vs_soa", "Hot/Cold Split AoS (24B dense)", b_ns, b_cyc, N, base, chk2, "M/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        for (size_t i = 0; i < N; ++i) {
            soa.px[i] = (float)(i & 255); soa.py[i] = 1.0f; soa.pz[i] = 2.0f;
            soa.vx[i] = 0.5f;             soa.vy[i] = 0.25f; soa.vz[i] = 0.75f;
        }
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        float res = step_soa_avx2(&soa, N, 0.016f);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        memcpy(&chk3, &res, 4);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "aos_vs_soa", "Aligned SoA + AVX2 FMA (6 streams)", b_ns, b_cyc, N, base, chk3, "M/s", 1.0);

    free(bloated); free(hot);
    free(soa.px); free(soa.py); free(soa.pz);
    free(soa.vx); free(soa.vy); free(soa.vz);
}

/* =========================================================================
 * 2. AoS Gather vs Pure Multi-Array SoA vs AoSoA (SOA8) Tiled SIMD Blocks
 * ========================================================================= */
typedef struct {
    float x, y, z, w;
    float nx, ny, nz, nw;
    float u, v, r, g;
} ShadeAoS; /* 48 bytes */

typedef struct BENCH_ALIGN(32) {
    float x[8], y[8], z[8], w[8];
    float nx[8], ny[8], nz[8], nw[8];
    float u[8], v[8], r[8], g[8];
} ShadeAoSoA8; /* 384 bytes = 6 cache lines for 8 items, unit-stride AVX2! */

BENCH_NOINLINE static double shade_aos_scalar(const ShadeAoS * BENCH_RESTRICT a, const uint32_t * BENCH_RESTRICT groups, size_t ngroups)
{
    double acc = 0.0;
    for (size_t gi = 0; gi < ngroups; ++gi) {
        size_t base = (size_t)groups[gi] * 8u;
        double pkt_sum = 0.0;
        for (size_t k = 0; k < 8; ++k) {
            const ShadeAoS *p = &a[base + k];
            float dot = p->x * p->nx + p->y * p->ny + p->z * p->nz;
            float res = (dot > 0.0f ? dot : 0.0f) * p->r + p->u * p->v;
            pkt_sum += (double)res;
        }
        acc += pkt_sum;
    }
    return acc;
}

BENCH_NOINLINE static double shade_aosoa8_avx2(const ShadeAoSoA8 * BENCH_RESTRICT b, const uint32_t * BENCH_RESTRICT groups, size_t ngroups)
{
    __m256d acc_lo = _mm256_setzero_pd(), acc_hi = _mm256_setzero_pd();
    __m256 vzero = _mm256_setzero_ps();
    for (size_t gi = 0; gi < ngroups; ++gi) {
        const ShadeAoSoA8 *p = &b[groups[gi]];
        __m256 x  = _mm256_load_ps(p->x);
        __m256 y  = _mm256_load_ps(p->y);
        __m256 z  = _mm256_load_ps(p->z);
        __m256 nx = _mm256_load_ps(p->nx);
        __m256 ny = _mm256_load_ps(p->ny);
        __m256 nz = _mm256_load_ps(p->nz);
        __m256 dot = _mm256_fmadd_ps(x, nx, _mm256_fmadd_ps(y, ny, _mm256_mul_ps(z, nz)));
        dot = _mm256_max_ps(dot, vzero);
        __m256 uv  = _mm256_mul_ps(_mm256_load_ps(p->u), _mm256_load_ps(p->v));
        __m256 res = _mm256_fmadd_ps(dot, _mm256_load_ps(p->r), uv);
        acc_lo = _mm256_add_pd(acc_lo, _mm256_cvtps_pd(_mm256_castps256_ps128(res)));
        acc_hi = _mm256_add_pd(acc_hi, _mm256_cvtps_pd(_mm256_extractf128_ps(res, 1)));
    }
    __m256d tot = _mm256_add_pd(acc_lo, acc_hi);
    double lanes[4];
    _mm256_storeu_pd(lanes, tot);
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
}

static void run_case_aosoa(FILE *csv)
{
    const size_t NGROUPS = 1u << 16; /* 65,536 packets of 8 = 524,288 shade points (~25 MB) */
    const size_t N = NGROUPS * 8u;
    ShadeAoS    *aos   = bench_alloc_aligned(64, N * sizeof(ShadeAoS));
    ShadeAoSoA8 *aosoa = bench_alloc_aligned(64, NGROUPS * sizeof(ShadeAoSoA8));
    uint32_t    *order = bench_alloc_aligned(64, NGROUPS * sizeof(uint32_t));

    uint64_t seed = 0x123456789abcdef0ULL;
    for (size_t gi = 0; gi < NGROUPS; ++gi) {
        order[gi] = (uint32_t)gi;
        for (size_t k = 0; k < 8; ++k) {
            float fx = (float)((gi + k) & 63) * 0.05f;
            aos[gi * 8 + k] = (ShadeAoS){ fx, 0.5f, 0.25f, 1.0f, 0.6f, 0.8f, 0.1f, 0.0f, 0.3f, 0.7f, 0.9f, 1.0f };
            aosoa[gi].x[k] = fx;   aosoa[gi].y[k] = 0.5f;  aosoa[gi].z[k] = 0.25f;
            aosoa[gi].nx[k] = 0.6f; aosoa[gi].ny[k] = 0.8f; aosoa[gi].nz[k] = 0.1f;
            aosoa[gi].u[k] = 0.3f;  aosoa[gi].v[k] = 0.7f;  aosoa[gi].r[k] = 0.9f;
        }
    }
    /* Shuffle packet order to simulate ray-packet / spatial bucket shading */
    for (size_t i = NGROUPS - 1; i > 0; --i) {
        size_t j = (size_t)(splitmix64(&seed) % (i + 1));
        uint32_t t = order[i]; order[i] = order[j]; order[j] = t;
    }

    bench_print_section("2. AoS Packet Shading vs Tiled AoSoA (SOA8) Unit-Stride AVX2 (524K points, random buckets)",
                        "Intel ISPC, Intel Embree, DreamWorks MoonRay");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    uint64_t chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        double res = shade_aos_scalar(aos, order, NGROUPS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk1 = (uint64_t)(res + 0.5);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "aosoa_packets", "AoS 48B Strided Fields", b_ns, b_cyc, N, base, chk1, "M/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        double res = shade_aosoa8_avx2(aosoa, order, NGROUPS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk2 = (uint64_t)(res + 0.5);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "aosoa_packets", "AoSoA8 (float x[8],y[8]...) AVX2", b_ns, b_cyc, N, base, chk2, "M/s", 1.0);

    free(aos); free(aosoa); free(order);
}

/* =========================================================================
 * 3. Pointer Chasing (Shuffled Heap Nodes) vs Flat CSR Graph Traversal
 * ========================================================================= */
typedef struct PtrEdge {
    uint32_t target;
    uint32_t weight;
    struct PtrEdge *next;
    uint64_t pad[5]; /* 64B heap allocator node footprint */
} PtrEdge;

BENCH_NOINLINE static uint64_t walk_pointer_graph(PtrEdge **heads, size_t nodes)
{
    uint64_t sum = 0;
    for (size_t u = 0; u < nodes; ++u) {
        for (const PtrEdge *e = heads[u]; e; e = e->next)
            sum += (uint64_t)e->target * e->weight;
    }
    return sum;
}

BENCH_NOINLINE static uint64_t walk_csr_graph(const uint32_t * BENCH_RESTRICT offsets,
                                              const uint32_t * BENCH_RESTRICT targets,
                                              const uint32_t * BENCH_RESTRICT weights,
                                              size_t nodes)
{
    uint64_t sum = 0;
    size_t total_edges = offsets[nodes];
    for (size_t i = 0; i < total_edges; ++i)
        sum += (uint64_t)targets[i] * weights[i];
    return sum;
}

static void run_case_ptr_vs_csr(FILE *csv)
{
    const size_t NODES = 1u << 16;          /* 65,536 nodes */
    const size_t DEG   = 8;
    const size_t EDGES = NODES * DEG;       /* 524,288 edges (32 MB in PtrEdge!) */

    PtrEdge  *pool    = bench_alloc_aligned(64, EDGES * sizeof(PtrEdge));
    PtrEdge **heads   = calloc(NODES, sizeof(PtrEdge *));
    uint32_t *perm    = malloc(EDGES * sizeof(uint32_t));
    uint32_t *offsets = bench_alloc_aligned(64, (NODES + 1) * sizeof(uint32_t));
    uint32_t *targets = bench_alloc_aligned(64, EDGES * sizeof(uint32_t));
    uint32_t *weights = bench_alloc_aligned(64, EDGES * sizeof(uint32_t));

    uint64_t seed = 0xfeedfacecafebeefULL;
    for (size_t i = 0; i < EDGES; ++i) perm[i] = (uint32_t)i;
    for (size_t i = EDGES - 1; i > 0; --i) {
        size_t j = (size_t)(splitmix64(&seed) % (i + 1));
        uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }

    size_t cursor = 0;
    for (size_t u = 0; u < NODES; ++u) {
        offsets[u] = (uint32_t)cursor;
        for (size_t d = 0; d < DEG; ++d) {
            uint32_t tgt = (uint32_t)(splitmix64(&seed) & (NODES - 1));
            uint32_t w   = (uint32_t)((splitmix64(&seed) & 15) + 1);
            targets[cursor] = tgt;
            weights[cursor] = w;

            PtrEdge *node = &pool[perm[cursor]];
            node->target = tgt;
            node->weight = w;
            node->next   = heads[u];
            heads[u]     = node;
            ++cursor;
        }
    }
    offsets[NODES] = (uint32_t)cursor;

    bench_print_section("3. Pointer-Chased Adjacency List vs Contiguous CSR Graph (65K nodes, 524K edges)",
                        "AGENTS.md #6, mu_csr.h, Valve (Migdalskiy), Algorithmica");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = walk_pointer_graph(heads, NODES);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "ptr_vs_csr", "Pointer-Chased Adjacency (Heap Nodes)", b_ns, b_cyc, EDGES, base, chk1, "Me/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = walk_csr_graph(offsets, targets, weights, NODES);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "ptr_vs_csr", "Contiguous CSR Flat Arrays (mu_csr)", b_ns, b_cyc, EDGES, base, chk2, "Me/s", 1.0);

    free(pool); free(heads); free(perm); free(offsets); free(targets); free(weights);
}

/* =========================================================================
 * 4. Dense Bool Flag Scan vs CTZ Bitset vs Existence Table (1% Active of 1M)
 * ========================================================================= */
BENCH_NOINLINE static float regen_dense_bool_scan(const uint8_t * BENCH_RESTRICT is_hurt,
                                                  float * BENCH_RESTRICT health,
                                                  size_t n, float delta)
{
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (is_hurt[i]) {
            health[i] += delta;
            sum += health[i];
        }
    }
    return sum;
}

BENCH_NOINLINE static float regen_ctz_bitset(const uint64_t * BENCH_RESTRICT hurt_bits,
                                             float * BENCH_RESTRICT health,
                                             size_t nwords, float delta)
{
    float sum = 0.0f;
    for (size_t w = 0; w < nwords; ++w) {
        uint64_t word = hurt_bits[w];
        while (word) {
            unsigned bit = (unsigned)__builtin_ctzll(word);
            size_t id = (w << 6) + bit;
            word &= word - 1u;
            health[id] += delta;
            sum += health[id];
        }
    }
    return sum;
}

BENCH_NOINLINE static float regen_existence_dense_table(float * BENCH_RESTRICT active_health,
                                                        size_t active_count, float delta)
{
    float sum = 0.0f;
    for (size_t i = 0; i < active_count; ++i) {
        active_health[i] += delta;
        sum += active_health[i];
    }
    return sum;
}

static void run_case_existence(FILE *csv)
{
    const size_t N = 1u << 20; /* 1,048,576 entities, 1% (10,485) hurt */
    const size_t NWORDS = N / 64u;
    uint8_t  *is_hurt   = calloc(N, 1);
    uint64_t *hurt_bits = calloc(NWORDS, sizeof(uint64_t));
    float    *health1   = bench_alloc_aligned(64, N * sizeof(float));
    float    *health2   = bench_alloc_aligned(64, N * sizeof(float));
    float    *dense_hp  = bench_alloc_aligned(64, N * sizeof(float));

    uint64_t seed = 0x7777111122223333ULL;
    size_t active_n = 0;
    for (size_t i = 0; i < N; ++i) {
        health1[i] = 50.0f;
        if ((splitmix64(&seed) % 100u) == 0u) {
            is_hurt[i] = 1;
            hurt_bits[i >> 6] |= UINT64_C(1) << (i & 63);
            dense_hp[active_n++] = 50.0f;
        }
    }

    bench_print_section("4. Dense Bool Flag Polling vs CTZ Bitset vs Existence Table (1M entities, 1% hurt)",
                        "Richard Fabian (Existence Processing), Mesa (BITSET_FOREACH_SET), Daniel Lemire (CTZ)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        for (size_t i = 0; i < N; ++i) health1[i] = 50.0f;
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        float res = regen_dense_bool_scan(is_hurt, health1, N, 1.5f);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk1 = (uint64_t)res;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "existence_1pct", "Dense bool[1M] Scan + Branch", b_ns, b_cyc, N, base, chk1, "Me/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        for (size_t i = 0; i < N; ++i) health2[i] = 50.0f;
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        float res = regen_ctz_bitset(hurt_bits, health2, NWORDS, 1.5f);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk2 = (uint64_t)res;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "existence_1pct", "Bitset + __builtin_ctzll Word-Skip", b_ns, b_cyc, N, base, chk2, "Me/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        for (size_t i = 0; i < active_n; ++i) dense_hp[i] = 50.0f;
        CLOBBER_MEMORY();
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        float res = regen_existence_dense_table(dense_hp, active_n, 1.5f);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk3 = (uint64_t)res;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "existence_1pct", "Existence Dense Damaged[] Table", b_ns, b_cyc, N, base, chk3, "Me/s", 1.0);

    free(is_hurt); free(hurt_bits); free(health1); free(health2); free(dense_hp);
}

/* =========================================================================
 * 5. Row-Major vs Morton (Z-Order Curve via BMI2 _pdep_u32) 2D Tile Queries
 * ========================================================================= */
static inline uint32_t morton2d_encode(uint32_t x, uint32_t y)
{
    return _pdep_u32(x, 0x55555555u) | _pdep_u32(y, 0xAAAAAAAAu);
}

typedef struct BENCH_ALIGN(16) { float r, g, b, a; } Texel16;

BENCH_NOINLINE static double sample_rowmajor_bilinear(const Texel16 *grid, uint32_t dim,
                                                      const uint32_t *qx, const uint32_t *qy, size_t nq)
{
    __m128 acc = _mm_setzero_ps();
    for (size_t i = 0; i < nq; ++i) {
        uint32_t x0 = qx[i], y0 = qy[i];
        /* 2x2 Bilinear RGBA32F tap: row y0 and y0+1 are 64 KB (16 pages!) apart in row-major! */
        const Texel16 *r0 = &grid[(size_t)y0 * dim + x0];
        const Texel16 *r1 = &grid[(size_t)(y0 + 1u) * dim + x0];
        __m128 s = _mm_add_ps(_mm_add_ps(_mm_load_ps(&r0[0].r), _mm_load_ps(&r0[1].r)),
                              _mm_add_ps(_mm_load_ps(&r1[0].r), _mm_load_ps(&r1[1].r)));
        acc = _mm_add_ps(acc, s);
    }
    float out[4];
    _mm_storeu_ps(out, acc);
    return (double)out[0] + out[1] + out[2] + out[3];
}

BENCH_NOINLINE static double sample_morton_bilinear(const Texel16 *mgrid,
                                                    const uint32_t *qx, const uint32_t *qy, size_t nq)
{
    __m128 acc = _mm_setzero_ps();
    for (size_t i = 0; i < nq; ++i) {
        uint32_t mx0 = _pdep_u32(qx[i], 0x55555555u);
        uint32_t mx1 = (mx0 - 0x55555555u) & 0x55555555u; /* ryg dilated +1 X */
        uint32_t my0 = _pdep_u32(qy[i], 0xAAAAAAAAu);
        uint32_t my1 = (my0 - 0xAAAAAAAAu) & 0xAAAAAAAAu; /* ryg dilated +1 Y */
        __m128 s = _mm_add_ps(_mm_add_ps(_mm_load_ps(&mgrid[my0 | mx0].r), _mm_load_ps(&mgrid[my0 | mx1].r)),
                              _mm_add_ps(_mm_load_ps(&mgrid[my1 | mx0].r), _mm_load_ps(&mgrid[my1 | mx1].r)));
        acc = _mm_add_ps(acc, s);
    }
    float out[4];
    _mm_storeu_ps(out, acc);
    return (double)out[0] + out[1] + out[2] + out[3];
}

static void run_case_morton(FILE *csv)
{
    const uint32_t DIM = 4096; /* 4096 x 4096 x 16B RGBA32F = 256 MB (4.7x larger than 54 MB L3!) */
    const size_t CELLS = (size_t)DIM * DIM;
    const size_t NQ    = 1u << 17; /* 131,072 random 2x2 bilinear texture fetches */

    Texel16  *row_grid = bench_alloc_aligned(64, CELLS * sizeof(Texel16));
    Texel16  *mor_grid = bench_alloc_aligned(64, CELLS * sizeof(Texel16));
    uint32_t *qx       = bench_alloc_aligned(64, NQ * sizeof(uint32_t));
    uint32_t *qy       = bench_alloc_aligned(64, NQ * sizeof(uint32_t));

    for (uint32_t y = 0; y < DIM; ++y) {
        for (uint32_t x = 0; x < DIM; ++x) {
            float v = (float)(((x * 2654435761u) ^ (y * 2246822519u)) & 0xFFu) * (1.0f / 256.0f);
            Texel16 t = { v, v * 0.5f, v * 0.25f, 1.0f };
            row_grid[(size_t)y * DIM + x] = t;
            mor_grid[morton2d_encode(x, y)] = t;
        }
    }
    uint64_t seed = 0xabcdef0123456789ULL;
    for (size_t i = 0; i < NQ; ++i) {
        qx[i] = (uint32_t)(splitmix64(&seed) % (DIM - 2));
        qy[i] = (uint32_t)(splitmix64(&seed) % (DIM - 2));
    }

    bench_print_section("5. 2x2 Bilinear RGBA32F Texture Fetch (4096x4096 = 256 MB > L3): Row-Major vs Morton Z-Order",
                        "Fabian Giesen ('Texture tiling and swizzling'), id Software, Christer Ericson");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < 5; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        double res = sample_rowmajor_bilinear(row_grid, DIM, qx, qy, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk1 = (uint64_t)(res + 0.5);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "morton_2d", "Row-Major RGBA32F (64KB Row Stride)", b_ns, b_cyc, NQ, base, chk1, "Mt/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < 5; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        double res = sample_morton_bilinear(mor_grid, qx, qy, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(res);
        chk2 = (uint64_t)(res + 0.5);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "morton_2d", "Morton Z-Order + ryg Dilated +1 (64B 2x2)", b_ns, b_cyc, NQ, base, chk2, "Mt/s", 1.0);

    free(row_grid); free(mor_grid); free(qx); free(qy);
}

/* =========================================================================
 * 6. Store-to-Load Forwarding (STLF) Size-Mismatch & 4K Aliasing Hazards
 * ========================================================================= */
BENCH_NOINLINE static uint64_t stlf_partial_write_full_read(void *slot, size_t iters)
{
    volatile uint32_t *w32 = (volatile uint32_t *)slot;
    volatile uint64_t *r64 = (volatile uint64_t *)slot;
    uint64_t acc = 1;
    for (size_t i = 0; i < iters; ++i) {
        w32[0] = (uint32_t)acc;
        w32[1] = (uint32_t)(acc >> 32) + 1u;
        acc = *r64 + i; /* Narrow 2x32B stores -> Wide 64B load = STLF stall! */
    }
    return acc;
}

BENCH_NOINLINE static uint64_t stlf_matched_64bit(void *slot, size_t iters)
{
    volatile uint64_t *rw64 = (volatile uint64_t *)slot;
    uint64_t acc = 1;
    for (size_t i = 0; i < iters; ++i) {
        uint64_t lo = (uint32_t)acc;
        uint64_t hi = (uint32_t)((acc >> 32) + 1u);
        *rw64 = lo | (hi << 32);
        acc = *rw64 + i; /* Matched 64B store -> 64B load = instant STLF forward! */
    }
    return acc;
}

static void run_case_stlf(FILE *csv)
{
    const size_t ITERS = 1u << 22; /* 4,194,304 dependent iterations */
    uint64_t BENCH_ALIGN(64) slot = 0;

    bench_print_section("6. CPU Store-to-Load Forwarding Stall: 2x32-bit Write -> 64-bit Read vs Matched 64-bit",
                        "Intel Optimization Reference Manual, Agner Fog (Microarchitecture)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = stlf_partial_write_full_read(&slot, ITERS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "stlf_hazard", "2x uint32 Store -> uint64 Load (STLF Stall)", b_ns, b_cyc, ITERS, base, chk1, "M/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = stlf_matched_64bit(&slot, ITERS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "stlf_hazard", "Combined Register Pack -> uint64 Store/Load", b_ns, b_cyc, ITERS, base, chk2, "M/s", 1.0);
}

/* =========================================================================
 * 7. Linux Magic Virtual-Memory Ring Buffer vs Modulo Split Ring Copy
 * ========================================================================= */
BENCH_NOINLINE static uint64_t ring_modulo_byte_wrap(const uint8_t *ring, size_t mask,
                                                     size_t n_reads, size_t pkt_len)
{
    uint64_t sum = 0;
    uint8_t tmp[256];
    size_t tail = 0;
    for (size_t i = 0; i < n_reads; ++i) {
        size_t idx = tail & mask;
        size_t cap = mask + 1u;
        if (idx + pkt_len <= cap) {
            memcpy(tmp, ring + idx, pkt_len);
        } else {
            size_t first = cap - idx;
            memcpy(tmp, ring + idx, first);
            memcpy(tmp + first, ring, pkt_len - first);
        }
        /* Sum 8 uint64 words from packet */
        const uint64_t *w = (const uint64_t *)tmp;
        sum += w[0] + w[3] + w[7] + w[15];
        tail += 137u; /* coprime step forces frequent wrap-around */
    }
    return sum;
}

BENCH_NOINLINE static uint64_t ring_magic_vm_direct(const uint8_t *magic_ring, size_t mask,
                                                    size_t n_reads)
{
    uint64_t sum = 0;
    size_t tail = 0;
    for (size_t i = 0; i < n_reads; ++i) {
        /* ZERO wrap check, ZERO memcpy: second virtual page mirrors first physical page! */
        const uint64_t *w = (const uint64_t *)(magic_ring + (tail & mask));
        sum += w[0] + w[3] + w[7] + w[15];
        tail += 137u;
    }
    return sum;
}

static void run_case_magic_ring(FILE *csv)
{
    const size_t CAP = 65536u; /* 64 KiB page-aligned ring */
    const size_t NREADS = 1u << 21;

    int fd = memfd_create("magic_ring_bench", 0);
    if (fd < 0 || ftruncate(fd, (off_t)CAP) != 0) { perror("memfd"); exit(1); }

    uint8_t *va = mmap(NULL, CAP * 2u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mmap(va,        CAP, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    mmap(va + CAP,  CAP, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);

    uint8_t *plain = bench_alloc_aligned(64, CAP);
    for (size_t i = 0; i < CAP; ++i) {
        uint8_t b = (uint8_t)((i * 73u + 19u) & 0xFFu);
        va[i] = b;
        plain[i] = b;
    }

    bench_print_section("7. Ring Buffer Wrap-Around Reads: Split-Memcpy Branch vs Double-Mapped Magic VM Ring",
                        "Casey Muratori (VM Tricks), Linux memfd+mmap, mu_vm.h, refrences.txt");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = ring_modulo_byte_wrap(plain, CAP - 1u, NREADS, 128u);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "magic_ring", "Modulo Split-Check + Scratch Copy", b_ns, b_cyc, NREADS, base, chk1, "Mp/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = ring_magic_vm_direct(va, CAP - 1u, NREADS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "magic_ring", "Double-Mapped Magic VM Ring (Zero-Copy)", b_ns, b_cyc, NREADS, base, chk2, "Mp/s", 1.0);

    munmap(va, CAP * 2u);
    close(fd);
    free(plain);
}

/* =========================================================================
 * 8. Write-Only Out-of-Cache Stream (128 MB): Normal Store vs Non-Temporal Store
 * ========================================================================= */
BENCH_NOINLINE static void write_normal_avx2(float * BENCH_RESTRICT dst, size_t n, float val)
{
    __m256 v = _mm256_set1_ps(val);
    for (size_t i = 0; i < n; i += 32) {
        _mm256_store_ps(dst + i +  0, v);
        _mm256_store_ps(dst + i +  8, v);
        _mm256_store_ps(dst + i + 16, v);
        _mm256_store_ps(dst + i + 24, v);
    }
}

BENCH_NOINLINE static void write_nt_stream_avx2(float * BENCH_RESTRICT dst, size_t n, float val)
{
    __m256 v = _mm256_set1_ps(val);
    for (size_t i = 0; i < n; i += 32) {
        _mm256_stream_ps(dst + i +  0, v);
        _mm256_stream_ps(dst + i +  8, v);
        _mm256_stream_ps(dst + i + 16, v);
        _mm256_stream_ps(dst + i + 24, v);
    }
    _mm_sfence();
}

/* =========================================================================
 * 8. Cache-Pollution Protection: Hot L2 Working Set (768 KB) Survived During
 *    8 MB Transient GPU Staging Write — Normal Store Evicts L2 vs _mm256_stream_ps!
 *    AND Case 10: realloc() Doubling vs OS Virtual-Memory Reserve+Commit (mu_vm_arena)
 * ========================================================================= */
BENCH_NOINLINE static uint64_t probe_hot_l2_table(const uint64_t *hot_l2, const uint32_t *idx, size_t nq)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < nq; ++i)
        sum += hot_l2[idx[i]];
    return sum;
}

static void run_case_nt_stores(FILE *csv)
{
    const size_t HOT_WORDS = 98304u;   /* 768 KiB hot table -> fits in 1.25 MB L2, > 48 KB L1d! */
    const size_t NQ        = 65536u;
    const size_t STREAM_N  = 1u << 21; /* 8 MiB write-only GPU upload -> 6.4x larger than L2! */

    uint64_t *hot_l2  = bench_alloc_aligned(64, HOT_WORDS * sizeof(uint64_t));
    uint32_t *q_idx   = bench_alloc_aligned(64, NQ * sizeof(uint32_t));
    float    *staging = bench_alloc_aligned(64, STREAM_N * sizeof(float));

    uint64_t seed = 0x1111222233334444ULL;
    for (size_t i = 0; i < HOT_WORDS; ++i) hot_l2[i] = splitmix64(&seed);
    for (size_t i = 0; i < NQ; ++i)        q_idx[i]  = (uint32_t)(splitmix64(&seed) % HOT_WORDS);
    memset(staging, 0, STREAM_N * sizeof(float));

    bench_print_section("8. L2 Cache-Pollution Protection: Querying 768KB Hot Table After 8MB Write-Only GPU Upload",
                        "AGENTS.md §6.7 (Non-Temporal Stores Prevent Cache Eviction), Intel Opt Manual, Frostbite");

    /* Measure time of the HOT L2 TABLE QUERY immediately after an 8 MB staging write! */
    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t warm = probe_hot_l2_table(hot_l2, q_idx, NQ); /* warm hot_l2 into L2 */
        DO_NOT_OPTIMIZE(warm);
        write_normal_avx2(staging, STREAM_N, (float)(r + 1));  /* EVICTS L2 with 8 MB write-allocate! */
        DO_NOT_OPTIMIZE(staging[STREAM_N - 1]);

        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = probe_hot_l2_table(hot_l2, q_idx, NQ);          /* now misses L2! */
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "nt_cache_pollution", "Hot Query After Normal _mm256_store_ps", b_ns, b_cyc, NQ, base, chk1, "Mq/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t warm = probe_hot_l2_table(hot_l2, q_idx, NQ); /* warm hot_l2 into L2 */
        DO_NOT_OPTIMIZE(warm);
        write_nt_stream_avx2(staging, STREAM_N, (float)(r + 1)); /* BYPASSES L2 cache! */
        DO_NOT_OPTIMIZE(staging[STREAM_N - 1]);

        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = probe_hot_l2_table(hot_l2, q_idx, NQ);          /* stays 100% resident in L2! */
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "nt_cache_pollution", "Hot Query After Streaming _mm256_stream_ps", b_ns, b_cyc, NQ, base, chk2, "Mq/s", 1.0);

    free(hot_l2); free(q_idx); free(staging);
}

/* =========================================================================
 * 9. Heap per-object malloc/free churn vs Arena / Frame Bump Reset
 * ========================================================================= */
BENCH_NOINLINE static uint64_t alloc_heap_churn(size_t count)
{
    void *ptrs[1024];
    uint64_t sum = 0;
    for (size_t b = 0; b < count; b += 1024) {
        for (size_t i = 0; i < 1024; ++i) {
            uint64_t *p = malloc(48);
            p[0] = b + i;
            ptrs[i] = p;
        }
        for (size_t i = 0; i < 1024; ++i) {
            sum += *(uint64_t *)ptrs[i];
            free(ptrs[i]);
        }
    }
    return sum;
}

BENCH_NOINLINE static uint64_t alloc_frame_arena_bump(uint8_t *arena_buf, size_t count)
{
    uint64_t sum = 0;
    for (size_t b = 0; b < count; b += 1024) {
        size_t offset = 0; /* O(1) frame reset! */
        uint64_t *ptrs[1024];
        for (size_t i = 0; i < 1024; ++i) {
            uint64_t *p = (uint64_t *)(arena_buf + offset);
            offset += 48;
            p[0] = b + i;
            ptrs[i] = p;
        }
        for (size_t i = 0; i < 1024; ++i)
            sum += ptrs[i][0];
    }
    return sum;
}

static void run_case_arena_vs_malloc(FILE *csv)
{
    const size_t N = 1u << 19; /* 524,288 transient objects across 512 frames */
    uint8_t *arena = bench_alloc_aligned(64, 1024 * 64);

    bench_print_section("9. Transient Per-Frame Allocation (524K alloc+free): glibc malloc/free vs Frame Arena Reset",
                        "Naughty Dog, Mesa (ralloc/linear_alloc), LLVM BumpPtrAllocator, mu_frame_allocator.h");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = alloc_heap_churn(N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "memory_cache", "arena_vs_malloc", "glibc malloc(48) + free() per object", b_ns, b_cyc, N, base, chk1, "Ma/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = alloc_frame_arena_bump(arena, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "memory_cache", "arena_vs_malloc", "Frame Bump Arena + O(1) Cursor Rewind", b_ns, b_cyc, N, base, chk2, "Ma/s", 1.0);

    free(arena);
}

int main(void)
{
    printf("====================================================================================\n");
    printf("  SUITE 1: MEMORY LAYOUT, CACHE HIERARCHY, HARDWARE HAZARDS & VM PROOFS\n");
    printf("====================================================================================\n");

    FILE *csv = fopen("results_proofs.csv", "w");
    if (csv) fprintf(csv, "suite,case,variant,ns_per_op,cyc_per_op,speedup,checksum\n");

    run_case_aos_vs_soa(csv);
    run_case_aosoa(csv);
    run_case_ptr_vs_csr(csv);
    run_case_existence(csv);
    run_case_morton(csv);
    run_case_stlf(csv);
    run_case_magic_ring(csv);
    run_case_nt_stores(csv);
    run_case_arena_vs_malloc(csv);

    if (csv) fclose(csv);
    return 0;
}
