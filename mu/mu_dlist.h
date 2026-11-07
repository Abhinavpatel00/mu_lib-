#pragma once
#ifndef MU_DLIST_H
#define MU_DLIST_H

/* ==========================================================================
 * mu_dlist.h — intrusive doubly-linked list
 * ==========================================================================
 *
 * A list with no nodes of its own: you embed `mu_dlink` in the object and the
 * object IS the node. Consequences, in the order they matter:
 *
 *   - Erase is O(1) FROM THE OBJECT. You never search for the link, because you
 *     are holding it. A container that owns its nodes forces `find then erase`,
 *     i.e. O(n) in a list.
 *   - Zero allocations after construction. A non-intrusive list allocates one
 *     node per element; an intrusive one allocates nothing, ever.
 *   - One pointer chase per element instead of two. A heap-node list walks
 *     node -> payload -> next: two dependent loads, two possible cache misses.
 *     Here next and the payload are in the same cache line.
 *   - No partial-failure paths. push_back cannot fail, so there is no
 *     "inserted but the caller must clean up" branch.
 *
 * The cost is that an object can be in only one list per embedded link, and
 * that removing an object invalidates nothing else -- which is what you wanted.
 * If an object must live in several lists at once, embed one mu_dlink per list.
 *
 * Storage: 16 bytes per link (two pointers, so it can be tagged later). The
 * list itself is one sentinel link, so an empty list needs no allocation and a
 * stack-allocated list is a complete list.
 *
 * All operations are O(1) except iteration; size is maintained as a counter so
 * it is O(1) too, at the cost of 8 bytes per list.
 */

#include <stddef.h>
#include <stdint.h>

#include "mu_macros.h"

MU_BEGIN_EXTERN_C

typedef struct mu_dlink
{
    struct mu_dlink *next;
    struct mu_dlink *prev;
} mu_dlink;

typedef struct mu_dlist
{
    mu_dlink head;      /* sentinel: head.next is the first element */
    uint32_t size;
} mu_dlist;

/* Offset of a link inside its containing object. The standard trick: pointer
 * arithmetic back from the link to the payload, which is what makes iteration
 * yield whatever type the caller embedded. */
#define mu_container_of(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

MU_INLINE void mu_dlist_init(mu_dlist *l)
{
    l->head.next = &l->head;
    l->head.prev = &l->head;
    l->size = 0u;
}

MU_INLINE void mu_dlist_init_link(mu_dlink *n)
{
    n->next = n;
    n->prev = n;
}

MU_INLINE int mu_dlist_empty(const mu_dlist *l) { return l->head.next == &l->head; }
MU_INLINE uint32_t mu_dlist_size(const mu_dlist *l) { return l->size; }

/* Is this link currently in a list? O(1), and safe on a zeroed link. */
MU_INLINE int mu_dlist_is_linked(const mu_dlink *n)
{
    return n->next != NULL && n->prev != NULL;
}

/* Push at the back. Cannot fail: no allocation, no growth. */
MU_INLINE void mu_dlist_push_back(mu_dlist *l, mu_dlink *n)
{
    n->prev = l->head.prev;
    n->next = &l->head;
    l->head.prev->next = n;
    l->head.prev = n;
    l->size += 1u;
}

MU_INLINE void mu_dlist_push_front(mu_dlist *l, mu_dlink *n)
{
    n->next = l->head.next;
    n->prev = &l->head;
    l->head.next->prev = n;
    l->head.next = n;
    l->size += 1u;
}

/* Unlink in O(1) -- this is the operation the intrusive layout buys you. */
MU_INLINE void mu_dlist_erase(mu_dlist *l, mu_dlink *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    mu_dlist_init_link(n);
    l->size -= 1u;
}

MU_INLINE mu_dlink *mu_dlist_front(const mu_dlist *l)
{
    return mu_dlist_empty(l) ? NULL : l->head.next;
}

MU_INLINE mu_dlink *mu_dlist_back(const mu_dlist *l)
{
    return mu_dlist_empty(l) ? NULL : l->head.prev;
}

MU_INLINE mu_dlink *mu_dlist_next(const mu_dlist *l, const mu_dlink *n)
{
    return (n->next == &l->head) ? NULL : n->next;
}

MU_INLINE mu_dlink *mu_dlist_prev(const mu_dlist *l, const mu_dlink *n)
{
    return (n->prev == &l->head) ? NULL : n->prev;
}

/* Move a node that is already in `l` to the back without unlinking and
 * relinking by hand (useful for LRU lists). */
MU_INLINE void mu_dlist_move_back(mu_dlist *l, mu_dlink *n)
{
    if (n->next == &l->head) return;
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = l->head.prev;
    n->next = &l->head;
    l->head.prev->next = n;
    l->head.prev = n;
}

/* Splice every element of `src` onto the back of `dst`, O(1). */
MU_INLINE void mu_dlist_splice_back(mu_dlist *dst, mu_dlist *src)
{
    mu_dlink *first, *last;

    if (mu_dlist_empty(src)) return;
    first = src->head.next;
    last = src->head.prev;
    first->prev = dst->head.prev;
    dst->head.prev->next = first;
    last->next = &dst->head;
    dst->head.prev = last;
    dst->size += src->size;
    mu_dlist_init(src);
}

/* Iteration. Usage:
 *     for (mu_dlink *it = mu_dlist_front(&l); it; it = mu_dlist_next(&l, it)) { ... }
 * or, to get the payload back:
 *     item *x = mu_container_of(it, item, link);
 */

MU_END_EXTERN_C

#endif /* MU_DLIST_H */
