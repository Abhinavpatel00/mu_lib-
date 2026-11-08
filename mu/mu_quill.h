#pragma once
#ifndef MU_QUILL_H
#define MU_QUILL_H

/* ==========================================================================
 * mu_quill -- gapped-chunk text buffer
 * ==========================================================================
 *
 * Invented for the measured editing workload, not for a textbook. The five
 * real editing traces in rope-benches are small documents (18-102 KB) edited
 * one byte at a time, and the edits are overwhelmingly SEQUENTIAL:
 *
 *   trace                 edits at exactly the previous cursor   within 8 B
 *   automerge-paper                     80.0 %                     98.3 %
 *   seph-blog1                          89.0 %                     94.7 %
 *   sveltecomponent                     74.6 %                     84.6 %
 *   rustcode                            71.8 %                     81.2 %
 *   friendsforever_flat                 31.0 %                     62.7 %
 *
 * A contiguous-chunk rope (mu_ted) memmoves half a chunk on EVERY insert and
 * throws all of that locality away: 1344.6 MB moved to build a 102 KB file.
 * A single flat gap buffer (rune) exploits locality perfectly but pays O(n)
 * for a distant cursor jump and O(n) for line lookup, so it loses on large
 * documents and on line-heavy reads.
 *
 * mu_quill takes both: a two-level index of fixed-capacity chunks, where EACH
 * CHUNK HAS ITS OWN GAP.
 *
 *   - insert at the cursor           -> O(len), no memmove at all
 *   - backspace / forward-delete     -> O(len), no memmove at all
 *   - cursor jump inside a chunk     -> O(|distance|), bounded by CHUNK
 *   - cursor jump across chunks      -> O(log n) descent, no text moved
 *   - line lookup                    -> O(log n) descent + lazy local scan
 *
 * The gap is why sequential typing costs nothing. The index is why a distant
 * jump does not cost O(n). Neither a flat gap buffer nor a contiguous-chunk
 * rope has both.
 *
 * --------------------------------------------------------------------------
 * The line model, and why it cannot have mu_ted's boundary bugs
 * --------------------------------------------------------------------------
 * Seven of the eight bugs found in mu_ted were line-start OWNERSHIP bugs: a
 * start sat on a chunk boundary and two chunks disagreed about who held it.
 * mu_quill removes the ambiguity by construction:
 *
 *   A chunk owns the start at logical 0 only if it is chunk 0.
 *   A chunk owns a start at p+1 for EVERY '\n' at p -- including when
 *   p+1 == nbytes, i.e. the start of a line whose text lives in the next
 *   chunk. The successor never emits a start at 0.
 *
 * So starts are never transferred, trimmed, restored or repaired. Chunk i's
 * start count is exactly (i == 0) + nl_i, and the document total is
 * 1 + sum(nl) by inspection. There is no boundary invariant to violate.
 *
 * Per-chunk `nl` is maintained eagerly (one SIMD count per edit). The actual
 * start OFFSETS are derived on demand and cached, invalidated by a dirty
 * bitset -- AGENTS.md 10.2, build an index for hot repeated queries, not
 * speculatively. Editing never touches a start array.
 *
 * --------------------------------------------------------------------------
 * Layout notes (AGENTS.md)
 * --------------------------------------------------------------------------
 * - 3.1/3.2  chunks are slots in one arena; the index is parallel arrays.
 * - 6.2      no pointer chasing: descent is array indexing, three levels.
 * - 4.1      liveness is a dense array + a dirty BITSET, not per-row flags.
 * - 11.8     `cont` is DERIVED from the predecessor's last byte, never stored.
 * - 9.8      ZII + VM reserve; slot 0 is never a valid chunk id sentinel use
 *            UINT32_MAX instead, and an all-zero mu_quill is a valid empty doc
 *            after init.
 * - 2.5      bandwidth first: the hot path moves ZERO bytes.
 */

#include "mu_common.h"
#include "mu_vm.h"
#include "mu_vm_arena.h"

#if defined(__AVX512F__) || defined(__AVX512BW__) || defined(__AVX2__)
#  include <immintrin.h>
#endif

#ifdef MU_QUILL_DEBUG
#  include <stdio.h>
#  define MU_QUILL_DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#  define MU_QUILL_DBG(...) ((void)0)
#endif

/* Chunk capacity is decoupled from edit cost: an insert at the gap moves
 * nothing regardless of how big the chunk is. That is the opposite of a
 * contiguous-chunk rope, where chunk size directly sets the memmove volume
 * per keystroke. So size the chunk for READ locality instead. Swept
 * 4096/8192/16384: 16384 wins 4 of 5 editing traces and every random-access
 * read op; only friendsforever (the least local trace, 31 % at the previous
 * cursor) prefers 4096, by 10 %. */
#ifndef MU_QUILL_CHUNK
#define MU_QUILL_CHUNK 16384u
#endif
#ifndef MU_QUILL_GROUP
#define MU_QUILL_GROUP 32u
#endif
#ifndef MU_QUILL_SUPER
#define MU_QUILL_SUPER 32u
#endif

MU_STATIC_ASSERT(MU_QUILL_CHUNK <= 65535u, "u16 in-chunk line offsets");
MU_STATIC_ASSERT(MU_IS_POW2(MU_QUILL_GROUP), "group is a power of two");
MU_STATIC_ASSERT(MU_IS_POW2(MU_QUILL_SUPER), "super is a power of two");

/* ==========================================================================
 * Types
 * ========================================================================== */

typedef struct mu_quill_line
{
    uint32_t offset; /* absolute byte offset of the line start */
    uint32_t len;    /* bytes, EXCLUDING the '\n'              */
} mu_quill_line;

typedef struct mu_quill_chunk
{
    uint32_t slot;    /* arena slot index of buf[MU_QUILL_CHUNK]        */
    uint32_t nbytes;  /* live (logical) bytes; gap_len = CHUNK - nbytes */
    uint32_t gap;     /* physical == logical index of the gap start     */
    uint32_t nl;      /* '\n' count, maintained eagerly                 */
    uint32_t nstarts; /* entries in starts[]                            */
    uint32_t scap;    /* capacity of starts[]                           */
    uint16_t starts[];/* logical line-start offsets, built on demand    */
} mu_quill_chunk;

typedef struct mu_quill
{
    /* text slots */
    mu_vm_arena arena;
    uint8_t*    fb_base;
    size_t      fb_size;
    uint32_t    slot_next;
    uint32_t    free_slot;

    /* chunks + absolute-prefix index */
    mu_quill_chunk** ch;
    uint32_t         nch;
    uint32_t         ch_cap;
    uint32_t*        cb; /* bytes before chunk i       */
    uint32_t*        cl; /* line starts before chunk i */

    uint32_t  ngroups;
    uint32_t  group_cap;
    uint32_t* gb;
    uint32_t* gl;

    uint32_t  nsupers;
    uint32_t  super_cap;
    uint32_t* sb;
    uint32_t* sl;

    /* lazy line-start cache: one bit per chunk, set = needs rebuild */
    uint64_t* dirty;
    uint32_t  dirty_words;

    uint32_t text_len;
    uint32_t nlines; /* >= 1 */

    /* cursor cache: the hot path is a single compare against cur_abs */
    uint32_t cur_ch;
    uint32_t cur_base;
    uint32_t cur_abs;
} mu_quill;

/* ==========================================================================
 * Internals: storage
 * ========================================================================== */

MU_INLINE uint8_t* mu_quill__base(const mu_quill* q)
{
    return q->arena.region.base ? q->arena.region.base : q->fb_base;
}

MU_INLINE uint8_t* mu_quill__buf(const mu_quill* q, const mu_quill_chunk* c)
{
    return mu_quill__base(q) + (size_t)c->slot * MU_QUILL_CHUNK;
}

