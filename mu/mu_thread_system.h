#ifndef MU_THREAD_SYSTEM_H
#define MU_THREAD_SYSTEM_H

/*
    mu_thread_system.h
    ---------------------------------------------------------------------------
    Standalone C99 single-header thread system + atomics helpers.

    This header merges ideas from:
    - The-Forge/Common_3/Utilities/Threading/ThreadSystem.h/.c
    - The-Forge/Common_3/Utilities/Threading/Atomics.h

    Design goals:
    1) Keep the API close to The Forge thread system behavior.
    2) Keep it header-only and dependency-light.
    3) Preserve "dummy mode" when thread_count == 0.

    VISUAL MODEL

      Producers call mu_ts_add_tasks()
                     |
                     v
            +-------------------+
            | FIFO task buffer  |
            +-------------------+
                     |
                     v
         worker 1  worker 2  ... worker N
             |         |              |
             +---- execute TaskFunc --+

    TaskFunc signature:
      void (*fn)(void* user, uint64_t thread_id)

    thread_id:
    - 1..N for worker threads
    - UINT64_MAX when executed by assist() from a non-worker/dummy mode
    - the calling worker's own id when a worker assists while waiting

    A worker that blocks on a scheduler wait must keep making progress for the
    tasks it is waiting on. `mu_ts_is_worker_thread()` lets upper layers detect
    that case and assist instead of parking the worker.

    ---------------------------------------------------------------------------
*/

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

#include "mu_macros.h"

/* ------------------------------------------------------------------------- */
/* Atomics -- owned by mu/mu_atomic.h (single implementation, see that file).  */
/* This header keeps source-compatible mu_ts_* aliases only. No new primitives. */
/* ------------------------------------------------------------------------- */

#include "mu_atomic.h"

typedef mu_atomic32_t  mu_ts_atomic32_t;
typedef mu_atomic64_t  mu_ts_atomic64_t;
typedef mu_atomicptr_t mu_ts_atomicptr_t;

/* ----- compat shims: same names/signatures, canonical backend ----- */

MU_INLINE void mu_ts_memorybarrier_acquire(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : : "memory");
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
}
MU_INLINE void mu_ts_memorybarrier_release(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : : "memory");
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
}

MU_INLINE uint32_t mu_ts_atomic32_load_relaxed(mu_ts_atomic32_t* p)
{
    return mu_atomic32_load_relaxed(p);
}
MU_INLINE uint32_t mu_ts_atomic32_store_relaxed(mu_ts_atomic32_t* p, uint32_t v)
{
    return mu_atomic32_exchange(p, v);
}
MU_INLINE uint32_t mu_ts_atomic32_add_relaxed(mu_ts_atomic32_t* p, int32_t v)
{
    return mu_atomic32_fetch_add(p, (uint32_t)v);
}
MU_INLINE uint32_t mu_ts_atomic32_cas_relaxed(mu_ts_atomic32_t* p, uint32_t cmp, uint32_t v)
{
    uint32_t expected = cmp;
    (void)mu_atomic32_compare_exchange(p, &expected, v);
    return expected;
}

MU_INLINE uint64_t mu_ts_atomic64_load_relaxed(mu_ts_atomic64_t* p)
{
    return mu_atomic64_load_relaxed(p);
}
MU_INLINE uint64_t mu_ts_atomic64_store_relaxed(mu_ts_atomic64_t* p, uint64_t v)
{
    return mu_atomic64_exchange(p, v);
}
MU_INLINE uint64_t mu_ts_atomic64_add_relaxed(mu_ts_atomic64_t* p, int64_t v)
{
    return mu_atomic64_fetch_add(p, (uint64_t)v);
}
MU_INLINE uint64_t mu_ts_atomic64_cas_relaxed(mu_ts_atomic64_t* p, uint64_t cmp, uint64_t v)
{
    uint64_t expected = cmp;
    (void)mu_atomic64_compare_exchange(p, &expected, v);
    return expected;
}

