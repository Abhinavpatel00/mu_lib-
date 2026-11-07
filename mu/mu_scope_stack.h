#pragma once
#ifndef MU_SCOPE_STACK_H
#define MU_SCOPE_STACK_H

/* ==========================================================================
 * mu_scope_stack.h — scoped linear allocator with destructors (C99)
 * ==========================================================================
 *
 * This is graphitemaster's scope_stack_alloc idea ("scope stacks": a linear
 * allocator that rewinds when a scope is left, calling destructors) expressed in
 * C, which is the interesting part, because the C++ original leans on RAII:
 *
 *     gStack || [&]() { auto &it = gStack.acquire<std::vector<int>>(); ... };
 *
 * C has no destructors and no lambdas, but it has three things that cover the
 * same ground:
 *
 *   1. `__attribute__((cleanup(fn)))` — a real, automatic scope-exit hook. This
 *      is what makes MU_SCOPE_LIFETIME() below rewind without a macro at every
 *      return, `break` and `goto`.
 *   2. Function pointers stored beside the allocation — the destructor, since a
 *      linear allocator otherwise cannot know whether the bytes it is about to
 *      reclaim held a FILE* or an int.
 *   3. setjmp/longjmp, which is exactly why frame indices exist: a longjmp skips
 *      every cleanup attribute, so the stack must be left in a state where
 *      mu_scope_purge_to() can tear down the frames that were skipped.
 *
 * The rules of the game, inherited from the original:
 *   - Allocations are LIFO. Rewinding a frame invalidates every pointer
 *     acquired after it, which is the whole point (they are all freed at once,
 *     for the price of one pointer store).
 *   - Only the top frame can be allocated from. You can *close* an inner frame
 *     while outer ones are live, but not allocate into a closed one.
 *   - A pointer acquired here must not escape the frame that acquired it.
 *
 * What you get, concretely:
 *   - One bump per allocation, and the whole frame released with `top = saved`.
 *   - No free-list traversal, no per-object header, no fragmentation inside a
 *     frame, and objects allocated together are adjacent, which is the cache
 *     behaviour this is really for.
 *   - Deterministic cost: allocation is a pointer add and a bounds check.
 *
 * Example:
 *
 *     static char arena_mem[1 << 16];
 *     mu_scope_stack arena;
 *     mu_scope_init(&arena, arena_mem, sizeof arena_mem);
 *
 *     void update(void)
 *     {
 *         MU_SCOPE_LIFETIME(&arena);                  // rewinds on every exit
 *
 *         float *scratch = mu_scope_array(&arena, float, 4096);
 *         char  *name    = mu_scope_strdup(&arena, "frame");
 *         FILE  *f       = mu_scope_own(&arena, fopen("x", "rb"), fclose);
 *         ...
 *         return;                                    // all four released here
 *     }
 *
 *     // A second frame inside the first, for a function that wants its own:
 *     void helper(mu_scope_stack *ss)
 *     {
 *         mu_scope_frame f = mu_scope_enter(ss);
 *         ...
 *         mu_scope_leave(ss, f);
 *     }
 *
 * Alignment: every allocation is aligned to MU_SCOPE_ALIGN (16 by default),
 * which is enough for any scalar, any pointer and any 16-byte SIMD type. Ask for
 * more with mu_scope_alloc_aligned().
 *
 * On failure — the arena is full — every allocator here returns NULL. It never
 * wraps, never silently reuses live memory and never aborts; a scratch arena
 * that runs out is a fallback decision, not a crash.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mu_macros.h"

MU_BEGIN_EXTERN_C

#ifndef MU_SCOPE_ALIGN
#  define MU_SCOPE_ALIGN 16u
#endif
#ifndef MU_SCOPE_MAX_ACTIONS
#  define MU_SCOPE_MAX_ACTIONS 256u
#endif

typedef void (*mu_scope_dtor)(void *ptr, void *user);

/* One undo entry. Recorded immediately before the allocation it protects, so a
 * rewind can walk them backwards by comparing against the save point. */
typedef struct mu_scope_action
{
    mu_scope_dtor fn;
    void         *ptr;
    void         *user;
    size_t        mark;     /* stack offset just before the guarded allocation */
} mu_scope_action;