MU_INLINE uint32_t mu_quill__alloc_slot(mu_quill* q)
{
    if (q->free_slot != UINT32_MAX)
    {
        const uint32_t s = q->free_slot;
        q->free_slot = *(uint32_t*)(mu_quill__base(q) + (size_t)s * MU_QUILL_CHUNK);
        return s;
    }
    const size_t byte = (size_t)q->slot_next * MU_QUILL_CHUNK;
    if (q->arena.region.base)
    {
        if (byte + MU_QUILL_CHUNK > q->arena.region.reserved)
            return UINT32_MAX;
        if (byte + MU_QUILL_CHUNK > q->arena.region.committed)
        {
            size_t want = MU_ALIGN_UP(byte + MU_QUILL_CHUNK, q->arena.chunk);
            if (want > q->arena.region.reserved)
                want = q->arena.region.reserved;
            if (!mu_vm_commit(&q->arena.region, want))
                return UINT32_MAX;
        }
    }
    else if (byte + MU_QUILL_CHUNK > q->fb_size)
        return UINT32_MAX;

    q->slot_next++;
    return q->slot_next - 1u;
}

MU_INLINE void mu_quill__free_slot(mu_quill* q, uint32_t slot)
{
    *(uint32_t*)(mu_quill__base(q) + (size_t)slot * MU_QUILL_CHUNK) = q->free_slot;
    q->free_slot = slot;
}

/* ==========================================================================
 * Internals: newline scanning
 * ========================================================================== */

MU_INLINE uint32_t mu_quill__count_nl(const uint8_t* d, uint32_t n)
{
    uint32_t i = 0, k = 0;
#if defined(__AVX512BW__)
    const __m512i nl = _mm512_set1_epi8('\n');
    for (; i + 64u <= n; i += 64u)
    {
        const __m512i v = _mm512_loadu_si512((const void*)(d + i));
        k += (uint32_t)mu_popcount_u64((uint64_t)_mm512_cmpeq_epi8_mask(v, nl));
    }
#elif defined(__AVX2__)
    const __m256i nl = _mm256_set1_epi8('\n');
    for (; i + 32u <= n; i += 32u)
    {
        const __m256i v = _mm256_loadu_si256((const __m256i*)(d + i));
        const uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, nl));
        k += (uint32_t)mu_popcount_u64(m);
    }
#endif
    for (; i < n; ++i)
        k += (d[i] == '\n');
    return k;
}

/* first '\n' at or after 0, or n if none */
MU_INLINE uint32_t mu_quill__find_nl(const uint8_t* p, uint32_t n)
{
    uint32_t i = 0;
#if defined(__AVX512BW__)
    const __m512i nl = _mm512_set1_epi8('\n');
    for (; i + 64u <= n; i += 64u)
    {
        const __m512i v = _mm512_loadu_si512((const void*)(p + i));
        const uint64_t m = (uint64_t)_mm512_cmpeq_epi8_mask(v, nl);
        if (m)
            return i + (uint32_t)mu_trailing_zeroes_u64(m);
    }
#elif defined(__AVX2__)
    const __m256i nl = _mm256_set1_epi8('\n');
    for (; i + 32u <= n; i += 32u)
    {
        const __m256i v = _mm256_loadu_si256((const __m256i*)(p + i));
        const uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, nl));
        if (m)
            return i + (uint32_t)mu_trailing_zeroes_u64(m);
    }
#endif
    for (; i < n; ++i)
        if (p[i] == '\n')
            return i;
    return n;
}

/* Append the logical start offset after EVERY '\n' in b[0..n) to out[k..].
 * One pass, one 64-byte load per 64 bytes.
 *
 * The previous version called find_nl once per newline, which re-loaded a
 * 64-byte window at each hit. For prose that is roughly one pass, but for
 * dense newlines it is up to 64x the necessary traffic, and it paid a branch
 * per newline. Here the mask is drained with ctz + blsr: no re-scan and no
 * per-byte comparison branch. The drain loop still iterates once per hit. */
MU_INLINE uint32_t mu_quill__emit_nl(uint16_t* MU_RESTRICT out, uint32_t k,
                                     const uint8_t* b, uint32_t n, uint32_t logical_base)
{
    uint32_t i = 0;
#if !defined(MU_QUILL_NO_SIMD) && defined(__AVX512BW__)
    {
        const __m512i nl = _mm512_set1_epi8('\n');
        for (; i + 64u <= n; i += 64u)
        {
            const __m512i x = _mm512_loadu_si512((const void*)(b + i));
            uint64_t      m = (uint64_t)_mm512_cmpeq_epi8_mask(x, nl);
            while (m)
            {
                out[k++] = (uint16_t)(logical_base + i + (uint32_t)__builtin_ctzll(m) + 1u);
                m &= m - 1u;
            }
        }
    }
#elif !defined(MU_QUILL_NO_SIMD) && defined(__AVX2__)
    {
        const __m256i nl = _mm256_set1_epi8('\n');
        for (; i + 32u <= n; i += 32u)
        {
            const __m256i x = _mm256_loadu_si256((const __m256i*)(b + i));
            uint32_t      m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, nl));
            while (m)
            {
                out[k++] = (uint16_t)(logical_base + i + (uint32_t)__builtin_ctz(m) + 1u);
                m &= m - 1u;
            }
        }
    }
#endif
    for (; i < n; ++i)
        if (b[i] == '\n')
            out[k++] = (uint16_t)(logical_base + i + 1u);
    return k;
}

/* ==========================================================================
 * Internals: gap mechanics
 * ========================================================================== */

/* Short copies dominate read_line: a prose line is 64-96 bytes. libc memcpy
 * dispatches on size with several branches; after the short-length check, a
 * masked AVX-512 load/store pair copies without a size-dispatch call. Masked
 * lanes suppress faults, so reading up to 64 bytes near the end of a chunk
 * is safe. */
MU_INLINE void mu_quill__copy(uint8_t* MU_RESTRICT dst, const uint8_t* MU_RESTRICT src,
                              uint32_t n)
{
#if !defined(MU_QUILL_NO_SIMD) && defined(__AVX512BW__)
    if (n <= 64u)
    {
        const __mmask64 k = (n == 64u) ? (__mmask64)~(uint64_t)0
                                       : (((uint64_t)1u << n) - 1u);
        const __m512i   v = _mm512_mask_loadu_epi8(_mm512_setzero_si512(), k, (const void*)src);
        _mm512_mask_storeu_epi8((void*)dst, k, v);
        return;
    }
    if (n <= 128u)
    {
        const uint32_t  r = n - 64u;
        const __mmask64 k = (r == 64u) ? (__mmask64)~(uint64_t)0
                                       : (((uint64_t)1u << r) - 1u);
        _mm512_storeu_si512((void*)dst, _mm512_loadu_si512((const void*)src));
        _mm512_mask_storeu_epi8((void*)(dst + 64),
                                k, _mm512_mask_loadu_epi8(_mm512_setzero_si512(), k,
                                                          (const void*)(src + 64)));
        return;
    }
#endif
    MU_MEMCPY(dst, src, n);
}

/* Copy logical [a, a+n) of chunk c to dst. Two runs at most. */
MU_INLINE void mu_quill__read(const mu_quill* q, const mu_quill_chunk* c, uint32_t a, uint32_t n,
                              uint8_t* MU_RESTRICT dst)
{
    const uint8_t* b  = mu_quill__buf(q, c);
    const uint32_t gl = MU_QUILL_CHUNK - c->nbytes;
    if (a + n <= c->gap)
    {
        mu_quill__copy(dst, b + a, n);
        return;
    }
    if (a >= c->gap)
    {
        mu_quill__copy(dst, b + a + gl, n);
        return;
    }
    {
        const uint32_t head = c->gap - a;
        mu_quill__copy(dst, b + a, head);
        mu_quill__copy(dst + head, b + c->gap + gl, n - head);
    }
}

MU_INLINE uint8_t mu_quill__at(const mu_quill* q, const mu_quill_chunk* c, uint32_t p)
{
    const uint8_t* b = mu_quill__buf(q, c);
    return p < c->gap ? b[p] : b[p + (MU_QUILL_CHUNK - c->nbytes)];
}