MU_INLINE uint32_t mu_ts_atomic32_load_acquire(mu_ts_atomic32_t* p)
{
    return mu_atomic32_load_acquire(p);
}
MU_INLINE uint32_t mu_ts_atomic32_store_release(mu_ts_atomic32_t* p, uint32_t v)
{
    uint32_t prev = mu_atomic32_exchange(p, v);
   (void)prev;
    /* exchange is already ACQ_REL; keep old void-ish call shape working */
    return prev;
}
MU_INLINE uint64_t mu_ts_atomic64_load_acquire(mu_ts_atomic64_t* p)
{
    return mu_atomic64_load_acquire(p);
}
MU_INLINE uint64_t mu_ts_atomic64_store_release(mu_ts_atomic64_t* p, uint64_t v)
{
    uint64_t prev = mu_atomic64_exchange(p, v);
    (void)prev;
    return prev;
}

MU_INLINE uint32_t mu_ts_atomic32_max_relaxed(mu_ts_atomic32_t* dst, uint32_t val)
{
    return mu_atomic32_fetch_max(dst, val);
}
MU_INLINE uint64_t mu_ts_atomic64_max_relaxed(mu_ts_atomic64_t* dst, uint64_t val)
{
    return mu_atomic64_fetch_max(dst, val);
}

/* ------------------------------------------------------------------------- */
/* Platform sync/thread wrappers (public so other single headers can reuse)  */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32)

typedef HANDLE             mu_ts_thread_handle;
typedef CRITICAL_SECTION   mu_ts_mutex;
typedef CONDITION_VARIABLE mu_ts_cond;

typedef DWORD(WINAPI* mu_ts_thread_entry)(LPVOID);

MU_INLINE bool mu_ts_mutex_init(mu_ts_mutex* m)
{
    InitializeCriticalSection(m);
    return true;
}
MU_INLINE void mu_ts_mutex_exit(mu_ts_mutex* m)
{
    DeleteCriticalSection(m);
}
MU_INLINE void mu_ts_mutex_lock(mu_ts_mutex* m)
{
    EnterCriticalSection(m);
}
MU_INLINE void mu_ts_mutex_unlock(mu_ts_mutex* m)
{
    LeaveCriticalSection(m);
}

MU_INLINE bool mu_ts_cond_init(mu_ts_cond* c)
{
    InitializeConditionVariable(c);
    return true;
}
MU_INLINE void mu_ts_cond_exit(mu_ts_cond* c)
{
    (void)c;
}
MU_INLINE void mu_ts_cond_wake_one(mu_ts_cond* c)
{
    WakeConditionVariable(c);
}
MU_INLINE void mu_ts_cond_wake_all(mu_ts_cond* c)
{
    WakeAllConditionVariable(c);
}
MU_INLINE bool mu_ts_cond_wait(mu_ts_cond* c, mu_ts_mutex* m, uint32_t timeout_ms)
{
    DWORD t = (timeout_ms == UINT32_MAX) ? INFINITE : timeout_ms;
    return SleepConditionVariableCS(c, m, t) != 0;
}

MU_INLINE bool mu_ts_thread_start(mu_ts_thread_handle* out, mu_ts_thread_entry fn, void* user)
{
    HANDLE h = CreateThread(NULL, 0, fn, user, 0, NULL);
    if(!h)
        return false;
    *out = h;
    return true;
}
MU_INLINE void mu_ts_thread_join(mu_ts_thread_handle h)
{
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}
MU_INLINE void mu_ts_thread_detach(mu_ts_thread_handle h)
{
    CloseHandle(h);
}
MU_INLINE uint64_t mu_ts_cpu_count(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (uint64_t)info.dwNumberOfProcessors;
}

#else

typedef pthread_t       mu_ts_thread_handle;
typedef pthread_mutex_t mu_ts_mutex;
typedef pthread_cond_t  mu_ts_cond;

typedef void* (*mu_ts_thread_entry)(void*);

