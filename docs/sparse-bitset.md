# `mu_sparse_bitset.h` — sparse bitset over huge ID spaces

A bitset that allocates storage only for 1024-bit windows that contain set
bits. Use it when IDs are large and scattered; use `mu_bitset` or
`mu_chunked_bitset` when the domain is small or mostly full.

```c
#include "mu.h"

mu_sparse_bitset visible;
mu_sparse_bitset_init(&visible);

mu_sparse_bitset_set(&visible, 5);
mu_sparse_bitset_set(&visible, 1u << 20);

if (mu_sparse_bitset_test(&visible, 5))
    draw();

mu_sparse_bitset_destroy(&visible);
```

## Layout

```
mu_sparse_bitset
  tree -> rb_tree of nodes

node (offset 0)      node (offset 1024)      node (offset 100352)
  bits 0..1023         bits 1024..2047          bits 100352..101375
```

Each node holds `1024` bits (`16` × `uint64_t`). Nodes are allocated on first
set and removed when their last bit is cleared.

## API

```c
void mu_sparse_bitset_init(mu_sparse_bitset* s);
void mu_sparse_bitset_destroy(mu_sparse_bitset* s);
void mu_sparse_bitset_clear_all(mu_sparse_bitset* s);

bool mu_sparse_bitset_set(mu_sparse_bitset* s, uint32_t bit);
void mu_sparse_bitset_clear(mu_sparse_bitset* s, uint32_t bit);
bool mu_sparse_bitset_test(const mu_sparse_bitset* s, uint32_t bit);

unsigned mu_sparse_bitset_count(const mu_sparse_bitset* s);
bool mu_sparse_bitset_empty(const mu_sparse_bitset* s);

bool mu_sparse_bitset_dup(mu_sparse_bitset* dst, const mu_sparse_bitset* src);
int  mu_sparse_bitset_merge(mu_sparse_bitset* dst, const mu_sparse_bitset* src);
bool mu_sparse_bitset_contains(const mu_sparse_bitset* a,
                               const mu_sparse_bitset* b);
```

`merge` returns `-1` on allocation failure, `0` if `dst` did not change, and
`1` if bits were added. `contains` answers "is `b` a subset of `a`?".

## Iteration

```c
uint32_t bit;
while (mu_sparse_bitset_next_set(&visible, &bit))
{
    use(bit);
    if (bit == UINT32_MAX)
        break;
    ++bit;
}

mu_sparse_bitset_foreach(&visible, bit)
    use(bit);
```

`next_set` finds the first set bit at or after `*from` and writes the bit back
to `*from`. `mu_sparse_bitset_foreach` handles the `UINT32_MAX` edge case.

## When to use it

| Domain | Best choice |
|--------|-------------|
| Small dense bits | `mu_bitset` |
| Large dense / hierarchical | `mu_chunked_bitset`, `mu_hier_bitset` |
| Large sparse IDs | `mu_sparse_bitset` |
| Per-ID membership without bits | `mu_sparse_set` |

## Threading and ownership

- Not thread-safe.
- Nodes are owned by the bitset and freed by `destroy` / `clear_all`.
- Iteration is invalidated by `set`, `clear`, `merge`, or `destroy` during
  the loop, except that `clear` removes empty nodes. Collect IDs first if the
  loop mutates the set.

Inspired by Mesa `util/sparse_bitset.h` (MIT).
