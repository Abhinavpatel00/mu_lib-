#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
/*
 * bench_math_vs_cglm.c — mu_math vs cglm, per call and per batch.
 *
 * METHOD (AGENTS.md §2.6, Seven-Field Evidence Rule)
 * --------------------------------------------------
 * The seven fields are printed by the program itself at startup, because a
 * number copied out of a run on someone else's box is not evidence:
 *   1. CPU model + microarchitecture   (from /proc/cpuinfo, decoded by hand)
 *   2. core count and affinity         (sched_setaffinity to one core)
 *   3. measured core frequency         (1-cycle dependent add chain, timed with
 *                                       CLOCK_MONOTONIC_RAW; NOT rdtsc, which
 *                                       counts reference time under DVFS)
 *   4. compiler and flags              (baked in by the Makefile)
 *   5. workload and working set        (printed per case: bytes touched vs
 *                                       L1d/L2/L3, non-zero initialised)
 *   6. baseline                        (cglm at the same -O3 -march=native;
 *                                       cglm's own SIMD paths are enabled by
 *                                       -march, so this is cglm at its best)
 *   7. method                          (paired round-robin, REPS >= 21,
 *                                       min for the latency floor, median for
 *                                       throughput, CV reported, CLOCK_
 *                                       MONOTONIC_RAW, OPAQUE + SINK barriers)
 *
 * WHY THE PAIRING MATTERS HERE: this is a 2-vCPU KVM guest on a shared host.
 * Absolute numbers drift by >20% between runs. Paired interleaving plus min
 * over 21 reps makes the RATIO stable to a few percent; the ratio is the only
 * thing this file claims.
 *
 * HONESTY NOTES ON THE CASES
 * --------------------------
 *  - The "per call" cases are throughput over an L1/L2-resident array, not
 *    latency. Latency of one multiply is a scheduling artefact here.
 *  - Case "soa scalar loop" is the control: SAME layout as the mu kernel, plain
 *    C. It exists so that the batch speedups can be attributed (layout vs
 *    vectorization) instead of asserted.
 *  - cglm's glm_inv_tr is a bare transpose: correct for rigid transforms only.
 *    The affine-inverse case therefore uses rigid matrices for the head-to-head,
 *    and a separate row reports the scaled case where glm_inv_tr is wrong and
 *    mu_inverse_affine is not (verified in test_math.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sched.h>
#include <time.h>
#include <stdint.h>

#include "mu_macros.h"
#include "mu_math.h"
#include "cglm/cglm.h"

#define REPS     21
#define WARMUP   3

/* A volatile global store is a real store: the compiler cannot delete the work
 * that produced the value, and unlike an asm barrier it needs no constraints
 * that differ per type. */
static volatile double g_sink;
#define OPAQUE(p)  do { g_sink = (double)(p); } while (0)
#define SINK(v)    do { g_sink = (double)(v); } while (0)

static double g_ghz = 1.0;
static FILE  *g_csv = NULL;

/* -------------------------------------------------------------------------- */
/* environment                                                                */
/* -------------------------------------------------------------------------- */

static double now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* Effective core clock: a chain of dependent adds is exactly 1 cycle per link
 * on every x86 core since Nehalem, so timing 100M links measures GHz directly
 * and honestly under turbo/DVFS. */
static double measure_ghz(void)
{
    uint64_t x;
    double t0, t1;
    long i;

    x  = 1;
    t0 = now_ns();
    /* The add is in an asm block on purpose: `for (i=0;i<n;++i) x+=1;` is a
     * closed form at -O3 and gets deleted, which is how you end up reporting
     * 549450 GHz (this measurement said exactly that before the fix). */
    for (i = 0; i < 100000000L; ++i)
        __asm__ __volatile__("addq $1, %0" : "+r"(x));
    t1 = now_ns();
    g_sink = (double)x;
    return 100000000.0 / (t1 - t0);
}

static void pin_to_core(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        perror("sched_setaffinity");
}

static void print_environment(void)
{
    FILE *f;
    char  line[512];
    int   cores;

    printf("===============================================================\n");
    printf(" mu_math vs cglm — seven-field record\n");
    printf("===============================================================\n");

    f = fopen("/proc/cpuinfo", "r");
    if (f)
    {
        while (fgets(line, sizeof line, f))
        {
            if (strncmp(line, "model name", 10) == 0)
                printf("  cpu        : %s", strchr(line, ':') + 2);
            else if (strncmp(line, "cpu MHz", 7) == 0)
                printf("  cpu MHz    : %s", strchr(line, ':') + 2);
        }
        fclose(f);
    }

    cores = 1;
    {
        cpu_set_t set;

        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof set, &set) == 0) cores = CPU_COUNT(&set);
    }
    printf("  affinity   : 1 core pinned (set of %d available)\n", cores);
    printf("  clock      : %.2f GHz measured (100M dependent adds / wall time)\n", g_ghz);
    printf("  isa        : avx512f=%d avx2=%d fma=%d\n",
#if defined(MU_MATH_AVX512)
           1,
#else
           0,
#endif
#if defined(MU_MATH_AVX2)
           1,
#else
           0,
#endif
#if defined(__FMA__)
           1
#else
           0
#endif
           );
    printf("  compiler   : %s\n",
#if defined(__clang__)
           "clang " __clang_version__
#elif defined(__GNUC__)
           "gcc " __VERSION__
#else
           "unknown"
#endif
           );
    printf("  baseline   : cglm (git HEAD), -O3 -march=native, its own SIMD paths on\n");
    printf("  method     : %d reps paired round-robin, min + median, CV reported\n", REPS);
    printf("===============================================================\n\n");
}

/* -------------------------------------------------------------------------- */
/* harness                                                                    */
/* -------------------------------------------------------------------------- */

static void csv_row(const char *suite, const char *name, double ns, double cyc,
                    double speedup, double cv, uint64_t checksum)
{
    if (!g_csv) return;
    fprintf(g_csv, "%s,\"%s\",%.4f,%.3f,%.3f,%.3f,%llu\n",
            suite, name, ns, cyc, speedup, cv, (unsigned long long)checksum);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) - (x > y);
}

/* Run one case. base/opt both return a checksum so the compiler cannot delete
 * the work; ops is how many logical operations one call performs. */
