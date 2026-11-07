/*
 * sched_bench.c -- PART 8 benchmarks for mu_task_scheduler / mu_thread_system
 * ===========================================================================
 * Companion to ../../jobsystems.txt (PART 8). Four measurements, no opinions:
 *
 *   B1  empty-task throughput: submit + dispatch + complete, ns per task
 *   B2  allocation count per task: must reach 0 after the slot table lands
 *   B3  backpressure: a worker that submits more than the ring holds
 *   B4  parallel_for: wall time vs grain, 1M float elements
 *   B5  dependency chain: submit cost vs chain length P (the O(P^2) test)
 *
 * Every line of output is one CSV row:
 *
 *   B1,<workers>,<dispatch_limit>,<tasks>,<ns_per_task>
 *   B2,<tasks>,<allocs>,<frees>
 *   B3,<verdict>,<ms>
 *   B4,<workers>,<grain>,<ms>,<M elems/s>
 *   B5,<P>,<submit_ms>,<drain_ms>
 *
 * Build:  make jobs        (see Makefile)
 * Run:    ./sched_bench [b1|b2|b3|b4|b5|all]
 *
 * The allocator is redirected through MU_MALLOC/MU_FREE before the headers are
 * included, which is the documented override point (mu_macros.h) and the only
 * honest way to count what the library allocates.
 * ===========================================================================
 */
#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* ---- allocator redirection: must come before any mu header ---------------- */

static uint64_t g_allocs;
static uint64_t g_frees;

static void* bench_alloc(size_t sz)
{
    g_allocs++;
    return malloc(sz);
}

static void bench_free(void* p)
{
    if (p)
        g_frees++;
    free(p);
}

#define MU_MALLOC(sz) bench_alloc(sz)
#define MU_FREE(p)    bench_free(p)

#include "mu_perf.h"
#include "mu_task_scheduler.h"

/* ---- helpers -------------------------------------------------------------- */

static volatile uint64_t g_sink;

static void empty_task(void* user, uint32_t worker_index)
{
    (void)user;
    (void)worker_index;
    g_sink++;
}

/* mu_time_now() returns ticks of mu_time_freq() Hz; on POSIX that is ns. */
static double ns_between(uint64_t t0, uint64_t t1)
{
    return (double)(t1 - t0) * 1e9 / mu_time_freq();
}

static double ms_between(uint64_t t0, uint64_t t1)
{
    return (double)(t1 - t0) * 1e3 / mu_time_freq();
}

static bool sched_open(mu_task_scheduler** out, uint64_t workers, uint64_t dispatch_limit)
{
    mu_sched_init_desc desc = MU_SCHED_INIT_DESC_DEFAULT;
    desc.worker_count   = workers;
    desc.thread_name    = "sched_bench";
    desc.dispatch_limit = dispatch_limit;   /* 0 => default max(1, workers*2) */
    return mu_sched_init(out, &desc);
}

/* ---- B1 / B2 -------------------------------------------------------------- */

static void b1_run(uint64_t workers, uint64_t dispatch_limit, uint32_t tasks)
{
    mu_task_scheduler* s = NULL;
    if (!sched_open(&s, workers, dispatch_limit)) {
        printf("B1,%llu,%llu,INIT_FAILED\n", (unsigned long long)workers,
               (unsigned long long)dispatch_limit);
        return;
    }

    mu_sched_wait_idle(s);                  /* let the workers wake up        */

    uint64_t t0 = mu_time_now();
    for (uint32_t i = 0; i < tasks; ++i) {
        mu_sched_task_desc d;
        d.fn                 = empty_task;
        d.user               = NULL;
        d.priority           = MU_SCHED_PRIORITY_NORMAL;
        d.depends_on         = NULL;
        d.completion_counter = NULL;
        if (!mu_sched_submit(s, &d))
            break;
    }
    mu_sched_wait_idle(s);
    uint64_t t1 = mu_time_now();

    printf("B1,%llu,%llu,%u,%.1f\n",
           (unsigned long long)workers,
           (unsigned long long)dispatch_limit,
           tasks,
           ns_between(t0, t1) / (double)tasks);

    mu_sched_exit(&s);
}

static void b1_sweep(void)
{
    static const uint64_t worker_counts[] = { 1, 2, 4, 8 };

    for (size_t w = 0; w < sizeof worker_counts / sizeof worker_counts[0]; ++w)
        for (uint32_t dl = 0; dl <= 1; ++dl)
            b1_run(worker_counts[w], dl ? 65536u : 0u, 32768u);
}

