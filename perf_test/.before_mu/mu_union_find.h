#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_uf — disjoint set (union-find).
 *
 * WHY: connected components in O(alpha(n)) — navmesh region building,
 * clustering, connectivity, "merge these two contact sets". Two flat uint32
 * arrays, no pointers, no allocation per element, cache-linear scans.
 *
 * PATH COMPRESSION: find() flattens the chain so the next lookup is O(1).
 *   Iterative, not recursive — no stack growth on deep trees (AGENT.md:
 *   "do not overfill the stack").
 *
 * CONTRACT: handles are 0 .. node_count-1. Not thread safe.
 * --------------------------------------------------------------------------- */

typedef struct mu_uf
{
    uint32_t* parent;    /* index tree; a root points at itself              */
    uint32_t* size;      /* subtree size; only meaningful on a root          */
    uint32_t  count;     /* number of elements                               */
    uint32_t  components;/* live component count, maintained by mu_uf_union  */
    uint32_t  owned;     /* 1 = heap buffers we may free                     */
} mu_uf;

MU_INLINE bool mu_uf_init(mu_uf* uf, uint32_t node_count)
{
    MU_ASSERT(uf);
    uf->count       = node_count;
    uf->components  = node_count;
    uf->owned       = 1;
    uf->parent      = NULL;
    uf->size        = NULL;

    if (node_count == 0)
        return true;

    uf->parent = (uint32_t*)MU_MALLOC((size_t)node_count * sizeof(uint32_t));
    uf->size   = (uint32_t*)MU_MALLOC((size_t)node_count * sizeof(uint32_t));
    if (!uf->parent || !uf->size)
    {
        MU_FREE(uf->parent);
        MU_FREE(uf->size);
        uf->parent   = NULL;
        uf->size     = NULL;
        uf->count    = 0;
        uf->owned    = 0;
        return false;
    }

    for (uint32_t i = 0; i < node_count; ++i)
    {
        uf->parent[i] = i;
        uf->size[i]   = 1;
    }
    return true;
}

/* Caller-provided arrays of node_count uint32s each. Never allocates. */
MU_INLINE bool mu_uf_init_static(mu_uf* uf, uint32_t* parent, uint32_t* size, uint32_t node_count)
{
    MU_ASSERT(uf);
    uf->parent     = parent;
    uf->size       = size;
    uf->count      = node_count;
    uf->components = node_count;
    uf->owned      = 0;

    if (node_count == 0)
        return true;
    if (!parent || !size)
    {
        uf->count    = 0;
        uf->components = 0;
        return false;
    }

    for (uint32_t i = 0; i < node_count; ++i)
    {
        parent[i] = i;
        size[i]   = 1;
    }
    return true;
}

MU_INLINE void mu_uf_destroy(mu_uf* uf)
{
    if (!uf)
        return;
    if (uf->owned)
    {
        MU_FREE(uf->parent);
        MU_FREE(uf->size);
    }
    uf->parent     = NULL;
    uf->size       = NULL;
    uf->count      = 0;
    uf->components = 0;
    uf->owned      = 0;
}

MU_INLINE void mu_uf_reset(mu_uf* uf)
{
    MU_ASSERT(uf);
    for (uint32_t i = 0; i < uf->count; ++i)
    {
        uf->parent[i] = i;
        uf->size[i]   = 1;
    }
    uf->components = uf->count;
}

/* Representative of the set containing x, with path compression.
   Iterative: the walk-and-flatten loop never recurses. */
MU_INLINE uint32_t mu_uf_find(mu_uf* uf, uint32_t x)
{
    MU_ASSERT(uf && x < uf->count);

    uint32_t root = x;
    while (uf->parent[root] != root)
        root = uf->parent[root];

    while (uf->parent[x] != root) /* flatten the whole chain */
    {
        uint32_t next = uf->parent[x];
        uf->parent[x] = root;
        x             = next;
    }
    return root;
}

/* Root without writing — safe on a const view / read-only pass. */
MU_INLINE uint32_t mu_uf_find_readonly(const mu_uf* uf, uint32_t x)
{
    MU_ASSERT(uf && x < uf->count);
    while (uf->parent[x] != x)
        x = uf->parent[x];
    return x;
}

MU_INLINE bool mu_uf_connected(mu_uf* uf, uint32_t a, uint32_t b)
{
    return mu_uf_find(uf, a) == mu_uf_find(uf, b);
}

/* Merge the sets of a and b. Returns true only if two sets became one
   (so callers can count merges without maintaining their own counter). */
MU_INLINE bool mu_uf_union(mu_uf* uf, uint32_t a, uint32_t b)
{
    uint32_t ra = mu_uf_find(uf, a);
    uint32_t rb = mu_uf_find(uf, b);

    if (ra == rb)
        return false;

    /* Union by size: attach the smaller tree under the larger one so depth
       stays logarithmic (and O(alpha) once compression is applied). */
    if (uf->size[ra] < uf->size[rb])
    {
        uint32_t t = ra;
        ra         = rb;
        rb         = t;
    }

    uf->parent[rb]  = ra;
    uf->size[ra]   += uf->size[rb];
    --uf->components;
    return true;
}

MU_INLINE uint32_t mu_uf_component_count(const mu_uf* uf)
{
    return uf ? uf->components : 0u;
}

MU_INLINE uint32_t mu_uf_size(mu_uf* uf, uint32_t x)
{
    return uf->size[mu_uf_find(uf, x)];
}
