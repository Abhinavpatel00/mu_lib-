# `mu_slab.h` — fixed-size block allocator with per-thread caches

A slab allocator for one object size. The parent owns pages and the global free
list; each child caches a batch of free blocks locally so the hot alloc/free
path does not touch a lock.

```c
#include "mu.h"

mu_slab_parent parent;
mu_slab_parent_init(&parent, sizeof(Command), 256);

mu_slab_child local;
mu_slab_child_init(&local, &parent);

Command* c = (Command*)mu_slab_zalloc(&local);
if (c)
{
    c->kind = CMD_MOVE;
    /* ... */
    mu_slab_free(&local, c);
}

mu_slab_child_destroy(&local);
mu_slab_parent_destroy(&parent);
```

Best for nodes, components, command packets and other objects with the same
size and high churn. Use a general-purpose allocator when sizes vary or
allocation frequency is low.

---

## Layout

```
mu_slab_parent
  pages      -> [ page | item 0 | item 1 | ... | item N-1 ]
                    |
                    +-> next page

  free_list  -> item -> item -> ...

mu_slab_child
  free_list  -> item -> item -> ...     (thread-local cache)
```

Each item:

```
+----------------------+--------------------------------+
| next pointer + pad   | user payload (elem_size bytes) |
+----------------------+--------------------------------+
  16-byte header                 user pointer
```

- The 16-byte header keeps the user pointer 16-byte aligned.
- `item_size` is `align_up(16 + elem_size, 16)`, so every item in a page has
  the same alignment.
- Pages are aligned up from a `malloc` block; the original pointer is stored
  in `page->raw` so teardown can free it correctly.
- Free items are threaded through the header's `next` pointer. No per-item
  allocation metadata exists outside the item itself.

## API

```c
bool mu_slab_parent_init(mu_slab_parent* parent,
                         size_t elem_size,
                         uint32_t items_per_page);
void mu_slab_parent_destroy(mu_slab_parent* parent);

bool mu_slab_child_init(mu_slab_child* child, mu_slab_parent* parent);
bool mu_slab_child_init_batch(mu_slab_child* child,
                              mu_slab_parent* parent,
                              uint32_t batch);
void mu_slab_child_destroy(mu_slab_child* child);

void* mu_slab_alloc(mu_slab_child* child);
void* mu_slab_zalloc(mu_slab_child* child);

void mu_slab_free(mu_slab_child* child, void* ptr);
void mu_slab_free_shared(mu_slab_parent* parent, void* ptr);
```

`mu_slab_child_init` uses a default batch of 16, clamped to
`items_per_page`. `mu_slab_child_init_batch` lets you tune how many blocks a
child pulls from the parent at once.

`mu_slab_zalloc` zeroes `elem_size` bytes after allocation.

## Threading model

- A `mu_slab_child` is **not thread-safe**. One thread at a time per child.
- Different children are independent. They meet only under the parent
  spinlock when refilling, returning blocks, or shutting down.
- `mu_slab_free_shared` may be called from any thread. Use it for objects
  retired by IO threads, job threads without a child, or deferred cleanup.
- `mu_slab_parent_destroy` must not race with any child. Destroy children
  first, then the parent.

The allocator does not track which child allocated a block. Freeing through a
different child is safe as long as neither child is being used concurrently by
two threads; the block simply migrates to the new child's cache.

## Tuning

| Knob | Effect |
|------|--------|
| `elem_size` | fixed payload size; overhead is 16 bytes + alignment padding |
| `items_per_page` | page granularity; larger = fewer trips to the parent |
| child `batch` | blocks cached per refill; larger = fewer parent locks, more idle memory per thread |

Rules of thumb:

- `items_per_page` around 64–512 for small objects.
- child `batch` around 8–64. Too small thrashes the parent lock; too large
  strands memory in threads that no longer allocate.
- If a child stops allocating, call `mu_slab_child_destroy` to return its
  cache to the parent.

## Limits

- One object size per parent.
- No shrinking of pages; `parent_destroy` frees them all.
- Double frees corrupt the free list (as with any freelist allocator).
- Pages are never returned to the OS until the parent is destroyed.
