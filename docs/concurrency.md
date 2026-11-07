# Concurrency: `mu/mu_thread_system.h` + `mu/mu_task_scheduler.h`

Two layers:

1. **`mu/mu_thread_system.h`** — a fixed worker pool plus portable atomics,
   mutexes, and condition variables.
2. **`mu/mu_task_scheduler.h`** — a job scheduler built on top: priorities,
   counters/groups, dependency tracking, and parallel-for.

Both are `static inline` single headers with malloc/free override macros
(`MU_TS_MALLOC`, `MU_SCHED_MALLOC`, ...).

---

## mu/mu_thread_system.h — worker pool + primitives

### Portability layer

| Group | Windows | POSIX |
|---|---|---|
| Thread handle | `HANDLE` | `pthread_t` |
| Mutex | `CRITICAL_SECTION` | `pthread_mutex_t` |
| Condvar | `CONDITION_VARIABLE` | `pthread_cond_t` |

```c
bool mu_ts_thread_start(mu_ts_thread_handle* out, mu_ts_thread_entry fn, void* user);
void mu_ts_thread_join(mu_ts_thread_handle h);
void mu_ts_thread_detach(mu_ts_thread_handle h);
uint64_t mu_ts_cpu_count(void);
```

### Atomics (`mu/mu_atomic.h` — the single implementation)

All atomics live in `mu/mu_atomic.h`. `mu_sync.h`, `mu_thread_system.h`,
and `mu_task_scheduler.h` add no primitives of their own — they only
compose these. Canonical types are small structs, acquire/release ordering
is explicit (correct on ARM, nearly free on x86):

```c
mu_atomic32_t v;  mu_atomic32_init(&v, 0);   /* struct wrapping one word */

mu_atomic32_load_relaxed(&v);  mu_atomic32_load_acquire(&v);
mu_atomic32_store_relaxed(&v, x); mu_atomic32_store_release(&v, x);
mu_atomic32_fetch_add(&v, delta);            /* returns previous           */
mu_atomic32_exchange(&v, x);                 /* returns previous           */
mu_atomic32_compare_exchange(&v, &expected, x); /* bool form; refreshes expected */
mu_atomic32_fetch_max(&v, x);                /* CAS loop, returns previous */

mu_cpu_relax(); /* pause/yield inside spin loops */
```

Same `mu_atomic64_*` and `mu_atomicptr_*` families. `mu_ts_atomic32_t` and
the `mu_ts_atomic32_*` names remain as thin aliases over this backend, so
existing thread-system / scheduler code keeps compiling.

### Mutex / condvar

```c
mu_ts_mutex m;  mu_ts_mutex_init(&m);  mu_ts_mutex_lock(&m);
mu_ts_mutex_unlock(&m);  mu_ts_mutex_exit(&m);

mu_ts_cond c;   mu_ts_cond_init(&c);
mu_ts_cond_wait(&c, &m, timeout_ms);   /* returns bool (false on timeout) */
mu_ts_cond_wake_one(&c);  mu_ts_cond_wake_all(&c);
mu_ts_cond_exit(&c);
```

### The pool

```c
typedef void (*mu_ts_task_fn)(void* user, uint64_t thread_id);

typedef struct mu_ts_init_desc
{
    bool        set_affinity_mask;  /* Windows only; no-op on POSIX       */
    uint64_t    affinity_mask[16];
    uint64_t    thread_count;       /* UINT64_MAX = hardware concurrency;
                                       0 = no workers (inline execution)  */
    const char* thread_name;
} mu_ts_init_desc;
static const mu_ts_init_desc MU_TS_INIT_DESC_DEFAULT = { ... };

typedef struct mu_ts_system* mu_ts_handle;

mu_ts_handle h;
mu_ts_init(&h, NULL);              /* NULL desc = defaults            */
mu_ts_add_task(h, my_fn, user);                    /* single task     */
mu_ts_add_tasks(h, my_fn, count, user_size, user_array);  /* batch: one task per element */
mu_ts_assist(h);                   /* run one pending task; false if none */
mu_ts_is_idle(h);                  /* true when nothing is queued/running  */
mu_ts_wait_idle(h);                /* block until queue drained       */
mu_ts_wait_idle_timeout(h, ms);    /* false on timeout                */
mu_ts_exit(h, NULL);               /* NULL = drain + join            */
```

- **Work model:** producers push tasks into a bounded ring
  (`MU_TS_TASK_RING_CAPACITY`, default 1024). Workers pop under one mutex,
  run the task, then signal the idle condition. When the ring is full,
  producers block on `condition_free` (backpressure) instead of growing
  without bound.
- `mu_ts_add_tasks` is the hot path: it enqueues `count` invocations over a
  contiguous user array (`user_size` = per-element stride) without one
  allocation per task. Passing a NULL handle (or `thread_count = 0`) runs
  the calls synchronously on the caller.
