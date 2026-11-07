#pragma once
#ifndef MU_SKIPLIST_H
#define MU_SKIPLIST_H

/* ==========================================================================
 * mu_skiplist.h — intrusive skip list with widths (order statistics)
 * ==========================================================================
 *
 * A skip list keeps a sorted sequence in O(log n) expected time without
 * rotations, without a rebalance pass and without a second array. This one is
 * the Pugh structure with the addition described in the jrsl reference: every
 * forward pointer carries a WIDTH (the number of level-0 elements it skips), so
 * the list can answer "what is at index i" and "what is the index of key k" in
 * O(log n) as well. That is the one thing a plain skip list cannot do and the
 * reason to bother with widths: it makes the structure a drop-in for the
 * "sorted array with fast insert" case, not just a set.
 *
 * INTRUSIVE BY DESIGN (see AGENTS.md, "data structures are intrusive by
 * default"). The list stores no keys of its own and copies no payload: you
 * embed a `mu_sl_node` in the object and hand over a key pointer. Removing an
 * object you already hold is O(log n) and takes no allocation, no search of a
 * parallel node array, and no cache miss budget beyond the pointers.
 *
 * ALLOCATION. Exactly one thing is allocated: the per-node forward/width
 * arrays, sized to the node's randomly chosen level. Those blocks are pooled by
 * level and RECYCLED -- after warmup an insert/remove pair never calls the
 * allocator at all (mu_sl_stats reports allocs vs recycles so you can check
 * that claim instead of believing it). Nodes whose maximum level is known
 * small can avoid even that in the steady state, or embed fixed arrays with
 * MU_SL_USE_EMBEDDED to allocate nothing ever.
 *
 * ORDERING. `cmp` compares two KEYS, not nodes, and follows qsort's convention:
 * <0, 0, >0. Duplicates are allowed; they are inserted after their equals, so
 * the list is stable. Keys must not change while inserted -- that is true of
 * every ordered container and still worth writing down.
 *
 * THREADING. No locking. One writer, or your own lock around it.
 *
 * Randomness: an internal xorshift64*, seeded per list, so a run is
 * reproducible from a seed. Deterministic level assignment is what makes the
 * gate able to compare this against a sorted-array oracle byte for byte.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "mu_macros.h"

MU_BEGIN_EXTERN_C

#ifndef MU_SL_MAX_LEVEL
#  define MU_SL_MAX_LEVEL 16
#endif

/* Pugh's p. 1/4 gives ~1.33 pointers per element; 1/2 gives 2 and a shallower
 * tree. 1/4 is the usual default (Redis uses it) and what the widths are sized
 * for here. */
#ifndef MU_SL_P_BITS
#  define MU_SL_P_BITS 2      /* p = 1/4 */
#endif

#define MU_SL_NONE 0xFFFFFFFFu

typedef struct mu_sl_node mu_sl_node;

struct mu_sl_node
{
    mu_sl_node  **next;      /* level forward pointers, length == level */
    uint32_t     *span;      /* width of each forward pointer, length == level */
    const void   *key;       /* borrowed; must outlive the node */
    uint16_t      level;     /* 1..MU_SL_MAX_LEVEL */
    uint8_t       embedded;  /* 1 if next/span point into user memory */
    void         *block;     /* the pooled level block, NULL if embedded */
};

typedef int (*mu_sl_cmp)(const void *a, const void *b);
typedef void *(*mu_sl_alloc_fn)(size_t size, void *user);
typedef void (*mu_sl_free_fn)(void *ptr, void *user);

typedef struct mu_skiplist
{
    mu_sl_node   head;                        /* sentinel, key never compared */
    mu_sl_node  *head_next[MU_SL_MAX_LEVEL];
    uint32_t     head_span[MU_SL_MAX_LEVEL];

    uint32_t     size;
    uint16_t     level;                       /* highest level in use, >= 1 */
    uint16_t     max_level;
    mu_sl_cmp    cmp;

    mu_sl_alloc_fn alloc;
    mu_sl_free_fn  dealloc;
    void          *alloc_user;

    /* free lists indexed by level; head stored inside the recycled block */
    mu_sl_node  *free_lists[MU_SL_MAX_LEVEL + 1];

    uint64_t     seed;
    uint64_t     n_allocs;
    uint64_t     n_recycles;
} mu_skiplist;

/* --------------------------------------------------------------------------
 * lifecycle
 * -------------------------------------------------------------------------- */

