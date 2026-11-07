# Containers: arrays, spans, strings, intrusive lists

Everything here is either a header-prefixed dynamic array, a non-owning view,
or an index-based utility for data stored elsewhere. None of it inserts
hidden allocations behind virtual calls; allocation points are always visible
in your code.

---

## mu_array.h — `array` (stretchy buffer, header-prefixed)

The workhorse dynamic array. A plain `T*` that hides a header right before
the data, so any function accepting `T*` keeps accepting it.

```c
typedef struct { uint32_t size; uint32_t capacity; uint32_t _pad0; uint32_t _pad1; } array_header_t;
```

### Layout

```
USER POINTER (a)
        |
        v
+---------------------+-----------------------+
| header              | actual data           |
| size | capacity     | a[0] a[1] a[2] ...    |
+---------------------+-----------------------+
        ^
        |
   (a - sizeof(array_header_t))
```

### API

```c
float* a = NULL;                       /* empty; allocates on first push */

array_push(a, 1.0f);                   /* grow x2 (start 16) as needed   */
array_push(a, 2.0f);

array_size(a)                          /* element count (0 if NULL)      */
array_capacity(a)                      /* allocated slots                */
array_full(a)                          /* size >= capacity               */
array_back(a)                          /* last element                   */
array_pop(a)                           /* size-- (does NOT destroy data) */
array_clear(a)                         /* size = 0, keeps memory         */
array_reserve(a, n)                    /* ensure capacity >= n           */
array_free(a)                          /* frees, sets a = NULL           */
```

### Semantics and gotchas

- **All macros evaluate `a` multiple times.** Don't call with expressions
  like `array_push(foo(), x)`.
- `NULL` is a valid empty array everywhere: `array_size(NULL) == 0`,
  `array_push` on `NULL` allocates.
- `array_free` assigns into the argument, so it must be an lvalue.
- Growth doubles from 16; `array_reserve` bypasses doubling to exactly `n`.
- Passing `a` to a function that pushes copies the pointer: reassignments
  inside the callee are invisible to the caller. Pass `T**` or return the new
  pointer.

Pairs naturally with `mu_span` (below) for read-only plumbing: build/own with
`array_*`, hand out `mu_span` views to consumers.

---

## mu_span.h — non-owning views

A span is a `(pointer, count)` fat pointer over memory owned by someone else.
No allocation, no ownership, no free. Zero-copy plumbing between systems.

```c
typedef struct { void* data; uint32_t count; } mu_span;  /* count = BYTES */

typedef struct { float* data; uint32_t count; } mu_span_f32;  /* count = ELEMENTS */
/* also: u8 u16 u32 u64 i8 i16 i32 i64 f64 size ptr */
```

**The one rule:** plain `mu_span` counts **bytes**; typed aliases count
**elements**. Keeping both in one header forces the distinction to be spelled
out at every call site.

### Construction

```c
float f[4] = {1,2,3,4};

mu_span_f32 s = mu_span_f32_make(f, 4);        /* ptr + count            */
mu_span_f32 s = mu_span_f32_from_array(f);     /* whole C array          */
mu_span bytes = mu_span_from_array(f);         /* whole array, in bytes  */
mu_span bytes = mu_span_from_mu_array(arr);    /* mu array, in bytes     */
mu_span_f32 e = mu_span_f32_make(NULL, 0);     /* empty; valid           */
```

`*_from_array` are macros on purpose: element counts must be computed where
the real array type is visible (a `T*` parameter would silently decay).

### Slicing (clamping, never asserting)

```c
/*  index:  0    1    2    3    4    5
    data:  [ a ][ b ][ c ][ d ][ e ][ f ]          */

mu_span_f32_sub(s, 1, 3)    /* [ b ][ c ][ d ]                    */
mu_span_f32_sub(s, 4, 99)   /* [ e ][ f ]   count clamped         */
mu_span_f32_sub(s, 99, 1)   /* empty        offset past end       */
mu_span_f32_first(s, 2)     /* [ a ][ b ]                         */
mu_span_f32_last(s, 2)      /* [ e ][ f ]                         */

mu_span_slice(bytes, 8, 8)  /* byte view: [8, 16)                 */
```

Out-of-range requests produce empty/shortened spans instead of traps, so
callers can pass lengths derived from untrusted data safely.

### Queries, search, copy

```c
mu_span_f32_count(s)             /* element count                    */
mu_span_f32_empty(s)             /* count == 0                       */
mu_span_f32_find(s, v)           /* first index or -1 (linear scan)  */
mu_span_f32_contains(s, v)       /* find >= 0                        */
mu_span_f32_copy_to(s, dst, n)   /* memmove into your buffer, clamped,
                                    returns elements written         */
mu_span_equal(a, b)              /* byte view: same len + memcmp==0  */
```

`copy_to` is overlap-safe (`memmove`) and clamps to
`min(src_count, dst_count)` — convenient for bounded-buffer dumps.

### Ownership contract

A span is a borrow. It is valid until the backing storage is freed or
realloc'd:

```c
mu_span_f32 bad = ...span over arr...;
array_push(arr, x);            /* may realloc!            */
/* bad.data may now dangle */
```

Same lifetime rules as raw pointers — a span just carries the length with it.

### Custom element types

```c
MU_SPAN_OF(struct Vertex, vertex_span);        /* the struct        */
MU_SPAN_IMPL(struct Vertex, vertex_span);      /* the helper family */
```

---

## mu_string.h — packed strings + intrusive index lists

Three small primitives for engine plumbing. `MU_INVALID_INDEX`
(`UINT32_MAX`) is the universal "no such thing" sentinel.

