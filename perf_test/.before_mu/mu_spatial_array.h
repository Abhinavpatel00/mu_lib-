#pragma once
#ifndef MU_SPATIAL_ARRAY_H
#define MU_SPATIAL_ARRAY_H

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_spatial_array -- dense 3D table that grows around integer coordinates.
 *
 * WHY
 * ---
 * `mu_grid` (mu_spatial_hash.h) is a broadphase over float positions: it
 * buckets items for range queries. `mu_rog_grid_*` (mu_roguelike.h) is a
 * fixed-size dense map with an explicit origin. Neither is a dense table
 * addressed by arbitrary signed 3D coordinates that keeps growing as new
 * coordinates arrive -- the shape you want for voxel fields, tile maps,
 * chunk-local grids, or any "world position -> cell" lookup where the world
 * extent is not known up front.
 *
 * LAYOUT
 * ------
 * One flat array of cells, indexed as
 *
 *     index = ix + width * (iy + height * iz)
 *
 * with ix/iy/iz measured from the array origin. The array covers the
 * half-open box [origin, origin + extent) on each axis, so negative world
 * coordinates are ordinary coordinates, not an error case.
 *
 * GROWTH
 * ------
 * Each axis extent is a power of two and doubles when a coordinate falls
 * outside it. The axis keeps its far edge anchored: growing towards +inf
 * extends the near edge, growing towards -inf pushes the origin down and
 * keeps the top edge where it was. A growth therefore never invalidates the
 * coordinates already stored -- cells are copied to their new offsets.
 *
 * Cost of one growth is one allocation plus one linear copy of the old
 * cells; growth is amortised O(1) per insertion because extents double.
 *
 * Reference: "Expanding ND array" -- https://lisyarus.github.io/blog/posts/expanding-nd-array.html
 *
 * CONTRACT
 * --------
 *   - Not thread safe: one writer at a time, like the rest of the library.
 *   - Cells hold references; store ids/handles, not long-lived pointers, if
 *     the values must survive the table.
 *   - `mu_spatial_array_get` returns NULL both for "outside the box" and for
 *     "cell holds NULL". Use `mu_spatial_array_contains` to tell them apart.
 * --------------------------------------------------------------------------- */

typedef struct mu_spatial_array
{
    void**  data;
    int64_t origin_x;
    int64_t origin_y;
    int64_t origin_z;
    size_t  width;
    size_t  height;
    size_t  depth;
} mu_spatial_array;

MU_INLINE void mu_spatial_array_init(mu_spatial_array* a)
{
    MU_ASSERT(a);
    a->data     = NULL;
    a->origin_x = 0;
    a->origin_y = 0;
    a->origin_z = 0;
    a->width    = 0;
    a->height   = 0;
    a->depth    = 0;
}

MU_INLINE void mu_spatial_array_destroy(mu_spatial_array* a)
{
    MU_ASSERT(a);
    MU_FREE(a->data);
    mu_spatial_array_init(a);
}

/* Wipe the cells but keep the extents, so the next fill does not reallocate. */
MU_INLINE void mu_spatial_array_clear(mu_spatial_array* a)
{
    MU_ASSERT(a);
    if(!a->data)
        return;
    memset(a->data, 0, a->width * a->height * a->depth * sizeof(void*));
}

/* Smallest power of two that is >= value (value >= 1). */
MU_INLINE size_t mu_spatial_array__pow2_at_least(size_t value)
{
    size_t result = 1;
    while (result < value)
    {
        if (result > (SIZE_MAX >> 1))
            return 0; /* overflow: no representable power of two */
        result <<= 1;
    }
    return result;
}

/*
 * Grow one axis so that `index` is inside it.
 *
 * `origin`/`size` describe the current half-open range [origin, origin+size).
 * Returns false only on integer overflow.
 */
MU_INLINE bool mu_spatial_array__expand_axis(int64_t index, int64_t* origin, size_t* size)
{
    int64_t current = *origin;
    size_t  extent  = *size;

    if(extent == 0)
    {
        *origin = index;
        *size   = 1;
        return true;
    }

    if(extent > (size_t)INT64_MAX)
        return false; /* extent no longer representable as a coordinate span */

    if(index >= current && index - current < (int64_t)extent)
        return true; /* already inside */

    size_t needed;
    int64_t new_origin;

    if(index < current)
    {
        /* Grow towards -inf: keep the far edge, push the origin down. */
        uint64_t distance = (uint64_t)(current - index);
        if(distance > (uint64_t)(SIZE_MAX - extent))
            return false;
        needed = (size_t)distance + extent;
    }
    else
    {
        /* Grow towards +inf: keep the origin, extend the far edge. */
        uint64_t distance = (uint64_t)(index - current);
        if(distance >= (uint64_t)SIZE_MAX)
            return false;
        needed = (size_t)distance + 1;
    }

    size_t grown = mu_spatial_array__pow2_at_least(needed);
    if(grown == 0)
        return false;

    if(index < current)
        new_origin = current - (int64_t)(grown - extent);
    else
        new_origin = current;

    *origin = new_origin;
    *size   = grown;
    return true;
}

