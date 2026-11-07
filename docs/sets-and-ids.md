# Sets, IDs, and Indices

The identity layer: bitsets, dense/sparse membership, stable handles, ranged
ID allocation, hash maps, and one-to-many indices. Everything here is built
around integer keys and array indices — no allocations hidden behind
iterators, all layouts serializable.

---

## mu_bitset.h — packed bit set (64-bit words)

`mu_bitset*.c`-backed set of bit indices with full set algebra. Dynamic
growing, shifting, iteration — the works.

```c
typedef struct mu_bitset
{
    uint64_t* MU_RESTRICT array;
    size_t word_count;                 /* words in use      */
    size_t word_capacity;              /* words allocated   */
} mu_bitset;
```

### Lifetime and basics

```c
mu_bitset* bs = mu_bitset_create();                 /* empty            */
mu_bitset* bs = mu_bitset_create_with_capacity(130);/* >= 130 bits      */
mu_bitset* cp = mu_bitset_copy(bs);

mu_bitset_set(bs, 63);       mu_bitset_reset(bs, 63);
mu_bitset_test(bs, 63);      /* -> bool */
mu_bitset_set_bit(bs, 63, true);                    /* value-style      */
mu_bitset_enable_bit(bs, 63); mu_bitset_disable_bit(bs, 63);

mu_bitset_clear(bs);         /* all zero           */
mu_bitset_fill(bs);          /* all one            */
mu_bitset_free(bs);
```

### Sizing and memory

```c
mu_bitset_resize_words(bs, n);   /* grow/shrink in 64-bit words, new zeroed */
mu_bitset_grow(bs, n);           /* grow only                               */
mu_bitset_trim(bs);              /* recover unused tail; false on realloc fail */
mu_bitset_size_in_bits(bs);      mu_bitset_size_in_words(bs);
mu_bitset_size_in_bytes(bs);
```

### Shifts

```c
mu_bitset_shift_left(bs, k);   /* members {1,2,10} -> {1+k, 2+k, 10+k} */
mu_bitset_shift_right(bs, k);  /* members {1,2,10} -> {1-k, 2-k, 10-k} */
```

Useful for rebasing indices after an offset change (e.g., merging two
sub-allocation ID spaces).

### Set algebra

```c
mu_bitset_inplace_union(a, b);          mu_bitset_union_count(a, b);
mu_bitset_inplace_intersection(a, b);   mu_bitset_intersection_count(a, b);
mu_bitset_inplace_difference(a, b);     mu_bitset_difference_count(a, b);
mu_bitset_inplace_symmetric_difference(a, b);
                                        mu_bitset_symmetric_difference_count(a, b);

mu_bitset_xor(out, a, b);  mu_bitset_or(out, a, b);
mu_bitset_and(out, a, b);  mu_bitset_not(out, a);   /* out = result */
mu_bitset_xor_assign(a, b); mu_bitset_or_assign(a, b); mu_bitset_and_assign(a, b);
```

All ops extend/limit logically — `*_count` variants don't modify. The
`*_assign` / inplace forms return `false` if an internal reallocation failed.

### Queries

```c
mu_bitset_count(bs);          /* popcount over all words        */
mu_bitset_empty(bs);
mu_bitset_minimum(bs);        /* lowest set bit, SIZE_MAX if none */
mu_bitset_maximum(bs);        /* highest set bit                */
mu_bitset_equal(a, b);
mu_bitsets_disjoint(a, b);    mu_bitsets_intersect(a, b);
mu_bitset_contains_all(a, b); /* b subset of a?                 */
```

### Iteration

Visitor callbacks return `false` to stop early:

```c
typedef bool (*mu_bitset_visit_fn)(size_t value, void* user);

mu_bitset_traverse(bs, visitor, user);
mu_bitset_traverse_range(bs, visitor, user, begin, count);
mu_bitset_for_each(bs, visitor, user);          /* inline, CTZ-driven */
```

Low-level scanning without callbacks:

