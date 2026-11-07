# `mu_slab_manager.h` — multi-heap reclaimable slab manager

`mu_slab` handles one fixed object size. `mu_slab_manager` handles many sizes
and multiple opaque heaps, and it delays reuse until a caller-provided
`can_reclaim` callback says the backing storage is safe.

Typical use: GPU buffer suballocation, descriptor pools, staging buffers, and
command pools. Free an entry when the CPU is done; reclaim it only after the
GPU fence has passed.

```c
typedef struct GpuEntry
{
    mu_slab_manager_entry entry;
    bool fence_passed;
} GpuEntry;
```

## Callbacks

The manager does not allocate entries or slabs. You provide three callbacks:

```c
mu_slab_manager_slab* alloc_slab(void* priv, unsigned heap,
                                 unsigned entry_size, unsigned group_index);
void free_slab(void* priv, mu_slab_manager_slab* slab);
bool can_reclaim(void* priv, mu_slab_manager_entry* entry);
```

Inside `alloc_slab`, initialize the slab and add every entry:

```c
mu_slab_manager_slab_init(slab, group_index, entry_size, count);
for (unsigned i = 0; i < count; ++i)
    mu_slab_manager_slab_add_entry(slab, &entries[i].entry);
```

The manager calls `can_reclaim` under its lock. Do not call back into the
manager from that callback. `alloc_slab` is called without the lock, so it may
allocate memory; `free_slab` must not call manager functions.

## API

```c
bool mu_slab_manager_init(mu_slab_manager* mgr,
                          unsigned min_order, unsigned max_order,
                          unsigned num_heaps, bool allow_three_fourths,
                          void* priv,
                          mu_slab_manager_can_reclaim_fn can_reclaim,
                          mu_slab_manager_alloc_fn slab_alloc,
                          mu_slab_manager_free_fn slab_free);

mu_slab_manager_entry* mu_slab_manager_alloc(mu_slab_manager* mgr,
                                             unsigned size, unsigned heap);
mu_slab_manager_entry* mu_slab_manager_alloc_reclaimed(mu_slab_manager* mgr,
                                                       unsigned size, unsigned heap,
                                                       bool reclaim_all);
void mu_slab_manager_free(mu_slab_manager* mgr, mu_slab_manager_entry* entry);
unsigned mu_slab_manager_reclaim(mu_slab_manager* mgr);
void mu_slab_manager_deinit(mu_slab_manager* mgr);
```

Sizes are rounded up to a power of two between `2^min_order` and
`2^max_order`. With `allow_three_fourths`, requests at or below 75% of a size
class use a 3/4-size slab, reducing internal fragmentation.

`heap` is an opaque index in `[0, num_heaps)`.

## Reclaim model

```
free(entry) -> reclaim list
                  |
          can_reclaim()?
             /          \
           yes           no
            |             |
     slab free list   stay queued
```

- `mu_slab_manager_free` only enqueues. It does not make the entry reusable.
- `mu_slab_manager_alloc` automatically tries to reclaim when the selected
  group has no free entries.
- `mu_slab_manager_reclaim` forces a scan and returns the number reclaimed.
- A slab is returned to `free_slab` when all its entries are free.

## Threading

The manager has one internal spinlock. Different heaps and size classes still
share that lock. Keep callbacks short. `mu_slab_manager_alloc` drops the lock
while calling `slab_alloc` to avoid deadlocks.

## Limits

- Caller owns node/slab memory.
- Free every live entry before `mu_slab_manager_deinit`; entries still held by
  callers are not discovered or freed.
- Do not free an entry twice.
- `max_order` must be less than 31.
- Not suitable for huge numbers of size classes; each class/heap consumes a
  group descriptor.

Ported from Mesa `util/pb_slab.{h,c}` (MIT).