MU_INLINE void mu_skiplist_init(mu_skiplist *l, mu_sl_cmp cmp)
{
    uint32_t i;

    for (i = 0; i < MU_SL_MAX_LEVEL; ++i)
    {
        l->head_next[i] = NULL;
        l->head_span[i] = 0u;
    }
    for (i = 0; i <= MU_SL_MAX_LEVEL; ++i) l->free_lists[i] = NULL;

    l->head.next     = l->head_next;
    l->head.span     = l->head_span;
    l->head.key      = NULL;
    l->head.level    = (uint16_t)MU_SL_MAX_LEVEL;
    l->head.embedded = 1;

    l->size      = 0u;
    l->level     = 1u;
    l->max_level = (uint16_t)MU_SL_MAX_LEVEL;
    l->cmp       = cmp;
    l->alloc     = NULL;
    l->dealloc   = NULL;
    l->alloc_user = NULL;
    l->seed      = 0x9E3779B97F4A7C15ull;
    l->n_allocs  = 0u;
    l->n_recycles = 0u;
}

MU_INLINE void mu_skiplist_init_alloc(mu_skiplist *l, mu_sl_cmp cmp,
                                      mu_sl_alloc_fn alloc, mu_sl_free_fn dealloc, void *user)
{
    mu_skiplist_init(l, cmp);
    l->alloc = alloc;
    l->dealloc = dealloc;
    l->alloc_user = user;
}

MU_INLINE void mu_skiplist_set_seed(mu_skiplist *l, uint64_t seed)
{
    l->seed = seed ? seed : 0x9E3779B97F4A7C15ull;
}

/* Release every pooled block. Nodes still inserted are NOT touched -- the list
 * has no way to find them once you forget them, and an intrusive container
 * never owns its elements. Call mu_sl_clear first if the list is populated. */
MU_INLINE void mu_skiplist_destroy(mu_skiplist *l)
{
    uint32_t i;

    for (i = 0; i <= MU_SL_MAX_LEVEL; ++i)
    {
        mu_sl_node *n = l->free_lists[i];

        while (n)
        {
            /* the free-list link lives in the block's own first pointer slot */
            mu_sl_node *next_block = n->next ? n->next[0] : NULL;

            if (l->dealloc) l->dealloc(n, l->alloc_user);
            else            free(n);
            n = next_block;
        }
        l->free_lists[i] = NULL;
    }
    l->size = 0u;
    l->level = 1u;
}

/* --------------------------------------------------------------------------
 * level blocks
 * -------------------------------------------------------------------------- */

MU_INLINE mu_sl_node *mu_sl__block_alloc(mu_skiplist *l, uint16_t level)
{
    mu_sl_node *recycled = l->free_lists[level];
    mu_sl_node *h;
    size_t      bytes;

    if (recycled)
    {
        /* the recycled block stores its own successor in next[0] */
        l->free_lists[level] = (mu_sl_node *)recycled->next[0];
        l->n_recycles += 1u;
        h = recycled;
        h->next = (mu_sl_node **)(void *)((unsigned char *)h + sizeof(mu_sl_node));
        h->span = (uint32_t *)(void *)(h->next + level);
        return h;
    }

    bytes = sizeof(mu_sl_node) + (size_t)level * sizeof(mu_sl_node *) +
            (size_t)level * sizeof(uint32_t);
    h = (mu_sl_node *)(l->alloc ? l->alloc(bytes, l->alloc_user) : malloc(bytes));
    if (!h) return NULL;
    l->n_allocs += 1u;
    h->next = (mu_sl_node **)(void *)((unsigned char *)h + sizeof(mu_sl_node));
    h->span = (uint32_t *)(void *)(h->next + level);
    return h;
}

MU_INLINE void mu_sl__block_free(mu_skiplist *l, mu_sl_node *h)
{
    const uint16_t level = h->level;

    /* push onto the per-level free list; the block's own first pointer slot
     * holds the link, so recycling costs nothing extra */
    h->next = (mu_sl_node **)(void *)((unsigned char *)h + sizeof(mu_sl_node));
    h->next[0] = (mu_sl_node *)l->free_lists[level];
    l->free_lists[level] = h;
}

/* Point a node at caller-owned arrays: nothing is ever allocated for it, then
 * insert it with mu_sl_insert_embedded(). Two entry points rather than a flag,
 * because a flag means insert() has to read node->embedded before it has
 * written anything -- and an uninitialized mu_sl_node then decides its own
 * fate. That crashed this header during development, in the obvious way. */