static void b2_count(uint64_t workers, uint32_t tasks)
{
    mu_task_scheduler* s = NULL;
    if (!sched_open(&s, workers, 0)) {
        printf("B2,INIT_FAILED\n");
        return;
    }

    mu_sched_wait_idle(s);

    /* Everything above this line is setup; count only the submit/complete
       path for `tasks` empty tasks, which is what a frame costs. */
    uint64_t allocs0 = g_allocs;
    uint64_t frees0  = g_frees;

    for (uint32_t i = 0; i < tasks; ++i) {
        mu_sched_task_desc d;
        d.fn                 = empty_task;
        d.user               = NULL;
        d.priority           = MU_SCHED_PRIORITY_NORMAL;
        d.depends_on         = NULL;
        d.completion_counter = NULL;
        mu_sched_submit(s, &d);
    }
    mu_sched_wait_idle(s);

    printf("B2,%u,%llu,%llu\n", tasks,
           (unsigned long long)(g_allocs - allocs0),
           (unsigned long long)(g_frees - frees0));

    mu_sched_exit(&s);
}

/* ---- B3: worker submits more than the ring holds -------------------------- */

static mu_ts_handle g_ts = NULL;

static void ring_filler(void* user, uint64_t thread_id)
{
    (void)user;
    (void)thread_id;
    g_sink++;
}

static void flood_task(void* user, uint64_t thread_id)
{
    (void)user;
    (void)thread_id;
    /* Ring capacity is MU_TS_TASK_RING_CAPACITY (1024). This task is itself a
       worker, so it asks the pool to queue 2000 items -- more than the ring
       holds -- from inside the pool. Before H5 was fixed this blocked on
       condition_free forever; now a worker pushes what fits and drains a task
       inline to make room, so the run finishes in a few milliseconds. */
    mu_ts_add_tasks(g_ts, ring_filler, 2000, 0, NULL);
}

static void* watchdog(void* arg)
{
    (void)arg;
    sleep(5);
    fflush(stdout);
    fprintf(stderr, "B3,HANG,>5000\n");
    _exit(3);
    return NULL;
}

static void b3_backpressure(void)
{
    mu_ts_init_desc desc = MU_TS_INIT_DESC_DEFAULT;
    desc.thread_count = 2;
    desc.thread_name  = "b3";

    if (!mu_ts_init(&g_ts, &desc)) {
        printf("B3,INIT_FAILED,0\n");
        return;
    }

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    pthread_detach(wd);

    uint64_t t0 = mu_time_now();
    mu_ts_add_tasks(g_ts, flood_task, 2, 0, NULL);
    mu_ts_wait_idle(g_ts);
    uint64_t t1 = mu_time_now();

    printf("B3,OK,%.1f\n", ms_between(t0, t1));

    mu_ts_exit_desc ed = MU_TS_EXIT_DESC_DEFAULT;
    mu_ts_exit(&g_ts, &ed);
}

/* ---- B4: parallel_for grain sweep ---------------------------------------- */

#define PF_ELEMS (1024u * 1024u)
static float g_floats[PF_ELEMS];

static void pf_leaf(uint32_t begin, uint32_t end, uint32_t worker_index, void* user)
{
    (void)worker_index;
    (void)user;
    float* p = g_floats;
    for (uint32_t i = begin; i < end; ++i)
        p[i] = p[i] * 1.000001f + 1.0f;
}

static void b4_grain(uint64_t workers)
{
    static const uint32_t grains[] = { 8, 32, 128, 1024 };

    mu_task_scheduler* s = NULL;
    if (!sched_open(&s, workers, 0)) {
        printf("B4,INIT_FAILED\n");
        return;
    }

    for (size_t g = 0; g < sizeof grains / sizeof grains[0]; ++g) {
        /* warm up, then best of 3 */
        mu_sched_parallel_for(s, 0, PF_ELEMS, grains[g], MU_SCHED_PRIORITY_NORMAL,
                              pf_leaf, NULL);
        double best = 1e30;
        for (int rep = 0; rep < 3; ++rep) {
            uint64_t t0 = mu_time_now();
            mu_sched_parallel_for(s, 0, PF_ELEMS, grains[g], MU_SCHED_PRIORITY_NORMAL,
                                  pf_leaf, NULL);
            uint64_t t1 = mu_time_now();
            double ms = ms_between(t0, t1);
            if (ms < best)
                best = ms;
        }
        printf("B4,%llu,%u,%.2f,%.1f\n",
               (unsigned long long)workers, grains[g], best,
               (double)PF_ELEMS / (best * 1e3));
    }

    mu_sched_exit(&s);
}