/* Slide the gap to logical p. Cost O(|p - gap|), NOT O(nbytes - p). */
MU_INLINE void mu_quill__seek(mu_quill* q, mu_quill_chunk* c, uint32_t p)
{
    uint8_t*       b  = mu_quill__buf(q, c);
    const uint32_t gl = MU_QUILL_CHUNK - c->nbytes;
    if (p < c->gap)
        MU_MEMMOVE(b + p + gl, b + p, (size_t)(c->gap - p));
    else if (p > c->gap)
        MU_MEMMOVE(b + c->gap, b + c->gap + gl, (size_t)(p - c->gap));
    c->gap = p;
}

/* ==========================================================================
 * Internals: dirty bitset (existence, not per-chunk flags)
 * ========================================================================== */

MU_INLINE void mu_quill__mark(mu_quill* q, uint32_t i)
{
    q->dirty[i >> 6] |= (uint64_t)1u << (i & 63u);
}

MU_INLINE int mu_quill__is_dirty(const mu_quill* q, uint32_t i)
{
    return (q->dirty[i >> 6] >> (i & 63u)) & 1u;
}

MU_INLINE void mu_quill__mark_all(mu_quill* q)
{
    MU_MEMSET(q->dirty, 0xFF, (size_t)q->dirty_words * sizeof(uint64_t));
}

/* ==========================================================================
 * Internals: index
 * ========================================================================== */

MU_INLINE bool mu_quill__grow_ch(mu_quill* q, uint32_t need)
{
    uint32_t cap;
    if (need <= q->ch_cap)
        return true;
    cap = q->ch_cap ? q->ch_cap : 64u;
    while (cap < need)
        cap *= 2u;
    {
        mu_quill_chunk** nch = (mu_quill_chunk**)MU_REALLOC(q->ch, (size_t)cap * sizeof *nch);
        uint32_t*        nb;
        uint32_t*        nl;
        uint32_t         dw;
        uint64_t*        nd;
        if (!nch)
            return false;
        q->ch = nch;
        nb = (uint32_t*)MU_REALLOC(q->cb, (size_t)cap * sizeof *nb);
        if (!nb)
            return false;
        q->cb = nb;
        nl = (uint32_t*)MU_REALLOC(q->cl, (size_t)cap * sizeof *nl);
        if (!nl)
            return false;
        q->cl = nl;
        dw = (cap + 63u) / 64u;
        nd = (uint64_t*)MU_REALLOC(q->dirty, (size_t)dw * sizeof *nd);
        if (!nd)
            return false;
        MU_MEMSET(nd + q->dirty_words, 0xFF, (size_t)(dw - q->dirty_words) * sizeof(uint64_t));
        q->dirty       = nd;
        q->dirty_words = dw;
        q->ch_cap      = cap;
    }
    return true;
}

MU_INLINE bool mu_quill__grow_groups(mu_quill* q, uint32_t need)
{
    uint32_t cap;
    if (need <= q->group_cap)
        return true;
    cap = q->group_cap ? q->group_cap : 8u;
    while (cap < need)
        cap *= 2u;
    {
        uint32_t* nb = (uint32_t*)MU_REALLOC(q->gb, (size_t)cap * sizeof *nb);
        uint32_t* nl;
        if (!nb)
            return false;
        q->gb = nb;
        nl = (uint32_t*)MU_REALLOC(q->gl, (size_t)cap * sizeof *nl);
        if (!nl)
            return false;
        q->gl        = nl;
        q->group_cap = cap;
    }
    return true;
}

MU_INLINE bool mu_quill__grow_supers(mu_quill* q, uint32_t need)
{
    uint32_t cap;
    if (need <= q->super_cap)
        return true;
    cap = q->super_cap ? q->super_cap : 4u;
    while (cap < need)
        cap *= 2u;
    {
        uint32_t* nb = (uint32_t*)MU_REALLOC(q->sb, (size_t)cap * sizeof *nb);
        uint32_t* nl;
        if (!nb)
            return false;
        q->sb = nb;
        nl = (uint32_t*)MU_REALLOC(q->sl, (size_t)cap * sizeof *nl);
        if (!nl)
            return false;
        q->sl        = nl;
        q->super_cap = cap;
    }
    return true;
}

/* Parent-relative prefixes (the mu_ted convention, AGENTS.md 6.2):
 *   cb[i] = bytes before chunk i WITHIN its group
 *   gb[g] = bytes before group g WITHIN its super
 *   sb[s] = bytes before super s (absolute)
 * so bstart(i) = sb[i/(G*S)] + gb[i/G] + cb[i]. An edit then only has to bump
 * <= GROUP entries, <= SUPER entries and the super tail -- O(1) amortized,
 * not O(nch). The first version used absolute prefixes and patched every
 * k > i, which cost 8 KB of stores per keystroke on an 8 MiB document and
 * made type_burst 14x slower than mu_ted.
 *
 * cl[i] counts (i == 0 ? 1 : 0) + nl_i: chunk 0 owns the start at document
 * offset 0, every other chunk's first line was already started by a
 * predecessor's trailing '\n'. */
MU_INLINE uint32_t mu_quill__bstart(const mu_quill* q, uint32_t i)
{
    const uint32_t g = i / MU_QUILL_GROUP;
    return q->sb[g / MU_QUILL_SUPER] + q->gb[g] + q->cb[i];
}

MU_INLINE uint32_t mu_quill__lstart(const mu_quill* q, uint32_t i)
{
    const uint32_t g = i / MU_QUILL_GROUP;
    return q->sl[g / MU_QUILL_SUPER] + q->gl[g] + q->cl[i];
}

MU_INLINE void mu_quill__reindex(mu_quill* q)
{
    const uint32_t ng = (q->nch + MU_QUILL_GROUP - 1u) / MU_QUILL_GROUP;
    const uint32_t ns = (ng + MU_QUILL_SUPER - 1u) / MU_QUILL_SUPER;
    uint32_t       tb = 0, tl = 0, g, si;

    q->ngroups = ng;
    q->nsupers = ns;

    for (si = 0; si < ns; ++si)
    {
        const uint32_t gend = MU_MIN((si + 1u) * MU_QUILL_SUPER, ng);
        uint32_t       gb = 0, gl = 0;
        q->sb[si] = tb;
        q->sl[si] = tl;
        for (g = si * MU_QUILL_SUPER; g < gend; ++g)
        {
            const uint32_t iend = MU_MIN((g + 1u) * MU_QUILL_GROUP, q->nch);
            uint32_t       cb = 0, cl = 0, i;
            q->gb[g] = gb;
            q->gl[g] = gl;
            for (i = g * MU_QUILL_GROUP; i < iend; ++i)
            {
                const uint32_t nl = q->ch[i]->nl + (i == 0u ? 1u : 0u);
                q->cb[i] = cb;
                q->cl[i] = cl;
                cb += q->ch[i]->nbytes;
                cl += nl;
            }
            gb += cb;
            gl += cl;
        }
        tb += gb;
        tl += gl;
    }
    q->text_len = tb;
    q->nlines   = tl;
}

MU_INLINE void mu_quill__add_u32(uint32_t* a, uint32_t from, uint32_t to, uint32_t d)
{
    uint32_t i;
    if (d == 0u)
        return;
    for (i = from; i < to; ++i)
        a[i] += d;
}

MU_INLINE void mu_quill__bump(mu_quill* q, uint32_t i, uint32_t db, uint32_t dl)
{
    const uint32_t g    = i / MU_QUILL_GROUP;
    const uint32_t si   = g / MU_QUILL_SUPER;
    const uint32_t gend = MU_MIN((g + 1u) * MU_QUILL_GROUP, q->nch);
    const uint32_t send = MU_MIN((si + 1u) * MU_QUILL_SUPER, q->ngroups);

    mu_quill__add_u32(q->cb, i + 1u, gend, db);
    mu_quill__add_u32(q->cl, i + 1u, gend, dl);
    mu_quill__add_u32(q->gb, g + 1u, send, db);
    mu_quill__add_u32(q->gl, g + 1u, send, dl);
    mu_quill__add_u32(q->sb, si + 1u, q->nsupers, db);
    mu_quill__add_u32(q->sl, si + 1u, q->nsupers, dl);

    q->text_len += db;
    q->nlines   += dl;
}

