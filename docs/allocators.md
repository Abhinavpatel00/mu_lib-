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

### `mu_arena` — linear bump allocator, two-ended

```c
typedef struct { uint8_t* base; uint32_t capacity; uint32_t used; uint32_t tail; } mu_arena;

void*    mu_arena_alloc(mu_arena* a, uint32_t size, uint32_t align);      /* front */
void*    mu_arena_alloc_top(mu_arena* a, uint32_t size, uint32_t align);  /* back  */
uint32_t mu_arena_top_mark(const mu_arena* a);          /* save the back cursor */
void     mu_arena_top_rewind(mu_arena* a, uint32_t mark); /* drop everything above it */
uint32_t mu_arena_available(const mu_arena* a);          /* free middle, in bytes */
void     mu_arena_reset(mu_arena* a);                    /* both cursors to 0 */
```

- Bump `used` upward with alignment; `NULL` when out of space.
- No per-allocation free — reclaim with `mu_arena_reset`.
- Perfect for per-frame, per-job, or load-phase scratch memory.
- **Two cursors, not one.** `used` grows up from the front and `tail` counts
  bytes reserved at the *end*; the invariant is `used + tail <= capacity`. A
  temporary buffer taken with `mu_arena_alloc_top` therefore cannot move the
  frame cursor or fragment what is already there — the classic "my scratch
  buffer ate my frame arena" failure. `mu_arena_top_mark` /
  `mu_arena_top_rewind` give you a nested scratchpad in two words.
- There is no `mu_arena_init`; zero the struct and set `base`/`capacity`. A
  garbage `tail` is not a safety problem (readers treat `tail > capacity` as
  "no room at the back" and refuse) but it does make the arena look full.
- The alignment gap above a top allocation (at most `align - 1` bytes) stays
  reserved until the matching rewind; that is the price of tracking the back
  with one counter. `perf_test/bench_allocators.c` measures the reversed cursor
  at 2.128 ns/op against 2.126 ns for the forward one — nothing.

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
Free blocks at each level are kept in a **doubly linked** freelist threaded
through per-block `free_next` / `free_prev` arrays.

**Metadata.** Sized byte-for-byte by `mu_buddy_metadata_size`, which you must
supply in `metadata`:

| Array | Purpose |
|---|---|
| `free_head[num_levels]` | per-level freelist heads |
| `free_next[num_blocks]` | freelist next links |
| `free_prev[num_blocks]` | freelist prev links — coalescing removes a block from the *middle* of a list, and only the prev link makes that O(1) |
| `split_map` (bitset) | internal node is split? |
| `merge_xor_map` (bitset) | sibling pair "exactly one of the two is free" |

That is `levels*4 + num_blocks*8 + bits` bytes; for a 4 MiB arena with 32-byte
leaves it is ~2 MiB of metadata, which is why the leaf size is a real
memory/speed trade and not a detail.

**The invariant.** `merge_xor_map` has one bit per **sibling pair**, indexed by
the pair's *parent* index — children are `2i+1` and `2i+2`, so `index/2` maps
them to two *different* bits and stalls half of all merges. The bit means
"exactly one of the two siblings is in this level's freelist", and it is
maintained from **both** sides:

* on alloc, the pair we took a block from has zero free siblings left → clear;
* on a fresh split, only the right child is pushed and the left is handed
  straight to the caller, so "exactly one free" is true by construction;
* on free, if the bit is 0 the sibling is allocated → set the bit and park the
  block; if it is 1 the sibling is free and in the list → unlink it, clear the
  bit, clear the parent's split bit, and merge at the parent level;
* on merge the bit is cleared, because the merged parent is now the free thing.

**Alloc.** Round `size` up to pow2 (min `leaf_size`, honoring `align`), find
its level, pop a free block. If none, walk up to the nearest ancestor that has a
free block, pop it, and split down: set `split_map` on each node split, push the
right child, carry the left one down. Offset in the arena is
`index_in_level * level_block_size`.

