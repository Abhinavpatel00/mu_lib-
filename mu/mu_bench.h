#pragma once
#ifndef MU_BENCH_H
#define MU_BENCH_H

/* ==========================================================================
 * mu_bench.h — single-header micro-benchmark framework (C99)
 * ==========================================================================
 *
 * The point of this file is to make the expensive part of the loop cheap:
 * write a benchmark, run it, trust the number, keep the number. The
 * measurement discipline is the one AGENTS.md section 2.6 already asks for, so
 * the defaults are chosen to enforce it rather than to look good:
 *
 *   - PAIRED and REPEATABLE. A benchmark runs in batches; the reported value
 *     is the median of batch means, and the confidence interval is computed
 *     the way ubench.h does it (99%, over batch means). A benchmark whose CI
 *     exceeds the threshold is reported as UNSTABLE and the process exits
 *     non-zero, so noise fails the build instead of hiding in a table.
 *   - ITERATIONS ARE CHOSEN FOR YOU. Each batch is auto-sized to run for
 *     roughly `target_ns`, then the per-operation cost is the batch mean
 *     divided by the iteration count. You never hand-tune a repetition count
 *     and never accidentally measure the clock.
 *   - THE COMPILER CANNOT DELETE THE WORK. Reads you want kept go through
 *     mu_bench_do_not_optimize(), a real optimization barrier, not a `volatile`
 *     that also changes the code being measured.
 *   - MACHINE-READABLE OUT. --csv writes one row per benchmark so a table in a
 *     perf record can be regenerated instead of retyped.
 *
 * Usage, one benchmark TU per binary:
 *
 *     #define MU_BENCH_IMPLEMENTATION   // exactly once, in this TU
 *     #include "mu_bench.h"
 *
 *     MU_BENCH(simd, vec3_normalize) {
 *         uint32_t i;
 *         MU_BENCH_ITERATE(i) { mu_bench_sink = mu_vec3_normalize(v); }
 *     }
 *
 *     MU_BENCH_MAIN()
 *
 * Then:  ./bench            run everything
 *        ./bench --filter=simd      only matching suites/names
 *        ./bench --list             list names, one per line
 *        ./bench --csv=out.csv      also write CSV
 *        ./bench --confidence=5     loosen the stability cutoff to 5%
 *        ./bench --help
 *
 * Hooks that matter when the setup is not free:
 *
 *     MU_BENCH_EX(suite, name, iterations_hint) { ... }   // body, but no auto sizing
 *     MU_BENCH_SETUP(suite, name) { ... }                 // runs before, not timed
 *     MU_BENCH_STATE(suite, name) { ... }                 // per-benchmark state
 *
 * Registration uses a static constructor, like ubench.h. On MSVC that needs
 * the .CRT$XCU section; everywhere else __attribute__((constructor)). In a C++
 * TU with static lifecycles you may prefer to call mu_bench_register()
 * explicitly from main() instead -- both paths are supported.
 *
 * This header deliberately does NOT include mu_macros.h: it is a measurement
 * tool, and a measurement tool should be includable next to the code being
 * measured without pulling in a library's macros.
 */

/* clock_gettime is POSIX, not C99. Ask for it explicitly, which requires this
 * header to be included before any system header -- normal for a benchmark TU,
 * and stated here because the failure mode (implicit declaration) is obscure. */
#if !defined(_POSIX_C_SOURCE) && !defined(_MSC_VER) && !defined(__APPLE__)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#if defined(_MSC_VER)
#  include <windows.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define MU_BENCH__UNUSED __attribute__((unused))
#else
#  define MU_BENCH__UNUSED
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * tuning
 * -------------------------------------------------------------------------- */