typedef struct mu_scope_stack
{
    unsigned char  *base;
    size_t          capacity;
    size_t          top;            /* bytes handed out so far */
    size_t          peak;           /* high-water mark, for sizing the arena */
    size_t          n_live;         /* live allocations, for reporting */
    uint32_t        depth;          /* frames entered, for assertions */
    mu_scope_action actions[MU_SCOPE_MAX_ACTIONS];
    uint32_t        n_actions;
    uint32_t        n_overflow;     /* times an allocation was refused */
} mu_scope_stack;

typedef struct mu_scope_frame
{
    struct mu_scope_stack *ss;      /* owning stack: lets a cleanup handler
                                     * rewind from the frame's address alone,
                                     * which is all __attribute__((cleanup))
                                     * will hand us */
    size_t   top;
    uint32_t n_actions;
    uint32_t depth;
} mu_scope_frame;

/* --------------------------------------------------------------------------
 * lifecycle
 * -------------------------------------------------------------------------- */

MU_INLINE void mu_scope_init(mu_scope_stack *ss, void *memory, size_t bytes)
{
    ss->base = (unsigned char *)memory;
    ss->capacity = bytes & ~(size_t)(MU_SCOPE_ALIGN - 1u);
    ss->top = 0;
    ss->peak = 0;
    ss->n_live = 0;
    ss->depth = 0;
    ss->n_actions = 0;
    ss->n_overflow = 0;
}

MU_INLINE void mu_scope_reset(mu_scope_stack *ss)
{
    ss->top = 0;
    ss->n_live = 0;
    ss->depth = 0;
    ss->n_actions = 0;
}

MU_INLINE size_t mu_scope_used(const mu_scope_stack *ss) { return ss->top; }
MU_INLINE size_t mu_scope_remaining(const mu_scope_stack *ss) { return ss->capacity - ss->top; }
MU_INLINE size_t mu_scope_peak(const mu_scope_stack *ss) { return ss->peak; }
MU_INLINE uint32_t mu_scope_overflow_count(const mu_scope_stack *ss) { return ss->n_overflow; }

/* --------------------------------------------------------------------------
 * frames
 * -------------------------------------------------------------------------- */

MU_INLINE mu_scope_frame mu_scope_enter(mu_scope_stack *ss)
{
    mu_scope_frame f;

    f.ss = ss;
    f.top = ss->top;
    f.n_actions = ss->n_actions;
    f.depth = ss->depth;
    ss->depth += 1u;
    return f;
}

/* Run the destructors recorded since `mark`, newest first, then rewind. This is
 * the only place memory is ever handed back. */
MU_INLINE void mu_scope_rewind_to(mu_scope_stack *ss, size_t mark, uint32_t n_actions)
{
    while (ss->n_actions > n_actions)
    {
        mu_scope_action *a = &ss->actions[ss->n_actions - 1u];

        ss->n_actions -= 1u;
        if (a->fn) a->fn(a->ptr, a->user);
    }
    ss->top = mark;
    if (ss->depth > 0u) ss->depth -= 1u;
    /* n_live is only a report; recompute it rather than try to track it */
}

MU_INLINE void mu_scope_leave(mu_scope_stack *ss, mu_scope_frame f)
{
    mu_scope_rewind_to(ss, f.top, f.n_actions);
}

/* Tear down everything above `depth` without unwinding — the setjmp/longjmp
 * path, where the cleanup attributes were skipped. */
MU_INLINE void mu_scope_purge_to(mu_scope_stack *ss, mu_scope_frame f)
{
    mu_scope_rewind_to(ss, f.top, f.n_actions);
    ss->depth = f.depth;
}

/* --------------------------------------------------------------------------
 * allocation
 * -------------------------------------------------------------------------- */

MU_INLINE void *mu_scope_alloc_aligned(mu_scope_stack *ss, size_t bytes, size_t align)
{
    const size_t mask = (size_t)(align - 1u);
    size_t       start = (ss->top + mask) & ~mask;

    if (align == 0u || (align & mask) != 0u) return NULL;
    if (start > ss->capacity || bytes > ss->capacity - start)
    {
        ss->n_overflow += 1u;
        return NULL;
    }
    ss->top = start + bytes;
    if (ss->top > ss->peak) ss->peak = ss->top;
    ss->n_live += 1u;
    return ss->base + start;
}

MU_INLINE void *mu_scope_alloc(mu_scope_stack *ss, size_t bytes)
{
    return mu_scope_alloc_aligned(ss, bytes, MU_SCOPE_ALIGN);
}

#define mu_scope_array(ss, type, count) \
    ((type *)mu_scope_alloc((ss), (size_t)(count) * sizeof(type)))

