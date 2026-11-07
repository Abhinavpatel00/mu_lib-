#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_csr — compressed sparse row (static adjacency / graph).
 *
 * WHY: the layout AGENT.md recommends for anything you iterate rather than
 * mutate. Rows are contiguous, adjacency is packed into one flat index array,
 * and there is zero pointer chasing: walking a node's neighbours is a linear
 * scan of uint32s.
 *
 *     offsets[0 .. node_count]   row starts; offsets[n] == edge_count
 *     cols[0 .. edge_count)      targets
 *
 * Build is the classic two-pass counting sort (count, prefix-sum, scatter),
 * so construction streams the edge list twice with no per-node allocation.
 * Rows are sorted for deterministic output regardless of input edge order.
 *
 * USE FOR: navmesh adjacency, contact graphs, dependency graphs, mesh edges,
 * any "build once, walk forever" topology.
 *
 * CONTRACT: node indices are in [0, node_count). Not thread safe.
 * --------------------------------------------------------------------------- */

typedef struct mu_csr
{
    uint32_t* offsets;   /* node_count + 1 entries                           */
    uint32_t* cols;      /* edge_count entries                               */
    uint32_t  node_count;
    uint32_t  edge_count;
    uint32_t  owned;     /* 1 = heap buffers we may free                     */
} mu_csr;

MU_INLINE void mu_csr_init(mu_csr* g)
{
    MU_ASSERT(g);
    g->offsets    = NULL;
    g->cols       = NULL;
    g->node_count = 0;
    g->edge_count = 0;
    g->owned      = 0;
}

/* Wrap caller-owned buffers. offsets must have node_count+1 entries. */
MU_INLINE void mu_csr_init_static(mu_csr* g, uint32_t* offsets, uint32_t* cols, uint32_t node_count, uint32_t edge_count)
{
    MU_ASSERT(g);
    g->offsets    = offsets;
    g->cols       = cols;
    g->node_count = node_count;
    g->edge_count = edge_count;
    g->owned      = 0;
}

MU_INLINE void mu_csr_destroy(mu_csr* g)
{
    if (!g)
        return;
    if (g->owned)
    {
        MU_FREE(g->offsets);
        MU_FREE(g->cols);
    }
    g->offsets    = NULL;
    g->cols       = NULL;
    g->node_count = 0;
    g->edge_count = 0;
    g->owned      = 0;
}

MU_INLINE void mu_csr_clear(mu_csr* g)
{
    MU_ASSERT(g);
    g->node_count = 0;
    g->edge_count = 0;
}

MU_INLINE uint32_t mu_csr_degree(const mu_csr* g, uint32_t node)
{
    MU_ASSERT(g && node < g->node_count);
    return g->offsets[node + 1u] - g->offsets[node];
}

/* Half-open [begin, end) range of g->cols for `node`. */
MU_INLINE uint32_t mu_csr_row_begin(const mu_csr* g, uint32_t node)
{
    return g->offsets[node];
}

MU_INLINE uint32_t mu_csr_row_end(const mu_csr* g, uint32_t node)
{
    return g->offsets[node + 1u];
}

/* Build from an edge list. If `symmetric`, each (a,b) also adds (b,a) —
   the usual case for undirected topology (navmesh, contact graphs).

   Buffers are allocated here: offsets (node_count+1), cols (2*edge_count if
   symmetric else edge_count), plus two scratch arrays freed before return. */
MU_INLINE bool mu_csr_build(mu_csr* g, uint32_t node_count, const uint32_t* a, const uint32_t* b,
                            uint32_t edge_count, bool symmetric)
{
    MU_ASSERT(g);
    mu_csr_destroy(g);

    if (node_count == 0)
        return true;
    if (edge_count > 0 && (!a || !b))
        return false;

    uint32_t dir        = symmetric ? 2u : 1u;
    uint64_t total      = (uint64_t)edge_count * dir;
    if (total > 0xffffffffull)
        return false;

    uint32_t* offsets = (uint32_t*)MU_MALLOC(((size_t)node_count + 1u) * sizeof(uint32_t));
    uint32_t* cols    = (uint32_t*)MU_MALLOC((size_t)total * sizeof(uint32_t));
    uint32_t* cursor  = (uint32_t*)MU_MALLOC(((size_t)node_count + 1u) * sizeof(uint32_t));
    if (!offsets || !cols || !cursor)
    {
        MU_FREE(offsets);
        MU_FREE(cols);
        MU_FREE(cursor);
        return false;
    }

    for (uint32_t i = 0; i <= node_count; ++i)
        offsets[i] = 0;

    /* Pass 1: degree count. */
    for (uint32_t i = 0; i < edge_count; ++i)
    {
        if (a[i] >= node_count || b[i] >= node_count)
            continue;
        ++offsets[a[i] + 1u];
        if (symmetric)
            ++offsets[b[i] + 1u];
    }

    /* Prefix sum -> row starts. */
    for (uint32_t i = 0; i < node_count; ++i)
        offsets[i + 1u] += offsets[i];

    for (uint32_t i = 0; i < node_count; ++i)
        cursor[i] = offsets[i];

    /* Pass 2: scatter. */
    for (uint32_t i = 0; i < edge_count; ++i)
    {
        if (a[i] >= node_count || b[i] >= node_count)
            continue;
        cols[cursor[a[i]]++] = b[i];
        if (symmetric)
            cols[cursor[b[i]]++] = a[i];
    }

    MU_FREE(cursor);

    /* Sort each row so the layout is deterministic for a given edge set
       (order-independent output => stable tests and stable iteration). */
    for (uint32_t u = 0; u < node_count; ++u)
    {
        uint32_t beg = offsets[u];
        uint32_t end = offsets[u + 1u];
        for (uint32_t i = beg + 1u; i < end; ++i)
        {
            uint32_t key = cols[i];
            uint32_t j   = i;
            while (j > beg && cols[j - 1u] > key)
            {
                cols[j] = cols[j - 1u];
                --j;
            }
            cols[j] = key;
        }
    }

    g->offsets    = offsets;
    g->cols       = cols;
    g->node_count = node_count;
    g->edge_count = (uint32_t)total;
    g->owned      = 1;
    return true;
}

/* Visit every neighbour of `node`. Return false from `fn` to stop. */
typedef bool (*mu_csr_visit_fn)(uint32_t neighbor, uint32_t node, void* user);

MU_INLINE bool mu_csr_for_each(const mu_csr* g, uint32_t node, mu_csr_visit_fn fn, void* user)
{
    MU_ASSERT(g && fn && node < g->node_count);
    for (uint32_t p = g->offsets[node]; p < g->offsets[node + 1u]; ++p)
        if (!fn(g->cols[p], node, user))
            return false;
    return true;
}