/* ---- B5: dependency chain ------------------------------------------------ */

#define B5_MAX 4000
static mu_sched_counter* g_chain;
static uint32_t          g_chain_len;
static uint32_t*         g_chain_hits;

static void chain_task(void* user, uint32_t worker_index)
{
    (void)worker_index;
    uint32_t i = (uint32_t)(uintptr_t)user;
    g_chain_hits[i]++;
}

static void b5_chain(uint32_t p)
{
    mu_task_scheduler* s = NULL;
    if (!sched_open(&s, 4, 0)) {
        printf("B5,INIT_FAILED\n");
        return;
    }

    g_chain      = (mu_sched_counter*)calloc(p + 1u, sizeof *g_chain);
    g_chain_hits = (uint32_t*)calloc(p, sizeof *g_chain_hits);
    g_chain_len  = p;

    for (uint32_t i = 0; i <= p; ++i)
        mu_sched_counter_init(&g_chain[i], 0);

    /* Chain construction, and the reason the submit order is ascending:
         task i : depends_on = c[i], completion_counter = c[i+1]
       Submitting task i bumps c[i+1] to 1 *before* task i+1 is submitted, so
       task i+1 really is "waiting". Task 0 depends on c[0], which is 0, so it
       is runnable immediately; when it finishes it drops c[1] to 0, which
       promotes task 1, and so on. (Do NOT also pre-add to c[i]: submit
       already increments the completion counter, and the extra +1 makes the
       chain never reach zero -- a two-line way to deadlock the scheduler.) */

    uint64_t t0 = mu_time_now();
    for (uint32_t i = 0; i < p; ++i) {
        mu_sched_task_desc d;
        d.fn                 = chain_task;
        d.user               = (void*)(uintptr_t)i;
        d.priority           = MU_SCHED_PRIORITY_NORMAL;
        d.depends_on         = &g_chain[i];
        d.completion_counter = &g_chain[i + 1u];
        if (!mu_sched_submit(s, &d))
            break;
    }
    uint64_t t1 = mu_time_now();
    mu_sched_wait_idle(s);
    uint64_t t2 = mu_time_now();

    bool ordered = true;
    for (uint32_t i = 0; i < p; ++i)
        if (g_chain_hits[i] != 1u)
            ordered = false;

    printf("B5,%u,%.2f,%.2f,%s\n", p,
           ms_between(t0, t1),
           ms_between(t1, t2),
           ordered ? "ordered" : "BROKEN");

    for (uint32_t i = 0; i <= p; ++i)
        mu_sched_counter_exit(&g_chain[i]);
    free(g_chain);
    free(g_chain_hits);
    mu_sched_exit(&s);
}

/* ---- B5b: wide fan-in -- P tasks waiting on one gate ---------------------- */

static mu_sched_counter* g_gate;

static void wide_task(void* user, uint32_t worker_index)
{
    (void)worker_index;
    uint32_t i = (uint32_t)(uintptr_t)user;
    g_chain_hits[i]++;
}

static void gate_release(void* user, uint32_t worker_index)
{
    (void)user;
    (void)worker_index;
    mu_sched_counter_sub(g_gate, 1);
}

static void b5_wide(uint32_t p)
{
    mu_task_scheduler* s = NULL;
    if (!sched_open(&s, 4, 0)) {
        printf("B5W,INIT_FAILED\n");
        return;
    }

    g_chain_hits = (uint32_t*)calloc(p, sizeof *g_chain_hits);

    /* One gate, held closed: every task goes to the pending list and stays
       there until the release task runs. This is the worst case for a
       scheduler that resolves dependencies by rescanning the pending list --
       and it is what a "wait for all of phase 1" join actually produces. */
    mu_sched_counter gate;
    mu_sched_counter_init(&gate, 1);
    g_gate = &gate;

    uint64_t t0 = mu_time_now();
    for (uint32_t i = 0; i < p; ++i) {
        mu_sched_task_desc d;
        d.fn                 = wide_task;
        d.user               = (void*)(uintptr_t)i;
        d.priority           = MU_SCHED_PRIORITY_NORMAL;
        d.depends_on         = &gate;
        d.completion_counter = NULL;
        if (!mu_sched_submit(s, &d))
            break;
    }
    uint64_t t1 = mu_time_now();

    mu_sched_task_desc r;
    r.fn                 = gate_release;
    r.user               = NULL;
    r.priority           = MU_SCHED_PRIORITY_NORMAL;
    r.depends_on         = NULL;
    r.completion_counter = NULL;
    mu_sched_submit(s, &r);

    mu_sched_wait_idle(s);
    uint64_t t2 = mu_time_now();

    bool all_ran = true;
    for (uint32_t i = 0; i < p; ++i)
        if (g_chain_hits[i] != 1u)
            all_ran = false;

    printf("B5W,%u,%.2f,%.2f,%.2f,%s\n", p,
           ms_between(t0, t1), ms_between(t1, t2),
           ns_between(t0, t1) / (double)p / 1000.0,
           all_ran ? "ran" : "LOST");

    mu_sched_counter_exit(&gate);
    free(g_chain_hits);
    mu_sched_exit(&s);
}