MU_INLINE bool mu_ts_mutex_init(mu_ts_mutex* m)
{
    return pthread_mutex_init(m, NULL) == 0;
}
MU_INLINE void mu_ts_mutex_exit(mu_ts_mutex* m)
{
    pthread_mutex_destroy(m);
}
MU_INLINE void mu_ts_mutex_lock(mu_ts_mutex* m)
{
    pthread_mutex_lock(m);
}
MU_INLINE void mu_ts_mutex_unlock(mu_ts_mutex* m)
{
    pthread_mutex_unlock(m);
}

MU_INLINE bool mu_ts_cond_init(mu_ts_cond* c)
{
    return pthread_cond_init(c, NULL) == 0;
}
MU_INLINE void mu_ts_cond_exit(mu_ts_cond* c)
{
    pthread_cond_destroy(c);
}
MU_INLINE void mu_ts_cond_wake_one(mu_ts_cond* c)
{
    pthread_cond_signal(c);
}
MU_INLINE void mu_ts_cond_wake_all(mu_ts_cond* c)
{
    pthread_cond_broadcast(c);
}

MU_INLINE bool mu_ts_cond_wait(mu_ts_cond* c, mu_ts_mutex* m, uint32_t timeout_ms)
{
    if(timeout_ms == UINT32_MAX)
    {
        return pthread_cond_wait(c, m) == 0;
    }

    struct timespec ts;
#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.tv_sec  = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000L;
#endif

    ts.tv_sec += (time_t)(timeout_ms / 1000U);
    ts.tv_nsec += (long)((timeout_ms % 1000U) * 1000000UL);
    if(ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    return pthread_cond_timedwait(c, m, &ts) == 0;
}

MU_INLINE bool mu_ts_thread_start(mu_ts_thread_handle* out, mu_ts_thread_entry fn, void* user)
{
    return pthread_create(out, NULL, fn, user) == 0;
}
MU_INLINE void mu_ts_thread_join(mu_ts_thread_handle h)
{
    pthread_join(h, NULL);
}
MU_INLINE void mu_ts_thread_detach(mu_ts_thread_handle h)
{
    pthread_detach(h);
}
MU_INLINE uint64_t mu_ts_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if(n <= 0)
        return 0;
    return (uint64_t)n;
}

#endif

/* ------------------------------------------------------------------------- */
/* Thread system API                                                          */
/* ------------------------------------------------------------------------- */

typedef void (*mu_ts_task_fn)(void* user, uint64_t thread_id);

/*
    Bounded task ring capacity. Producers block when the ring is full, which
    gives backpressure instead of unbounded growth and per-task allocation.
    Must be a power of two (index wrap uses a mask).
*/
#ifndef MU_TS_TASK_RING_CAPACITY
#define MU_TS_TASK_RING_CAPACITY 1024
#endif
MU_STATIC_ASSERT(MU_IS_POW2(MU_TS_TASK_RING_CAPACITY), "MU_TS_TASK_RING_CAPACITY must be a power of two");

typedef struct mu_ts_init_desc
{
    bool        set_affinity_mask;
    uint64_t    affinity_mask[16];
    uint64_t    thread_count;
    const char* thread_name;
} mu_ts_init_desc;

typedef struct mu_ts_exit_desc
{
    bool abandon_tasks;
    bool detach_threads;
} mu_ts_exit_desc;

typedef struct mu_ts_info
{
    uint64_t    thread_count;
    uint64_t    executed_thread_count;
    uint64_t    active_thread_count;
    const char* thread_name;
} mu_ts_info;

struct mu_ts_system;
typedef struct mu_ts_system* mu_ts_handle;

static const mu_ts_init_desc MU_TS_INIT_DESC_DEFAULT = {
    false,
    {0},
    UINT64_MAX,
    NULL,
};

static const mu_ts_exit_desc MU_TS_EXIT_DESC_DEFAULT = {
    false,
    false,
};

