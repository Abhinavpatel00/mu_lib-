#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_toposort — Kahn's algorithm over a CSR graph.
 *
 * WHY: derive a valid execution order from dependency edges — asset load
 * order, job scheduling, shader/pipeline compile order, animation state
 * ordering, build systems. Zero allocation inside the run (scratch comes from
 * init) and both scratch arrays are flat uint32, so the two passes stream
 * linearly over the edge list.
 *
 * INPUT: CSR (compressed sparse row), the layout recommended by AGENT.md:
 *     offsets[0..count]  row start in cols, offsets[count] == edge_count
 *     cols[0..edge_count)  targets
 *
 * OUTPUT: out_order[0..count) receives a topological order. Returns the
 * number of nodes placed; a return < node_count means there is a CYCLE
 * (those nodes are unreachable from any zero-in-degree node).
 *
 * TWO PASSES, both linear:
 *     1. compute in-degrees
 *     2. pop zero-in-degree nodes, emit, decrement successors
 *
 * CONTRACT: edges must reference nodes in [0, node_count). Not thread safe.
 * --------------------------------------------------------------------------- */

typedef struct mu_toposort
{
    uint32_t* degree; /* scratch: running in-degree, size count            */
    uint32_t* queue;  /* scratch: frontier of ready nodes, size count      */
    uint32_t  count;
    uint32_t  owned;  /* 1 = heap buffers we may free                      */
} mu_toposort;

MU_INLINE bool mu_toposort_init(mu_toposort* ts, uint32_t node_count)
{
    MU_ASSERT(ts);
    ts->degree = NULL;
    ts->queue  = NULL;
    ts->count  = node_count;
    ts->owned  = 1;

    if (node_count == 0)
        return true;

    ts->degree = (uint32_t*)MU_MALLOC((size_t)node_count * sizeof(uint32_t));
    ts->queue  = (uint32_t*)MU_MALLOC((size_t)node_count * sizeof(uint32_t));
    if (!ts->degree || !ts->queue)
    {
        MU_FREE(ts->degree);
        MU_FREE(ts->queue);
        ts->degree = NULL;
        ts->queue  = NULL;
        ts->count  = 0;
        ts->owned  = 0;
        return false;
    }
    return true;
}

/* Caller-provided scratch: two arrays of node_count uint32 each. */
MU_INLINE bool mu_toposort_init_static(mu_toposort* ts, uint32_t* degree, uint32_t* queue, uint32_t node_count)
{
    MU_ASSERT(ts);
    ts->degree = degree;
    ts->queue  = queue;
    ts->count  = node_count;
    ts->owned  = 0;

    if (node_count == 0)
        return true;
    if (!degree || !queue)
    {
        ts->count = 0;
        return false;
    }
    return true;
}

MU_INLINE void mu_toposort_destroy(mu_toposort* ts)
{
    if (!ts)
        return;
    if (ts->owned)
    {
        MU_FREE(ts->degree);
        MU_FREE(ts->queue);
    }
    ts->degree = NULL;
    ts->queue  = NULL;
    ts->count  = 0;
    ts->owned  = 0;
}

/* Returns node_count on success, or the number actually ordered on a cycle.
   Distinguishes the two via *out_has_cycle when non-NULL. */
MU_INLINE uint32_t mu_toposort_run(const mu_toposort* ts, const uint32_t* offsets, const uint32_t* cols,
                                  uint32_t node_count, uint32_t* out_order, bool* out_has_cycle)
{
    MU_ASSERT(ts && offsets);
    if (out_has_cycle)
        *out_has_cycle = false;

    if (node_count == 0)
        return 0;
    if (ts->count < node_count || !ts->degree || !ts->queue || !out_order)
        return 0;

    uint32_t* degree = ts->degree;
    uint32_t* queue  = ts->queue;

    /* Pass 1: in-degrees (linear in edges). */
    for (uint32_t v = 0; v < node_count; ++v)
        degree[v] = 0;

    for (uint32_t u = 0; u < node_count; ++u)
    {
        for (uint32_t p = offsets[u]; p < offsets[u + 1u]; ++p)
        {
            uint32_t v = cols[p];
            if (v < node_count)
                ++degree[v];
        }
    }

    /* Seed the frontier with all zero-in-degree nodes. */
    uint32_t head = 0;
    uint32_t tail = 0;
    for (uint32_t v = 0; v < node_count; ++v)
        if (degree[v] == 0u)
            queue[tail++] = v;

    /* Pass 2: emit ready nodes and release their successors. */
    uint32_t written = 0;
    while (head < tail)
    {
        uint32_t u = queue[head++];
        out_order[written++] = u;

        for (uint32_t p = offsets[u]; p < offsets[u + 1u]; ++p)
        {
            uint32_t v = cols[p];
            if (v < node_count && --degree[v] == 0u)
                queue[tail++] = v;
        }
    }

    if (written != node_count)
    {
        if (out_has_cycle)
            *out_has_cycle = true;
        return written;
    }
    return written;
}
