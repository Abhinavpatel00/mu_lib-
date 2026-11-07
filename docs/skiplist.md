# `mu_skiplist.h` - intrusive skip list with widths

A Pugh skip list with the jrsl extension: every forward pointer carries a
**width**, the number of level-0 elements it skips. Widths turn the structure
into an order-statistic container, so index -> node and node -> index are both
O(log n) on a linked structure.

```c
typedef struct item { int key; mu_sl_node node; } item;

mu_skiplist l;
mu_skiplist_init(&l, cmp_int);                 /* cmp compares KEYS */
mu_sl_insert(&l, &it->node, &it->key);         /* returns the index it landed at */
mu_sl_find(&l, &key);                          /* first equal */
mu_sl_rank(&l, &key);                          /* lower bound index */
mu_sl_at(&l, 42);                              /* O(log n) random access */
mu_sl_index_of(&l, &it->node);                 /* node -> index, by identity */
mu_sl_remove(&l, &it->node);                   /* O(log n) */
```

## API

| function | complexity | notes |
| --- | --- | --- |
| `mu_skiplist_init(l, cmp)` | O(1) | `cmp` compares two keys (qsort convention) |
| `mu_sl_insert(l, node, key)` | O(log n) | duplicates land **after** their equals; returns the index |
| `mu_sl_remove(l, node)` | O(log n) | by identity, so duplicates are unambiguous |
| `mu_sl_find` / `mu_sl_rank` | O(log n) | first match / lower bound |
| `mu_sl_at` / `mu_sl_index_of` | O(log n) | random access and rank, via widths |
| `mu_sl_first` / `mu_sl_next` | O(1) | level-0 iteration |
| `mu_sl_validate` | O(n) | structural invariant check for tests |
| `mu_sl_get_stats` | O(pool) | size, level, allocs, recycles |

## Allocation

One allocation per node is the textbook cost: the forward/width arrays, sized to
the node's random level. Here those arrays are **pooled per level and recycled**,
so after warmup an insert/remove pair does not call the allocator.
`test_skiplist.c` asserts it: eight rounds of 512 inserts cost less than 1.5
rounds' worth of allocations.

Nodes with caller-owned arrays never allocate at all:

```c
typedef struct big {
    int key; mu_sl_node node;
    mu_sl_node *lv_next[MU_SL_MAX_LEVEL];
    uint32_t    lv_span[MU_SL_MAX_LEVEL];
} big;

mu_sl_use_embedded(&b->node, b->lv_next, b->lv_span);
mu_sl_insert_embedded(&l, &b->node, &b->key);
```

Two entry points rather than a flag: a flag would make `insert` read a field
before it has written anything, and an uninitialized node would then decide its
own fate. (It did. That is why this is two functions.)

## Levels and determinism

`p = 1/4` (`MU_SL_P_BITS = 2`), max level 16 by default (`MU_SL_MAX_LEVEL`).
The PRNG is an internal xorshift64* seeded per list, so a run reproduces from a
seed - which is what lets the gate compare the list against a sorted-array oracle
deterministically.

## Width semantics (the two off-by-ones worth writing down)

`span[i]` of a node is the number of elements between it and its level-`i`
successor, **inclusive of the successor**. The sentinel ranks 0, so the element
at 0-based index `i` has 1-based rank `i + 1`, and a descent that must *land on*
it advances while `traversed + span <= i + 1`. A descent that must stop *before*
it (removal) advances while `traversed + span <= i`. Getting those two backwards
produces a list that silently keeps removed nodes linked.

## Gate

`perf_test/test_skiplist.c` - 309 checks: empty/single/duplicate/removal paths,
a 20 000-operation fuzz mirrored against a sorted-array oracle (order, size,
`at(i)`, `index_of`, full forward walk), the width invariant at every checkpoint,
block recycling, embedded nodes, and a counting allocator that proves `destroy`
returns every block it took.
