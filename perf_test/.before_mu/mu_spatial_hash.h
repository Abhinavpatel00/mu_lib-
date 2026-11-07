#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_grid — 2D uniform-grid spatial hash (count -> prefix-sum -> scatter).
 *
 * WHY: the broadphase layout AGENT.md recommends. No per-cell allocation and
 * no buckets: the whole structure is three flat arrays.
 *
 *     offsets[0 .. cell_count]   row starts into items; offsets[c+1] end it
 *     items[0 .. count)          item indices, grouped by cell
 *
 * Build streams the position arrays twice (count, then prefix, then scatter),
 * so it is cache-linear both ways. Query walks only the cells a rect overlaps,
 * so cost scales with local density, not with total N — the point of a
 * broadphase (the performance skill's physics chapter).
 *
 * SoA input (xs[], ys[]) not AoS: pass two arrays, scan them together.
 *
 * GUARD: cell_count is capped so a silly cell_size against a huge bound cannot
 * allocate gigabytes — cell_size is widened instead.
 *
 * CONTRACT: xs/ys must stay alive for as long as the grid is queried (the grid
 * stores indices, not positions). Not thread safe.
 * --------------------------------------------------------------------------- */

#ifndef MU_GRID_MAX_CELLS
#define MU_GRID_MAX_CELLS (1u << 20) /* 1M cells ~= 4MB of offsets */
#endif

typedef struct mu_grid
{
    uint32_t* offsets;  /* cell_count + 1                                     */
    uint32_t* items;    /* count, grouped by cell                             */
    uint32_t  cell_count;
    uint32_t  grid_w;
    uint32_t  grid_h;
    uint32_t  count;
    uint32_t  owned;
    float     cell_size;
    float     inv_cell_size;
    float     min_x;
    float     min_y;
} mu_grid;

MU_INLINE void mu_grid_init(mu_grid* g)
{
    MU_ASSERT(g);
    g->offsets      = NULL;
    g->items        = NULL;
    g->cell_count   = 0;
    g->grid_w       = 0;
    g->grid_h       = 0;
    g->count        = 0;
    g->owned        = 0;
    g->cell_size    = 1.0f;
    g->inv_cell_size = 1.0f;
    g->min_x        = 0.0f;
    g->min_y        = 0.0f;
}

MU_INLINE void mu_grid_destroy(mu_grid* g)
{
    if (!g)
        return;
    if (g->owned)
    {
        MU_FREE(g->offsets);
        MU_FREE(g->items);
    }
    g->offsets    = NULL;
    g->items      = NULL;
    g->cell_count = 0;
    g->grid_w     = 0;
    g->grid_h     = 0;
    g->count      = 0;
    g->owned      = 0;
}

MU_INLINE void mu_grid_clear(mu_grid* g)
{
    MU_ASSERT(g);
    g->count = 0;
}

MU_INLINE uint32_t mu_grid_count(const mu_grid* g)
{
    return g ? g->count : 0u;
}

MU_INLINE uint32_t mu_grid_cell_count(const mu_grid* g)
{
    return g ? g->cell_count : 0u;
}

/* Half-open [begin, end) range into items[] for a cell. */
MU_INLINE uint32_t mu_grid_cell_begin(const mu_grid* g, uint32_t cell)
{
    return g->offsets[cell];
}

MU_INLINE uint32_t mu_grid_cell_end(const mu_grid* g, uint32_t cell)
{
    return g->offsets[cell + 1u];
}

/* ------------------------------------------------------------------ *
 * Build
 * ------------------------------------------------------------------ */

MU_INLINE bool mu_grid_build(mu_grid* g, const float* xs, const float* ys, uint32_t count, float cell_size)
{
    MU_ASSERT(g);
    mu_grid_destroy(g);
    mu_grid_init(g);

    if (count == 0)
        return true;
    if (!xs || !ys || cell_size <= 0.0f)
        return false;

    /* Bounds in one linear pass. */
    float min_x = xs[0], max_x = xs[0];
    float min_y = ys[0], max_y = ys[0];
    for (uint32_t i = 1; i < count; ++i)
    {
        if (xs[i] < min_x) min_x = xs[i];
        if (xs[i] > max_x) max_x = xs[i];
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }

    float inv = 1.0f / cell_size;
    uint32_t w = (uint32_t)((max_x - min_x) * inv) + 1u;
    uint32_t h = (uint32_t)((max_y - min_y) * inv) + 1u;
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    /* Widening cell_size by f shrinks the cell count by f^2, so widening by
       the raw ratio always lands under the cap (and costs no precision we
       care about — a broadphase only needs locality, not an exact size). */
    uint64_t total = (uint64_t)w * (uint64_t)h;
    if (total > MU_GRID_MAX_CELLS)
    {
        float f = (float)((double)total / (double)MU_GRID_MAX_CELLS);
        cell_size *= f;
        inv = 1.0f / cell_size;
        w   = (uint32_t)((max_x - min_x) * inv) + 1u;
        h   = (uint32_t)((max_y - min_y) * inv) + 1u;
        if (w == 0) w = 1;
        if (h == 0) h = 1;
        total = (uint64_t)w * (uint64_t)h;
        if (total > MU_GRID_MAX_CELLS)
        {
            w = 1;
            h = 1;
            total = 1;
        }
    }

    uint32_t cell_count = (uint32_t)total;

    uint32_t* offsets = (uint32_t*)MU_MALLOC(((size_t)cell_count + 1u) * sizeof(uint32_t));
    uint32_t* items   = (uint32_t*)MU_MALLOC((size_t)count * sizeof(uint32_t));
    if (!offsets || !items)
    {
        MU_FREE(offsets);
        MU_FREE(items);
        return false;
    }

    g->offsets       = offsets;
    g->items         = items;
    g->cell_count    = cell_count;
    g->grid_w        = w;
    g->grid_h        = h;
    g->count         = count;
    g->owned         = 1;
    g->cell_size     = cell_size;
    g->inv_cell_size = inv;
    g->min_x         = min_x;
    g->min_y         = min_y;

    for (uint32_t i = 0; i <= cell_count; ++i)
        offsets[i] = 0;

    /* Pass 1: count per cell, stored at offsets[cell + 1]. */
    for (uint32_t i = 0; i < count; ++i)
    {
        int32_t cx = (int32_t)((xs[i] - min_x) * inv);
        int32_t cy = (int32_t)((ys[i] - min_y) * inv);
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx >= (int32_t)w) cx = (int32_t)w - 1;
        if (cy >= (int32_t)h) cy = (int32_t)h - 1;
        ++offsets[(uint32_t)cy * w + (uint32_t)cx + 1u];
    }

    /* Prefix sum -> row starts. */
    for (uint32_t c = 0; c < cell_count; ++c)
        offsets[c + 1u] += offsets[c];

    /* Pass 2: scatter, incrementing the running start. Afterwards offsets[c]
       holds cell c's END, so a single right-shift turns them back into
       starts — no scratch cursor array needed. */
    for (uint32_t i = 0; i < count; ++i)
    {
        int32_t cx = (int32_t)((xs[i] - min_x) * inv);
        int32_t cy = (int32_t)((ys[i] - min_y) * inv);
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx >= (int32_t)w) cx = (int32_t)w - 1;
        if (cy >= (int32_t)h) cy = (int32_t)h - 1;
        uint32_t cell = (uint32_t)cy * w + (uint32_t)cx;
        items[offsets[cell]++] = i;
    }

    for (uint32_t c = cell_count; c > 0; --c)
        offsets[c] = offsets[c - 1u];
    offsets[0] = 0;

    return true;
}