MU_INLINE void mu_sl_use_embedded(mu_sl_node *node, mu_sl_node **next_arrays, uint32_t *span_arrays)
{
    node->next = next_arrays;
    node->span = span_arrays;
    node->block = NULL;
    node->embedded = 1u;
}

/* --------------------------------------------------------------------------
 * random level
 * -------------------------------------------------------------------------- */

MU_INLINE uint16_t mu_sl__random_level(mu_skiplist *l)
{
    uint64_t x = l->seed;
    uint32_t level = 1u;

    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    l->seed = x;
    x *= 2685821657736338717ull;

    /* count trailing zero groups of MU_SL_P_BITS */
    while ((x & ((1ull << MU_SL_P_BITS) - 1ull)) == 0ull && level < l->max_level)
    {
        level += 1u;
        x >>= MU_SL_P_BITS;
    }
    return (uint16_t)level;
}

/* --------------------------------------------------------------------------
 * insert
 * -------------------------------------------------------------------------- */

/* Core insert. `node` already has `level` forward/width arrays wired up. */
MU_INLINE uint32_t mu_sl__insert_core(mu_skiplist *l, mu_sl_node *node, const void *key, uint16_t level)
{
    mu_sl_node *update[MU_SL_MAX_LEVEL];
    uint32_t    rank[MU_SL_MAX_LEVEL];
    mu_sl_node *x;
    uint16_t    i;

    node->level = level;
    node->key   = key;

    x = &l->head;
    for (i = (uint16_t)l->level; i-- > 0u; )
    {
        rank[i] = (i == l->level - 1u) ? 0u : rank[i + 1u];
        /* <= 0: a run of equal keys is passed over, so a duplicate lands after
         * its equals and insertion order is preserved within a run */
        while (x->next[i] && l->cmp(x->next[i]->key, key) <= 0)
        {
            rank[i] += x->span[i];
            x = x->next[i];
        }
        update[i] = x;
    }

    if (level > l->level)
    {
        for (i = l->level; i < level; ++i)
        {
            rank[i] = 0u;
            update[i] = &l->head;
            l->head.span[i] = l->size;
        }
        l->level = level;
    }

    for (i = 0; i < level; ++i)
    {
        node->next[i] = update[i]->next[i];
        node->span[i] = update[i]->span[i] - (rank[0] - rank[i]);
        update[i]->next[i] = node;
        update[i]->span[i] = (rank[0] - rank[i]) + 1u;
    }
    for (i = level; i < l->level; ++i) update[i]->span[i] += 1u;

    l->size += 1u;
    return rank[0];
}

/* Insert with a level block taken from (or returned to) the list's pool. */
MU_INLINE uint32_t mu_sl_insert(mu_skiplist *l, mu_sl_node *node, const void *key)
{
    uint16_t    level;
    mu_sl_node *block;

    if (!l || !node) return MU_SL_NONE;
    level = mu_sl__random_level(l);
    block = mu_sl__block_alloc(l, level);
    if (!block) return MU_SL_NONE;

    node->block    = block;
    node->next     = block->next;
    node->span     = block->span;
    node->embedded = 0u;
    return mu_sl__insert_core(l, node, key, level);
}

/* Insert a node whose arrays are embedded (see mu_sl_use_embedded). Allocates
 * nothing, ever. */
MU_INLINE uint32_t mu_sl_insert_embedded(mu_skiplist *l, mu_sl_node *node, const void *key)
{
    uint16_t level;

    if (!l || !node || !node->next || !node->span) return MU_SL_NONE;
    level = mu_sl__random_level(l);
    if (level > MU_SL_MAX_LEVEL) level = (uint16_t)MU_SL_MAX_LEVEL;
    node->block    = NULL;
    node->embedded = 1u;
    return mu_sl__insert_core(l, node, key, level);
}

/* --------------------------------------------------------------------------
 * search, rank, index
 * -------------------------------------------------------------------------- */

/* First node whose key compares equal, or NULL. */
MU_INLINE mu_sl_node *mu_sl_find(const mu_skiplist *l, const void *key)
{
    const mu_sl_node *x = &l->head;
    int               i;

    for (i = (int)l->level - 1; i >= 0; --i)
        while (x->next[i] && l->cmp(x->next[i]->key, key) < 0) x = x->next[i];
    x = x->next[0];
    return (x && l->cmp(x->key, key) == 0) ? (mu_sl_node *)x : NULL;
}

