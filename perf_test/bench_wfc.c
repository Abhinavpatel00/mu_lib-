/* bench_wfc.c — mu_wfc vs a byte-per-tile twin that runs the SAME algorithm.
 *
 * There is no upstream WFC library to anchor to (the way cglm anchors the math
 * bench), so the baseline here is written in this file: `naive_wfc`, a
 * straightforward implementation of the same constraint-propagation algorithm
 * with the same worklist. The delta between it and mu_wfc is deliberately
 * narrowed to what this header actually claims:
 *
 *   wave representation   uint8_t[cells][tiles]  vs  uint64_t[cells][words]
 *   wave intersect        scan all T entries     vs  one AND (T <= 64)
 *   possibility count     scan the wave          vs  popcount / x & (x-1)
 *   selection scan        all cells, every pick  vs  compacted undecided array
 *   entropy sums          recomputed per pick    vs  maintained incrementally
 *
 * The worklist (and its de-duplication bitmap) is in BOTH, so the ratio below
 * is not the whole story the header tells -- it is #1, #3 and #4 of the four
 * things listed there, with #2 held constant. Saying otherwise would be
 * flattering and wrong.
 *
 * Reps are 9, not the 21 the math bench uses: the twin takes ~1 s on a 64x64
 * grid with 58 tiles, so 21 paired reps of the bigger cases would run for
 * minutes. CV is reported and the claim is the ratio, not the absolute.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>

#include "mu_wfc.h"

static volatile float g_sink;

#define REPS   9
#define WARMUP 1
#define CHAIN_PASSES 1

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

/* ------------------------------------------------------------------ naive */

typedef struct naive_wfc
{
    uint32_t w, h, cells, dirs, tiles;
    float   *tw;
    uint8_t *compat;        /* [dirs][tiles][tiles] */
    uint8_t *wave;          /* [cells][tiles] */
    float   *sum_w, *sum_wlogw;
    uint32_t *queue, qhead, qtail, qcap;
    uint8_t  *queued;
    uint32_t *result;
    uint32_t *neighbor;
    uint8_t  *allowed;      /* [tiles] scratch */
    mu_pcg32  rng;
    uint64_t  seed;
    uint32_t  restarts;
} naive_wfc;

static void naive_reset(naive_wfc *n, uint64_t seed);

static int naive_init(naive_wfc *n, uint32_t w, uint32_t h, uint32_t dirs, uint32_t tiles,
                      const float *tw, uint64_t seed)
{
    uint32_t c, t;

    memset(n, 0, sizeof *n);
    n->w = w; n->h = h; n->dirs = dirs; n->tiles = tiles; n->cells = w * h; n->seed = seed;
    n->tw       = (float *)malloc(tiles * 4u);
    n->compat   = (uint8_t *)calloc((size_t)dirs * tiles * tiles, 1u);
    n->wave     = (uint8_t *)malloc((size_t)n->cells * tiles);
    n->sum_w    = (float *)malloc(n->cells * 4u);
    n->sum_wlogw = (float *)malloc(n->cells * 4u);
    n->queued   = (uint8_t *)calloc(n->cells, 1u);   /* MUST be zeroed: it gates pushes */
    n->result   = (uint32_t *)malloc(n->cells * 4u);
    n->neighbor = (uint32_t *)malloc((size_t)n->cells * dirs * 4u);
    n->allowed  = (uint8_t *)malloc(tiles);
    n->qcap     = n->cells + 1u;
    n->queue    = (uint32_t *)malloc((size_t)n->qcap * 4u);
    if (!n->tw || !n->compat || !n->wave || !n->sum_w || !n->sum_wlogw || !n->queued ||
        !n->result || !n->neighbor || !n->allowed || !n->queue)
        return 0;

    for (t = 0; t < tiles; ++t) n->tw[t] = tw[t];
    for (c = 0; c < n->cells * dirs; ++c) n->neighbor[c] = MU_WFC_NONE;

    naive_reset(n, seed);
    return 1;
}

