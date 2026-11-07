# `mu_worklist.h` — unique double-ended work queue

A bounded deque of integer indices in `[0, capacity)` that also deduplicates:
pushing a value that is already queued is a no-op. Designed for graph walks,
streaming updates and job systems that need to take work from either end
without scanning a "visited" array.

```c
#include "mu.h"

mu_worklist w;
mu_worklist_init(&w, 1024);

mu_worklist_push_tail(&w, 7);
mu_worklist_push_head(&w, 3);

uint32_t id;
while (mu_worklist_pop_head(&w, &id))
    process(id);

mu_worklist_destroy(&w);
```

---

## Layout

```
entries (ring):
    start -> [ 7 | 2 | 9 | 4 | . | . | . | . ]
              ^                 ^
              head              tail

present (bitset):
    bit v set <=> value v is currently in the queue
```

- `entries[capacity]` stores the live indices contiguously in ring order.
- `present[(capacity + 63) / 64]` makes membership O(1) and avoids duplicate
  entries without touching the ring.
- Nothing is allocated after `init`; the static form uses caller-owned arrays.

## API

```c
bool mu_worklist_init(mu_worklist* w, uint32_t capacity);
bool mu_worklist_init_static(mu_worklist* w,
                             uint32_t* entries,
                             uint64_t* present,
                             uint32_t capacity);
void mu_worklist_destroy(mu_worklist* w);   /* frees only owned memory */
void mu_worklist_clear(mu_worklist* w);     /* reset to empty, keep storage */

bool mu_worklist_push_head(mu_worklist* w, uint32_t value);
bool mu_worklist_push_tail(mu_worklist* w, uint32_t value);
bool mu_worklist_pop_head(mu_worklist* w, uint32_t* out_value);
bool mu_worklist_pop_tail(mu_worklist* w, uint32_t* out_value);
bool mu_worklist_peek_head(const mu_worklist* w, uint32_t fallback);
bool mu_worklist_peek_tail(const mu_worklist* w, uint32_t fallback);
uint32_t mu_worklist_at(const mu_worklist* w, uint32_t offset);
bool mu_worklist_contains(const mu_worklist* w, uint32_t value);
```

Queries: `count`, `capacity`, `empty`, `full`.

### Return values

`push_head` / `push_tail` return `false` when:

- `value >= capacity` (illegal id),
- the value is already present, or
- the queue is full.

`pop_head` / `pop_tail` return `false` only when the queue is empty.

### Static storage

`present` must hold `(capacity + 63) / 64` words. It is cleared by
`init_static`, so the caller does not need to zero it first:

```c
static uint32_t entries[256];
static uint64_t present[(256 + 63) / 64];
mu_worklist w;
mu_worklist_init_static(&w, entries, present, 256);
```

`destroy` is still safe on a static worklist; it only frees memory when the
worklist was created with `mu_worklist_init`.

## Choosing head vs tail

- Use **tail / head** for BFS (FIFO).
- Use **tail / tail** for DFS (LIFO).
- Use **head / head** if new urgent work should be processed next.

All four directions are O(1) and deduplicate on insertion.

## Limits

- Capacity is fixed. Size it from the number of possible ids, not the number
  of currently queued items.
- Not thread-safe; use one worklist per producer/consumer or an external lock.
- Values are indices only. Store payloads in a separate array and push the
  index.
