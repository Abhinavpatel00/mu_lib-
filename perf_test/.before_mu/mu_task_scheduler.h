#ifndef MU_TASK_SCHEDULER_H
#define MU_TASK_SCHEDULER_H

/*
    mu_task_scheduler.h
    ---------------------------------------------------------------------------
    Standalone C99 single-header task scheduler.

    Inspired by The Forge TaskScheduler concepts:
    - Task groups / wait counters
    - Priority queues
    - Dependency-aware task submission
    - Worker thread pool backend

    Backend:
    - Uses mu_thread_system.h for worker execution.

    VISUAL MODEL

      submit(task)
          |
          +--> pending list (if dependency not zero)
          |          |
          |          +-- promoted to ready when dependency hits zero
          |
          +--> ready queues: HIGH -> NORMAL -> LOW
                                  |
                                  v
                        dispatched to thread pool
                                  |
                                  v
                         task completes -> counter--

    Group/counter rule:
    - Counter increments when task is submitted with completion_counter.
    - Counter decrements when that task finishes.
    - Waiting on counter/group == waiting for all associated tasks.

    ---------------------------------------------------------------------------
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mu_thread_system.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "mu_macros.h"

/* ------------------------------------------------------------------------- */
/* Wait counter / task group                                                  */
/* ------------------------------------------------------------------------- */

typedef struct mu_task_scheduler mu_task_scheduler;

MU_INLINE bool mu_sched_assist(mu_task_scheduler* s);

typedef struct mu_sched_counter {
    mu_ts_atomic32_t value;
    mu_ts_mutex mutex;
    mu_ts_cond condition_zero;
    mu_ts_atomic64_t owner;   /* scheduler this counter was first used with */
    bool initialized;
} mu_sched_counter;

typedef mu_sched_counter mu_sched_group;

MU_INLINE bool mu_sched_counter_init(mu_sched_counter* c, uint32_t initial)
{
    if (!c) return false;
    memset(c, 0, sizeof(*c));

    if (!mu_ts_mutex_init(&c->mutex)) return false;
    if (!mu_ts_cond_init(&c->condition_zero)) {
        mu_ts_mutex_exit(&c->mutex);
        return false;
    }

    mu_ts_atomic32_store_relaxed(&c->value, initial);
    c->initialized = true;
    return true;
}

MU_INLINE void mu_sched_counter_exit(mu_sched_counter* c)
{
    if (!c || !c->initialized) return;
    mu_ts_cond_exit(&c->condition_zero);
    mu_ts_mutex_exit(&c->mutex);
    c->initialized = false;
}

MU_INLINE uint32_t mu_sched_counter_get(mu_sched_counter* c)
{
    if (!c || !c->initialized) return 0;
    return mu_ts_atomic32_load_relaxed(&c->value);
}

MU_INLINE void mu_sched_counter_add(mu_sched_counter* c, uint32_t delta)
{
    if (!c || !c->initialized || delta == 0) return;
    (void)mu_ts_atomic32_add_relaxed(&c->value, (int32_t)delta);
}

/* Defined after the scheduler: publishing zero also promotes dependents. */
MU_INLINE void mu_sched_counter_sub(mu_sched_counter* c, uint32_t delta);
MU_INLINE bool mu_sched__counter_sub_impl(mu_sched_counter* c, uint32_t delta, bool promote_dependents);

MU_INLINE bool mu_sched_counter_is_zero(mu_sched_counter* c)
{
    return mu_sched_counter_get(c) == 0;
}

MU_INLINE void mu_sched__counter_bind(mu_sched_counter* c, mu_task_scheduler* s)
{
    if (!c) return;

    uint64_t want = (uint64_t)(uintptr_t)s;
    uint64_t prev = mu_ts_atomic64_cas_relaxed(&c->owner, 0, want);

    /* One counter belongs to one scheduler; waits from other workers can't assist it. */
    MU_ASSERT(prev == 0 || prev == want);
}

MU_INLINE mu_task_scheduler* mu_sched__counter_owner(mu_sched_counter* c)
{
    if (!c) return NULL;
    return (mu_task_scheduler*)(uintptr_t)mu_ts_atomic64_load_acquire(&c->owner);
}

