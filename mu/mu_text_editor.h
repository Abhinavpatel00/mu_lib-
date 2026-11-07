#pragma once
#ifndef MU_TEXT_EDITOR_H
#define MU_TEXT_EDITOR_H

#include "mu_common.h"
#include "mu_vm.h"
#include "mu_vm_arena.h"

#if defined(__AVX512BW__)
#  include <immintrin.h>
#endif

#ifdef MU_TED_DEBUG
#  define MU_TED_DBG(...) (printf(__VA_ARGS__), fflush(stdout))
#else
#  define MU_TED_DBG(...) ((void)0)
#endif

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * mu_ted — "Ribbon Segments": a text/code-editor buffer.
 *
 * THE PROBLEM
 * -----------
 * Every classic editor buffer trades one query against the others:
 *
 *   gap buffer    O(1) typing at the cursor, O(n) cursor jump, no line index
 *   piece table   O(1) edits, O(log pieces) offset->pos, 8-16 B per piece
 *   rope / treap  O(log n) everything, 24-40 B per node, pointer chase
 *   line array    O(1) line->offset, O(n) rebuild per edit
 *
 * A code editor runs a *mixture*: bursty typing, selection delete, 1 MB paste,
 * goto-line, viewport render (lines i..i+80), scroll, 10 k diagnostics mapped
 * offset -> line:col, undo. The mixture is what the structure is built around,
 * not any single operation.
 *
 * THE STRUCTURE (four flat levels, no trees, no rebalancing, no nodes)
 * -------------------------------------------------------------------
 *   L3 super   : 32 groups     absolute bytes/lines  (u32, few entries)
 *   L2 group   : 32 segments   absolute bytes/lines  (u32, nsegs/32 entries)
 *   L1 segment : 16 KiB text + u16 line starts       (u32, nsegs entries)
 *   L0 text    : raw bytes in 64 B-aligned VM slots
 *
 * Why it wins:
 *
 *  1. A line start costs 2 B, not 8 or 16. It is an offset *inside* a 16 KiB
 *     segment, so u16 suffices; a line's length is derived (next start - 1), so
 *     there is no length array at all. 100 MB / 2.2 M lines => 4.4 MB of line
 *     index in one contiguous array (pure streaming, no pointer chase), versus
 *     ~35 MB for a rope and ~90 MB for a piece table.
 *
 *  2. Every index level is a flat prefix-sum array updated in place. An edit
 *     changes one segment's totals, so exactly one suffix per level needs
 *     `+= delta`: <= 32 + 32 + few u32 adds (one AVX-512 masked add each).
 *     No path copy, no allocation, no renumbering, no rebalancing. That is the
 *     rope/piece-table killer: their per-edit cost is O(log n) *cache misses*;
 *     this is ~40 L1 stores.
 *
 *  3. Queries descend with SIMD masked compares (vpcmpud/vpcmpuw), not
 *     branches: offset -> (line, col) is 3 prefix probes + one in-segment
 *     masked compare. No binary search, no unpredictable branches
 *     (dodnotes Part 3/7), no pointer chase.
 *
 *  4. The cursor cache makes typing ~free: the cached segment absorbs the
 *     keystroke with an 8-16 B store (memmove of the tail inside a 16 KiB
 *     buffer that is L1/L2 resident) plus the suffix adds of item 2.
 *     Keystroke cost is independent of document size.
 *
 *  5. Text memory is 1x + at most one half-full segment: segments are cut at
 *     newline boundaries when possible, and adjacent segments are merged on
 *     delete when they fit, so occupancy stays high.
 *
 *  6. Undo is an inverse-op log with byte payloads (payload only for erases),
 *     coalesced per direction: a 1 MB typing burst costs 1 MB, not n MB.
 *
 * WHY NOT PERSISTENT (path-copying) SEGMENTS
 * ------------------------------------------
 * Undo by structural sharing copies the touched root-to-leaf path per
 * keystroke (or a whole 16 KiB segment at best); a 10 k-keystroke session then
 * holds hundreds of MB of history. The op log stores only the bytes typed.
 * The layout is still COW-ready for *named* snapshots: a snapshot is just the
 * segment pointer array plus the index arrays.
 *
 * CONTRACTS / LIMITS
 * ------------------
 *  - Document < 4 GiB (u32 offsets).
 *  - A line may be longer than a segment: it spans continuation segments and
 *    still costs exactly one line start.
 *  - '\n' is the only terminator; a '\r' is line content (excluded from len).
 *  - Line count = 1 + number of newlines, matching what an editor shows. A
 *    document that ends with '\n' therefore has a trailing empty line whose
 *    cursor position is the end of the text. The empty document has 1 line.
 *  - Not thread safe: one buffer, one owner (dodnotes Appendix M).
 *  - Best with virtual memory; without it the text falls back to one
 *    malloc'd reservation (checked at init).
 *
 * TUNABLES (define before including mu.h)
 * ---------------------------------------
 *   MU_TED_SEG_TEXT     16384  bytes of text per segment slot
 *   MU_TED_GROUP_SEGS     32   segments per group
 *   MU_TED_SUPER_GROUPS   32   groups per super
 *   MU_TED_NO_UNDO        0    1 = compile the undo log out
 * ------------------------------------------------------------------------- */

#ifndef MU_TED_SEG_TEXT
#define MU_TED_SEG_TEXT 16384u
#endif

/* Adaptive segment fill target.
 *
 * A one-byte insert memmoves the segment tail, so the cost of a keystroke is
 * ~nbytes/2. At a fixed 16 KiB fill that is ~8 KiB per character: measured
 * 1344 MB of memmove to replay a 259778-patch session ending at 102 KB
 * (5176 B moved per patch). Crop and JumpRope use ~256 B chunks and move ~128 B,
 * which is the entire reason they win rope-benches' editing traces.
 *
 * Those traces are SMALL documents (18-102 KB). Large documents want large
 * segments: fewer index entries, shallower descent, better scan bandwidth.
 * So the target scales with the document instead of being a constant:
 *
 *   target = clamp(text_len / MU_TED_MIN_SEGS, MU_TED_SEG_MIN, MU_TED_SEG_TEXT)
 *
 * At >= 1 MiB this evaluates to MU_TED_SEG_TEXT, i.e. byte-for-byte the old
 * behaviour, so the large-document query results are untouched by construction.
 * Only documents below ~1 MiB get smaller segments.
 *
 * Set MU_TED_ADAPTIVE_SEG=0 to restore the fixed fill target. */
#ifndef MU_TED_ADAPTIVE_SEG
#define MU_TED_ADAPTIVE_SEG 1
#endif
#ifndef MU_TED_MIN_SEGS
#define MU_TED_MIN_SEGS 32u
#endif
#ifndef MU_TED_SEG_MIN
#define MU_TED_SEG_MIN 2048u
#endif
#ifndef MU_TED_GROUP_SEGS
#define MU_TED_GROUP_SEGS 32u
#endif
#ifndef MU_TED_SUPER_GROUPS
#define MU_TED_SUPER_GROUPS 32u
#endif
#ifndef MU_TED_NO_UNDO
#define MU_TED_NO_UNDO 0
#endif

MU_STATIC_ASSERT(MU_TED_SEG_TEXT <= 65535u, "u16 in-segment line offsets");
MU_STATIC_ASSERT(MU_IS_POW2(MU_TED_GROUP_SEGS), "group is a power of two");
MU_STATIC_ASSERT(MU_IS_POW2(MU_TED_SUPER_GROUPS), "super is a power of two");

/* ==========================================================================
 * Types
 * ========================================================================== */

typedef struct mu_ted_line
{
    uint32_t offset; /* absolute byte offset of the line start */
    uint32_t len;    /* bytes before the '\n' (or end of document) */
} mu_ted_line;

typedef struct mu_ted_cursor
{
    uint32_t line;
    uint32_t col;
    uint32_t offset;
} mu_ted_cursor;

/* One segment: text slot + compact line-start array, single allocation.
 * Header is 24 B, so 200 lines cost 24 + 400 B for up to 16 KiB of text. */
typedef struct mu_ted_seg
{
    uint32_t text;   /* byte offset of this segment's slot in the arena */
    uint32_t nbytes; /* live bytes in the slot                          */
    uint32_t nlines; /* line starts recorded in offs[]                  */
    uint32_t cap;    /* capacity of offs[] in entries                   */
    uint32_t cont;   /* 1 = first byte continues a line from before     */
    uint32_t dead;   /* 1 = tombstone (awaiting compaction)             */
    uint16_t offs[]; /* ascending line-start offsets inside the slot    */
} mu_ted_seg;

#if !MU_TED_NO_UNDO
typedef struct mu_ted_op
{
    uint32_t off;  /* document offset the op started at     */
    uint32_t len;  /* bytes                                 */
    uint32_t pay;  /* payload start in the payload buffer   */
    uint32_t kind; /* MU_TED_OP_INSERT / MU_TED_OP_ERASE    */
} mu_ted_op;

enum mu_ted_op_kind
{
    MU_TED_OP_INSERT = 0u, /* payload = bytes that were inserted */
    MU_TED_OP_ERASE  = 1u  /* payload = bytes that were erased   */
};
#endif

typedef struct mu_ted
{
    /* text slots */
    mu_vm_arena arena;
    uint8_t*    fb_base;   /* fallback reservation when VM is absent */
    size_t      fb_size;
    uint32_t    slot_next; /* bump cursor in slots                   */
    uint32_t    free_slot; /* free-slot list head (slot index)       */

    /* segments + index (all prefix arrays are ABSOLUTE) */
    mu_ted_seg** seg;
    uint32_t     nsegs;
    uint32_t     seg_cap;
    uint32_t     ndead;

    uint32_t* sb; /* bytes       before segment i */
    uint32_t* sl; /* line starts before segment i */

    uint32_t  ngroups; /* ceil(nsegs / GROUP)   */
    uint32_t  group_cap;
    uint32_t* gb; /* bytes       before group g */
    uint32_t* gl; /* line starts before group g */

    uint32_t  nsupers; /* ceil(ngroups / SUPER) */
    uint32_t  super_cap;
    uint32_t* sub; /* bytes       before super s */
    uint32_t* sul; /* line starts before super s */

    /* totals */
    uint32_t text_len;
    uint32_t nlines; /* document lines (>= 1) */

    /* cursor cache */
    uint32_t cache_seg;
    uint32_t cache_bstart;

    /* undo */
#if !MU_TED_NO_UNDO
    mu_ted_op* ops;
    uint32_t   nops;
    uint32_t   ops_cap;
    uint8_t*   pay;     /* payloads grow down from pay_lo, up to pay_hi */
    uint32_t   pay_cap;
    uint32_t   pay_lo;
    uint32_t   pay_hi;
    uint32_t   undo_barrier;
    uint32_t   in_undo;
#endif
} mu_ted;

/* ==========================================================================
 * Scan helpers (dodnotes Part 6/7: masked compares, not branches)
 * ========================================================================== */

/* Last index with a[k] <= v (arrays ascending), or UINT32_MAX. */
MU_INLINE uint32_t mu_ted__last_le_u16(const uint16_t* a, uint32_t n, uint16_t v)
{
#if defined(__AVX512BW__)
    uint32_t i = n;
    const __m512i vv = _mm512_set1_epi16((short)v);
    while (i >= 32u)
    {
        i -= 32u;
        const __m512i x   = _mm512_loadu_si512((const void*)(a + i));
        const __mmask32 m = _mm512_cmple_epu16_mask(x, vv);
        if (m)
            return i + 31u - (uint32_t)__builtin_clz((unsigned)m);
    }
    uint32_t best = UINT32_MAX;
    for (uint32_t k = 0; k < i; ++k)
        if (a[k] <= v)
            best = k;
    return best;
#else
    uint32_t best = UINT32_MAX;
    for (uint32_t k = 0; k < n; ++k)
        if (a[k] <= v)
            best = k; /* branchless-ish: single cmov per element */
    return best;
#endif
}

