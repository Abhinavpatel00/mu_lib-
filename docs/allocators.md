# Allocators: `mu/allocators.h`, `mu/offset_allocator.h`, `mu/mu_mmgr.h`

Three layers of memory management, from "which block do I hand out" to "who
leaked what, where, and when."

---

## mu/allocators.h — arena / pool / buddy

C99 single-header collection. Define `MU_ALLOCATORS_IMPLEMENTATION` in exactly
one TU to get the function bodies.

### Shared utilities

```c
mu_align_up_u32(x, a)        mu_align_up_ptr(x, a)
mu_is_pow2_u32(x)            mu_next_pow2_u32(x)   /* 0->1, 5->8, 16->16 */
```

### `mu_arena` — linear bump allocator

```c
typedef struct { uint8_t* base; uint32_t capacity; uint32_t used; } mu_arena;

void* mu_arena_alloc(mu_arena* a, uint32_t size, uint32_t align);
void  mu_arena_reset(mu_arena* a);
```

- Bump `used` upward with alignment; `NULL` when out of space.
- No per-allocation free — reclaim with `mu_arena_reset`.
- Perfect for per-frame, per-job, or load-phase scratch memory.

### `mu_pool` — fixed-size block pool

```c
int   mu_pool_init(mu_pool* p, void* buffer, uint32_t block_size, uint32_t block_count);
void* mu_pool_alloc(mu_pool* p);   /* NULL when exhausted */
void  mu_pool_free(mu_pool* p, void* ptr);
```

- Caller owns `buffer`; it must be `block_size * block_count` bytes and
  `block_size >= sizeof(uint32_t)`.
- Free blocks are chained **through their own memory** (intrusive freelist) —
  zero metadata overhead, O(1) alloc/free.
- Any-size allocation (buddy/arena) or same-size churn (pool) — the classic
  trade: pool gives O(1) and zero fragmentation for one stride.

### `mu_buddy` — power-of-two buddy allocator

For when you need many sizes from one arena, with automatic coalescing.

```c
uint32_t mu_buddy_metadata_size(uint32_t total_size, uint32_t leaf_size);
int      mu_buddy_init(mu_buddy* b, void* arena, uint32_t arena_size,
                       uint32_t leaf_size, void* metadata, uint32_t metadata_size);
void*    mu_buddy_alloc(mu_buddy* b, uint32_t size, uint32_t align);
void     mu_buddy_free_known(mu_buddy* b, void* ptr, uint32_t size);
void     mu_buddy_free(mu_buddy* b, void* ptr);
```

**Layout.** The arena is a complete binary tree of blocks: level 0 is one
block of `total_size`, each level halves the block size down to `leaf_size`.
Free blocks at each level are kept in an intrusive freelist threaded through a
per-block `free_next` array.

**Metadata.** Sized by `mu_buddy_metadata_size`, which you must supply in
`metadata`:

| Array | Purpose |
|---|---|
| `free_head[num_levels]` | per-level freelist heads |
| `free_next[num_blocks]` | intrusive freelist links |
| `split_map` (bitset) | internal node is split? |
| `merge_xor_map` (bitset) | buddy-pair "exactly one free" parity |

**Alloc.** Round `size` up to pow2 (min `leaf_size`, honoring `align`), find
its level, pop a free block. If none, recurse up to split the smallest
available ancestor: set `split_map` on the parent, push both children. Offset
in the arena is `index_in_level * level_block_size`.

**Free.** Walk the tree down from the root using `split_map` to find the
allocation's level (that's what `mu_buddy_free` does; `mu_buddy_free_known`
skips the walk if you remembered the size). Then merge upward: flip the buddy
pair's parity bit, and when both buddies are free, remove the buddy from the
freelist, clear the parent's split bit, and continue at the parent level.

**Tradeoffs.** Fast (O(log n) alloc, amortized O(log n) free with merge),
zero external fragmentation beyond pow2 rounding, but pow2-only granularity:
a 1.1 MB request costs a 2 MB block. Internal overhead per allocation is up
to ~2x for awkward sizes.

---

## mu/offset_allocator.h — two-level bin allocator (inspired by Sebastian Aaltonen offset allocator)

General-purpose sub-allocator with **waste-free size classes** and O(1)
alloc/free. Used for GPU-style allocations where you need `void`-free
placement, hundreds of thousands of live allocations, and no fragmentation
death spiral. C99 port; define `USE_16_BIT_NODE_INDICES` to halve node memory
below 65536 nodes.

### Model