/* Index of the first node whose key is not less than `key` (lower bound). */
MU_INLINE uint32_t mu_sl_rank(const mu_skiplist *l, const void *key)
{
    const mu_sl_node *x = &l->head;
    uint32_t          rank = 0u;
    int               i;

    for (i = (int)l->level - 1; i >= 0; --i)
        while (x->next[i] && l->cmp(x->next[i]->key, key) < 0)
        {
            rank += x->span[i];
            x = x->next[i];
        }
    return rank;
}

/* Node at index `index`, or NULL. This is the widths paying for themselves:
 * O(log n) random access into a linked structure. */
MU_INLINE mu_sl_node *mu_sl_at(const mu_skiplist *l, uint32_t index)
{
    const mu_sl_node *x = &l->head;
    uint32_t          before = 0u;     /* 1-based rank of x; the sentinel is 0 */
    int               i;

    if (index >= l->size) return NULL;

    /* Width semantics: x->next[i] is the element at 0-based index
     * `before + span[i] - 1`, because the sentinel precedes element 0 and its
     * level-0 width is 1. Getting this off by one silently returns the
     * sentinel for index 0 -- which is exactly what it did first. */
    for (i = (int)l->level - 1; i >= 0; --i)
        while (x->next[i] && before + x->span[i] <= index + 1u)
        {
            before += x->span[i];
            x = x->next[i];
        }
    /* `before` is a 1-based rank (the sentinel ranks 0), so index i is rank
     * i + 1. This is the second halves of the same off-by-one. */
    return (before == index + 1u) ? (mu_sl_node *)x : NULL;
}

/* Index of a node you already hold, by identity. O(log n) to the first equal
 * key, then a walk across that key's run -- bounded by the duplicate count. */
MU_INLINE uint32_t mu_sl_index_of(const mu_skiplist *l, const mu_sl_node *node)
{
    const mu_sl_node *x = &l->head;
    const mu_sl_node *n;
    uint32_t          rank = 0u;
    int               i;

    if (!node) return MU_SL_NONE;

    /* strict <: land just before the whole run of equal keys, then walk it at
     * level 0 comparing identity. The walk is bounded by the duplicate count. */
    for (i = (int)l->level - 1; i >= 0; --i)
        while (x->next[i] && l->cmp(x->next[i]->key, node->key) < 0)
        {
            rank += x->span[i];
            x = x->next[i];
        }

    n = x->next[0];
    while (n && n != node && l->cmp(n->key, node->key) == 0)
    {
        n = n->next[0];
        rank += 1u;
    }
    return (n == node) ? rank : MU_SL_NONE;
}

MU_INLINE uint32_t mu_sl_size(const mu_skiplist *l) { return l->size; }

MU_INLINE mu_sl_node *mu_sl_first(const mu_skiplist *l) { return l->head_next[0]; }

MU_INLINE mu_sl_node *mu_sl_next(const mu_sl_node *node) { return node ? node->next[0] : NULL; }

/* --------------------------------------------------------------------------
 * remove
 * -------------------------------------------------------------------------- */

MU_INLINE int mu_sl_remove(mu_skiplist *l, mu_sl_node *node)
{
    mu_sl_node *update[MU_SL_MAX_LEVEL];
    mu_sl_node *x;
    uint32_t    target, traversed = 0u;
    uint16_t     i;

    if (!l || !node || !l->size) return 0;

    /* Remove BY RANK, in two O(log n) passes.
     *
     * The obvious alternative -- descend by key, then patch spans at every
     * level -- is ambiguous as soon as duplicates exist: key order does not say
     * WHICH of the equal keys you are removing, so at a level where the node is
     * not the immediate successor you cannot tell whether the span shrinks.
     * The rank has no such ambiguity: it identifies the element exactly, and
     * `traversed + span <= target` either passes the node or does not. */
    target = mu_sl_index_of(l, node);
    if (target == MU_SL_NONE) return 0;

    /* `<= target`, not `<= target + 1`: this descent must stop BEFORE the
     * target so update[] holds its predecessors. Landing on it, update[0]
     * becomes the node itself, the relink writes a node's own successor back
     * into itself, and the node stays linked while claiming to be removed --
     * which is exactly what the first version did. */
    x = &l->head;
    for (i = (uint16_t)l->level; i-- > 0u; )
    {
        while (x->next[i] && traversed + x->span[i] <= target)
        {
            traversed += x->span[i];
            x = x->next[i];
        }
        update[i] = x;
    }
    /* update[0] is the predecessor, so the target is its level-0 successor */
    if (x->next[0] != node) return 0;    /* this node is not in this list */

    for (i = 0; i < l->level; ++i)
    {
        if (update[i]->next[i] == node)
        {
            /* the pointer skips over the node: hand on its width, minus the
             * node itself */
            update[i]->span[i] += node->span[i] - 1u;
            update[i]->next[i] = node->next[i];
        }
        else
        {
            /* the node sits inside this span but is not its endpoint */
            update[i]->span[i] -= 1u;
        }
    }

    while (l->level > 1u && l->head_next[l->level - 1u] == NULL) l->level -= 1u;
    l->size -= 1u;

    if (!node->embedded && node->block)
    {
        mu_sl_node *block = (mu_sl_node *)node->block;

        block->level = node->level;
        mu_sl__block_free(l, block);
        node->block = NULL;
        node->next = NULL;
        node->span = NULL;
    }
    return 1;
}

