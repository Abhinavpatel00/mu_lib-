#pragma once
#ifndef MU_SPARSE_ARRAY_H
#define MU_SPARSE_ARRAY_H

#include "mu_common.h"
#include "mu_sync.h"

/* ---------------------------------------------------------------------------
 * mu_sparse_array -- paged, 64-bit-indexed sparse array with stable addresses.
 *
 * WHY
 * ---
 * `mu_sparse_set` is excellent when ids are dense and bounded by a known
 * capacity. Worlds, asset registries and handles are often neither. A 64-bit
 * id should not force a 2^64-entry allocation, and an element should not move
 * because some unrelated id was touched.
 *
 * LAYOUT
 * ------
 * The array is a tree of fixed-size nodes. Internal nodes hold `node_size`
 * child pointers. The leaves are data pages holding `node_size` elements.
 *
 *     root (internal, level N)
 *       |
 *       +-- child[0] -> internal level N-1
 *       +-- child[1] -> ...
 *       +-- ...
 *                   |
 *                   +-- child[k] -> data page [ node_size * elem_size bytes ]
 *
 * The root is always an internal node (level >= 1), so data pages are never
 * mistaken for nodes. A root level of L covers `node_size^(L+1)` indices.
 *
 * PROPERTIES
 * ----------
 *   - O(1)..O(level) lookup; level is at most ceil(64 / log2(node_size)).
 *   - Elements never move after allocation.
 *   - Reads are lock-free and safe from multiple threads.
 *   - Growth is serialized by a spinlock; new nodes are published with
 *     release stores after they are fully initialised.
 *   - Newly allocated elements are zeroed.
 *
 * NOT FOR
 * -------
 *   - Hard real-time hot loops that must never allocate. Warm the array first
 *     or use a dense table.
 *   - Iteration over all elements. This is an index, not a container.
 *
 * CONTRACT
 * --------
 *   - `mu_sparse_array_destroy()` / `clear()` must not race with readers.
 *   - `elem_size` and `node_size` are fixed for the array's lifetime.
 * --------------------------------------------------------------------------- */

typedef struct mu_sparse_array_node
{
    uint32_t level;      /* 0 = data page (never a root), >= 1 = internal */
    uint32_t _pad;
    mu_atomicptr_t child[1];
} mu_sparse_array_node;

typedef struct mu_sparse_array
{
    size_t          elem_size;
    uint32_t        node_size;
    uint32_t        node_size_log2;
    uint32_t        node_mask;
    mu_atomicptr_t  root;
    mu_spinlock     grow_lock;
} mu_sparse_array;

MU_INLINE uint32_t mu_sparse_array__log2(uint32_t x)
{
    uint32_t l = 0;
    while ((1u << l) < x)
        ++l;
    return l;
}

/* True when a root at `level` can address `idx`. */
MU_INLINE bool mu_sparse_array__covers(uint32_t level, uint32_t log2, uint64_t idx)
{
    uint32_t bits = (level + 1u) * log2;
    if (bits >= 64u)
        return true;
    return (idx >> bits) == 0;
}

MU_INLINE mu_sparse_array_node* mu_sparse_array__node_new(uint32_t node_size, uint32_t level)
{
    mu_sparse_array_node* node;
    size_t bytes;

    if (node_size == 0)
        return NULL;

#if SIZE_MAX <= UINT32_MAX
    if ((size_t)(node_size - 1u) > ((size_t)-1 - sizeof(mu_sparse_array_node))
                                    / sizeof(mu_atomicptr_t))
        return NULL;
#endif

    bytes = sizeof(mu_sparse_array_node)
          + (size_t)(node_size - 1u) * sizeof(mu_atomicptr_t);

    node = (mu_sparse_array_node*)MU_CALLOC(1, bytes);
    if (!node)
        return NULL;
    node->level = level;
    return node;
}

MU_INLINE void mu_sparse_array__node_free(mu_sparse_array_node* node, uint32_t node_size)
{
    uint32_t i;

    if (!node)
        return;

    if (node->level > 0)
    {
        for (i = 0; i < node_size; ++i)
        {
            mu_sparse_array_node* child =
                (mu_sparse_array_node*)mu_atomicptr_load_relaxed(&node->child[i]);
            if (!child)
                continue;
            if (node->level == 1)
                MU_FREE(child); /* data page */
            else
                mu_sparse_array__node_free(child, node_size);
        }
    }

    MU_FREE(node);
}

MU_INLINE bool mu_sparse_array_init(mu_sparse_array* arr, size_t elem_size, uint32_t node_size)
{
    if (!arr || elem_size == 0 || node_size < 2 || !MU_IS_POW2(node_size))
        return false;

    arr->elem_size     = elem_size;
    arr->node_size     = node_size;
    arr->node_size_log2 = mu_sparse_array__log2(node_size);
    arr->node_mask     = node_size - 1u;
    mu_atomicptr_init(&arr->root, NULL);
    mu_spinlock_init(&arr->grow_lock);
    return true;
}

