# `mu_id_alloc.h` — dense, thread-safe, and sparse ID allocators

Bitmap-backed numeric ID allocation for bindless handles, resource indices,
entity IDs, and descriptor slots.

```c
#include "mu.h"

mu_id_alloc ids;
mu_id_alloc_init(&ids, 1024);

uint32_t id = mu_id_alloc_alloc(&ids);
mu_id_alloc_free(&ids, id);
mu_id_alloc_fini(&ids);
```

`MU_INVALID_INDEX` (`UINT32_MAX`) means "no ID available" or "allocation
failed". ID `0` is a valid ID unless you reserve it yourself.

## Dense allocator

```c
typedef struct mu_id_alloc
{
    uint32_t* data;
    unsigned num_elements;
    unsigned num_set_elements;
    unsigned lowest_free_idx;
} mu_id_alloc;

bool mu_id_alloc_init(mu_id_alloc* buf, unsigned initial_num_ids);
void mu_id_alloc_fini(mu_id_alloc* buf);

uint32_t mu_id_alloc_alloc(mu_id_alloc* buf);
uint32_t mu_id_alloc_alloc_range(mu_id_alloc* buf, unsigned count);
void mu_id_alloc_free(mu_id_alloc* buf, uint32_t id);
void mu_id_alloc_reserve(mu_id_alloc* buf, uint32_t id);
bool mu_id_alloc_test(const mu_id_alloc* buf, uint32_t id);
```

`alloc_range` returns the first ID of a contiguous free block. The block is
word-aligned in the bitmap, so the returned ID is a multiple of 32.

Iteration:

```c
mu_id_alloc_foreach(&ids, id)
    use(id);
```

The dense allocator grows by doubling and never shrinks its bitmap. It is not
thread-safe.

## Thread-safe wrapper

```c
mu_id_alloc_mt mt;
mu_id_alloc_mt_init(&mt, 1024, true);  /* true = never hand out ID 0 */

uint32_t id = mu_id_alloc_mt_alloc(&mt);
mu_id_alloc_mt_free(&mt, id);
mu_id_alloc_mt_fini(&mt);
```

The wrapper uses a spinlock around dense operations. `skip_zero` reserves ID 0
during initialization.

## Sparse allocator

The sparse allocator splits the 32-bit ID space into 1024 segments of 2^22
IDs. Each segment grows independently, so touching a high ID does not reserve
a 512 MB bitmap.

```c
mu_id_alloc_sparse ids;
mu_id_alloc_sparse_init(&ids);

uint32_t id = mu_id_alloc_sparse_alloc(&ids);
mu_id_alloc_sparse_free(&ids, id);
mu_id_alloc_sparse_fini(&ids);
```

API:

```c
bool mu_id_alloc_sparse_init(mu_id_alloc_sparse* buf);
void mu_id_alloc_sparse_fini(mu_id_alloc_sparse* buf);
uint32_t mu_id_alloc_sparse_alloc(mu_id_alloc_sparse* buf);
uint32_t mu_id_alloc_sparse_alloc_range(mu_id_alloc_sparse* buf, unsigned count);
void mu_id_alloc_sparse_free(mu_id_alloc_sparse* buf, uint32_t id);
void mu_id_alloc_sparse_reserve(mu_id_alloc_sparse* buf, uint32_t id);
```

Range allocation does not span segment boundaries. If a request cannot fit in
one segment, it returns `MU_INVALID_INDEX`.

## Choosing a variant

| Need | Use |
|------|-----|
| Small/medium dense ID space | `mu_id_alloc` |
| Dense + concurrent callers | `mu_id_alloc_mt` |
| Large sparse 32-bit ID space | `mu_id_alloc_sparse` |
| Range-list semantics / existing code | `mu_id_pool` |