MU_INLINE char *mu_scope_strdup(mu_scope_stack *ss, const char *s)
{
    size_t n;
    char  *p;

    if (!s) return NULL;
    n = strlen(s) + 1u;
    p = (char *)mu_scope_alloc(ss, n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Claim an allocation made ELSEWHERE (a FILE*, a malloc'd buffer) with a
 * destructor, so the frame owns it too. This is the C stand-in for acquiring a
 * C++ object whose destructor runs at scope exit. Pass the same mark semantics:
 * the destructor runs when the frame that registered it is left. */
MU_INLINE void mu_scope_register(mu_scope_stack *ss, void *ptr, mu_scope_dtor fn, void *user)
{
    mu_scope_action *a;

    if (ss->n_actions >= MU_SCOPE_MAX_ACTIONS) return;
    a = &ss->actions[ss->n_actions++];
    a->fn = fn;
    a->ptr = ptr;
    a->user = user;
    a->mark = ss->top;
}

/* Allocate and register a destructor in one step. */
MU_INLINE void *mu_scope_own(mu_scope_stack *ss, void *ptr, mu_scope_dtor fn)
{
    if (ptr) mu_scope_register(ss, ptr, fn, NULL);
    return ptr;
}

/* Wake up an entire frame at a nested depth in one call: enter a "checkpoint"
 * once, and rewind to it repeatedly when you want to reuse the memory. */
MU_INLINE mu_scope_frame mu_scope_checkpoint(mu_scope_stack *ss)
{
    ss->top = (ss->top + (MU_SCOPE_ALIGN - 1u)) & ~(size_t)(MU_SCOPE_ALIGN - 1u);
    return mu_scope_enter(ss);
}

MU_INLINE void mu_scope_rewind(mu_scope_stack *ss, mu_scope_frame f)
{
    mu_scope_rewind_to(ss, f.top, f.n_actions);
    ss->depth += 1u;                 /* rewind_to un-entered the frame */
}

/* --------------------------------------------------------------------------
 * automatic lifetime (the RAII substitute)
 * --------------------------------------------------------------------------
 *
 * __attribute__((cleanup(fn))) runs `fn(&var)` when `var` goes out of scope, on
 * every exit path including `return`, `break`, `goto` and a plain fallthrough.
 * That is the piece C++ gets from destructors, and it is enough to make a scope
 * stack behave like the original: enter on declaration, rewind on any exit.
 *
 * Two consequences worth knowing:
 *   - It does NOT run on longjmp. That is not a defect to work around, it is
 *     why mu_scope_purge_to() exists: a setjmp/longjmp path must purge the
 *     frames it skipped, using a frame captured before the setjmp.
 *   - Only one guard per block is needed no matter how many allocations happen;
 *     the whole point is that one rewind releases all of them.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define MU_SCOPE_HAS_CLEANUP 1

MU_INLINE void mu_scope__leave_frame(mu_scope_frame *f)
{
    mu_scope_leave(f->ss, *f);
}

#  define MU_SCOPE_LIFETIME_IMPL(ss, ctr)                                       \
       __attribute__((cleanup(mu_scope__leave_frame)))                          \
       mu_scope_frame mu_scope_frame_##ctr = mu_scope_enter(ss)

#  define MU_SCOPE_LIFETIME_2(ss, ctr)  MU_SCOPE_LIFETIME_IMPL(ss, ctr)
#  define MU_SCOPE_LIFETIME_1(ss, ctr)  MU_SCOPE_LIFETIME_2(ss, ctr)
#  define MU_SCOPE_LIFETIME(ss)         MU_SCOPE_LIFETIME_1(ss, __COUNTER__)
#else
#  define MU_SCOPE_HAS_CLEANUP 0
   /* No cleanup attribute: the caller must pair this with mu_scope_leave(). */
#  define MU_SCOPE_LIFETIME_IMPL(ss, ctr) mu_scope_frame mu_scope_frame_##ctr = mu_scope_enter(ss)
#  define MU_SCOPE_LIFETIME_2(ss, ctr)  MU_SCOPE_LIFETIME_IMPL(ss, ctr)
#  define MU_SCOPE_LIFETIME_1(ss, ctr)  MU_SCOPE_LIFETIME_2(ss, ctr)
#  define MU_SCOPE_LIFETIME(ss)         MU_SCOPE_LIFETIME_1(ss, __COUNTER__)
#endif

MU_END_EXTERN_C

#endif /* MU_SCOPE_STACK_H */