/* Defined after the scheduler: workers assist instead of parking a thread. */
MU_INLINE bool mu_sched_counter_wait_timeout(mu_sched_counter* c, uint32_t timeout_ms);

MU_INLINE void mu_sched_counter_wait(mu_sched_counter* c)
{
    (void)mu_sched_counter_wait_timeout(c, UINT32_MAX);
}

/* Group aliases */
MU_INLINE bool mu_sched_group_init(mu_sched_group* g, uint32_t initial) { return mu_sched_counter_init(g, initial); }
MU_INLINE void mu_sched_group_exit(mu_sched_group* g) { mu_sched_counter_exit(g); }
MU_INLINE void mu_sched_group_wait(mu_sched_group* g) { mu_sched_counter_wait(g); }
MU_INLINE bool mu_sched_group_wait_timeout(mu_sched_group* g, uint32_t ms) { return mu_sched_counter_wait_timeout(g, ms); }
MU_INLINE uint32_t mu_sched_group_count(mu_sched_group* g) { return mu_sched_counter_get(g); }

/* ------------------------------------------------------------------------- */
/* Task scheduler                                                             */
/* ------------------------------------------------------------------------- */

typedef enum mu_sched_priority {
    MU_SCHED_PRIORITY_HIGH = 0,
    MU_SCHED_PRIORITY_NORMAL = 1,
    MU_SCHED_PRIORITY_LOW = 2,
    MU_SCHED_PRIORITY_COUNT = 3
} mu_sched_priority;

typedef void (*mu_sched_task_fn)(void* user, uint32_t worker_index);

typedef struct mu_sched_task_desc {
    mu_sched_task_fn fn;
    void* user;
    mu_sched_priority priority;

    /* Optional: task can run only when this counter reaches zero. */
    mu_sched_counter* depends_on;

    /* Optional: incremented on submit and decremented on completion. */
    mu_sched_counter* completion_counter;
} mu_sched_task_desc;

typedef struct mu_sched_init_desc {
    uint64_t worker_count;
    const char* thread_name;
    bool set_affinity_mask;
    uint64_t affinity_mask[16];

    /*
       Dispatch limit controls max number of tasks concurrently handed to
       mu_thread_system.

       0 => default = max(1, worker_count * 2)
    */
    uint64_t dispatch_limit;
} mu_sched_init_desc;

static const mu_sched_init_desc MU_SCHED_INIT_DESC_DEFAULT = {
    UINT64_MAX,
    "mu_sched",
    false,
    {0},
    0,
};

typedef struct mu_sched_task_node {
    mu_sched_task_desc desc;
    mu_task_scheduler* scheduler;
    struct mu_sched_task_node* next;
} mu_sched_task_node;

struct mu_task_scheduler {
    mu_ts_handle thread_system;

    mu_ts_mutex mutex;
    mu_ts_cond condition_idle;

    mu_sched_task_node* ready_head[MU_SCHED_PRIORITY_COUNT];
    mu_sched_task_node* ready_tail[MU_SCHED_PRIORITY_COUNT];
    mu_sched_task_node* pending_head;

    uint64_t inflight_count;  /* pending + ready + active */
    uint64_t active_count;    /* submitted to thread system, not completed */
    uint64_t dispatch_limit;
    uint64_t worker_count;
};

MU_INLINE bool mu_sched_wait_idle_timeout(mu_task_scheduler* s, uint32_t timeout_ms);
MU_INLINE void mu_sched_wait_idle(mu_task_scheduler* s);

MU_INLINE mu_sched_priority mu_sched__clamp_priority(mu_sched_priority p)
{
    if ((int)p < 0 || (int)p >= (int)MU_SCHED_PRIORITY_COUNT) return MU_SCHED_PRIORITY_NORMAL;
    return p;
}

MU_INLINE void mu_sched__ready_push(mu_task_scheduler* s, mu_sched_task_node* node)
{
    mu_sched_priority p = mu_sched__clamp_priority(node->desc.priority);
    node->next = NULL;

    if (!s->ready_head[p]) {
        s->ready_head[p] = node;
        s->ready_tail[p] = node;
    } else {
        s->ready_tail[p]->next = node;
        s->ready_tail[p] = node;
    }
}