/* Wipe the wave and re-seed. Called once by init and again on every restart,
 * because mu_wfc_solve restarts on contradiction and the comparison has to be
 * like for like: a baseline that gives up where the other one retries is not a
 * baseline, it is a different algorithm. */
static void naive_reset(naive_wfc *n, uint64_t seed)
{
    uint32_t c, t;

    for (c = 0; c < n->cells; ++c)
    {
        float sw = 0.0f, swl = 0.0f;

        for (t = 0; t < n->tiles; ++t)
        {
            n->wave[(size_t)c * n->tiles + t] = 1u;
            sw  += n->tw[t];
            swl += n->tw[t] * logf(n->tw[t]);
        }
        n->sum_w[c]     = sw;
        n->sum_wlogw[c] = swl;
        n->result[c]    = MU_WFC_NONE;
        n->queued[c]    = 0u;
    }
    n->qhead = n->qtail = 0u;
    mu_pcg32_init(&n->rng, seed, 0x9E3779B97F4A7C15ull);
}

static void naive_free(naive_wfc *n)
{
    free(n->tw); free(n->compat); free(n->wave); free(n->sum_w); free(n->sum_wlogw);
    free(n->queued); free(n->result); free(n->neighbor); free(n->allowed); free(n->queue);
}

static void naive_neighbors_grid(naive_wfc *n)
{
    static const int dx[4] = { 1, 0, -1, 0 };
    static const int dy[4] = { 0, 1, 0, -1 };
    uint32_t x, y, d;

    for (y = 0; y < n->h; ++y)
        for (x = 0; x < n->w; ++x)
            for (d = 0; d < n->dirs; ++d)
            {
                const int nx = (int)x + dx[d], ny = (int)y + dy[d];

                if (nx < 0 || ny < 0 || nx >= (int)n->w || ny >= (int)n->h) continue;
                n->neighbor[((size_t)y * n->w + x) * n->dirs + d] = (uint32_t)ny * n->w + (uint32_t)nx;
            }
}

static void naive_push(naive_wfc *n, uint32_t c)
{
    if (n->queued[c]) return;
    n->queued[c] = 1u;
    n->queue[n->qtail] = c;
    n->qtail = (n->qtail + 1u == n->qcap) ? 0u : n->qtail + 1u;
}

static int naive_run(naive_wfc *n)
{
    while (1)
    {
        uint32_t c, best = MU_WFC_NONE;
        float    bsum = 0.0f, bent = 0.0f;

        /* selection: scan EVERY cell, recompute the weight sum from the wave */
        for (c = 0; c < n->cells; ++c)
        {
            const uint8_t *wr = n->wave + (size_t)c * n->tiles;
            uint32_t t, cnt = 0;
            float    sw = 0.0f, swl = 0.0f;

            for (t = 0; t < n->tiles; ++t)
                if (wr[t]) { ++cnt; sw += n->tw[t]; swl += n->tw[t] * logf(n->tw[t]); }
            if (cnt <= 1u) continue;

            {
                const float ent = logf(sw) - swl / sw;

                if (best == MU_WFC_NONE || sw < bsum || (sw == bsum && ent < bent))
                { best = c; bsum = sw; bent = ent; }
            }
        }
        if (best == MU_WFC_NONE) break;

        /* collapse: weighted pick, then propagate */
        {
            uint8_t *wr = n->wave + (size_t)best * n->tiles;
            const float r = ((float)(mu_pcg32_next_u32(&n->rng) >> 8) * (1.0f / 16777216.0f)) * bsum;
            float    acc = 0.0f;
            uint32_t t;
            int      chosen = -1;

            for (t = 0; t < n->tiles; ++t)
                if (wr[t]) { acc += n->tw[t]; if (acc > r) { chosen = (int)t; break; } }
            if (chosen < 0)
                for (t = n->tiles; t-- > 0u; ) if (wr[t]) { chosen = (int)t; break; }
            if (chosen < 0) return MU_WFC_CONTRADICTION;
            for (t = 0; t < n->tiles; ++t) wr[t] = (t == (uint32_t)chosen) ? 1u : 0u;
            n->result[best] = (uint32_t)chosen;
            naive_push(n, best);
        }

        /* propagate */
        while (n->qhead != n->qtail)
        {
            const uint32_t c2 = n->queue[n->qhead];
            const uint8_t *wc = n->wave + (size_t)c2 * n->tiles;
            uint32_t d;

            n->qhead = (n->qhead + 1u == n->qcap) ? 0u : n->qhead + 1u;
            n->queued[c2] = 0u;

            for (d = 0; d < n->dirs; ++d)
            {
                const uint32_t nb = n->neighbor[(size_t)c2 * n->dirs + d];
                uint8_t  *wn;
                uint32_t t, t2, cnt = 0, last = 0;
                int      changed = 0;

                if (nb == MU_WFC_NONE) continue;
                wn = n->wave + (size_t)nb * n->tiles;

                for (t = 0; t < n->tiles; ++t) n->allowed[t] = 0u;
                for (t = 0; t < n->tiles; ++t)
                    if (wc[t])
                        for (t2 = 0; t2 < n->tiles; ++t2)
                            if (n->compat[((size_t)d * n->tiles + t) * n->tiles + t2]) n->allowed[t2] = 1u;

                for (t = 0; t < n->tiles; ++t)
                {
                    if (wn[t] && !n->allowed[t]) { wn[t] = 0u; changed = 1; }
                }
                if (!changed) continue;

                for (t = 0; t < n->tiles; ++t) if (wn[t]) { ++cnt; last = t; }
                if (cnt == 0u) return MU_WFC_CONTRADICTION;
                if (cnt == 1u) n->result[nb] = last;
                naive_push(n, nb);
            }
        }
    }
    return MU_WFC_DONE;
}