**Free.** Walk the tree down from the root using `split_map` to find the
allocation's level (that's what `mu_buddy_free` does; `mu_buddy_free_known`
skips the walk if you remembered the size), then merge upward as above.

**What the previous version got wrong, and why it matters.** The parity bit was
*toggled* on free and never touched on alloc. Toggling encodes "exactly one
free" only if a pair strictly alternates free/alloc — i.e. only if blocks are
freed in the reverse of the allocation order. Reuse a freed block and free it
again, or free every other block, and the bit says 1 while the sibling is
allocated: the merge ran anyway, and the merged parent went onto the freelist on
top of a live child. Two live allocations came out of the same bytes — measured
as 16396 duplicate pointers in a 32768-block arena, with no crash to point at
it, because both users just wrote to the same memory. The regression is now
pinned by `perf_test/test_allocators.c` with a byte-level owner map, which
catches aliasing whatever the free order; a test that only allocates, or only
frees in LIFO order, would have kept it hidden forever.

**Tradeoffs.** O(1) alloc from a non-empty level (O(levels) worst case when it
has to split down), O(1) free including the merge, no external fragmentation
beyond pow2 rounding, but pow2-only granularity: a 1.1 MB request costs a 2 MB
block, so internal overhead is up to ~2x for awkward sizes. Alignment is
derived from the arena base, so the arena has to be aligned to the largest
`align` you intend to ask for.

**Gate and numbers.** `make -C perf_test alloc-test` (10 148 checks: capacity,
free-all-then-refill, free-every-other, a 20 000-step random model against a
byte owner map, alignment, `free_known`, and the unlink-vs-scan equivalence that
makes the bench's `_scan` baseline a fair one), `make -C perf_test alloc-bench`,
and `perf_test/PERF_ALLOCATORS.md` for the measured cost of each path.

What the bench says, since it is not what the design notes implied: the double
link is **not** a speedup in LIFO free order (65.9 ns/op unlinked vs 61.6 ns/op
scanned at a 256-entry freelist — no difference inside the CI, and `free_prev`
costs 4 B per leaf on every push and pop). It pays 2.0x at a 256-entry freelist
and 5.1x at 1024 **only** when the free order does not match the park order, and
the extra cost is 2.1 ns per freelist step, the same at both lengths. The reason
to have it is correctness — a merge has to remove the sibling from wherever it
sits, and doing that by scan is O(n) — not the fast path. `mu_pool` remains
~10x cheaper per operation (6.3 ns alloc+free) and `mu_arena` ~33x (2.0 ns), so
the buddy is for generality, not for churn.

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

#### Two things to know before you trust it

- **The node budget is the budget that binds first.** Every split needs a
  metadata node, so an arena with plenty of bytes can still refuse an
  allocation because `max_allocs` ran out; an aligned request that has to skip
  a leading gap needs *two*. `oa_storage_report` tells you the truth about the
  bytes (it used to report 0 as soon as `free_offset` hit 0 — fixed, and the
  case is now gate-locked in `perf_test/test_offset.c`).
- **Alignment quality is not tracked.** `oa_allocate_aligned` returns the first
  region it can align into, not the one whose start is already aligned. Making
  the scan prefer the latter was implemented, measured, and does not pay — the
  bins are size-indexed, so a bin's nodes differ by at most 12.5% and the
  first fit is already the aligned one whenever one exists. The knob
  (`OA_ALIGN_LOOKAHEAD`) defaults to 0 and exists so the experiment stays
  reproducible: `perf_test/PERF_ALLOCATORS.md` §5, `make offset-ab`.
- Naming note: this header is a port of Sebastian Aaltonen's OffsetAllocator
  and keeps its `oa_*` symbols and `mu_offset_allocator.{h,c}` file name; the
  rest of the library uses `mu_*` because the rule is "reference, not vendor".
  Renaming this one is mechanical and not yet done.

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