static void run_case(const char *suite, const char *name, size_t ops,
                     double (*base)(void *), double (*opt)(void *), void *arg,
                     const char *workingset)
{
    double t_base[REPS], t_opt[REPS];
    double min_base, min_opt, med_base, med_opt, mean, var, cv, geo;
    double logsum = 0.0;
    double chk_base = 0.0, chk_opt = 0.0;
    int r, i;

    for (r = 0; r < WARMUP; ++r)
    {
        chk_base = base(arg);
        chk_opt  = opt(arg);
    }
    OPAQUE(chk_base);
    OPAQUE(chk_opt);

    for (r = 0; r < REPS; ++r)
    {
        double t0, t1;

        t0 = now_ns(); chk_base += base(arg); t1 = now_ns();
        t_base[r] = t1 - t0;

        t0 = now_ns(); chk_opt  += opt(arg);  t1 = now_ns();
        t_opt[r]  = t1 - t0;

        /* Paired ratio, taken before the arrays are sorted (sorting destroys
         * the pairing). AGENTS.md 2.6.3: normalized ratios are summarised by
         * their geometric mean, and this is the statistic that survives a
         * single descheduled rep. min/median-of-ratio-of-mins do not: one
         * lucky rep collapses min_base/min_opt to nonsense while the median
         * stays sane, which is exactly what CV>100% rows in older runs show. */
        if (t_opt[r] > 0.0 && t_base[r] > 0.0)
            logsum += log(t_base[r] / t_opt[r]);
    }
    geo = exp(logsum / (double)REPS);

    qsort(t_base, REPS, sizeof(double), cmp_double);
    qsort(t_opt,  REPS, sizeof(double), cmp_double);

    min_base = t_base[0];
    min_opt  = t_opt[0];
    med_base = t_base[REPS / 2];
    med_opt  = t_opt[REPS / 2];

    mean = 0.0;
    for (i = 0; i < REPS; ++i) mean += t_opt[i];
    mean /= (double)REPS;
    var = 0.0;
    for (i = 0; i < REPS; ++i) var += (t_opt[i] - mean) * (t_opt[i] - mean);
    var /= (double)REPS;
    cv   = mean > 0.0 ? 100.0 * sqrt(var) / mean : 0.0;

    printf("  %-42s %9.2f %7.2f  %8.2fx %8.2fx %8.2fx %5.1f%%\n",
           name,
           med_opt / (double)ops,
           med_opt / (double)ops * g_ghz,
           min_base / min_opt,
           med_base / med_opt,
           geo,
           cv);
    if (workingset) printf("      %s\n", workingset);

    /* The checksum only proves the work was not deleted; it is not a result,
     * so its sign carries no meaning -- fabs keeps the CSV readable instead of
     * printing a negative sum as 18446744073709551615. */
    csv_row(suite, name, med_opt / (double)ops, med_opt / (double)ops * g_ghz,
            med_base / med_opt, cv, (uint64_t)(fabs(chk_opt) * 1000.0));
}

static void print_case_header(const char *title, const char *note)
{
    printf("\n── %s ────────────────────────────────────────────\n", title);
    if (note) printf("   %s\n", note);
    printf("  %-42s %9s %7s  %8s %8s %8s %6s\n",
           "case", "ns/op", "cyc/op", "min-ratio", "med-ratio", "geo-ratio", "cv");
}

/* -------------------------------------------------------------------------- */
/* workload data                                                              */
/* -------------------------------------------------------------------------- */

enum { NMAT = 256, NVEC = 1024, NQUAT = 512, NBONES = 256 };
enum { NBIG = 1 << 20, NSMALL = 1 << 16 };

typedef struct
{
    mu_mat4  *mats_general;   /* arbitrary 4x4            */
    mu_mat4  *mats_rigid;     /* rotation + translation   */
    mu_mat4  *mats_scaled;    /* rotation + axis scale    */
    mu_vec3  *v3a, *v3b;
    mu_quat  *qa, *qb;
    mu_mat3  *m3;

    /* big SoA set, does not fit in cache */
    float *bx, *by, *bz, *box, *boy, *boz;
    /* small SoA set, L2 resident */
    float *sx, *sy, *sz, *sox, *soy, *soz;
    /* AoS points: the idiomatic cglm layout */
    mu_vec3 *apts, *aout;

    mu_mat4    m;
    mu_mat4_hot mh;
    mu_frustum frustum;

    mu_mat4 *ba, *bb, *bd;      /* bone palette batch */

    float sink;
} Bag;

static uint32_t rng_state = 0x12345678u;

static float frand(float lo, float hi)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return lo + (hi - lo) * ((float)(rng_state & 0xFFFFFF) / 16777215.0f);
}

static void rand_general(mu_mat4 out)
{
    int i, j;

    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i)
            out[4 * j + i] = frand(-2.0f, 2.0f);
}

static void rand_rigid(mu_mat4 out)
{
    mu_vec3 translation;
    mu_vec3 axis;
    mu_quat rotation;
    mu_vec3 scale;

    mu_vec3_make(translation, frand(-9, 9), frand(-9, 9), frand(-9, 9));
    mu_vec3_make(axis, frand(-1, 1), frand(-1, 1), frand(-1, 1));
    mu_vec3_normalize(axis, axis);
    mu_quat_axis_angle(rotation, axis, frand(-3.0f, 3.0f));
    mu_vec3_splat(scale, 1.0f);
    mu_mat4_trs(out, translation, rotation, scale);
}

static void rand_scaled(mu_mat4 out)
{
    mu_vec3 translation;
    mu_vec3 axis;
    mu_quat rotation;
    mu_vec3 scale;

    mu_vec3_make(axis, frand(-1, 1), frand(-1, 1), frand(-1, 1));
    mu_vec3_normalize(axis, axis);
    mu_vec3_make(translation, frand(-9, 9), frand(-9, 9), frand(-9, 9));
    mu_quat_axis_angle(rotation, axis, frand(-3.0f, 3.0f));
    mu_vec3_make(scale, frand(0.5f, 2.0f), frand(0.5f, 2.0f), frand(0.5f, 2.0f));
    mu_mat4_trs(out, translation, rotation, scale);
}