/* --------------------------------------------------------------- instance */

typedef struct
{
    uint8_t *patterns;
    float   *weights;
    uint32_t tiles;
    uint32_t n;
    uint32_t grid;          /* w == h == grid */
} instance;

/* Three synthetic samples, chosen so the tile set spans the interesting range:
 *   STRIPES 12 tiles  -- almost no propagation work, the floor
 *   SEAMS   42 tiles  -- real overlap constraints, the main case
 *   SEAMS_W 72 tiles  -- crosses 64, so the wave needs more than one word
 * The samples are periodic by construction, which keeps the constraint graph
 * strongly connected and the instance satisfiable; no case needs a restart. */
enum { SAMPLE_STRIPES = 0, SAMPLE_SEAMS = 1, SAMPLE_SEAMS_WIDE = 2 };

static int instance_build(instance *in, uint32_t variant, uint32_t grid, uint32_t n)
{
    enum { SW = 32, SH = 32 };
    static uint8_t sample[SW * SH];
    uint32_t maxw = 0;
    int      x, y;

    for (y = 0; y < SH; ++y)
        for (x = 0; x < SW; ++x)
        {
            int v = ((x / 2) + (y / 2)) % 3;

            if (variant == SAMPLE_SEAMS && (x % 5) == 0) v = 3;
            if (variant == SAMPLE_SEAMS_WIDE && ((x + y) % 7) == 0) v = 3;
            sample[y * SW + x] = (uint8_t)v;
        }

    in->n = n;
    in->grid = grid;
    in->tiles = mu_wfc_pattern_count(sample, SW, SH, n, &maxw);
    if (in->tiles < 2u) return 0;
    in->patterns = (uint8_t *)malloc((size_t)in->tiles * n * n);
    in->weights  = (float *)calloc(in->tiles, 4u);
    if (!in->patterns || !in->weights) return 0;
    if (mu_wfc_extract_patterns(sample, SW, SH, n, in->patterns, in->weights, in->tiles) != in->tiles)
        return 0;
    return 1;
}

/* ------------------------------------------------------------ the two runs */