MU_INLINE bool mu_quill__splice(mu_quill* q, uint32_t at, mu_quill_chunk* c)
{
    if (!mu_quill__grow_ch(q, q->nch + 1u))
        return false;
    if (!mu_quill__grow_groups(q, (q->nch + 1u + MU_QUILL_GROUP - 1u) / MU_QUILL_GROUP))
        return false;
    if (!mu_quill__grow_supers(q,
            ((q->nch + 1u + MU_QUILL_GROUP - 1u) / MU_QUILL_GROUP + MU_QUILL_SUPER - 1u)
            / MU_QUILL_SUPER))
        return false;

    MU_MEMMOVE(q->ch + at + 1u, q->ch + at, (size_t)(q->nch - at) * sizeof(mu_quill_chunk*));
    q->ch[at] = c;
    q->nch++;
    /* chunk indices shifted: invalidate every cached start array */
    mu_quill__mark_all(q);
    mu_quill__reindex(q);
    return true;
}

MU_INLINE void mu_quill__remove(mu_quill* q, uint32_t at)
{
    mu_quill_chunk* c = q->ch[at];
    mu_quill__free_slot(q, c->slot);
    MU_FREE(c);
    MU_MEMMOVE(q->ch + at, q->ch + at + 1u, (size_t)(q->nch - at - 1u) * sizeof(mu_quill_chunk*));
    q->nch--;
    mu_quill__mark_all(q);
    mu_quill__reindex(q);
}

MU_INLINE mu_quill_chunk* mu_quill__chunk_new(mu_quill* q, uint32_t scap)
{
    const uint32_t slot = mu_quill__alloc_slot(q);
    mu_quill_chunk* c;
    if (slot == UINT32_MAX)
        return NULL;
    c = (mu_quill_chunk*)MU_MALLOC(sizeof *c + (size_t)scap * sizeof(uint16_t));
    if (!c)
    {
        mu_quill__free_slot(q, slot);
        return NULL;
    }
    c->slot    = slot;
    c->nbytes  = 0;
    c->gap     = 0;
    c->nl      = 0;
    c->nstarts = 0;
    c->scap    = scap;
    return c;
}

/* ==========================================================================
 * Internals: descent
 * ========================================================================== */

/* Last index with a[i] <= v, over an ascending array.
 *
 * GROUP and SUPER are both 32, so every level of the descent searches at most
 * 32 u32 = two AVX-512 vectors. Do it in ONE shot: two masked loads, two
 * unsigned compares, combine the masks, 31 - clz. The scalar binary search
 * this replaces cost five DEPENDENT loads per level, three levels per
 * lookup -- 86 ns of the 126 ns warm read_line. mu_ted already did this
 * (mu_ted__last_le_u32); mu_quill's first version did not.
 *
 * maskz loads mean n < 16 (and n == 0) never read out of bounds. */
MU_INLINE uint32_t mu_quill__last_le(const uint32_t* a, uint32_t n, uint32_t v)
{
#if defined(MU_QUILL_NO_SIMD)
    uint32_t lo = 0, hi = n;
    if (n == 0u || a[0] > v)
        return UINT32_MAX;
    while (lo + 1u < hi)
    {
        const uint32_t mid = (lo + hi) >> 1;
        if (a[mid] <= v) lo = mid; else hi = mid;
    }
    return lo;
#elif defined(__AVX512F__)
    const __m512i vv = _mm512_set1_epi32((int)v);
    if (n <= 16u)
    {
        const __mmask16 k = (__mmask16)((n == 16u) ? 0xFFFFu : ((1u << n) - 1u));
        const __m512i   x = _mm512_maskz_loadu_epi32(k, (const void*)a);
        const __mmask16 m = _mm512_mask_cmple_epu32_mask(k, x, vv);
        return m ? (31u - (uint32_t)__builtin_clz((unsigned)m)) : UINT32_MAX;
    }
    if (n <= 32u)
    {
        const uint32_t  nh = n - 16u;
        const __mmask16 k2 = (__mmask16)((nh == 16u) ? 0xFFFFu : ((1u << nh) - 1u));
        const __m512i   lo = _mm512_loadu_si512((const void*)a);
        const __m512i   hi = _mm512_maskz_loadu_epi32(k2, (const void*)(a + 16));
        const uint32_t  m  = (uint32_t)_mm512_cmple_epu32_mask(lo, vv) |
                            ((uint32_t)_mm512_mask_cmple_epu32_mask(k2, hi, vv) << 16);
        return m ? (31u - (uint32_t)__builtin_clz(m)) : UINT32_MAX;
    }
    {
        /* Rare: only the super level can exceed 32, and only past 1 GiB of
         * text. Walk backwards in 16-wide blocks, as mu_ted does. */
        uint32_t i = n;
        while (i >= 16u)
        {
            const __m512i   x = _mm512_loadu_si512((const void*)(a + (i - 16u)));
            const __mmask16 m = _mm512_cmple_epu32_mask(x, vv);
            if (m)
                return (i - 16u) + 31u - (uint32_t)__builtin_clz((unsigned)m);
            i -= 16u;
        }
        {
            uint32_t best = UINT32_MAX, k;
            for (k = 0; k < i; ++k)
                if (a[k] <= v)
                    best = k;
            return best;
        }
    }
#else
    uint32_t lo = 0, hi = n;
    if (n == 0u || a[0] > v)
        return UINT32_MAX;
    while (lo + 1u < hi)
    {
        const uint32_t mid = (lo + hi) >> 1;
        if (a[mid] <= v) lo = mid; else hi = mid;
    }
    return lo;
#endif
}

MU_INLINE uint32_t mu_quill__by_byte(const mu_quill* q, uint32_t off)
{
    uint32_t si = 0, g, i, k;
    if (q->nch == 0u)
        return UINT32_MAX;
    if (q->nsupers > 1u)
    {
        k = mu_quill__last_le(q->sb, q->nsupers, off);
        if (k != UINT32_MAX) si = k;
    }
    {
        const uint32_t gbase = si * MU_QUILL_SUPER;
        const uint32_t gend  = MU_MIN(gbase + MU_QUILL_SUPER, q->ngroups);
        g = gbase;
        if (gend > gbase + 1u)
        {
            k = mu_quill__last_le(q->gb + gbase, gend - gbase, off - q->sb[si]);
            if (k != UINT32_MAX) g = gbase + k;
        }
    }
    {
        const uint32_t ibase = g * MU_QUILL_GROUP;
        const uint32_t iend  = MU_MIN(ibase + MU_QUILL_GROUP, q->nch);
        const uint32_t rel   = off - q->sb[si] - q->gb[g];
        i = ibase;
        k = mu_quill__last_le(q->cb + ibase, iend - ibase, rel);
        if (k != UINT32_MAX) i = ibase + k;
    }
    /* normalise over empty chunks and exact-boundary hits */
    while (i > 0u && mu_quill__bstart(q, i) > off)
        --i;
    while (i + 1u < q->nch && mu_quill__bstart(q, i) + q->ch[i]->nbytes <= off)
        ++i;
    return i;
}

MU_INLINE uint32_t mu_quill__by_line(const mu_quill* q, uint32_t line)
{
    uint32_t si = 0, g, i, k;
    if (q->nch == 0u)
        return UINT32_MAX;
    if (q->nsupers > 1u)
    {
        k = mu_quill__last_le(q->sl, q->nsupers, line);
        if (k != UINT32_MAX) si = k;
    }
    {
        const uint32_t gbase = si * MU_QUILL_SUPER;
        const uint32_t gend  = MU_MIN(gbase + MU_QUILL_SUPER, q->ngroups);
        g = gbase;
        if (gend > gbase + 1u)
        {
            k = mu_quill__last_le(q->gl + gbase, gend - gbase, line - q->sl[si]);
            if (k != UINT32_MAX) g = gbase + k;
        }
    }
    {
        const uint32_t ibase = g * MU_QUILL_GROUP;
        const uint32_t iend  = MU_MIN(ibase + MU_QUILL_GROUP, q->nch);
        const uint32_t rel   = line - q->sl[si] - q->gl[g];
        i = ibase;
        k = mu_quill__last_le(q->cl + ibase, iend - ibase, rel);
        if (k != UINT32_MAX) i = ibase + k;
    }
    while (i > 0u && mu_quill__lstart(q, i) > line)
        --i;
    while (i + 1u < q->nch &&
           mu_quill__lstart(q, i) + q->ch[i]->nl + (i == 0u ? 1u : 0u) <= line)
        ++i;
    return i;
}