static void *bag_new(void)
{
    Bag *b = (Bag *)calloc(1, sizeof(Bag));
    int i;

    b->mats_general = (mu_mat4 *)aligned_alloc(64, sizeof(mu_mat4) * NMAT);
    b->mats_rigid   = (mu_mat4 *)aligned_alloc(64, sizeof(mu_mat4) * NMAT);
    b->mats_scaled  = (mu_mat4 *)aligned_alloc(64, sizeof(mu_mat4) * NMAT);
    b->v3a = (mu_vec3 *)aligned_alloc(64, sizeof(mu_vec3) * NVEC);
    b->v3b = (mu_vec3 *)aligned_alloc(64, sizeof(mu_vec3) * NVEC);
    b->qa  = (mu_quat *)aligned_alloc(64, sizeof(mu_quat) * NQUAT);
    b->qb  = (mu_quat *)aligned_alloc(64, sizeof(mu_quat) * NQUAT);
    b->m3  = (mu_mat3 *)aligned_alloc(64, sizeof(mu_mat3) * NMAT);

    b->bx  = (float *)aligned_alloc(64, sizeof(float) * NBIG);
    b->by  = (float *)aligned_alloc(64, sizeof(float) * NBIG);
    b->bz  = (float *)aligned_alloc(64, sizeof(float) * NBIG);
    b->box = (float *)aligned_alloc(64, sizeof(float) * NBIG);
    b->boy = (float *)aligned_alloc(64, sizeof(float) * NBIG);
    b->boz = (float *)aligned_alloc(64, sizeof(float) * NBIG);

    b->sx  = (float *)aligned_alloc(64, sizeof(float) * NSMALL);
    b->sy  = (float *)aligned_alloc(64, sizeof(float) * NSMALL);
    b->sz  = (float *)aligned_alloc(64, sizeof(float) * NSMALL);
    b->sox = (float *)aligned_alloc(64, sizeof(float) * NSMALL);
    b->soy = (float *)aligned_alloc(64, sizeof(float) * NSMALL);
    b->soz = (float *)aligned_alloc(64, sizeof(float) * NSMALL);

    b->apts = (mu_vec3 *)aligned_alloc(64, sizeof(mu_vec3) * NBIG);
    b->aout = (mu_vec3 *)aligned_alloc(64, sizeof(mu_vec3) * NBIG);

    b->ba = (mu_mat4 *)aligned_alloc(64, sizeof(mu_mat4) * NBONES);
    b->bb = (mu_mat4 *)aligned_alloc(64, sizeof(mu_mat4) * NBONES);
    b->bd = (mu_mat4 *)aligned_alloc(64, sizeof(mu_mat4) * NBONES);

    for (i = 0; i < NMAT; ++i)
    {
        rand_general(b->mats_general[i]);
        rand_rigid(b->mats_rigid[i]);
        rand_scaled(b->mats_scaled[i]);
        mu_mat4_identity(b->mats_general[i]);   /* keep it invertible */
        mu_mat3 linear;
        mu_mat3_from_mat4_linear(linear, b->mats_scaled[i]);
        memcpy(b->m3[i], linear, sizeof(b->m3[i]));
    }
    for (i = 0; i < NVEC; ++i)
    {
        mu_vec3 v0;
        mu_vec3_make(v0, frand(-4, 4), frand(-4, 4), frand(-4, 4));
        memcpy(b->v3a[i], v0, sizeof(b->v3a[i]));
        mu_vec3 v1;
        mu_vec3_make(v1, frand(-4, 4), frand(-4, 4), frand(-4, 4));
        memcpy(b->v3b[i], v1, sizeof(b->v3b[i]));
    }
    for (i = 0; i < NQUAT; ++i)
    {
        mu_vec3 v2;
        mu_vec3_make(v2, frand(-1, 1), frand(-1, 1), frand(-1, 1));
        mu_vec3 normalized0;
        mu_vec3_normalize(normalized0, v2);
        mu_quat quat0;
        mu_quat_axis_angle(quat0, normalized0, frand(-3, 3));
        memcpy(b->qa[i], quat0, sizeof(b->qa[i]));
        mu_vec3 v3;
        mu_vec3_make(v3, frand(-1, 1), frand(-1, 1), frand(-1, 1));
        mu_vec3 normalized1;
        mu_vec3_normalize(normalized1, v3);
        mu_quat quat1;
        mu_quat_axis_angle(quat1, normalized1, frand(-3, 3));
        memcpy(b->qb[i], quat1, sizeof(b->qb[i]));
    }

    for (i = 0; i < NBIG; ++i)
    {
        float x, y, z;

        x = frand(-100, 100); y = frand(-100, 100); z = frand(-100, 100);
        b->bx[i] = x; b->by[i] = y; b->bz[i] = z;
        mu_vec3 v4;
        mu_vec3_make(v4, x, y, z);
        memcpy(b->apts[i], v4, sizeof(b->apts[i]));
    }
    for (i = 0; i < NSMALL; ++i)
    {
        float x, y, z;

        x = frand(-100, 100); y = frand(-100, 100); z = frand(-100, 100);
        b->sx[i] = x; b->sy[i] = y; b->sz[i] = z;
    }
    for (i = 0; i < NBONES; ++i)
    {
        rand_general(b->ba[i]);
        rand_general(b->bb[i]);
    }

    rand_scaled(b->m);
    b->mh = mu_mat4_make_hot(b->m);

    {
        mu_mat4 proj, view, vp;

        mu_mat4_perspective(proj, mu_radians(60.0f), 1.777f, 0.1f, 200.0f);
        const mu_vec3 eye = {0.0f, 0.0f, 10.0f};
        const mu_vec3 target = {0.0f, 0.0f, 0.0f};
        const mu_vec3 up = {0.0f, 1.0f, 0.0f};

        mu_mat4_look_at(view, eye, target, up);
        mu_mat4_mul(proj, view, vp);
        mu_frustum_from_view_proj(&b->frustum, vp);
    }
    return b;
}

/* -------------------------------------------------------------------------- */
/* 1. per-call: single objects                                                */
/* -------------------------------------------------------------------------- */

static double base_vec3_dot(void *a)
{
    Bag *b = (Bag *)a;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
        acc += glm_vec3_dot((float *)b->v3a[i], (float *)b->v3b[i]);
    SINK(acc);
    return (double)acc;
}

