#pragma once
#ifndef MU_SLAB_H
#define MU_SLAB_H

#include "mu_common.h"
#include "mu_sync.h"

/* ---------------------------------------------------------------------------
 * mu_slab -- fixed-size block allocator with per-thread free lists.
 *
 * WHY
 * ---
 * A general-purpose malloc pays for size classes, locks and metadata on every
 * allocation. Game/engine code usually has a handful of hot object sizes
 * (nodes, components, command packets) that live, die and are recycled in
 * bulk. A slab serves those without a general heap call per object.
 *
 * LAYOUT
 * ------
 * A parent owns pages and the global free list:
 *
 *     mu_slab_parent
 *       pages      -> [ page | item 0 | item 1 | ... | item N-1 ]
 *                         |
 *                         +-> next page
 *
 *       free_list  -> item -> item -> ...
 *
 * A child (normally one per thread) caches a batch of free items locally, so
 * the hot alloc/free path never touches the parent lock:
 *
 *     mu_slab_child
 *       free_list -> item -> item -> ...
 *
 * Every item starts with a 16-byte header. The user pointer is immediately
 * after it, so a user object is aligned to 16 bytes as long as the page base
 * is. The header's `next` link is the only metadata needed while free.
 *
 * THREADING
 * ---------
 *   - A `mu_slab_child` is NOT thread-safe: one thread at a time per child.
 *   - Different children are independent; they only meet under the parent
 *     spinlock when refilling or being destroyed.
 *   - `mu_slab_free_shared()` may be called from any thread for an object
 *     with no child (IO thread, deferred cleanup, teardown).
 *   - `mu_slab_parent_destroy()` must not race with any child.
 *
 * CONTRACT
 * --------
 *   - `elem_size` and `items_per_page` are fixed at parent init.
 *   - Do not free the same object twice.
 *   - Destroy children before the parent.
 *   - This allocator does not track which child owns a block. Freeing a block
 *     from a different child is fine as long as neither child is accessed
 *     concurrently by two threads.
 * --------------------------------------------------------------------------- */

#define MU_SLAB_ALIGNMENT 16u

typedef struct mu_slab_element
{
    struct mu_slab_element* next;
    uint64_t                _pad; /* keep the user payload 16-byte aligned */
} mu_slab_element;

typedef struct mu_slab_page
{
    void*                raw;  /* original malloc pointer (page may be aligned up) */
    struct mu_slab_page* next;
} mu_slab_page;

typedef struct mu_slab_parent
{
    mu_spinlock      lock;
    mu_slab_element* free_list;
    mu_slab_page*    pages;
    size_t           elem_size;
    size_t           item_size;      /* header + payload, rounded to 16 */
    uint32_t         items_per_page;
} mu_slab_parent;

typedef struct mu_slab_child
{
    mu_slab_parent*  parent;
    mu_slab_element* free_list;
    uint32_t         local_count;
    uint32_t         batch;
} mu_slab_child;

MU_INLINE size_t mu_slab__align_up(size_t x)
{
    return (x + (MU_SLAB_ALIGNMENT - 1u)) & ~(size_t)(MU_SLAB_ALIGNMENT - 1u);
}

MU_INLINE size_t mu_slab__page_header(void)
{
    return mu_slab__align_up(sizeof(mu_slab_page));
}

MU_INLINE bool mu_slab_parent_init(mu_slab_parent* parent, size_t elem_size,
                                   uint32_t items_per_page)
{
    size_t item_size;

    if (!parent || elem_size == 0 || items_per_page == 0)
        return false;

    if (elem_size > (size_t)-1 - sizeof(mu_slab_element)
                             - (MU_SLAB_ALIGNMENT - 1u))
        return false;

    item_size = mu_slab__align_up(sizeof(mu_slab_element) + elem_size);
    if (item_size < elem_size)
        return false;

    mu_spinlock_init(&parent->lock);
    parent->free_list      = NULL;
    parent->pages          = NULL;
    parent->elem_size      = elem_size;
    parent->item_size      = item_size;
    parent->items_per_page = items_per_page;
    return true;
}

MU_INLINE mu_slab_page* mu_slab__add_page(mu_slab_parent* parent)
{
    size_t header = mu_slab__page_header();
    size_t count  = (size_t)parent->items_per_page;
    size_t bytes;
    void* raw;
    mu_slab_page* page;
    uint8_t* item_base;
    uint32_t i;

    if (count > ((size_t)-1 - (MU_SLAB_ALIGNMENT - 1u) - header) / parent->item_size)
        return NULL;

    /* Extra alignment slack lets us align the page itself, not just the
       payload, so all item addresses share the same 16-byte phase. */
    bytes = (MU_SLAB_ALIGNMENT - 1u) + header + count * parent->item_size;
    raw = MU_MALLOC(bytes);
    if (!raw)
        return NULL;

    page = (mu_slab_page*)(((uintptr_t)raw + (MU_SLAB_ALIGNMENT - 1u))
                           & ~(uintptr_t)(MU_SLAB_ALIGNMENT - 1u));
    page->raw  = raw;
    page->next = parent->pages;
    parent->pages = page;

    item_base = (uint8_t*)page + header;
    for (i = 0; i < parent->items_per_page; ++i)
    {
        mu_slab_element* item =
            (mu_slab_element*)(item_base + (size_t)i * parent->item_size);
        item->next = parent->free_list;
        parent->free_list = item;
    }

    return page;
}