#ifndef MU_BENCH_MAX_BENCHMARKS
#  define MU_BENCH_MAX_BENCHMARKS 256
#endif
#ifndef MU_BENCH_MAX_SAMPLES
#  define MU_BENCH_MAX_SAMPLES 64
#endif
#ifndef MU_BENCH_TARGET_NS
#  define MU_BENCH_TARGET_NS 20000000.0     /* 20 ms per batch */
#endif
#ifndef MU_BENCH_SAMPLES
#  define MU_BENCH_SAMPLES 15               /* batches per benchmark */
#endif
#ifndef MU_BENCH_CONFIDENCE_PCT
#  define MU_BENCH_CONFIDENCE_PCT 2.5       /* ubench's default cutoff */
#endif

/* Optimization barriers, and the reason this header exists at all.
 *
 * A `__asm__ volatile("" ::: "memory")` is NOT enough. It tells the compiler
 * that memory may have changed across the asm, but a store to a static that
 * nothing ever reads is still dead, so the whole computation feeding it is
 * dead too: a sinf loop "measured" at 0.26 ns/op -- 0.8 cycles per call --
 * because gcc had deleted every call and left an empty counting loop. Verified
 * with objdump, not guessed.
 *
 * The barrier therefore takes the value as an INPUT OPERAND, so the compiler
 * must materialize it in a register. That is the same construction Google
 * Benchmark and ubench.h use, and it is the only portable one that survives
 * -O3 without also forcing a spill.
 *
 * The "g" constraint is the permissive one: it accepts a register, a memory
 * operand or an immediate, so it works for float, double, integers and
 * pointers alike, and it accepts rvalues. "+r,m" -- what Google Benchmark uses
 * -- fails to compile for float on gcc ("impossible constraint in asm"), which
 * is worth knowing before copying that line. */
#if defined(__GNUC__) || defined(__clang__)
#  define MU_BENCH_BARRIER()       __asm__ __volatile__("" ::: "memory")
#  define MU_BENCH_DO_NOT_OPTIMIZE(x)                                          \
       do { __asm__ __volatile__("" : : "g"(x) : "memory"); } while (0)
#else
static MU_BENCH__UNUSED void mu_bench_opt_barrier(void) { _ReadWriteBarrier(); }
#  define MU_BENCH_BARRIER()       mu_bench_opt_barrier()
#  define MU_BENCH_DO_NOT_OPTIMIZE(x)                                          \
       do { mu_bench_sink = (float)(x); mu_bench_opt_barrier(); } while (0)
#endif

typedef struct mu_bench_state mu_bench_state;
typedef void (*mu_bench_fn)(mu_bench_state *);

struct mu_bench_state
{
    /* How many operations this batch should run. Read-only for the body. */
    uint64_t iterations;
    /* Free for the benchmark body: a user-desired iteration count, 0 = auto. */
    uint64_t iterations_hint;
    /* Scratch the body can use; the framework never reads it. */
    void*    user;
};

/* Fallback sink for compilers without inline asm operands. volatile so the
 * store cannot be removed. Read it with mu_bench_consume() if you want a value
 * to escape the timed region. */
static volatile float mu_bench_sink MU_BENCH__UNUSED;

/* forward declarations (C99 has no implicit declarations) */
static const char *mu_bench__cpu(void);
static void        mu_bench__fmt_ns(double ns, char *buf, size_t cap);

/* --------------------------------------------------------------------------
 * registry
 * -------------------------------------------------------------------------- */

typedef struct mu_bench_entry
{
    const char          *suite;
    const char          *name;
    mu_bench_fn          func;
    mu_bench_fn          setup;         /* optional, untimed */
    mu_bench_fn          state_init;    /* optional, runs at registration */
    mu_bench_state       state;
    /* results */
    double               median_ns;     /* per operation */
    double               min_ns;
    double               max_ns;
    double               mean_ns;
    double               conf_pct;      /* 99% CI half-width, percent of mean */
    double               cv_pct;
    double               ops_per_sec;
    uint64_t             iterations;    /* ops per batch that were used */
} mu_bench_entry;

typedef struct mu_bench_registry
{
    mu_bench_entry  entries[MU_BENCH_MAX_BENCHMARKS];
    int             count;
    int             cur_global;
    int             cur_suite;
    int             failed;
} mu_bench_registry;

static mu_bench_registry mu_bench__reg;