MU_INLINE uint32_t mu_ted__last_le_u32(const uint32_t* a, uint32_t n, uint32_t v)
{
#if defined(__AVX512F__)
    uint32_t i = n;
    const __m512i vv = _mm512_set1_epi32((int)v);
    while (i >= 16u)
    {
        i -= 16u;
        const __m512i x   = _mm512_loadu_si512((const void*)(a + i));
        const __mmask16 m = _mm512_cmple_epu32_mask(x, vv);
        if (m)
            return i + 31u - (uint32_t)__builtin_clz((unsigned)m);
    }
    uint32_t best = UINT32_MAX;
    for (uint32_t k = 0; k < i; ++k)
        if (a[k] <= v)
            best = k;
    return best;
#else
    uint32_t best = UINT32_MAX;
    for (uint32_t k = 0; k < n; ++k)
        if (a[k] <= v)
            best = k;
    return best;
#endif
}

/* First '\n' in [0,n), or n. */
MU_INLINE uint32_t mu_ted__find_nl(const char* p, uint32_t n)
{
    uint32_t i = 0;
#if defined(__AVX512BW__)
    const __m512i nl = _mm512_set1_epi8('\n');
    while (i + 64u <= n)
    {
        const __m512i  x = _mm512_loadu_si512((const void*)(p + i));
        const uint64_t m = (uint64_t)_mm512_cmpeq_epi8_mask(x, nl);
        if (m)
            return i + (uint32_t)__builtin_ctzll(m);
        i += 64u;
    }
#endif
    for (; i < n; ++i)
        if (p[i] == '\n')
            return i;
    return n;
}

/* Last '\n' at index < n, or UINT32_MAX. */
MU_INLINE uint32_t mu_ted__rfind_nl(const char* p, uint32_t n)
{
    uint32_t i = n;
#if defined(__AVX512BW__)
    const __m512i nl = _mm512_set1_epi8('\n');
    while (i >= 64u)
    {
        i -= 64u;
        const __m512i  x = _mm512_loadu_si512((const void*)(p + i));
        const uint64_t m = (uint64_t)_mm512_cmpeq_epi8_mask(x, nl);
        if (m)
            return i + 63u - (uint32_t)__builtin_clzll(m);
    }
    while (i-- > 0u)
        if (p[i] == '\n')
            return i;
    return UINT32_MAX;
#else
    while (i-- > 0u)
        if (p[i] == '\n')
            return i;
    return UINT32_MAX;
#endif
}

/* ==========================================================================
 * Internals: storage
 * ========================================================================== */

MU_INLINE uint8_t* mu_ted__base(const mu_ted* t)
{
    return t->arena.region.base ? t->arena.region.base : t->fb_base;
}

MU_INLINE uint8_t* mu_ted__seg_text(const mu_ted* t, const mu_ted_seg* s)
{
    return mu_ted__base(t) + s->text;
}

MU_INLINE uint32_t mu_ted__alloc_slot(mu_ted* t)
{
    if (t->free_slot != UINT32_MAX)
    {
        const uint32_t s = t->free_slot;
        t->free_slot     = *(uint32_t*)(mu_ted__base(t) + (size_t)s * MU_TED_SEG_TEXT);
        return s;
    }
    const size_t byte = (size_t)t->slot_next * MU_TED_SEG_TEXT;
    if (t->arena.region.base)
    {
        if (byte + MU_TED_SEG_TEXT > t->arena.region.reserved)
            return UINT32_MAX;
        if (byte + MU_TED_SEG_TEXT > t->arena.region.committed)
        {
            size_t want = MU_ALIGN_UP(byte + MU_TED_SEG_TEXT, t->arena.chunk);
            if (want > t->arena.region.reserved)
                want = t->arena.region.reserved;
            if (!mu_vm_commit(&t->arena.region, want))
                return UINT32_MAX;
        }
    }
    else if (byte + MU_TED_SEG_TEXT > t->fb_size)
        return UINT32_MAX;

    t->slot_next++;
    return t->slot_next - 1u;
}

MU_INLINE void mu_ted__free_slot(mu_ted* t, uint32_t slot)
{
    *(uint32_t*)(mu_ted__base(t) + (size_t)slot * MU_TED_SEG_TEXT) = t->free_slot;
    t->free_slot                                                  = slot;
}

MU_INLINE mu_ted_seg* mu_ted__seg_new(mu_ted* t, uint32_t slot, uint32_t cap)
{
    MU_UNUSED(t);
    if (cap < 4u)
        cap = 4u;
    mu_ted_seg* s = (mu_ted_seg*)MU_MALLOC(sizeof(mu_ted_seg) + (size_t)cap * sizeof(uint16_t));
    if (!s)
        return NULL;
    s->text   = slot * MU_TED_SEG_TEXT;
    s->nbytes = 0;
    s->nlines = 0;
    s->cap    = cap;
    s->cont   = 0;
    s->dead   = 0;
    return s;
}

MU_INLINE void mu_ted__seg_free(mu_ted* t, mu_ted_seg* s)
{
    if (!s)
        return;
    mu_ted__free_slot(t, s->text / MU_TED_SEG_TEXT);
    MU_FREE(s);
}

/* ==========================================================================
 * Internals: prefix index (the only place edit costs live)
 * ========================================================================== */

MU_INLINE void mu_ted__add_u32(uint32_t* a, uint32_t from, uint32_t to, uint32_t d)
{
    if (from >= to || d == 0u)
        return;
    uint32_t i = from;
#if defined(__AVX512F__) && !defined(MU_TED_NO_SIMD_ADD)
    const __m512i dd = _mm512_set1_epi32((int)d);
    while (i + 16u <= to)
    {
        _mm512_storeu_si512((void*)(a + i),
                            _mm512_add_epi32(_mm512_loadu_si512((const void*)(a + i)), dd));
        i += 16u;
    }
#endif
    for (; i < to; ++i)
        a[i] += d;
}

/* Absolute byte offset / line-start count before segment i.
 * sb/sl hold intra-group offsets, gb/gl intra-super offsets, sub/sul are
 * absolute: every level only ever shifts inside its own parent, which is what
 * keeps a keystroke down to a couple of short vector adds. */
MU_INLINE uint32_t mu_ted__bstart(const mu_ted* t, uint32_t i)
{
    const uint32_t g = i / MU_TED_GROUP_SEGS;
    return t->sub[g / MU_TED_SUPER_GROUPS] + t->gb[g] + t->sb[i];
}

MU_INLINE uint32_t mu_ted__lstart(const mu_ted* t, uint32_t i)
{
    const uint32_t g = i / MU_TED_GROUP_SEGS;
    return t->sul[g / MU_TED_SUPER_GROUPS] + t->gl[g] + t->sl[i];
}

/* Segment i's totals changed by (db, dl): shift the later entries of i's
 * group, then the later groups of i's super, then the later supers. */
MU_INLINE void mu_ted__bump(mu_ted* t, uint32_t i, uint32_t db, uint32_t dl)
{
#ifdef MU_TED_DEBUG_BUMP
    {
        uint32_t sum = 0;
        for (uint32_t q = 0; q < t->nsegs; ++q)
            sum += t->seg[q]->dead ? 0u : t->seg[q]->nlines;
        if (sum != t->nlines)
            MU_TED_DBG("BUMP drift BEFORE: caller=%p i=%u db=%d dl=%d sum=%u nlines=%u\n",
                       __builtin_return_address(0), i, (int)db, (int)dl, sum, t->nlines);
    }
#endif
    const uint32_t g = i / MU_TED_GROUP_SEGS;
    const uint32_t s = g / MU_TED_SUPER_GROUPS;

    const uint32_t gend = MU_MIN((g + 1u) * MU_TED_GROUP_SEGS, t->nsegs);
    mu_ted__add_u32(t->sb, i + 1u, gend, db);
    mu_ted__add_u32(t->sl, i + 1u, gend, dl);

    const uint32_t send = MU_MIN((s + 1u) * MU_TED_SUPER_GROUPS, t->ngroups);
    mu_ted__add_u32(t->gb, g + 1u, send, db);
    mu_ted__add_u32(t->gl, g + 1u, send, dl);
    mu_ted__add_u32(t->sub, s + 1u, t->nsupers, db);
    mu_ted__add_u32(t->sul, s + 1u, t->nsupers, dl);

    t->text_len += db;
    t->nlines += dl;
}

/* Recompute every level from the segment headers. O(nsegs), used only by
 * compaction and by splits (once per ~8 KiB typed). */
MU_INLINE void mu_ted__rebuild_all(mu_ted* t)
{
    const uint32_t ng = (t->nsegs + MU_TED_GROUP_SEGS - 1u) / MU_TED_GROUP_SEGS;
    const uint32_t ns = (ng + MU_TED_SUPER_GROUPS - 1u) / MU_TED_SUPER_GROUPS;
    t->ngroups = ng;
    t->nsupers = ns;

    uint32_t gbase = 0, sbase = 0; /* absolute start of the current group/super */
    uint32_t g = 0;
    for (uint32_t si = 0; si < ns; ++si)
    {
        t->sub[si] = sbase;
        t->sul[si] = 0u; /* filled below for lines */
        const uint32_t gend = MU_MIN(g + MU_TED_SUPER_GROUPS, ng);
        for (; g < gend; ++g)
        {
            t->gb[g] = gbase - sbase;
            t->gl[g] = 0u;
            const uint32_t iend = MU_MIN((g + 1u) * MU_TED_GROUP_SEGS, t->nsegs);
            uint32_t ib = 0;
            for (uint32_t i = g * MU_TED_GROUP_SEGS; i < iend; ++i)
            {
                const mu_ted_seg* q = t->seg[i];
                t->sb[i] = ib;
                ib += q->nbytes;
            }
            gbase += ib;
        }
        sbase = gbase;
    }
    t->text_len = gbase;

    /* Lines: same walk, separate accumulators. */
    uint32_t lgbase = 0, lsbase = 0, l = 0, total_lines = 0;
    for (uint32_t si = 0; si < ns; ++si)
    {
        t->sul[si] = lsbase;
        const uint32_t gend = MU_MIN(l + MU_TED_SUPER_GROUPS, ng);
        for (; l < gend; ++l)
        {
            t->gl[l] = lgbase - lsbase;
            const uint32_t iend = MU_MIN((l + 1u) * MU_TED_GROUP_SEGS, t->nsegs);
            uint32_t il = 0;
            for (uint32_t i = l * MU_TED_GROUP_SEGS; i < iend; ++i)
            {
                const mu_ted_seg* q = t->seg[i];
                t->sl[i] = il;
                il += q->nlines;
            }
            lgbase += il;
        }
        lsbase = lgbase;
    }
    total_lines = lsbase;

    t->nlines    = total_lines ? total_lines : 1u;
    t->cache_seg = UINT32_MAX;
}

MU_INLINE bool mu_ted__grow_segs(mu_ted* t, uint32_t need)
{
    if (need <= t->seg_cap)
        return true;
    uint32_t cap = t->seg_cap ? t->seg_cap : 16u;
    while (cap < need)
        cap *= 2u;

    mu_ted_seg** ns = (mu_ted_seg**)MU_REALLOC(t->seg, (size_t)cap * sizeof(mu_ted_seg*));
    if (!ns)
        return false;
    t->seg = ns;
    uint32_t* nb = (uint32_t*)MU_REALLOC(t->sb, (size_t)cap * sizeof(uint32_t));
    if (!nb)
        return false;
    t->sb = nb;
    uint32_t* nl = (uint32_t*)MU_REALLOC(t->sl, (size_t)cap * sizeof(uint32_t));
    if (!nl)
        return false;
    t->sl      = nl;
    t->seg_cap = cap;
    return true;
}