MU_INLINE void mu_slab_parent_destroy(mu_slab_parent* parent)
{
    mu_slab_page* page;

    if (!parent)
        return;

    page = parent->pages;
    while (page)
    {
        mu_slab_page* next = page->next;
        MU_FREE(page->raw);
        page = next;
    }

    parent->free_list      = NULL;
    parent->pages          = NULL;
    parent->elem_size      = 0;
    parent->item_size      = 0;
    parent->items_per_page = 0;
}

MU_INLINE bool mu_slab_child_init_batch(mu_slab_child* child, mu_slab_parent* parent,
                                        uint32_t batch)
{
    if (!child || !parent || parent->item_size == 0)
        return false;

    child->parent      = parent;
    child->free_list   = NULL;
    child->local_count = 0;
    child->batch       = batch ? batch : parent->items_per_page;

    if (child->batch > parent->items_per_page)
        child->batch = parent->items_per_page;

    return true;
}

MU_INLINE bool mu_slab_child_init(mu_slab_child* child, mu_slab_parent* parent)
{
    return mu_slab_child_init_batch(child, parent, 16u);
}

MU_INLINE bool mu_slab__child_refill(mu_slab_child* child)
{
    mu_slab_parent* parent = child->parent;
    uint32_t want;

    mu_spinlock_lock(&parent->lock);

    if (!parent->free_list && !mu_slab__add_page(parent))
    {
        mu_spinlock_unlock(&parent->lock);
        return false;
    }

    want = child->batch;
    while (want-- > 0 && parent->free_list)
    {
        mu_slab_element* item = parent->free_list;
        parent->free_list = item->next;
        item->next = child->free_list;
        child->free_list = item;
        ++child->local_count;
    }

    mu_spinlock_unlock(&parent->lock);
    return child->free_list != NULL;
}

MU_INLINE void* mu_slab_alloc(mu_slab_child* child)
{
    mu_slab_element* item;

    if (!child || !child->parent)
        return NULL;

    if (!child->free_list && !mu_slab__child_refill(child))
        return NULL;

    item = child->free_list;
    child->free_list = item->next;
    --child->local_count;
    item->next = NULL;

    return (uint8_t*)item + sizeof(mu_slab_element);
}

MU_INLINE void* mu_slab_zalloc(mu_slab_child* child)
{
    void* ptr = mu_slab_alloc(child);
    if (ptr)
        MU_MEMSET(ptr, 0, child->parent->elem_size);
    return ptr;
}

/* Free through a child. Any block from this parent may be used; no ownership
   check is performed. The child must not be accessed concurrently. */
MU_INLINE void mu_slab_free(mu_slab_child* child, void* ptr)
{
    mu_slab_element* item;

    if (!child || !child->parent || !ptr)
        return;

    item = (mu_slab_element*)((uint8_t*)ptr - sizeof(mu_slab_element));
    item->next = child->free_list;
    child->free_list = item;
    ++child->local_count;
}

/* Thread-safe path for objects with no owning child. */
MU_INLINE void mu_slab_free_shared(mu_slab_parent* parent, void* ptr)
{
    mu_slab_element* item;

    if (!parent || !ptr)
        return;

    item = (mu_slab_element*)((uint8_t*)ptr - sizeof(mu_slab_element));

    mu_spinlock_lock(&parent->lock);
    item->next = parent->free_list;
    parent->free_list = item;
    mu_spinlock_unlock(&parent->lock);
}

/* Return this child's cached blocks to the parent. Safe to call at thread
   shutdown; the child becomes unusable until re-initialised. */
MU_INLINE void mu_slab_child_destroy(mu_slab_child* child)
{
    mu_slab_parent* parent;

    if (!child || !child->parent)
        return;

    parent = child->parent;

    if (child->free_list)
    {
        mu_slab_element* tail = child->free_list;
        while (tail->next)
            tail = tail->next;

        mu_spinlock_lock(&parent->lock);
        tail->next = parent->free_list;
        parent->free_list = child->free_list;
        mu_spinlock_unlock(&parent->lock);
    }

    child->free_list   = NULL;
    child->local_count = 0;
    child->parent      = NULL;
}

#endif /* MU_SLAB_H */