MU_INLINE mu_sched_task_node* mu_sched__ready_pop(mu_task_scheduler* s)
{
    for (int p = 0; p < (int)MU_SCHED_PRIORITY_COUNT; ++p) {
        mu_sched_task_node* n = s->ready_head[p];
        if (!n) continue;

        s->ready_head[p] = n->next;
        if (!s->ready_head[p]) s->ready_tail[p] = NULL;
        n->next = NULL;
        return n;
    }
    return NULL;
}

MU_INLINE void mu_sched__pending_push(mu_task_scheduler* s, mu_sched_task_node* node)
{
    node->next = s->pending_head;
    s->pending_head = node;
}

MU_INLINE void mu_sched__append_submit_list(mu_sched_task_node** head, mu_sched_task_node** tail, mu_sched_task_node* node)
{
    node->next = NULL;
    if (!*head) {
        *head = node;
        *tail = node;
    } else {
        (*tail)->next = node;
        *tail = node;
    }
}

MU_INLINE void mu_sched__promote_pending_locked(mu_task_scheduler* s)
{
    mu_sched_task_node* prev = NULL;
    mu_sched_task_node* it = s->pending_head;

    while (it) {
        mu_sched_task_node* next = it->next;

        bool ready = true;
        if (it->desc.depends_on) {
            ready = mu_sched_counter_is_zero(it->desc.depends_on);
        }

        if (ready) {
            if (prev) prev->next = next;
            else s->pending_head = next;

            it->next = NULL;
            mu_sched__ready_push(s, it);
        } else {
            prev = it;
        }

        it = next;
    }
}

MU_INLINE void mu_sched__collect_dispatch_locked(mu_task_scheduler* s, mu_sched_task_node** submit_head, mu_sched_task_node** submit_tail)
{
    while (s->active_count < s->dispatch_limit) {
        mu_sched_task_node* n = mu_sched__ready_pop(s);
        if (!n) break;

        s->active_count += 1;
        mu_sched__append_submit_list(submit_head, submit_tail, n);
    }
}

MU_INLINE void mu_sched__task_thunk(void* user, uint64_t thread_id)
{
    mu_sched_task_node* node = (mu_sched_task_node*)user;
    mu_task_scheduler* s = node->scheduler;

    uint32_t worker_index = (thread_id == UINT64_MAX) ? UINT32_MAX : (uint32_t)(thread_id - 1);
    node->desc.fn(node->desc.user, worker_index);

    bool released = false;
    if (node->desc.completion_counter) {
        released = mu_sched__counter_sub_impl(node->desc.completion_counter, 1, false);
    }

    mu_sched_task_node* submit_head = NULL;
    mu_sched_task_node* submit_tail = NULL;

    mu_ts_mutex_lock(&s->mutex);

    MU_ASSERT(s->active_count > 0);
    MU_ASSERT(s->inflight_count > 0);

    s->active_count -= 1;
    s->inflight_count -= 1;

    if (released) {
        mu_sched__promote_pending_locked(s);
    }
    mu_sched__collect_dispatch_locked(s, &submit_head, &submit_tail);

    if (s->inflight_count == 0) {
        mu_ts_cond_wake_all(&s->condition_idle);
    }

    mu_ts_mutex_unlock(&s->mutex);

    MU_FREE(node);

    /* Submit new ready tasks after unlock (safe even in dummy mode). */
    mu_sched_task_node* it = submit_head;
    while (it) {
        mu_sched_task_node* next = it->next;
        it->next = NULL;
        mu_ts_add_task(s->thread_system, mu_sched__task_thunk, it);
        it = next;
    }
}

MU_INLINE mu_sched_task_node* mu_sched__counter_publish_zero_locked(mu_sched_counter* c,
                                                       mu_task_scheduler** owner_out)
{
    *owner_out = NULL;

    mu_task_scheduler* s = mu_sched__counter_owner(c);
    if (!s) return NULL;

    mu_sched_task_node* head = NULL;
    mu_sched_task_node* tail = NULL;

    mu_ts_mutex_lock(&s->mutex);
    mu_sched__promote_pending_locked(s);
    mu_sched__collect_dispatch_locked(s, &head, &tail);
    mu_ts_mutex_unlock(&s->mutex);

    *owner_out = s;
    return head;
}