- Free space is classified into 256 size classes: 32 top bins × 8 leaf bins.
- A top-bin bitmap (`used_bins_top`) + per-top-bin leaf bitmaps let
  "find smallest class with a free slot" run in a couple of bit instructions.
- Allocations live in `OA_Node`s doubly linked into per-class bin lists and
  into a physical neighbor list (for adjacent-free coalescing on free).

### API

```c
oa_init(&alloc, size, max_allocs);            // owns node arrays
OA_Allocation a = oa_allocate(&alloc, size);  // .offset, .metadata
a = oa_allocate_aligned(&alloc, size, alignment);  // round-up placement
oa_free(&alloc, a);
oa_allocation_size(&alloc, a);                // recover size from handle
oa_storage_report(&alloc);                    // { total_free, largest_free }
oa_storage_report_full(&alloc);               // per-bin { size, count }
oa_reset(&alloc);                             // release everything
oa_debug_validate(&alloc);                    // DEBUG builds only
```

- `OA_Allocation.metadata` is opaque — pass it back to `oa_free` /
  `oa_allocation_size` unchanged. `OA_NO_SPACE` (`0xffffffff`) signals
  allocation failure.
- You allocate the backing memory yourself at the returned offsets; the
  allocator only tracks placement.

### When to use which allocator

| Need | Use |
|---|---|
| Per-frame scratch, reset-all | `mu_arena` / `mu_linear_allocator` |
| One stride, infinite churn | `mu_pool` |
| Many sizes, auto coalesce, one arena | `mu_buddy` |
| Many sizes, huge counts, GPU-style | `offset_allocator` |

---

## mu/mu_mmgr.h — memory manager / leak tracker

Standalone C99 memory tracker inspired by fluid studio memory manager.
Wraps every allocation with padding guards, source location, optional
stack trace, and a leak report. Define `MU_MMGR_IMPLEMENTATION` in one TU.

### Usage

```c
#define MU_MMGR_IMPLEMENTATION
#include "mu/mu_mmgr.h"

int main(void)
{
    mu_mmgr_init("my_app");          // start tracking
    void* p = MU_MMGR_MALLOC(256);
    p = MU_MMGR_REALLOC(p, 512);
    MU_MMGR_FREE(p);
    mu_mmgr_shutdown();              // writes <app>.memleaks, asserts on leaks
    return 0;
}
```

### What you get

- **Padding guards.** 4 uint32 of `0xbaadf00d` before and `0xdeadc0de` after
  each allocation; validated on every free (and optionally on every
  allocation) to catch buffer over/underruns at the exact guilty free, not
  at crash time.
- **Full bookkeeping.** Every allocation records file/line/function, type
  (`malloc`/`calloc`/`realloc`/`new`/...), a sequence number, and (on
  glibc/Win32) a backtrace of up to `MU_MMGR_BACKTRACE_SIZE` frames.
- **Leak report.** `mu_mmgr_shutdown()` writes `my_app.memleaks` listing each
  leak with its allocation site and stack, then asserts if
  `MU_MMGR_ASSERT_ON_LEAK` is 1.
- **Statistics.** `mu_mmgr_stats_get()` returns `mu_mmgr_memory_stats`:
  current/peak/accumulated reported-vs-actual memory and allocation counts.
  `mu_mmgr_dump_memory_report()` writes a full snapshot to file.
- **Debug knobs.**

  | Knob | Effect |
  |---|---|
  | `mu_mmgr_always_validate_all()` | validate guard bytes on every operation |
  | `mu_mmgr_always_log_all()` | log every alloc/free to an in-memory log |
  | `mu_mmgr_always_wipe_all()` / `mu_mmgr_random_wipe()` | fill new memory with `0xfeedface` |
  | `mu_mmgr_break_on_allocation(n)` | break on the n-th allocation |
  | `mu_mmgr_break_on_dealloc(ptr)` / `mu_mmgr_break_on_realloc(ptr)` | break on that pointer |
  | `MU_MMGR_STRESS_TEST` define | enable all of the above at once |

### Costs & cautions

- Every allocation is padded by ~32 bytes and tracked in a hash table with a
  mutex; this is a **debug/build-verification** tool, not a shipping heap.
- Thread-safe (two internal mutexes), but re-entry from allocator callbacks
  within the tracker itself will deadlock.
- Route the rest of the library through it by overriding `mu_malloc` /
  `mu_free` (see [core.md](core.md)) with `MU_MMGR_MALLOC` / `MU_MMGR_FREE`.