/* Make sure (x, y, z) is inside the array, growing the extents if needed. */
MU_INLINE bool mu_spatial_array_reserve(mu_spatial_array* a, int64_t x, int64_t y, int64_t z)
{
    MU_ASSERT(a);

    int64_t origin_x = a->origin_x;
    int64_t origin_y = a->origin_y;
    int64_t origin_z = a->origin_z;
    size_t  width    = a->width;
    size_t  height   = a->height;
    size_t  depth    = a->depth;

    if(!mu_spatial_array__expand_axis(x, &origin_x, &width))
        return false;
    if(!mu_spatial_array__expand_axis(y, &origin_y, &height))
        return false;
    if(!mu_spatial_array__expand_axis(z, &origin_z, &depth))
        return false;

    if(width == a->width && height == a->height && depth == a->depth &&
       origin_x == a->origin_x && origin_y == a->origin_y && origin_z == a->origin_z)
        return true;

    if(height > SIZE_MAX / width)
        return false;
    size_t plane = width * height;

    if(depth > SIZE_MAX / plane)
        return false;
    size_t count = plane * depth;

    if(count > SIZE_MAX / sizeof(void*))
        return false;

    void** grown = (void**)MU_CALLOC(count, sizeof(void*));
    if(!grown)
        return false;

    size_t shift_x = (size_t)(a->origin_x - origin_x);
    size_t shift_y = (size_t)(a->origin_y - origin_y);
    size_t shift_z = (size_t)(a->origin_z - origin_z);

    for(size_t iz = 0; iz < a->depth; ++iz)
    {
        for(size_t iy = 0; iy < a->height; ++iy)
        {
            size_t old_offset = a->width * (iy + a->height * iz);
            size_t new_offset = (iz + shift_z) * plane + (iy + shift_y) * width + shift_x;
            memcpy(&grown[new_offset], &a->data[old_offset], a->width * sizeof(void*));
        }
    }

    MU_FREE(a->data);
    a->data     = grown;
    a->origin_x = origin_x;
    a->origin_y = origin_y;
    a->origin_z = origin_z;
    a->width    = width;
    a->height   = height;
    a->depth    = depth;
    return true;
}

MU_INLINE bool mu_spatial_array_set(mu_spatial_array* a, int64_t x, int64_t y, int64_t z, void* value)
{
    MU_ASSERT(a);

    if(!mu_spatial_array_reserve(a, x, y, z))
        return false;

    size_t ix = (size_t)(x - a->origin_x);
    size_t iy = (size_t)(y - a->origin_y);
    size_t iz = (size_t)(z - a->origin_z);

    a->data[ix + a->width * (iy + a->height * iz)] = value;
    return true;
}

MU_INLINE bool mu_spatial_array_contains(const mu_spatial_array* a, int64_t x, int64_t y, int64_t z)
{
    MU_ASSERT(a);

    if(!a->data)
        return false;
    if(x < a->origin_x || y < a->origin_y || z < a->origin_z)
        return false;
    if(x - a->origin_x >= (int64_t)a->width)
        return false;
    if(y - a->origin_y >= (int64_t)a->height)
        return false;
    if(z - a->origin_z >= (int64_t)a->depth)
        return false;
    return true;
}

MU_INLINE void* mu_spatial_array_get(const mu_spatial_array* a, int64_t x, int64_t y, int64_t z)
{
    MU_ASSERT(a);

    if(!mu_spatial_array_contains(a, x, y, z))
        return NULL;

    size_t ix = (size_t)(x - a->origin_x);
    size_t iy = (size_t)(y - a->origin_y);
    size_t iz = (size_t)(z - a->origin_z);

    return a->data[ix + a->width * (iy + a->height * iz)];
}

MU_INLINE void mu_spatial_array_bounds(const mu_spatial_array* a,
                                       int64_t* min_x, int64_t* min_y, int64_t* min_z,
                                       int64_t* max_x, int64_t* max_y, int64_t* max_z)
{
    MU_ASSERT(a);

    if(min_x) *min_x = a->origin_x;
    if(min_y) *min_y = a->origin_y;
    if(min_z) *min_z = a->origin_z;
    if(max_x) *max_x = a->origin_x + (int64_t)a->width  - 1;
    if(max_y) *max_y = a->origin_y + (int64_t)a->height - 1;
    if(max_z) *max_z = a->origin_z + (int64_t)a->depth  - 1;
}

#endif /* MU_SPATIAL_ARRAY_H */