/* Detach everything. Nodes are left untouched; only the pooled blocks return. */
MU_INLINE void mu_sl_clear(mu_skiplist *l)
{
    uint32_t i;

    for (i = 0; i < MU_SL_MAX_LEVEL; ++i)
    {
        l->head_next[i] = NULL;
        l->head_span[i] = 0u;
    }
    l->size = 0u;
    l->level = 1u;
}

/* --------------------------------------------------------------------------
 * introspection (for the gate and the perf record)
 * -------------------------------------------------------------------------- */

typedef struct mu_sl_stats
{
    uint32_t size;
    uint16_t level;
    uint16_t max_level;
    uint64_t allocs;
    uint64_t recycles;
    uint32_t pooled_blocks;
} mu_sl_stats;

MU_INLINE mu_sl_stats mu_sl_get_stats(const mu_skiplist *l)
{
    mu_sl_stats s;
    uint32_t    i;

    s.size = l->size;
    s.level = l->level;
    s.max_level = l->max_level;
    s.allocs = l->n_allocs;
    s.recycles = l->n_recycles;
    s.pooled_blocks = 0u;
    for (i = 0; i <= MU_SL_MAX_LEVEL; ++i)
    {
        mu_sl_node *n = l->free_lists[i];

        while (n) { s.pooled_blocks += 1u; n = (mu_sl_node *)n->next[0]; }
    }
    return s;
}

/* Verify the width invariant end to end: spans at level 0 must all be 1, every
 * level-i pointer must land on a node of level > i, and the widths must sum to
 * the list size. This is the structural self-check worth running after a fuzz
 * session; it is O(n) and not on any hot path. */
MU_INLINE int mu_sl_validate(const mu_skiplist *l)
{
    const mu_sl_node *x;
    int               i;

    /* 1. level 0: a plain linked list of exactly `size` nodes, sorted, and
     *    every level-0 width is 1 */
    {
        uint32_t n = 0u;

        x = &l->head;
        while (x->next[0])
        {
            if (x->span[0] != 1u) return 0;
            /* the sentinel carries no key: never hand it to the comparator */
            if (x != &l->head && l->cmp(x->next[0]->key, x->key) < 0) return 0;
            x = x->next[0];
            n += 1u;
        }
        /* The trailing width of the last node is unused (this header leaves it
         * at 0); only widths that actually point somewhere are requirements. */
        if (n != l->size) return 0;
    }

    /* 2. every level: the widths that lead somewhere must sum to `size`, and
     *    any node linked at level i must have level > i (otherwise it is not
     *    tall enough to be reachable there) */
    for (i = (int)l->level - 1; i >= 0; --i)
    {
        uint32_t total = 0u;

        x = &l->head;
        while (x->next[i])
        {
            if (x->next[i]->level <= (uint16_t)i) return 0;
            total += x->span[i];
            x = x->next[i];
        }
        /* The trailing width counts too: at level i the walk usually ends
         * before the end of the list, so the last edge's width is what carries
         * the remaining elements. Excluding it made a 64-element list report
         * 61 at level 1 and "fail to validate" while being perfectly correct. */
        total += x->span[i];
        if (l->size > 0u && total != l->size) return 0;
    }

    if (l->level > l->max_level) return 0;
    return 1;
}

MU_END_EXTERN_C

#endif /* MU_SKIPLIST_H */