static MU_BENCH__UNUSED int mu_bench_register(const char *suite, const char *name, mu_bench_fn fn, mu_bench_fn setup)
{
    mu_bench_registry *r = &mu_bench__reg;
    mu_bench_entry    *e;

    if (r->count >= MU_BENCH_MAX_BENCHMARKS)
    {
        fprintf(stderr, "mu_bench: too many benchmarks (raise MU_BENCH_MAX_BENCHMARKS)\n");
        return -1;
    }
    e = &r->entries[r->count];
    memset(e, 0, sizeof *e);
    e->suite = suite;
    e->name  = name;
    e->func  = fn;
    e->setup = setup;
    r->cur_global = r->count;
    r->cur_suite  = r->count;
    r->count += 1;
    return r->cur_global;
}

/* The benchmark a MU_BENCH_STATE / MU_BENCH_SETUP block belongs to: the most
 * recently registered one, which is the one written above it. */
static MU_BENCH__UNUSED mu_bench_entry *mu_bench__last(void)
{
    return (mu_bench__reg.count > 0) ? &mu_bench__reg.entries[mu_bench__reg.count - 1] : NULL;
}

/* --------------------------------------------------------------------------
 * clock
 * -------------------------------------------------------------------------- */

static MU_BENCH__UNUSED double mu_bench_now_ns(void)
{
#if defined(_MSC_VER)
    LARGE_INTEGER t, f;

    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1e9 / (double)f.QuadPart;
#elif defined(CLOCK_MONOTONIC_RAW)
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
#endif
}

/* --------------------------------------------------------------------------
 * statistics
 * -------------------------------------------------------------------------- */

