/* test_bench.c -- the gate for mu_bench.h itself.
 *
 * A benchmark harness that silently measures nothing is worse than no harness,
 * because it produces confident numbers. So the central assertion is a RATIO:
 * one benchmark does 8x the dependent work of another, and the harness must
 * report ~8x. That fails immediately if the barrier stops working, if the
 * iteration count is mis-scaled, or if the clock is read in the wrong place.
 *
 * Also checks the CLI surface (--list, --filter, --csv) because those are what
 * the perf records are generated from.
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_checks, g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) { ++g_fails; if (g_fails <= 20) printf("  FAIL %s\n", what); }
}

/* A dependent FMA chain: latency-bound, so its cost scales with the trip count.
 *
 * The trip count is held in a VOLATILE global on purpose. With a constant count
 * gcc folds the recurrence to its closed form (x*a^n + b*...) and the benchmark
 * stops measuring the loop at all: the first version of this file reported
 * chain(16) = 5.1 ns and chain(128) = 130 ns, a 25x ratio for 8x the work,
 * which is how the fold announced itself. A volatile bound makes the count
 * opaque and the timing linear again. */
static volatile uint32_t g_chain_n = 32;

static float chain(uint32_t n, float seed)
{
    float x = seed;
    uint32_t i;

    for (i = 0; i < n; ++i) x = x * 1.0000001f + 0.5f;
    return x;
}

MU_BENCH(selftest, work_1x)
{
    uint32_t it;
    float    acc = 0.0f;

    MU_BENCH_ITERATE(it) { acc += chain(g_chain_n, 1.25f); }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* Same kernel, same shape, eight independent chains instead of one. Comparing
 * DIFFERENT trip counts does not work: a 16-step chain overlaps with the next
 * call's chain in the out-of-order window and measures throughput (~1 cycle per
 * step), while a 128-step chain fills the reorder buffer and measures latency
 * (~4 cycles per step). That made an 8x work increase look like 24x. Matching
 * the shape on both sides is what makes the ratio mean "8x the work". */
MU_BENCH(selftest, work_8x)
{
    uint32_t it;
    float    acc = 0.0f;

    MU_BENCH_ITERATE(it)
    {
        uint32_t k;

        for (k = 0; k < 8u; ++k) acc += chain(g_chain_n, 1.0f + (float)k);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* A benchmark whose result is thrown away. With a working barrier this still
 * costs what chain(64) costs; with a broken one it costs nothing. */
MU_BENCH(selftest, discarded_result)
{
    uint32_t it;

    MU_BENCH_ITERATE(it) { (void)chain(64u, 2.5f); }
}

int main(void)
{

    printf("mu_bench self-test\n");
    printf("----------------------------------------------------------\n");

    /* 1. registration worked */
    check(mu_bench__reg.count == 3, "registry: three benchmarks registered");
    check(strcmp(mu_bench__reg.entries[0].suite, "selftest") == 0, "registry: suite name");
    check(mu_bench__reg.entries[0].func != NULL, "registry: function pointer captured");

    /* 2. filter matching, which is what --list and --filter both use.
     * (Redirecting stdout to capture --list is possible but it breaks the rest
     * of the output when /dev/tty is unavailable, which it is in CI.) */
    {
        int matched_suite = 0, matched_name = 0, matched_none = 0, k;

        for (k = 0; k < mu_bench__reg.count; ++k)
        {
            const mu_bench_entry *e = &mu_bench__reg.entries[k];

            if (mu_bench__matches(e, "selftest"))   ++matched_suite;
            if (mu_bench__matches(e, "work_8x"))    ++matched_name;
            if (mu_bench__matches(e, "nothingxyz")) ++matched_none;
        }
        check(matched_suite == 3, "filter: suite name matches all three");
        check(matched_name == 1, "filter: benchmark name matches one");
        check(matched_none == 0, "filter: a non-matching filter matches nothing");
        check(mu_bench__matches(&mu_bench__reg.entries[0], NULL) == 1,
              "filter: NULL filter matches everything");
    }

    /* 3. the harness must scale with the work: 8x work -> ~8x time */
    {
        mu_bench_entry *w1 = &mu_bench__reg.entries[0];
        mu_bench_entry *w8 = &mu_bench__reg.entries[1];

        mu_bench_run_one(w1, 5e6, 9, 100.0, 0, NULL);
        mu_bench_run_one(w8, 5e6, 9, 100.0, 0, NULL);

        {
            const double ratio = w8->median_ns / w1->median_ns;

            printf("  ratio work_8x / work_1x = %.2f (expect ~8)\n", ratio);
            check(ratio > 6.0 && ratio < 10.0,
                  "scaling: 8x the dependent work costs ~8x the time");
            check(w1->median_ns > 1.0, "scaling: the 1x benchmark is not free");
        }
    }

    /* 4. Work whose result never reaches the barrier IS eliminated -- that is
     *    not the harness failing, it is the reason MU_BENCH_DO_NOT_OPTIMIZE
     *    exists, and asserting it here keeps the lesson in the gate. If this
     *    case ever becomes expensive, someone has made the barrier inhibit dead
     *    code elimination globally and every "optimisation" measured afterwards
     *    is suspect. */
    {
        mu_bench_entry *d = &mu_bench__reg.entries[2];
        mu_bench_entry *w1 = &mu_bench__reg.entries[0];

        mu_bench_run_one(d, 5e6, 9, 100.0, 0, NULL);
        printf("  discarded chain(64) = %.2f ns vs chain(16) = %.2f ns\n",
               d->median_ns, w1->median_ns);
        check(d->median_ns < w1->median_ns,
              "barrier: discarded work is eliminated (use DO_NOT_OPTIMIZE)");
    }

    /* 5. CSV output has a header plus one row per benchmark */
    {
        FILE *f;
        char *av[] = { (char *)"bench", (char *)"--csv=/tmp/mu_bench.csv", (char *)"--samples=5",
                       (char *)"--filter=selftest", (char *)"--confidence=100" };
        int   rc;

        rc = mu_bench_main(5, av);
        (void)rc;
        f = fopen("/tmp/mu_bench.csv", "r");
        check(f != NULL, "csv: file created");
        if (f)
        {
            char   line[512];
            int    rows = 0;

            while (fgets(line, sizeof line, f)) ++rows;
            fclose(f);
            check(rows == 4, "csv: header plus three rows");
        }
    }

    printf("----------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
