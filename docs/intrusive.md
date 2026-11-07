# Intrusive data structures (`mu_dlist.h`, and the rule in AGENTS.md 3.15)

**Rule:** if an object has to be a member of a container, the link goes *inside*
the object. The container owns no nodes and allocates nothing.

```c
typedef struct entity { uint32_t id; mu_dlink link; } entity;

mu_dlist list;
mu_dlist_init(&list);
mu_dlist_push_back(&list, &e->link);          /* no allocation, cannot fail */

/* iterate: you get links back, and recover the payload */
for (mu_dlink *it = mu_dlist_front(&list); it; it = mu_dlist_next(&list, it))
{
    entity *e = mu_container_of(it, entity, link);
    use(e);
}

mu_dlist_erase(&list, &e->link);              /* O(1) from the object */
```

## Why bother

| | intrusive (`mu_dlist`) | node-owning (`std::list`-style) |
| --- | --- | --- |
| allocation per element | **none** | one, plus one free |
| memory per element | 16 B link inside the payload | 24 B node + payload, two objects |
| erase an element you hold | **O(1)** | O(1) if you kept the node pointer |
| erase given the *payload* | **O(1)** | O(n): payload -> node needs a search |
| push_back failure mode | none (no allocation) | out-of-memory mid-insert |
| cache lines touched per element | 1 | 2 (node, then payload) |

## Measured (`perf_test/bench_intrusive.c`, 1 048 576 elements, 6 independent runs)

Speedup of the intrusive list over a node-owning list, one row per run, so the
spread is visible instead of a single flattering pick:

| operation | run 1 | 2 | 3 | 4 | 5 | 6 | claimable |
| --- | --- | --- | --- | --- | --- | --- | --- |
| push vs **pooled** nodes | 0.91 | 1.35 | 1.28 | 0.65 | 0.87 | 1.29 | **no - it is a tie** |
| push vs heap nodes | 1.10 | 1.13 | 1.14 | 1.80 | 1.30 | 1.08 | 1.1x |
| traverse vs pooled nodes | 1.50 | 1.39 | 1.41 | 1.14 | 1.79 | 1.70 | 1.4x |
| traverse vs heap nodes | 1.30 | 1.28 | 1.29 | 2.13 | 1.98 | 1.41 | 1.3x |
| erase by handle vs pooled | 1.46 | 1.04 | 1.85 | 1.40 | 1.72 | 1.83 | 1.4x |
| erase by handle vs heap | 4.72 | 3.60 | 4.50 | 4.83 | 4.08 | 4.12 | **4.0x, stable** |

Allocations: **1 048 576** per push for both node variants, **0** for intrusive.

Erase given a payload pointer, reverse (worst) order, n = 8192, us per pass:
44 411 / 80 572 for the node variants vs **7.3** for intrusive - a 6.1e3x / 1.1e4x
ratio that grows with n. This one is **algorithmic, not cache**: the node
variants must search for the node that owns the payload, so they are O(n) per
erase and O(n^2) per pass; the intrusive list is O(1) because the payload *is*
the link.

The honest reading, in order of how much you should believe it:

1. **Erasing a known payload is O(1) instead of O(n).** 4 000x here, and it grows
   with n. This is the argument; everything else is a coefficient.
2. **Zero allocations.** 1 048 576 become 0, so the out-of-memory branch, the
   free-list walk and the fragmentation are gone from the hot path - and, unlike
   a timing number, this one is not noisy. (Also 16 B spent inside the payload
   versus 24 B for payload + node in two separate objects.)
3. **Coalesced traversal**, 1.3-1.8x, because a payload+link is one cache line
   and a node + separate payload is two.
4. **Push against a good pooled allocator is a tie - and sometimes a loss.**
   Pooled nodes arrive in allocation order, so they are already contiguous; the
   only thing intrusive removes is the allocation call. Do not sell intrusiveness
   as a push optimization.

If this table looks underwhelming next to a cache-locality claim, that is the
point: on a 2 vCPU shared host with CVs up to 35% we do not get to claim more
than the lower bound, and the lower bound is what a reader should plan with.
## When not to use it

* An object in several containers needs several links (one per container) - fine, but count the bytes.
* Objects shorter-lived than their membership (temporary lookup keys).
* Hot SoA streams where a link field would break unit stride (AGENTS.md 3.2 wins).
* Anything that gets shallow-copied: two copies would share the same links.

## See also

* `mu_rb_tree.h` - intrusive red-black tree (same discipline, 3 words per node, colour packed into the parent pointer).
* `mu_skiplist.h` - intrusive skip list with widths; variable-height links borrow pooled, recycled arrays.
