# `mu_deferred_free.h` — frame-delayed retirement queue

A small ring of pointer buckets that frees objects a fixed number of
`advance()` calls after they are pushed. Useful when a renderer, worker or
async submit may still hold a raw pointer for another frame.

```c
#include "mu.h"

static void release_command(void* ptr, void* user)
{
    mu_slab_parent* slab = (mu_slab_parent*)user;
    mu_slab_free_shared(slab, ptr);
}

mu_deferred_free retired;
mu_deferred_free_init(&retired, 3, 256, release_command, &command_slab);

/* simulation drops an object */
mu_deferred_free_push(&retired, cmd);

/* once per frame */
mu_deferred_free_advance(&retired);

/* shutdown */
mu_deferred_free_destroy(&retired);
```

With `frames = 3`, a pointer pushed during frame 0 is freed on frame 3.

---

## Layout

```
cursor
  |
  v
[ bucket k ][ bucket k+1 ][ ... ][ bucket k+N-1 ]
  pushed       older                freed next
```

Each bucket holds up to `capacity` pointers. `push` writes to the current
bucket. `advance` moves the cursor forward and frees the bucket it lands on, so
a pointer survives exactly `frames` advances.

For `frames = 3`:

```
push on frame 0        -> bucket 0
advance to frame 1     -> frees bucket 1 (empty)
advance to frame 2     -> frees bucket 2 (empty)
advance to frame 3     -> frees bucket 0 (the object)
```

## API

```c
typedef void (*mu_deferred_free_fn)(void* ptr, void* user);

bool mu_deferred_free_init(mu_deferred_free* q,
                           uint32_t frames,
                           uint32_t capacity,
                           mu_deferred_free_fn free_fn,
                           void* user);

bool mu_deferred_free_push(mu_deferred_free* q, void* ptr);
void mu_deferred_free_advance(mu_deferred_free* q);
void mu_deferred_free_drain(mu_deferred_free* q);
void mu_deferred_free_destroy(mu_deferred_free* q);

uint32_t mu_deferred_free_pending(const mu_deferred_free* q);
```

- `free_fn == NULL` means `MU_FREE(ptr)`.
- `push` returns `false` when the current bucket is full. The caller decides
  whether to free immediately, drop, or grow `capacity`.
- `advance` frees the bucket that just became current.
- `drain` frees everything immediately; use at a known hard safe point or
  shutdown.
- `destroy` drains all buckets, then frees the queue's own arrays.
- `pending` reports the total number of queued pointers.

## Threading

The queue is **not internally synchronised**. Typical patterns:

- one simulation thread pushes, the frame boundary advances;
- or guard it with an external lock if producers are multiple threads.

For a lock-free single-producer/single-consumer handoff, use `mu_spsc_queue.h`
and drain into a deferred-free queue at the safe point.

## Choosing `frames`

- `frames = 1` releases on the next `advance`.
- `frames = 2` covers "one frame in flight plus the current frame".
- `frames = 3` is a common default for CPU→GPU style pipelines with one frame
  of buffering.

The queue does not know about GPU fences or async completion. If a resource
can be used for longer than `frames` advances, either increase `frames` or
retire it only after the fence is known to have completed.

## Failure behaviour

`init` allocates `frames * capacity` pointers plus one count per frame. It
returns `false` on allocation failure or integer overflow. A failed `push`
leaves the object untouched; nothing is silently dropped.