```c
size_t i = 0;
while(mu_bitset_next_set_bit(bs, &i)) { /* i = member */ i++; }

size_t buf[64];
size_t from = 0;
size_t n = mu_bitset_next_set_bits(bs, buf, 64, &from);  /* batch extract */
```

Both use trailing-zero bit tricks (one iteration per set bit, skipping zero
words at 64 bits per step).

`mu_bitset_print(bs)` dumps an ASCII picture to stdout for debugging.

---

## mu_sparse_set.h — O(1) membership over an integer range

Dense + sparse pair for sets of IDs in `[0, capacity)`. The ECS workhorse.

```c
typedef struct
{
    uint32_t* dense;      /* packed active values   */
    uint32_t* sparse;     /* value -> dense index   */
    uint32_t count;       /* active count           */
    uint32_t capacity;    /* max legal value + 1    */
} mu_sparse_set;
```

### Layout

```
dense (packed active values)
index:   0   1   2   3
       +---+---+---+---+
dense: | 7 | 2 | 9 | 4 |
       +---+---+---+---+

sparse (value -> dense index)
index:   0 1 2 3 4 5 6 7 8 9
       +---------------------+
sparse:| x x 1 x 3 x x 0 x 2 |
       +---------------------+

sparse[7] = 0  ->  value 7 is dense[0]
sparse[2] = 1  ->  value 2 is dense[1]
```

Membership: `sparse[v] < count && dense[sparse[v]] == v`. The second check
rejects stale sparse entries left behind by swap-deletes.

### API

```c
mu_sparse_set_init(&set, capacity);   /* both arrays allocated        */
mu_sparse_set_insert(&set, v);        /* O(1), false if OOR/dup       */
mu_sparse_set_remove(&set, v);        /* O(1) swap-delete             */
mu_sparse_set_contains(&set, v);      /* O(1)                         */
mu_sparse_set_clear(&set);            /* count = 0                    */
mu_sparse_set_destroy(&set);

for(uint32_t i = 0; i < set.count; ++i)
    uint32_t v = mu_sparse_set_at(&set, i);   /* dense[i], tightly packed */
```

- **Iteration is over `dense`** — perfectly packed, cache-perfect.
- Remove is swap-delete: last dense element moves into the hole. Order is
  not preserved (it never is in a sparse set).
- Capacity is fixed at init (max value + 1); sparse array is allocated for
  the whole range up front. For huge ID spaces use `mu_bulk_storage`
  handles or `mu_hash32` instead.

---

## mu_bulk_storage.h — slots with holes + generational handles

Sparse bulk storage where IDs never change and stale references fail
cleanly. This is "array storage with holes, plus generation-checked
handles."

```c
typedef struct mu_weak_handle { uint32_t id; uint32_t generation; } mu_weak_handle;

typedef struct mu_bulk_storage
{
    uint8_t*  slots;          /* raw object memory; slot 0 = freelist header */
    uint32_t* generations;    /* per slot, bumped on free                    */
    uint8_t*  live;           /* 0 = dead, 1 = alive                         */
    size_t    slot_size;
    uint32_t  slot_capacity;  /* includes slot 0                             */
    uint32_t  live_count;
    uint32_t  next_unused;    /* first never-used slot                       */
} mu_bulk_storage;
```

### Mental model

