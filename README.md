# mylib (`mu`)

A C99 single-header collection of data structures, allocators, math, ECS, and
concurrency primitives for engine and tools code. One umbrella header:

```c
#include "mu.h"
```

Full documentation lives in [docs/README.md](docs/README.md).

## Pick by access pattern

| Need                     | Use                          |
|--------------------------|------------------------------|
| Dense hot iteration      | `array` / frame arena        |
| Non-owning view          | `mu_span`                    |
| Stable handles           | `mu_bulk_storage`            |
| Dense active IDs         | `mu_sparse_set`              |
| Sparse 64-bit IDs        | `mu_sparse_array`            |
| Unique work queue        | `mu_worklist`                |
| Fixed-size churn         | `mu_slab`                    |
| Delayed destruction      | `mu_deferred_free`           |
| Ordered intrusive tree   | `mu_rb_tree`                 |
| Overlapping intervals    | `mu_rb_tree` interval tree   |
| Reclaimable size classes | `mu_slab_manager`            |
| Dense/ranged IDs         | `mu_id_alloc`                |
| Huge sparse ID sets      | `mu_sparse_bitset`           |
| Dependency graph         | `mu_dag`                     |
| Range -> pointer         | `mu_range_remap`             |
| key -> one value         | `mu_hash32` / `mu_hash64`    |
| static keys, perfect O(1)| `mu_phf`                     |
| key -> many values       | `mu_multi_index`             |
| packed booleans          | `mu_bitset`                  |
| ranged IDs               | `mu_id_pool`                 |
| variable children        | `mu_chunked_u32_array`       |
| packed strings           | `mu_string_arena`            |
| intrusive lists          | `mu_pool_link` / `mu_index_list` |

## Quick reference

| Data structure | When to use | How it works (brief) |
|---|---|---|
| `array` | Dense hot iteration, cache‑friendly storage | Contiguous dynamic array; grows exponentially; O(1) push/pop at end; iteration via pointer range |
| `mu_span` | Non‑owning view / slice of data, fast iteration | Three `size_t` fields (ptr, len, cap); no allocation; just a window into existing memory |
| `mu_bulk_storage` | Stable weak handles to allocated memory, outlive the allocator | Arena‑style pool with generation offset; hand‑over‑hand pointers stay valid as long as base arena alive |
| `mu_sparse_set` | Fast membership test + iteration over active IDs | Dual arrays: dense (ordered active IDs) and sparse (inverse map); O(1) contain/insert/remove |
| `mu_sparse_array` | Sparse 64‑bit indexed data, stable addresses without indirection | Paged page‑table; each page holds array of values; address = page*stride + offset; growth adds new pages |
| `mu_worklist` | Unique work items, push/pop without duplicates | Dense array + swap‑with‑last; pop removes last element; guarantees uniqueness via pop‑last trick |
| `mu_slab` | Fixed‑size block allocation, high‑throughput many small allocs | Pre‑partitioned chunks; bitmap of free blocks; per‑thread cache; O(1) alloc/free |
| `mu_deferred_free` | Safe frame‑delayed retirement of memory, avoid use‑after‑free | Per‑frame linked list of handles; freed after N frames; insertion O(1), reclamation batched |
| `mu_rb_tree` | Ordered red‑black tree, interval queries, intrusive updates | Each user struct holds parent/left/right/color; rotations maintain RB property; O(log n) ops |
| `mu_slab_manager` | Size‑classed allocator with reusable blocks, multi‑heap | Bins by size class; each bin has freelist; threads get private cache; coalescing on free |
| `mu_id_alloc` | Bitmap‑based ID allocation with generations, stable handles | Bitfield of free IDs; find‑first‑zero; generation word prevents retired ID reuse |
| `mu_sparse_bitset` | Huge sparse ID sets (>64k) where a full bitset is too big | Chunked array of small bitsets (e.g., 1024‑bit chunks); only allocate used chunks |
| `mu_dag` | Dependency graph, topological ordering, cycle detection | Adjacency list per node; Kahn’s algorithm or DFS; bottom‑up traversal via entry/exit times |
| `mu_range_remap` | Map a sorted range of keys to pointers/values, lookup by range | Sorted array of (start, end, ptr) entries; binary search for containing range; O(log n) |
| `mu_hash32` / `mu_hash64` | Key → value look‑up, open addressing, power‑of‑two size | Hash table with linear probing; empty slots are 0; resizing when load factor > threshold |
| `mu_phf` | Perfect hash for static key sets, guaranteed O(1) worst‑case | Two‑level scheme: first hash selects bucket, second hash is perfect within bucket; built at init |
| `mu_multi_index` | Key → many values, multiple sorted/unindexed views | Hash map from key to a vector of values; optional secondary indices (sorted, unique) |
| `mu_bitset` | Packed booleans, bit‑level operations, mask operations | Array of `uint64_t`; bit index = word_index*64 + bit_offset; ops via shifts/masks |
| `mu_id_pool` | Pool of stable handles with generation, entity‑system style | Struct with `uint32_t generation` and `uint32_t index`; allocation reuses freed slots with generation bump |
| `mu_chunked_u32_array` | Variable‑size children collected in fixed‑size chunks | Array of chunks each holding up to N `uint32_t`; growth adds new chunk; iteration across chunks |
| `mu_string_arena` | Packed, null‑terminated strings, many allocations | Arena of bytes; allocate string by writing at offset, store offset+length; termination via next‑string offset |
| `mu_pool_link` / `mu_index_list` | Intrusive doubly‑linked / singly‑linked lists, zero‑overhead | Each element contains a `mu_pool_link` struct with `next` pointer; list ops manipulate only that field |
## Additional containers

| Data structure | When to use | How it works (brief) |
|---|---|---|
| `mu_bloom` | Probabilistic set membership, cheap early‑out gate before hash/asset look‑ups | Probabilistic filter using double‑hashing; stores `uint64_t` words; O(k) probe test, false‑positive rate controllable by size |
| `mu_chunked_bitset` | Growing bitset where existing indices stay stable | Fixed‑size block words; growth adds new blocks only, old indices unaffected; O(1) set/clear/test |
| `mu_hier_bitset` | Bitset for extremely large ID domains, sparse allocation | Multi‑level hierarchy: top‑level chunk bitmap + per‑chunk word arrays; only allocates used branches |
| `mu_length_index` | Sparse key → value map with O(1) average lookup | Flat array of entries + overflow linked list; linear probing within bucket; good for sparse mappings |
| `mu_min_containers` | Small, zero‑overhead dynamic array + static perfect‑hash | Stretchy buffer that doubles in size; tiny `mu_hash64_static` for static keys, no runtime allocation |
## Layout

- `mu.h` — umbrella header for the core (`mu/` directory)
- `mu/` — every library header and its implementations (containers, allocators,
  math, RNG, profiling, ECS, threading)
- `docs/` — detailed per-module documentation

## Building the tests

```sh
cc -std=c99 -O2 -D_POSIX_C_SOURCE=199309L test.c mu.c -o test && ./test
c++ -std=c++17 -O2 -D_POSIX_C_SOURCE=199309L -pthread test.cpp mu.c -o test_cpp && ./test_cpp
```
# mu_lib-
# mu_lib-