MU_INLINE bool mu_sched__counter_sub_impl(mu_sched_counter* c, uint32_t delta, bool promote_dependents)
{
    if (!c || !c->initialized || delta == 0) return false;

    for (;;) {
        uint32_t old = mu_ts_atomic32_load_relaxed(&c->value);
        MU_ASSERT(old >= delta);
        uint32_t now = old - delta;

        if (now == 0) {
            mu_ts_mutex_lock(&c->mutex);
            if (mu_ts_atomic32_load_relaxed(&c->value) != old) {
                mu_ts_mutex_unlock(&c->mutex);
                continue;
            }

            mu_ts_atomic32_store_relaxed(&c->value, 0);
            mu_ts_cond_wake_all(&c->condition_zero);

            if (!promote_dependents) {
                mu_ts_mutex_unlock(&c->mutex);
                return true;
            }

            mu_task_scheduler* owner = NULL;
            mu_sched_task_node* head = mu_sched__counter_publish_zero_locked(c, &owner);

            mu_ts_mutex_unlock(&c->mutex);

            if (head) {
                mu_task_scheduler* s = owner;
                mu_sched_task_node* it = head;
                while (it) {
                    mu_sched_task_node* next = it->next;
                    it->next = NULL;
                    mu_ts_add_task(s->thread_system, mu_sched__task_thunk, it);
                    it = next;
                }
            }

            return true;
        }

        if (mu_ts_atomic32_cas_relaxed(&c->value, old, now) == old)
            return false;
    }
}

MU_INLINE void mu_sched_counter_sub(mu_sched_counter* c, uint32_t delta)
{
    (void)mu_sched__counter_sub_impl(c, delta, true);
}

MU_INLINE bool mu_sched_init(mu_task_scheduler** out_sched, const mu_sched_init_desc* in_desc)
{
    if (!out_sched) return false;
    *out_sched = NULL;

    mu_sched_init_desc local_desc = MU_SCHED_INIT_DESC_DEFAULT;
    const mu_sched_init_desc* desc = in_desc ? in_desc : &local_desc;

    mu_task_scheduler* s = (mu_task_scheduler*)MU_MALLOC(sizeof(mu_task_scheduler));
    if (!s) return false;
    memset(s, 0, sizeof(*s));

    bool mutex_ok = mu_ts_mutex_init(&s->mutex);
    bool idle_ok = false;
    if (mutex_ok) idle_ok = mu_ts_cond_init(&s->condition_idle);

    if (!mutex_ok || !idle_ok) {
        if (idle_ok) mu_ts_cond_exit(&s->condition_idle);
        if (mutex_ok) mu_ts_mutex_exit(&s->mutex);
        MU_FREE(s);
        return false;
    }

    mu_ts_init_desc ts_desc = MU_TS_INIT_DESC_DEFAULT;
    ts_desc.thread_count = desc->worker_count;
    ts_desc.thread_name = desc->thread_name ? desc->thread_name : "mu_sched";
    ts_desc.set_affinity_mask = desc->set_affinity_mask;
    memcpy(ts_desc.affinity_mask, desc->affinity_mask, sizeof(ts_desc.affinity_mask));

    if (!mu_ts_init(&s->thread_system, &ts_desc)) {
        mu_ts_cond_exit(&s->condition_idle);
        mu_ts_mutex_exit(&s->mutex);
        MU_FREE(s);
        return false;
    }

    {
        mu_ts_info info;
        mu_ts_get_info(s->thread_system, &info);
        s->worker_count = info.thread_count;
    }

    if (desc->dispatch_limit != 0) s->dispatch_limit = desc->dispatch_limit;
    else {
        uint64_t wc = s->worker_count;
        if (wc == 0) wc = 1;
        s->dispatch_limit = wc * 2;
    }

    *out_sched = s;
    return true;
}