MU_INLINE bool mu_ted__grow_groups(mu_ted* t, uint32_t need_groups)
{
    if (need_groups > t->group_cap)
    {
        uint32_t cap = t->group_cap ? t->group_cap : 8u;
        while (cap < need_groups)
            cap *= 2u;
        uint32_t* nb = (uint32_t*)MU_REALLOC(t->gb, (size_t)cap * sizeof(uint32_t));
        if (!nb)
            return false;
        t->gb = nb;
        uint32_t* nl = (uint32_t*)MU_REALLOC(t->gl, (size_t)cap * sizeof(uint32_t));
        if (!nl)
            return false;
        t->gl        = nl;
        t->group_cap = cap;
    }
    const uint32_t need_supers = (need_groups + MU_TED_SUPER_GROUPS - 1u) / MU_TED_SUPER_GROUPS;
    if (need_supers > t->super_cap)
    {
        uint32_t cap = t->super_cap ? t->super_cap : 4u;
        while (cap < need_supers)
            cap *= 2u;
        uint32_t* nb = (uint32_t*)MU_REALLOC(t->sub, (size_t)cap * sizeof(uint32_t));
        if (!nb)
            return false;
        t->sub = nb;
        uint32_t* nl = (uint32_t*)MU_REALLOC(t->sul, (size_t)cap * sizeof(uint32_t));
        if (!nl)
            return false;
        t->sul       = nl;
        t->super_cap = cap;
    }
    return true;
}

/* ==========================================================================
 * Internals: per-segment line-start array edits
 * ========================================================================== */

MU_INLINE bool mu_ted__seg_reserve_lines(mu_ted* t, uint32_t i, uint32_t need)
{
    mu_ted_seg* s = t->seg[i];
    if (need <= s->cap)
        return true;
    uint32_t cap = s->cap ? s->cap : 8u;
    while (cap < need)
        cap *= 2u;
    mu_ted_seg* ns =
        (mu_ted_seg*)MU_REALLOC(s, sizeof(mu_ted_seg) + (size_t)cap * sizeof(uint16_t));
    if (!ns)
        return false;
    ns->cap   = cap;
    t->seg[i] = ns;
    return true;
}

/* Number of newlines in d[0..len). */
MU_INLINE uint32_t mu_ted__count_nl(const char* d, uint32_t len)
{
    uint32_t n = 0, i = 0;
    while (i < len)
    {
        const uint32_t k = mu_ted__find_nl(d + i, len - i);
        i += k + 1u;
        if (k >= len - (i - k - 1u))
            break;
        ++n;
    }
    return n;
}

/* Insert len bytes at local offset p of segment i (caller guarantees it fits).
 * Returns the number of line starts added via *out_starts. */
MU_INLINE bool mu_ted__seg_insert(mu_ted* t, uint32_t i, uint32_t p, const char* d, uint32_t len,
                                  uint32_t* out_starts)
{
    *out_starts = 0u;
    if (len == 0u)
        return true;
    if (p > t->seg[i]->nbytes)
    {
        MU_TED_DBG("BAD seg_insert i=%u p=%u nbytes=%u nlines=%u cont=%u nsegs=%u\n", i, p,
                   t->seg[i]->nbytes, t->seg[i]->nlines, t->seg[i]->cont, t->nsegs);
    }
    MU_ASSERT(t->seg[i]->nbytes + len <= MU_TED_SEG_TEXT);

    uint32_t k = 0;
    {
        uint32_t o = 0;
        while (o < len)
        {
            const uint32_t q = mu_ted__find_nl(d + o, len - o);
            if (q == len - o)
                break;
            ++k;
            o += q + 1u;
        }
    }

    /* BUG #8. A '\n' at local p-1 owns a line start at p. When p == nbytes that
     * start is not stored here: with a live successor trim_tail_start handed it
     * over, and the successor holds it as offs[0] == 0. Inserting AT nbytes
     * makes it an interior start of THIS segment, so it has to be materialised
     * locally -- fix_boundary is about to turn the successor into a
     * continuation (the inserted tail ends mid-line) and delete its copy, and
     * then the line exists in the bytes but in neither index.
     *
     * Reached whenever a chunked insert resumes at a segment boundary that
     * lands on a newline: mu_ted__split assigns the start at `cut` to the right
     * piece, but the insert loop only hops right when p > cut, so p == cut
     * keeps writing left. Latent while a 16 KiB slot swallowed nearly every
     * paste whole; the adaptive segment target makes chunked inserts routine.
     * Repro: ted_lines_test flavour 2 seed 7 step 1, 2187 B at 275311. */
    uint32_t boundary = 0;
    {
        const mu_ted_seg* s0  = t->seg[i];
        const uint8_t*    tx0 = mu_ted__seg_text(t, s0);
        if (p == s0->nbytes && p > 0u && tx0[p - 1u] == '\n' &&
            (s0->nlines == 0u || (uint32_t)s0->offs[s0->nlines - 1u] != p))
            boundary = 1u;
    }

    if (!mu_ted__seg_reserve_lines(t, i, t->seg[i]->nlines + k + boundary))
        return false;

    mu_ted_seg* s   = t->seg[i];
    uint8_t*    txt = mu_ted__seg_text(t, s);
    MU_MEMMOVE(txt + p + len, txt + p, (size_t)(s->nbytes - p));
    MU_MEMCPY(txt + p, d, len);

    /* Existing starts <= p stay put; starts > p shift by len. */
    const uint32_t jl   = mu_ted__last_le_u16(s->offs, s->nlines, (uint16_t)p);
    const uint32_t keep = (jl == UINT32_MAX) ? 0u : (jl + 1u);
    const uint32_t tail = s->nlines - keep;

    if (tail)
        MU_MEMMOVE(s->offs + keep + k, s->offs + keep, (size_t)tail * sizeof(uint16_t));

    {
        uint32_t x = 0;
        uint32_t* o = (uint32_t*)(void*)0; /* silence unused warnings */
        (void)o;
        uint16_t* base = s->offs + keep + boundary + k;
#if defined(__AVX512BW__)
        for (; x + 32u <= tail; x += 32u)
        {
            __m512i v = _mm512_loadu_si512((const void*)(base + x));
            v         = _mm512_add_epi16(v, _mm512_set1_epi16((short)len));
            _mm512_storeu_si512((void*)(base + x), v);
        }
#elif defined(__AVX2__)
        for (; x + 8u <= tail; x += 8u)
        {
            __m128i v = _mm_loadu_si128((const __m128i*)(base + x));
            v         = _mm_add_epi16(v, _mm_set1_epi16((short)len));
            _mm_storeu_si128((__m128i*)(base + x), v);
        }
#endif
        for (; x < tail; ++x)
            base[x] = (uint16_t)(base[x] + len);
    }

    {
        uint32_t w = keep, o = 0;
        if (boundary)
            s->offs[w++] = (uint16_t)p;
        while (o < len)
        {
            const uint32_t q = mu_ted__find_nl(d + o, len - o);
            if (q == len - o)
                break;
            s->offs[w++] = (uint16_t)(p + o + q + 1u);
            o += q + 1u;
        }
        MU_ASSERT(w == keep + boundary + k);
    }

    s->nbytes += len;
    s->nlines += k + boundary;
    *out_starts = k + boundary;
    return true;
}

/* Erase [p, p+len) from segment i. */
MU_INLINE void mu_ted__seg_erase(mu_ted* t, uint32_t i, uint32_t p, uint32_t len)
{
    mu_ted_seg* s   = t->seg[i];
    uint8_t*    txt = mu_ted__seg_text(t, s);
#ifdef MU_TED_DEBUG
    {
        uint32_t nlx = 0;
        for (uint32_t q = p; q < p + len; ++q)
            if (txt[q] == '\n')
                ++nlx;
        MU_TED_DBG("    seg_erase i=%u p=%u len=%u nl_in_range=%u nbytes=%u nlines=%u\n", i, p, len,
                   nlx, s->nbytes, s->nlines);
    }
#endif
    MU_MEMMOVE(txt + p, txt + p + len, (size_t)(s->nbytes - p - len));
    s->nbytes -= len;

    /* Starts in (p, p+len] belonged to deleted newlines: drop them.
     * Surviving starts (> p+len) shift down by len. Starts == p stay. */
    const uint32_t jl    = mu_ted__last_le_u16(s->offs, s->nlines, (uint16_t)p);
    const uint32_t keep  = (jl == UINT32_MAX) ? 0u : (jl + 1u);
    const uint32_t jt    = mu_ted__last_le_u16(s->offs, s->nlines, (uint16_t)(p + len));
    const uint32_t tfrom = (jt == UINT32_MAX) ? 0u : (jt + 1u);
    const uint32_t tail  = s->nlines - tfrom;

    if (tail)
        MU_MEMMOVE(s->offs + keep, s->offs + tfrom, (size_t)tail * sizeof(uint16_t));

    {
        uint32_t  x    = 0;
        uint16_t* base = s->offs + keep;
#if defined(__AVX512BW__)
        for (; x + 32u <= tail; x += 32u)
        {
            __m512i v = _mm512_loadu_si512((const void*)(base + x));
            v         = _mm512_sub_epi16(v, _mm512_set1_epi16((short)len));
            _mm512_storeu_si512((void*)(base + x), v);
        }
#elif defined(__AVX2__)
        for (; x + 8u <= tail; x += 8u)
        {
            __m128i v = _mm_loadu_si128((const __m128i*)(base + x));
            v         = _mm_sub_epi16(v, _mm_set1_epi16((short)len));
            _mm_storeu_si128((__m128i*)(base + x), v);
        }
#endif
        for (; x < tail; ++x)
            base[x] = (uint16_t)(base[x] - len);
    }
#ifdef MU_TED_DEBUG
    MU_TED_DBG("      keep=%u jt=%u tfrom=%u tail=%u -> nlines=%u\n", keep, jt, tfrom, tail,
               keep + tail);
#endif
    s->nlines = keep + tail;
}

/* Does segment i end inside a line? */
MU_INLINE uint32_t mu_ted__ends_mid_line(const mu_ted* t, const mu_ted_seg* s)
{
    if (s->nbytes == 0u)
        return 0u;
    return mu_ted__seg_text(t, s)[s->nbytes - 1u] != '\n';
}

/* A line start at local offset == nbytes means "the line that begins at the
 * first byte of the next segment". It is only recorded in the LAST segment
 * (the phantom trailing line); otherwise it duplicates the next segment's
 * start at 0. */
MU_INLINE void mu_ted__trim_tail_start(mu_ted* t, uint32_t i)
{
    mu_ted_seg* s = t->seg[i];
    if (s->nlines == 0u || s->nbytes == 0u)
        return;
    if ((uint32_t)s->offs[s->nlines - 1u] != s->nbytes)
        return;
    uint32_t j = i + 1u;
    while (j < t->nsegs && t->seg[j]->dead)
        ++j;
    if (j >= t->nsegs)
        return; /* phantom start is legitimate at end of document */
    s->nlines--;
    mu_ted__bump(t, i, 0u, (uint32_t)0u - 1u);
}

/* Inverse of trim_tail_start. When segment i has become the LAST live segment
 * and its text ends with '\n', the empty final line needs a start at nbytes.
 * Erasing a whole trailing segment removed the successor that owned that start
 * and nothing re-installed it in the predecessor (whose own copy had been
 * trimmed earlier, correctly, while the successor was alive), so the document
 * reported one line too few. Cold path: once per erase, not per chunk. */
MU_INLINE void mu_ted__restore_tail_start(mu_ted* t, uint32_t i)
{
    mu_ted_seg* s;
    uint32_t    j;
    if (i >= t->nsegs)
        return;
    s = t->seg[i];
    if (s->dead || s->nbytes == 0u)
        return;
    if (mu_ted__seg_text(t, s)[s->nbytes - 1u] != '\n')
        return;
    if (s->nlines && (uint32_t)s->offs[s->nlines - 1u] == s->nbytes)
        return; /* already present */
    j = i + 1u;
    while (j < t->nsegs && t->seg[j]->dead)
        ++j;
    if (j < t->nsegs)
        return; /* a live successor owns that start */
    if (!mu_ted__seg_reserve_lines(t, i, s->nlines + 1u))
        return;
    s = t->seg[i]; /* reserve may have moved the block */
    s->offs[s->nlines++] = (uint16_t)s->nbytes;
    mu_ted__bump(t, i, 0u, 1u);
}