MU_INLINE void mu_sparse_array_destroy(mu_sparse_array* arr)
{
    if (!arr)
        return;

    mu_sparse_array_node* root =
        (mu_sparse_array_node*)mu_atomicptr_load_relaxed(&arr->root);
    if (root)
        mu_sparse_array__node_free(root, arr->node_size);

    mu_atomicptr_store_relaxed(&arr->root, NULL);
    arr->elem_size = 0;
}

/* Free every node but keep elem_size/node_size configuration. */
MU_INLINE void mu_sparse_array_clear(mu_sparse_array* arr)
{
    MU_ASSERT(arr);
    mu_sparse_array_node* root =
        (mu_sparse_array_node*)mu_atomicptr_load_relaxed(&arr->root);
    if (root)
        mu_sparse_array__node_free(root, arr->node_size);
    mu_atomicptr_store_relaxed(&arr->root, NULL);
}

MU_INLINE void* mu_sparse_array__get_slow(mu_sparse_array* arr, uint64_t idx)
{
    mu_sparse_array_node* root;
    mu_sparse_array_node* node;
    uint32_t level;

    mu_spinlock_lock(&arr->grow_lock);

    root = (mu_sparse_array_node*)mu_atomicptr_load_relaxed(&arr->root);
    if (!root)
    {
        root = mu_sparse_array__node_new(arr->node_size, 1u);
        if (!root)
        {
            mu_spinlock_unlock(&arr->grow_lock);
            return NULL;
        }
        mu_atomicptr_store_release(&arr->root, root);
    }

    level = root->level;
    while (!mu_sparse_array__covers(level, arr->node_size_log2, idx))
    {
        mu_sparse_array_node* new_root =
            mu_sparse_array__node_new(arr->node_size, level + 1u);
        if (!new_root)
        {
            mu_spinlock_unlock(&arr->grow_lock);
            return NULL;
        }

        mu_atomicptr_store_release(&new_root->child[0], root);
        mu_atomicptr_store_release(&arr->root, new_root);
        root = new_root;
        ++level;
    }

    node = root;
    while (level > 0)
    {
        uint32_t child_idx = (uint32_t)
            ((idx >> (level * arr->node_size_log2)) & arr->node_mask);
        mu_sparse_array_node* child =
            (mu_sparse_array_node*)mu_atomicptr_load_relaxed(&node->child[child_idx]);

        if (!child)
        {
            if (level == 1)
                child = (mu_sparse_array_node*)MU_CALLOC(arr->node_size, arr->elem_size);
            else
                child = mu_sparse_array__node_new(arr->node_size, level - 1u);

            if (!child)
            {
                mu_spinlock_unlock(&arr->grow_lock);
                return NULL;
            }

            mu_atomicptr_store_release(&node->child[child_idx], child);
        }

        node = child;
        --level;
    }

    mu_spinlock_unlock(&arr->grow_lock);

    return (char*)node + (size_t)(idx & arr->node_mask) * arr->elem_size;
}

/* Get (allocating and zeroing on first touch). Returns NULL on OOM. */
MU_INLINE void* mu_sparse_array_get(mu_sparse_array* arr, uint64_t idx)
{
    mu_sparse_array_node* root;
    mu_sparse_array_node* node;
    uint32_t level;

    MU_ASSERT(arr && arr->elem_size);

    root = (mu_sparse_array_node*)mu_atomicptr_load_acquire(&arr->root);
    if (!root || !mu_sparse_array__covers(root->level, arr->node_size_log2, idx))
        return mu_sparse_array__get_slow(arr, idx);

    node  = root;
    level = root->level;

    while (level > 0)
    {
        uint32_t child_idx = (uint32_t)
            ((idx >> (level * arr->node_size_log2)) & arr->node_mask);

        node = (mu_sparse_array_node*)mu_atomicptr_load_acquire(&node->child[child_idx]);
        if (!node)
            return mu_sparse_array__get_slow(arr, idx);

        --level;
    }

    return (char*)node + (size_t)(idx & arr->node_mask) * arr->elem_size;
}

/* Non-allocating lookup: NULL if the path or element page does not exist. */
MU_INLINE void* mu_sparse_array_peek(mu_sparse_array* arr, uint64_t idx)
{
    mu_sparse_array_node* root;
    mu_sparse_array_node* node;
    uint32_t level;

    if (!arr || arr->elem_size == 0)
        return NULL;

    root = (mu_sparse_array_node*)mu_atomicptr_load_acquire(&arr->root);
    if (!root || !mu_sparse_array__covers(root->level, arr->node_size_log2, idx))
        return NULL;

    node  = root;
    level = root->level;

    while (level > 0)
    {
        uint32_t child_idx = (uint32_t)
            ((idx >> (level * arr->node_size_log2)) & arr->node_mask);

        node = (mu_sparse_array_node*)mu_atomicptr_load_acquire(&node->child[child_idx]);
        if (!node)
            return NULL;

        --level;
    }

    return (char*)node + (size_t)(idx & arr->node_mask) * arr->elem_size;
}

#endif /* MU_SPARSE_ARRAY_H */