/* ---- B6: dependency written outside a task completion ---------------------- */

static void b6_external_completion(void)
{
    mu_task_scheduler* s = NULL;
    if (!sched_open(&s, 2, 0)) {
        printf("B6,INIT_FAILED\n");
        return;
    }

    static uint32_t hits;
    hits = 0;
    g_chain_hits = &hits;

    mu_sched_counter gate;
    mu_sched_counter_init(&gate, 1);
    g_gate = &gate;

    mu_sched_task_desc d;
    d.fn                 = wide_task;
    d.user               = (void*)(uintptr_t)0;
    d.priority           = MU_SCHED_PRIORITY_NORMAL;
    d.depends_on         = &gate;
    d.completion_counter = NULL;
    mu_sched_submit(s, &d);

    /* The main thread completes the dependency directly -- the "IO finished" /
       "streaming callback" case. Before H18 was fixed this published zero and
       woke counter waiters but left the fan-out task pending until some
       unrelated task finished, which also hung mu_sched_wait_idle/exit. The
       counter now promotes its dependents at the point where it reaches zero. */
    mu_sched_counter_sub(&gate, 1);

    /* 200 ms is an eternity for two idle workers; if it has not run, the
       counter write alone did not promote it. */
    for (int i = 0; i < 20 && hits == 0; ++i) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    if (hits == 0) {
        /* mu_sched_assist() calls promote before it pops, so one assist is the
           escape hatch. Note mu_sched_wait_idle() does NOT promote -- it only
           waits on condition_idle -- so a stranded fan-out makes wait_idle and
           mu_sched_exit() block forever. That is why this branch never calls
           them before proving the task can be released. */
        mu_sched_assist(s);
        printf("B6,%s,%u\n", hits ? "STRANDED_UNTIL_ASSIST" : "STRANDED_HARD", hits);
        if (!hits) { fflush(stdout); _exit(4); }
    } else {
        printf("B6,ran,%u\n", hits);
    }

    g_chain_hits = NULL;
    mu_sched_counter_exit(&gate);
    mu_sched_exit(&s);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char** argv)
{
    const char* what = (argc > 1) ? argv[1] : "all";

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("# sched_bench cores=%ld build=%s %s\n", cores, __DATE__, __TIME__);

    if (!strcmp(what, "b1") || !strcmp(what, "all"))
        b1_sweep();
    if (!strcmp(what, "b2") || !strcmp(what, "all"))
        b2_count(4, 10000);
    if (!strcmp(what, "b4") || !strcmp(what, "all"))
        for (uint64_t w = 1; w <= 8; w *= 2)
            b4_grain(w);
    if (!strcmp(what, "b5") || !strcmp(what, "all")) {
        static const uint32_t lens[] = { 1, 10, 100, 1000, 4000 };
        for (size_t i = 0; i < sizeof lens / sizeof lens[0]; ++i)
            b5_chain(lens[i]);
        for (size_t i = 0; i < sizeof lens / sizeof lens[0]; ++i)
            b5_wide(lens[i]);
    }
    if (!strcmp(what, "b6") || !strcmp(what, "all"))
        b6_external_completion();

    /* b3 is last only because it is the one bench that can legitimately kill
       the process: its watchdog turns a regression into a 5 s exit(3) instead
       of a hung test suite. It is a pass/fail guard, not a timing measurement. */
    if (!strcmp(what, "b3") || !strcmp(what, "all"))
        b3_backpressure();

    return 0;
}
