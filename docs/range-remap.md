# `mu_range_remap.h` — sorted range-to-pointer map

Maps closed unsigned ranges to pointers with O(log n) lookup over a flat
sorted array. Ranges never overlap.

```c
#include "mu.h"

mu_range_remap remap;
mu_range_remap_init(&remap);

mu_range_remap_insert(&remap, 0x0000, 0x0FFF, page0);
mu_range_remap_insert(&remap, 0x1000, 0x1FFF, page1);

void* p = mu_range_remap_get(&remap, 0x1234);  /* page1 */

mu_range_remap_destroy(&remap);
```

## API

```c
void mu_range_remap_init(mu_range_remap* remap);
bool mu_range_remap_init_static(mu_range_remap* remap,
                                mu_range_entry* entries, uint32_t capacity);
void mu_range_remap_destroy(mu_range_remap* remap);
void mu_range_remap_clear(mu_range_remap* remap);

const mu_range_entry* mu_range_remap_find(const mu_range_remap* remap,
                                          uint32_t value);
void* mu_range_remap_get(const mu_range_remap* remap, uint32_t value);
bool mu_range_remap_contains(const mu_range_remap* remap, uint32_t value);

mu_range_entry* mu_range_remap_insert(mu_range_remap* remap,
                                      uint32_t start, uint32_t end,
                                      void* ptr);
mu_range_entry* mu_range_remap_insert_truncate(mu_range_remap* remap,
                                               uint32_t start, uint32_t end,
                                               void* ptr);
bool mu_range_remap_remove(mu_range_remap* remap,
                           uint32_t start, uint32_t end);

uint32_t mu_range_remap_count(const mu_range_remap* remap);
const mu_range_entry* mu_range_remap_at(const mu_range_remap* remap,
                                        uint32_t index);
```

Ranges are inclusive: `{10, 20}` contains `10` through `20`.

`insert` rejects overlaps and returns `NULL`. Adjacent ranges are allowed:
`{10, 20}` and `{21, 30}` do not overlap. Passing `NULL` as `ptr` means
"look up an exact range only".

`insert_truncate` is useful when a larger mapping is split: if `start` matches
an existing range and `end` is inside it, the existing range is shortened and
its pointer updated.

## Ownership and layout

```
entries:
  [ start | end | ptr ] [ start | end | ptr ] ...
    sorted by start, non-overlapping
```

- `mu_range_remap_init` owns a heap array that grows by doubling.
- `mu_range_remap_init_static` uses caller storage and never reallocates; a
  full table makes `insert` return `NULL`.
- `destroy` frees only owned storage.
- Removal shifts the tail with `memmove`, O(n).

## Use cases

- Virtual address/page remapping
- Sparse virtual resource page tables
- Asset byte-range lookup
- GPU descriptor range translation
- Interval-to-handle tables when you only need point lookup

For overlapping interval queries use `mu_rb_tree.h`'s interval tree instead.