static uint64_t mu_solve_checksum(const instance *in, uint64_t seed, int *ok, uint32_t *restarts)
{
    mu_wfc  *wf = mu_wfc_create(in->grid, in->grid, 4, in->tiles, in->weights, seed);
    uint64_t sum = 0;
    uint32_t c;

    *ok = 0;
    if (!wf) return 0;
    mu_wfc_neighbors_grid(wf, 0);
    mu_wfc_constrain_from_patterns(wf, in->patterns, in->n);
    *ok = (mu_wfc_solve(wf, 8) == MU_WFC_DONE);
    *restarts = wf->restarts;
    if (*ok)
        for (c = 0; c < wf->cells; ++c)
        {
            sum = sum * 1000003ull + wf->result[c];

            /* validity: every adjacency legal (the same gate as the test) */
            {
                uint32_t d;

                for (d = 0; d < wf->dirs; ++d)
                {
                    const uint32_t nb = wf->neighbor[(size_t)c * wf->dirs + d];

                    if (nb != MU_WFC_NONE && !mu_wfc_is_allowed(wf, d, wf->result[c], wf->result[nb]))
                        *ok = 0;
                }
            }
        }
    mu_wfc_destroy(wf);
    return sum;
}

static uint64_t naive_solve_checksum(const instance *in, uint64_t seed, int *ok, uint32_t *restarts)
{
    naive_wfc n;
    uint64_t  sum = 0;
    uint32_t  c;

    *ok = 0;
    if (!naive_init(&n, in->grid, in->grid, 4, in->tiles, in->weights, seed)) return 0;
    naive_neighbors_grid(&n);

    /* same constraints as mu: pairwise pattern overlap, symmetric */
    {
        uint32_t a, b, d;
        const uint32_t k = in->n;

        for (a = 0; a < in->tiles; ++a)
            for (b = 0; b < in->tiles; ++b)
                for (d = 0; d < 4u; ++d)
                {
                    const int dx = (d == 0u) ? 1 : (d == 2u ? -1 : 0);
                    const int dy = (d == 1u) ? 1 : (d == 3u ? -1 : 0);
                    int i, j, good = 1;

                    for (i = 0; i < (int)k && good; ++i)
                        for (j = 0; j < (int)k && good; ++j)
                        {
                            const int ax = j + dx, ay = i + dy;

                            if (ax < 0 || ay < 0 || ax >= (int)k || ay >= (int)k) continue;
                            if (in->patterns[(size_t)a * k * k + (size_t)ay * k + (size_t)ax] !=
                                in->patterns[(size_t)b * k * k + (size_t)i * k + (size_t)j])
                                good = 0;
                        }
                    if (good)
                    {
                        n.compat[((size_t)d * in->tiles + a) * in->tiles + b] = 1u;
                        /* the twin is given the same symmetric relation */
                        n.compat[(((size_t)((d + 2u) % 4u) * in->tiles + b) * in->tiles + a)] = 1u;
                    }
                }
    }

    {
        int done = 0;

        for (n.restarts = 0u; n.restarts <= 8u; ++n.restarts)
        {
            if (n.restarts) naive_reset(&n, seed + (uint64_t)n.restarts * 0x9E3779B97F4A7C15ull);
            if (naive_run(&n) == MU_WFC_DONE) { done = 1; break; }
        }
        *ok = done;
        if (restarts) *restarts = n.restarts;
    }
    if (*ok)
        for (c = 0; c < n.cells; ++c) sum = sum * 1000003ull + n.result[c];
    naive_free(&n);
    return sum;
}

/* ------------------------------------------------------------------ driver */

typedef struct
{
    double t_naive[REPS], t_mu[REPS];
    uint64_t sum_naive, sum_mu;
    int      ok_naive, ok_mu;
    uint32_t restarts, naive_restarts;
} result;