- Every task receives `(user_data, thread_id)` — `thread_id` lets you pin
  per-worker scratch without TLS. Worker ids are `1..N`; synchronous calls
  and `mu_ts_assist` from a non-worker pass `UINT64_MAX`. A worker that
  assists while waiting passes its own id, so `worker_index` stays valid.
- `mu_ts_exit_desc` controls shutdown: `abandon_tasks` skips queued work,
  `detach_threads` returns without joining. The system is reference
  counted (owner + one per worker), so detached workers free it when the
  last one exits — always safe to destroy immediately after `mu_ts_exit`.

### Worker identity and wait rules

```c
bool mu_ts_is_worker_thread(mu_ts_handle h);
```

Each worker publishes its OS thread id into the system when it starts, and
the check compares the caller against those ids. There is no compiler TLS,
so the answer is still correct when the same header is included from several
translation units; the cost is O(worker count), which is fine because it is
only used on wait paths.

- `mu_ts_wait_idle()` / `mu_ts_wait_idle_timeout(h, ms > 0)` from a worker is
  a contract violation: the worker's *own* running task counts as pending, so
  zero is unreachable. Debug builds assert; release returns `false`
  immediately instead of hanging.
- `mu_ts_wait_idle_timeout(h, 0)` is still a valid non-blocking query and
  correctly reports `false` from inside a task.
- From a worker, use `mu_ts_assist(h)` to keep draining work.

---

## mu/mu_task_scheduler.h — jobs with priorities and dependencies

Built on `mu_thread_system.h`. Adds a ready/pending task graph with
priorities, completion counters, and group waits.

### Priorities

```c
typedef enum mu_sched_priority
{
    /* high / normal / low tiers; clamped via mu_sched__clamp_priority */
} mu_sched_priority;
```

High-priority tasks jump ahead of queued low-priority work; low-priority
tasks also yield during `assist` (see below) so foreground work finishes
first.

### Task descriptor

```c
typedef void (*mu_sched_task_fn)(void* user, uint32_t worker_index);

typedef struct mu_sched_task_desc
{
    mu_sched_task_fn fn;
    void*            user;
    mu_sched_priority priority;
    mu_sched_group*   group;    /* optional completion group to signal */
} mu_sched_task_desc;
```

### Scheduler lifetime

```c
mu_task_scheduler* s;
mu_sched_init(&s, NULL);           /* starts its own thread system     */
mu_sched_exit(&s);
```

### Submitting work

```c
mu_sched_task_desc d = { .fn = job, .user = ctx, .priority = MU_SCHED_PRIORITY_NORMAL };
mu_sched_submit(&s, &d);           /* single task                      */

mu_sched_submit_tasks(&s, fn, user_array, count, user_size, priority, group);
                                   /* count tasks over a user array    */
```

### Counters and groups

```c
typedef struct mu_sched_counter mu_sched_counter;   /* atomic + condvar */
typedef mu_sched_counter mu_sched_group;

mu_sched_group_init(&g, initial);   /* counter starts at N             */
/* ... submit tasks with .group = &g ... each completion decrements   */
mu_sched_group_wait(&g);            /* block until zero                */
mu_sched_group_wait_timeout(&g, ms);
mu_sched_group_count(&g);
mu_sched_group_exit(&g);
```

Same primitives under plain `mu_sched_counter_*` names. This is the
dependency mechanism: a task can *add* to a counter for downstream work,
then another thread waits on it.

A counter remembers the first scheduler it is used with (`owner`, bound by
`mu_sched_submit` from `completion_counter` or `depends_on`). That is what
lets a worker waiting on the counter assist the right scheduler. A counter
is expected to belong to one scheduler; mixing schedulers asserts.

Counters may be stack-local. Zero is published while holding the counter
mutex, and every successful wait takes that mutex once as a fence before
returning, so `group_exit` / destruction immediately after a successful
wait is safe — including when the waiter is a worker running the last task.

### Waiting vs assisting

```c
mu_sched_wait_idle(&s);              /* block until everything drains   */
mu_sched_wait_idle_timeout(&s, ms);
mu_sched_assist(&s);                 /* don't block — run pending tasks
                                        yourself until queue empties    */
```

`assist` is the important one for frame loops: main-thread waiters execute
jobs instead of sleeping, keeping total wall time down.

`mu_sched_wait_idle` (and a nonzero timeout) cannot be used from a worker:
`inflight_count` includes the calling task, so it could never drain. Debug
builds assert; release returns `false`. Use `mu_sched_assist` there, or move
the wait to the owner thread.

### Waiting from inside a task (cooperative assist)

Blocking a worker on work that is queued *behind* that worker is the classic
job-system deadlock:

```text
worker_count = 1, task A runs on the only worker
  A: parallel_for(...)  -> submits chunk tasks
  A: waits on done      -> parks the only worker
  chunk tasks           -> never run        => hang
```