/* Build chunk i's logical line starts. See the ownership rule at the top:
 * chunk 0 emits 0, every chunk emits p+1 for each '\n' at p, nobody else
 * emits anything. No trimming, no boundary repair. */
MU_INLINE void mu_quill__starts(mu_quill* q, uint32_t i)
{
    mu_quill_chunk* c = q->ch[i];
    const uint32_t  need = c->nl + (i == 0u ? 1u : 0u);
    uint32_t        k = 0;

    if (!mu_quill__is_dirty(q, i) && c->nstarts == need)
        return;

    if (c->scap < need)
    {
        uint32_t cap = c->scap ? c->scap : 16u;
        mu_quill_chunk* nc;
        while (cap < need)
            cap *= 2u;
        nc = (mu_quill_chunk*)MU_REALLOC(c, sizeof *nc + (size_t)cap * sizeof(uint16_t));
        if (!nc)
            return;
        nc->scap = cap;
        q->ch[i] = nc;
        c = nc;
    }

    /* Scan the two physical runs directly. Staging the whole chunk through a
     * 4 KB stack buffer cost a memcpy per rebuild for no benefit. */
    {
        const uint8_t* b  = mu_quill__buf(q, c);
        const uint32_t gl = MU_QUILL_CHUNK - c->nbytes;
        const uint8_t* r2 = b + c->gap + gl;
        const uint32_t n2 = c->nbytes - c->gap;
        if (i == 0u)
            c->starts[k++] = 0u;
        k = mu_quill__emit_nl(c->starts, k, b,  c->gap, 0u);
        k = mu_quill__emit_nl(c->starts, k, r2, n2,     c->gap);
    }
    c->nstarts = k;
    q->dirty[i >> 6] &= ~((uint64_t)1u << (i & 63u));
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

MU_INLINE bool mu_quill_init(mu_quill* q, size_t reserve_bytes)
{
    MU_ASSERT(q);
    if (!q)
        return false;
    MU_MEMSET(q, 0, sizeof *q);
    q->free_slot = UINT32_MAX;
    q->cur_ch    = UINT32_MAX;
    q->nlines    = 1u;

    {
        size_t want = reserve_bytes ? reserve_bytes : ((size_t)1u << 30);
        if (want < ((size_t)1u << 20))
            want = (size_t)1u << 20;
        if (!(mu_vm_available() && mu_vm_arena_init(&q->arena, want, (size_t)4u << 20)))
        {
            MU_MEMSET(&q->arena, 0, sizeof q->arena);
            while (want >= ((size_t)1u << 20))
            {
                uint8_t* p = (uint8_t*)MU_MALLOC(want);
                if (p)
                {
                    q->fb_base = p;
                    q->fb_size = want;
                    break;
                }
                want /= 2u;
            }
            if (!q->fb_base)
                return false;
        }
    }

    if (!mu_quill__grow_ch(q, 64u) || !mu_quill__grow_groups(q, 8u) ||
        !mu_quill__grow_supers(q, 4u))
        return false;

    {
        mu_quill_chunk* c = mu_quill__chunk_new(q, 16u);
        if (!c)
            return false;
        q->ch[0] = c;
        q->nch   = 1u;
    }
    mu_quill__mark_all(q);
    mu_quill__reindex(q);
    q->cur_ch   = 0;
    q->cur_base = 0;
    q->cur_abs  = 0;
    return true;
}

MU_INLINE void mu_quill_destroy(mu_quill* q)
{
    uint32_t i;
    if (!q)
        return;
    for (i = 0; i < q->nch; ++i)
        MU_FREE(q->ch[i]);
    MU_FREE(q->ch);
    MU_FREE(q->cb);
    MU_FREE(q->cl);
    MU_FREE(q->gb);
    MU_FREE(q->gl);
    MU_FREE(q->sb);
    MU_FREE(q->sl);
    MU_FREE(q->dirty);
    if (q->arena.region.base)
        mu_vm_arena_destroy(&q->arena);
    MU_FREE(q->fb_base);
    MU_MEMSET(q, 0, sizeof *q);
}

MU_INLINE uint32_t mu_quill_text_len(const mu_quill* q) { return q->text_len; }
MU_INLINE uint32_t mu_quill_line_count(const mu_quill* q) { return q->nlines; }

/* ==========================================================================
 * Insert
 * ========================================================================== */

/* Split chunk i near its middle, preferring a newline. The right piece gets
 * its gap at logical 0 so an insert continuing there is free. */
MU_INLINE uint32_t mu_quill__split(mu_quill* q, uint32_t i)
{
    mu_quill_chunk* c = q->ch[i];
    uint8_t         tmp[MU_QUILL_CHUNK];
    uint32_t        cut, right_n, slot;
    mu_quill_chunk* nc;

    if (c->nbytes < 8u)
        return UINT32_MAX;
    mu_quill__read(q, c, 0, c->nbytes, tmp);
    cut = c->nbytes / 2u;
    {
        const uint32_t k = mu_quill__find_nl(tmp + cut, c->nbytes - cut);
        if (k < c->nbytes - cut)
            cut += k + 1u;
    }
    if (cut == 0u || cut >= c->nbytes)
        cut = c->nbytes / 2u;

    right_n = c->nbytes - cut;
    nc = mu_quill__chunk_new(q, 16u);
    if (!nc)
        return UINT32_MAX;
    slot = nc->slot;
    {
        uint8_t* nb = mu_quill__base(q) + (size_t)slot * MU_QUILL_CHUNK;
        MU_MEMCPY(nb + MU_QUILL_CHUNK - right_n, tmp + cut, right_n);
    }
    nc->nbytes = right_n;
    nc->gap    = 0u;
    nc->nl     = mu_quill__count_nl(tmp + cut, right_n);

    c->nbytes = cut;
    c->gap    = cut;
    c->nl     = mu_quill__count_nl(tmp, cut);

    if (!mu_quill__splice(q, i + 1u, nc))
    {
        mu_quill__free_slot(q, nc->slot);
        MU_FREE(nc);
        return UINT32_MAX;
    }
    return i + 1u;
}

MU_INLINE bool mu_quill_insert(mu_quill* q, uint32_t offset, const char* data, size_t len)
{
    const uint8_t* d = (const uint8_t*)data;
    uint32_t       rem, i, p;

    MU_ASSERT(q);
    if (len == 0u)
        return true;
    if (offset > q->text_len)
        return false;
    if (len > (size_t)UINT32_MAX - (size_t)q->text_len)
        return false;

    rem = (uint32_t)len;
    while (rem)
    {
        uint32_t take, room, nl;

        /* HOT PATH: the cursor is exactly where the gap already is. 71-89 %
         * of real edits land here. One compare, no descent, no memmove. */
        i = q->cur_ch;
        if (offset != q->cur_abs || i >= q->nch || offset < mu_quill__bstart(q, i) ||
            offset - mu_quill__bstart(q, i) > q->ch[i]->nbytes)
        {
            i = mu_quill__by_byte(q, offset);
            if (i == UINT32_MAX)
                return false;
        }
        /* An insert at a chunk's end may stay in that chunk so its gap can
         * absorb it -- but only while the chunk is FULL do we have to move
         * on. The cache is revalidated above because a previous edit may
         * have shrunk this chunk until the cursor sits at its boundary. */
        while (i + 1u < q->nch && offset == mu_quill__bstart(q, i) + q->ch[i]->nbytes &&
               q->ch[i]->nbytes == MU_QUILL_CHUNK)
            ++i;
        p = offset - mu_quill__bstart(q, i);

        room = MU_QUILL_CHUNK - q->ch[i]->nbytes;
        if (room == 0u)
        {
            const uint32_t ni = mu_quill__split(q, i);
            if (ni == UINT32_MAX)
                return false;
            if (p >= q->ch[i]->nbytes)
            {
                p -= q->ch[i]->nbytes;
                i = ni;
            }
            room = MU_QUILL_CHUNK - q->ch[i]->nbytes;
        }

        take = MU_MIN(rem, room);
        if (q->ch[i]->gap != p)
            mu_quill__seek(q, q->ch[i], p);

        {
            mu_quill_chunk* c  = q->ch[i];
            uint8_t*        b  = mu_quill__buf(q, c);
            MU_MEMCPY(b + c->gap, d, take);
            nl = mu_quill__count_nl(d, take);
            c->gap    += take;
            c->nbytes += take;
            c->nl     += nl;
        }
        mu_quill__mark(q, i);

        mu_quill__bump(q, i, take, nl);

        d      += take;
        rem    -= take;
        offset += take;

        q->cur_ch   = i;
        q->cur_base = mu_quill__bstart(q, i);
        q->cur_abs  = offset;
    }
    return true;
}

MU_INLINE bool mu_quill_insert_str(mu_quill* q, uint32_t offset, const char* s)
{
    return mu_quill_insert(q, offset, s, strlen(s));
}

/* ==========================================================================
 * Erase
 * ========================================================================== */

MU_INLINE bool mu_quill_erase(mu_quill* q, uint32_t offset, size_t len)
{
    uint32_t rem;

    MU_ASSERT(q);
    if (len == 0u)
        return true;
    if (offset > q->text_len || len > (size_t)(q->text_len - offset))
        return false;

    rem = (uint32_t)len;
    while (rem)
    {
        uint32_t        i, p, take, nl;
        mu_quill_chunk* c;

        i = q->cur_ch;
        if (offset != q->cur_abs || i >= q->nch || offset < mu_quill__bstart(q, i) ||
            offset - mu_quill__bstart(q, i) > q->ch[i]->nbytes)
        {
            i = mu_quill__by_byte(q, offset);
            if (i == UINT32_MAX)
                return false;
        }
        /* Erase must advance past any chunk that ends at the cursor: there
         * is nothing to delete at a chunk's own end. This runs for the
         * cached case too -- after deleting to the end of a chunk the cache
         * still names it, and skipping this walk read p == nbytes. */
        while (i + 1u < q->nch && offset == mu_quill__bstart(q, i) + q->ch[i]->nbytes)
            ++i;
        if (i >= q->nch)
        {
            MU_QUILL_DBG("erase: i=%u >= nch=%u off=%u len=%u\n", i, q->nch, offset, rem);
            return false;
        }
        c = q->ch[i];
        if (c->nbytes == 0u)
        {
            /* empty chunk: drop it and retry at the same offset */
            mu_quill__remove(q, i);
            q->cur_ch = UINT32_MAX;
            if (q->nch == 0u)
                return false;
            continue;
        }
        p    = offset - mu_quill__bstart(q, i);
        take = MU_MIN(rem, c->nbytes - p);
        if (take == 0u)
        {
            uint32_t z;
            MU_QUILL_DBG("erase take=0: i=%u p=%u nbytes=%u cb=%u off=%u rem=%u text_len=%u nch=%u\n",
                         i, p, c->nbytes, mu_quill__bstart(q, i), offset, rem, q->text_len, q->nch);
            for (z = 0; z < q->nch; ++z)
                MU_QUILL_DBG("   ch%u cb=%u nbytes=%u gap=%u nl=%u cl=%u\n",
                             z, mu_quill__bstart(q, z), q->ch[z]->nbytes, q->ch[z]->gap, q->ch[z]->nl, mu_quill__lstart(q, z));
            return false;
        }

        /* Count newlines in the doomed range, then make it gap. Deleting
         * immediately before or after the gap needs NO memmove at all --
         * that is backspace and forward-delete, the common cases. */
        {
            uint8_t        tmp[MU_QUILL_CHUNK];
            const uint32_t gl = MU_QUILL_CHUNK - c->nbytes;
            const uint8_t* b  = mu_quill__buf(q, c);
            if (p + take <= c->gap)
                nl = mu_quill__count_nl(b + p, take);
            else if (p >= c->gap)
                nl = mu_quill__count_nl(b + p + gl, take);
            else
            {
                mu_quill__read(q, c, p, take, tmp);
                nl = mu_quill__count_nl(tmp, take);
            }
        }

        if (p + take == c->gap)
            c->gap -= take;                 /* backspace: shrink gap leftward */
        else if (p == c->gap)
            ;                               /* forward-delete: gap grows right */
        else
        {
            mu_quill__seek(q, c, p);
        }
        c->nbytes -= take;
        c->nl     -= nl;
        mu_quill__mark(q, i);

        mu_quill__bump(q, i, 0u - take, 0u - nl);

        rem -= take;
        q->cur_ch   = i;
        q->cur_base = mu_quill__bstart(q, i);
        q->cur_abs  = offset;

        /* Coalesce a nearly-empty pair so fragmentation stays bounded. */
        if (i + 1u < q->nch &&
            c->nbytes + q->ch[i + 1u]->nbytes <= (MU_QUILL_CHUNK * 3u) / 4u)
        {
            mu_quill_chunk* n = q->ch[i + 1u];
            uint8_t         tmp[MU_QUILL_CHUNK];
            uint8_t*        b = mu_quill__buf(q, c);
            mu_quill__read(q, n, 0, n->nbytes, tmp);
            /* append n's text at the end of c, then drop n */
            mu_quill__seek(q, c, c->nbytes);
            MU_MEMCPY(b + c->gap, tmp, n->nbytes);
            c->gap    += n->nbytes;
            c->nbytes += n->nbytes;
            c->nl     += n->nl;
            mu_quill__remove(q, i + 1u);
            q->cur_ch  = UINT32_MAX;
            q->cur_abs = offset;
        }
    }
    return true;
}

/* ==========================================================================
 * Reads
 * ========================================================================== */

MU_INLINE uint32_t mu_quill_text(const mu_quill* q, uint32_t off, uint32_t len,
                                 char* MU_RESTRICT dst)
{
    uint32_t done = 0, i;
    if (off > q->text_len)
        return 0;
    len = MU_MIN(len, q->text_len - off);
    if (len == 0u)
        return 0;
    i = mu_quill__by_byte(q, off);
    while (done < len && i < q->nch)
    {
        const mu_quill_chunk* c = q->ch[i];
        const uint32_t p = off + done - mu_quill__bstart(q, i);
        const uint32_t n = MU_MIN(len - done, c->nbytes - p);
        if (n)
            mu_quill__read(q, c, p, n, (uint8_t*)dst + done);
        done += n;
        ++i;
    }
    return done;
}

MU_INLINE uint32_t mu_quill_line_offset(mu_quill* q, uint32_t line)
{
    uint32_t i;
    if (line >= q->nlines)
        return q->text_len;
    i = mu_quill__by_line(q, line);
    if (i == UINT32_MAX)
        return q->text_len;
    mu_quill__starts(q, i);
    {
        const uint32_t k = line - mu_quill__lstart(q, i);
        if (k >= q->ch[i]->nstarts)
            return q->text_len;
        return mu_quill__bstart(q, i) + q->ch[i]->starts[k];
    }
}

/* One descent, not two: the next start is almost always in the same chunk,
 * so read it straight out of starts[] instead of searching again. */
MU_INLINE mu_quill_line mu_quill_line_at(mu_quill* q, uint32_t line)
{
    mu_quill_line   r;
    uint32_t        i, k, base;
    mu_quill_chunk* c;

    r.offset = q->text_len;
    r.len    = 0u;
    if (line >= q->nlines)
        return r;
    i = mu_quill__by_line(q, line);
    if (i == UINT32_MAX)
        return r;
    mu_quill__starts(q, i);
    c    = q->ch[i];
    k    = line - mu_quill__lstart(q, i);
    base = mu_quill__bstart(q, i);
    if (k >= c->nstarts)
        return r;
    r.offset = base + c->starts[k];
    if (k + 1u < c->nstarts)
        r.len = (uint32_t)c->starts[k + 1u] - (uint32_t)c->starts[k] - 1u;
    else if (line + 1u >= q->nlines)
        r.len = q->text_len - r.offset;
    else
        r.len = mu_quill_line_offset(q, line + 1u) - r.offset - 1u;
    return r;
}

/* One descent total. The previous version called line_at (one descent) and
 * then by_line again to find the owning chunk (a second descent), which alone
 * cost ~77 ns of the 209 ns warm read_line. */
MU_INLINE uint32_t mu_quill_line_text(mu_quill* q, uint32_t line, char* MU_RESTRICT dst,
                                      uint32_t cap)
{
    uint32_t        i, k, base, off, len, n;
    mu_quill_chunk* c;

    if (line >= q->nlines)
        return 0;
    i = mu_quill__by_line(q, line);
    if (i == UINT32_MAX)
        return 0;
    mu_quill__starts(q, i);
    c    = q->ch[i];
    k    = line - mu_quill__lstart(q, i);
    base = mu_quill__bstart(q, i);
    if (k >= c->nstarts)
        return 0;
    off = base + c->starts[k];
    if (k + 1u < c->nstarts)
        len = (uint32_t)c->starts[k + 1u] - (uint32_t)c->starts[k] - 1u;
    else if (line + 1u >= q->nlines)
        len = q->text_len - off;
    else
        len = mu_quill_line_offset(q, line + 1u) - off - 1u;

    n = MU_MIN(len, cap);
    if (n == 0u)
        return 0;
    {
        const uint32_t p = off - base;
        if (p <= c->nbytes && n <= c->nbytes - p)
        {
            mu_quill__read(q, c, p, n, (uint8_t*)dst);
            return n;
        }
    }
    return mu_quill_text(q, off, n, dst);
}

/* Fill up to k lines starting at `line`. ONE descent, then a forward walk:
 * calling line_offset per line did k independent three-level searches and
 * made line_range 19x slower than mu_ted's fused walk. */
MU_INLINE uint32_t mu_quill_lines(mu_quill* q, uint32_t line, uint32_t k,
                                  mu_quill_line* MU_RESTRICT out)
{
    uint32_t n = 0, i, local, base;

    if (line >= q->nlines)
        return 0;
    k = MU_MIN(k, q->nlines - line);
    if (k == 0u)
        return 0;

    i = mu_quill__by_line(q, line);
    if (i == UINT32_MAX)
        return 0;
    mu_quill__starts(q, i);
    local = line - mu_quill__lstart(q, i);
    base  = mu_quill__bstart(q, i);

    while (n < k)
    {
        const mu_quill_chunk* c  = q->ch[i];
        const uint32_t        ns = c->nl + (i == 0u ? 1u : 0u);

        /* Emit offset AND length together, 16 at a time, straight from the
         * u16 start array. The scalar version wrote all the offsets first and
         * then looped back computing out[j+1].offset - out[j].offset, which
         * read every cache line it had just stored -- a store-to-load
         * forwarding stall on the whole output (AGENTS.md 6.11).
         *
         * Interleaving two u32 vectors into {offset,len} pairs needs
         * permutex2var, NOT unpacklo/hi + shuffle_i32x4: the latter cannot
         * produce o0,l0,o1,l1,... across 128-bit lanes. */
#if !defined(MU_QUILL_NO_SIMD) && defined(__AVX512F__)
        {
            const __m512i basev = _mm512_set1_epi32((int)base);
            const __m512i one   = _mm512_set1_epi32(1);
            const __m512i i0    = _mm512_setr_epi32(0,16,1,17,2,18,3,19,4,20,5,21,6,22,7,23);
            const __m512i i1    = _mm512_setr_epi32(8,24,9,25,10,26,11,27,12,28,13,29,14,30,15,31);
            while (local + 16u < ns && n + 16u <= k)
            {
                const __m256i s0 = _mm256_loadu_si256((const __m256i*)(c->starts + local));
                const __m256i s1 = _mm256_loadu_si256((const __m256i*)(c->starts + local + 1));
                const __m512i o  = _mm512_cvtepu16_epi32(s0);
                const __m512i d  = _mm512_sub_epi32(_mm512_sub_epi32(
                                       _mm512_cvtepu16_epi32(s1), o), one);
                const __m512i ab = _mm512_add_epi32(o, basev);
                _mm512_storeu_si512((void*)(out + n),
                                    _mm512_permutex2var_epi32(ab, i0, d));
                _mm512_storeu_si512((void*)(out + n + 8),
                                    _mm512_permutex2var_epi32(ab, i1, d));
                n     += 16u;
                local += 16u;
            }
        }
#endif
        while (local < ns && n < k)
        {
            out[n].offset = base + c->starts[local];
            out[n].len    = (local + 1u < ns)
                          ? (uint32_t)c->starts[local + 1u] - (uint32_t)c->starts[local] - 1u
                          : 0u; /* patched below: crosses into the next chunk */
            ++n;
            ++local;
        }
        if (n == k)
            break;
        ++i;
        if (i >= q->nch)
            break;
        mu_quill__starts(q, i);
        local = 0u;
        base  = mu_quill__bstart(q, i);
    }

    /* Only the entries whose successor was not available above still need a
     * length: at most one per chunk boundary, plus the final line. */
    {
        uint32_t j;
        for (j = 0; j + 1u < n; ++j)
            if (out[j].len == 0u && out[j + 1u].offset != out[j].offset + 1u)
                out[j].len = out[j + 1u].offset - out[j].offset - 1u;
        if (n)
        {
            const uint32_t last = n - 1u;
            if (line + n < q->nlines)
            {
                if (out[last].len == 0u)
                    out[last].len =
                        mu_quill_line_offset(q, line + n) - out[last].offset - 1u;
            }
            else
                out[last].len = q->text_len - out[last].offset;
        }
    }
    return n;
}

/* Bulk load packs chunks to capacity. Going through insert() split every
 * chunk near its middle, leaving them ~half full: 4014 chunks for an 8 MiB
 * document instead of 2048, which deepened every descent and doubled the
 * number of lazy line-start rebuilds. */
MU_INLINE bool mu_quill_load(mu_quill* q, const char* text, size_t len)
{
    size_t   off = 0;
    uint32_t need;

    MU_ASSERT(q);
    if (len == 0u)
        return true;
    if (q->text_len != 0u)
        return mu_quill_insert(q, q->text_len, text, len);
    if (len > (size_t)UINT32_MAX)
        return false;

    need = (uint32_t)((len + MU_QUILL_CHUNK - 1u) / MU_QUILL_CHUNK);
    if (!mu_quill__grow_ch(q, need) ||
        !mu_quill__grow_groups(q, (need + MU_QUILL_GROUP - 1u) / MU_QUILL_GROUP) ||
        !mu_quill__grow_supers(q, ((need + MU_QUILL_GROUP - 1u) / MU_QUILL_GROUP
                                   + MU_QUILL_SUPER - 1u) / MU_QUILL_SUPER))
        return false;

    /* reuse the empty chunk init() created for the first slice */
    while (off < len)
    {
        const uint32_t n = (uint32_t)MU_MIN((size_t)MU_QUILL_CHUNK, len - off);
        mu_quill_chunk* c;
        if (off == 0u)
            c = q->ch[0];
        else
        {
            c = mu_quill__chunk_new(q, 16u);
            if (!c)
                return false;
            q->ch[q->nch++] = c;
        }
        {
            uint8_t* b = mu_quill__buf(q, c);
            MU_MEMCPY(b, text + off, n);
            c->nbytes = n;
            c->gap    = n;
            c->nl     = mu_quill__count_nl((const uint8_t*)text + off, n);
        }
        off += n;
    }
    mu_quill__mark_all(q);
    mu_quill__reindex(q);
    q->cur_ch   = q->nch - 1u;
    q->cur_base = mu_quill__bstart(q, q->cur_ch);
    q->cur_abs  = q->text_len;
    return true;
}

/* ==========================================================================
 * Cursor, zero-copy spans, and sequential viewport views
 * ========================================================================== */

typedef struct mu_quill_cursor
{
    uint32_t offset;
    uint32_t line;
    uint32_t col; /* bytes from line start, not display cells */
} mu_quill_cursor;

/* Contiguous physical run starting at `offset`. The returned pointer is valid
 * until the next mutation of q. This is the zero-copy feed for Tree-sitter's
 * TSInput.read: no whole-document flatten, no per-chunk memcpy. */
MU_INLINE const uint8_t* mu_quill_span(const mu_quill* q, uint32_t offset,
                                       uint32_t* MU_RESTRICT len)
{
    const mu_quill_chunk* c;
    const uint8_t* b;
    uint32_t i, p, gl;

    *len = 0u;
    if (offset >= q->text_len)
        return NULL;
    i = mu_quill__by_byte(q, offset);
    if (i == UINT32_MAX)
        return NULL;
    c  = q->ch[i];
    p  = offset - mu_quill__bstart(q, i);
    gl = MU_QUILL_CHUNK - c->nbytes;
    b  = mu_quill__buf(q, c);
    if (p < c->gap)
    {
        *len = c->gap - p;
        return b + p;
    }
    *len = c->nbytes - p;
    return b + p + gl;
}

MU_INLINE uint32_t mu_quill_cursor_to_offset(mu_quill* q, uint32_t line, uint32_t col)
{
    uint32_t off;
    if (q->nlines == 0u)
        return 0u;
    if (line >= q->nlines)
        line = q->nlines - 1u;
    off = mu_quill_line_offset(q, line);
    {
        const uint32_t next = (line + 1u < q->nlines)
            ? mu_quill_line_offset(q, line + 1u) - 1u
            : q->text_len;
        return off + MU_MIN(col, next - off);
    }
}

MU_INLINE mu_quill_cursor mu_quill_offset_to_cursor(mu_quill* q, uint32_t offset)
{
    mu_quill_cursor r;
    mu_quill_chunk* c;
    uint32_t i, p, lo, hi, k, base, ls;

    r.offset = MU_MIN(offset, q->text_len);
    r.line   = 0u;
    r.col    = 0u;
    if (q->text_len == 0u)
        return r;
    if (r.offset == q->text_len)
    {
        r.line = q->nlines - 1u;
        r.col  = r.offset - mu_quill_line_offset(q, r.line);
        return r;
    }

    i = mu_quill__by_byte(q, r.offset);
    if (i == UINT32_MAX)
        return r;
    /* A chunk boundary after a newline belongs to the start owned by the
     * predecessor (p+1 == nbytes). Stay on that predecessor so col == 0. */
    if (i > 0u && mu_quill__bstart(q, i) == r.offset)
        --i;
    mu_quill__starts(q, i);
    c    = q->ch[i];
    base = mu_quill__bstart(q, i);
    p    = r.offset - base;
    ls   = mu_quill__lstart(q, i);
    if (c->nstarts == 0u || (uint32_t)c->starts[0] > p)
    {
        r.line = ls ? ls - 1u : 0u;
        r.col  = r.offset - mu_quill_line_offset(q, r.line);
        return r;
    }
    lo = 0u; hi = c->nstarts;
    while (lo + 1u < hi)
    {
        const uint32_t mid = (lo + hi) >> 1;
        if ((uint32_t)c->starts[mid] <= p) lo = mid; else hi = mid;
    }
    k = lo;
    r.line = ls + k;
    r.col  = p - (uint32_t)c->starts[k];
    return r;
}

/* Sequential line view: ONE descent, then a forward walk over chunk starts.
 * This is the viewport/scroll path. It avoids the per-line descent that made
 * true incremental scrolling slower than mu_ted in round 9. */
typedef struct mu_quill_view
{
    uint32_t i;     /* chunk index */
    uint32_t local; /* start index inside chunk */
    uint32_t base;  /* absolute byte base of chunk */
    uint32_t line;  /* next absolute line index */
} mu_quill_view;

MU_INLINE bool mu_quill_view_start(mu_quill* q, uint32_t line,
                                   mu_quill_view* MU_RESTRICT v)
{
    uint32_t i;
    if (line >= q->nlines)
    {
        v->i = q->nch; v->local = 0u; v->base = 0u; v->line = q->nlines;
        return false;
    }
    i = mu_quill__by_line(q, line);
    if (i == UINT32_MAX)
    {
        v->i = q->nch; v->local = 0u; v->base = 0u; v->line = q->nlines;
        return false;
    }
    mu_quill__starts(q, i);
    v->i     = i;
    v->local = line - mu_quill__lstart(q, i);
    v->base  = mu_quill__bstart(q, i);
    v->line  = line;
    return true;
}

/* Advance to the next chunk that owns a line start. Chunks with zero starts
 * are continuations of a long line and cannot begin a new line. */
MU_INLINE bool mu_quill__view_next_chunk(mu_quill* q, mu_quill_view* v)
{
    uint32_t j = v->i + 1u;
    while (j < q->nch)
    {
        const uint32_t ns = q->ch[j]->nl + (j == 0u ? 1u : 0u);
        if (ns != 0u)
        {
            mu_quill__starts(q, j);
            v->i     = j;
            v->local = 0u;
            v->base  = mu_quill__bstart(q, j);
            return true;
        }
        ++j;
    }
    v->i = q->nch;
    return false;
}

MU_INLINE uint32_t mu_quill__view_len(mu_quill* q, const mu_quill_view* v,
                                      uint32_t ns, uint32_t off)
{
    const mu_quill_chunk* c = q->ch[v->i];
    if (v->local + 1u < ns)
        return (uint32_t)c->starts[v->local + 1u] - (uint32_t)c->starts[v->local] - 1u;
    if (v->line + 1u >= q->nlines)
        return q->text_len - off;
    {
        mu_quill_view peek = *v;
        if (!mu_quill__view_next_chunk(q, &peek))
            return q->text_len - off;
        return peek.base + (uint32_t)q->ch[peek.i]->starts[0] - off - 1u;
    }
}

MU_INLINE bool mu_quill_view_next(mu_quill* q, mu_quill_view* MU_RESTRICT v,
                                  mu_quill_line* MU_RESTRICT out)
{
    mu_quill_chunk* c;
    uint32_t ns, off;

    if (v->line >= q->nlines || v->i >= q->nch)
        return false;
    c  = q->ch[v->i];
    ns = c->nl + (v->i == 0u ? 1u : 0u);
    if (v->local >= ns)
    {
        if (!mu_quill__view_next_chunk(q, v))
            return false;
        c  = q->ch[v->i];
        ns = c->nl + (v->i == 0u ? 1u : 0u);
    }
    off = v->base + (uint32_t)c->starts[v->local];
    out->offset = off;
    out->len    = mu_quill__view_len(q, v, ns, off);
    ++v->line;
    ++v->local;
    if (v->local >= ns)
        (void)mu_quill__view_next_chunk(q, v);
    return true;
}

MU_INLINE bool mu_quill_view_next_text(mu_quill* q, mu_quill_view* MU_RESTRICT v,
                                       char* MU_RESTRICT dst, uint32_t cap,
                                       mu_quill_line* MU_RESTRICT out)
{
    mu_quill_chunk* c;
    uint32_t ns, off, len, n, p;

    if (v->line >= q->nlines || v->i >= q->nch)
        return false;
    c  = q->ch[v->i];
    ns = c->nl + (v->i == 0u ? 1u : 0u);
    if (v->local >= ns)
    {
        if (!mu_quill__view_next_chunk(q, v))
            return false;
        c  = q->ch[v->i];
        ns = c->nl + (v->i == 0u ? 1u : 0u);
    }
    off = v->base + (uint32_t)c->starts[v->local];
    len = mu_quill__view_len(q, v, ns, off);
    out->offset = off;
    out->len    = len;

    n = MU_MIN(len, cap);
    if (n != 0u)
    {
        p = off - v->base;
        if (p <= c->nbytes && n <= c->nbytes - p)
            mu_quill__read(q, c, p, n, (uint8_t*)dst);
        else
            (void)mu_quill_text(q, off, n, dst);
    }

    ++v->line;
    ++v->local;
    if (v->local >= ns)
        (void)mu_quill__view_next_chunk(q, v);
    return true;
}

#endif /* MU_QUILL_H */