static void run_case(const char *name, uint32_t variant, uint32_t grid, uint32_t n,
                     uint64_t seed, result *out)
{
    instance in;
    int      r, i;

    if (!instance_build(&in, variant, grid, n))
    {
        printf("  %-34s <instance build failed>\n", name);
        return;
    }

    for (r = 0; r < WARMUP; ++r)
    {
        out->sum_naive = naive_solve_checksum(&in, seed, &out->ok_naive, &out->naive_restarts);
        out->sum_mu    = mu_solve_checksum(&in, seed, &out->ok_mu, &out->restarts);
    }
    for (r = 0; r < REPS; ++r)
    {
        double t0, t1;

        t0 = now_ns(); out->sum_naive = naive_solve_checksum(&in, seed, &out->ok_naive, &out->naive_restarts); t1 = now_ns();
        out->t_naive[r] = t1 - t0;

        t0 = now_ns(); out->sum_mu = mu_solve_checksum(&in, seed, &out->ok_mu, &out->restarts); t1 = now_ns();
        out->t_mu[r] = t1 - t0;
    }

    qsort(out->t_naive, REPS, sizeof(double), cmp_double);
    qsort(out->t_mu, REPS, sizeof(double), cmp_double);

    {
        double mean = 0.0, var = 0.0, geo = 0.0, med_n = out->t_naive[REPS / 2], med_m = out->t_mu[REPS / 2];

        for (i = 0; i < REPS; ++i) { mean += out->t_mu[i]; geo += log(out->t_naive[i] / out->t_mu[i]); }
        mean /= REPS;
        for (i = 0; i < REPS; ++i) var += (out->t_mu[i] - mean) * (out->t_mu[i] - mean);
        var /= REPS;

        printf("  %-34s %6u %9.1f %9.2f %8.2fx %8.2fx %6.1f%%   %-12s %u/%u\n",
               name, in.tiles,
               med_n / 1e6, med_m / 1e6,
               out->t_naive[0] / out->t_mu[0], med_n / med_m,
               mean > 0.0 ? 100.0 * sqrt(var) / mean : 0.0,
               out->ok_naive ? (out->ok_mu ? "both valid" : "mu INVALID") : "twin INVALID",
               out->naive_restarts, out->restarts);
    }
    free(in.patterns);
    free(in.weights);
}

/* absolute scaling for mu only: the twin is too slow to run at these sizes,
 * which is itself part of the result */
static void run_mu_only(const char *name, uint32_t variant, uint32_t grid, uint32_t n, uint64_t seed)
{
    instance in;
    int      r;
    double   t[REPS];
    int      ok = 0;
    uint32_t restarts = 0;

    char     restarts_str[16];

    if (!instance_build(&in, variant, grid, n)) return;

    for (r = 0; r < WARMUP; ++r) mu_solve_checksum(&in, seed, &ok, &restarts);
    for (r = 0; r < REPS; ++r)
    {
        double t0 = now_ns();
        mu_solve_checksum(&in, seed, &ok, &restarts);
        t[r] = now_ns() - t0;
    }
    qsort(t, REPS, sizeof(double), cmp_double);

    {
        double mean = 0.0, var = 0.0, med = t[REPS / 2];
        int    i;

        for (i = 0; i < REPS; ++i) mean += t[i];
        mean /= REPS;
        for (i = 0; i < REPS; ++i) var += (t[i] - mean) * (t[i] - mean);
        var /= REPS;

        snprintf(restarts_str, sizeof restarts_str, "%u", restarts);
        printf("  %-34s %6u %9.3f %9.2f %8s %8s %6.1f%%   %-12s %s\n",
               name, in.tiles, med / 1e6, med / (double)(grid * grid), "-", "-",
               mean > 0.0 ? 100.0 * sqrt(var) / mean : 0.0,
               ok ? "valid" : "INVALID", restarts_str);
    }
    free(in.patterns);
    free(in.weights);
}

static void pin_to_core(int core)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(core, &set);
    sched_setaffinity(0, sizeof set, &set);
}