### `mu_string_arena` — packed string storage with stable indices

All strings live end-to-end in one byte buffer; an offsets array maps
indices to string starts. Indices (not pointers) are the stable identity —
the whole arena serializes as two flat arrays.

```
   offsets[]                  data[]
+----------+              +---+---+---+---+---+---+---+---+
|    0     | -----------> | p | l | a | y | e | r | 0 | e |
+----------+              +---+---+---+---+---+---+---+---+
|    7     | --------------------------------------------^
+----------+                                              |
|   13     | ----------------------------------------+    |
+----------+                                         |    |
                                                     v    v
                                                  "enemy" "fx"
```

```c
mu_string_arena_init(a);                       /* zeroed            */
uint32_t idx;
mu_string_arena_push(a, "player", &idx);       /* idx = 0           */
mu_string_arena_push(a, "enemy", &idx);

mu_string_arena_get(a, 1);                     /* "enemy"           */
mu_string_arena_count(a);                      /* 2                 */
mu_string_arena_bytes(a);                      /* bytes used        */
mu_string_arena_clear(a);                      /* keep memory       */
mu_string_arena_free(a);                       /* release           */
```

- Storage uses the `array` macros internally (double allocation chain:
  `data` + `offsets`).
- `push` returns `false` if the total byte size would overflow `uint32_t`.
- `clear` keeps allocated memory (by design: no realloc-per-frame churn).

### `mu_pool_link` — intrusive index-based links

```c
typedef struct { uint32_t prev; uint32_t next; } mu_pool_link;
```

Two indices instead of two pointers: survives serialization, realloc, and
network replication. Nodes live in your arrays; the links just stitch them.

- `mu_pool_link_detach(links, node)` — node points at itself
- `mu_pool_link_is_detached(links, node)` — detached test

### Indexed intrusive list (sentinel-based)

```
      head (sentinel)
     +-------------+
     | prev =  3   |
     | next =  1   |
     +-------------+
        ^       |
        |       v
     +------+ +------+ +------+
     |  3   |<|  1   |<|  2   |
     +------+ +------+ +------+
```

```c
mu_index_list_init(links, head);              /* empty: head<->head     */
mu_index_list_empty(links, head);
mu_index_list_insert_after(links, at, node);  /* O(1), node must be detached */
mu_index_list_insert_before(links, at, node);
mu_index_list_remove(links, node);            /* O(1), node must be attached */
```

The head is a sentinel node in the same links array; iteration is
`for (u32 n = links[head].next; n != head; n = links[n].next)`.

### `mu_freelist` — index stack (push/pop free slots)

```c
mu_freelist_init(links, head);
mu_freelist_push(links, head, node);    /* LIFO            */
uint32_t n = mu_freelist_pop(links, head);  /* MU_INVALID_INDEX when empty */
```

Uses only `next` (ignores `prev`), making it compatible with nodes also
enlisted in a full list at other times — as long as you never mix the two
roles concurrently. Classic use: recycling entity/instance slots in a pool.

---

## mu_min_containers.h — standalone minimalist containers

A dependency-free subset with a slightly different style (`MUC_API`,
`MUC_MALLOC` overrides). Useful when you want just the basics without the
`mu.h` umbrella. Define `MUC_STATIC` for internal linkage.

### `mu_array` (stretchy buffer, size_t header)

Same header-before-data trick as `mu_array.h`, but with `size_t` fields and
its own growth policy (start 8, double as needed):

```c
mu_array_ensure(a, n);          /* reserve                  */
mu_array_push(a, v);            /* append                   */
mu_array_pop(a);                /* size--                   */
mu_array_clear(a);
mu_array_size(a) / mu_array_capacity(a) / mu_array_empty(a)
mu_array_free(a);
```

Plus `mu_array_printf(char** buf, fmt, ...)` — printf straight into a
stretchy char buffer (append buffer pattern for building strings/messages).

### `mu_hash64` — open addressing with tombstones

Full-featured u64→u64 table: power-of-two capacity, linear probing,
`MU_HASH_EMPTY` / `MU_HASH_TOMBSTONE` sentinels, automatic rehash at 70%
load, deletion via tombstone + rehash cleanup.

```c
mu_hash64_init(&h, 16);
mu_hash64_set(&h, key, value);           /* false on OOM/bad key      */
mu_hash64_get(&h, key, &out);            /* false on miss             */
mu_hash64_get_or(&h, key, fallback);
mu_hash64_remove(&h, key);               /* tombstones                */
mu_hash64_clear(&h); / mu_hash64_free(&h);
```

Reserved keys: `UINT64_MAX` and `UINT64_MAX - 1` cannot be stored.

### `mu_hash64_static` — fixed-capacity, caller-owned arrays

```c
uint64_t keys[64], vals[64];
mu_hash64_static h = { keys, vals, 64 };
mu_hash64_static_clear(&h);
mu_hash64_static_set(&h, k, v);
mu_hash64_static_get(&h, k, &out);
```

No allocation, no growth: you own the arrays, capacity is yours to plan.

### Other pieces

- `mu_string_block` — string arena variant over stretchy buffers
  (`mu_string_block_init/push/get/free`).
- `mu_index_link` / `mu_index_list_*` / `mu_index_freelist_*` — same
  intrusive index-list idea as `mu_string.h`, separate implementation for
  standalone use.

Choose one family per project: `mu_array.h`+`mu_string.h` inside `mu.h`, or
`mu_min_containers.h` standalone — mixing both in one file works but
multiplies container concepts.