- Big slot array; dead slots recycled through an **intrusive freelist** whose
  head lives in slot 0 and whose links are stored in the first `uint32_t` of
  each dead slot (dead objects don't need dignity).
- Each slot has a generation counter. Handles carry `(id, generation)`.
- When slot 42 dies and is reused, its generation increments; old
  `{42, gen=7}` handles no longer match `{42, gen=8}` and are rejected.

```
old handle {42, 7}  →  slot freed  →  slot reused, gen 8
                       {42,7} != {42,8}  →  rejected
```

### API

```c
mu_bulk_storage_init(&st, sizeof(MyItem), 64);  /* min capacity forced to 2 */
mu_bulk_storage_deinit(&st);                    /* frees bytes, not objects */

uint32_t id = mu_bulk_storage_alloc(&st);       /* 0 on failure             */
MyItem*  it = (MyItem*)mu_bulk_storage_ptr(&st, id);
mu_bulk_storage_free(&st, id);                  /* false if invalid/dead    */
mu_bulk_storage_is_live(&st, id);

mu_weak_handle h  = mu_bulk_storage_make_handle(&st, id);
bool fresh        = mu_bulk_storage_validate_handle(&st, h);
MyItem* resolved  = (MyItem*)mu_bulk_storage_resolve_handle(&st, h); /* NULL if stale */

mu_bulk_storage_visit_live(&st, visitor, user); /* visitor returns false to stop */
```

### Contract notes

- `alloc` reuses freelist slots first, then fresh slots; O(1) either way.
- Slot 0 is metadata, never an object; `free(0)` returns false.
- **IDs and handles stay stable across growth; raw pointers do not.**
  Storing `mu_bulk_storage_ptr` results across `alloc`s that might grow is
  the one big footgun.
- `deinit` does not run destructors: the container owns bytes, not semantics.
- Iteration is a sparse scan with hole-skips — this container buys stable
  identity, not dense iteration. For dense iteration use `mu_sparse_set` or
  an ECS archetype.

---

## mu_id_pool.h — ranged ID allocation

Allocate/free individual IDs **or contiguous ranges** from `[0, pool_size)`.
Free ranges coalesce into maximal runs. Originally built for bindless Vulkan
descriptor allocation (see nvpro / humus MakeID).

```c
typedef struct { uint32_t first; uint32_t last; } mu_id_pool_range;

typedef struct
{
    mu_id_pool_range* ranges;   /* sorted free runs          */
    uint32_t count, capacity;
    uint32_t max_id;
    uint32_t used_ids;
} mu_id_pool;
```

### API

```c
mu_id_pool_init(&pool, 4096);            /* IDs [0, 4096)              */
mu_id_pool_deinit(&pool);

uint32_t id;
mu_id_pool_create_id(&pool, &id);        /* single                     */
mu_id_pool_create_range_id(&pool, &id, 16);  /* 16 contiguous          */

mu_id_pool_destroy_id(&pool, id);
mu_id_pool_destroy_range_id(&pool, id, 16);

mu_id_pool_get_available_ids(&pool);
mu_id_pool_is_id(&pool, id);             /* is allocated?              */
mu_id_pool_is_range_available(&pool, n); /* could n contiguous fit?    */
mu_id_pool_get_largest_continuous_range(&pool);

mu_id_pool_destroy_all(&pool);
mu_id_pool_print_ranges(&pool);          /* debug dump                 */
mu_id_pool_check_ranges(&pool);          /* debug invariant check      */
```

All functions return `bool` success; allocation fails when no run is big
enough. Free runs are kept sorted and merged on destroy, so fragmentation
stays visible and `largest_continuous_range` is always exact.

---

## Hash tables (u64 key → value)

Two fixed-capacity linear-probing tables live in `mu_hash_table.h`, the
tombstoning `mu_hash64` in `mu_min_containers.h`
([containers.md](containers.md)), and the static-key `mu_phf` minimal perfect
hash in `mu_perfect_hash.h`.

### `hash_t` — quick u64→u64 (header-only, no asserts)

```c
hash_t h;
hash_init(&h, 1024);
hash_put(&h, key, value);
uint64_t v = hash_get(&h, key, default_value);
hash_free(&h);
```

Minimal: no count tracking, no removal, no resize — keys must never equal
`HASH_EMPTY` (0). Keys are mixed with a Murmur3-style finalizer
(`hash_u64`) before indexing.

### `mu_hash32_t` — owning u64→u32, fixed capacity

The "honest" version: allocates its arrays, tracks `count`, supports
removal, returns success/failure instead of magic 0s.

```c
mu_hash32_t h;
mu_hash32_init(&h, 1024);            /* false on OOM              */
mu_hash32_set(&h, key, value);       /* insert or overwrite       */
uint32_t out;
mu_hash32_get(&h, key, &out);        /* true/false — no magic 0   */
mu_hash32_contains(&h, key);
mu_hash32_remove(&h, key);           /* backshift repair          */
mu_hash32_count(&h); mu_hash32_capacity(&h); mu_hash32_empty(&h);
mu_hash32_clear(&h);
mu_hash32_destroy(&h);
```

- Keys must not equal `MU_HASH_UNUSED` (`0xFFFFFFFFFFFFFFFF`).
- `set` asserts `count < capacity` — full tables are a capacity-planning
  bug, not a resize trigger.
- **Removal uses backshift repair**: after blanking a slot, the rest of the
  cluster is reinserted so later lookups can't be broken by a hole:

```
Before:            remove K1 naively:     after repair:
[K1][K2][K3][--]   [--][K2][K3][--]       [K2][K3][--][--]
                   lookup(K3) fails!
```

### `mu_hash32_static_t` — non-owning, caller-provided memory

```c
uint64_t keys[256]; uint32_t vals[256];
mu_hash32_static_t h;
mu_hash32_static_init(&h, keys, vals, 256);
mu_hash32_static_set(&h, key, value);
uint32_t v = mu_hash32_static_get(&h, key);   /* 0 if missing */
mu_hash32_static_clear(&h);
```

Zero allocation, no growth, no removal. Ideal for hot-path resource lookup
(texture_id, mesh_id) where malloc is a sin and capacity is known.
Note: `get` returns 0 for missing keys — don't store 0 as a meaningful value.

### Which one

| Need | Use |
|---|---|
| One-shot scratch map | `hash_t` |
| General owned map, deletions | `mu_hash32_t` |
| Fixed memory, hottest path | `mu_hash32_static_t` |
| u64 values, tombstones, rehash | `mu_hash64` (min_containers) |
| Static keys, O(1) perfect lookup | `mu_phf` (perfect_hash) |

### `mu_phf` — runtime u64 minimal perfect hash

For a fixed set of unique `uint64_t` keys, builds an index in `[0, n)` with no
collisions. Lookup is one bucket displacement + one key compare.

```c
mu_phf phf;
mu_phf_init(&phf);

/* values optional: NULL makes lookup return the original key index */
if (!mu_phf_build(&phf, keys, values, count, seed))
    /* duplicate keys or OOM */;

uint64_t value;
if (mu_phf_lookup(&phf, key, &value))
    use(value);

uint64_t v = mu_phf_value_or(&phf, key, fallback);
bool present = mu_phf_contains(&phf, key);

mu_phf_destroy(&phf);
```

- **Minimal**: `n` keys occupy exactly `n` slots; `keys` and `values` are
  copied and owned by the table.
- **Static**: build once, then lookup only. There is no insert/remove.
- **Duplicates are rejected** (`build` returns `false`).
- **Seed** controls hashing; retries are deterministic and stored in
  `mu_phf_seed`.
- Memory is about `16n` bytes for keys/values plus `4 * ceil(n / 3)` bytes
  of displacement data.
- Build is expected `O(n)` after the duplicate check; lookup is `O(1)`.
- `mu_phf_count`, `mu_phf_bucket_count`, `mu_phf_seed`, and `mu_phf_empty`
  expose metadata.

Use this instead of `mu_hash64` when the key set is known at load time and
lookup speed or memory predictability matters. For mutable maps, byte keys,
or iteration, use `mu_hash64` / `mu_hash32_t`.

---

## mu_multi_index.h — one key, many values

Hash map from key → first node, plus a circular doubly linked list of nodes
per key. O(1) add/remove when you hold the node index; O(k) traversal of a
key's value list.

```c
#define MU_MULTI_INDEX_NONE UINT32_MAX

typedef struct mu_multi_index_node
{
    uint64_t key;
    uint32_t value;
    uint32_t prev, next;   /* per-key circular list (node indices) */
    uint32_t alive;
} mu_multi_index_node;

typedef struct mu_multi_index
{
    mu_multi_index_node* nodes;      /* node pool + freelist        */
    uint32_t node_count, node_capacity, free_head;

    uint64_t* map_keys;              /* open-addressed key->head    */
    uint32_t* map_values;
    uint8_t*  map_states;
    uint32_t  map_capacity, map_count;
} mu_multi_index;
```

### API

```c
mu_multi_index_init(&idx, 64, 64);       /* nodes, map capacity        */
mu_multi_index_deinit(&idx);

uint32_t node = mu_multi_index_add(&idx, key, value);  /* MU_MULTI_INDEX_NONE on OOM */
mu_multi_index_remove(&idx, node);       /* O(1): unlink + recycle     */

uint32_t first = mu_multi_index_first(&idx, key);
uint32_t next  = mu_multi_index_next(&idx, first, node);

mu_multi_index_value(&idx, node);
mu_multi_index_key(&idx, node);
mu_multi_index_node_valid(&idx, node);
mu_multi_index_count_key(&idx, key);     /* O(k) count                 */

mu_multi_index_visit_key(&idx, key, visitor, user);  /* false stops early */
```

### Iterating one key's values

```c
for(uint32_t n = mu_multi_index_first(&idx, key);
    mu_multi_index_node_valid(&idx, n);
    n = mu_multi_index_next(&idx, n, node))
{
    uint32_t v = mu_multi_index_value(&idx, n);
}
```

Node indices are stable handles into the pool (freelist recycling keeps
`alive` flags). Use for entity→component-set indices, spatial buckets, or
tag databases — anywhere one key fans out to many values.

---

## mu_chunked_array.h — arrays of arrays over a shared chunk pool

Many variable-length `uint32_t` lists that allocate their storage from one
shared pool of fixed-size chunks. Each logical array is just three integers
(first chunk, last chunk, count), so arrays are cheap to copy and serialize.

```c
#define MU_ARRAY_OF_ARRAYS_CHUNK_SIZE 14u      /* values per chunk (override) */
#define MU_CHUNKED_U32_NONE UINT32_MAX

typedef struct mu_chunked_u32_chunk
{
    uint32_t values[MU_ARRAY_OF_ARRAYS_CHUNK_SIZE];
    uint32_t used, prev_chunk, next_chunk, free_next;
} mu_chunked_u32_chunk;

typedef struct mu_chunked_u32_pool
{
    mu_chunked_u32_chunk* chunks;
    uint32_t chunk_count, chunk_capacity, free_head;
} mu_chunked_u32_pool;

typedef struct mu_chunked_u32_array
{
    uint32_t first_chunk, last_chunk, count;
} mu_chunked_u32_array;
```

### API

```c
mu_chunked_u32_pool_init(&pool, 256);    /* chunk capacity             */
mu_chunked_u32_pool_deinit(&pool);

mu_chunked_u32_array arr;                /* cheap value struct         */
mu_chunked_u32_array_init(&arr);         /* empty                      */

mu_chunked_u32_array_push(&pool, &arr, 42);
uint32_t v;
mu_chunked_u32_array_pop(&pool, &arr, &v);
mu_chunked_u32_array_get(&pool, &arr, 0, &v);

mu_chunked_u32_array_visit(&pool, &arr, visitor, user);  /* false stops */
mu_chunked_u32_array_clear(&pool, &arr); /* chunks return to the pool  */
```

### Design notes

- Chunks are chained with **indices** (`prev_chunk`/`next_chunk` into
  `pool->chunks`), never pointers — the pool can realloc freely.
- `push`/`pop` are O(1) amortized (grab/return a chunk from the freelist as
  needed); `get` walks the chain O(index/CHUNK_SIZE).
- The logical array struct contains no heap state: copy it, store it,
  serialize it, and as long as the pool outlives it the data is reachable.
- Typical use: per-entity children lists, per-node edge lists, tagged
  buckets — many small lists with big aggregate churn, without one-alloc-per-
  list overhead.
