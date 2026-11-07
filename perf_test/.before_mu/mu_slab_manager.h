#pragma once
#ifndef MU_SLAB_MANAGER_H
#define MU_SLAB_MANAGER_H

/*
 * mu_slab_manager.h -- multi-heap, size-classed, reclaimable slab manager.
 *
 * Unlike mu_slab (one fixed object size), the manager handles many sizes and
 * multiple opaque heaps. Freed entries enter a reclaim list and are only
 * reused when the caller's can_reclaim callback says the backing storage is
 * safe (for example, after a GPU fence has passed).
 *
 * The caller owns slab and entry storage. The manager only links descriptors.
 *
 * Ported from Mesa util/pb_slab.{h,c} (MIT).
 */

#include "mu_common.h"
#include "mu_sync.h"

MU_BEGIN_EXTERN_C

typedef struct mu_slab_manager_link
{
    struct mu_slab_manager_link* prev;
    struct mu_slab_manager_link* next;
} mu_slab_manager_link;

struct mu_slab_manager_slab;

typedef struct mu_slab_manager_entry
{
    mu_slab_manager_link link;
    struct mu_slab_manager_slab* slab;
} mu_slab_manager_entry;

typedef struct mu_slab_manager_slab
{
    mu_slab_manager_link link;
    mu_slab_manager_link free;
    unsigned num_free;
    unsigned num_entries;
    unsigned group_index;
    unsigned entry_size;
} mu_slab_manager_slab;

typedef struct mu_slab_manager_group
{
    mu_slab_manager_link slabs;
} mu_slab_manager_group;

typedef mu_slab_manager_slab* (*mu_slab_manager_alloc_fn)(
    void* priv, unsigned heap, unsigned entry_size, unsigned group_index);
typedef void (*mu_slab_manager_free_fn)(void* priv, mu_slab_manager_slab* slab);
typedef bool (*mu_slab_manager_can_reclaim_fn)(void* priv, mu_slab_manager_entry* entry);

typedef struct mu_slab_manager
{
    mu_spinlock lock;
    unsigned min_order;
    unsigned num_orders;
    unsigned num_heaps;
    bool allow_three_fourths;
    mu_slab_manager_group* groups;
    mu_slab_manager_link reclaim;
    void* priv;
    mu_slab_manager_can_reclaim_fn can_reclaim;
    mu_slab_manager_alloc_fn slab_alloc;
    mu_slab_manager_free_fn slab_free;
} mu_slab_manager;

MU_INLINE void mu_slab_manager__link_init(mu_slab_manager_link* link)
{
    link->prev = link;
    link->next = link;
}

MU_INLINE void mu_slab_manager__link_clear(mu_slab_manager_link* link)
{
    link->prev = NULL;
    link->next = NULL;
}

MU_INLINE bool mu_slab_manager__link_is_linked(const mu_slab_manager_link* link)
{
    return link->prev != NULL;
}

MU_INLINE bool mu_slab_manager__link_empty(const mu_slab_manager_link* head)
{
    return head->next == head;
}

MU_INLINE void mu_slab_manager__link_add_tail(mu_slab_manager_link* head,
                                              mu_slab_manager_link* link)
{
    link->prev = head->prev;
    link->next = head;
    head->prev->next = link;
    head->prev = link;
}

MU_INLINE void mu_slab_manager__link_remove(mu_slab_manager_link* link)
{
    link->prev->next = link->next;
    link->next->prev = link->prev;
    link->prev = NULL;
    link->next = NULL;
}

MU_INLINE mu_slab_manager_entry* mu_slab_manager__entry(mu_slab_manager_link* link)
{
    return MU_CONTAINER_OF(link, mu_slab_manager_entry, link);
}

MU_INLINE mu_slab_manager_slab* mu_slab_manager__slab(mu_slab_manager_link* link)
{
    return MU_CONTAINER_OF(link, mu_slab_manager_slab, link);
}

MU_INLINE unsigned mu_slab_manager__ceil_log2(unsigned value)
{
    unsigned order = 0;
    if (value > 1)
    {
        --value;
        while (value)
        {
            value >>= 1;
            ++order;
        }
    }
    return order;
}