MU_INLINE void mu_sched_exit(mu_task_scheduler** sched)
{
    if (!sched || !*sched) return;
    mu_task_scheduler* s = *sched;
    *sched = NULL;

    /* Ordered shutdown: finish all scheduler work first. */
    mu_sched_wait_idle(s);
    mu_ts_wait_idle(s->thread_system);

    mu_ts_exit(&s->thread_system, &MU_TS_EXIT_DESC_DEFAULT);

    /* Free any leftover nodes if user exited while abandoning externally. */
    for (int p = 0; p < (int)MU_SCHED_PRIORITY_COUNT; ++p) {
        mu_sched_task_node* it = s->ready_head[p];
        while (it) {
            mu_sched_task_node* next = it->next;
            if (it->desc.completion_counter)
                mu_sched__counter_sub_impl(it->desc.completion_counter, 1, false);
            MU_FREE(it);
            it = next;
        }
    }

    {
        mu_sched_task_node* it = s->pending_head;
        while (it) {
            mu_sched_task_node* next = it->next;
            if (it->desc.completion_counter)
                mu_sched__counter_sub_impl(it->desc.completion_counter, 1, false);
            MU_FREE(it);
            it = next;
        }
    }

    mu_ts_cond_exit(&s->condition_idle);
    mu_ts_mutex_exit(&s->mutex);
    MU_FREE(s);
}

MU_INLINE bool mu_sched_submit(mu_task_scheduler* s, const mu_sched_task_desc* desc)
{
    if (!s || !desc || !desc->fn) return false;

    mu_sched_task_node* node = (mu_sched_task_node*)MU_MALLOC(sizeof(mu_sched_task_node));
    if (!node) return false;

    node->desc = *desc;
    node->desc.priority = mu_sched__clamp_priority(node->desc.priority);
    node->scheduler = s;
    node->next = NULL;

    if (node->desc.completion_counter) {
        mu_sched_counter_add(node->desc.completion_counter, 1);
        mu_sched__counter_bind(node->desc.completion_counter, s);
    }

    /* Remember who owns the dependency so worker waits can assist this scheduler. */
    if (node->desc.depends_on) {
        mu_sched__counter_bind(node->desc.depends_on, s);
    }

    mu_sched_task_node* submit_head = NULL;
    mu_sched_task_node* submit_tail = NULL;

    mu_ts_mutex_lock(&s->mutex);

    s->inflight_count += 1;

    if (node->desc.depends_on && !mu_sched_counter_is_zero(node->desc.depends_on)) {
        mu_sched__pending_push(s, node);
    } else {
        mu_sched__ready_push(s, node);
    }

    mu_sched__promote_pending_locked(s);
    mu_sched__collect_dispatch_locked(s, &submit_head, &submit_tail);

    mu_ts_mutex_unlock(&s->mutex);

    mu_sched_task_node* it = submit_head;
    while (it) {
        mu_sched_task_node* next = it->next;
        it->next = NULL;
        mu_ts_add_task(s->thread_system, mu_sched__task_thunk, it);
        it = next;
    }

    return true;
}

MU_INLINE bool mu_sched_submit_tasks(mu_task_scheduler* s,
                                           mu_sched_task_fn fn,
                                           uint64_t count,
                                           uint64_t user_size,
                                           void* user_array,
                                           mu_sched_priority priority,
                                           mu_sched_counter* depends_on,
                                           mu_sched_counter* completion_counter)
{
    if (!s || !fn || count == 0) return false;

    uint8_t* users = (uint8_t*)user_array;
    for (uint64_t i = 0; i < count; ++i) {
        mu_sched_task_desc d;
        d.fn = fn;
        d.user = users ? (void*)(users + i * user_size) : NULL;
        d.priority = priority;
        d.depends_on = depends_on;
        d.completion_counter = completion_counter;

        if (!mu_sched_submit(s, &d)) {
            return false;
        }
    }

    return true;
}

MU_INLINE bool mu_sched_assist(mu_task_scheduler* s)
{
    if (!s) return false;

    mu_sched_task_node* submit_head = NULL;
    mu_sched_task_node* submit_tail = NULL;

    mu_ts_mutex_lock(&s->mutex);
    mu_sched__promote_pending_locked(s);
    mu_sched__collect_dispatch_locked(s, &submit_head, &submit_tail);
    mu_ts_mutex_unlock(&s->mutex);

    mu_sched_task_node* it = submit_head;
    while (it) {
        mu_sched_task_node* next = it->next;
        it->next = NULL;
        mu_ts_add_task(s->thread_system, mu_sched__task_thunk, it);
        it = next;
    }

    return mu_ts_assist(s->thread_system);
}