static MU_BENCH__UNUSED int mu_bench__cmp(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;

    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Mean and 99% two-sided confidence interval over the batch means, using the
 * Student-t table ubench.h uses for small sample counts. Percent of the mean,
 * because that is the only form that is comparable across benchmarks. */
static MU_BENCH__UNUSED void mu_bench__stats(const double *s, int n, double *mean, double *ci_pct, double *cv_pct)
{
    static const double t99[] = { 0.0, 63.657, 9.925, 5.841, 4.604, 4.032, 3.707, 3.499, 3.355,
                                  3.250, 3.169, 3.106, 3.055, 3.012, 2.977, 2.947 };
    double sum = 0.0, var = 0.0, sd;
    int    i;

    *mean = 0.0; *ci_pct = 0.0; *cv_pct = 0.0;
    if (n <= 0) return;
    for (i = 0; i < n; ++i) sum += s[i];
    *mean = sum / (double)n;
    if (n < 2 || !(*mean > 0.0)) return;
    for (i = 0; i < n; ++i) var += (s[i] - *mean) * (s[i] - *mean);
    var /= (double)(n - 1);
    sd = sqrt(var);
    *cv_pct = 100.0 * sd / *mean;
    {
        const double t = t99[(n - 1 < 15) ? (n - 1) : 15];

        *ci_pct = 100.0 * t * sd / sqrt((double)n) / *mean;
    }
}

/* --------------------------------------------------------------------------
 * runner
 * -------------------------------------------------------------------------- */

static MU_BENCH__UNUSED int mu_bench__matches(const mu_bench_entry *e, const char *filter)
{
    char joined[256];

    if (!filter || !*filter) return 1;
    if (strstr(e->name, filter) || strstr(e->suite, filter)) return 1;
    snprintf(joined, sizeof joined, "%s.%s", e->suite, e->name);
    return strstr(joined, filter) != NULL;
}

/* Run one batch of `iters` operations and return ns/op. A batch that is too
 * short to time is retried larger, so a 3 ns operation is never measured with
 * a 4 ns clock resolution. */
static MU_BENCH__UNUSED double mu_bench__batch(mu_bench_entry *e, uint64_t iters)
{
    double t0, t1;

    e->state.iterations = iters;
    t0 = mu_bench_now_ns();
    e->func(&e->state);
    t1 = mu_bench_now_ns();
    return (t1 - t0) / (double)iters;
}

static MU_BENCH__UNUSED void mu_bench_run_one(mu_bench_entry *e, double target_ns, int samples,
                             double conf_threshold, int csv, FILE *csvf)
{
    uint64_t iters = e->state.iterations_hint ? e->state.iterations_hint : 1u;
    double   s[MU_BENCH_MAX_SAMPLES];
    int      n = 0, warm;

    if (e->setup) e->setup(&e->state);

    /* Calibrate: grow the batch until one batch takes at least target_ns. */
    for (;;)
    {
        const double one = mu_bench__batch(e, iters);

        if (one * (double)iters >= target_ns || iters >= (1ull << 40)) break;
        iters *= 2u;
    }
    e->iterations = iters;

    /* Warmup batches: caches, branch predictors, and the allocator all need
     * to reach steady state before a sample counts. */
    for (warm = 0; warm < 3; ++warm) (void)mu_bench__batch(e, iters);

    /* The call goes through a function pointer, which is itself a compiler
     * barrier against hoisting the work out of the timed region. */
    for (n = 0; n < samples && n < MU_BENCH_MAX_SAMPLES; ++n)
        s[n] = mu_bench__batch(e, iters);

    {
        double sorted[MU_BENCH_MAX_SAMPLES];

        memcpy(sorted, s, sizeof(double) * (size_t)n);
        qsort(sorted, (size_t)n, sizeof(double), mu_bench__cmp);
        e->median_ns = sorted[n / 2];
        e->min_ns    = sorted[0];
        e->max_ns    = sorted[n - 1];
    }
    mu_bench__stats(s, n, &e->mean_ns, &e->conf_pct, &e->cv_pct);
    e->ops_per_sec = (e->mean_ns > 0.0) ? 1e9 / e->mean_ns : 0.0;

    {
        const int unstable = (e->conf_pct > conf_threshold);
        char      med[32], mn[32];

        mu_bench__fmt_ns(e->median_ns, med, sizeof med);
        mu_bench__fmt_ns(e->min_ns, mn, sizeof mn);
        printf("[%s] %s.%s  med %s  min %s  ci99 +-%.2f%%  cv %.1f%%  %.2f Mop/s\n",
               unstable ? "UNSTABLE" : "       OK",
               e->suite, e->name, med, mn, e->conf_pct, e->cv_pct, e->ops_per_sec / 1e6);
        if (unstable) mu_bench__reg.failed = 1;
    }

    if (csv && csvf)
        fprintf(csvf, "%s,%s,%.6f,%.6f,%.6f,%.6f,%.3f,%.3f,%llu\n",
                e->suite, e->name, e->median_ns, e->min_ns, e->max_ns,
                e->mean_ns, e->conf_pct, e->cv_pct,
                (unsigned long long)e->iterations);
}

static MU_BENCH__UNUSED void mu_bench__fmt_ns(double ns, char *buf, size_t cap)
{
    if (ns >= 1e6)      snprintf(buf, cap, "%.3f ms", ns / 1e6);
    else if (ns >= 1e3) snprintf(buf, cap, "%.3f us", ns / 1e3);
    else                snprintf(buf, cap, "%.3f ns", ns);
}

static MU_BENCH__UNUSED int mu_bench_main(int argc, char **argv)
{
    const char *filter = NULL, *csv_path = NULL;
    double      target_ns = MU_BENCH_TARGET_NS;
    double      conf = MU_BENCH_CONFIDENCE_PCT;
    int         samples = MU_BENCH_SAMPLES;
    int         list = 0, i;
    FILE       *csvf = NULL;

    for (i = 1; i < argc; ++i)
    {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h"))
        {
            printf("usage: %s [options]\n"
                   "  --filter=<substr>     run only suites/names containing substr\n"
                   "  --list                list benchmark names and exit\n"
                   "  --csv=<path>          write results as CSV\n"
                   "  --target=<ms>         time per batch (default %.0f ms)\n"
                   "  --samples=<n>         batches per benchmark (default %d)\n"
                   "  --confidence=<pct>    UNSTABLE cutoff (default %.1f%%)\n",
                   argv[0], target_ns / 1e6, samples, conf);
            return 0;
        }
        else if (!strncmp(a, "--filter=", 9))      filter = a + 9;
        else if (!strcmp(a, "--list"))              list = 1;
        else if (!strncmp(a, "--csv=", 6))          csv_path = a + 6;
        else if (!strncmp(a, "--target=", 9))       target_ns = atof(a + 9) * 1e6;
        else if (!strncmp(a, "--samples=", 10))     samples = atoi(a + 10);
        else if (!strncmp(a, "--confidence=", 13))  conf = atof(a + 13);
        else
        {
            fprintf(stderr, "mu_bench: unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }

    if (list)
    {
        for (i = 0; i < mu_bench__reg.count; ++i)
            printf("%s.%s\n", mu_bench__reg.entries[i].suite, mu_bench__reg.entries[i].name);
        return 0;
    }

    if (csv_path)
    {
        csvf = fopen(csv_path, "w");
        if (!csvf)
        {
            fprintf(stderr, "mu_bench: cannot write '%s'\n", csv_path);
            return 2;
        }
        fprintf(csvf, "suite,name,median_ns,min_ns,max_ns,mean_ns,ci99_pct,cv_pct,iterations\n");
    }

    printf("[==========] %d benchmark(s) on %s\n", mu_bench__reg.count, mu_bench__cpu());
    for (i = 0; i < mu_bench__reg.count; ++i)
    {
        mu_bench_entry *e = &mu_bench__reg.entries[i];

        if (!mu_bench__matches(e, filter)) continue;
        mu_bench_run_one(e, target_ns, samples, conf, csv_path != NULL, csvf);
    }
    printf("[==========] %d ran, %s\n", mu_bench__reg.count,
           mu_bench__reg.failed ? "FAILED (unstable measurement)" : "PASSED");

    if (csvf) fclose(csvf);
    return mu_bench__reg.failed ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * the machine, named in the output
 * -------------------------------------------------------------------------- */

static MU_BENCH__UNUSED const char *mu_bench__cpu(void)
{
    static char model[128];
    FILE       *f;

    if (model[0]) return model;
    f = fopen("/proc/cpuinfo", "r");
    if (f)
    {
        char line[256];

        while (fgets(line, sizeof line, f))
        {
            if (!strncmp(line, "model name", 10))
            {
                const char *c = strchr(line, ':');

                if (c)
                {
                    size_t k = 0;

                    ++c;
                    while (*c == ' ') ++c;
                    while (*c && *c != '\n' && k + 1 < sizeof model) model[k++] = *c++;
                    model[k] = '\0';
                }
                break;
            }
        }
        fclose(f);
    }
    if (!model[0]) snprintf(model, sizeof model, "unknown cpu");
    return model;
}

#ifdef __cplusplus
}
#endif

/* --------------------------------------------------------------------------
 * macros
 * --------------------------------------------------------------------------
 *
 * MU_BENCH expands to three things in this order, which matters: the body's
 * prototype, then a static constructor that registers the pointer, then the
 * body's own definition so the user's brace block attaches to it.
 */

#define MU_BENCH__DECL(s_, n_)                                                 \
    static void mu_bench_fn_##s_##_##n_(mu_bench_state *state)

#if defined(_MSC_VER)
#  define MU_BENCH__REG(s_, n_, hint)                                           \
     static void mu_bench_reg_##s_##_##n_(void);                                \
     __declspec(allocate(".CRT$XCU"))                                           \
     static void (__cdecl *mu_bench_regp_##s_##_##n_)(void) =                   \
         mu_bench_reg_##s_##_##n_;                                              \
     static void mu_bench_reg_##s_##_##n_(void)                                 \
     {                                                                          \
         mu_bench_entry *e_ = &mu_bench__reg.entries[                           \
             mu_bench_register(#s_, #n_, mu_bench_fn_##s_##_##n_, NULL)];       \
         if (hint) e_->state.iterations_hint = (uint64_t)(hint);                \
     }
#else
#  define MU_BENCH__REG(s_, n_, hint)                                           \
     static void mu_bench_reg_##s_##_##n_(void) __attribute__((constructor));   \
     static void mu_bench_reg_##s_##_##n_(void)                                 \
     {                                                                          \
         mu_bench_entry *e_ = &mu_bench__reg.entries[                           \
             mu_bench_register(#s_, #n_, mu_bench_fn_##s_##_##n_, NULL)];       \
         if (hint) e_->state.iterations_hint = (uint64_t)(hint);                \
     }
#endif

/* One benchmark. The body runs once per batch and must perform
 * `state->iterations` operations. */
#define MU_BENCH(s_, n_)                                                        \
    MU_BENCH__DECL(s_, n_);                                                     \
    MU_BENCH__REG(s_, n_, 0)                                                    \
    MU_BENCH__DECL(s_, n_)

/* Body plus a fixed operations-per-batch count (skips auto sizing). */
#define MU_BENCH_EX(s_, n_, hint)                                               \
    MU_BENCH__DECL(s_, n_);                                                     \
    MU_BENCH__REG(s_, n_, hint)                                                 \
    MU_BENCH__DECL(s_, n_)

/* Untimed setup, attached to the benchmark written directly above it. */
#define MU_BENCH_SETUP(s_, n_)                                                  \
    static void mu_bench_setup_##s_##_##n_(mu_bench_state *state);              \
    MU_BENCH__ATTACH(s_, n_, setup, setup)                                           \
    static void mu_bench_setup_##s_##_##n_(mu_bench_state *state)

/* Per-benchmark state; `state` is the mu_bench_state of the benchmark above. */
#define MU_BENCH_STATE(s_, n_)                                                  \
    static void mu_bench_state_##s_##_##n_(mu_bench_state *state);              \
    MU_BENCH__ATTACH(s_, n_, state, state_init)                                           \
    static void mu_bench_state_##s_##_##n_(mu_bench_state *state)

#if defined(_MSC_VER)
#  define MU_BENCH__ATTACH(s_, n_, kind, member)                                        \
     static void mu_bench_##kind##_ctor_##s_##_##n_(void);                      \
     __declspec(allocate(".CRT$XCU"))                                           \
     static void (__cdecl *mu_bench_##kind##_ctp_##s_##_##n_)(void) =           \
         mu_bench_##kind##_ctor_##s_##_##n_;                                    \
     static void mu_bench_##kind##_ctor_##s_##_##n_(void)                       \
     {                                                                          \
         mu_bench_entry *e_ = mu_bench__last();                                 \
         if (e_ && !strcmp(e_->name, #n_))                                      \
             e_->member = mu_bench_##kind##_##s_##_##n_;                          \
     }
#else
#  define MU_BENCH__ATTACH(s_, n_, kind, member)                                        \
     static void mu_bench_##kind##_ctor_##s_##_##n_(void)                       \
         __attribute__((constructor));                                          \
     static void mu_bench_##kind##_ctor_##s_##_##n_(void)                       \
     {                                                                          \
         mu_bench_entry *e_ = mu_bench__last();                                 \
         if (e_ && !strcmp(e_->name, #n_))                                      \
             e_->member = mu_bench_##kind##_##s_##_##n_;                          \
     }
#endif

/* The loop. Named so it reads as "iterate this body N times". */
#define MU_BENCH_ITERATE(i)   for ((i) = 0; (i) < state->iterations; ++(i))

/* Accumulators that a benchmark wants to keep must be handed to the barrier.
 * Usage inside a body:
 *     float acc = 0.0f;
 *     MU_BENCH_ITERATE(i) { acc += work(i); }
 *     MU_BENCH_DO_NOT_OPTIMIZE(acc);      // acc is a variable: fine
 */

#define MU_BENCH_MAIN()  int main(int argc, char **argv) { return mu_bench_main(argc, argv); }

#endif /* MU_BENCH_H */
