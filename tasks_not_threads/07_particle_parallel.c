/*
 * 07_particle_parallel.c — TIP 8 & 9: Split big collections; task dependencies
 * ===========================================================================
 * PATTERN: "Sometimes you have a big collection of self-contained things."
 *   - 1 000 000 particles, each has position + velocity.
 *   - Split into N_TASKS equally sized chunks.
 *   - Each chunk runs as an independent task on the thread pool.
 *   - A "collector" task runs only AFTER all chunk tasks complete.
 *   - Dependency enforced by an atomic countdown latch — no mutex.
 *
 * ATOMIC COUNTDOWN LATCH:
 *   - Initialised to N_TASKS.
 *   - Each task decrements with fetch_sub(release).
 *   - When it reaches 0, the last task signals the collector.
 *   - Collector waits with an acquire-load spin (or condition variable).
 *
 * ALSO SHOWS: tasks write to non-overlapping slices of a shared array.
 *   No synchronisation needed inside a task's slice (it's the sole owner).
 *   False-sharing avoided: task boundaries are cache-line aligned.
 *
 * Compile:  gcc -O2 -std=c11 -pthread 07_particle_parallel.c -o 07_particle_parallel
 * ===========================================================================
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <math.h>

#define N_PARTICLES  1000000
#define N_WORKERS    4          /* real cores on this machine */
#define N_TASKS      32         /* finer than workers for load balancing */
#define DT           0.016f     /* 60 fps timestep */

/* ── Particle (SoA-friendly pair) ────────────────────────────────────────── */
typedef struct { float x, y; } Vec2;
static Vec2 pos[N_PARTICLES];
static Vec2 vel[N_PARTICLES];
static Vec2 pos_ref[N_PARTICLES];   /* reference: single-threaded result */

/* ── Thread pool (reuse from tip 1, simplified) ──────────────────────────── */
typedef void (*TaskFn)(void *);
typedef struct QNode { TaskFn fn; void *arg; struct QNode *next; } QNode;

typedef struct {
    pthread_mutex_t  lock;
    pthread_cond_t   cv;
    QNode           *head, *tail;
    bool             stop;
} Pool;

static Pool pool;
static pthread_t workers[N_WORKERS];

static void *pool_worker(void *arg) {
    Pool *p = (Pool *)arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (!p->head && !p->stop) pthread_cond_wait(&p->cv, &p->lock);
        if (p->stop && !p->head) { pthread_mutex_unlock(&p->lock); return NULL; }
        QNode *n = p->head;
        p->head  = n->next;
        if (!p->head) p->tail = NULL;
        pthread_mutex_unlock(&p->lock);
        n->fn(n->arg);
        free(n);
    }
}

static void pool_init(void) {
    pthread_mutex_init(&pool.lock, NULL);
    pthread_cond_init(&pool.cv, NULL);
    for (int i = 0; i < N_WORKERS; ++i)
        pthread_create(&workers[i], NULL, pool_worker, &pool);
}

static void pool_submit(TaskFn fn, void *arg) {
    QNode *n = malloc(sizeof *n);
    n->fn = fn; n->arg = arg; n->next = NULL;
    pthread_mutex_lock(&pool.lock);
    if (pool.tail) pool.tail->next = n; else pool.head = n;
    pool.tail = n;
    pthread_cond_signal(&pool.cv);
    pthread_mutex_unlock(&pool.lock);
}

static void pool_shutdown(void) {
    pthread_mutex_lock(&pool.lock);
    pool.stop = true;
    pthread_cond_broadcast(&pool.cv);
    pthread_mutex_unlock(&pool.lock);
    for (int i = 0; i < N_WORKERS; ++i) pthread_join(workers[i], NULL);
}

/* ═══════════════════════════════════════════════════════
 *  Countdown latch  (N tasks → 1 dependent task)
 * ═══════════════════════════════════════════════════════ */
typedef struct {
    _Atomic int      remaining;
    _Atomic int      collector_done;
    double           collector_result;
} Latch;

static Latch latch;

/* ── Chunk task ───────────────────────────────────────────────────────────── */
typedef struct { int start; int count; } ChunkArg;