typedef struct mu_ts_task
{
    mu_ts_task_fn fn;
    void*         user;
} mu_ts_task;

typedef struct mu_ts_worker_bootstrap
{
    struct mu_ts_system* sys;
    uint64_t             worker_id;
    bool                 set_affinity;
    uint64_t             affinity_mask[16];
} mu_ts_worker_bootstrap;

typedef struct mu_ts_system
{
    mu_ts_mutex mutex;
    mu_ts_cond  condition_tasks;
    mu_ts_cond  condition_free;
    mu_ts_cond  condition_idle;

    const char* name;
    uint64_t    thread_count;

    mu_ts_thread_handle*    threads;
    mu_ts_worker_bootstrap* boot;

    /*
        Published OS thread id per worker slot (1-based slot index). Used to
        answer "is the calling thread one of my workers?" without requiring
        compiler TLS, which would not survive a header-only multi-TU build.
    */
    mu_ts_atomic64_t* worker_thread_ids;

    /*
        Fixed ring. tasks_alloc_index is the producer cursor, tasks_work_index
        the consumer cursor. Both only grow; the slot is (index & (cap - 1)).
        Free slots = capacity - (alloc - work).
    */
    mu_ts_task* tasks;
    uint64_t    tasks_capacity;
    uint64_t    tasks_alloc_index;
    uint64_t    tasks_work_index;

    /* Queued + currently running. waitIdle is simply "this reaches zero". */
    uint64_t waiting_task_count;

    /*
        Reference count: owner handle + one per live worker. The last release
        destroys the system, which makes detachThreads safe.
    */
    mu_ts_atomic32_t ref_count;
    mu_ts_atomic32_t activated_thread_count_atomic;
    mu_ts_atomic32_t active_thread_count_atomic;

    mu_ts_atomic32_t stop;
    mu_ts_atomic32_t stop_abandon;
} mu_ts_system;

MU_INLINE uint64_t mu_ts__now_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)(tv.tv_usec / 1000);
#endif
}