/* Symmetric to restore_tail_start: the document's FIRST line always starts at
 * byte 0. Erasing a whole leading segment leaves the new first segment still
 * flagged as a continuation with no start at 0, so line 0 resolved to the
 * second line and the count was one short. Mirrors fix_boundary's install
 * branch, which cannot run here because the predecessor no longer exists. */
MU_INLINE void mu_ted__restore_head_start(mu_ted* t, uint32_t i)
{
    mu_ted_seg* s;
    if (i >= t->nsegs)
        return;
    s = t->seg[i];
    if (s->dead || s->nbytes == 0u)
        return;
    if (s->nlines && s->offs[0] == 0u)
    {
        s->cont = 0u;
        return;
    }
    if (mu_ted__seg_reserve_lines(t, i, s->nlines + 1u))
    {
        s = t->seg[i]; /* reserve may have moved the block */
        MU_MEMMOVE(s->offs + 1, s->offs, (size_t)s->nlines * sizeof(uint16_t));
        s->offs[0] = 0u;
        s->nlines++;
        mu_ted__bump(t, i, 0u, 1u);
    }
    t->seg[i]->cont = 0u;
}

/* Restore the invariant between segment i and i+1 after i changed shape. */
MU_INLINE void mu_ted__fix_boundary(mu_ted* t, uint32_t i)
{
    if (i + 1u >= t->nsegs)
        return;
    /* An EMPTY segment has no last byte, so it cannot say whether the next
     * segment continues a line. Treating it as "ends with a newline" (which
     * ends_mid_line reports for nbytes==0) installed a phantom line start at
     * the successor whenever a whole segment was erased. The real boundary
     * is re-checked by the predecessor once the hole is closed. */
    if (t->seg[i]->nbytes == 0u)
        return;
    uint32_t j = i + 1u;
    while (j < t->nsegs && t->seg[j]->dead)
        ++j;
    if (j >= t->nsegs)
        return;

    mu_ted_seg* nx = t->seg[j];
    const uint32_t want_cont = mu_ted__ends_mid_line(t, t->seg[i]);
    if (want_cont == nx->cont)
        return;

    if (want_cont)
    {
        if (nx->nlines > 0u && nx->offs[0] == 0u)
        {
            MU_MEMMOVE(nx->offs, nx->offs + 1, (size_t)(nx->nlines - 1u) * sizeof(uint16_t));
            nx->nlines--;
            mu_ted__bump(t, j, 0u, (uint32_t)0u - 1u);
        }
        nx->cont = 1u;
    }
    else
    {
        if (mu_ted__seg_reserve_lines(t, j, nx->nlines + 1u))
        {
            nx = t->seg[j];
            MU_MEMMOVE(nx->offs + 1, nx->offs, (size_t)nx->nlines * sizeof(uint16_t));
            nx->offs[0] = 0u;
            nx->nlines++;
            mu_ted__bump(t, j, 0u, 1u);
        }
        nx->cont = 0u;
    }
}

/* Soft fill target for the current document size. See MU_TED_ADAPTIVE_SEG.
 * Slots stay MU_TED_SEG_TEXT so allocation is unchanged; only the point at
 * which a segment splits (and the point at which two merge) moves. */
MU_INLINE uint32_t mu_ted__seg_target(const mu_ted* t)
{
#if MU_TED_ADAPTIVE_SEG
    uint32_t want = t->text_len / MU_TED_MIN_SEGS;
    if (want < MU_TED_SEG_MIN)
        want = MU_TED_SEG_MIN;
    if (want > MU_TED_SEG_TEXT)
        want = MU_TED_SEG_TEXT;
    return want;
#else
    (void)t;
    return MU_TED_SEG_TEXT;
#endif
}

/* ==========================================================================
 * Internals: descent
 * ========================================================================== */

MU_INLINE uint32_t mu_ted__seg_by_byte(const mu_ted* t, uint32_t off)
{
    if (t->nsegs == 0u)
        return UINT32_MAX;

    uint32_t s = 0;
    if (t->nsupers > 1u)
    {
        const uint32_t k = mu_ted__last_le_u32(t->sub, t->nsupers, off);
        if (k != UINT32_MAX)
            s = k;
    }
    const uint32_t gbase = s * MU_TED_SUPER_GROUPS;
    const uint32_t gend  = MU_MIN(gbase + MU_TED_SUPER_GROUPS, t->ngroups);
    uint32_t       g     = gbase;
    if (gend > gbase + 1u)
    {
        const uint32_t k = mu_ted__last_le_u32(t->gb + gbase, gend - gbase, off - t->sub[s]);
        if (k != UINT32_MAX)
            g = gbase + k;
    }

    const uint32_t ibase = g * MU_TED_GROUP_SEGS;
    const uint32_t iend  = MU_MIN(ibase + MU_TED_GROUP_SEGS, t->nsegs);
    const uint32_t rel   = off - t->sub[s] - t->gb[g];
    uint32_t       i     = ibase;
    {
        const uint32_t k = mu_ted__last_le_u32(t->sb + ibase, iend - ibase, rel);
        if (k != UINT32_MAX)
            i = ibase + k;
    }

    /* Normalize over tombstones and zero-length segments. */
    while (i > 0u && (t->seg[i]->dead || mu_ted__bstart(t, i) > off))
        --i;
    for (;;)
    {
        const mu_ted_seg* sg = t->seg[i];
        const uint32_t    bs = mu_ted__bstart(t, i);
        if (!sg->dead && (bs + sg->nbytes > off || i + 1u >= t->nsegs))
            return i;
        uint32_t j = i + 1u;
        while (j < t->nsegs && t->seg[j]->dead)
            ++j;
        if (j >= t->nsegs || mu_ted__bstart(t, j) > off)
            return i;
        i = j;
    }
}

MU_INLINE uint32_t mu_ted__seg_by_line(const mu_ted* t, uint32_t line, uint32_t* local)
{
    *local = 0u;
    if (t->nsegs == 0u)
        return UINT32_MAX;

    uint32_t s = 0;
    if (t->nsupers > 1u)
    {
        const uint32_t k = mu_ted__last_le_u32(t->sul, t->nsupers, line);
        if (k != UINT32_MAX)
            s = k;
    }
    const uint32_t gbase = s * MU_TED_SUPER_GROUPS;
    const uint32_t gend  = MU_MIN(gbase + MU_TED_SUPER_GROUPS, t->ngroups);
    uint32_t       g     = gbase;
    if (gend > gbase + 1u)
    {
        const uint32_t k = mu_ted__last_le_u32(t->gl + gbase, gend - gbase, line - t->sul[s]);
        if (k != UINT32_MAX)
            g = gbase + k;
    }

    const uint32_t ibase = g * MU_TED_GROUP_SEGS;
    const uint32_t iend  = MU_MIN(ibase + MU_TED_GROUP_SEGS, t->nsegs);
    const uint32_t rel   = line - t->sul[s] - t->gl[g];
    uint32_t       i     = ibase;
    {
        const uint32_t k = mu_ted__last_le_u32(t->sl + ibase, iend - ibase, rel);
        if (k != UINT32_MAX)
            i = ibase + k;
    }

    while (i > 0u && (t->seg[i]->nlines == 0u || mu_ted__lstart(t, i) > line))
        --i;
    for (;;)
    {
        const mu_ted_seg* sg = t->seg[i];
        const uint32_t    ls = mu_ted__lstart(t, i);
        if (!sg->dead && sg->nlines && line < ls + sg->nlines)
        {
            *local = line - ls;
            return i;
        }
        uint32_t j = i + 1u;
        while (j < t->nsegs && (t->seg[j]->dead || t->seg[j]->nlines == 0u))
            ++j;
        if (j >= t->nsegs || mu_ted__lstart(t, j) > line)
        {
            *local = (line > mu_ted__lstart(t, i)) ? (line - mu_ted__lstart(t, i)) : 0u;
            return i;
        }
        i = j;
    }
}


#ifdef MU_TED_DEBUG
MU_INLINE uint32_t mu_ted__headersum(const mu_ted* t)
{
    uint32_t sum = 0;
    for (uint32_t q = 0; q < t->nsegs; ++q)
        sum += t->seg[q]->dead ? 0u : t->seg[q]->nlines;
    return sum;
}
/* Internal consistency probe: sum of headers vs totals, plus per-segment
 * line-start/byte agreement. Prints and returns false on the first break. */
MU_INLINE bool mu_ted__check(const mu_ted* t, const char* where)
{
    uint32_t sum = 0, bytes = 0;
    for (uint32_t i = 0; i < t->nsegs; ++i)
    {
        const mu_ted_seg* s = t->seg[i];
        if (s->dead)
        {
            if (s->nbytes || s->nlines)
            {
                MU_TED_DBG("[%s] dead seg %u holds nb=%u nl=%u\n", where, i, s->nbytes, s->nlines);
                return false;
            }
            continue;
        }
        sum += s->nlines;
        bytes += s->nbytes;
        const uint8_t* txt = mu_ted__seg_text(t, s);
        /* Starts contributed by newlines: only those still inside the slot.
         * A newline whose successor byte is exactly nbytes must be recorded in
         * the NEXT segment (except at end of document, where it is the phantom
         * trailing line). */
        uint32_t starts_nl = 0;
        for (uint32_t q = 0; q + 1u < s->nbytes; ++q)
            if (txt[q] == '\n')
                ++starts_nl;
        if (i + 1u >= t->nsegs && s->nbytes && txt[s->nbytes - 1u] == '\n')
            ++starts_nl; /* phantom trailing line at EOF */
        const uint32_t starts_zero = (s->nlines && s->offs[0] == 0u) ? 1u : 0u;
        if (starts_nl + starts_zero != s->nlines)
        {
            MU_TED_DBG("[%s] seg %u: nl_starts=%u zero_start=%u nlines=%u cont=%u nb=%u\n", where, i,
                       starts_nl, starts_zero, s->nlines, s->cont, s->nbytes);
            return false;
        }
    }
    if (sum != t->nlines || bytes != t->text_len)
    {
        MU_TED_DBG("[%s] totals: sum_lines=%u nlines=%u sum_bytes=%u text_len=%u\n", where, sum,
                   t->nlines, bytes, t->text_len);
        return false;
    }
    return true;
}
#endif

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

MU_INLINE bool mu_ted_init(mu_ted* t, size_t reserve_bytes)
{
    MU_ASSERT(t);
    if (!t)
        return false;
    MU_MEMSET(t, 0, sizeof(*t));
    t->free_slot = UINT32_MAX;
    t->cache_seg = UINT32_MAX;
    t->nlines    = 1u;

    size_t want = reserve_bytes ? reserve_bytes : ((size_t)1u << 30);
    if (want < ((size_t)1u << 20))
        want = (size_t)1u << 20;

    if (mu_vm_available() && mu_vm_arena_init(&t->arena, want, (size_t)4u << 20))
        return true;

    MU_MEMSET(&t->arena, 0, sizeof(t->arena));
    while (want >= ((size_t)1u << 20))
    {
        uint8_t* p = (uint8_t*)MU_MALLOC(want);
        if (p)
        {
            t->fb_base = p;
            t->fb_size = want;
            return true;
        }
        want /= 2u;
    }
    return false;
}