/*
    Wait for a counter to reach zero.

    On a scheduler worker this cannot block: the pending work it is waiting on
    may be queued behind this very task. Instead it drains ready tasks for the
    owning scheduler until the counter reaches zero, then returns. Only when
    nothing is runnable locally (work produced by another thread) does it nap
    on the counter for a short slice.

    On non-worker threads the original condvar wait is used unchanged.
*/
MU_INLINE bool mu_sched_counter_wait_timeout(mu_sched_counter* c, uint32_t timeout_ms)
{
    if (!c || !c->initialized) return true;

    mu_task_scheduler* owner = mu_sched__counter_owner(c);

    if (owner && mu_ts_is_worker_thread(owner->thread_system)) {
        uint64_t start = mu_ts__now_ms();

        for (;;) {
            while (mu_ts_atomic32_load_acquire(&c->value) != 0) {
                if (mu_sched_assist(owner)) continue;

                if (timeout_ms == 0) return false;
                if (timeout_ms != UINT32_MAX &&
                    mu_ts__now_ms() - start >= (uint64_t)timeout_ms)
                    return false;

                /* No local work: sleep briefly so an external producer can finish. */
                mu_ts_mutex_lock(&c->mutex);
                if (mu_ts_atomic32_load_relaxed(&c->value) != 0)
                    (void)mu_ts_cond_wait(&c->condition_zero, &c->mutex, 2);
                mu_ts_mutex_unlock(&c->mutex);
            }

            /*
                Fence against the last decrementer: it publishes zero under the
                mutex, so taking the mutex here guarantees it is done touching
                the counter and the caller may destroy it.
            */
            mu_ts_mutex_lock(&c->mutex);
            bool zero = mu_ts_atomic32_load_relaxed(&c->value) == 0;
            mu_ts_mutex_unlock(&c->mutex);

            if (zero) return true;
        }
    }

    mu_ts_mutex_lock(&c->mutex);

    uint32_t elapsed = 0;
    const uint32_t slice_ms = 5;

    while (mu_ts_atomic32_load_relaxed(&c->value) != 0) {
        if (timeout_ms == 0) {
            mu_ts_mutex_unlock(&c->mutex);
            return false;
        }

        if (timeout_ms == UINT32_MAX) {
            (void)mu_ts_cond_wait(&c->condition_zero, &c->mutex, UINT32_MAX);
            continue;
        }

        uint32_t wait_ms = (timeout_ms - elapsed < slice_ms) ? (timeout_ms - elapsed) : slice_ms;
        if (wait_ms == 0) {
            mu_ts_mutex_unlock(&c->mutex);
            return false;
        }

        (void)mu_ts_cond_wait(&c->condition_zero, &c->mutex, wait_ms);
        elapsed += wait_ms;
        if (elapsed >= timeout_ms && mu_ts_atomic32_load_relaxed(&c->value) != 0) {
            mu_ts_mutex_unlock(&c->mutex);
            return false;
        }
    }

    mu_ts_mutex_unlock(&c->mutex);
    return true;
}