int main(int argc, char **argv)
{
    int ablate = (argc > 1 && strcmp(argv[1], "--mu-only") == 0);

    pin_to_core(0);

    printf("===============================================================\n");
    printf(" mu_wfc vs byte-per-tile twin — same algorithm, different data\n");
    printf("===============================================================\n");
    printf("  cpu        : ");
    fflush(stdout);
    if (system("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-") != 0) printf("<unknown>\n");
    printf("  affinity   : 1 core pinned\n");
    printf("  compiler   : gcc, -O3 -march=native -std=c11\n");
    printf("  baseline   : naive_wfc in this file (byte wave, full T-scan intersect,\n");
    printf("               scan-all-cells selection, entropy recomputed per pick)\n");
    printf("  shared     : worklist + de-dup bitmap, identical constraints, identical seed\n");
    printf("  method     : %d paired round-robin reps, %d warmup, min + median + CV\n", REPS, WARMUP);
    printf("  instance   : 32x32 sample, n x n patterns, weights = occurrence counts\n");
    printf("===============================================================\n\n");

    if (ablate)
    {
        printf("── ablation build: %s%s%s ──\n",
               "mu only", "", "");
        printf("  %-34s %6s %9s %9s %9s %9s %7s   %-12s %s\n",
               "case", "tiles", "ms", "ns/cell", "-", "-", "cv", "status", "rst");
        run_mu_only("64x64   n=3 seams (42)",  SAMPLE_SEAMS,      64,  3, 0xC0FFEEull);
        run_mu_only("128x128 n=3 seams (42)",  SAMPLE_SEAMS,     128,  3, 0xC0FFEEull);
        run_mu_only("256x256 n=3 seams (42)",  SAMPLE_SEAMS,     256,  3, 0xC0FFEEull);
        run_mu_only("256x256 n=2 seams (24)",  SAMPLE_SEAMS,     256,  2, 0xC0FFEEull);
        run_mu_only("256x256 n=3 wide  (72)",  SAMPLE_SEAMS_WIDE, 256, 3, 0xC0FFEEull);
        printf("\n── done ────────────────────────────────────────────────────────\n");
        return 0;
    }

    printf("── head to head (both implementations run) ─────────────────────\n");
    printf("  %-34s %6s %9s %9s %9s %9s %7s   %-12s %s\n",
           "case", "tiles", "twin ms", "mu ms", "min-ratio", "med-ratio", "cv", "status",
           "rst tw/mu");

    {
        result r;

        run_case("32x32  n=3  stripes (12)", SAMPLE_STRIPES,   32, 3, 0xC0FFEEull, &r);
        run_case("32x32  n=2  seams   (24)", SAMPLE_SEAMS,     32, 2, 0xC0FFEEull, &r);
        run_case("32x32  n=3  seams   (42)", SAMPLE_SEAMS,     32, 3, 0xC0FFEEull, &r);
        run_case("48x48  n=3  seams   (42)", SAMPLE_SEAMS,     48, 3, 0xC0FFEEull, &r);
        run_case("64x64  n=3  seams   (42)", SAMPLE_SEAMS,     64, 3, 0xC0FFEEull, &r);
        run_case("64x64  n=4  seams   (60)", SAMPLE_SEAMS,     64, 4, 0xC0FFEEull, &r);
        run_case("48x48  n=3  wide    (72)", SAMPLE_SEAMS_WIDE, 48, 3, 0xC0FFEEull, &r);
        run_case("64x64  n=3  wide    (72)", SAMPLE_SEAMS_WIDE, 64, 3, 0xC0FFEEull, &r);
    }

    printf("\n── mu only: the twin is minutes at these sizes ─────────────────\n");
    printf("  %-34s %6s %9s %9s %9s %9s %7s   %-12s %s\n",
           "case", "tiles", "ms", "ns/cell", "-", "-", "cv", "status", "rst");

    run_mu_only("128x128 n=3 seams (42)", SAMPLE_SEAMS,      128, 3, 0xC0FFEEull);
    run_mu_only("256x256 n=3 seams (42)", SAMPLE_SEAMS,      256, 3, 0xC0FFEEull);
    run_mu_only("512x512 n=3 seams (42)", SAMPLE_SEAMS,      512, 3, 0xC0FFEEull);
    run_mu_only("256x256 n=3 wide  (72)", SAMPLE_SEAMS_WIDE, 256, 3, 0xC0FFEEull);
    run_mu_only("256x256 n=2 seams (24)", SAMPLE_SEAMS,      256, 2, 0xC0FFEEull);

    printf("\n── done ────────────────────────────────────────────────────────\n");
    g_sink = 0.0f;
    return 0;
}