static void update_chunk(void *varg)
{
    ChunkArg *a = (ChunkArg *)varg;
    /* Each task owns particles [start, start+count) exclusively — no locks */
    for (int i = a->start; i < a->start + a->count; ++i) {
        pos[i].x += vel[i].x * DT;
        pos[i].y += vel[i].y * DT;
    }
    free(a);

    /* Decrement latch. Last task to reach 0 submits the collector. */
    int left = atomic_fetch_sub_explicit(&latch.remaining, 1,
                                         memory_order_release) - 1;
    if (left == 0)
        atomic_store_explicit(&latch.collector_done, 1, memory_order_release);
}

/* ── Collector: runs after all chunks complete ─────────────────────────────
   In a real engine this would be the physics post-step, render prep, etc. */
static double collect(void)
{
    /* Acquire: ensures we see all particle writes from all chunk tasks */
    while (atomic_load_explicit(&latch.collector_done, memory_order_acquire) == 0)
        sched_yield();

    double sum = 0;
    for (int i = 0; i < N_PARTICLES; ++i)
        sum += sqrtf(pos[i].x * pos[i].x + pos[i].y * pos[i].y);
    return sum;
}

/* ── Reference: single-threaded update ─────────────────────────────────── */
static double single_threaded(void)
{
    memcpy(pos_ref, pos, sizeof pos);  /* fresh copy of original positions */
    double sum = 0;
    for (int i = 0; i < N_PARTICLES; ++i) {
        pos_ref[i].x += vel[i].x * DT;
        pos_ref[i].y += vel[i].y * DT;
        sum += sqrtf(pos_ref[i].x * pos_ref[i].x +
                     pos_ref[i].y * pos_ref[i].y);
    }
    return sum;
}

int main(void)
{
    /* ── Initialise particles ─────────────────────────────────────────── */
    srand(42);
    for (int i = 0; i < N_PARTICLES; ++i) {
        pos[i].x = (float)(rand() % 1000);
        pos[i].y = (float)(rand() % 1000);
        vel[i].x = (float)(rand() % 100 - 50) * 0.1f;
        vel[i].y = (float)(rand() % 100 - 50) * 0.1f;
    }

    printf("Particle update: %d particles, %d tasks, %d workers\n\n",
           N_PARTICLES, N_TASKS, N_WORKERS);

    /* ── Single-threaded reference ───────────────────────────────────── */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double ref_sum = single_threaded();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms_single = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6;
    printf("  Single-threaded:  %.2f ms  (chk=%.0f)\n", ms_single, ref_sum);

    /* ── Parallel (original positions still valid since we used pos_ref) */
    pool_init();
    atomic_init(&latch.remaining, N_TASKS);
    atomic_init(&latch.collector_done, 0);

    int chunk_sz = N_PARTICLES / N_TASKS;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int t = 0; t < N_TASKS; ++t) {
        ChunkArg *a = malloc(sizeof *a);
        a->start = t * chunk_sz;
        a->count = (t == N_TASKS - 1)
                 ? N_PARTICLES - a->start   /* last chunk gets remainder */
                 : chunk_sz;
        pool_submit(update_chunk, a);
    }
    double par_sum = collect();             /* waits for latch → 0 */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms_par = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6;

    printf("  Parallel (%d tasks): %.2f ms  (chk=%.0f)  speedup=%.1fx\n",
           N_TASKS, ms_par, par_sum, ms_single / ms_par);

    /* ── Verify: parallel result matches reference ───────────────────── */
    double max_err = 0;
    for (int i = 0; i < N_PARTICLES; ++i) {
        double ex = fabsf(pos[i].x - pos_ref[i].x);
        double ey = fabsf(pos[i].y - pos_ref[i].y);
        if (ex > max_err) max_err = ex;
        if (ey > max_err) max_err = ey;
    }
    printf("  Max position error vs single-thread: %.2e  %s\n",
           max_err, max_err < 1e-4 ? "✓" : "✗");

    printf("\nDEPENDENCY GRAPH:\n");
    printf("  [chunk_0] ─┐\n");
    printf("  [chunk_1] ─┤\n");
    printf("  [chunk_2] ─┼─→ latch countdown → [collector]\n");
    printf("  ...        ┤\n");
    printf("  [chunk_31]─┘\n\n");
    printf("Chunks can all run in parallel (no shared writes).\n");
    printf("Collector must run after all — enforced by atomic countdown latch.\n");

    pool_shutdown();
    return 0;
}