MU_INLINE void mu_slab_manager_slab_init(mu_slab_manager_slab* slab,
                                         unsigned group_index,
                                         unsigned entry_size,
                                         unsigned num_entries)
{
    mu_slab_manager__link_clear(&slab->link);
    mu_slab_manager__link_init(&slab->free);
    slab->num_free = 0;
    slab->num_entries = num_entries;
    slab->group_index = group_index;
    slab->entry_size = entry_size;
}

MU_INLINE void mu_slab_manager_entry_init(mu_slab_manager_entry* entry,
                                          mu_slab_manager_slab* slab)
{
    mu_slab_manager__link_clear(&entry->link);
    entry->slab = slab;
}

MU_INLINE void mu_slab_manager_slab_add_entry(mu_slab_manager_slab* slab,
                                              mu_slab_manager_entry* entry)
{
    MU_ASSERT(slab && entry);
    MU_ASSERT(slab->num_free < slab->num_entries);
    mu_slab_manager_entry_init(entry, slab);
    mu_slab_manager__link_add_tail(&slab->free, &entry->link);
    ++slab->num_free;
}

MU_INLINE void mu_slab_manager__reclaim_locked(mu_slab_manager* mgr,
                                               mu_slab_manager_entry* entry)
{
    mu_slab_manager_slab* slab = entry->slab;

    mu_slab_manager__link_remove(&entry->link);
    mu_slab_manager__link_add_tail(&slab->free, &entry->link);
    ++slab->num_free;

    if (!mu_slab_manager__link_is_linked(&slab->link))
    {
        mu_slab_manager_group* group = &mgr->groups[slab->group_index];
        mu_slab_manager__link_add_tail(&group->slabs, &slab->link);
    }

    if (slab->num_free >= slab->num_entries)
    {
        mu_slab_manager__link_remove(&slab->link);
        mgr->slab_free(mgr->priv, slab);
    }
}

MU_INLINE unsigned mu_slab_manager__reclaim_scan_locked(mu_slab_manager* mgr,
                                                        bool reclaim_all)
{
    mu_slab_manager_link* link = mgr->reclaim.next;
    unsigned failed = 0;
    unsigned reclaimed = 0;

    while (link != &mgr->reclaim)
    {
        mu_slab_manager_link* next = link->next;
        mu_slab_manager_entry* entry = mu_slab_manager__entry(link);

        if (mgr->can_reclaim(mgr->priv, entry))
        {
            mu_slab_manager__reclaim_locked(mgr, entry);
            ++reclaimed;
        }
        else if (!reclaim_all && ++failed >= 2u)
        {
            break;
        }

        link = next;
    }

    return reclaimed;
}

MU_INLINE mu_slab_manager_entry* mu_slab_manager_alloc_reclaimed(
    mu_slab_manager* mgr, unsigned size, unsigned heap, bool reclaim_all)
{
    unsigned order;
    unsigned entry_size;
    unsigned group_index;
    bool three_fourths = false;
    mu_slab_manager_group* group;
    mu_slab_manager_slab* slab;
    mu_slab_manager_entry* entry;

    MU_ASSERT(mgr && size > 0);
    MU_ASSERT(heap < mgr->num_heaps);

    order = mu_slab_manager__ceil_log2(size);
    if (order < mgr->min_order)
        order = mgr->min_order;
    MU_ASSERT(order < mgr->min_order + mgr->num_orders);

    entry_size = 1u << order;
    if (mgr->allow_three_fourths && size <= entry_size * 3u / 4u)
    {
        entry_size = entry_size * 3u / 4u;
        three_fourths = true;
    }

    group_index = (heap * mgr->num_orders + (order - mgr->min_order))
                * (1u + (mgr->allow_three_fourths ? 1u : 0u))
                + (three_fourths ? 1u : 0u);
    group = &mgr->groups[group_index];

    mu_spinlock_lock(&mgr->lock);

    if (mu_slab_manager__link_empty(&group->slabs) ||
        mu_slab_manager__link_empty(
            &mu_slab_manager__slab(group->slabs.next)->free))
    {
        mu_slab_manager__reclaim_scan_locked(mgr, reclaim_all);
    }

    while (!mu_slab_manager__link_empty(&group->slabs))
    {
        slab = mu_slab_manager__slab(group->slabs.next);
        if (!mu_slab_manager__link_empty(&slab->free))
            break;
        mu_slab_manager__link_remove(&slab->link);
    }

    if (mu_slab_manager__link_empty(&group->slabs))
    {
        mu_spinlock_unlock(&mgr->lock);
        slab = mgr->slab_alloc(mgr->priv, heap, entry_size, group_index);
        if (!slab)
            return NULL;
        mu_spinlock_lock(&mgr->lock);
        mu_slab_manager__link_add_tail(&group->slabs, &slab->link);
    }

    slab = mu_slab_manager__slab(group->slabs.next);
    MU_ASSERT(slab->num_free > 0);

    entry = mu_slab_manager__entry(slab->free.next);
    mu_slab_manager__link_remove(&entry->link);
    --slab->num_free;

    mu_spinlock_unlock(&mgr->lock);
    return entry;
}