With N workers the same happens when all N tasks fork-join at once. Before,
`mu_sched_parallel_for`, `mu_sched_group_wait` and `mu_sched_counter_wait`
always parked the calling thread on a condition variable.

Now a wait behaves differently depending on who calls it:

| Caller | Behavior |
|---|---|
| owner/non-worker thread | condvar wait (unchanged) |
| worker of the counter's owner scheduler | drain tasks until the counter hits zero |
| worker of a different scheduler | condvar wait; assist would not help |

The worker path in `mu_sched_counter_wait_timeout` is:

```text
loop until value == 0:
    if mu_sched_assist(owner) -> progress, retry immediately
    if timeout expired        -> return false
    otherwise                 -> nap 2 ms on the counter's condvar
lock/unlock the counter mutex (fence against the last decrementer)
```

`mu_sched_parallel_for` gets this for free: its `done` counter is submitted
as a `completion_counter`, so calling it from a task is safe. Assisted tasks
are run by the waiting worker and receive that worker's id, so
`worker_index` is never `UINT32_MAX` merely because of assisting.

```c
static void chunk_fn(uint32_t begin, uint32_t end, uint32_t worker_index, void* user)
{
    /* worker_index is a real worker slot when a worker assists */
}

static void simulation_job(void* user, uint32_t worker_index)
{
    mu_task_scheduler* s = (mu_task_scheduler*)user;

    /* Safe from a worker: while waiting, this worker executes ready jobs. */
    mu_sched_parallel_for(s, 0, 1000, 32, MU_SCHED_PRIORITY_NORMAL, chunk_fn, NULL);
}

static void submit_simulation(mu_task_scheduler* s)
{
    mu_sched_task_desc d;
    d.fn = simulation_job;
    d.user = s;
    d.priority = MU_SCHED_PRIORITY_NORMAL;
    d.depends_on = NULL;
    d.completion_counter = NULL;
    mu_sched_submit(s, &d);
}
```

What is guaranteed:

- Acyclic fork-join makes progress, including `worker_count == 1` and
  nested `parallel_for`/group waits.
- A worker that waits runs other ready tasks instead of sleeping, so a small
  pool does not lose a slot for the duration of the wait.

What is *not* guaranteed:

- Cyclic waits still deadlock (task A waits on B, B waits on A). Fibers do
  not fix that either.
- Waits fed by a different scheduler or an external thread can only nap in
  2 ms slices. If the producer expects the blocked worker to run something,
  move the wait to the owner thread or restructure into dependencies.
- There are no stackful fibers. Arbitrary blocking inside a task (OS mutex,
  blocking I/O) still parks a worker. Games should submit that work as a
  chain (`depends_on`) instead.

### parallel_for

Range-parallel helper that splits `[begin, end)` across workers:

```c
typedef void (*mu_sched_parallel_for_fn)(uint32_t begin, uint32_t end, uint32_t worker_index, void* user);

mu_sched_parallel_for(&s, begin, end, fn, user /*, min batch */);
```

The range is chunked; each chunk runs on some worker with its exclusive
`[begin, end)` slice and the executing worker's index for scratch arrays.
It is safe to call from a scheduler task: the wait uses the cooperative
assist path described above. The call still blocks the *calling* thread in
the sense that it does not return until the range is done, but a worker
caller keeps executing other ready tasks meanwhile.

### Practical notes

- Task `user` data must stay alive until the task runs (or the group it
  belongs to completes) — the scheduler copies pointers, not buffers.
- Priorities affect *submission order*, not preemption: a running low-pri
  task is never interrupted.
- All wait functions have `_timeout` variants; use them in interactive
  paths so a stuck task can't freeze your frame loop forever.

### Tests

`test.cpp` covers the cooperative-wait contract with six reentrancy cases
(each run under `-pthread`):

| Case | What it proves |
|---|---|
| `parallel_for inside task (1 worker)` | the single-worker fork-join deadlock is gone |
| `parallel_for inside task (2 workers)` | assist works with a small pool |
| `nested parallel_for (1/2 workers)` | two levels of fork-join make progress |
| `group wait inside task (1/2 workers)` | `mu_sched_group_wait` drains via assist |

The same cases also check that `worker_index != UINT32_MAX` inside assisted
ranges, that `mu_ts_is_worker_thread` is true in tasks, and that a
non-blocking `mu_sched_wait_idle_timeout(s, 0)` reports false from a task.
Build with `-pthread`:

```sh
c++ -std=c++17 -O2 -D_POSIX_C_SOURCE=199309L -pthread test.cpp mu.c -o test_cpp
```

The suite is verified clean under AddressSanitizer, UndefinedBehaviorSanitizer
and ThreadSanitizer; the TSan run is what caught the counter lifetime race
that the mutex fence above fixes.