MU_INLINE bool mu_sched_wait_idle_timeout(mu_task_scheduler* s, uint32_t timeout_ms)
{
    if (!s) return true;

    /*
        On a worker, inflight_count includes the calling task itself, so an
        idle wait could never complete. timeout_ms == 0 remains a valid query
        and returns false.
    */
    if (timeout_ms != 0 && mu_ts_is_worker_thread(s->thread_system)) {
        MU_ASSERT(!"mu_sched_wait_idle: a worker cannot wait for itself; use mu_sched_assist");
        return false;
    }

    mu_ts_mutex_lock(&s->mutex);

    uint32_t elapsed = 0;
    const uint32_t slice_ms = 5;

    while (s->inflight_count != 0) {
        if (timeout_ms == 0) {
            mu_ts_mutex_unlock(&s->mutex);
            return false;
        }

        if (timeout_ms == UINT32_MAX) {
            (void)mu_ts_cond_wait(&s->condition_idle, &s->mutex, UINT32_MAX);
            continue;
        }

        uint32_t wait_ms = (timeout_ms - elapsed < slice_ms) ? (timeout_ms - elapsed) : slice_ms;
        if (wait_ms == 0) {
            mu_ts_mutex_unlock(&s->mutex);
            return false;
        }

        (void)mu_ts_cond_wait(&s->condition_idle, &s->mutex, wait_ms);
        elapsed += wait_ms;

        if (elapsed >= timeout_ms && s->inflight_count != 0) {
            mu_ts_mutex_unlock(&s->mutex);
            return false;
        }
    }

    mu_ts_mutex_unlock(&s->mutex);
    return true;
}

MU_INLINE void mu_sched_wait_idle(mu_task_scheduler* s)
{
    (void)mu_sched_wait_idle_timeout(s, UINT32_MAX);
}

MU_INLINE uint64_t mu_sched_worker_count(mu_task_scheduler* s)
{
    if (!s) return 0;
    return s->worker_count;
}

/* ------------------------------------------------------------------------- */
/* Parallel-for helper                                                       */
/* ------------------------------------------------------------------------- */

typedef void (*mu_sched_parallel_for_fn)(uint32_t begin, uint32_t end, uint32_t worker_index, void* user);

typedef struct mu_sched_parallel_for_context {
    mu_sched_parallel_for_fn fn;
    void* user;
    uint32_t begin;
    uint32_t end;
    uint32_t grain;
    mu_ts_atomic32_t next;
} mu_sched_parallel_for_context;

MU_INLINE void mu_sched__parallel_for_worker(void* user, uint32_t worker_index)
{
    mu_sched_parallel_for_context* ctx = (mu_sched_parallel_for_context*)user;

    for (;;) {
        uint32_t start = mu_ts_atomic32_add_relaxed(&ctx->next, (int32_t)ctx->grain);
        if (start >= ctx->end) break;

        uint32_t stop = start + ctx->grain;
        if (stop > ctx->end) stop = ctx->end;

        ctx->fn(start, stop, worker_index, ctx->user);
    }
}

MU_INLINE bool mu_sched_parallel_for(mu_task_scheduler* s,
                                           uint32_t begin,
                                           uint32_t end,
                                           uint32_t grain,
                                           mu_sched_priority priority,
                                           mu_sched_parallel_for_fn fn,
                                           void* user)
{
    if (!s || !fn) return false;
    if (begin >= end) return true;
    if (grain == 0) grain = 1;

    uint32_t total = end - begin;
    uint32_t chunks = (total + grain - 1) / grain;

    uint64_t workers = mu_sched_worker_count(s);
    if (workers == 0) workers = 1;

    uint32_t launch = (uint32_t)((chunks < workers) ? chunks : workers);
    if (launch == 0) launch = 1;

    mu_sched_parallel_for_context* ctx = (mu_sched_parallel_for_context*)MU_MALLOC(sizeof(mu_sched_parallel_for_context));
    if (!ctx) return false;

    ctx->fn = fn;
    ctx->user = user;
    ctx->begin = begin;
    ctx->end = end;
    ctx->grain = grain;
    mu_ts_atomic32_store_relaxed(&ctx->next, begin);

    mu_sched_counter done;
    if (!mu_sched_counter_init(&done, 0)) {
        MU_FREE(ctx);
        return false;
    }

    for (uint32_t i = 0; i < launch; ++i) {
        mu_sched_task_desc d;
        d.fn = mu_sched__parallel_for_worker;
        d.user = ctx;
        d.priority = priority;
        d.depends_on = NULL;
        d.completion_counter = &done;

        if (!mu_sched_submit(s, &d)) {
            mu_sched_counter_wait(&done);
            mu_sched_counter_exit(&done);
            MU_FREE(ctx);
            return false;
        }
    }

    mu_sched_counter_wait(&done);
    mu_sched_counter_exit(&done);
    MU_FREE(ctx);

    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* MU_TASK_SCHEDULER_H */