static double opt_vec3_dot(void *a)
{
    Bag *b = (Bag *)a;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
        acc += mu_vec3_dot(b->v3a[i], b->v3b[i]);
    SINK(acc);
    return (double)acc;
}

static double base_vec3_cross(void *a)
{
    Bag *b = (Bag *)a;
    vec3 d;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
    {
        glm_vec3_cross((float *)b->v3a[i], (float *)b->v3b[i], d);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_vec3_cross(void *a)
{
    Bag *b = (Bag *)a;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
    {
        mu_vec3 d;
        mu_vec3_cross(d, b->v3a[i], b->v3b[i]);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_vec3_normalize(void *a)
{
    Bag *b = (Bag *)a;
    vec3  d;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
    {
        glm_vec3_normalize_to((float *)b->v3a[i], d);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_vec3_normalize(void *a)
{
    Bag *b = (Bag *)a;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
    {
        mu_vec3 d;
        mu_vec3_normalize(d, b->v3a[i]);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_vec3_normalize_fast(void *a)
{
    Bag *b = (Bag *)a;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
    {
        mu_vec3 d;
        mu_vec3_normalize_fast(d, b->v3a[i]);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_vec3_add(void *a)
{
    Bag *b = (Bag *)a;
    vec3  d;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
    {
        glm_vec3_add((float *)b->v3a[i], (float *)b->v3b[i], d);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_vec3_add(void *a)
{
    Bag *b = (Bag *)a;
    float acc = 0.0f;
    int i;

    for (i = 0; i < NVEC; ++i)
    {
        mu_vec3 d;
        mu_vec3_add(d, b->v3a[i], b->v3b[i]);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

/* ---- normalize as a dependent chain: latency, not throughput ----
 * The throughput case above vectorizes both libraries into the same shape. A
 * chain of normalizes cannot vectorize (each result feeds the next), so this is
 * the regime a per-call API is actually used in: one object at a time. */
enum { CHAIN = 2048 };

static double base_normalize_chain(void *a)
{
    Bag  *b = (Bag *)a;
    vec3  v;
    float acc;
    int   i;

    memcpy(v, b->v3a[0], sizeof v);
    for (i = 0; i < CHAIN; ++i)
    {
        glm_vec3_normalize(v);
        glm_vec3_scale(v, 0.99f, v);   /* keeps the chain alive and off unit */
        v[0] += 1e-3f;
    }
    acc = v[0] + v[1] + v[2];
    SINK(acc);
    return (double)acc;
}

static double opt_normalize_chain(void *a)
{
    Bag    *b = (Bag *)a;
    mu_vec3 v;
    float   acc;
    int     i;

    memcpy(v, b->v3a[0], sizeof(v));
    for (i = 0; i < CHAIN; ++i)
    {
        mu_vec3_normalize(v, v);
        mu_vec3_scale(v, v, 0.99f);
        v[0] += 1e-3f;
    }
    acc = v[0] + v[1] + v[2];
    SINK(acc);
    return (double)acc;
}

static double opt_normalize_fast_chain(void *a)
{
    Bag    *b = (Bag *)a;
    mu_vec3 v;
    float   acc;
    int     i;

    memcpy(v, b->v3a[0], sizeof(v));
    for (i = 0; i < CHAIN; ++i)
    {
        mu_vec3_normalize_fast(v, v);
        mu_vec3_scale(v, v, 0.99f);
        v[0] += 1e-3f;
    }
    acc = v[0] + v[1] + v[2];
    SINK(acc);
    return (double)acc;
}

/* ---- mat4_mul: the case where cglm is already optimal per instruction ---- */

static double base_mat4_mul(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NMAT; ++i)
    {
        glm_mat4_mul((float (*)[4])b->mats_general[i],
                     (float (*)[4])b->mats_rigid[i], d);
        acc += d[0][0] + d[3][3];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_mat4_mul(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NMAT; ++i)
    {
        mu_mat4_mul(b->mats_general[i], b->mats_rigid[i], d);
        acc += d[4 * 0 + 0] + d[4 * 3 + 3];
    }
    SINK(acc);
    return (double)acc;
}

static double base_mat4_mul_affine(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NMAT; ++i)
    {
        glm_mul((float (*)[4])b->mats_rigid[i], (float (*)[4])b->mats_scaled[i], d);
        acc += d[0][0] + d[3][3];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_mat4_mul_affine(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NMAT; ++i)
    {
        mu_mat4_mul_affine(d, b->mats_rigid[i], b->mats_scaled[i]);
        acc += d[4 * 0 + 0] + d[4 * 3 + 3];
    }
    SINK(acc);
    return (double)acc;
}

/* glm_mat4_mulv3 (the vec3 entry point), not glm_mat4_mulv: the latter takes a
 * vec4 and would read 4 bytes past the end of a packed vec3 array. That is not
 * a benchmark detail, it is the kind of thing that segfaults on the last
 * element of a page-aligned buffer. */
static double base_mat4_mulv(void *a)
{
    Bag  *b = (Bag *)a;
    vec3  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NVEC; ++i)
    {
        glm_mat4_mulv3((float (*)[4])b->m, (float *)b->v3a[i], 1.0f, d);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_mat4_mulv(void *a)
{
    Bag     *b = (Bag *)a;
    float    acc = 0.0f;
    int      i;

    for (i = 0; i < NVEC; ++i)
    {
        mu_vec3 point;
        mu_vec4 homogeneous;

        mu_mat4_transform_point(point, b->m, b->v3a[i]);
        mu_vec3_to_vec4(homogeneous, point, 1.0f);
        acc += homogeneous[0];
    }
    SINK(acc);
    return (double)acc;
}

/* Same math, but the matrix is hoisted into registers once. This is the
 * "one object is not the unit of work" argument at its smallest scale. */
static double opt_mat4_hot_point(void *a)
{
    Bag   *b = (Bag *)a;
    float  acc = 0.0f;
    int    i;

    for (i = 0; i < NVEC; ++i)
    {
        mu_vec3 d;
        mu_mat4_hot_point(d, &b->mh, b->v3a[i]);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_mat4_inverse(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NMAT; ++i)
    {
        glm_mat4_inv((float (*)[4])b->mats_general[i], d);
        acc += d[0][0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_mat4_inverse(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NMAT; ++i)
    {
        mu_mat4_inverse(b->mats_general[i], d);
        acc += d[4 * 0 + 0];
    }
    SINK(acc);
    return (double)acc;
}

/* rigid transforms: the fair head-to-head against glm_inv_tr */
static double base_inv_tr(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NMAT; ++i)
    {
        memcpy(d, b->mats_rigid[i], sizeof d);
        glm_inv_tr(d);
        acc += d[0][0];
    }
    SINK(acc);
    return (double)acc;
}

/* orthonormal fast path: transpose + three dots, no reciprocal. The row above
 * uses mu_inverse_affine, which pays three rcp for its scale case; this is the
 * apples-to-apples partner for glm_inv_tr. */
static double opt_inverse_rigid(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NMAT; ++i)
    {
        mu_inverse_rigid(b->mats_rigid[i], d);
        acc += d[4 * 0 + 0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_inverse_affine(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NMAT; ++i)
    {
        mu_inverse_affine(b->mats_rigid[i], d);
        acc += d[4 * 0 + 0];
    }
    SINK(acc);
    return (double)acc;
}

/* and the general inverse on the same work, for the scaled case where
 * glm_inv_tr would be wrong */
static double base_inv_general_rigid(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NMAT; ++i)
    {
        glm_mat4_inv((float (*)[4])b->mats_rigid[i], d);
        acc += d[0][0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_inv_vs_scaled(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NMAT; ++i)
    {
        glm_mat4_inv((float (*)[4])b->mats_scaled[i], d);
        acc += d[0][0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_inverse_affine_scaled(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NMAT; ++i)
    {
        mu_inverse_affine(b->mats_scaled[i], d);
        acc += d[4 * 0 + 0];
    }
    SINK(acc);
    return (double)acc;
}

/* quaternion * vector: cglm's glm_quat_rotatev normalizes the quaternion on
 * every call (documented: it accepts unnormalized input), mu's does not. Both
 * are fed the same already-normalized quaternions, so this is a fair timing of
 * what each API actually asks the CPU to do -- the difference is work cglm
 * chose to do per call. */
static double base_quat_rotate(void *a)
{
    Bag  *b = (Bag *)a;
    /* restrict on BOTH sides: without it GCC cannot prove bx/by/bz and
     * box/boy/boz do not overlap, refuses to vectorize, and both loops become
     * latency-bound on the serial accumulator -- which reads as "2.4x" instead
     * of the ~5x the same code does when the aliasing question is answered. */
    const float * MU_RESTRICT xin = b->bx, * MU_RESTRICT yin = b->by, * MU_RESTRICT zin = b->bz;
    float       * MU_RESTRICT xo  = b->box, * MU_RESTRICT yo = b->boy, * MU_RESTRICT zo = b->boz;
    const mu_quat * MU_RESTRICT qa = b->qa;
    vec3   cv, cd;
    versor cq;
    float  acc = 0.0f;
    int    i;

    /* Walk the quaternion array contiguously (i < NQUAT), like every other
     * per-call row. An i % NQUAT rotating index here defeated vectorization
     * on BOTH sides and silently turned this into a scalar-latency test. */
    for (i = 0; i < NQUAT; ++i)
    {
        memcpy(cq, &qa[i], sizeof cq);
        cv[0] = xin[i]; cv[1] = yin[i]; cv[2] = zin[i];
        glm_quat_rotatev(cq, cv, cd);
        xo[i] = cd[0];
        yo[i] = cd[1];
        zo[i] = cd[2];
        acc += cd[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_quat_rotate(void *a)
{
    Bag  *b = (Bag *)a;
    const float * MU_RESTRICT xin = b->bx, * MU_RESTRICT yin = b->by, * MU_RESTRICT zin = b->bz;
    float       * MU_RESTRICT xo  = b->box, * MU_RESTRICT yo = b->boy, * MU_RESTRICT zo = b->boz;
    const mu_quat * MU_RESTRICT qa = b->qa;
    float  acc = 0.0f;
    int    i;

    for (i = 0; i < NQUAT; ++i)
    {
        mu_vec3 d;
        mu_vec3 v;
        mu_vec3_make(v, xin[i], yin[i], zin[i]);
        mu_quat_rotate_vec3(d, qa[i], v);

        xo[i] = d[0];
        yo[i] = d[1];
        zo[i] = d[2];
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_mat4_transpose(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NMAT; ++i)
    {
        glm_mat4_transpose_to((float (*)[4])b->mats_general[i], d);
        acc += d[0][0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_mat4_transpose(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NMAT; ++i)
    {
        mu_mat4_transpose(b->mats_general[i], d);
        acc += d[4 * 0 + 0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_mat3_inv_transpose(void *a)
{
    Bag  *b = (Bag *)a;
    mat3  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NMAT; ++i)
    {
        glm_mat3_inv((float (*)[3])b->m3[i], d);
        glm_mat3_transpose(d);
        acc += d[0][0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_mat3_inv_transpose(void *a)
{
    Bag    *b = (Bag *)a;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NMAT; ++i)
    {
        mu_mat3 d;
        mu_mat3_inverse_transpose(d, b->m3[i]);
        acc += d[3 * 0 + 0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_quat_mul(void *a)
{
    Bag    *b = (Bag *)a;
    versor  d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NQUAT; ++i)
    {
        glm_quat_mul((float *)b->qa[i], (float *)&b->qb[i], d);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_quat_mul(void *a)
{
    Bag   *b = (Bag *)a;
    float  acc = 0.0f;
    int    i;

    for (i = 0; i < NQUAT; ++i)
    {
        mu_quat d;
        mu_quat_mul(d, b->qa[i], b->qb[i]);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_quat_slerp(void *a)
{
    Bag    *b = (Bag *)a;
    versor  d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NQUAT; ++i)
    {
        glm_quat_slerp((float *)b->qa[i], (float *)&b->qb[i], 0.37f, d);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_quat_slerp(void *a)
{
    Bag   *b = (Bag *)a;
    float  acc = 0.0f;
    int    i;

    for (i = 0; i < NQUAT; ++i)
    {
        mu_quat d;
        mu_quat_slerp(d, b->qa[i], b->qb[i], 0.37f);
        acc += d[0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_mat4_from_quat(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NQUAT; ++i)
    {
        glm_quat_mat4((float *)b->qa[i], d);
        acc += d[0][0];
    }
    SINK(acc);
    return (double)acc;
}

/* cglm normalizes inside the conversion; mu does not (documented contract).
 * This row pays for the normalize so the two are doing identical work. */
static double opt_mat4_from_quat_norm(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NQUAT; ++i)
    {
        mu_quat normalized;
        mu_quat_normalize(normalized, b->qa[i]);
        mu_mat4_from_quat(d, normalized);
        acc += d[4 * 0 + 0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_mat4_from_quat(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    for (i = 0; i < NQUAT; ++i)
    {
        mu_mat4_from_quat(d, b->qa[i]);
        acc += d[4 * 0 + 0];
    }
    SINK(acc);
    return (double)acc;
}

static double base_perspective(void *a)
{
    Bag  *b = (Bag *)a;
    mat4  d;
    float acc = 0.0f;
    int   i;

    (void)b;
    for (i = 0; i < 64; ++i)
    {
        glm_perspective(1.0f + 0.001f * (float)i, 1.777f, 0.1f, 200.0f, d);
        acc += d[2][3];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_perspective(void *a)
{
    Bag    *b = (Bag *)a;
    mu_mat4 d;
    float   acc = 0.0f;
    int     i;

    (void)b;
    for (i = 0; i < 64; ++i)
    {
        mu_mat4_perspective(d, 1.0f + 0.001f * (float)i, 1.777f, 0.1f, 200.0f);
        acc += d[4 * 2 + 3];
    }
    SINK(acc);
    return (double)acc;
}

/* -------------------------------------------------------------------------- */
/* 2. batch: many objects                                                     */
/* -------------------------------------------------------------------------- */

/* idiomatic cglm: AoS points, one call each */
static double base_xform_aos(void *a)
{
    Bag  *b = (Bag *)a;
    vec3 *out = (vec3 *)b->aout;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NBIG; ++i)
    {
        glm_mat4_mulv3((float (*)[4])b->m, (float *)&b->apts[i], 1.0f, out[i]);
        acc += out[i][0];
    }
    SINK(acc);
    return (double)acc;
}

/* Same SoA layout as the mu kernel, plain C loop: isolates implementation
 * quality from layout. Not a cglm case — a control case. */
static double base_xform_soa_scalar(void *a)
{
    Bag   *b = (Bag *)a;
    float  acc = 0.0f;
    int    i;

    for (i = 0; i < NBIG; ++i)
    {
        float x, y, z;

        x = b->bx[i]; y = b->by[i]; z = b->bz[i];
        b->box[i] = b->m[4 * 0 + 0] * x + b->m[4 * 1 + 0] * y + b->m[4 * 2 + 0] * z + b->m[4 * 3 + 0];
        b->boy[i] = b->m[4 * 0 + 1] * x + b->m[4 * 1 + 1] * y + b->m[4 * 2 + 1] * z + b->m[4 * 3 + 1];
        b->boz[i] = b->m[4 * 0 + 2] * x + b->m[4 * 1 + 2] * y + b->m[4 * 2 + 2] * z + b->m[4 * 3 + 2];
        acc += b->box[i];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_xform_soa(void *a)
{
    Bag *b = (Bag *)a;
    float acc;

    mu_xform_points3_soa(b->m, b->bx, b->by, b->bz, b->box, b->boy, b->boz, NBIG);
    acc = b->box[0] + b->box[NBIG - 1] + b->boz[NBIG / 2];
    SINK(acc);
    return (double)acc;
}

enum { SMALL_PASSES = 8 };

static double base_xform_soa_small(void *a)
{
    Bag   *b = (Bag *)a;
    float  acc = 0.0f;
    int    i, p;

    for (p = 0; p < SMALL_PASSES; ++p)
    for (i = 0; i < NSMALL; ++i)
    {
        float x, y, z;

        x = b->sx[i]; y = b->sy[i]; z = b->sz[i];
        b->sox[i] = b->m[4 * 0 + 0] * x + b->m[4 * 1 + 0] * y + b->m[4 * 2 + 0] * z + b->m[4 * 3 + 0];
        b->soy[i] = b->m[4 * 0 + 1] * x + b->m[4 * 1 + 1] * y + b->m[4 * 2 + 1] * z + b->m[4 * 3 + 1];
        b->soz[i] = b->m[4 * 0 + 2] * x + b->m[4 * 1 + 2] * y + b->m[4 * 2 + 2] * z + b->m[4 * 3 + 2];
        acc += b->sox[i];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_xform_soa_small(void *a)
{
    Bag *b = (Bag *)a;
    float acc;
    int   p;

    for (p = 0; p < SMALL_PASSES; ++p)
        mu_xform_points3_soa(b->m, b->sx, b->sy, b->sz, b->sox, b->soy, b->soz, NSMALL);
    acc = b->sox[0] + b->sox[NSMALL - 1];
    SINK(acc);
    return (double)acc;
}

static double base_normalize_soa(void *a)
{
    Bag   *b = (Bag *)a;
    float  acc = 0.0f;
    int    i;

    for (i = 0; i < NBIG; ++i)
    {
        float x, y, z, inv;

        x = b->bx[i]; y = b->by[i]; z = b->bz[i];
        inv = 1.0f / sqrtf(x * x + y * y + z * z);
        b->box[i] = x * inv;
        b->boy[i] = y * inv;
        b->boz[i] = z * inv;
        acc += b->box[i];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_normalize_soa(void *a)
{
    Bag *b = (Bag *)a;
    float acc;

    mu_normalize3_soa(b->bx, b->by, b->bz, b->box, b->boy, b->boz, NBIG);
    acc = b->box[0] + b->boz[NBIG - 1];
    SINK(acc);
    return (double)acc;
}

/* rotate 1M points by one quaternion.
 *
 * base  : glm_quat_rotatev per point (re-normalizes the quaternion per call)
 * ctrl  : mu_quat_rotate_vec3 per point -- same algebra as the kernel, scalar
 *         call per element, so the difference between ctrl and opt is layout
 *         and loop-invariant hoisting, not the algorithm
 * opt   : mu_quat_rotate_vec3_soa
 */
static double base_rotate_1m(void *a)
{
    Bag   *b = (Bag *)a;
    versor cq;
    vec3   cv, cd;
    float  acc = 0.0f;
    int    i;

    memcpy(cq, b->qa[0], sizeof cq);
    for (i = 0; i < NBIG; ++i)
    {
        cv[0] = b->bx[i]; cv[1] = b->by[i]; cv[2] = b->bz[i];
        glm_quat_rotatev(cq, cv, cd);
        b->box[i] = cd[0];
        b->boy[i] = cd[1];
        b->boz[i] = cd[2];
        acc += cd[0];
    }
    SINK(acc);
    return (double)acc;
}

static double ctrl_rotate_1m(void *a)
{
    Bag   *b = (Bag *)a;
    float  acc = 0.0f;
    int    i;

    for (i = 0; i < NBIG; ++i)
    {
        mu_vec3 r;
        mu_vec3 v;
        mu_vec3_make(v, b->bx[i], b->by[i], b->bz[i]);
        mu_quat_rotate_vec3(r, b->qa[0], v);

        b->box[i] = r[0];
        b->boy[i] = r[1];
        b->boz[i] = r[2];
        acc += r[0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_rotate_1m(void *a)
{
    Bag  *b = (Bag *)a;
    float acc;

    mu_quat_rotate_vec3_soa(b->qa[0], b->bx, b->by, b->bz, b->box, b->boy, b->boz, NBIG);
    acc = b->box[0] + b->boz[NBIG - 1];
    SINK(acc);
    return (double)acc;
}

/* frustum cull: scalar loop over cglm-built planes vs compacting kernel.
 * The baseline also writes a visible[] array, because that is what a scalar
 * cull has to hand the next stage; mu emits the compacted index list. */
static double base_cull(void *a)
{
    Bag   *b = (Bag *)a;
    float  acc = 0.0f;
    int    i, k;
    static uint8_t vis[NBIG];

    for (i = 0; i < NBIG; ++i)
    {
        float x = b->apts[i][0], y = b->apts[i][1], z = b->apts[i][2];
        int   inside = 1;

        for (k = 0; k < 6; ++k)
        {
            const float *p = (const float *)b->frustum.p[k];

            if (p[0] * x + p[1] * y + p[2] * z + p[3] < -1.0f) { inside = 0; break; }
        }
        vis[i] = (uint8_t)inside;
        acc += (float)inside;
    }
    SINK(acc);
    SINK(vis[NBIG / 2]);
    return (double)acc;
}

static double opt_cull(void *a)
{
    Bag      *b = (Bag *)a;
    static uint32_t idx[NBIG];
    size_t    n;
    float     acc;

    n   = mu_frustum_cull_spheres_compact(&b->frustum, b->bx, b->by, b->bz, 1.0f, idx, NBIG);
    acc = (float)n + (float)idx[0];
    SINK(acc);
    return (double)acc;
}

/* bone palette: 256 unrelated matrix multiplies */
static double base_mat4_batch(void *a)
{
    Bag  *b = (Bag *)a;
    float acc = 0.0f;
    int   i;

    for (i = 0; i < NBONES; ++i)
    {
        glm_mat4_mul((float (*)[4])b->ba[i], (float (*)[4])b->bb[i], (float (*)[4])b->bd[i]);
        acc += b->bd[i][0];
    }
    SINK(acc);
    return (double)acc;
}

static double opt_mat4_batch(void *a)
{
    Bag *b = (Bag *)a;
    float acc;

    mu_mat4_mul_batch(&b->bd[0][0], sizeof(mu_mat4), &b->ba[0][0], sizeof(mu_mat4),
                      &b->bb[0][0], sizeof(mu_mat4), NBONES);
    acc = b->bd[0][0] + b->bd[NBONES - 1][12];
    SINK(acc);
    return (double)acc;
}

/* Steady state: the palette already lives in lane=matrix form, which is how a
 * skinning system that cared would store it. No pack/unpack in the loop, so
 * this isolates the kernel. */
static double base_mat4_batch_packed(void *a)
{
    return base_mat4_batch(a);
}

/* the packed kernel keeps four matrices in one register; this reads lane 0 of
 * column 0 back out for the sink without depending on the SIMD type */
static float mu_mat4x4_col0_lane0(const mu_mat4x4 *p)
{
    float m[16];
    mu_mat4x4_unpack(m, sizeof(mu_mat4), p);
    return m[0];
}

static double opt_mat4_batch_packed(void *a)
{
    Bag     *b = (Bag *)a;
    mu_mat4x4 pack_a[NBONES / 4], pack_b[NBONES / 4];
    float    acc;
    int      i;

    /* built once outside the timed region in real code; here it is inside the
     * baseline too, so the comparison stays honest (both pay it) */
    for (i = 0; i < NBONES / 4; ++i)
    {
        mu_mat4x4_pack(&pack_a[i], &b->ba[i * 4][0], sizeof(mu_mat4));
        mu_mat4x4_pack(&pack_b[i], &b->bb[i * 4][0], sizeof(mu_mat4));
    }
    for (i = 0; i < NBONES / 4; ++i)
        mu_mat4x4_mul(&pack_a[i], &pack_a[i], &pack_b[i]);

    acc = (float)i + mu_mat4x4_col0_lane0(&pack_a[0]);
    SINK(acc);
    return (double)acc;
}

static double opt_mat4_batch_nopack(void *a)
{
    Bag     *b = (Bag *)a;
    static mu_mat4x4 pack_a[NBONES / 4], pack_b[NBONES / 4];
    float    acc;
    int      i;

    if (mu_mat4x4_col0_lane0(&pack_a[0]) == 0.0f)
    {
        for (i = 0; i < NBONES / 4; ++i)
        {
            mu_mat4x4_pack(&pack_a[i], &b->ba[i * 4][0], sizeof(mu_mat4));
            mu_mat4x4_pack(&pack_b[i], &b->bb[i * 4][0], sizeof(mu_mat4));
        }
    }
    for (i = 0; i < NBONES / 4; ++i)
        mu_mat4x4_mul(&pack_a[i], &pack_a[i], &pack_b[i]);

    acc = (float)i + mu_mat4x4_col0_lane0(&pack_a[0]);
    SINK(acc);
    return (double)acc;
}

/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    Bag *bag;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "--no-pin") != 0)
        pin_to_core(atoi(argv[1]));
    else if (argc <= 1)
        pin_to_core(0);

    g_ghz = measure_ghz();
    print_environment();

    g_csv = fopen("results_math.csv", "w");
    if (g_csv)
        fprintf(g_csv, "suite,case,ns_per_op,cyc_per_op,speedup_vs_cglm,cv_pct,checksum\n");

    bag = bag_new();

    print_case_header("per call: single object, L1/L2-resident inputs",
                      "the regime cglm is already optimal in — expect parity, not magic");
    run_case("call", "vec3_dot",            NVEC,  base_vec3_dot,          opt_vec3_dot,          bag, "2 x 12 KB read");
    run_case("call", "vec3_cross",          NVEC,  base_vec3_cross,        opt_vec3_cross,        bag, "2 x 12 KB read");
    run_case("call", "vec3_add",            NVEC,  base_vec3_add,          opt_vec3_add,          bag, "2 x 12 KB read");
    run_case("call", "vec3_normalize",      NVEC,  base_vec3_normalize,    opt_vec3_normalize,    bag, "12 KB read");
    run_case("call", "vec3_normalize_fast", NVEC,  base_vec3_normalize,    opt_vec3_normalize_fast, bag, "12 KB read");
    run_case("call", "vec3_normalize (dependent chain)", CHAIN, base_normalize_chain, opt_normalize_chain, bag, "latency regime: no ILP, no vectorization");
    run_case("call", "vec3_normalize_fast (chain)",      CHAIN, base_normalize_chain, opt_normalize_fast_chain, bag, "latency regime: rsqrt + 1 NR");
    run_case("call", "mat4_mul",            NMAT,  base_mat4_mul,          opt_mat4_mul,          bag, "256 x 128 B");
    run_case("call", "mat4_mul_affine",     NMAT,  base_mat4_mul_affine,   opt_mat4_mul_affine,   bag, "256 x 128 B");
    run_case("call", "mat4_mulv",           NVEC,  base_mat4_mulv,         opt_mat4_mulv,         bag, "64 B matrix, 12 KB");
    run_case("call", "mat4_point (hot matrix)", NVEC, base_mat4_mulv,      opt_mat4_hot_point,    bag, "64 B matrix hoisted");
    run_case("call", "mat4_transpose",      NMAT,  base_mat4_transpose,    opt_mat4_transpose,    bag, "256 x 64 B");
    run_case("call", "mat4_inverse (general)", NMAT, base_mat4_inverse,    opt_mat4_inverse,      bag, "256 x 64 B");
    run_case("call", "inverse_rigid (rigid)", NMAT, base_inv_tr,           opt_inverse_rigid,     bag, "256 x 64 B");
    run_case("call", "inverse_affine (rigid)", NMAT, base_inv_tr,          opt_inverse_affine,    bag, "256 x 64 B: pays 3 rcp for scale");
    run_case("call", "inverse_affine vs general inv", NMAT, base_inv_general_rigid, opt_inverse_affine, bag, "same work, better algorithm");
    run_case("call", "inverse_affine (scaled)", NMAT, base_inv_vs_scaled,  opt_inverse_affine_scaled, bag, "cglm has no correct cheap path here");
    run_case("call", "mat3_inverse_transpose", NMAT, base_mat3_inv_transpose, opt_mat3_inv_transpose, bag, "256 x 36 B");
    run_case("call", "quat_mul",            NQUAT, base_quat_mul,          opt_quat_mul,          bag, "512 x 32 B");
    run_case("call", "quat_rotate_vec3",    NQUAT, base_quat_rotate,       opt_quat_rotate,       bag, "cglm re-normalizes q per call");
    run_case("call", "quat_slerp",          NQUAT, base_quat_slerp,        opt_quat_slerp,        bag, "512 x 32 B");
    run_case("call", "mat4_from_quat",      NQUAT, base_mat4_from_quat,    opt_mat4_from_quat,    bag, "mu skips the internal normalize (documented)");
    run_case("call", "mat4_from_quat +norm", NQUAT, base_mat4_from_quat,   opt_mat4_from_quat_norm, bag, "identical work to cglm");
    run_case("call", "perspective",         64,    base_perspective,       opt_perspective,       bag, "64 calls");

    print_case_header("batch: many objects — this is the actual thesis",
                      "same math, different unit of work");
    run_case("batch", "xform 1M pts (cglm doing the work)", NBIG, base_xform_aos,       opt_xform_soa, bag, "~28 MB touched: DRAM");
    run_case("batch", "xform 1M pts (same layout, C loop)", NBIG, base_xform_soa_scalar, opt_xform_soa, bag, "control: isolates SIMD from layout");
    run_case("batch", "xform 64K pts x8",                   NSMALL * SMALL_PASSES, base_xform_soa_small, opt_xform_soa_small, bag, "~1.5 MB working set: L2/L3 resident");
    run_case("batch", "normalize 1M vec3",                  NBIG, base_normalize_soa,    opt_normalize_soa, bag, "~24 MB touched");
    run_case("batch", "rotate 1M pts by 1 quat (SoA)",      NBIG, base_rotate_1m,        opt_rotate_1m,     bag, "~36 MB: DRAM stream");
    run_case("batch", "rotate 1M (mu per-call loop)",       NBIG, base_rotate_1m,        ctrl_rotate_1m,    bag, "control: isolates layout+hoisting");
    run_case("batch", "frustum cull 1M spheres",            NBIG, base_cull,             opt_cull,          bag, "~24 MB touch -> compacted list");
    run_case("batch", "mat4 palette 256 (pack+unpack)",     NBONES, base_mat4_batch,        opt_mat4_batch,         bag, "48 KB: L1/L2");
    run_case("batch", "mat4 palette 256 (pack each iter)",  NBONES, base_mat4_batch_packed, opt_mat4_batch_packed,  bag, "isolates the lane=matrix kernel");
    run_case("batch", "mat4 palette 256 (kernel only)",     NBONES, base_mat4_batch,        opt_mat4_batch_nopack,  bag, "already packed: kernel vs cglm AoS");

    printf("\n── done ───────────────────────────────────────────────────\n");
    printf("  results_math.csv written. Ratios are min-of-%d and median-of-%d\n", REPS, REPS);
    printf("  over paired round-robin runs on one pinned core.\n");
    if (g_csv) fclose(g_csv);
    return 0;
}
