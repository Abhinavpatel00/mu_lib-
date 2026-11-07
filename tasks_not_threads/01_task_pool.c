/*
 * 01_task_pool.c — TIP 1 & 2: Use tasks not threads; one worker per real core
 * ===========================================================================
 * A minimal but complete thread pool.
 *
 * KEY IDEAS from Herb Sutter + tips:
 *  - Threads are expensive OS resources (stack, kernel TLS, scheduler slot).
 *    Create them ONCE at startup; keep them alive waiting for tasks.
 *  - Tasks are cheap: a function pointer + a void* argument.
 *  - One worker per PHYSICAL core.  On this machine: 4 cores / 8 HT threads.
 *    Hyperthreads share an ALU; two HT threads don't run truly in parallel.
 *  - The pool uses a mutex + condition variable for the work queue.
 *    This is correct and fast: threads block in the kernel (no spinning) while
 *    idle, waking instantly when work arrives.
 *
 * WHAT THIS PROVES:
 *  Creating 100 000 OS threads would OOM or at least thrash the scheduler.
 *  Submitting 100 000 tasks to 4 workers processes them all efficiently.
 *
 * Compile:  gcc -O2 -std=c11 -pthread 01_task_pool.c -o 01_task_pool
 * Run:      ./01_task_pool
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
#include <time.h>
#include <unistd.h>

/* ── How many real cores? ────────────────────────────────────────────────── */
static int real_core_count(void)
{
#ifdef __linux__
    // Read thread_siblings_list to count logical CPUs sharing one physical core.
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "r");
    if (f) {
        int siblings = 0, n;
        while (fscanf(f, "%d%*[-,]", &n) == 1) siblings++;
        fclose(f);
        if (siblings > 0) return (int)sysconf(_SC_NPROCESSORS_ONLN) / siblings;
    }
#endif
    return (int)sysconf(_SC_NPROCESSORS_ONLN); /* fallback: logical count */
}

/* ═══════════════════════════════════════════════════════
 *  Task type
 * ═══════════════════════════════════════════════════════ */
typedef void (*task_fn)(void *arg);

typedef struct Task {
    task_fn     fn;
    void       *arg;
    struct Task *next;  /* intrusive linked list */
} Task;

/* ═══════════════════════════════════════════════════════
 *  Thread pool
 * ═══════════════════════════════════════════════════════ */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  work_ready;
    Task           *head;           /* FIFO queue: push to tail, pop from head */
    Task           *tail;
    pthread_t      *threads;
    int             n_threads;
    bool            shutdown;
    _Atomic int     pending;        /* tasks submitted but not yet finished     */
} ThreadPool;

static void pool_push(ThreadPool *pool, Task *t)
{
    /* called with lock held */
    t->next = NULL;
    if (pool->tail) pool->tail->next = t;
    else            pool->head = t;
    pool->tail = t;
}

static Task *pool_pop(ThreadPool *pool)
{
    /* called with lock held */
    Task *t = pool->head;
    if (t) {
        pool->head = t->next;
        if (!pool->head) pool->tail = NULL;
    }
    return t;
}

static void *worker_fn(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (!pool->head && !pool->shutdown)
            pthread_cond_wait(&pool->work_ready, &pool->lock);

        if (pool->shutdown && !pool->head) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }

        Task *t = pool_pop(pool);
        pthread_mutex_unlock(&pool->lock);

        if (t) {
            t->fn(t->arg);
            free(t);
            /* signal completion */
            atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_release);
        }
    }
}

static ThreadPool *pool_create(int n_threads)
{
    ThreadPool *pool = calloc(1, sizeof *pool);
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->work_ready, NULL);
    pool->n_threads = n_threads;
    pool->threads   = malloc((size_t)n_threads * sizeof(pthread_t));

    printf("[pool] spawning %d worker threads\n", n_threads);
    for (int i = 0; i < n_threads; ++i)
        pthread_create(&pool->threads[i], NULL, worker_fn, pool);

    return pool;
}

/* Submit a task — fn(arg) will be called on a worker thread */
static void pool_submit(ThreadPool *pool, task_fn fn, void *arg)
{
    Task *t   = malloc(sizeof *t);
    t->fn     = fn;
    t->arg    = arg;
    t->next   = NULL;

    atomic_fetch_add_explicit(&pool->pending, 1, memory_order_relaxed);

    pthread_mutex_lock(&pool->lock);
    pool_push(pool, t);
    pthread_cond_signal(&pool->work_ready);
    pthread_mutex_unlock(&pool->lock);
}

/* Wait for all submitted tasks to finish */
static void pool_wait(ThreadPool *pool)
{
    /* spin-wait: fine for a demo; real code uses a condition variable */
    while (atomic_load_explicit(&pool->pending, memory_order_acquire) > 0)
        sched_yield();
}

static void pool_destroy(ThreadPool *pool)
{
    pool_wait(pool);
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->work_ready);
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < pool->n_threads; ++i)
        pthread_join(pool->threads[i], NULL);
    free(pool->threads);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->work_ready);
    free(pool);
}

/* ═══════════════════════════════════════════════════════
 *  Demo: submit 100 000 tasks to a 4-worker pool
 * ═══════════════════════════════════════════════════════ */
static _Atomic int64_t grand_total;

static void sum_task(void *arg)
{
    int64_t *chunk = (int64_t *)arg;
    int64_t  n     = chunk[0];     /* how many numbers */
    int64_t  start = chunk[1];     /* starting value   */
    int64_t  local = 0;
    for (int64_t i = start; i < start + n; ++i) local += i;
    atomic_fetch_add_explicit(&grand_total, local, memory_order_relaxed);
    free(chunk);
}

int main(void)
{
    int real_cores = real_core_count();
    printf("Logical CPUs: %d   Physical cores (workers): %d\n",
           (int)sysconf(_SC_NPROCESSORS_ONLN), real_cores);

    ThreadPool *pool = pool_create(real_cores);

    /* Submit 10 000 tasks, each summing 1 000 consecutive integers */
    int n_tasks = 10000;
    int chunk   = 1000;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < n_tasks; ++i) {
        int64_t *arg = malloc(2 * sizeof(int64_t));
        arg[0] = chunk;
        arg[1] = (int64_t)i * chunk;
        pool_submit(pool, sum_task, arg);
    }
    pool_wait(pool);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec)*1e3 + (t1.tv_nsec - t0.tv_nsec)*1e-6;

    int64_t N = (int64_t)n_tasks * chunk;
    int64_t expected = N * (N - 1) / 2;
    printf("Sum of 0..%lld = %lld  (expected %lld)  %s\n",
           (long long)(N-1),
           (long long)atomic_load(&grand_total),
           (long long)expected,
           atomic_load(&grand_total) == expected ? "✓" : "✗ WRONG");
    printf("Processed %d tasks in %.2f ms using %d workers\n",
           n_tasks, ms, real_cores);

    pool_destroy(pool);
    return 0;
}
