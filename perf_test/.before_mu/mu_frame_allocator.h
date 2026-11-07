#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_frame_arena / mu_frame_allocator — per-frame bump allocation, no free().
 *
 * WHY: per-frame scratch (matrix batches, draw lists, transient strings,
 * culling results) should never hit malloc. A bump pointer resets in O(1) at
 * the frame boundary instead of freeing thousands of small blocks, which is
 * the difference between a smooth frame time and a hitch.
 *
 *   mu_frame_arena     one linear region, `alloc` advances a cursor
 *   mu_frame_allocator a ring of lanes, one active per frame
 *
 * WHY LANES: GPU work submitted this frame may still read last frame's data
 * when the next CPU frame starts. Rotating lanes means the buffer you are
 * about to overwrite was last used `depth` frames ago, so in-flight work
 * never reads memory you are recycling. depth = 3 covers a triple-buffered
 * renderer; depth = 1 is a plain arena.
 *
 * ALIGNMENT: every allocation is aligned to a caller-specified power of two
 * (defaults to MU_DEFAULT_ALIGN), so SIMD/SoA rows land on line boundaries.
 *
 * CONTRACT: pointers handed out are invalidated by reset()/begin_frame().
 * Never free() an individual allocation. Not thread safe (give each thread
 * its own arena — that is the point of a linear allocator).
 * --------------------------------------------------------------------------- */

#ifndef MU_DEFAULT_ALIGN
#define MU_DEFAULT_ALIGN 16u
#endif

#ifndef MU_FRAME_ALLOCATOR_DEPTH
#define MU_FRAME_ALLOCATOR_DEPTH 3u
#endif

typedef struct mu_frame_arena
{
    uint8_t* base;
    uint64_t capacity;
    uint64_t used;
    uint64_t peak;   /* high-water mark: the number you size the arena from */
    uint32_t owned;
} mu_frame_arena;

MU_INLINE bool mu_frame_arena_init(mu_frame_arena* a, uint64_t capacity_bytes)
{
    MU_ASSERT(a);
    a->base     = NULL;
    a->capacity = capacity_bytes;
    a->used     = 0;
    a->peak     = 0;
    a->owned    = 1;

    a->base = (uint8_t*)MU_MALLOC(capacity_bytes ? (size_t)capacity_bytes : 1u);
    if (!a->base)
    {
        a->capacity = 0;
        a->owned    = 0;
        return false;
    }
    return true;
}

MU_INLINE bool mu_frame_arena_init_static(mu_frame_arena* a, void* memory, uint64_t capacity_bytes)
{
    MU_ASSERT(a);
    a->base     = (uint8_t*)memory;
    a->capacity = capacity_bytes;
    a->used     = 0;
    a->peak     = 0;
    a->owned    = 0;
    return memory != NULL;
}

MU_INLINE void mu_frame_arena_destroy(mu_frame_arena* a)
{
    if (!a)
        return;
    if (a->owned)
        MU_FREE(a->base);
    a->base     = NULL;
    a->capacity = 0;
    a->used     = 0;
    a->peak     = 0;
    a->owned    = 0;
}

MU_INLINE void mu_frame_arena_reset(mu_frame_arena* a)
{
    MU_ASSERT(a);
    if (a->used > a->peak)
        a->peak = a->used;
    a->used = 0;
}

MU_INLINE uint64_t mu_frame_arena_used(const mu_frame_arena* a)
{
    return a ? a->used : 0;
}

MU_INLINE uint64_t mu_frame_arena_remaining(const mu_frame_arena* a)
{
    if (!a || a->used > a->capacity)
        return 0;
    return a->capacity - a->used;
}

MU_INLINE uint64_t mu_frame_arena_peak(const mu_frame_arena* a)
{
    return a ? a->peak : 0;
}

/* Bump-allocate `size` bytes at `alignment` (power of two).
   Returns NULL when the arena is full — a real, cheap, visible condition
   rather than a hidden malloc. */
MU_INLINE void* mu_frame_arena_alloc(mu_frame_arena* a, uint64_t size, uint32_t alignment)
{
    MU_ASSERT(a);
    if (!a->base || size == 0)
        return NULL;
    if (alignment == 0)
        alignment = MU_DEFAULT_ALIGN;

    uint64_t aligned = (a->used + (alignment - 1u)) & ~((uint64_t)alignment - 1u);
    if (aligned + size > a->capacity)
        return NULL;

    void* ptr = a->base + aligned;
    a->used   = aligned + size;
    if (a->used > a->peak)
        a->peak = a->used;
    return ptr;
}

/* -------------------------------------------------------------------------- *
 * Frame allocator: a ring of arenas, one live lane per frame.
 * -------------------------------------------------------------------------- */

typedef struct mu_frame_allocator
{
    mu_frame_arena lanes[MU_FRAME_ALLOCATOR_DEPTH];
    uint32_t       depth;
    uint32_t       current;
    uint32_t       owned;
} mu_frame_allocator;

MU_INLINE bool mu_frame_allocator_init(mu_frame_allocator* f, uint64_t bytes_per_lane)
{
    MU_ASSERT(f);
    f->depth   = MU_FRAME_ALLOCATOR_DEPTH;
    f->current = 0;
    f->owned   = 1;

    for (uint32_t i = 0; i < f->depth; ++i)
    {
        if (!mu_frame_arena_init(&f->lanes[i], bytes_per_lane))
        {
            for (uint32_t j = 0; j < i; ++j)
                mu_frame_arena_destroy(&f->lanes[j]);
            f->owned = 0;
            return false;
        }
    }
    return true;
}

MU_INLINE bool mu_frame_allocator_init_static(mu_frame_allocator* f, void* memory, uint64_t bytes_per_lane)
{
    MU_ASSERT(f);
    f->depth   = MU_FRAME_ALLOCATOR_DEPTH;
    f->current = 0;
    f->owned   = 0;

    uint8_t* cursor = (uint8_t*)memory;
    for (uint32_t i = 0; i < f->depth; ++i)
    {
        if (!mu_frame_arena_init_static(&f->lanes[i], cursor ? cursor + (size_t)bytes_per_lane * i : NULL, bytes_per_lane))
        {
            f->depth = i;
            return false;
        }
    }
    return true;
}

MU_INLINE void mu_frame_allocator_destroy(mu_frame_allocator* f)
{
    if (!f)
        return;
    if (f->owned)
        for (uint32_t i = 0; i < f->depth; ++i)
            mu_frame_arena_destroy(&f->lanes[i]);
    f->depth   = 0;
    f->current = 0;
    f->owned   = 0;
}

/* Call once at the top of each frame: advance to the next lane and rewind it.
   The lane just released was last written `depth - 1` frames ago, so any GPU
   or worker still reading it is safe. */
MU_INLINE void mu_frame_allocator_begin_frame(mu_frame_allocator* f)
{
    MU_ASSERT(f);
    f->current = (f->current + 1u) % f->depth;
    mu_frame_arena_reset(&f->lanes[f->current]);
}

MU_INLINE mu_frame_arena* mu_frame_allocator_current(mu_frame_allocator* f)
{
    MU_ASSERT(f);
    return &f->lanes[f->current];
}

MU_INLINE void* mu_frame_allocator_alloc(mu_frame_allocator* f, uint64_t size, uint32_t alignment)
{
    return mu_frame_arena_alloc(mu_frame_allocator_current(f), size, alignment);
}
