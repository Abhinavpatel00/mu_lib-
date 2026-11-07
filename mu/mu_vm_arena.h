#pragma once
#ifndef MU_VM_ARENA_H
#define MU_VM_ARENA_H

#include "mu_common.h"
#include "mu_vm.h"

/* ---------------------------------------------------------------------------
 * mu_vm_arena — a bump allocator on a reservation that never moves.
 *
 * WHY
 * ---
 * mu_linear_allocator needs its memory up front and mu_array grows with
 * realloc. Both force a choice: pick a fixed size (waste or run out), or let
 * the buffer move (copy cost, and every pointer into it dies). Reserving a
 * large address range and committing on demand removes the choice:
 *
 *     reserve 4 GB of VA   ->  0.012 ms, 0 RSS, no physical memory
 *     first write to a page ->  commit + fault, ~1 us
 *     grow                   ->  mprotect, never a copy
 *
 * The base address is fixed for the life of the arena, so offsets, handles and
 * pointers handed out earlier stay valid. That is the property callers
 * actually want; the copy saving is a side effect.
 *
 * COST MODEL
 * ----------
 * Touching new memory is the expensive part (~1 us/page), not reserving or
 * committing. So this is for arenas that hold megabytes, not for per-frame
 * scratch that you reset every frame: re-faulting 4 MB costs ~1 ms, which is
 * far worse than `head = 0` on memory that is already hot.
 *
 * USE IT FOR
 *   - append-only text/asset arenas (editor piece add-buffer, interning)
 *   - scratch that lives for a document/project, not a frame
 *   - containers whose element count can grow to millions
 *
 * DO NOT USE IT FOR
 *   - per-frame scratch (use mu_frame_allocator; reset is cheaper than fault)
 *   - small allocations (a page is the unit; a 40-byte object wastes 4 KB)
 *   - anything that must run on a platform without virtual memory; check
 *     mu_vm_arena_init()'s return value and fall back to mu_linear_allocator
 *
 * CONTRACT
 * --------
 * - base() never changes. reset() does not move it either.
 * - alloc() returns NULL when the reservation is exhausted; it never grows the
 *   reservation and never relocates.
 * - reset() rewinds to zero and keeps the committed pages warm (fast).
 * - release() keeps the reservation, returns the physical pages (slow, and the
 *   next touch faults each page back in). Call it when going idle, not per
 *   operation.
 * - Not thread safe: one arena, one owner.
 * --------------------------------------------------------------------------- */

typedef struct mu_vm_arena
{
    mu_vm_region region;
    size_t       head;  /* bytes handed out                     */
    size_t       peak;  /* high-water mark: size the arena from */
    size_t       chunk; /* commit granularity, page rounded     */
} mu_vm_arena;

/* `reserve_bytes` is address space only; `chunk_bytes` is how much is made
   writable per grow (rounded up to a page, minimum one page). */
MU_INLINE bool mu_vm_arena_init(mu_vm_arena* a, size_t reserve_bytes, size_t chunk_bytes)
{
    MU_ASSERT(a);
    if (!a)
        return false;

    a->head  = 0;
    a->peak  = 0;
    a->chunk = 0;

    if (!mu_vm_reserve(&a->region, reserve_bytes))
    {
        memset(a, 0, sizeof(*a));
        return false;
    }

    a->chunk = mu_vm_round_up(chunk_bytes ? chunk_bytes : mu_vm_page_size());
    if (a->chunk == 0)
        a->chunk = mu_vm_page_size();
    return true;
}

MU_INLINE void* mu_vm_arena_alloc(mu_vm_arena* a, size_t size, size_t align)
{
    size_t at;
    size_t need;

    MU_ASSERT(a);
    if (!a || !a->region.base || size == 0)
        return NULL;

    if (align == 0)
        align = 16u;
    MU_ASSERT((align & (align - 1u)) == 0);

    at = MU_ALIGN_UP(a->head, align);
    if (at > a->region.reserved || size > a->region.reserved - at)
        return NULL;

    need = at + size;
    if (need > a->region.committed)
    {
        size_t want = MU_ALIGN_UP(need, a->chunk);
        if (want > a->region.reserved)
            want = a->region.reserved;
        if (!mu_vm_commit(&a->region, want))
            return NULL;
    }

    a->head = need;
    if (need > a->peak)
        a->peak = need;

    return a->region.base + at;
}

/* Rewind. Keeps the committed pages warm, so this is O(1) and does not fault. */
MU_INLINE void mu_vm_arena_reset(mu_vm_arena* a)
{
    MU_ASSERT(a);
    if (a)
        a->head = 0;
}

/* Return the physical pages, keep the address range. The next touch faults. */
MU_INLINE void mu_vm_arena_release(mu_vm_arena* a)
{
    MU_ASSERT(a);
    if (!a)
        return;
    mu_vm_release(&a->region);
    a->head = 0;
}

MU_INLINE void mu_vm_arena_destroy(mu_vm_arena* a)
{
    MU_ASSERT(a);
    if (!a)
        return;
    mu_vm_unmap(&a->region);
    memset(a, 0, sizeof(*a));
}

MU_INLINE void* mu_vm_arena_base(const mu_vm_arena* a)
{
    return a ? (void*)a->region.base : NULL;
}

MU_INLINE size_t mu_vm_arena_used(const mu_vm_arena* a)
{
    return a ? a->head : 0;
}

MU_INLINE size_t mu_vm_arena_peak(const mu_vm_arena* a)
{
    return a ? a->peak : 0;
}

MU_INLINE size_t mu_vm_arena_reserved(const mu_vm_arena* a)
{
    return a ? a->region.reserved : 0;
}

MU_INLINE size_t mu_vm_arena_committed(const mu_vm_arena* a)
{
    return a ? a->region.committed : 0;
}

#endif /* MU_VM_ARENA_H */