MU_INLINE void mu_ted_destroy(mu_ted* t)
{
    if (!t)
        return;
    for (uint32_t i = 0; i < t->nsegs; ++i)
        MU_FREE(t->seg[i]);
    MU_FREE(t->seg);
    MU_FREE(t->sb);
    MU_FREE(t->sl);
    MU_FREE(t->gb);
    MU_FREE(t->gl);
    MU_FREE(t->sub);
    MU_FREE(t->sul);
#if !MU_TED_NO_UNDO
    MU_FREE(t->ops);
    MU_FREE(t->pay);
#endif
    if (t->arena.region.base)
        mu_vm_arena_destroy(&t->arena);
    MU_FREE(t->fb_base);
    MU_MEMSET(t, 0, sizeof(*t));
}

MU_INLINE void mu_ted_clear(mu_ted* t)
{
    for (uint32_t i = 0; i < t->nsegs; ++i)
        MU_FREE(t->seg[i]);
    t->nsegs     = 0u;
    t->ndead     = 0u;
    t->slot_next = 0u;
    t->free_slot = UINT32_MAX;
    t->text_len  = 0u;
    t->nlines    = 1u;
    t->ngroups   = 0u;
    t->nsupers   = 0u;
    t->cache_seg = UINT32_MAX;
    if (t->arena.region.base)
        mu_vm_arena_reset(&t->arena);
#if !MU_TED_NO_UNDO
    t->nops         = 0u;
    t->pay_lo       = 0u;
    t->pay_hi       = 0u;
    t->undo_barrier = 0u;
    t->in_undo      = 0u;
#endif
}

/* ==========================================================================
 * Undo log: inverse ops, coalesced per direction
 * ========================================================================== */

#if !MU_TED_NO_UNDO

MU_INLINE bool mu_ted__pay_grow(mu_ted* t, uint32_t extra)
{
    uint32_t live, cap, need;
    uint8_t* p;

    /* Fast path: already room on BOTH sides, so no allocation is needed.
     * Without this, every coalesced keystroke re-malloc'd the arena and
     * copied the whole live payload, making a typing run O(n^2) in bytes
     * copied. Callers only ever append at pay_hi or prepend at pay_lo. */
    if (t->pay_lo >= extra && t->pay_cap - t->pay_hi >= extra)
        return true;

    /* Guard the doubling loops against u32 overflow on absurd requests. */
    if (extra > (UINT32_MAX / 4u) || t->pay_hi > (UINT32_MAX / 2u))
        return false;

    live = t->pay_hi - t->pay_lo;
    cap  = t->pay_cap ? t->pay_cap : 4096u;

    if (t->pay_lo >= extra)
    {
        /* Only the TOP is short. Extend in place: realloc preserves every
         * op->pay offset, and for large blocks glibc remaps pages instead of
         * copying, so growing a multi-MB undo arena costs no copy at all.
         * The size bound here is pay_hi + extra, NOT the recentring bound
         * below -- with a large pay_lo the recentring bound can be satisfied
         * while the top has no room left at all. */
        need = t->pay_hi + extra + 64u;
        while (cap < need)
            cap *= 2u;
        p = (uint8_t*)MU_REALLOC(t->pay, cap);
        if (!p)
            return false;
        t->pay     = p;
        t->pay_cap = cap;
        return true;
    }

    /* The BOTTOM is short, and payload grows downward, so the live range has
     * to move. 2*extra, not extra: after recentring, headroom on each side is
     * (cap-live)/2, so guaranteeing only live+extra+64 left LESS than `extra`
     * below pay_lo and an erase payload of len>64 underflowed it. */
    need = live + 2u * extra + 64u;
    while (cap < need)
        cap *= 2u;
    p = (uint8_t*)MU_MALLOC(cap);
    if (!p)
        return false;
    {
        const uint32_t nlo = (cap - live) / 2u;
        if (live)
            MU_MEMCPY(p + nlo, t->pay + t->pay_lo, live);
        for (uint32_t i = 0; i < t->nops; ++i)
            t->ops[i].pay = t->ops[i].pay - t->pay_lo + nlo;
        MU_FREE(t->pay);
        t->pay     = p;
        t->pay_cap = cap;
        t->pay_lo  = nlo;
        t->pay_hi  = nlo + live;
    }
    return true;
}

MU_INLINE bool mu_ted__op_push(mu_ted* t)
{
    if (t->nops < t->ops_cap)
        return true;
    const uint32_t cap = t->ops_cap ? t->ops_cap * 2u : 64u;
    mu_ted_op*     p   = (mu_ted_op*)MU_REALLOC(t->ops, (size_t)cap * sizeof(mu_ted_op));
    if (!p)
        return false;
    t->ops     = p;
    t->ops_cap = cap;
    return true;
}

MU_INLINE void mu_ted__log_insert(mu_ted* t, uint32_t off, uint32_t len, const char* d)
{
    if (len == 0u)
        return;

    /* Typing forward at the end of the previous insert: extend it in place. */
    if (t->nops > 0u && !t->undo_barrier)
    {
        mu_ted_op* top = &t->ops[t->nops - 1u];
        if (top->kind == MU_TED_OP_INSERT && top->off + top->len == off &&
            top->pay + top->len == t->pay_hi && mu_ted__pay_grow(t, len))
        {
            top = &t->ops[t->nops - 1u];
            MU_MEMCPY(t->pay + top->pay + top->len, d, len);
            t->pay_hi = top->pay + top->len + len;
            top->len += len;
            return;
        }
    }

    if (!mu_ted__op_push(t) || !mu_ted__pay_grow(t, len))
    {
        t->undo_barrier = 1u;
        return;
    }
    mu_ted_op* op = &t->ops[t->nops++];
    op->off       = off;
    op->len       = len;
    op->pay       = t->pay_hi;
    op->kind      = MU_TED_OP_INSERT;
    MU_MEMCPY(t->pay + t->pay_hi, d, len);
    t->pay_hi += len;
    t->undo_barrier = 0u;
}

MU_INLINE void mu_ted__log_erase(mu_ted* t, uint32_t off, uint32_t len, const char* d)
{
    if (len == 0u)
        return;

    if (t->nops > 0u && !t->undo_barrier)
    {
        mu_ted_op* top = &t->ops[t->nops - 1u];
        if (top->kind == MU_TED_OP_ERASE)
        {
            /* Holding Delete: same start, payload continues upward. */
            if (top->off == off && top->pay + top->len == t->pay_hi &&
                mu_ted__pay_grow(t, len))
            {
                top = &t->ops[t->nops - 1u];
                MU_MEMCPY(t->pay + top->pay + top->len, d, len);
                t->pay_hi = top->pay + top->len + len;
                top->len += len;
                return;
            }
            /* Backspace: new bytes precede the previous run, payload grows down. */
            if (off + len == top->off && mu_ted__pay_grow(t, len))
            {
                top = &t->ops[t->nops - 1u];
                t->pay_lo -= len;
                MU_MEMCPY(t->pay + t->pay_lo, d, len);
                top->pay = t->pay_lo;
                top->off = off;
                top->len += len;
                return;
            }
        }
    }

    if (!mu_ted__op_push(t) || !mu_ted__pay_grow(t, len))
    {
        t->undo_barrier = 1u;
        return;
    }
    t->pay_lo -= len;
    mu_ted_op* op = &t->ops[t->nops++];
    op->off       = off;
    op->len       = len;
    op->pay       = t->pay_lo;
    op->kind      = MU_TED_OP_ERASE;
    MU_MEMCPY(t->pay + t->pay_lo, d, len);
    t->undo_barrier = 0u;
}

#endif /* !MU_TED_NO_UNDO */

/* ==========================================================================
 * Core mutation
 * ========================================================================== */

MU_INLINE void mu_ted__invalidate(mu_ted* t) { t->cache_seg = UINT32_MAX; }

/* Insert a new segment header at logical index `at`, shifting the rest.
 * Prefix arrays stay valid because they are absolute: only the new entry and
 * the suffix need work. */
MU_INLINE bool mu_ted__seg_splice(mu_ted* t, uint32_t at, mu_ted_seg* s)
{
    if (!mu_ted__grow_segs(t, t->nsegs + 1u))
        return false;
    if (!mu_ted__grow_groups(t, (t->nsegs + 1u + MU_TED_GROUP_SEGS - 1u) / MU_TED_GROUP_SEGS))
        return false;

    MU_MEMMOVE(t->seg + at + 1u, t->seg + at, (size_t)(t->nsegs - at) * sizeof(mu_ted_seg*));
    MU_MEMMOVE(t->sb + at + 1u, t->sb + at, (size_t)(t->nsegs - at) * sizeof(uint32_t));
    MU_MEMMOVE(t->sl + at + 1u, t->sl + at, (size_t)(t->nsegs - at) * sizeof(uint32_t));
    t->seg[at] = s;
    t->sb[at]  = 0u;
    t->sl[at]  = 0u;
    t->nsegs++;
    /* Splits are rare (one per ~8 KiB typed); a full rebuild is a few hundred
     * loads and keeps the three levels trivially consistent. */
    mu_ted__rebuild_all(t);
    return true;
}

/* Split segment i near its middle (preferring a newline) and return the new
 * right-hand segment index, or UINT32_MAX. */
MU_INLINE uint32_t mu_ted__split(mu_ted* t, uint32_t i, uint32_t* out_cut)
{
    mu_ted_seg* s = t->seg[i];
    if (s->nbytes < 8u)
        return UINT32_MAX;

    const uint8_t* txt = mu_ted__seg_text(t, s);
    uint32_t       cut = s->nbytes / 2u;
    {
        const uint32_t k = mu_ted__find_nl((const char*)txt + cut, s->nbytes - cut);
        if (k < s->nbytes - cut)
            cut = cut + k + 1u;
        else
        {
            const uint32_t q = mu_ted__rfind_nl((const char*)txt, cut);
            if (q != UINT32_MAX)
                cut = q + 1u;
        }
    }
    if (cut == 0u || cut >= s->nbytes)
        cut = s->nbytes / 2u;
    if (out_cut)
        *out_cut = cut;

    const uint32_t slot = mu_ted__alloc_slot(t);
    if (slot == UINT32_MAX)
        return UINT32_MAX;

    const uint32_t jl    = mu_ted__last_le_u16(s->offs, s->nlines, (uint16_t)(cut - 1u));
    const uint32_t nkeep = (jl == UINT32_MAX) ? 0u : (jl + 1u);
    const uint32_t nmove = s->nlines - nkeep;

    mu_ted_seg* ns = mu_ted__seg_new(t, slot, nmove + 4u);
    if (!ns)
    {
        mu_ted__free_slot(t, slot);
        return UINT32_MAX;
    }

    MU_MEMCPY(mu_ted__seg_text(t, ns), txt + cut, s->nbytes - cut);
    ns->nbytes = s->nbytes - cut;
    ns->nlines = nmove;
    ns->cont   = 1u;
    for (uint32_t q = 0; q < nmove; ++q)
    {
        ns->offs[q] = (uint16_t)(s->offs[nkeep + q] - cut);
        if (q == 0u && ns->offs[0] == 0u)
            ns->cont = 0u;
    }

    s->nbytes = cut;
    s->nlines = nkeep;

    if (!mu_ted__seg_splice(t, i + 1u, ns))
    {
        mu_ted__seg_free(t, ns);
        return UINT32_MAX;
    }
    return i + 1u;
}

/* Drop tombstones once they reach 1/8 of the segment array. */
MU_INLINE void mu_ted__compact(mu_ted* t)
{
    if (t->ndead == 0u)
        return;
    uint32_t w = 0;
    for (uint32_t i = 0; i < t->nsegs; ++i)
    {
        mu_ted_seg* s = t->seg[i];
        if (s->dead)
        {
            mu_ted__seg_free(t, s);
            continue;
        }
        t->seg[w++] = s;
    }
    t->nsegs = w;
    t->ndead = 0u;
    mu_ted__rebuild_all(t);
}