/* ------------------------------------------------------------------ *
 * Query
 * ------------------------------------------------------------------ */

MU_INLINE uint32_t mu_grid_cell(const mu_grid* g, float x, float y)
{
    int32_t cx = (int32_t)((x - g->min_x) * g->inv_cell_size);
    int32_t cy = (int32_t)((y - g->min_y) * g->inv_cell_size);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= (int32_t)g->grid_w) cx = (int32_t)g->grid_w - 1;
    if (cy >= (int32_t)g->grid_h) cy = (int32_t)g->grid_h - 1;
    return (uint32_t)cy * g->grid_w + (uint32_t)cx;
}

/* Items within `radius` of (cx, cy), written into out[]. Returns how many
   were written; stops early at out_cap. radius <= 0 gives a point query. */
MU_INLINE uint32_t mu_grid_query(const mu_grid* g, const float* xs, const float* ys, float cx, float cy,
                                 float radius, uint32_t* out, uint32_t out_cap)
{
    if (!g || g->cell_count == 0 || !out || out_cap == 0)
        return 0;

    float r2 = radius * radius;
    if (r2 < 0.0f)
        r2 = 0.0f;

    float inv  = g->inv_cell_size;
    int32_t x0 = (int32_t)((cx - radius - g->min_x) * inv);
    int32_t x1 = (int32_t)((cx + radius - g->min_x) * inv);
    int32_t y0 = (int32_t)((cy - radius - g->min_y) * inv);
    int32_t y1 = (int32_t)((cy + radius - g->min_y) * inv);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (int32_t)g->grid_w) x1 = (int32_t)g->grid_w - 1;
    if (y1 >= (int32_t)g->grid_h) y1 = (int32_t)g->grid_h - 1;
    if (x1 < 0 || y1 < 0 || x0 > x1 || y0 > y1)
        return 0;

    uint32_t found = 0;
    for (int32_t iy = y0; iy <= y1; ++iy)
    {
        for (int32_t ix = x0; ix <= x1; ++ix)
        {
            uint32_t cell = (uint32_t)iy * g->grid_w + (uint32_t)ix;
            uint32_t beg  = g->offsets[cell];
            uint32_t end  = g->offsets[cell + 1u];

            for (uint32_t p = beg; p < end; ++p)
            {
                uint32_t idx = g->items[p];
                float    dx  = xs[idx] - cx;
                float    dy  = ys[idx] - cy;
                if (dx * dx + dy * dy <= r2)
                {
                    if (found >= out_cap)
                        return found;
                    out[found++] = idx;
                }
            }
        }
    }
    return found;
}