MU_INLINE uint64_t mu_ts__current_thread_id(void)
{
#if defined(_WIN32)
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

/* Returns the 1-based worker id of the calling thread, or 0 if not a worker. */
MU_INLINE uint64_t mu_ts__worker_id(mu_ts_system* t)
{
    if(!t || !t->worker_thread_ids || t->thread_count == 0)
        return 0;

    uint64_t self = mu_ts__current_thread_id();
    for(uint64_t i = 0; i < t->thread_count; ++i)
    {
        if(mu_ts_atomic64_load_acquire(&t->worker_thread_ids[i]) == self)
            return i + 1;
    }
    return 0;
}

MU_INLINE bool mu_ts_is_worker_thread(mu_ts_handle h)
{
    return mu_ts__worker_id(h) != 0;
}

MU_INLINE void mu_ts__destroy(mu_ts_system* t)
{
    mu_ts_cond_exit(&t->condition_tasks);
    mu_ts_cond_exit(&t->condition_free);
    mu_ts_cond_exit(&t->condition_idle);
    mu_ts_mutex_exit(&t->mutex);

    MU_FREE(t->tasks);
    MU_FREE(t->threads);
    MU_FREE(t->boot);
    MU_FREE(t->worker_thread_ids);
    MU_FREE(t);
}

MU_INLINE void mu_ts__acquire(mu_ts_system* t)
{
    (void)mu_ts_atomic32_add_relaxed(&t->ref_count, 1);
}

MU_INLINE void mu_ts__release(mu_ts_system* t)
{
    uint32_t previous = mu_ts_atomic32_add_relaxed(&t->ref_count, -1);
    if(previous == 1)
        mu_ts__destroy(t);
}

MU_INLINE bool mu_ts__stopping(mu_ts_system* t)
{
    return mu_ts_atomic32_load_relaxed(&t->stop) != 0 ||
           mu_ts_atomic32_load_relaxed(&t->stop_abandon) != 0;
}

MU_INLINE void mu_ts__task_completed(mu_ts_system* t)
{
    mu_ts_mutex_lock(&t->mutex);
    MU_ASSERT(t->waiting_task_count > 0);
    --t->waiting_task_count;
    if(t->waiting_task_count == 0)
        mu_ts_cond_wake_all(&t->condition_idle);
    mu_ts_mutex_unlock(&t->mutex);
}

MU_INLINE mu_ts_task mu_ts__get_task(mu_ts_system* t, bool blocking)
{
    mu_ts_task task;
    task.fn   = NULL;
    task.user = NULL;

    mu_ts_mutex_lock(&t->mutex);

    for(;;)
    {
        if(mu_ts_atomic32_load_relaxed(&t->stop_abandon) != 0)
            break;

        if(t->tasks_work_index < t->tasks_alloc_index)
        {
            task = t->tasks[t->tasks_work_index & (uint64_t)(MU_TS_TASK_RING_CAPACITY - 1)];
            ++t->tasks_work_index;
            mu_ts_cond_wake_one(&t->condition_free);
            break;
        }

        if(mu_ts_atomic32_load_relaxed(&t->stop) != 0)
            break;

        if(!blocking)
            break;

        (void)mu_ts_cond_wait(&t->condition_tasks, &t->mutex, UINT32_MAX);
    }

    mu_ts_mutex_unlock(&t->mutex);
    return task;
}

#if defined(_WIN32)
static DWORD WINAPI mu_ts__worker_entry(LPVOID arg)
#else
static void* mu_ts__worker_entry(void* arg)
#endif
{
    mu_ts_worker_bootstrap* boot = (mu_ts_worker_bootstrap*)arg;
    mu_ts_system*           t    = boot->sys;
    uint64_t                tid  = boot->worker_id;

    mu_ts_atomic64_store_release(&t->worker_thread_ids[tid - 1], mu_ts__current_thread_id());

#if !defined(_WIN32)
    (void)boot;
    (void)t;
    (void)tid;
    /*
        Affinity mask and per-thread naming are intentionally no-op here to keep
        strict C99 portability without requiring platform-specific pthread
        extension prototypes.
    */
#endif

    (void)mu_ts_atomic32_add_relaxed(&t->activated_thread_count_atomic, 1);

    for(;;)
    {
        mu_ts_task task = mu_ts__get_task(t, true);
        if(task.fn)
        {
            task.fn(task.user, tid);
            mu_ts__task_completed(t);
            continue;
        }
        if(mu_ts__stopping(t))
        {
            break;
        }
    }

    (void)mu_ts_atomic32_add_relaxed(&t->active_thread_count_atomic, -1);

    /* Last worker release (detach path) is what frees the system. */
    mu_ts__release(t);

#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

MU_INLINE bool mu_ts_init(mu_ts_handle* out, const mu_ts_init_desc* in_desc)
{
    mu_ts_init_desc        local_desc = MU_TS_INIT_DESC_DEFAULT;
    const mu_ts_init_desc* desc       = in_desc ? in_desc : &local_desc;

    if(!out)
        return false;
    *out = NULL;

    if(desc->thread_count == 0)
    {
        return true;
    }

    uint64_t cpu_count = mu_ts_cpu_count();
    uint64_t count     = desc->thread_count;

    if(count > cpu_count)
        count = cpu_count;
    if(count == 0)
        return false;

    mu_ts_system* t = (mu_ts_system*)MU_MALLOC(sizeof(mu_ts_system));
    if(!t)
        return false;
    memset(t, 0, sizeof(*t));

    t->threads = (mu_ts_thread_handle*)MU_MALLOC((size_t)(count * sizeof(mu_ts_thread_handle)));
    t->boot    = (mu_ts_worker_bootstrap*)MU_MALLOC((size_t)(count * sizeof(mu_ts_worker_bootstrap)));
    t->tasks   = (mu_ts_task*)MU_MALLOC((size_t)MU_TS_TASK_RING_CAPACITY * sizeof(mu_ts_task));
    t->worker_thread_ids = (mu_ts_atomic64_t*)MU_CALLOC((size_t)count, sizeof(mu_ts_atomic64_t));

    if(!t->threads || !t->boot || !t->tasks || !t->worker_thread_ids)
    {
        MU_FREE(t->worker_thread_ids);
        MU_FREE(t->tasks);
        MU_FREE(t->threads);
        MU_FREE(t->boot);
        MU_FREE(t);
        return false;
    }

    t->name            = desc->thread_name ? desc->thread_name : "mu_ts";
    t->thread_count    = count;
    t->tasks_capacity  = (uint64_t)MU_TS_TASK_RING_CAPACITY;

    bool mutex_ok = mu_ts_mutex_init(&t->mutex);
    bool tasks_ok = false;
    bool free_ok  = false;
    bool idle_ok  = false;

    if(mutex_ok)
        tasks_ok = mu_ts_cond_init(&t->condition_tasks);
    if(mutex_ok && tasks_ok)
        free_ok = mu_ts_cond_init(&t->condition_free);
    if(mutex_ok && tasks_ok && free_ok)
        idle_ok = mu_ts_cond_init(&t->condition_idle);

    if(!mutex_ok || !tasks_ok || !free_ok || !idle_ok)
    {
        if(idle_ok)
            mu_ts_cond_exit(&t->condition_idle);
        if(free_ok)
            mu_ts_cond_exit(&t->condition_free);
        if(tasks_ok)
            mu_ts_cond_exit(&t->condition_tasks);
        if(mutex_ok)
            mu_ts_mutex_exit(&t->mutex);
        MU_FREE(t->tasks);
        MU_FREE(t->threads);
        MU_FREE(t->boot);
        MU_FREE(t->worker_thread_ids);
        MU_FREE(t);
        return false;
    }

    mu_ts_atomic32_store_relaxed(&t->ref_count, 0);
    mu_ts_atomic32_store_relaxed(&t->activated_thread_count_atomic, 0);
    mu_ts_atomic32_store_relaxed(&t->active_thread_count_atomic, (uint32_t)count);
    mu_ts_atomic32_store_relaxed(&t->stop, 0);
    mu_ts_atomic32_store_relaxed(&t->stop_abandon, 0);

    /* Owner reference keeps the system alive until the last worker is gone. */
    mu_ts__acquire(t);

    for(uint64_t i = 0; i < count; ++i)
    {
        t->boot[i].sys          = t;
        t->boot[i].worker_id    = i + 1;
        t->boot[i].set_affinity = desc->set_affinity_mask;
        memcpy(t->boot[i].affinity_mask, desc->affinity_mask, sizeof(t->boot[i].affinity_mask));

        mu_ts__acquire(t); /* reference owned by the worker */

        if(!mu_ts_thread_start(&t->threads[i], mu_ts__worker_entry, &t->boot[i]))
        {
            mu_ts__release(t); /* undo the worker reference that never started */

            mu_ts_mutex_lock(&t->mutex);
            mu_ts_atomic32_store_relaxed(&t->stop, 1);
            mu_ts_atomic32_store_relaxed(&t->stop_abandon, 1);
            mu_ts_cond_wake_all(&t->condition_tasks);
            mu_ts_cond_wake_all(&t->condition_free);
            mu_ts_mutex_unlock(&t->mutex);

            for(uint64_t j = 0; j < i; ++j)
            {
                mu_ts_thread_join(t->threads[j]);
            }

            /* Owner ref is last: this destroys the system. */
            mu_ts__release(t);
            return false;
        }
    }

    *out = t;
    return true;
}

MU_INLINE void mu_ts_exit(mu_ts_handle* h, const mu_ts_exit_desc* in_desc)
{
    mu_ts_exit_desc        local_desc = MU_TS_EXIT_DESC_DEFAULT;
    const mu_ts_exit_desc* desc       = in_desc ? in_desc : &local_desc;

    if(!h || !*h)
        return;

    mu_ts_system* t = *h;
    *h              = NULL;

    mu_ts_mutex_lock(&t->mutex);
    mu_ts_atomic32_store_relaxed(&t->stop, 1);
    if(desc->abandon_tasks)
        mu_ts_atomic32_store_relaxed(&t->stop_abandon, 1);
    mu_ts_cond_wake_all(&t->condition_tasks);
    mu_ts_cond_wake_all(&t->condition_free);
    mu_ts_mutex_unlock(&t->mutex);

    for(uint64_t i = 0; i < t->thread_count; ++i)
    {
        if(!desc->detach_threads)
            mu_ts_thread_join(t->threads[i]);
        else
            mu_ts_thread_detach(t->threads[i]);
    }

    /*
        Drop the owner reference. With join this is the last reference and the
        system is destroyed here. With detach the last worker to exit destroys
        it, so no worker can touch freed memory.
    */
    mu_ts__release(t);
}

MU_INLINE void mu_ts_add_tasks(mu_ts_handle h, mu_ts_task_fn fn, uint64_t count, uint64_t user_size, void* user_array)
{
    if(!fn || count == 0)
        return;

    if(!h)
    {
        uint8_t* users = (uint8_t*)user_array;
        for(uint64_t i = 0; i < count; ++i)
        {
            void* user = users ? (void*)(users + i * user_size) : NULL;
            fn(user, UINT64_MAX);
        }
        return;
    }

    mu_ts_system* t      = h;
    uint8_t*      users  = (uint8_t*)user_array;
    uint64_t      submitted = 0;

    /*
        A worker must never block here. If every worker is inside a task that
        fans out, the only threads that could drain the ring are the blocked
        ones, and condition_free never fires again: a permanent deadlock from
        a perfectly ordinary "submit more work from inside a task". So a worker
        pushes only what fits right now and, when the ring is completely full,
        runs one queued task inline to make room (work-first, the same policy
        mu_ts_assist uses). External producers keep the blocking backpressure:
        they cannot run the pool's work, so spinning or running nothing would
        only burn a core.
    */
    uint64_t self_id   = mu_ts__worker_id(t);
    bool     is_worker = self_id != 0;

    while(submitted < count)
    {
        uint64_t chunk = count - submitted;
        if(chunk > t->tasks_capacity)
            chunk = t->tasks_capacity;

        mu_ts_mutex_lock(&t->mutex);

        if(mu_ts__stopping(t))
        {
            mu_ts_mutex_unlock(&t->mutex);
            return;
        }

        uint64_t free_slots = t->tasks_capacity - (t->tasks_alloc_index - t->tasks_work_index);

        if(is_worker)
        {
            if(free_slots == 0)
            {
                /* Ring full: make room by running one queued task ourselves. */
                MU_ASSERT(t->tasks_work_index < t->tasks_alloc_index);

                uint64_t   slot  = t->tasks_work_index & (uint64_t)(MU_TS_TASK_RING_CAPACITY - 1);
                mu_ts_task spare = t->tasks[slot];

                ++t->tasks_work_index;
                mu_ts_cond_wake_one(&t->condition_free);
                mu_ts_mutex_unlock(&t->mutex);

                if(spare.fn)
                {
                    /* Outside the lock: the task may submit more work itself. */
                    spare.fn(spare.user, self_id);
                    mu_ts__task_completed(t);
                }
                continue;
            }

            if(chunk > free_slots)
                chunk = free_slots; /* partial push; never wait for the rest */
        }
        else
        {
            /* Backpressure: block until the ring has room for this whole chunk. */
            while(free_slots < chunk)
            {
                (void)mu_ts_cond_wait(&t->condition_free, &t->mutex, UINT32_MAX);
                if(mu_ts__stopping(t))
                {
                    mu_ts_mutex_unlock(&t->mutex);
                    return;
                }
                free_slots = t->tasks_capacity - (t->tasks_alloc_index - t->tasks_work_index);
            }
        }

        for(uint64_t i = 0; i < chunk; ++i)
        {
            uint64_t slot = t->tasks_alloc_index & (uint64_t)(MU_TS_TASK_RING_CAPACITY - 1);
            uint64_t index = submitted + i;

            t->tasks[slot].fn   = fn;
            t->tasks[slot].user = users ? (void*)(users + index * user_size) : NULL;
            ++t->tasks_alloc_index;
        }

        t->waiting_task_count += chunk;

        if(chunk == 1)
            mu_ts_cond_wake_one(&t->condition_tasks);
        else
            mu_ts_cond_wake_all(&t->condition_tasks);

        mu_ts_mutex_unlock(&t->mutex);

        submitted += chunk;
    }
}

MU_INLINE void mu_ts_add_task(mu_ts_handle h, mu_ts_task_fn fn, void* user)
{
    mu_ts_add_tasks(h, fn, 1, 0, user);
}

MU_INLINE bool mu_ts_assist(mu_ts_handle h)
{
    if(!h)
        return false;

    mu_ts_task task = mu_ts__get_task(h, false);
    if(!task.fn)
        return false;

    /* Keep worker_index meaningful when a worker assists while waiting. */
    uint64_t worker_id = mu_ts__worker_id(h);
    task.fn(task.user, worker_id ? worker_id : UINT64_MAX);
    mu_ts__task_completed(h);

    return true;
}

MU_INLINE bool mu_ts_wait_idle_timeout(mu_ts_handle h, uint32_t timeout_ms)
{
    if(!h)
        return true;

    /*
        A worker's own task counts as waiting, so blocking here could never
        observe zero. timeout_ms == 0 stays valid as an "am I idle?" query and
        naturally reports false.
    */
    if(timeout_ms != 0 && mu_ts_is_worker_thread(h))
    {
        MU_ASSERT(!"mu_ts_wait_idle: a worker cannot wait for itself; use mu_ts_assist");
        return false;
    }

    mu_ts_system* t     = h;
    uint64_t      start = mu_ts__now_ms();

    mu_ts_mutex_lock(&t->mutex);

    for(;;)
    {
        if(t->waiting_task_count == 0)
        {
            mu_ts_mutex_unlock(&t->mutex);
            return true;
        }

        if(timeout_ms == 0)
        {
            mu_ts_mutex_unlock(&t->mutex);
            return false;
        }

        uint32_t wait_ms = UINT32_MAX;
        if(timeout_ms != UINT32_MAX)
        {
            uint64_t elapsed = mu_ts__now_ms() - start;
            if(elapsed >= (uint64_t)timeout_ms)
            {
                mu_ts_mutex_unlock(&t->mutex);
                return false;
            }

            uint64_t remaining = (uint64_t)timeout_ms - elapsed;
            wait_ms = (remaining > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)remaining;
        }

        (void)mu_ts_cond_wait(&t->condition_idle, &t->mutex, wait_ms);
    }
}

MU_INLINE bool mu_ts_is_idle(mu_ts_handle h)
{
    return mu_ts_wait_idle_timeout(h, 0);
}

MU_INLINE void mu_ts_wait_idle(mu_ts_handle h)
{
    (void)mu_ts_wait_idle_timeout(h, UINT32_MAX);
}

MU_INLINE void mu_ts_get_info(mu_ts_handle h, mu_ts_info* out_info)
{
    if(!out_info)
        return;
    memset(out_info, 0, sizeof(*out_info));

    if(!h)
        return;

    out_info->thread_count          = h->thread_count;
    out_info->executed_thread_count = (uint64_t)mu_ts_atomic32_load_relaxed(&h->activated_thread_count_atomic);
    out_info->active_thread_count   = (uint64_t)mu_ts_atomic32_load_relaxed(&h->active_thread_count_atomic);
    out_info->thread_name           = h->name;
}

#ifdef __cplusplus
}
#endif

#endif /* MU_THREAD_SYSTEM_H */