/* Create the first segment of an empty document (one line start at 0). */
MU_INLINE bool mu_ted__first_seg(mu_ted* t)
{
    const uint32_t slot = mu_ted__alloc_slot(t);
    if (slot == UINT32_MAX)
        return false;
    mu_ted_seg* s = mu_ted__seg_new(t, slot, 8u);
    if (!s)
    {
        mu_ted__free_slot(t, slot);
        return false;
    }
    s->offs[0] = 0u;
    s->nlines  = 1u;
    if (!mu_ted__seg_splice(t, 0u, s))
    {
        mu_ted__seg_free(t, s);
        return false;
    }
    mu_ted__rebuild_all(t);
    return true;
}

MU_INLINE bool mu_ted_insert(mu_ted* t, uint32_t offset, const char* data, size_t len)
{
    MU_ASSERT(t);
    if (len == 0u)
        return true;
    if (offset > t->text_len)
        return false;
    if (len > (size_t)UINT32_MAX - (size_t)t->text_len)
        return false;
    MU_ASSERT(data || len == 0u);

#if !MU_TED_NO_UNDO
    if (!t->in_undo)
        mu_ted__log_insert(t, offset, (uint32_t)len, data);
#endif

    if (t->nsegs == 0u && !mu_ted__first_seg(t))
        return false;

#ifdef MU_TED_DEBUG
    mu_ted__check(t, "insert-begin");
#endif
    const char* d   = data;
    uint32_t    rem = (uint32_t)len;
    uint32_t    i, p;

    if (t->cache_seg != UINT32_MAX && offset >= t->cache_bstart &&
        offset <= t->cache_bstart + t->seg[t->cache_seg]->nbytes)
    {
        i = t->cache_seg;
        p = offset - t->cache_bstart;
    }
    else
    {
        i = mu_ted__seg_by_byte(t, offset);
        if (i == UINT32_MAX)
            return false;
        p = offset - mu_ted__bstart(t, i);
    }

    while (rem > 0u)
    {
        const uint32_t target = mu_ted__seg_target(t);
        uint32_t avail = (t->seg[i]->nbytes < target) ? (target - t->seg[i]->nbytes) : 0u;
        if (avail < rem && t->seg[i]->nbytes >= 8u)
        {
            uint32_t       cut = 0;
            const uint32_t ni  = mu_ted__split(t, i, &cut);
            if (ni == UINT32_MAX)
                return false;
            if (p > cut)
            {
                i = ni;
                p -= cut;
            }
            avail = (t->seg[i]->nbytes < target) ? (target - t->seg[i]->nbytes) : 0u;
            if (avail == 0u)
                avail = MU_TED_SEG_TEXT - t->seg[i]->nbytes; /* split refused: use the slot */
        }

        uint32_t take = MU_MIN(rem, avail);
        if (take < rem)
        {
            /* Cut at the last newline in the window so lines stay whole. */
            const uint32_t q = mu_ted__rfind_nl(d, take);
            if (q != UINT32_MAX)
                take = q + 1u;
        }

        uint32_t added = 0;
        if (!mu_ted__seg_insert(t, i, p, d, take, &added))
            return false;
        mu_ted__bump(t, i, take, added);
        mu_ted__trim_tail_start(t, i);
        mu_ted__fix_boundary(t, i);

        /* The insertion point follows the INSERTED bytes, not the end of the
         * segment. Those coincide only when the insert landed at the segment
         * tail; when the original tail survives after the cursor (any paste
         * that spans more than one chunk), snapping p to nbytes skipped that
         * tail and wrote the next chunk past it, corrupting the document.
         * Neither trim_tail_start nor fix_boundary moves bytes or changes
         * nbytes, so p + take is still the right local offset here. */
        if (take == 0u)
            return false; /* cannot progress; never observed, guards the loop */
        d += take;
        rem -= take;
        offset += take;
        p += take;

        t->cache_seg    = i;
        t->cache_bstart = mu_ted__bstart(t, i);
    }
#ifdef MU_TED_DEBUG
    mu_ted__check(t, "insert-end");
#endif
    return true;
}

MU_INLINE bool mu_ted_insert_str(mu_ted* t, uint32_t offset, const char* s)
{
    return mu_ted_insert(t, offset, s, s ? strlen(s) : 0u);
}

MU_INLINE bool mu_ted_erase(mu_ted* t, uint32_t offset, size_t len)
{
    MU_ASSERT(t);
    if (len == 0u)
        return true;
    if (offset >= t->text_len)
        return false;
    if (len > (size_t)(t->text_len - offset))
        len = t->text_len - offset;

#if !MU_TED_NO_UNDO
    if (!t->in_undo)
    {
        char* buf = (char*)MU_MALLOC(len);
        if (!buf)
            return false;
        uint32_t got = 0, o = offset, left = (uint32_t)len;
        while (left > 0u)
        {
            const uint32_t i = mu_ted__seg_by_byte(t, o);
            if (i == UINT32_MAX)
                break;
            const mu_ted_seg* s    = t->seg[i];
            const uint32_t    p    = o - mu_ted__bstart(t, i);
            const uint32_t    take = MU_MIN(left, s->nbytes - p);
            if (take == 0u)
                break;
            MU_MEMCPY(buf + got, mu_ted__seg_text(t, s) + p, take);
            got += take;
            o += take;
            left -= take;
        }
        mu_ted__log_erase(t, offset, got, buf);
        MU_FREE(buf);
    }
#endif

#ifdef MU_TED_DEBUG
    mu_ted__check(t, "erase-begin");
#endif
    uint32_t left = (uint32_t)len;
    uint32_t o    = offset;
    while (left > 0u)
    {
        const uint32_t i = mu_ted__seg_by_byte(t, o);
        if (i == UINT32_MAX)
            break;
        mu_ted_seg*    s    = t->seg[i];
        const uint32_t p    = o - mu_ted__bstart(t, i);
        const uint32_t take = MU_MIN(left, s->nbytes - p);
        if (take == 0u)
            break;
        const uint32_t before = s->nlines;

        mu_ted__seg_erase(t, i, p, take);
        s = t->seg[i];
#ifdef MU_TED_DEBUG
        MU_TED_DBG("  step seg_erase: seg nl=%u t->nlines=%u headersum=%u\n", s->nlines, t->nlines,
                   mu_ted__headersum(t));
#endif
        mu_ted__bump(t, i, (uint32_t)0u - take, s->nlines - before);
#ifdef MU_TED_DEBUG
        MU_TED_DBG("  step bump(%d): t->nlines=%u headersum=%u\n", (int)(before - s->nlines),
                   t->nlines, mu_ted__headersum(t));
#endif
        mu_ted__trim_tail_start(t, i);
#ifdef MU_TED_DEBUG
        MU_TED_DBG("  step trim: seg nl=%u t->nlines=%u\n", t->seg[i]->nlines, t->nlines);
#endif
        mu_ted__fix_boundary(t, i);
#ifdef MU_TED_DEBUG
        MU_TED_DBG("  step fixb: seg nl=%u t->nlines=%u headersum=%u\n", t->seg[i]->nlines, t->nlines,
                   mu_ted__headersum(t));
#endif

        /* 'o' is a valid offset in the document as it is NOW: the bytes after
         * the hole slid down by `take`, so the same offset points at the next
         * byte still to be erased. */
        left -= take;
#ifdef MU_TED_DEBUG
        {
            uint32_t sum = 0;
            for (uint32_t q = 0; q < t->nsegs; ++q)
                sum += t->seg[q]->dead ? 0u : t->seg[q]->nlines;
            if (sum != t->nlines)
            {
                MU_TED_DBG("erase chunk: seg i=%u p=%u take=%u before=%u now=%u sum=%u t->nlines=%u\n",
                           i, p, take, before, t->seg[i]->nlines, sum, t->nlines);
                for (uint32_t q = (i > 2u ? i - 2u : 0u); q < (i + 3u < t->nsegs ? i + 3u : t->nsegs); ++q)
                    MU_TED_DBG("   seg %u dead=%u nb=%u nl=%u cont=%u off0=%u\n", q, t->seg[q]->dead,
                               t->seg[q]->nbytes, t->seg[q]->nlines, t->seg[q]->cont,
                               t->seg[q]->nlines ? t->seg[q]->offs[0] : 0u);
            }
        }
#endif

        if (t->seg[i]->nbytes == 0u && t->nsegs > 1u)
        {
            mu_ted_seg* z = t->seg[i];
            if (z->nlines)
            {
                mu_ted__bump(t, i, 0u, (uint32_t)0u - z->nlines);
                z->nlines = 0u;
            }
            z->dead = 1u;
            t->ndead++;
            t->cache_seg = UINT32_MAX;
            if (t->ndead * 8u >= t->nsegs)
                mu_ted__compact(t);
        }
        else if (i + 1u < t->nsegs && !t->seg[i + 1u]->dead &&
                 t->seg[i]->nbytes + t->seg[i + 1u]->nbytes <= mu_ted__seg_target(t))
        {
            /* Merge the successor into i when the pair fits one slot. */
            mu_ted_seg* nx = t->seg[i + 1u];
            const uint32_t db = nx->nbytes, dl = nx->nlines;
            if (db && mu_ted__seg_reserve_lines(t, i, t->seg[i]->nlines + dl))
            {
                mu_ted_seg* s2 = t->seg[i];
                MU_MEMCPY(mu_ted__seg_text(t, s2) + s2->nbytes, mu_ted__seg_text(t, nx), db);
                for (uint32_t q = 0; q < dl; ++q)
                    s2->offs[s2->nlines + q] = (uint16_t)(nx->offs[q] + s2->nbytes);
                s2->nlines += dl;
                s2->nbytes += db;
                /* Merging is a storage reshuffle, not a document change: the
                 * totals stay put (the two shifts cancel), but the prefix
                 * levels must follow the data from i+1 into i. */
                mu_ted__bump(t, i, db, dl);
                mu_ted__bump(t, i + 1u, (uint32_t)0u - db, (uint32_t)0u - dl);
                nx->nbytes = 0u;
                nx->nlines = 0u;
                nx->dead   = 1u;
                t->ndead++;
                t->cache_seg = UINT32_MAX;
                if (t->ndead * 8u >= t->nsegs)
                    mu_ted__compact(t);
            }
        }
        else
            t->cache_seg = UINT32_MAX;
#ifdef MU_TED_DEBUG
        mu_ted__check(t, "erase-chunk");
#endif
    }

    /* Closing a hole can delete every segment between two survivors. The
     * boundary that matters is then between the last live segment before
     * `offset` and the first live one after it -- a pair that did not exist
     * while the loop ran, so no per-chunk fix_boundary covered it. */
    {
        uint32_t k = mu_ted__seg_by_byte(t, offset);
        if (k == UINT32_MAX)
            k = t->nsegs ? t->nsegs - 1u : 0u;
        while (k > 0u && (t->seg[k]->dead || t->seg[k]->nbytes == 0u ||
                          mu_ted__bstart(t, k) >= offset))
            --k;
        if (k < t->nsegs && !t->seg[k]->dead && t->seg[k]->nbytes)
            mu_ted__fix_boundary(t, k);
    }
    /* The hole may have swallowed the document's first or last segment; the
     * new endpoints need the starts that only an endpoint owns. */
    {
        uint32_t k = t->nsegs;
        while (k > 0u && t->seg[k - 1u]->dead)
            --k;
        if (k > 0u)
            mu_ted__restore_tail_start(t, k - 1u);
        k = 0u;
        while (k < t->nsegs && t->seg[k]->dead)
            ++k;
        if (k < t->nsegs)
            mu_ted__restore_head_start(t, k);
    }
    return true;
}

/* Replace [offset, offset+len) with `data`. */
MU_INLINE bool mu_ted_replace(mu_ted* t, uint32_t offset, size_t len, const char* data,
                              size_t n)
{
    if (!mu_ted_erase(t, offset, len))
        return false;
    return mu_ted_insert(t, offset, data, n);
}

/* ==========================================================================
 * Queries
 * ========================================================================== */

