# `mu_sparse_array.h` — paged sparse array with stable addresses

A 64-bit-indexed sparse array built from fixed-size pages. Only the paths you
touch are allocated, elements never move after first access, and reads are
lock-free.

```c
#include "mu.h"

mu_sparse_array entities;
mu_sparse_array_init(&entities, sizeof(Entity), 256);

Entity* e = (Entity*)mu_sparse_array_get(&entities, 0x123456789ull);
if (e)
{
    e->health = 100;
}

Entity* existing = (Entity*)mu_sparse_array_peek(&entities, 42);
if (!existing)
{
    /* 42 has never been touched, or allocation failed earlier */
}

mu_sparse_array_destroy(&entities);
```

Use this instead of `mu_sparse_set` when ids are large, sparse or 64-bit. Use
`mu_sparse_set` when ids are dense and bounded by a known capacity.

---

## Layout

The array is a tree of fixed-size nodes. Internal nodes hold `node_size`
child pointers; leaves are data pages holding `node_size` elements.

```
root (internal, level N)
  |
  +-- child[0] -> internal level N-1
  +-- child[1] -> ...
  |
  +-- child[k] -> internal level 1
                    |
                    +-- child[j] -> data page
                                      [ elem 0 | elem 1 | ... | elem N-1 ]
```

- The root is always an internal node (`level >= 1`), so a data page is never
  mistaken for a node.
- A root at level `L` covers `node_size^(L + 1)` indices.
- `node_size` must be a power of two and at least 2.
- New pages and nodes are zeroed. The first `get` for an index returns a
  pointer to a zeroed element.

### Index decomposition

For `node_size = 256` (`log2 = 8`):

```
index bits:  [ child 1 ][ child 0 ][ page offset ]
                 8          8            8
```

A root at level 2 covers `256^3 = 16,777,216` indices. Higher levels are
created on demand. Each level adds 8 bits of reach; a root at level 7 covers
the full 64-bit index space.

## API

```c
bool mu_sparse_array_init(mu_sparse_array* arr,
                          size_t elem_size,
                          uint32_t node_size);

void* mu_sparse_array_get(mu_sparse_array* arr, uint64_t idx);
void* mu_sparse_array_peek(mu_sparse_array* arr, uint64_t idx);

void mu_sparse_array_clear(mu_sparse_array* arr);
void mu_sparse_array_destroy(mu_sparse_array* arr);
```

- `get` allocates missing internal nodes/pages, then returns the element.
  Returns `NULL` on allocation failure.
- `peek` never allocates; it returns `NULL` if the path or page does not
  exist.
- `clear` frees all pages/nodes but keeps `elem_size` and `node_size`.
- `destroy` frees everything and resets `elem_size` to 0.

## Properties

- **Stable addresses:** once an element is returned, the page never moves.
  Store the pointer until you explicitly `clear`/`destroy`.
- **O(1)–O(level) access:** at most `ceil(64 / log2(node_size))` pointer
  hops, typically 3–5.
- **No iteration:** this is an index, not a container. Keep a separate list of
  live ids if you need to walk them.
- **No shrinking:** removing individual elements is not supported. Use
  `clear` at a safe point, or store a live/dead flag in the element.
- **Memory grain:** pages are `node_size * elem_size` bytes. Choose
  `node_size` from the expected density: larger pages mean fewer tree hops,
  smaller pages mean less memory for sparse id spaces.

## Threading

- Concurrent `get` / `peek` from multiple threads is supported.
- Missing-path creation is serialized by `grow_lock`; readers use acquire
  loads and only see fully initialised children.
- `clear` and `destroy` require that no other thread is reading or writing.

Typical use: one thread creates/populates the array during load, many threads
read it at runtime. If multiple threads may create new pages, `get` is still
safe, but contention on `grow_lock` should be expected.

## Failure handling

`get` can fail because a node or page allocation failed. It returns `NULL`
without corrupting existing data; a later retry may succeed. `peek` is the
right choice when a missing element is a normal condition and you do not want
to allocate.