MU_INLINE mu_slab_manager_entry* mu_slab_manager_alloc(mu_slab_manager* mgr,
                                                       unsigned size,
                                                       unsigned heap)
{
    return mu_slab_manager_alloc_reclaimed(mgr, size, heap, false);
}

MU_INLINE void mu_slab_manager_free(mu_slab_manager* mgr,
                                    mu_slab_manager_entry* entry)
{
    MU_ASSERT(mgr && entry);
    mu_spinlock_lock(&mgr->lock);
    mu_slab_manager__link_add_tail(&mgr->reclaim, &entry->link);
    mu_spinlock_unlock(&mgr->lock);
}

MU_INLINE unsigned mu_slab_manager_reclaim(mu_slab_manager* mgr)
{
    unsigned reclaimed;
    MU_ASSERT(mgr);
    mu_spinlock_lock(&mgr->lock);
    reclaimed = mu_slab_manager__reclaim_scan_locked(mgr, false);
    mu_spinlock_unlock(&mgr->lock);
    return reclaimed;
}

MU_INLINE bool mu_slab_manager_init(mu_slab_manager* mgr,
                                    unsigned min_order,
                                    unsigned max_order,
                                    unsigned num_heaps,
                                    bool allow_three_fourths,
                                    void* priv,
                                    mu_slab_manager_can_reclaim_fn can_reclaim,
                                    mu_slab_manager_alloc_fn slab_alloc,
                                    mu_slab_manager_free_fn slab_free)
{
    unsigned num_groups;
    unsigned i;

    MU_ASSERT(mgr != NULL);
    if (!can_reclaim || !slab_alloc || !slab_free || num_heaps == 0)
        return false;
    if (min_order > max_order || max_order >= 31u)
        return false;

    mgr->min_order = min_order;
    mgr->num_orders = max_order - min_order + 1u;
    mgr->num_heaps = num_heaps;
    mgr->allow_three_fourths = allow_three_fourths;
    mgr->priv = priv;
    mgr->can_reclaim = can_reclaim;
    mgr->slab_alloc = slab_alloc;
    mgr->slab_free = slab_free;

    mu_spinlock_init(&mgr->lock);
    mu_slab_manager__link_init(&mgr->reclaim);

    num_groups = mgr->num_orders * num_heaps
               * (1u + (allow_three_fourths ? 1u : 0u));
    mgr->groups = (mu_slab_manager_group*)MU_CALLOC(num_groups, sizeof(*mgr->groups));
    if (!mgr->groups)
        return false;

    for (i = 0; i < num_groups; ++i)
        mu_slab_manager__link_init(&mgr->groups[i].slabs);

    return true;
}

MU_INLINE void mu_slab_manager_deinit(mu_slab_manager* mgr)
{
    MU_ASSERT(mgr != NULL);
    if (!mgr->groups)
        return;

    while (!mu_slab_manager__link_empty(&mgr->reclaim))
    {
        mu_slab_manager_entry* entry =
            mu_slab_manager__entry(mgr->reclaim.next);
        mu_slab_manager__reclaim_locked(mgr, entry);
    }

    MU_FREE(mgr->groups);
    mgr->groups = NULL;
    mgr->num_orders = 0;
    mgr->num_heaps = 0;
}

MU_END_EXTERN_C

#endif /* MU_SLAB_MANAGER_H */