MU_INLINE uint32_t mu_ted_text_len(const mu_ted* t) { return t->text_len; }
MU_INLINE uint32_t mu_ted_line_count(const mu_ted* t) { return t->nlines; }
MU_INLINE uint32_t mu_ted_segment_count(const mu_ted* t) { return t->nsegs; }

MU_INLINE uint32_t mu_ted_line_offset(const mu_ted* t, uint32_t line)
{
    if (t->nsegs == 0u || line >= t->nlines)
        return t->text_len;
    uint32_t       local = 0;
    const uint32_t i     = mu_ted__seg_by_line(t, line, &local);
    if (i == UINT32_MAX)
        return 0u;
    const mu_ted_seg* s = t->seg[i];
    if (local >= s->nlines)
        return mu_ted__bstart(t, i) + s->nbytes;
    return mu_ted__bstart(t, i) + s->offs[local];
}

/* Resolve a line's start AND the next line's start in ONE index descent.
 * `line` and `line+1` share a segment except at segment boundaries, so the
 * two-call form paid a full second descent (super/group/seg binary searches
 * plus the dead-segment walk) to read a value already sitting in the same
 * offs[] array. Results are bit-identical to calling mu_ted_line_offset twice;
 * only the boundary case still falls back. */
MU_INLINE uint32_t mu_ted__line_locate(const mu_ted* t, uint32_t line,
                                       uint32_t* MU_RESTRICT off,
                                       uint32_t* MU_RESTRICT next)
{
    uint32_t          local = 0u;
    uint32_t          i;
    const mu_ted_seg* s;
    uint32_t          base;

    if (t->nsegs == 0u || line >= t->nlines)
    {
        *off  = t->text_len;
        *next = t->text_len;
        return UINT32_MAX;
    }
    i = mu_ted__seg_by_line(t, line, &local);
    if (i == UINT32_MAX)
    {
        *off  = 0u;
        *next = (line + 1u < t->nlines) ? mu_ted_line_offset(t, line + 1u) : t->text_len;
        return UINT32_MAX;
    }
    s    = t->seg[i];
    base = mu_ted__bstart(t, i);
    *off = base + ((local < s->nlines) ? (uint32_t)s->offs[local] : s->nbytes);

    if (line + 1u >= t->nlines)
        *next = t->text_len;
    else if (local + 1u < s->nlines)
        *next = base + (uint32_t)s->offs[local + 1u];
    else
        *next = mu_ted_line_offset(t, line + 1u);
    return i;
}

MU_INLINE void mu_ted__line_span(const mu_ted* t, uint32_t line,
                                 uint32_t* MU_RESTRICT off, uint32_t* MU_RESTRICT next)
{
    (void)mu_ted__line_locate(t, line, off, next);
}

MU_INLINE mu_ted_line mu_ted_line_range(const mu_ted* t, uint32_t line)
{
    mu_ted_line r;
    uint32_t    next;
    mu_ted__line_span(t, line, &r.offset, &next);
    if (line + 1u < t->nlines)
        r.len = next - r.offset - 1u;
    else
        r.len = t->text_len - r.offset;
    return r;
}

MU_INLINE mu_ted_cursor mu_ted_offset_to_cursor(const mu_ted* t, uint32_t offset)
{
    mu_ted_cursor c;
    c.offset = offset;
    c.line   = 0u;
    c.col    = 0u;
    if (t->nsegs == 0u || t->text_len == 0u)
        return c;
    if (offset > t->text_len)
        offset = t->text_len;

    uint32_t i, bs;
    if (t->cache_seg != UINT32_MAX && offset >= t->cache_bstart &&
        offset <= t->cache_bstart + t->seg[t->cache_seg]->nbytes)
    {
        i  = t->cache_seg;
        bs = t->cache_bstart;
    }
    else
    {
        i = mu_ted__seg_by_byte(t, offset);
        if (i == UINT32_MAX)
        {
            c.line = t->nlines - 1u;
            return c;
        }
        bs = mu_ted__bstart(t, i);
    }

    uint32_t p = offset - bs;
    /* A boundary byte belongs to the FOLLOWING segment (the one that starts a
     * line there); the cached segment must obey the same rule as the descent
     * or offset -> cursor -> offset would not round trip. */
    while (p == t->seg[i]->nbytes && i + 1u < t->nsegs)
    {
        uint32_t j = i + 1u;
        while (j < t->nsegs && t->seg[j]->dead)
            ++j;
        if (j >= t->nsegs)
            break;
        i  = j;
        p  = 0u;
        bs = mu_ted__bstart(t, j);
    }
    const mu_ted_seg* s = t->seg[i];
    if (p > s->nbytes)
        p = s->nbytes;

    const uint32_t ls = mu_ted__lstart(t, i);
    if (s->nlines == 0u)
    {
        /* Continuation bytes: the line began in an earlier segment, so the
         * column is measured from that line's start, not from this segment. */
        c.line = (ls > 0u) ? (ls - 1u) : 0u;
        c.col  = offset - mu_ted_line_offset(t, c.line);
        return c;
    }

    const uint32_t k = mu_ted__last_le_u16(s->offs, s->nlines, (uint16_t)p);
    if (k == UINT32_MAX)
    {
        c.line = (ls > 0u) ? (ls - 1u) : 0u;
        c.col  = offset - mu_ted_line_offset(t, c.line);
        return c;
    }
    c.line = ls + k;
    c.col  = p - s->offs[k];
    return c;
}

MU_INLINE uint32_t mu_ted_cursor_to_offset(const mu_ted* t, uint32_t line, uint32_t col)
{
    uint32_t base, next;
    mu_ted__line_span(t, line, &base, &next);
    if (line + 1u < t->nlines)
    {
        const uint32_t maxc = next - base - 1u;
        return base + MU_MIN(col, maxc);
    }
    return base + MU_MIN(col, t->text_len - base);
}

/* Fill up to k consecutive lines starting at `line`; returns count written. */
MU_INLINE uint32_t mu_ted_lines(const mu_ted* t, uint32_t line, uint32_t k, mu_ted_line* out)
{
    if (k == 0u || line >= t->nlines || t->nsegs == 0u)
        return 0u;
    if (line + k > t->nlines)
        k = t->nlines - line;

    uint32_t       local = 0;
    uint32_t       i     = mu_ted__seg_by_line(t, line, &local);
    if (i == UINT32_MAX)
        return 0u;

    uint32_t w = 0;
    while (w < k && i < t->nsegs)
    {
        if (t->seg[i]->dead)
        {
            ++i;
            local = 0u;
            continue;
        }
        const mu_ted_seg* s  = t->seg[i];
        const uint32_t    bs = mu_ted__bstart(t, i);
#if defined(__AVX512BW__) && !defined(MU_TED_NO_SIMD)
        /* 16 lines per iteration. Inside a segment every line is
         *   offset = bs + offs[j],  len = offs[j+1] - offs[j] - 1
         * so the whole viewport walk is two u16 loads, a widen, a add and a
         * subtract -- no per-line branch. The scalar tail below handles the
         * last line of the segment, which needs the next segment's start.
         * Requires offs[local+16] to be readable, hence local+16 < nlines. */
        {
            /* {offset,len} interleave over the 32-element concatenation
             * {OFF[0..15], LEN[0..15]}: want o0,l0,o1,l1,... in order.
             * unpacklo/hi + shuffle_i32x4 CANNOT express this (shuffle takes
             * lanes 0,1 from the first source and 2,3 from the second, giving
             * 0,1,4,5,2,3,...), so use two explicit vpermt2d. */
            static const uint32_t MU_TED_ILV0[16] =
                {0,16,1,17,2,18,3,19,4,20,5,21,6,22,7,23};
            static const uint32_t MU_TED_ILV1[16] =
                {8,24,9,25,10,26,11,27,12,28,13,29,14,30,15,31};
            const __m512i vbs  = _mm512_set1_epi32((int)bs);
            const __m512i one  = _mm512_set1_epi32(1);
            const __m512i ilv0 = _mm512_loadu_si512((const void*)MU_TED_ILV0);
            const __m512i ilv1 = _mm512_loadu_si512((const void*)MU_TED_ILV1);
            while (local + 16u < s->nlines && w + 16u <= k)
            {
                const __m256i o16 = _mm256_loadu_si256((const __m256i*)(const void*)(s->offs + local));
                const __m256i n16 = _mm256_loadu_si256((const __m256i*)(const void*)(s->offs + local + 1u));
                const __m512i O   = _mm512_cvtepu16_epi32(o16);
                const __m512i N   = _mm512_cvtepu16_epi32(n16);
                const __m512i OFF = _mm512_add_epi32(O, vbs);
                const __m512i LEN = _mm512_sub_epi32(_mm512_sub_epi32(N, O), one);
                const __m512i r0  = _mm512_permutex2var_epi32(OFF, ilv0, LEN);
                const __m512i r1  = _mm512_permutex2var_epi32(OFF, ilv1, LEN);
                _mm512_storeu_si512((void*)(out + w),      r0);
                _mm512_storeu_si512((void*)(out + w + 8u), r1);
                w += 16u;
                local += 16u;
            }
        }
#endif
        while (local < s->nlines && w < k)
        {
            out[w].offset = bs + s->offs[local];
            if (local + 1u < s->nlines)
                out[w].len = (uint32_t)s->offs[local + 1u] - s->offs[local] - 1u;
            else
            {
                uint32_t j = i + 1u;
                while (j < t->nsegs && t->seg[j]->nlines == 0u)
                    ++j;
                if (j < t->nsegs)
                    out[w].len = mu_ted__bstart(t, j) + t->seg[j]->offs[0] - out[w].offset - 1u;
                else
                    /* The DOCUMENT's last line has no trailing newline to
                     * exclude. Subtracting 1 unconditionally underflowed to
                     * UINT32_MAX for the final line of every file that ends
                     * with '\n' -- i.e. almost every file. mu_ted_line_range
                     * already special-cased this; the walk did not. */
                    out[w].len = t->text_len - out[w].offset;
            }
            ++w;
            ++local;
        }
        ++i;
        local = 0u;
    }
    return w;
}

/* Copy [off, off+len) out of the document; returns bytes copied. */
MU_INLINE uint32_t mu_ted_text(const mu_ted* t, uint32_t off, uint32_t len, char* dst)
{
    if (off >= t->text_len || t->nsegs == 0u)
        return 0u;
    if (len > t->text_len - off)
        len = t->text_len - off;
    /* Descend ONCE, then walk the directory forward. Segments are contiguous
     * in byte space, so after consuming segment i the next byte lives in the
     * next live segment -- re-descending the super/group/seg levels for every
     * chunk of a bulk read was O(chunks * log n) for an O(chunks) walk. */
    uint32_t got = 0;
    uint32_t i   = mu_ted__seg_by_byte(t, off);
    if (i == UINT32_MAX)
        return 0u;
    while (got < len && i < t->nsegs)
    {
        const mu_ted_seg* s = t->seg[i];
        const uint32_t    bs = mu_ted__bstart(t, i);
        uint32_t          p, take;
        if (s->dead || s->nbytes == 0u || bs + s->nbytes <= off + got)
        {
            ++i;
            continue;
        }
        p    = off + got - bs;
        take = MU_MIN(len - got, s->nbytes - p);
        if (take == 0u)
            break;
        MU_MEMCPY(dst + got, mu_ted__seg_text(t, s) + p, take);
        got += take;
        ++i;
    }
    return got;
}

/* One descent, not two. The old form called line_range (a line descent) and
 * then mu_ted_text, which re-descended by BYTE to find the very segment the
 * first descent had already located. Reading a line as text is the hottest
 * operation in a real editor, so the second descent was pure waste: the line
 * lies inside one segment except when it is long enough to span a boundary. */
MU_INLINE uint32_t mu_ted_line_text(const mu_ted* t, uint32_t line, char* dst, uint32_t cap)
{
    uint32_t          off, next, i, len, p, avail;
    const mu_ted_seg* s;

    i = mu_ted__line_locate(t, line, &off, &next);
    if (i == UINT32_MAX)
        return 0u;
    len = (line + 1u < t->nlines) ? (next - off - 1u) : (t->text_len - off);
    if (len > cap)
        len = cap;
    if (len == 0u)
        return 0u;

    s     = t->seg[i];
    p     = off - mu_ted__bstart(t, i);
    avail = s->nbytes - p;
    if (MU_LIKELY(len <= avail))
    {
        MU_MEMCPY(dst, mu_ted__seg_text(t, s) + p, len);
        return len;
    }
    /* Spans a segment boundary: take the head here, walk forward for the rest. */
    MU_MEMCPY(dst, mu_ted__seg_text(t, s) + p, avail);
    return avail + mu_ted_text(t, off + avail, len - avail, dst + avail);
}

/* ==========================================================================
 * Bulk load (build path is untouched by the edit machinery)
 * ========================================================================== */

MU_INLINE bool mu_ted_load(mu_ted* t, const char* text, size_t len)
{
    mu_ted_clear(t);
    if (len == 0u || !text)
        return true;
    if (len > (size_t)UINT32_MAX)
        return false;

    /* Plan the whole segment layout first so the arrays are grown once. */
    uint32_t nseg = (uint32_t)((len + MU_TED_SEG_TEXT - 1u) / MU_TED_SEG_TEXT);
    if (!mu_ted__grow_segs(t, nseg) || !mu_ted__grow_groups(t, (nseg + MU_TED_GROUP_SEGS - 1u) / MU_TED_GROUP_SEGS))
        return false;

    uint32_t pos   = 0u;
    bool     first = true;
    while (pos < (uint32_t)len)
    {
        uint32_t take = MU_MIN(MU_TED_SEG_TEXT, (uint32_t)len - pos);
        if (pos + take < (uint32_t)len)
        {
            const uint32_t q = mu_ted__rfind_nl(text + pos, take);
            if (q != UINT32_MAX)
                take = q + 1u;
        }

        /* The newline-aligned cut above shortens chunks, so the real segment
         * count can EXCEED the ceil(len/SEG_TEXT) estimate grown once above.
         * An 8 MiB load plans exactly 512 segments and then writes a 513th
         * past the end of the pointer array. Grow on demand; the pre-plan
         * still makes this a no-op for the overwhelming majority of chunks. */
        if (!mu_ted__grow_segs(t, t->nsegs + 1u) ||
            !mu_ted__grow_groups(t, (t->nsegs + 1u + MU_TED_GROUP_SEGS - 1u) / MU_TED_GROUP_SEGS))
            return false;

        const uint32_t slot = mu_ted__alloc_slot(t);
        if (slot == UINT32_MAX)
            return false;

        const uint32_t cont = (first || text[pos - 1u] == '\n') ? 0u : 1u;
        uint32_t       nl   = mu_ted__count_nl(text + pos, take) + (cont ? 0u : 1u);
        mu_ted_seg* s = mu_ted__seg_new(t, slot, nl + 4u);
        if (!s)
            return false;

        MU_MEMCPY(mu_ted__seg_text(t, s), text + pos, take);
        s->nbytes = take;
        s->cont   = cont;

        uint32_t w = 0;
        if (!cont)
            s->offs[w++] = 0u;
        {
            uint32_t o = 0;
            while (o < take)
            {
                const uint32_t q = mu_ted__find_nl(text + pos + o, take - o);
                if (q == take - o)
                    break;
                s->offs[w++] = (uint16_t)(o + q + 1u);
                o += q + 1u;
            }
        }
        if (pos + take < (uint32_t)len && w > 0u && (uint32_t)s->offs[w - 1u] == take)
            --w; /* continuation start lives in the next segment */
        s->nlines          = w;
        t->seg[t->nsegs++] = s;
        pos += take;
        first = false;
    }
    mu_ted__rebuild_all(t);
    return true;
}

/* Append is the common fast case: no descent, no split. */
MU_INLINE bool mu_ted_append(mu_ted* t, const char* data, size_t len)
{
    return mu_ted_insert(t, t->text_len, data, len);
}

/* ==========================================================================
 * Undo
 * ========================================================================== */

#if !MU_TED_NO_UNDO

MU_INLINE size_t mu_ted_undo_depth(const mu_ted* t) { return t->nops; }

/* Start a new undo group: the next mutation will not coalesce. */
MU_INLINE void mu_ted_checkpoint(mu_ted* t) { t->undo_barrier = 1u; }

MU_INLINE void mu_ted_undo_clear(mu_ted* t)
{
    t->nops         = 0u;
    t->pay_lo       = 0u;
    t->pay_hi       = 0u;
    t->undo_barrier = 0u;
}

MU_INLINE bool mu_ted_undo(mu_ted* t)
{
    if (t->nops == 0u)
        return false;
    const mu_ted_op op    = t->ops[--t->nops];
    const char*     p     = (const char*)t->pay + op.pay;

    t->in_undo = 1u;
    if (op.kind == MU_TED_OP_INSERT)
        mu_ted_erase(t, op.off, op.len);
    else
        mu_ted_insert(t, op.off, p, op.len);
    t->in_undo      = 0u;
    t->undo_barrier = 1u;

    if (t->nops == 0u)
        mu_ted_undo_clear(t);
    return true;
}

#endif /* !MU_TED_NO_UNDO */

/* Delete whole lines [first, first+n): the byte range including the trailing
 * terminator, so neighbours never end up sharing a line by accident. */
MU_INLINE bool mu_ted_delete_lines(mu_ted* t, uint32_t first, uint32_t n)
{
    if (n == 0u || first >= t->nlines)
        return true;
    if (first + n > t->nlines)
        n = t->nlines - first;
    const uint32_t off = mu_ted_line_offset(t, first);
    const uint32_t end = (first + n < t->nlines) ? mu_ted_line_offset(t, first + n)
                                                 : t->text_len;
    return mu_ted_erase(t, off, end - off);
}

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */

MU_INLINE size_t mu_ted_text_bytes(const mu_ted* t)
{
    return (size_t)t->slot_next * MU_TED_SEG_TEXT;
}

MU_INLINE size_t mu_ted_memory_usage(const mu_ted* t)
{
    size_t bytes = sizeof(mu_ted) + mu_ted_text_bytes(t);
    bytes += (size_t)t->seg_cap * (sizeof(mu_ted_seg*) + 2u * sizeof(uint32_t));
    bytes += (size_t)t->group_cap * 2u * sizeof(uint32_t);
    bytes += (size_t)t->super_cap * 2u * sizeof(uint32_t);
    for (uint32_t i = 0; i < t->nsegs; ++i)
        if (t->seg[i])
            bytes += sizeof(mu_ted_seg) + (size_t)t->seg[i]->cap * sizeof(uint16_t);
#if !MU_TED_NO_UNDO
    bytes += (size_t)t->ops_cap * sizeof(mu_ted_op) + t->pay_cap;
#endif
    return bytes;
}

/* Index/bookkeeping only: memory_usage() minus the reserved text slots. */
MU_INLINE size_t mu_ted_index_usage(const mu_ted* t)
{
    size_t bytes = sizeof(mu_ted) + mu_ted_text_bytes(t);
    return (mu_ted_memory_usage(t) >= bytes) ? (mu_ted_memory_usage(t) - bytes) : 0u;
}

/* Bytes of live text + index, i.e. without the undo log or slot slack. */
MU_INLINE size_t mu_ted_document_bytes(const mu_ted* t)
{
    size_t bytes = sizeof(mu_ted) + t->text_len;
    bytes += (size_t)t->seg_cap * (sizeof(mu_ted_seg*) + 2u * sizeof(uint32_t));
    bytes += (size_t)t->group_cap * 2u * sizeof(uint32_t);
    bytes += (size_t)t->super_cap * 2u * sizeof(uint32_t);
    for (uint32_t i = 0; i < t->nsegs; ++i)
        if (t->seg[i])
            bytes += sizeof(mu_ted_seg) + (size_t)t->seg[i]->cap * sizeof(uint16_t);
    return bytes;
}

/* Full invariant check. Returns false and prints the first failure. */
MU_INLINE bool mu_ted_validate(const mu_ted* t)
{
#define MU_TED_FAIL(msg)                                                                     \
    do                                                                                       \
    {                                                                                        \
        printf("mu_ted_validate: %s (nsegs=%u nlines=%u text_len=%u)\n", msg,                \
               (unsigned)t->nsegs, (unsigned)t->nlines, (unsigned)t->text_len);              \
        return false;                                                                        \
    } while (0)

    uint32_t bl = 0, ln = 0;

    for (uint32_t i = 0; i < t->nsegs; ++i)
    {
        const mu_ted_seg* s = t->seg[i];
        if (!s)
            MU_TED_FAIL("null segment");
        if (mu_ted__bstart(t, i) != bl)
            MU_TED_FAIL("L1 byte prefix mismatch");
        if (mu_ted__lstart(t, i) != ln)
            MU_TED_FAIL("L1 line prefix mismatch");
        if (s->dead)
        {
            if (s->nbytes || s->nlines)
                MU_TED_FAIL("tombstone carries data");
            continue;
        }
        if (s->nbytes > MU_TED_SEG_TEXT)
            MU_TED_FAIL("segment overflows its slot");
        if (s->nlines > s->cap)
            MU_TED_FAIL("line array overflow");

        const uint8_t* txt = mu_ted__seg_text(t, s);
        for (uint32_t k = 0; k < s->nlines; ++k)
        {
            if ((uint32_t)s->offs[k] >= (k ? s->nbytes + 1u : MU_TED_SEG_TEXT + 1u))
                MU_TED_FAIL("line start beyond segment");
            if (k && s->offs[k] <= s->offs[k - 1u])
                MU_TED_FAIL("line starts not ascending");
            if (s->offs[k] != 0u && (uint32_t)s->offs[k] > s->nbytes)
                MU_TED_FAIL("line start past end");
            if (s->offs[k] != 0u && txt[s->offs[k] - 1u] != '\n')
                MU_TED_FAIL("line start not preceded by newline");
        }
        if (i == 0u && (s->nlines == 0u || s->offs[0] != 0u))
            MU_TED_FAIL("document does not start at offset 0");
        if (s->nlines && s->offs[0] != 0u && s->cont == 0u)
            MU_TED_FAIL("continuation flag not set");
        if (s->nlines && s->offs[0] == 0u && s->cont != 0u)
            MU_TED_FAIL("continuation flag set on a line start");

        bl += s->nbytes;
        ln += s->nlines;
    }

    if (bl != t->text_len)
        MU_TED_FAIL("text_len mismatch");
    if ((ln ? ln : 1u) != t->nlines)
        MU_TED_FAIL("line count mismatch");

    for (uint32_t l = 0; l < t->nlines; ++l)
    {
        const mu_ted_line r = mu_ted_line_range(t, l);
        if (l && r.offset <= mu_ted_line_offset(t, l - 1u))
            MU_TED_FAIL("line offsets not ascending");
        if (r.offset + r.len > t->text_len)
            MU_TED_FAIL("line range past end");
    }

    for (uint32_t off = 0; off <= t->text_len; ++off)
    {
        const mu_ted_cursor c = mu_ted_offset_to_cursor(t, off);
        if (c.line >= t->nlines)
            MU_TED_FAIL("cursor line out of range");
        if (mu_ted_cursor_to_offset(t, c.line, c.col) != off)
        {
            printf("mu_ted_validate: round trip failed at off=%u -> (%u,%u) -> %u\n", off,
                   c.line, c.col, mu_ted_cursor_to_offset(t, c.line, c.col));
            return false;
        }
    }
#undef MU_TED_FAIL
    return true;
}

MU_END_EXTERN_C

#endif /* MU_TEXT_EDITOR_H */
