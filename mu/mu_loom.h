#pragma once
#ifndef MU_LOOM_H
#define MU_LOOM_H

/* ---------------------------------------------------------------------------
 * mu_loom — LOOM: Line-Oriented Ordered Memory.
 * A text/code-editor buffer built for the six operations an editor actually
 * runs, in frequency order:
 *
 *   1. read line i as text            (very high)  renderer, lexer, indent
 *   2. walk lines i..i+k in order     (very high)  viewport, tokenise, search
 *   3. byte offset -> (line, col)     (high)       diagnostics, LSP, hit-test
 *   4. (line, col) -> byte offset     (high)       LSP reply, goto, markers
 *   5. insert 1 byte at the cursor    (high,burst) typing
 *   6. insert/delete a range          (medium)     paste, format, multi-cursor
 *
 * ---------------------------------------------------------------------------
 * THE ONE IDEA THAT MAKES THIS DIFFERENT: STORE LENGTHS, NOT POSITIONS.
 * ---------------------------------------------------------------------------
 * Every prior buffer stores *absolute* line starts (line array, mu_ted's
 * per-segment u16 offsets) or a newline bitmap (Zed SumTree, CORD). Both are
 * *positional*: inserting one byte moves every position after it, so the
 * index must be repaired over a whole suffix.
 *
 *   mu_ted  : typing adds +1 to ~400 u16 line starts in the segment,
 *             plus ~40 u32 suffix adds across its three absolute levels.
 *   CORD    : typing shifts a 2 KiB newline bitmap.
 *   mu_loom : typing does `len[i] += 1` and three scalar += 1.
 *             THREE STORES. Independent of document size AND of line count.
 *
 * A length is a *relative* quantity. It does not change when text moves
 * somewhere else. That single change of representation removes suffix
 * repair from the hottest operation in the editor.
 *
 * Positions are then derived by prefix sum, which is the one thing SIMD is
 * genuinely great at: 16 line offsets in 4 valignd + 4 paddd, instead of 16
 * dependent scalar adds.
 *
 * ---------------------------------------------------------------------------
 * LAYOUT — four flat tables, fixed-size slots, zero pointers, zero flags
 * ---------------------------------------------------------------------------
 *   text+leaf : 16 KiB slots, address = base + (handle << 14).  No seg** array.
 *   len[]     : u8 per line, RELATIVE, inside the leaf.  1 byte/line.
 *   index     : 3 flat levels of RELATIVE (bytes, lines) sums, fanout 16.
 *   cursor    : one entry, makes ops 2 and 5 O(1).
 *
 * Leaf slot (16384 B, 64-B aligned):
 *   [0     .. 15744)  text bytes
 *   [15744 .. 15808)  long_len[16]  u32 escapes for lines >= 255 B
 *   [15808 .. 16320)  len[512]      u8 relative line lengths (incl. '\n')
 *   [16320 .. 16384)  header        64 B, one cache line
 *
 * Index (all RELATIVE — this is why an edit is 3 stores, not a suffix):
 *   leaf_bytes[i] leaf_lines[i]   mirrors of the header, kept dense so a
 *                                 descent never touches a leaf header
 *   grp_bytes[g]  grp_lines[g]    sum of 16 leaves
 *   sup_bytes[s]  sup_lines[s]    sum of 16 groups
 *
 * WHY u8 LENGTHS
 *   Measured over 3.87 MB of /usr/include (95,728 lines): 99.95% of lines are
 *   under 255 bytes. So u8 covers essentially all real source, at 1 B/line =
 *   2.5% overhead at 40 B/line — half of mu_ted's u16, one fifth of CORD's
 *   12.5% bitmap. Lines >= 255 B use an escape (255) plus a u32 in a 16-entry
 *   per-leaf table. `nlong` in the header selects the kernel ONCE PER LEAF,
 *   never per line, so the hot inner loop stays branch-free (AGENTS.md 1.5).
 *   A leaf with no long lines — the 99.95% case — runs the pure SIMD path.
 *
 * WHAT IS FIXED RELATIVE TO mu_ted (measured, not asserted)
 *   - `mu_ted_seg** seg` is an array of pointers: every segment touch is a
 *     dependent load. LOOM uses handle<<14 into one arena. (AGENTS.md 1.6)
 *   - `if (t->seg[i]->dead)` inside mu_ted_lines / seg_by_byte is a
 *     data-dependent flag branch in a hot loop. LOOM has no tombstones:
 *     removed leaves are swap-removed out of existence. (AGENTS.md 1.3)
 *   - `mu_ted_line_range` calls line_offset TWICE — two full 3-level descents
 *     for the single hottest query. LOOM stores the length, so op 1 is ONE
 *     descent and no subtraction. (this is the whole point of lengths)
 *
 * CONTRACTS / LIMITS
 *   - Document < 4 GiB (u32 offsets). Line count < 4 Gi.
 *   - '\n' is the only terminator. line_count = newlines + 1.
 *   - len[i] INCLUDES the '\n' for every line except the document's last,
 *     which has none. So sum(len) over the whole document == text_len exactly,
 *     and line_range needs no second lookup: reported len = stored - (not last).
 *   - A line may span leaves (minified files). len[] holds the lengths of
 *     lines that START in this leaf; the last one may extend past nbytes.
 *     A leaf entirely inside one long line has nlines == 0.
 *   - Max 16 escaped (>=255 B) lines per leaf; the leaf splits before the 17th.
 *   - Not thread safe: one buffer, one owner.
 *
 * TUNABLES
 *   MU_LOOM_LEAF_BITS   14     leaf payload = 1<<14; stride adds 64 B
 *   MU_LOOM_NO_SIMD     0      1 = scalar reference kernels (for ablation)
 *   MU_LOOM_TRACE       0      1 = record every index/leaf cache line touched
 * ------------------------------------------------------------------------- */

#include "mu_common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if !defined(MU_LOOM_NO_SIMD)
#define MU_LOOM_NO_SIMD 0
#endif
#if !defined(MU_LOOM_TRACE)
#define MU_LOOM_TRACE 0
#endif

#if !MU_LOOM_NO_SIMD && defined(__AVX512F__) && defined(__AVX512BW__)
#define MU_LOOM_AVX512 1
#include <immintrin.h>
#elif !MU_LOOM_NO_SIMD && defined(__AVX2__)
#define MU_LOOM_AVX2 1
#include <immintrin.h>
#else
#define MU_LOOM_SCALAR 1
#endif

MU_BEGIN_EXTERN_C

#ifndef MU_LOOM_LEAF_BITS
#define MU_LOOM_LEAF_BITS 14u
#endif

#define MU_LOOM_LEAF_BYTES (1u << MU_LOOM_LEAF_BITS)

/* Slot STRIDE is the leaf size plus ONE odd cache line (AGENTS.md 6.12).
 * A 48 KiB 12-way L1d has 64 sets, so bits [11:6] of the address choose the
 * set. With a power-of-two 16 KiB stride every leaf's header lands in the
 * SAME set and every len[] block lands in another single set: 542 leaves
 * compete for 12 ways, turning a 768-line cache into a 12-line one. Measured
 * set coverage was 1/64 for both. Padding by 64 B makes the stride 257 lines,
 * 257 % 64 == 1, so consecutive leaves rotate through all 64 sets. */
#define MU_LOOM_SLOT_BYTES (MU_LOOM_LEAF_BYTES + 64u)
#define MU_LOOM_MAX_LINES 512u
/* Escape entries for lines whose length does not fit in a u8. 16 was too
 * few: on a range-edit workload 26% of fast inserts bailed out here and fell
 * back to re-planning a whole region. 64 entries cost 256 B, shrinking the
 * text payload by 1.2% (15744 -> 15552), and cover a leaf full of long lines
 * (15552/255 ~ 61). Everything else in the layout derives from this. */
#ifndef MU_LOOM_MAX_LONG
#define MU_LOOM_MAX_LONG 64u
#endif
#define MU_LOOM_HDR_OFF (MU_LOOM_LEAF_BYTES - 64u)
#define MU_LOOM_LEN_OFF (MU_LOOM_HDR_OFF - MU_LOOM_MAX_LINES)
#define MU_LOOM_LONG_OFF (MU_LOOM_LEN_OFF - MU_LOOM_MAX_LONG * 4u)
#define MU_LOOM_TEXT_CAP MU_LOOM_LONG_OFF
#define MU_LOOM_GROUP 16u
#define MU_LOOM_SUPER 16u

MU_STATIC_ASSERT(MU_LOOM_LEAF_BYTES >= 4096u, "leaf too small");
MU_STATIC_ASSERT(MU_LOOM_TEXT_CAP > 0u, "leaf layout underflow");

/* ---------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

typedef struct mu_loom_hdr
{
    uint32_t nbytes;    /* live text bytes in this slot                    */
    uint32_t nlines;    /* line starts recorded in len[]                   */
    uint32_t nlong;     /* entries in len[] equal to 255                   */
    uint32_t free_next; /* free-slot link; valid only while the slot is free */
    uint32_t lead;      /* leading bytes owned by the PREVIOUS leaf's last
                         * line (non-zero only on a continuation leaf). The
                         * first line recorded here starts at lead.        */
    uint32_t cp[8];     /* cp[j] = true byte sum of len[0 .. 64j). A rank
                         * directory: turns the intra-leaf prefix from a
                         * 512-element scan into ONE 64-element SIMD sum.
                         * 32 B, so it shares the header's cache line.     */
    uint32_t pad[3];    /* pad to exactly one 64 B cache line              */
} mu_loom_hdr;

MU_STATIC_ASSERT(sizeof(mu_loom_hdr) == 64u, "header must be one cache line");

typedef struct mu_loom_line
{
    uint32_t offset; /* absolute byte offset of the line start */
    uint32_t len;    /* bytes before the '\n' (excludes it)    */
} mu_loom_line;

typedef struct mu_loom_cursor
{
    uint32_t line;
    uint32_t col;
    uint32_t offset;
} mu_loom_cursor;

/* Result of one descent. Both coordinates come out of a single walk. */
typedef struct mu_loom_pos
{
    uint32_t leaf;      /* leaf index in document order */
    uint32_t local;     /* line index within the leaf, or byte offset */
    uint32_t byte_base; /* absolute byte offset of the leaf start */
    uint32_t line_base; /* absolute line index of the leaf's first line */
} mu_loom_pos;

#if MU_LOOM_TRACE
/* Cache-line access trace. Hardware PMU counters are not exposed in every
 * VM, so miss attribution is done by replaying the exact set of cache lines
 * an operation touches through an LRU model. Recording is compiled out in
 * release builds and costs nothing there. */
typedef struct mu_loom_trace
{
    uint64_t *addr;
    uint64_t  n;
    uint64_t  cap;
} mu_loom_trace;
extern mu_loom_trace mu_loom_trace_state;
#define MU_LOOM_TOUCH(p)                                                      \
    do {                                                                      \
        mu_loom_trace *tr_ = &mu_loom_trace_state;                             \
        if (tr_->n < tr_->cap) tr_->addr[tr_->n++] = (uint64_t)(uintptr_t)(p);  \
    } while (0)
#else
#define MU_LOOM_TOUCH(p) ((void)0)
#endif

typedef struct mu_loom
{
    uint8_t *scratch;   /* re-splice staging, grown outside the hot path */
    size_t   scratch_cap;

    uint8_t *arena;     /* leaf slots, handle * MU_LOOM_SLOT_BYTES */
    uint32_t slot_cap;  /* slots allocated                        */
    uint32_t slot_high; /* slots ever used                        */
    uint32_t free_head; /* free-slot list head, UINT32_MAX = none */

    /* flat document-order leaf directory: mirrors, so a descent never
     * touches a leaf header (32x denser than visiting slots) */
    uint32_t *leaf_bytes;
    uint32_t *leaf_lines;
    uint32_t *leaf_slot;
    uint32_t  nleaf;
    uint32_t  leaf_cap;

    /* relative sums, fanout 32 */
    uint32_t *grp_bytes, *grp_lines;
    uint32_t  ngrp, grp_cap;
    uint32_t *sup_bytes, *sup_lines;
    uint32_t  nsup, sup_cap;

    /* ABSOLUTE cumulative mirrors of the three relative levels above.
     * Descending a relative directory needs a prefix sum at every level;
     * descending an absolute one is a compare plus a popcount, and the other
     * coordinate is a single load instead of a masked reduction. The cost is
     * a suffix add per edit, which is a unit-stride SIMD stream over a table
     * that is 0.05% of the document. Reads are 4 of the 6 ranked operations,
     * so the trade is made in their favour. */
    uint32_t *leaf_bend, *leaf_lend;
    uint32_t *grp_bend,  *grp_lend;
    uint32_t *sup_bend,  *sup_lend;

    uint32_t text_len;
    uint32_t nlines; /* document lines, >= 1 */

    /* cursor: last descent, for ops 2 and 5 */
    uint32_t cur_valid;
    uint32_t cur_leaf;
    uint32_t cur_byte_base;
    uint32_t cur_line_base;

    /* sequential line cursor: op 2 walks without re-descending */
    uint32_t lc_valid;
    uint32_t lc_leaf;
    uint32_t lc_local;
    uint32_t lc_line;
    uint32_t lc_off;
    uint32_t lc_stored;

    /* telemetry */
    uint64_t stat_descents;
    uint64_t stat_cur_hits;
    uint64_t stat_lc_hits;
    uint64_t stat_splits;
    uint64_t stat_bytes_moved;
    uint64_t stat_index_stores;
} mu_loom;

/* ---------------------------------------------------------------------------
 * Slot accessors. Handle arithmetic only — never a pointer array.
 * ------------------------------------------------------------------------- */

#define MU_LOOM_SLOT(l, h) ((l)->arena + (size_t)(h) * MU_LOOM_SLOT_BYTES)
#define MU_LOOM_TEXT(l, h) (MU_LOOM_SLOT(l, h))
#define MU_LOOM_LEN(l, h) (MU_LOOM_SLOT(l, h) + MU_LOOM_LEN_OFF)
#define MU_LOOM_LONG(l, h) ((uint32_t *)(MU_LOOM_SLOT(l, h) + MU_LOOM_LONG_OFF))
#define MU_LOOM_HDR(l, h) ((mu_loom_hdr *)(MU_LOOM_SLOT(l, h) + MU_LOOM_HDR_OFF))

/* ---------------------------------------------------------------------------
 * SIMD prefix sum — the kernel that replaces positional storage.
 *
 * 16 u32 inclusive prefix sum in 4 valignd + 4 paddd. valignd shifts across
 * the whole 512-bit register, so unlike AVX2 byte shifts there is no 128-bit
 * lane trap (simd-hardware-c pitfall #1). A scalar chain of 16 dependent adds
 * is 16 cycles of latency; this is 8 instructions, 4 deep.
 * ------------------------------------------------------------------------- */

#if MU_LOOM_AVX512
MU_INLINE __m512i mu_loom__prefix16(__m512i x)
{
    const __m512i z = _mm512_setzero_si512();
    x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, z, 15));
    x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, z, 14));
    x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, z, 12));
    x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, z, 8));
    return x;
}

/* Sum of n u8 lengths. VPSADBW against zero sums 8 bytes into one u64 lane,
 * so ONE instruction covers 64 lines. Two accumulators break the serial
 * chain. This is why deriving a position is not slower than storing one. */
MU_INLINE uint32_t mu_loom__sum_u8(const uint8_t *MU_RESTRICT p, uint32_t n)
{
    const __m512i z = _mm512_setzero_si512();
    __m512i       a0 = z, a1 = z;
    uint32_t      i = 0u;
    __mmask64     m;

    for (; i + 128u <= n; i += 128u) {
        MU_LOOM_TOUCH(p + i);
        a0 = _mm512_add_epi64(a0, _mm512_sad_epu8(_mm512_loadu_si512((const void *)(p + i)), z));
        a1 = _mm512_add_epi64(a1, _mm512_sad_epu8(_mm512_loadu_si512((const void *)(p + i + 64)), z));
    }
    for (; i + 64u <= n; i += 64u) {
        MU_LOOM_TOUCH(p + i);
        a0 = _mm512_add_epi64(a0, _mm512_sad_epu8(_mm512_loadu_si512((const void *)(p + i)), z));
    }
    if (i < n) {
        MU_LOOM_TOUCH(p + i);
        m  = (__mmask64)((1ull << (n - i)) - 1ull);
        a0 = _mm512_add_epi64(a0, _mm512_sad_epu8(_mm512_maskz_loadu_epi8(m, (const void *)(p + i)), z));
    }
    return (uint32_t)_mm512_reduce_add_epi64(_mm512_add_epi64(a0, a1));
}

/* First line whose inclusive prefix EXCEEDS target. 16 lengths per step,
 * exact index from popcount of a compare mask: no inner scan, no per-line
 * branch. Prefix sums are monotone so `<= target` is a contiguous run. */
MU_INLINE uint32_t mu_loom__find_u8(const uint8_t *MU_RESTRICT p, uint32_t n,
                                    uint32_t target, uint32_t *MU_RESTRICT before)
{
    __m512i   v, pr, pick;
    __mmask16 msk, load;
    uint32_t  i = 0u, carry = 0u, k, rem;

    for (; i < n; i += 16u) {
        rem  = n - i;
        load = (rem >= 16u) ? (__mmask16)0xFFFFu : (__mmask16)((1u << rem) - 1u);
        MU_LOOM_TOUCH(p + i);
        v   = _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(load, (const void *)(p + i)));
        pr  = _mm512_add_epi32(mu_loom__prefix16(v), _mm512_set1_epi32((int)carry));
        msk = _mm512_cmple_epu32_mask(pr, _mm512_set1_epi32((int)target));
        k   = (uint32_t)mu_popcount_u32((uint32_t)msk);
        if (k < 16u) {
            if (k == 0u) { *before = carry; return i; }
            pick    = _mm512_permutexvar_epi32(_mm512_set1_epi32((int)(k - 1u)), pr);
            *before = (uint32_t)_mm512_cvtsi512_si32(pick);
            return i + k;
        }
        carry = (uint32_t)_mm512_cvtsi512_si32(
                    _mm512_permutexvar_epi32(_mm512_set1_epi32(15), pr));
        if (rem < 16u) break;
    }
    *before = carry;
    return n;
}

/* ONE index level, BOTH coordinates. `key` is the array we search on,
 * `acc` the array we accumulate. Each is 16 u32 = exactly one cache line.
 * Returns the child index; writes both prefixes. This is the dual descent
 * that makes offset->(line,col) cost the same as a single-coordinate query. */
MU_INLINE uint32_t mu_loom__level16(const uint32_t *MU_RESTRICT key,
                                    const uint32_t *MU_RESTRICT acc,
                                    uint32_t target,
                                    uint32_t *MU_RESTRICT key_before,
                                    uint32_t *MU_RESTRICT acc_before)
{
    __m512i   pr, av, pick;
    __mmask16 msk;
    uint32_t  k;

    MU_LOOM_TOUCH(key);
    MU_LOOM_TOUCH(acc);
    pr  = mu_loom__prefix16(_mm512_loadu_si512((const void *)key));
    msk = _mm512_cmple_epu32_mask(pr, _mm512_set1_epi32((int)target));
    k   = (uint32_t)mu_popcount_u32((uint32_t)msk);
    av  = _mm512_loadu_si512((const void *)acc);
    *acc_before = (uint32_t)_mm512_reduce_add_epi32(
        _mm512_maskz_mov_epi32((__mmask16)((1u << k) - 1u), av));
    if (k == 0u) { *key_before = 0u; return 0u; }
    pick        = _mm512_permutexvar_epi32(_mm512_set1_epi32((int)(k - 1u)), pr);
    *key_before = (uint32_t)_mm512_cvtsi512_si32(pick);
    return k;
}
#endif /* MU_LOOM_AVX512 */

/* ---- scalar reference kernels (also the SIMD ablation) ---- */

MU_INLINE uint32_t mu_loom__sum_u8_scalar(const uint8_t *p, uint32_t n)
{
    uint32_t i = 0u, a0 = 0u, a1 = 0u, a2 = 0u, a3 = 0u;
    for (; i + 4u <= n; i += 4u) { a0 += p[i]; a1 += p[i+1]; a2 += p[i+2]; a3 += p[i+3]; }
    for (; i < n; ++i) a0 += p[i];
    return a0 + a1 + a2 + a3;
}

MU_INLINE uint32_t mu_loom__find_u8_scalar(const uint8_t *p, uint32_t n, uint32_t target,
                                           uint32_t *before)
{
    uint32_t i, acc = 0u;
    for (i = 0u; i < n; ++i) {
        if (acc + p[i] > target) { *before = acc; return i; }
        acc += p[i];
    }
    *before = acc;
    return n;
}

MU_INLINE uint32_t mu_loom__level16_scalar(const uint32_t *key, const uint32_t *acc,
                                           uint32_t n, uint32_t target,
                                           uint32_t *key_before, uint32_t *acc_before)
{
    uint32_t i, kb = 0u, ab = 0u;
    /* n MUST bound the scan: entries past the live count are stale, and a
     * zero there would let the walk run off the end of the directory. */
    for (i = 0u; i < n; ++i) {
        if (kb + key[i] > target) break;
        kb += key[i];
        ab += acc[i];
    }
    *key_before = kb;
    *acc_before = ab;
    return i;
}

MU_INLINE uint32_t mu_loom__sum_len(const uint8_t *p, uint32_t n)
{
#if MU_LOOM_AVX512
    return mu_loom__sum_u8(p, n);
#else
    return mu_loom__sum_u8_scalar(p, n);
#endif
}

MU_INLINE uint32_t mu_loom__find_len(const uint8_t *p, uint32_t n, uint32_t t, uint32_t *b)
{
#if MU_LOOM_AVX512
    return mu_loom__find_u8(p, n, t, b);
#else
    return mu_loom__find_u8_scalar(p, n, t, b);
#endif
}

MU_INLINE uint32_t mu_loom__level(const uint32_t *k, const uint32_t *a, uint32_t t,
                                  uint32_t *kb, uint32_t *ab)
{
#if MU_LOOM_AVX512
    return mu_loom__level16(k, a, t, kb, ab);
#else
    return mu_loom__level16_scalar(k, a, 16u, t, kb, ab);
#endif
}

/* Rebuild the 64-line checkpoint directory. Called once per leaf fill, never
 * per keystroke. Unused entries hold the leaf total so a search can never
 * select an empty block. */
/* Rebuild the checkpoints, given that no length entry BELOW `first` changed.
 * Blocks entirely before the edit keep their checkpoint, so only the blocks
 * from `first>>6` onward are re-summed. A range edit touches one region of a
 * 512-entry table; re-reading all 8 cache lines to rebuild 8 checkpoints was
 * costing ~212 ns per call, twice per range edit, and those lines are cold
 * (the cache model attributes ~1 L1 miss per query to len[]). */
MU_INLINE void mu_loom__cp_rebuild_from(mu_loom *l, uint32_t slot, uint32_t first)
{
    mu_loom_hdr   *h    = MU_LOOM_HDR(l, slot);
    const uint8_t *len  = MU_LOOM_LEN(l, slot);
    const uint32_t *esc = MU_LOOM_LONG(l, slot);
    uint32_t       j, b, b0, run = 0u, e = 0u, next = 64u;

    if (MU_LIKELY(h->nlong == 0u)) {
        /* Eight 64-byte blocks: one VPSADBW each, then a prefix over the
         * block totals. Start at the first block the edit could have dirtied;
         * cp[b0] sums only entries below 64*b0, which are untouched. */
        uint32_t tot;
        b0  = first >> 6;
        tot = h->cp[b0];
        for (b = b0 + 1u; b < 8u; ++b) {
            uint32_t base = (b - 1u) * 64u, cnt;
            if (base >= h->nlines) { h->cp[b] = tot; continue; }
            cnt = h->nlines - base; if (cnt > 64u) cnt = 64u;
            tot += mu_loom__sum_len(len + base, cnt);
            h->cp[b] = tot;
        }
        return;
    }
    h->cp[0] = 0u;
    for (j = 0u; j < h->nlines; ++j) {
        if (j == next) { h->cp[next >> 6] = run; next += 64u; }
        if (len[j] == 255u) { run += esc[e]; ++e; } else run += len[j];
    }
    /* Blocks past the last line hold the leaf total, so a checkpoint search
     * can never select a block that has no lines in it. */
    b = (h->nlines + 63u) >> 6;
    for (; b < 8u; ++b) h->cp[b] = run;
}

MU_INLINE void mu_loom__cp_rebuild(mu_loom *l, uint32_t slot)
{
    mu_loom_hdr *h = MU_LOOM_HDR(l, slot);
    h->cp[0] = 0u;
    mu_loom__cp_rebuild_from(l, slot, 0u);
}

/* Apply a length delta to every checkpoint AFTER the edited line's block.
 * Eight u32 = one AVX2 register, so this is a compare plus a masked add:
 * branchless, and it keeps typing O(1) in the line count. */
MU_INLINE void mu_loom__cp_add(mu_loom *l, uint32_t slot, uint32_t idx, int32_t delta)
{
    mu_loom_hdr *h = MU_LOOM_HDR(l, slot);
#if MU_LOOM_AVX512
    __m256i v   = _mm256_loadu_si256((const __m256i *)h->cp);
    __m256i lim = _mm256_set1_epi32((int)(idx >> 6));
    __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i m   = _mm256_cmpgt_epi32(lane, lim);
    __m256i add = _mm256_and_si256(m, _mm256_set1_epi32(delta));
    _mm256_storeu_si256((__m256i *)h->cp, _mm256_add_epi32(v, add));
#else
    uint32_t j;
    for (j = (idx >> 6) + 1u; j < 8u; ++j) h->cp[j] = (uint32_t)((int32_t)h->cp[j] + delta);
#endif
}

/* Number of escape entries strictly before line `n`. Branchless: 64 lengths
 * per AVX-512 compare, popcount the mask. Only used on escaped leaves. */
MU_INLINE uint32_t mu_loom__esc_before(const uint8_t *len, uint32_t n)
{
    uint32_t i = 0u, c = 0u;
#if MU_LOOM_AVX512
    for (; i + 64u <= n; i += 64u) {
        __m512i v = _mm512_loadu_si512((const void *)(len + i));
        __mmask64 m = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8((char)255));
        c += (uint32_t)mu_popcount_u64((uint64_t)m);
    }
#endif
    for (; i < n; ++i) c += (uint32_t)(len[i] == 255u);
    return c;
}

/* ---------------------------------------------------------------------------
 * Escape resolution. Called once per query, never per line in a loop, so the
 * branch is cold and the 99.95% leaf takes the pure-SIMD path above.
 * ------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_loom__esc_index(const uint8_t *len, uint32_t local)
{
    uint32_t i, k = 0u;
    for (i = 0u; i < local; ++i) k += (uint32_t)(len[i] == 255u);
    return k;
}

/* Byte offset of line `local` inside its leaf + that line's stored length
 * (INCLUDING its '\n' if present). */
MU_INLINE uint32_t mu_loom__leaf_off(const mu_loom *l, uint32_t slot, uint32_t local,
                                     uint32_t *out_len)
{
    const mu_loom_hdr *h   = MU_LOOM_HDR(l, slot);
    const uint8_t     *len = MU_LOOM_LEN(l, slot);
    uint32_t           off, k;
    const uint32_t    *esc;
    uint32_t           i;

    MU_LOOM_TOUCH(h);
    MU_LOOM_TOUCH(len);
    off = mu_loom__sum_len(len, local);
    if (MU_LIKELY(h->nlong == 0u)) { *out_len = len[local]; return off; }

    esc = MU_LOOM_LONG(l, slot);
    MU_LOOM_TOUCH(esc);
    k = 0u;
    for (i = 0u; i < local; ++i)
        if (len[i] == 255u) { off += esc[k] - 255u; ++k; }
    *out_len = (len[local] == 255u) ? esc[k] : len[local];
    return off;
}

/* Inverse: which line contains byte `off` within the leaf. */
/* Branchless search of up to 64 u8 lengths. Four independent 16-lane prefix
 * sums (no carry chain between them), then the group offsets are added and
 * each group is compared with a VALID-LANE mask. Because the prefixes are
 * monotone, the number of entries <= target IS the answer, so the four
 * popcounts just add. The looping version cost 71 ns because its exit
 * iteration is data-dependent and mispredicts; this has no branch at all. */
MU_INLINE uint32_t mu_loom__find64(const uint8_t *MU_RESTRICT p, uint32_t n, uint32_t target)
{
#if MU_LOOM_AVX512
    __mmask16 q0, q1, q2, q3;
    __m512i   p0, p1, p2, p3, tv;
    uint32_t  n0, n1, n2, n3, t0, t1, t2, g1, g2, g3;
    const __m512i tgt = _mm512_set1_epi32((int)target);

    n0 = n > 16u ? 16u : n;
    n1 = n > 32u ? 16u : (n > 16u ? n - 16u : 0u);
    n2 = n > 48u ? 16u : (n > 32u ? n - 32u : 0u);
    n3 = n > 48u ? n - 48u : 0u;
    q0 = (__mmask16)((1u << n0) - 1u);
    q1 = (__mmask16)((1u << n1) - 1u);
    q2 = (__mmask16)((1u << n2) - 1u);
    q3 = (__mmask16)((1u << n3) - 1u);

    p0 = mu_loom__prefix16(_mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(q0, (const void *)(p))));
    p1 = mu_loom__prefix16(_mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(q1, (const void *)(p + 16))));
    p2 = mu_loom__prefix16(_mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(q2, (const void *)(p + 32))));
    p3 = mu_loom__prefix16(_mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(q3, (const void *)(p + 48))));

    tv = _mm512_permutexvar_epi32(_mm512_set1_epi32(15), p0);
    t0 = (uint32_t)_mm512_cvtsi512_si32(tv);
    tv = _mm512_permutexvar_epi32(_mm512_set1_epi32(15), p1);
    t1 = (uint32_t)_mm512_cvtsi512_si32(tv);
    tv = _mm512_permutexvar_epi32(_mm512_set1_epi32(15), p2);
    t2 = (uint32_t)_mm512_cvtsi512_si32(tv);
    g1 = t0; g2 = t0 + t1; g3 = t0 + t1 + t2;
    p1 = _mm512_add_epi32(p1, _mm512_set1_epi32((int)g1));
    p2 = _mm512_add_epi32(p2, _mm512_set1_epi32((int)g2));
    p3 = _mm512_add_epi32(p3, _mm512_set1_epi32((int)g3));

    return (uint32_t)mu_popcount_u32((uint32_t)_mm512_mask_cmple_epu32_mask(q0, p0, tgt))
         + (uint32_t)mu_popcount_u32((uint32_t)_mm512_mask_cmple_epu32_mask(q1, p1, tgt))
         + (uint32_t)mu_popcount_u32((uint32_t)_mm512_mask_cmple_epu32_mask(q2, p2, tgt))
         + (uint32_t)mu_popcount_u32((uint32_t)_mm512_mask_cmple_epu32_mask(q3, p3, tgt));
#else
    uint32_t i, acc = 0u, c = 0u;
    for (i = 0u; i < n; ++i) { c += (uint32_t)(acc + p[i] <= target); acc += p[i]; }
    return c;
#endif
}

MU_INLINE uint32_t mu_loom__leaf_find(const mu_loom *l, uint32_t slot, uint32_t off,
                                      uint32_t *out_local_off)
{
    const mu_loom_hdr *h   = MU_LOOM_HDR(l, slot);
    const uint8_t     *len = MU_LOOM_LEN(l, slot);
    uint32_t           b, base, cnt, idx;

    MU_LOOM_TOUCH(h);
    /* Pick the 64-line block with 8 compares instead of scanning 512
     * lengths. cp is monotone, so this is a straight unrolled walk. */
    /* One compare + popcount over the 8 checkpoints. The scalar form is a
     * chain of 7 dependent adds; this is a single 256-bit compare. */
#if MU_LOOM_AVX512
    {
        __m256i v = _mm256_loadu_si256((const __m256i *)h->cp);
        __m256i m = _mm256_cmpgt_epi32(v, _mm256_set1_epi32((int)off));
        uint32_t gt = (uint32_t)mu_popcount_u32((uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(m)));
        /* cp[0] is always 0, so the count of entries <= off is 8-gt and the
         * selected block is one less than that. */
        b = 7u - gt;
    }
#else
    b = 0u;
    b += (uint32_t)(h->cp[0] <= off);
    b += (uint32_t)(h->cp[1] <= off);
    b += (uint32_t)(h->cp[2] <= off);
    b += (uint32_t)(h->cp[3] <= off);
    b += (uint32_t)(h->cp[4] <= off);
    b += (uint32_t)(h->cp[5] <= off);
    b += (uint32_t)(h->cp[6] <= off);
    b += (uint32_t)(h->cp[7] <= off);
    if (b > 0u) --b;
#endif
    if (h->nlines > 0u && b > ((h->nlines - 1u) >> 6)) b = (h->nlines - 1u) >> 6;
    base = b * 64u;
    cnt  = h->nlines - base;
    if (cnt > 64u) cnt = 64u;
    MU_LOOM_TOUCH(len + base);

    if (MU_LIKELY(h->nlong == 0u)) {
        idx = mu_loom__find64(len + base, cnt, off - h->cp[b]);
        *out_local_off = h->cp[b] + mu_loom__sum_len(len + base, idx);
        return base + idx;
    }
    {
        const uint32_t *esc = MU_LOOM_LONG(l, slot);
        uint32_t e = mu_loom__esc_before(len, base), i, run = h->cp[b];
        MU_LOOM_TOUCH(esc);
        for (i = 0u; i < cnt; ++i) {
            uint32_t v = (len[base + i] == 255u) ? esc[e++] : len[base + i];
            if (run + v > off) { *out_local_off = run; return base + i; }
            run += v;
        }
        *out_local_off = run;
        return h->nlines;
    }
}

/* ---------------------------------------------------------------------------
 * Index maintenance.
 *
 * THE PAYOFF OF RELATIVE AGGREGATES: an edit that changes one leaf's byte
 * count by db and line count by dl updates exactly SIX scalars — one pair
 * per level. No suffix, no SIMD add over a span, independent of document
 * size. mu_ted's absolute prefix arrays need ~40 suffix adds for the same
 * edit; a line array needs O(L).
 * ------------------------------------------------------------------------- */

/* Count entries <= target in an ABSOLUTE cumulative array of n <= 16 u32.
 * Lanes past n are filled with UINT32_MAX so they can never compare true:
 * that is what removes the bounds branch from the hot descent. */
MU_INLINE uint32_t mu_loom__search_abs(const uint32_t *MU_RESTRICT a, uint32_t n, uint32_t target)
{
    uint32_t i = 0u, c = 0u;
#if MU_LOOM_AVX512
    /* The array is monotone, so the first partial block ends the search.
     * n is NOT bounded by 16: a newline-dense document reaches 22 supers at
     * 8 MiB and would silently search only the first 16 otherwise. */
    for (; i + 16u <= n; i += 16u) {
        __m512i   v = _mm512_loadu_si512((const void *)(a + i));
        __mmask16 k = _mm512_cmple_epu32_mask(v, _mm512_set1_epi32((int)target));
        uint32_t  pc = (uint32_t)mu_popcount_u32((uint32_t)k);
        MU_LOOM_TOUCH(a + i);
        c += pc;
        if (pc < 16u) return c;
    }
    if (i < n) {
        __mmask16 m = (__mmask16)((1u << (n - i)) - 1u);
        __m512i   v = _mm512_mask_loadu_epi32(_mm512_set1_epi32(-1), m, (const void *)(a + i));
        __mmask16 k = _mm512_cmple_epu32_mask(v, _mm512_set1_epi32((int)target));
        c += (uint32_t)mu_popcount_u32((uint32_t)k);
    }
    return c;
#else
    for (; i < n; ++i) c += (uint32_t)(a[i] <= target);
    return c;
#endif
}

/* Add delta to a[from .. n). Unit stride, so this is a pure SIMD stream with
 * no gather, no branch per element, and a scalar tail of at most 15. */
MU_INLINE void mu_loom__suffix_add(uint32_t *MU_RESTRICT a, uint32_t n, uint32_t from, int32_t delta)
{
    uint32_t i = from;
    if (delta == 0) return;
#if MU_LOOM_AVX512
    {
        const __m512i d = _mm512_set1_epi32(delta);
        for (; i + 16u <= n; i += 16u)
            _mm512_storeu_si512((void *)(a + i),
                _mm512_add_epi32(_mm512_loadu_si512((const void *)(a + i)), d));
        if (i < n) {
            __mmask16 m = (__mmask16)((1u << (n - i)) - 1u);
            _mm512_mask_storeu_epi32((void *)(a + i), m,
                _mm512_add_epi32(_mm512_maskz_loadu_epi32(m, (const void *)(a + i)), d));
            i = n;
        }
    }
#endif
    for (; i < n; ++i) a[i] = (uint32_t)((int32_t)a[i] + delta);
}

MU_INLINE void mu_loom__touch(mu_loom *l, uint32_t i, int32_t db, int32_t dl)
{
    uint32_t g = i / MU_LOOM_GROUP;
    uint32_t s = g / MU_LOOM_SUPER;

    l->leaf_bytes[i] = (uint32_t)((int64_t)l->leaf_bytes[i] + db);
    l->leaf_lines[i] = (uint32_t)((int64_t)l->leaf_lines[i] + dl);
    l->grp_bytes[g]  = (uint32_t)((int64_t)l->grp_bytes[g] + db);
    l->grp_lines[g]  = (uint32_t)((int64_t)l->grp_lines[g] + dl);
    l->sup_bytes[s]  = (uint32_t)((int64_t)l->sup_bytes[s] + db);
    l->sup_lines[s]  = (uint32_t)((int64_t)l->sup_lines[s] + dl);
    mu_loom__suffix_add(l->leaf_bend, l->nleaf, i, db);
    mu_loom__suffix_add(l->leaf_lend, l->nleaf, i, dl);
    mu_loom__suffix_add(l->grp_bend,  l->ngrp,  g, db);
    mu_loom__suffix_add(l->grp_lend,  l->ngrp,  g, dl);
    mu_loom__suffix_add(l->sup_bend,  l->nsup,  s, db);
    mu_loom__suffix_add(l->sup_lend,  l->nsup,  s, dl);
    l->stat_index_stores += 6u;
}

MU_INLINE void mu_loom__invalidate(mu_loom *l)
{
    l->cur_valid = 0u;
    l->lc_valid  = 0u;
}

/* Rebuild every group and super sum. Pure sequential SIMD reductions, so
 * although this is O(nleaf) it runs at ~16 elements per few instructions —
 * an order of magnitude cheaper per element than a Fenwick's scattered
 * i += i&-i writes. Called only on a structural change (leaf split/merge). */
MU_INLINE void mu_loom__reindex(mu_loom *l)
{
    uint32_t g, s, i, end;
    uint32_t b, n;

    l->ngrp = (l->nleaf + MU_LOOM_GROUP - 1u) / MU_LOOM_GROUP;
    l->nsup = (l->ngrp + MU_LOOM_SUPER - 1u) / MU_LOOM_SUPER;
    if (l->ngrp == 0u) l->ngrp = 1u;
    if (l->nsup == 0u) l->nsup = 1u;
    for (g = 0u; g < l->ngrp; ++g) {
        end = (g + 1u) * MU_LOOM_GROUP;
        if (end > l->nleaf) end = l->nleaf;
        b = 0u; n = 0u;
        for (i = g * MU_LOOM_GROUP; i < end; ++i) { b += l->leaf_bytes[i]; n += l->leaf_lines[i]; }
        l->grp_bytes[g] = b;
        l->grp_lines[g] = n;
    }
    for (s = 0u; s < l->nsup; ++s) {
        end = (s + 1u) * MU_LOOM_SUPER;
        if (end > l->ngrp) end = l->ngrp;
        b = 0u; n = 0u;
        for (i = s * MU_LOOM_SUPER; i < end; ++i) { b += l->grp_bytes[i]; n += l->grp_lines[i]; }
        l->sup_bytes[s] = b;
        l->sup_lines[s] = n;
    }
    /* absolute mirrors: three unit-stride running sums */
    {
        uint32_t rb = 0u, rn = 0u;
        for (i = 0u; i < l->nleaf; ++i) {
            rb += l->leaf_bytes[i]; rn += l->leaf_lines[i];
            l->leaf_bend[i] = rb; l->leaf_lend[i] = rn;
        }
        rb = 0u; rn = 0u;
        for (g = 0u; g < l->ngrp; ++g) {
            rb += l->grp_bytes[g]; rn += l->grp_lines[g];
            l->grp_bend[g] = rb; l->grp_lend[g] = rn;
        }
        rb = 0u; rn = 0u;
        for (s = 0u; s < l->nsup; ++s) {
            rb += l->sup_bytes[s]; rn += l->sup_lines[s];
            l->sup_bend[s] = rb; l->sup_lend[s] = rn;
        }
    }
    mu_loom__invalidate(l);
}

/* ---------------------------------------------------------------------------
 * Descent: three levels, one walk, both coordinates.
 * `want_line` selects which aggregate is the search key; the other is
 * accumulated for free from the same cache lines.
 * ------------------------------------------------------------------------- */

MU_INLINE void mu_loom__descend(mu_loom *l, uint32_t target, int want_line,
                                mu_loom_pos *out)
{
    uint32_t s, g, i, n, kb, ab, ns;
    const uint32_t *key, *acc;

    /* cursor: typing and viewport walks stay inside one leaf */
    if (l->cur_valid) {
        uint32_t span = want_line ? l->leaf_lines[l->cur_leaf] : l->leaf_bytes[l->cur_leaf];
        uint32_t base = want_line ? l->cur_line_base : l->cur_byte_base;
        if (target >= base && target - base < span) {
            out->leaf      = l->cur_leaf;
            out->local     = target - base;
            out->byte_base = l->cur_byte_base;
            out->line_base = l->cur_line_base;
            ++l->stat_cur_hits;
            return;
        }
    }
    ++l->stat_descents;

    /* The end-of-document byte position is legal (append, hit-test at EOF)
     * but is NOT inside any leaf's half-open span. Resolve it directly. */
    if (!want_line && target >= l->text_len) {
        i = l->nleaf - 1u;
        out->leaf      = i;
        out->local     = l->leaf_bytes[i];
        out->byte_base = l->text_len - l->leaf_bytes[i];
        out->line_base = l->nlines - l->leaf_lines[i];
        l->cur_valid     = 1u;
        l->cur_leaf      = i;
        l->cur_byte_base = out->byte_base;
        l->cur_line_base = out->line_base;
        return;
    }

    /* Three levels, each a compare + popcount on ABSOLUTE ends. The other
     * coordinate is one load, not a reduction, so offset->(line,col) costs
     * the same as a single-coordinate query. */
    key = want_line ? l->sup_lend : l->sup_bend;
    acc = want_line ? l->sup_bend : l->sup_lend;
    ns  = mu_loom__search_abs(key, l->nsup, target);
    s   = (ns >= l->nsup) ? l->nsup - 1u : ns;
    kb  = s ? key[s - 1u] : 0u;
    ab  = s ? acc[s - 1u] : 0u;

    key = (want_line ? l->grp_lend : l->grp_bend) + s * MU_LOOM_SUPER;
    acc = (want_line ? l->grp_bend : l->grp_lend) + s * MU_LOOM_SUPER;
    n   = l->ngrp - s * MU_LOOM_SUPER;
    if (n > MU_LOOM_SUPER) n = MU_LOOM_SUPER;
    g   = mu_loom__search_abs(key, n, target);
    if (g >= n) g = n - 1u;
    i   = s * MU_LOOM_SUPER + g;
    /* i is the global group index; read the global arrays so the g == 0
     * case naturally falls back to the previous super's end. */
    kb  = i ? (want_line ? l->grp_lend[i - 1u] : l->grp_bend[i - 1u]) : 0u;
    ab  = i ? (want_line ? l->grp_bend[i - 1u] : l->grp_lend[i - 1u]) : 0u;

    key = (want_line ? l->leaf_lend : l->leaf_bend) + i * MU_LOOM_GROUP;
    acc = (want_line ? l->leaf_bend : l->leaf_lend) + i * MU_LOOM_GROUP;
    n   = l->nleaf - i * MU_LOOM_GROUP;
    if (n > MU_LOOM_GROUP) n = MU_LOOM_GROUP;
    g   = mu_loom__search_abs(key, n, target);
    if (g >= n) g = n - 1u;
    i   = i * MU_LOOM_GROUP + g;
    kb  = i ? (want_line ? l->leaf_lend[i - 1u] : l->leaf_bend[i - 1u]) : 0u;
    ab  = i ? (want_line ? l->leaf_bend[i - 1u] : l->leaf_lend[i - 1u]) : 0u;

    out->leaf      = i;
    out->local     = target - kb;
    out->byte_base = want_line ? ab : kb;
    out->line_base = want_line ? kb : ab;
    l->cur_valid     = 1u;
    l->cur_leaf      = i;
    l->cur_byte_base = out->byte_base;
    l->cur_line_base = out->line_base;
}

/* ---------------------------------------------------------------------------
 * Slot pool. Free slots are an inactive list (existence, not a dead flag).
 * ------------------------------------------------------------------------- */

MU_INLINE bool mu_loom__grow_slots(mu_loom *l, uint32_t need)
{
    uint32_t cap;
    uint8_t *na;

    if (need <= l->slot_cap) return true;
    cap = l->slot_cap ? l->slot_cap : 16u;
    while (cap < need) cap *= 2u;
    na = (uint8_t *)realloc(l->arena, (size_t)cap * MU_LOOM_SLOT_BYTES);
    if (!na) return false;
    l->arena    = na;
    l->slot_cap = cap;
    return true;
}

MU_INLINE uint32_t mu_loom__slot_alloc(mu_loom *l)
{
    uint32_t     h;
    mu_loom_hdr *hd;

    if (l->free_head != UINT32_MAX) {
        h            = l->free_head;
        l->free_head = MU_LOOM_HDR(l, h)->free_next;
    } else {
        if (!mu_loom__grow_slots(l, l->slot_high + 1u)) return UINT32_MAX;
        h = l->slot_high++;
    }
    hd            = MU_LOOM_HDR(l, h);
    hd->nbytes    = 0u;
    hd->nlines    = 0u;
    hd->nlong     = 0u;
    hd->free_next = UINT32_MAX;
    return h;
}

MU_INLINE void mu_loom__slot_free(mu_loom *l, uint32_t h)
{
    MU_LOOM_HDR(l, h)->free_next = l->free_head;
    l->free_head                 = h;
}

MU_INLINE bool mu_loom__grow_dir(mu_loom *l, uint32_t need)
{
    uint32_t cap, gcap, scap;
    uint32_t *nb, *nl, *ns;

    if (need <= l->leaf_cap) return true;
    cap = l->leaf_cap ? l->leaf_cap : 64u;
    while (cap < need) cap *= 2u;
    nb = (uint32_t *)realloc(l->leaf_bytes, (size_t)cap * 4u);
    if (!nb) return false;
    l->leaf_bytes = nb;
    nl = (uint32_t *)realloc(l->leaf_lines, (size_t)cap * 4u);
    if (!nl) return false;
    l->leaf_lines = nl;
    ns = (uint32_t *)realloc(l->leaf_slot, (size_t)cap * 4u);
    if (!ns) return false;
    l->leaf_slot = ns;
    nb = (uint32_t *)realloc(l->leaf_bend, (size_t)cap * 4u);
    if (!nb) return false;
    l->leaf_bend = nb;
    nl = (uint32_t *)realloc(l->leaf_lend, (size_t)cap * 4u);
    if (!nl) return false;
    l->leaf_lend = nl;
    l->leaf_cap  = cap;

    gcap = (cap + MU_LOOM_GROUP - 1u) / MU_LOOM_GROUP + 1u;
    scap = (gcap + MU_LOOM_SUPER - 1u) / MU_LOOM_SUPER + 1u;
    if (gcap > l->grp_cap) {
        nb = (uint32_t *)realloc(l->grp_bytes, (size_t)gcap * 4u);
        if (!nb) return false;
        l->grp_bytes = nb;
        nl = (uint32_t *)realloc(l->grp_lines, (size_t)gcap * 4u);
        if (!nl) return false;
        l->grp_lines = nl;
        nb = (uint32_t *)realloc(l->grp_bend, (size_t)gcap * 4u);
        if (!nb) return false;
        l->grp_bend = nb;
        nl = (uint32_t *)realloc(l->grp_lend, (size_t)gcap * 4u);
        if (!nl) return false;
        l->grp_lend = nl;
        l->grp_cap   = gcap;
    }
    if (scap > l->sup_cap) {
        nb = (uint32_t *)realloc(l->sup_bytes, (size_t)scap * 4u);
        if (!nb) return false;
        l->sup_bytes = nb;
        nl = (uint32_t *)realloc(l->sup_lines, (size_t)scap * 4u);
        if (!nl) return false;
        l->sup_lines = nl;
        nb = (uint32_t *)realloc(l->sup_bend, (size_t)scap * 4u);
        if (!nb) return false;
        l->sup_bend = nb;
        nl = (uint32_t *)realloc(l->sup_lend, (size_t)scap * 4u);
        if (!nl) return false;
        l->sup_lend = nl;
        l->sup_cap   = scap;
    }
    return true;
}

/* Splice one leaf into the document-order directory. O(nleaf) memmove of
 * three u32 streams + one vectorised reindex; paid once per ~8 KB of typing. */
MU_INLINE bool mu_loom__dir_insert(mu_loom *l, uint32_t at, uint32_t slot,
                                   uint32_t nbytes, uint32_t nlines)
{
    if (!mu_loom__grow_dir(l, l->nleaf + 1u)) return false;
    memmove(l->leaf_bytes + at + 1u, l->leaf_bytes + at, (size_t)(l->nleaf - at) * 4u);
    memmove(l->leaf_lines + at + 1u, l->leaf_lines + at, (size_t)(l->nleaf - at) * 4u);
    memmove(l->leaf_slot  + at + 1u, l->leaf_slot  + at, (size_t)(l->nleaf - at) * 4u);
    l->leaf_bytes[at] = nbytes;
    l->leaf_lines[at] = nlines;
    l->leaf_slot[at]  = slot;
    ++l->nleaf;
    mu_loom__reindex(l);
    return true;
}

MU_INLINE void mu_loom__dir_remove(mu_loom *l, uint32_t at)
{
    mu_loom__slot_free(l, l->leaf_slot[at]);
    memmove(l->leaf_bytes + at, l->leaf_bytes + at + 1u, (size_t)(l->nleaf - at - 1u) * 4u);
    memmove(l->leaf_lines + at, l->leaf_lines + at + 1u, (size_t)(l->nleaf - at - 1u) * 4u);
    memmove(l->leaf_slot  + at, l->leaf_slot  + at + 1u, (size_t)(l->nleaf - at - 1u) * 4u);
    --l->nleaf;
    mu_loom__reindex(l);
}

/* ---------------------------------------------------------------------------
 * Line scanning. AVX-512 compares 64 bytes per instruction; the mask's set
 * bits ARE the newline positions, so lengths come out of a TZCNT loop over
 * at most (lines in 64 bytes) iterations — not a branch per byte.
 * ------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_loom__count_nl(const uint8_t *p, uint32_t n)
{
    uint32_t i = 0u, total = 0u;
#if MU_LOOM_AVX512
    const __m512i nl = _mm512_set1_epi8('\n');
    __mmask64     m;
    for (; i + 64u <= n; i += 64u) {
        m = _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void *)(p + i)), nl);
        total += (uint32_t)mu_popcount_u64((uint64_t)m);
    }
    if (i < n) {
        m = _mm512_cmpeq_epi8_mask(
            _mm512_maskz_loadu_epi8((__mmask64)((1ull << (n - i)) - 1ull), (const void *)(p + i)), nl);
        total += (uint32_t)mu_popcount_u64((uint64_t)m);
    }
#else
    for (; i < n; ++i) total += (uint32_t)(p[i] == '\n');
#endif
    return total;
}

/* Position of the first '\n' at or after `from` within n bytes, or n. */
MU_INLINE uint32_t mu_loom__next_nl(const uint8_t *p, uint32_t n, uint32_t from)
{
    uint32_t i = from;
#if MU_LOOM_AVX512
    const __m512i nl = _mm512_set1_epi8('\n');
    __mmask64     m;
    for (; i + 64u <= n; i += 64u) {
        m = _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void *)(p + i)), nl);
        if (m) return i + (uint32_t)mu_trailing_zeroes_u64((uint64_t)m);
    }
    if (i < n) {
        m = _mm512_cmpeq_epi8_mask(
            _mm512_maskz_loadu_epi8((__mmask64)((1ull << (n - i)) - 1ull), (const void *)(p + i)), nl);
        if (m) return i + (uint32_t)mu_trailing_zeroes_u64((uint64_t)m);
    }
#else
    for (; i < n; ++i) if (p[i] == '\n') return i;
#endif
    return n;
}

/* Last '\n' strictly before `n` bytes, or UINT32_MAX. */
MU_INLINE uint32_t mu_loom__prev_nl(const uint8_t *p, uint32_t n)
{
    uint32_t i = n;
#if MU_LOOM_AVX512
    const __m512i nl = _mm512_set1_epi8('\n');
    __mmask64     m;
    while (i >= 64u) {
        i -= 64u;
        m = _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void *)(p + i)), nl);
        if (m) return i + 63u - (uint32_t)mu_leading_zeroes_u64((uint64_t)m);
    }
    if (i > 0u) {
        m = _mm512_cmpeq_epi8_mask(
            _mm512_maskz_loadu_epi8((__mmask64)((1ull << i) - 1ull), (const void *)p), nl);
        if (m) return 63u - (uint32_t)mu_leading_zeroes_u64((uint64_t)m);
    }
#else
    while (i > 0u) { --i; if (p[i] == '\n') return i; }
#endif
    return UINT32_MAX;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

MU_INLINE bool mu_loom_init(mu_loom *l, size_t reserve_bytes)
{
    uint32_t want;

    memset(l, 0, sizeof *l);
    l->free_head = UINT32_MAX;
    l->nlines    = 1u;
    want = (uint32_t)(reserve_bytes / MU_LOOM_SLOT_BYTES) + 2u;
    if (want < 4u) want = 4u;
    if (!mu_loom__grow_slots(l, want)) return false;
    if (!mu_loom__grow_dir(l, 64u)) { free(l->arena); l->arena = NULL; return false; }
    /* an empty document is one empty leaf holding one empty line */
    {
        uint32_t h = mu_loom__slot_alloc(l);
        if (h == UINT32_MAX) return false;
        MU_LOOM_LEN(l, h)[0] = 0u;
        MU_LOOM_HDR(l, h)->nlines = 1u;
        l->leaf_slot[0]  = h;
        l->leaf_bytes[0] = 0u;
        l->leaf_lines[0] = 1u;
        l->nleaf = 1u;
        mu_loom__reindex(l);
    }
    return true;
}

MU_INLINE void mu_loom_destroy(mu_loom *l)
{
    free(l->scratch);
    free(l->arena);
    free(l->leaf_bend);
    free(l->leaf_lend);
    free(l->grp_bend);
    free(l->grp_lend);
    free(l->sup_bend);
    free(l->sup_lend);
    free(l->leaf_bytes);
    free(l->leaf_lines);
    free(l->leaf_slot);
    free(l->grp_bytes);
    free(l->grp_lines);
    free(l->sup_bytes);
    free(l->sup_lines);
    memset(l, 0, sizeof *l);
    l->free_head = UINT32_MAX;
}

MU_INLINE uint32_t mu_loom_text_len(const mu_loom *l) { return l->text_len; }
MU_INLINE uint32_t mu_loom_line_count(const mu_loom *l) { return l->nlines; }
MU_INLINE uint32_t mu_loom_leaf_count(const mu_loom *l) { return l->nleaf; }

MU_INLINE size_t mu_loom_mem(const mu_loom *l)
{
    return ((size_t)l->slot_cap * MU_LOOM_SLOT_BYTES)
         + (size_t)l->leaf_cap * 12u
         + (size_t)l->grp_cap * 8u
         + (size_t)l->sup_cap * 8u;
}

MU_INLINE size_t mu_loom_live_bytes(const mu_loom *l)
{
    return (size_t)l->text_len + (size_t)l->nlines  /* 1 B per line */
         + (size_t)l->nleaf * 64u                  /* headers */
         + (size_t)l->nleaf * 12u + (size_t)l->ngrp * 8u + (size_t)l->nsup * 8u;
}

/* ---------------------------------------------------------------------------
 * Bulk load. Plans leaves from a contiguous source, cutting at newline
 * boundaries, then builds the index once.
 * ------------------------------------------------------------------------- */

/* Record one length, using the escape table when it does not fit u8.
 * Returns false when the leaf's escape table is full. */
MU_INLINE bool mu_loom__put_len(mu_loom *l, uint32_t slot, uint32_t idx, uint32_t length)
{
    mu_loom_hdr *h = MU_LOOM_HDR(l, slot);
    uint8_t     *len = MU_LOOM_LEN(l, slot);

    if (length < 255u) { len[idx] = (uint8_t)length; return true; }
    if (h->nlong >= MU_LOOM_MAX_LONG) return false;
    len[idx] = 255u;
    MU_LOOM_LONG(l, slot)[h->nlong++] = length;
    return true;
}

/* Fill leaf `slot` from d[0..avail). Returns bytes consumed.
 * `is_tail` = this is the end of the document, so a trailing partial line
 * belongs here. Cuts at a newline when it can, so op 1 stays one descent. */
MU_INLINE uint32_t mu_loom__fill(mu_loom *l, uint32_t slot, const uint8_t *d,
                                 uint32_t avail, int at_line_start);

/* Fill one leaf from `d`. The line split is a BULK bitmask scan: one AVX-512
 * compare produces 64 newline bits, and each line costs a tzcnt, a subtract,
 * a store and a blsr. Calling a find-newline helper per line instead costs
 * ~26 cycles per line (measured 4480 ns per 16 KiB leaf); this is ~4.
 * The checkpoint directory is accumulated in the same pass, so it is free. */
MU_INLINE uint32_t mu_loom__fill_lim(mu_loom *l, uint32_t slot, const uint8_t *d,
                                     uint32_t avail, uint32_t limit, int at_line_start)
{
    uint32_t     take, i, prev, nlines, length, tail, run, e, nextcp;
    uint8_t     *txt = MU_LOOM_TEXT(l, slot);
    uint8_t     *len = MU_LOOM_LEN(l, slot);
    mu_loom_hdr *h   = MU_LOOM_HDR(l, slot);

    h->nlong = 0u;
    h->lead  = 0u;
    take = avail < limit ? avail : limit;
    if (take > MU_LOOM_TEXT_CAP) take = MU_LOOM_TEXT_CAP;
    if (take == 0u) {
        h->nbytes = 0u; h->nlines = 1u; len[0] = 0u;
        mu_loom__cp_rebuild(l, slot);
        return 0u;
    }
    memcpy(txt, d, take);
    tail = (take == avail);
    prev = 0u; i = 0u; nlines = 0u;

    if (!at_line_start) {
        /* Opens mid-line: everything up to the first newline belongs to a
         * line recorded in an EARLIER leaf. That is how a >16 KiB line
         * spans leaves without being counted twice. */
        uint32_t end = mu_loom__next_nl(txt, take, 0u);
        if (end == take) {
            h->nbytes = take; h->nlines = 0u; h->lead = take;
            mu_loom__cp_rebuild(l, slot);
            return take;
        }
        prev = end + 1u; i = prev;
        h->lead = prev;
    }

    run = 0u; e = 0u; nextcp = 64u;
    h->cp[0] = 0u;

    while (i < take) {
        uint32_t blk = take - i;
        uint64_t m;
        if (blk > 64u) blk = 64u;
#if MU_LOOM_AVX512
        {
            __mmask64 lm = (blk == 64u) ? (__mmask64)~0ull : (__mmask64)((1ull << blk) - 1ull);
            __m512i   v  = _mm512_maskz_loadu_epi8(lm, (const void *)(txt + i));
            m = (uint64_t)_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\n'));
        }
#else
        {
            uint32_t q;
            m = 0ull;
            for (q = 0u; q < blk; ++q) if (txt[i + q] == (uint8_t)'\n') m |= (1ull << q);
        }
#endif
        while (m) {
            uint32_t end = i + (uint32_t)mu_trailing_zeroes_u64(m);
            if (nlines >= MU_LOOM_MAX_LINES) { take = prev; goto done; }
            length = end - prev + 1u;
            if (MU_LIKELY(length < 255u)) {
                len[nlines] = (uint8_t)length;
            } else {
                if (h->nlong >= MU_LOOM_MAX_LONG) { take = prev; goto done; }
                MU_LOOM_LONG(l, slot)[h->nlong++] = length;
                len[nlines] = 255u;
            }
            if (nlines + 1u == nextcp && nextcp < 512u) { h->cp[nextcp >> 6] = run + length; }
            if (nlines + 1u == nextcp) nextcp += 64u;
            run += length;
            ++nlines;
            prev = end + 1u;
            m &= m - 1u;
        }
        i += blk;
    }
    /* no newline left in the chunk */
    if (prev < take) {
        if (tail) {
            length = take - prev;
            if (nlines >= MU_LOOM_MAX_LINES) { take = prev; goto done; }
            if (length < 255u) {
                len[nlines] = (uint8_t)length;
            } else {
                if (h->nlong >= MU_LOOM_MAX_LONG) { take = prev; goto done; }
                MU_LOOM_LONG(l, slot)[h->nlong++] = length;
                len[nlines] = 255u;
            }
            if (nlines + 1u == nextcp && nextcp < 512u) { h->cp[nextcp >> 6] = run + length; }
            if (nlines + 1u == nextcp) nextcp += 64u;
            run += length;
            ++nlines;
        } else if (prev > 0u) {
            take = prev;                     /* hand the partial line on */
        } else {
            /* One line longer than a whole leaf: record its TRUE length
             * here once; the continuations carry nlines == 0. */
            uint32_t nl = mu_loom__next_nl(d, avail, take);
            length = (nl < avail) ? nl + 1u : avail;
            MU_LOOM_LONG(l, slot)[h->nlong++] = length;
            len[0] = 255u;
            run = length;
            nlines = 1u;
            if (nextcp == 64u) { /* cp[1..] get the total below */ }
        }
    }
done:
    h->nbytes = take;
    h->nlines = nlines;
    {
        uint32_t b = (nlines + 63u) >> 6;
        for (; b < 8u; ++b) h->cp[b] = run;
    }
    (void)e;
    return take;
}

MU_INLINE uint32_t mu_loom__fill(mu_loom *l, uint32_t slot, const uint8_t *d,
                                 uint32_t avail, int at_line_start)
{
    return mu_loom__fill_lim(l, slot, d, avail, MU_LOOM_TEXT_CAP, at_line_start);
}

MU_INLINE bool mu_loom_load(mu_loom *l, const uint8_t *data, size_t n)
{
    uint32_t off, slot, used, total_lines;

    if (n > 0xFFFFFFFFu) return false;
    /* reset to empty, keeping allocations */
    l->nleaf = 0u;
    l->slot_high = 0u;
    l->free_head = UINT32_MAX;
    l->text_len = 0u;
    l->nlines = 1u;
    mu_loom__invalidate(l);

    if (n == 0u) {
        slot = mu_loom__slot_alloc(l);
        if (slot == UINT32_MAX) return false;
        MU_LOOM_LEN(l, slot)[0] = 0u;
        MU_LOOM_HDR(l, slot)->nlines = 1u;
        MU_LOOM_HDR(l, slot)->nbytes = 0u;
        MU_LOOM_HDR(l, slot)->nlong  = 0u;
        MU_LOOM_HDR(l, slot)->lead   = 0u;
        if (!mu_loom__grow_dir(l, 1u)) return false;
        l->leaf_slot[0] = slot; l->leaf_bytes[0] = 0u; l->leaf_lines[0] = 1u;
        l->nleaf = 1u;
        mu_loom__reindex(l);
        return true;
    }

    off = 0u; total_lines = 0u;
    while (off < (uint32_t)n) {
        slot = mu_loom__slot_alloc(l);
        if (slot == UINT32_MAX) return false;
        used = mu_loom__fill(l, slot, data + off, (uint32_t)n - off,
                             off == 0u || data[off - 1u] == (uint8_t)'\n');
        if (used == 0u) { mu_loom__slot_free(l, slot); break; }
        if (!mu_loom__grow_dir(l, l->nleaf + 1u)) return false;
        l->leaf_slot[l->nleaf]  = slot;
        l->leaf_bytes[l->nleaf] = used;
        l->leaf_lines[l->nleaf] = MU_LOOM_HDR(l, slot)->nlines;
        total_lines += MU_LOOM_HDR(l, slot)->nlines;
        ++l->nleaf;
        off += used;
    }
    l->text_len = (uint32_t)n;
    /* lines = newlines + 1; a trailing '\n' opens one empty line */
    l->nlines = mu_loom__count_nl(data, (uint32_t)n) + 1u;
    if (total_lines < l->nlines) {
        /* append the trailing empty line to the last leaf */
        uint32_t last = l->leaf_slot[l->nleaf - 1u];
        mu_loom_hdr *h = MU_LOOM_HDR(l, last);
        if (h->nlines < MU_LOOM_MAX_LINES && mu_loom__put_len(l, last, h->nlines, 0u)) {
            ++h->nlines;
            ++l->leaf_lines[l->nleaf - 1u];
        } else {
            slot = mu_loom__slot_alloc(l);
            if (slot == UINT32_MAX) return false;
            MU_LOOM_LEN(l, slot)[0] = 0u;
            MU_LOOM_HDR(l, slot)->nlines = 1u;
            MU_LOOM_HDR(l, slot)->nbytes = 0u;
            MU_LOOM_HDR(l, slot)->nlong  = 0u;
            MU_LOOM_HDR(l, slot)->lead   = 0u;
            if (!mu_loom__grow_dir(l, l->nleaf + 1u)) return false;
            l->leaf_slot[l->nleaf] = slot;
            l->leaf_bytes[l->nleaf] = 0u;
            l->leaf_lines[l->nleaf] = 1u;
            ++l->nleaf;
        }
    }
    mu_loom__reindex(l);
    return true;
}

/* ---------------------------------------------------------------------------
 * QUERIES
 *
 * Length semantics: stored len[i] INCLUDES the '\n', except for the very
 * last line of the document which has none. Therefore sum(len) == text_len
 * exactly, and the reported length is stored - (line is not last).
 * ------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_loom__stored_len(const mu_loom *l, uint32_t slot, uint32_t local)
{
    const uint8_t *len = MU_LOOM_LEN(l, slot);
    uint32_t       v = len[local];
    if (MU_LIKELY(v != 255u)) return v;
    return MU_LOOM_LONG(l, slot)[mu_loom__esc_index(len, local)];
}

/* Byte offset of local line `idx` within its leaf: one VPSADBW reduction over
 * the u8 lengths, plus the escapes that precede it. This is the only place a
 * line index is turned into a byte position inside a leaf. */
MU_INLINE uint32_t mu_loom__leaf_prefix(const mu_loom *l, uint32_t slot, uint32_t idx)
{
    const mu_loom_hdr *h    = MU_LOOM_HDR(l, slot);
    const uint8_t     *len  = MU_LOOM_LEN(l, slot);
    uint32_t           base = idx & ~63u, off = idx & 63u, sum, j;

    sum = h->lead + h->cp[idx >> 6];
    if (MU_LIKELY(h->nlong == 0u))
        return sum + mu_loom__sum_len(len + base, off);
    /* Escaped leaf: the u8 sum counts 255 per escape, so correct only the
     * escapes inside this 64-line block, not the whole leaf. */
    sum += mu_loom__sum_len(len + base, off);
    {
        const uint32_t *esc = MU_LOOM_LONG(l, slot);
        uint32_t e = mu_loom__esc_before(len, base);
        for (j = 0u; j < off; ++j) {
            if (len[base + j] == 255u) { sum += esc[e] - 255u; ++e; }
        }
    }
    return sum;
}

/* Op 1. The line cursor caches (line, byte offset, leaf, local index) of the
 * NEXT line to read, so a sequential walk pays one u8 load per line and never
 * re-descends. It stores an OFFSET, not a length, so it stays exact across
 * leaf boundaries and across edits that invalidate it. */
MU_INLINE mu_loom_line mu_loom_line_range(mu_loom *l, uint32_t line)
{
    mu_loom_line r;
    mu_loom_pos  p;
    uint32_t     stored, nl, exact, k;

    r.offset = 0u;
    r.len    = 0u;
    if (line >= l->nlines) return r;

    if (l->lc_valid && line == l->lc_line) {
        /* lc_off is the ABSOLUTE offset of this line, lead included. It must
         * not have the intra-leaf prefix added a second time. */
        p.leaf  = l->lc_leaf;
        p.local = l->lc_local;
        exact   = l->lc_off;
        while (p.leaf < l->nleaf && l->leaf_lines[p.leaf] == 0u) { ++p.leaf; p.local = 0u; }
        if (p.leaf >= l->nleaf) { l->lc_valid = 0u; return r; }
    } else {
        mu_loom__descend(l, line, 1, &p);
        exact = p.byte_base + mu_loom__leaf_prefix(l, l->leaf_slot[p.leaf], p.local);
    }

    nl = l->leaf_lines[p.leaf];
    if (p.local >= nl) { l->lc_valid = 0u; return r; }
    stored   = mu_loom__stored_len(l, l->leaf_slot[p.leaf], p.local);
    r.offset = exact;
    r.len    = stored - ((line + 1u < l->nlines) ? 1u : 0u);

    l->lc_line  = line + 1u;
    l->lc_off   = exact + stored;
    l->lc_leaf  = p.leaf;
    l->lc_local = p.local + 1u;
    l->lc_valid = 1u;
    if (l->lc_local >= nl) {
        k = p.leaf + 1u;
        while (k < l->nleaf && l->leaf_lines[k] == 0u) ++k;
        if (k < l->nleaf) { l->lc_leaf = k; l->lc_local = 0u; }
        else l->lc_valid = 0u;
    }
    return r;
}

MU_INLINE uint32_t mu_loom_line_offset(mu_loom *l, uint32_t line)
{
    return mu_loom_line_range(l, line).offset;
}

/* Op 3: byte offset -> (line, col). One descent on the byte aggregate,
 * accumulating the line aggregate from the same cache lines. */
MU_INLINE mu_loom_cursor mu_loom_offset_to_cursor(mu_loom *l, uint32_t offset)
{
    mu_loom_cursor c;
    mu_loom_pos    p;
    mu_loom_hdr   *h;
    uint32_t       local, before, slot, rel;

    c.offset = offset;
    c.line = 0u; c.col = 0u;
    if (offset > l->text_len) offset = l->text_len;
    if (l->nleaf == 0u) return c;

    mu_loom__descend(l, offset, 0, &p);
    slot = l->leaf_slot[p.leaf];
    h    = MU_LOOM_HDR(l, slot);

    if (h->nlines == 0u || p.local < h->lead) {
        /* The offset lands inside a line that STARTS in an earlier leaf.
         * Walk back until a leaf's LAST recorded line starts at or before
         * the offset. Testing the actual start (rather than just nlines==0)
         * is what makes this correct for a leaf that has both a lead region
         * and line starts of its own. */
        uint32_t j = p.leaf, b = p.byte_base, lbase = p.line_base;
        uint32_t own = 0u, st = 0u;
        for (;;) {
            if (l->leaf_lines[j] > 0u) {
                own = l->leaf_lines[j] - 1u;
                st  = b + mu_loom__leaf_prefix(l, l->leaf_slot[j], own);
                if (st <= offset) break;
            }
            if (j == 0u) break;
            --j;
            b     -= l->leaf_bytes[j];
            lbase -= l->leaf_lines[j];
        }
        c.line = lbase + own;
        c.col  = offset - st;
        if (c.line >= l->nlines) c.line = l->nlines - 1u;
        return c;
    }
    rel    = p.local - h->lead;
    local  = mu_loom__leaf_find(l, slot, rel, &before);
    if (local >= h->nlines) { local = h->nlines - 1u; before = mu_loom__leaf_prefix(l, slot, local) - h->lead; }
    c.line = p.line_base + local;
    c.col  = rel - before;
    if (c.line >= l->nlines) c.line = l->nlines - 1u;
    return c;
}

/* Op 4 with a column. */
MU_INLINE uint32_t mu_loom_cursor_to_offset(mu_loom *l, uint32_t line, uint32_t col)
{
    mu_loom_line r = mu_loom_line_range(l, line);
    if (col > r.len) col = r.len;
    return r.offset + col;
}

/* Op 2: k consecutive lines. One descent, then a linear stream over the u8
 * length array. The escape-free case touches no text bytes at all. */
MU_INLINE uint32_t mu_loom_lines(mu_loom *l, uint32_t line, uint32_t k,
                                 mu_loom_line *MU_RESTRICT out)
{
    mu_loom_pos p;
    uint32_t    produced = 0u, i, lo, base, last, pos, j, nl, slot;

    if (line >= l->nlines || k == 0u) return 0u;
    if (k > l->nlines - line) k = l->nlines - line;
    last = l->nlines - 1u;
    mu_loom__descend(l, line, 1, &p);
    i = p.leaf; lo = p.local; base = p.byte_base;

    while (produced < k && i < l->nleaf) {
        nl   = l->leaf_lines[i];
        slot = l->leaf_slot[i];
        if (nl == 0u) { base += l->leaf_bytes[i]; ++i; lo = 0u; continue; }
        {
            const uint8_t  *len = MU_LOOM_LEN(l, slot);
            const uint32_t *esc = MU_LOOM_LONG(l, slot);
            uint32_t e = 0u;
            pos = MU_LOOM_HDR(l, slot)->lead;
            if (MU_LOOM_HDR(l, slot)->nlong == 0u) {
                for (j = 0u; j < lo; ++j) pos += len[j];
                for (; lo < nl && produced < k; ++lo) {
                    out[produced].offset = base + pos;
                    out[produced].len    = len[lo] - ((line + produced < last) ? 1u : 0u);
                    pos += len[lo];
                    ++produced;
                }
            } else {
                for (j = 0u; j < lo; ++j) {
                    if (len[j] == 255u) { pos += esc[e]; ++e; } else pos += len[j];
                }
                for (; lo < nl && produced < k; ++lo) {
                    uint32_t v;
                    if (len[lo] == 255u) { v = esc[e]; ++e; } else v = (uint32_t)len[lo];
                    out[produced].offset = base + pos;
                    out[produced].len    = v - ((line + produced < last) ? 1u : 0u);
                    pos += v;
                    ++produced;
                }
            }
        }
        base += l->leaf_bytes[i];
        if (lo < nl) break;                 /* stopped mid-leaf */
        ++i; lo = 0u;
    }
    /* park the cursor exactly on the next unread line */
    l->lc_line  = line + produced;
    l->lc_valid = (produced > 0u && l->lc_line < l->nlines);
    if (l->lc_valid) {
        l->lc_off = out[produced - 1u].offset + out[produced - 1u].len +
                    ((line + produced - 1u < last) ? 1u : 0u);
        while (i < l->nleaf && l->leaf_lines[i] == 0u) ++i;
        l->lc_leaf  = i;
        l->lc_local = lo;
        if (i >= l->nleaf) l->lc_valid = 0u;
    }
    return produced;
}

MU_INLINE uint32_t mu_loom_text(mu_loom *l, uint32_t off, uint32_t len, uint8_t *MU_RESTRICT dst)
{
    uint32_t done = 0u, i, local, take, n;
    mu_loom_pos p;

    if (off >= l->text_len || len == 0u) return 0u;
    if (len > l->text_len - off) len = l->text_len - off;
    mu_loom__descend(l, off, 0, &p);
    i = p.leaf; local = p.local;
    while (done < len && i < l->nleaf) {
        n    = l->leaf_bytes[i];
        take = n - local;
        if (take > len - done) take = len - done;
        memcpy(dst + done, MU_LOOM_TEXT(l, l->leaf_slot[i]) + local, take);
        done += take;
        ++i; local = 0u;
    }
    return done;
}

MU_INLINE uint32_t mu_loom_line_text(mu_loom *l, uint32_t line, uint8_t *dst, uint32_t cap)
{
    mu_loom_line r = mu_loom_line_range(l, line);
    uint32_t n = r.len;
    if (n > cap) n = cap;
    return mu_loom_text(l, r.offset, n, dst);
}

/* ---------------------------------------------------------------------------
 * EDITS
 * ------------------------------------------------------------------------- */

MU_INLINE bool mu_loom__scratch(mu_loom *l, size_t need)
{
    uint8_t *ns;
    size_t   cap;
    if (need <= l->scratch_cap) return true;
    cap = l->scratch_cap ? l->scratch_cap : 65536u;
    while (cap < need) cap *= 2u;
    ns = (uint8_t *)realloc(l->scratch, cap);
    if (!ns) return false;
    l->scratch = ns;
    l->scratch_cap = cap;
    return true;
}

/* Does byte 0 of leaf i begin a line? True for leaf 0, or when the previous
 * leaf's final byte is a newline. One byte load, no descent. */
MU_INLINE int mu_loom__starts_line(const mu_loom *l, uint32_t i)
{
    uint32_t prev;
    uint8_t *t;
    if (i == 0u) return 1;
    prev = l->leaf_slot[i - 1u];
    if (l->leaf_bytes[i - 1u] == 0u) return mu_loom__starts_line(l, i - 1u);
    t = MU_LOOM_TEXT(l, prev);
    return t[l->leaf_bytes[i - 1u] - 1u] == '\n';
}

/* Absorb right-hand neighbours until the region is at least one full leaf.
 * Without this, re-planning a slightly-shrunk leaf leaves a runt behind and
 * the leaf count ratchets upward: measured 534 -> 1066 leaves under
 * size-neutral random edits. Padding gives the re-plan enough material to
 * produce balanced leaves instead. */
MU_INLINE void mu_loom__region_pad(mu_loom *l, uint32_t *beg, uint32_t *end)
{
    uint32_t tot = 0u, k;
    for (k = *beg; k <= *end; ++k) tot += l->leaf_bytes[k];
    while (tot < MU_LOOM_TEXT_CAP && *end + 1u < l->nleaf) {
        ++(*end);
        tot += l->leaf_bytes[*end];
        while (*end + 1u < l->nleaf && l->leaf_bytes[*end] > 0u &&
               MU_LOOM_TEXT(l, l->leaf_slot[*end])[l->leaf_bytes[*end] - 1u] != (uint8_t)'\n') {
            ++(*end);
            tot += l->leaf_bytes[*end];
        }
    }
}

/* Replace leaves [beg..end] with a re-plan of `text`. This is the general
 * edit path: it handles newline insertion, leaf split, leaf merge and lines
 * that span leaves, with no special cases left to get wrong. */
MU_INLINE bool mu_loom__resplice(mu_loom *l, uint32_t beg, uint32_t end,
                                 const uint8_t *text, uint32_t len, int at_doc_end)
{
    uint32_t k, off, slot, used, old_lines = 0u, old_bytes = 0u, chunk_limit;
    uint32_t new_count = 0u, new_lines = 0u;
    uint32_t slots[64];
    uint32_t sb[64], sl[64];

    for (k = beg; k <= end; ++k) { old_lines += l->leaf_lines[k]; old_bytes += l->leaf_bytes[k]; }

    off = 0u;
    if (len == 0u && beg == 0u && end == l->nleaf - 1u) {
        /* the WHOLE document became empty: keep one empty leaf */
        slot = mu_loom__slot_alloc(l);
        if (slot == UINT32_MAX) return false;
        MU_LOOM_LEN(l, slot)[0] = 0u;
        MU_LOOM_HDR(l, slot)->nlines = 1u;
        MU_LOOM_HDR(l, slot)->nbytes = 0u;
        MU_LOOM_HDR(l, slot)->nlong  = 0u;
        MU_LOOM_HDR(l, slot)->lead   = 0u;
        slots[0] = slot; sb[0] = 0u; sl[0] = 1u; new_count = 1u; new_lines = 1u;
    }
    /* otherwise an empty region simply removes its leaves: erasing whole
     * lines from the middle must not invent a line where none starts. */
    /* Balance the chunks: greedy filling always leaves a runt in the last
     * leaf, which is what fragments the document over many edits. */
    {
        /* B+tree-style balanced split. When the re-planned text needs exactly
         * two leaves, cut near the midpoint so BOTH halves end up ~half full
         * and the next edit hits the leaf-local fast path in either one.
         * Greedy filling here produced one full leaf plus a runt; the runt's
         * neighbour was still full, so the next edit split again and produced
         * another runt -- the fragmentation spiral. Larger regions keep greedy
         * filling: balancing those measured 2.1x slower. */
        chunk_limit = (len > MU_LOOM_TEXT_CAP && len <= 2u * MU_LOOM_TEXT_CAP)
                    ? ((len + 1u) / 2u)
                    : MU_LOOM_TEXT_CAP;
    }
    while (off < len) {
        if (new_count >= 64u) return false;   /* region bounded by construction */
        slot = mu_loom__slot_alloc(l);
        if (slot == UINT32_MAX) return false;
        used = mu_loom__fill_lim(l, slot, text + off, len - off,
                             off == 0u ? chunk_limit : MU_LOOM_TEXT_CAP,
                             off == 0u || text[off - 1u] == (uint8_t)'\n');
        if (used == 0u) { mu_loom__slot_free(l, slot); break; }
        slots[new_count] = slot;
        sb[new_count]    = used;
        sl[new_count]    = MU_LOOM_HDR(l, slot)->nlines;
        new_lines += sl[new_count];
        ++new_count;
        off += used;
    }
    /* free the old slots, then rewrite the directory in one shift */
    for (k = beg; k <= end; ++k) mu_loom__slot_free(l, l->leaf_slot[k]);
    {
        uint32_t old_n = end - beg + 1u;
        int32_t  delta = (int32_t)new_count - (int32_t)old_n;
        uint32_t tail  = l->nleaf - end - 1u;
        if (delta > 0 && !mu_loom__grow_dir(l, l->nleaf + (uint32_t)delta)) return false;
        if (delta != 0) {
            memmove(l->leaf_bytes + beg + new_count, l->leaf_bytes + end + 1u, (size_t)tail * 4u);
            memmove(l->leaf_lines + beg + new_count, l->leaf_lines + end + 1u, (size_t)tail * 4u);
            memmove(l->leaf_slot  + beg + new_count, l->leaf_slot  + end + 1u, (size_t)tail * 4u);
        }
        for (k = 0u; k < new_count; ++k) {
            l->leaf_bytes[beg + k] = sb[k];
            l->leaf_lines[beg + k] = sl[k];
            l->leaf_slot[beg + k]  = slots[k];
        }
        l->nleaf = (uint32_t)((int32_t)l->nleaf + delta);
    }
    l->text_len = (uint32_t)((int64_t)l->text_len - old_bytes + len);
    l->nlines   = (uint32_t)((int64_t)l->nlines - old_lines + new_lines);
    /* Repair the document tail: a trailing '\n' opens one empty line, and
     * that line must be recorded in the LAST leaf. Doing it here (after the
     * splice) covers the case where the region emptied out entirely. */
    if (at_doc_end && l->nleaf > 0u) {
        uint32_t li = l->nleaf - 1u, ls = l->leaf_slot[li];
        mu_loom_hdr *lh = MU_LOOM_HDR(l, ls);
        int need = (lh->nbytes == 0u)
                 ? (lh->nlines == 0u)
                 : (MU_LOOM_TEXT(l, ls)[lh->nbytes - 1u] == (uint8_t)'\n');
        if (need) {
            if (lh->nlines < MU_LOOM_MAX_LINES && mu_loom__put_len(l, ls, lh->nlines, 0u)) {
                ++lh->nlines; ++l->leaf_lines[li]; ++l->nlines;
            } else {
                uint32_t extra = mu_loom__slot_alloc(l);
                if (extra == UINT32_MAX) return false;
                MU_LOOM_LEN(l, extra)[0] = 0u;
                MU_LOOM_HDR(l, extra)->nlines = 1u;
                MU_LOOM_HDR(l, extra)->nbytes = 0u;
                MU_LOOM_HDR(l, extra)->nlong  = 0u;
                MU_LOOM_HDR(l, extra)->lead   = 0u;
                if (!mu_loom__grow_dir(l, l->nleaf + 1u)) return false;
                l->leaf_slot[l->nleaf] = extra;
                l->leaf_bytes[l->nleaf] = 0u;
                l->leaf_lines[l->nleaf] = 1u;
                ++l->nleaf; ++l->nlines;
            }
        }
    }
    if (l->nlines == 0u) l->nlines = 1u;
    ++l->stat_splits;
    mu_loom__reindex(l);
    return true;
}

/* Widen [i..i] to a line-aligned region [beg..end]. */
MU_INLINE void mu_loom__region(mu_loom *l, uint32_t i, uint32_t *beg, uint32_t *end)
{
    uint32_t b = i, e = i;
    while (b > 0u && !mu_loom__starts_line(l, b)) --b;
    while (e + 1u < l->nleaf && l->leaf_bytes[e] > 0u &&
           MU_LOOM_TEXT(l, l->leaf_slot[e])[l->leaf_bytes[e] - 1u] != '\n') ++e;
    *beg = b;
    *end = e;
}

MU_INLINE bool mu_loom__general_insert(mu_loom *l, uint32_t off, const uint8_t *data, uint32_t n)
{
    mu_loom_pos p;
    uint32_t    beg, end, k, region_bytes, at, total;

    mu_loom__descend(l, off, 0, &p);
    mu_loom__region(l, p.leaf, &beg, &end);
    region_bytes = 0u;
    for (k = beg; k <= end; ++k) region_bytes += l->leaf_bytes[k];
    /* LOCAL SPLIT. `__region` already returned the minimal self-contained
     * region (starts at a line boundary, ends at one). If that region plus the
     * new bytes fills at least one leaf, re-plan ONLY it: one leaf becomes two
     * balanced leaves, exactly a B+tree node split. Padding out to TEXT_CAP
     * used to drag in 2-3 healthy neighbours and re-scan ~40 KB per edit,
     * which was the whole of the range_edit deficit. Pad only when the result
     * would be a runt, so small regions still coalesce with their neighbours. */
    if (region_bytes + n < MU_LOOM_TEXT_CAP) {
        mu_loom__region_pad(l, &beg, &end);
        region_bytes = 0u;
        for (k = beg; k <= end; ++k) region_bytes += l->leaf_bytes[k];
    }
    total = region_bytes + n;
    if (!mu_loom__scratch(l, total)) return false;

    /* byte offset of the region start */
    {
        uint32_t base = 0u;
        for (k = 0u; k < beg; ++k) base += l->leaf_bytes[k];
        at = off - base;
    }
    /* stage: region text with `data` spliced in at `at` */
    {
        uint32_t done = 0u, local = at, take, nb;
        int      placed = 0;
        for (k = beg; k <= end; ++k) {
            nb = l->leaf_bytes[k];
            if (!placed && local <= nb) {
                take = local;
                memcpy(l->scratch + done, MU_LOOM_TEXT(l, l->leaf_slot[k]), take);
                done += take;
                memcpy(l->scratch + done, data, n);
                done += n;
                memcpy(l->scratch + done, MU_LOOM_TEXT(l, l->leaf_slot[k]) + take, nb - take);
                done += nb - take;
                placed = 1;
            } else {
                memcpy(l->scratch + done, MU_LOOM_TEXT(l, l->leaf_slot[k]), nb);
                done += nb;
                if (!placed) local -= nb;
            }
        }
        if (!placed) { memcpy(l->scratch + done, data, n); done += n; }
        total = done;
    }
    /* move out of scratch before resplice reuses slots (scratch is separate,
     * so this is safe; resplice only reads it) */
    return mu_loom__resplice(l, beg, end, l->scratch, total, end == l->nleaf - 1u);
}

/* Fast range insert: patch the u8 length array IN PLACE instead of
 * re-planning the leaf. Re-planning costs a full 16 KiB newline scan
 * (measured 1171 ns) even for a 200-byte paste; this costs one text memmove,
 * one length-array shift and a checkpoint rebuild. Bails to the general path
 * whenever escapes, leaf capacity or continuation bytes are involved. */
/* Lengths of the newline-terminated runs in d[0..n).
 * Writes at most `cap` run lengths, returns the newline count (UINT32_MAX if
 * it would exceed cap) and stores the bytes after the last newline in *tail.
 * One compare per 64 bytes plus one ctz per newline, instead of a branch per
 * byte: AGENTS.md 5.x -- a data-dependent branch on every input byte costs a
 * mispredict per newline and cannot be vectorized by the compiler. */
MU_INLINE uint32_t mu_loom__nl_runs(const uint8_t *d, uint32_t n,
                                    uint32_t *MU_RESTRICT runs, uint32_t cap,
                                    uint32_t *MU_RESTRICT tail)
{
    uint32_t i = 0u, k = 0u, prev = 0u;

#if MU_LOOM_AVX512
    {
        const __m512i nl = _mm512_set1_epi8('\n');
        for (; i + 64u <= n; i += 64u) {
            uint64_t m = (uint64_t)_mm512_cmpeq_epi8_mask(
                             _mm512_loadu_si512((const void *)(d + i)), nl);
            while (m) {
                uint32_t end = i + (uint32_t)mu_trailing_zeroes_u64(m) + 1u;
                if (k >= cap) return UINT32_MAX;
                runs[k++] = end - prev;
                prev = end;
                m &= m - 1u;
            }
        }
    }
#elif defined(__AVX2__) && !defined(MU_LOOM_NO_SIMD)
    {
        const __m256i nl = _mm256_set1_epi8('\n');
        for (; i + 32u <= n; i += 32u) {
            uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(
                             _mm256_loadu_si256((const __m256i *)(const void *)(d + i)), nl));
            while (m) {
                uint32_t end = i + (uint32_t)mu_trailing_zeroes_u32(m) + 1u;
                if (k >= cap) return UINT32_MAX;
                runs[k++] = end - prev;
                prev = end;
                m &= m - 1u;
            }
        }
    }
#endif
    for (; i < n; ++i) {
        if (d[i] == (uint8_t)'\n') {
            if (k >= cap) return UINT32_MAX;
            runs[k++] = i + 1u - prev;
            prev = i + 1u;
        }
    }
    *tail = n - prev;
    return k;
}

MU_INLINE bool mu_loom__fast_range_insert(mu_loom *l, uint32_t off,
                                          const uint8_t *data, uint32_t n,
                                          uint32_t nl_new)
{
    mu_loom_pos  p;
    mu_loom_hdr *h;
    uint32_t     slot, idx, before, col, old, k, i, prev, runs[66], piece;
    uint8_t     *t, *len;

    if (nl_new == 0u || nl_new > 64u) return false;
    mu_loom__descend(l, off, 0, &p);
    slot = l->leaf_slot[p.leaf];
    h    = MU_LOOM_HDR(l, slot);
    if ((uint64_t)h->nbytes + n > MU_LOOM_TEXT_CAP) return false;
    if (h->nlines == 0u || p.local < h->lead) return false;
    if ((uint64_t)h->nlines + nl_new > MU_LOOM_MAX_LINES) return false;

    len = MU_LOOM_LEN(l, slot);
    idx = mu_loom__leaf_find(l, slot, p.local - h->lead, &before);
    if (idx >= h->nlines) {
        if (!(p.leaf + 1u == l->nleaf && p.local == h->nbytes)) return false;
        idx = h->nlines - 1u;
        before = mu_loom__leaf_prefix(l, slot, idx) - h->lead;
    }
    /* A non-last leaf whose final byte is not '\n' has a last line that
     * CONTINUES into the next leaf; that leaf's hdr.lead counts the borrowed
     * bytes and this leaf's stored length already includes them. Adding a
     * newline inside that line hands the tail after the newline to a NEW
     * line owned by the next leaf, which this leaf-local path cannot express:
     * it would credit the whole tail here and leave the next leaf's lead
     * stale, double-counting one line. Re-plan instead. */
    if (p.leaf + 1u < l->nleaf && idx + 1u == h->nlines &&
        MU_LOOM_TEXT(l, slot)[h->nbytes - 1u] != (uint8_t)'\n')
        return false;
    old = mu_loom__stored_len(l, slot, idx);
    col = p.local - h->lead - before;

    /* segment runs of the inserted text, each including its trailing '\n' */
    k = mu_loom__nl_runs(data, n, runs, nl_new, &prev);
    if (k != nl_new) return false;
    runs[k] = prev; /* nl_runs returns the trailing byte count in prev */

    /* Pieces may exceed the u8 range; that costs an escape entry each, not
     * a re-plan. Count them first so the table can be shifted once. */
    {
        uint32_t pieces[66], need = 0u, e0, had, delta, w, j;
        uint32_t *esc = MU_LOOM_LONG(l, slot);
        pieces[0] = col + runs[0];
        for (i = 1u; i < k; ++i) pieces[i] = runs[i];
        pieces[k] = runs[k] + (old - col);
        for (i = 0u; i <= k; ++i) need += (uint32_t)(pieces[i] >= 255u);
        had = (uint32_t)(len[idx] == 255u);
        if ((uint64_t)h->nlong - had + need > MU_LOOM_MAX_LONG) return false;
        e0 = mu_loom__esc_before(len, idx);
        delta = need - had;
        /* move the escapes belonging to lines AFTER idx out of the way */
        if (delta) {
            uint32_t tail = h->nlong - e0 - had;
            if (need > had) {
                for (j = tail; j > 0u; --j) esc[e0 + need + j - 1u] = esc[e0 + had + j - 1u];
            } else {
                for (j = 0u; j < tail; ++j) esc[e0 + need + j] = esc[e0 + had + j];
            }
        }
        w = e0;
        for (i = 0u; i <= k; ++i) if (pieces[i] >= 255u) esc[w++] = pieces[i];
        h->nlong = h->nlong - had + need;

        t = MU_LOOM_TEXT(l, slot);
        memmove(t + p.local + n, t + p.local, h->nbytes - p.local);
        memcpy(t + p.local, data, n);
        l->stat_bytes_moved += h->nbytes - p.local;

        memmove(len + idx + 1u + k, len + idx + 1u, h->nlines - idx - 1u);
        for (i = 0u; i <= k; ++i)
            len[idx + i] = (pieces[i] >= 255u) ? 255u : (uint8_t)pieces[i];
    }
    (void)piece;
    h->nbytes += n;
    h->nlines += k;
    mu_loom__cp_rebuild_from(l, slot, idx); /* entries below idx are untouched */
    mu_loom__touch(l, p.leaf, (int32_t)n, (int32_t)k);
    l->text_len += n;
    l->nlines   += k;
    l->lc_valid  = 0u;
    return true;
}

/* Fast range erase: the affected lines merge into one. Same idea, opposite
 * direction — shift the length array left instead of re-planning. */
MU_INLINE bool mu_loom__fast_range_erase(mu_loom *l, uint32_t off, uint32_t n)
{
    mu_loom_pos  p;
    mu_loom_hdr *h;
    uint32_t     slot, i0, i1, b0, b1, c0, c1, merged, j, rel, endrel;
    uint8_t     *t, *len;

    mu_loom__descend(l, off, 0, &p);
    slot = l->leaf_slot[p.leaf];
    h    = MU_LOOM_HDR(l, slot);
    if (h->nlines == 0u) return false;
    rel = p.local;
    if (rel < h->lead) return false;
    endrel = rel + n;
    if (endrel > h->nbytes) return false;
    /* Reaching the END of a non-last leaf can flip whether that leaf ends
     * with '\n'. If it stops ending with one, its last line now continues
     * into the next leaf, which must absorb a line and gain a matching
     * hdr.lead -- bookkeeping this leaf-local path does not do. Only an erase
     * that reaches the leaf end can change that final byte. */
    if (endrel == h->nbytes && p.leaf + 1u < l->nleaf) return false;
    len = MU_LOOM_LEN(l, slot);
    rel -= h->lead; endrel -= h->lead;

    i0 = mu_loom__leaf_find(l, slot, rel, &b0);
    if (i0 >= h->nlines) return false;
    c0 = rel - b0;
    /* Anchor on the first SURVIVING byte. If the erase consumed line i1's
     * trailing newline then the next line's text joins the merged line, so
     * that line must be counted as merged too — the merge spans exactly
     * (newlines removed + 1) lines. */
    i1 = mu_loom__leaf_find(l, slot, endrel, &b1);
    if (i1 >= h->nlines) {
        if (endrel != h->nbytes - h->lead) return false;
        i1 = h->nlines - 1u;
        b1 = mu_loom__leaf_prefix(l, slot, i1) - h->lead;
    }
    c1 = endrel - b1;
    if (i1 < i0) return false;
    if (i1 == i0) return false;                      /* no newline: other fast path */
    merged = c0 + (mu_loom__stored_len(l, slot, i1) - c1);
    if (h->nlines - (i1 - i0) < 1u) return false;
    {
        uint32_t e0 = mu_loom__esc_before(len, i0), ein = 0u, need, tail;
        uint32_t *esc = MU_LOOM_LONG(l, slot);
        for (j = i0; j <= i1; ++j) ein += (uint32_t)(len[j] == 255u);
        need = (uint32_t)(merged >= 255u);
        if (h->nlong - ein + need > MU_LOOM_MAX_LONG) return false;
        tail = h->nlong - e0 - ein;
        if (need > ein) { for (j = tail; j > 0u; --j) esc[e0 + need + j - 1u] = esc[e0 + ein + j - 1u]; }
        else            { for (j = 0u; j < tail; ++j) esc[e0 + need + j] = esc[e0 + ein + j]; }
        if (need) esc[e0] = merged;
        h->nlong = h->nlong - ein + need;
    }

    t = MU_LOOM_TEXT(l, slot);
    memmove(t + p.local, t + p.local + n, h->nbytes - p.local - n);
    l->stat_bytes_moved += h->nbytes - p.local - n;
    memmove(len + i0 + 1u, len + i1 + 1u, h->nlines - i1 - 1u);
    len[i0] = (merged >= 255u) ? 255u : (uint8_t)merged;
    h->nlines -= (i1 - i0);
    h->nbytes -= n;
    mu_loom__cp_rebuild_from(l, slot, i0); /* entries below i0 are untouched */
    mu_loom__touch(l, p.leaf, -(int32_t)n, -(int32_t)(i1 - i0));
    l->text_len -= n;
    l->nlines   -= (i1 - i0);
    l->lc_valid  = 0u;
    return true;
}

/* Op 5 / Op 6: insert. The typing fast path is O(1): one memmove inside a
 * 16 KiB L1/L2-resident leaf, ONE u8 increment, and SIX index stores. */
MU_INLINE bool mu_loom_insert(mu_loom *l, uint32_t off, const uint8_t *data, size_t n)
{
    mu_loom_pos  p;
    mu_loom_hdr *h;
    uint32_t     slot, idx, before, stored, nl_new, esc;
    uint8_t     *t, *len;

    if (n == 0u) return true;
    if (off > l->text_len) return false;
    if ((uint64_t)l->text_len + n > 0xFFFFFFFFu) return false;
    if (n > 0xFFFFFFFu) return mu_loom__general_insert(l, off, data, (uint32_t)n);

    nl_new = mu_loom__count_nl(data, (uint32_t)n);
    mu_loom__descend(l, off, 0, &p);
    slot = l->leaf_slot[p.leaf];
    h    = MU_LOOM_HDR(l, slot);
    t    = MU_LOOM_TEXT(l, slot);
    len  = MU_LOOM_LEN(l, slot);

    /* FAST PATH: no newline added, room in the leaf, and the affected line
     * starts here. This is >95% of keystrokes. */
    if (nl_new == 0u && (uint64_t)h->nbytes + n <= MU_LOOM_TEXT_CAP && h->nlines > 0u) {
        int owns = 1;
        if (p.local < h->lead) owns = 0;   /* byte belongs to an earlier leaf */
        idx = owns ? mu_loom__leaf_find(l, slot, p.local - h->lead, &before) : 0u;
        if (idx >= h->nlines) {
            /* Inserting at the very end of a leaf: those bytes extend the
             * document's last line ONLY if this is the last leaf. Otherwise
             * they begin a line owned by the NEXT leaf, and crediting them
             * here silently corrupts both leaves' lengths. */
            if (owns && p.leaf + 1u == l->nleaf && p.local == h->nbytes) idx = h->nlines - 1u;
            else owns = 0;
        }
        stored = owns ? mu_loom__stored_len(l, slot, idx) : 0u;
        if (owns && ((uint64_t)stored + n < 255u || h->nlong < MU_LOOM_MAX_LONG ||
            len[idx] == 255u)) {
            memmove(t + p.local + n, t + p.local, h->nbytes - p.local);
            memcpy(t + p.local, data, n);
            l->stat_bytes_moved += h->nbytes - p.local;
            h->nbytes += (uint32_t)n;
            if ((uint64_t)stored + n < 255u) {
                len[idx] = (uint8_t)(stored + n);
            } else if (len[idx] == 255u) {
                MU_LOOM_LONG(l, slot)[mu_loom__esc_index(len, idx)] += (uint32_t)n;
            } else {
                /* A new escape must be INSERTED at its rank, not appended:
                 * esc_index() locates it by counting 255s before this line,
                 * so the table has to stay in line order. */
                uint32_t *tab = MU_LOOM_LONG(l, slot);
                esc = mu_loom__esc_index(len, idx);
                uint32_t j;
                for (j = h->nlong; j > esc; --j) tab[j] = tab[j - 1u];
                tab[esc] = stored + (uint32_t)n;
                ++h->nlong;
                len[idx] = 255u;
            }
            mu_loom__cp_add(l, slot, idx, (int32_t)n);
            mu_loom__touch(l, p.leaf, (int32_t)n, 0);
            l->text_len += (uint32_t)n;
            l->lc_valid = 0u;   /* lengths inside this leaf moved */
            return true;
        }
    }
    if (mu_loom__fast_range_insert(l, off, data, (uint32_t)n, nl_new)) return true;
    return mu_loom__general_insert(l, off, data, (uint32_t)n);
}

MU_INLINE bool mu_loom_erase(mu_loom *l, uint32_t off, size_t n)
{
    mu_loom_pos  p;
    mu_loom_hdr *h;
    uint32_t     slot, k, region_bytes, base, total, done, nb, skip;
    uint32_t     beg, end, nl_del;

    if (n == 0u) return true;
    if (off >= l->text_len) return false;
    if (n > l->text_len - off) n = l->text_len - off;

    /* count newlines removed with one SIMD pass over the affected leaves */
    {
        uint8_t stackbuf[256];
        uint32_t got;
        if (n <= sizeof stackbuf) {
            got = mu_loom_text(l, off, (uint32_t)n, stackbuf);
            nl_del = mu_loom__count_nl(stackbuf, got);
        } else {
            if (!mu_loom__scratch(l, n)) return false;
            got = mu_loom_text(l, off, (uint32_t)n, l->scratch);
            nl_del = mu_loom__count_nl(l->scratch, got);
        }
    }

    mu_loom__descend(l, off, 0, &p);
    slot = l->leaf_slot[p.leaf];
    h    = MU_LOOM_HDR(l, slot);

    /* FAST PATH: the erased range is inside one leaf, removes no newline,
     * and does not empty the leaf. One memmove, one u8 decrement, 6 stores. */
    if (nl_del == 0u && (uint64_t)p.local + n <= h->nbytes && h->nlines > 0u) {
        uint32_t idx, before, stored;
        uint8_t *t = MU_LOOM_TEXT(l, slot), *len = MU_LOOM_LEN(l, slot);
        idx = (p.local < h->lead) ? UINT32_MAX
                                  : mu_loom__leaf_find(l, slot, p.local - h->lead, &before);
        if (idx >= h->nlines && idx != UINT32_MAX) {
            if (p.leaf + 1u == l->nleaf && h->nlines > 0u) idx = h->nlines - 1u;
            else idx = UINT32_MAX;
        }
        stored = (idx == UINT32_MAX) ? 0u : mu_loom__stored_len(l, slot, idx);
        if (idx != UINT32_MAX && stored >= n) {
            memmove(t + p.local, t + p.local + n, h->nbytes - p.local - (uint32_t)n);
            l->stat_bytes_moved += h->nbytes - p.local - (uint32_t)n;
            h->nbytes -= (uint32_t)n;
            stored -= (uint32_t)n;
            if (stored < 255u) {
                if (len[idx] == 255u) {
                    /* escape no longer needed: drop it and shift the table */
                    uint32_t e = mu_loom__esc_index(len, idx), j;
                    for (j = e; j + 1u < h->nlong; ++j)
                        MU_LOOM_LONG(l, slot)[j] = MU_LOOM_LONG(l, slot)[j + 1u];
                    --h->nlong;
                }
                len[idx] = (uint8_t)stored;
            } else {
                MU_LOOM_LONG(l, slot)[mu_loom__esc_index(len, idx)] = stored;
            }
            mu_loom__cp_add(l, slot, idx, -(int32_t)n);
            mu_loom__touch(l, p.leaf, -(int32_t)n, 0);
            l->text_len -= (uint32_t)n;
            l->lc_valid = 0u;
            return true;
        }
    }

    if (mu_loom__fast_range_erase(l, off, (uint32_t)n)) return true;

    /* general: stage the region minus the erased range, then re-splice.
     * The region must cover EVERY leaf the erased range touches, not just
     * the one holding its first byte, then widen to line boundaries. */
    {
        mu_loom_pos pe;
        uint32_t    e2, after = off + (uint32_t)n;
        mu_loom__region(l, p.leaf, &beg, &end);
        /* Anchor on the first SURVIVING byte, not the last erased one. If the
         * erase consumes the region's final '\n', the line that owned it
         * merges with the next leaf's first line, so that leaf must join the
         * region too — otherwise the re-plan records a partial line as whole
         * and the document gains a phantom line. */
        if (after < l->text_len) {
            mu_loom__descend(l, after, 0, &pe);
            e2 = pe.leaf;
            if (e2 > end) {
                end = e2;
                while (end + 1u < l->nleaf && l->leaf_bytes[end] > 0u &&
                       MU_LOOM_TEXT(l, l->leaf_slot[end])[l->leaf_bytes[end] - 1u] != '\n') ++end;
            }
        }
        /* An erase that reaches EOF must also swallow the trailing empty
         * leaf: removing the document's final '\n' destroys the empty line
         * that leaf exists to hold. It carries no bytes, so the byte-range
         * descent above can never reach it. */
        /* Keep the erase local when enough survives to fill half a leaf;
         * otherwise widen so the remainder coalesces instead of leaving a
         * runt. Mirrors the local-split rule on the insert side. */
        {
            uint32_t rb = 0u, q;
            for (q = beg; q <= end; ++q) rb += l->leaf_bytes[q];
            if (rb < (uint32_t)n || rb - (uint32_t)n < MU_LOOM_TEXT_CAP / 2u)
                mu_loom__region_pad(l, &beg, &end);
        }
        if (off + (uint32_t)n == l->text_len) end = l->nleaf - 1u;
    }
    region_bytes = 0u;
    for (k = beg; k <= end; ++k) region_bytes += l->leaf_bytes[k];
    if (!mu_loom__scratch(l, region_bytes)) return false;
    base = 0u;
    for (k = 0u; k < beg; ++k) base += l->leaf_bytes[k];
    done = 0u; skip = (uint32_t)n;
    {
        uint32_t local = off - base;
        for (k = beg; k <= end; ++k) {
            uint8_t *t = MU_LOOM_TEXT(l, l->leaf_slot[k]);
            nb = l->leaf_bytes[k];
            if (local >= nb) { memcpy(l->scratch + done, t, nb); done += nb; local -= nb; continue; }
            {
                uint32_t keep = local;
                uint32_t rem  = nb - keep;
                uint32_t cut  = rem < skip ? rem : skip;
                memcpy(l->scratch + done, t, keep); done += keep;
                memcpy(l->scratch + done, t + keep + cut, rem - cut); done += rem - cut;
                skip -= cut;
                local = 0u;
            }
        }
        total = done;
    }
    return mu_loom__resplice(l, beg, end, l->scratch, total, end == l->nleaf - 1u);
}

/* ---------------------------------------------------------------------------
 * Verification. Names the violated invariant, the leaf, and both counts —
 * a boolean verifier is nearly useless for index bugs.
 * ------------------------------------------------------------------------- */

MU_INLINE bool mu_loom_verify_report(mu_loom *l, char *out, size_t cap)
{
    uint32_t i, j, b, n, tb = 0u, tn = 0u, stored, sum;
    uint32_t g, s, gb, gn;

    for (i = 0u; i < l->nleaf; ++i) {
        uint32_t slot = l->leaf_slot[i];
        mu_loom_hdr *h = MU_LOOM_HDR(l, slot);
        if (h->nbytes != l->leaf_bytes[i]) {
            snprintf(out, cap, "L1 leaf %u: header nbytes %u != dir %u", i, h->nbytes, l->leaf_bytes[i]);
            return false;
        }
        if (h->nlines != l->leaf_lines[i]) {
            snprintf(out, cap, "L2 leaf %u: header nlines %u != dir %u", i, h->nlines, l->leaf_lines[i]);
            return false;
        }
        if (h->nlines > MU_LOOM_MAX_LINES) {
            snprintf(out, cap, "L3 leaf %u: nlines %u over max", i, h->nlines); return false;
        }
        if (h->nlong > MU_LOOM_MAX_LONG) {
            snprintf(out, cap, "L4 leaf %u: nlong %u over max", i, h->nlong); return false;
        }
        sum = h->lead;
        for (j = 0u; j < h->nlines; ++j) sum += mu_loom__stored_len(l, slot, j);
        /* a leaf's last line may extend into the next leaf; otherwise the
         * stored lengths must account for exactly its bytes */
        if (sum < h->nbytes) {
            snprintf(out, cap, "L5 leaf %u: sum(len)=%u < nbytes=%u", i, sum, h->nbytes);
            return false;
        }
        if (sum > h->nbytes && i + 1u < l->nleaf) {
            uint32_t over = sum - h->nbytes, k = i + 1u, avail = 0u;
            while (k < l->nleaf && avail < over) { avail += l->leaf_bytes[k]; ++k; }
            if (avail < over) {
                snprintf(out, cap, "L6 leaf %u: last line overruns document by %u", i, over - avail);
                return false;
            }
        }
        /* nlong must equal the number of 255 entries */
        {
            uint32_t cnt = 0u;
            const uint8_t *len = MU_LOOM_LEN(l, slot);
            for (j = 0u; j < h->nlines; ++j) cnt += (uint32_t)(len[j] == 255u);
            if (cnt != h->nlong) {
                snprintf(out, cap, "L7 leaf %u: nlong %u but %u escapes", i, h->nlong, cnt);
                return false;
            }
        }
        tb += l->leaf_bytes[i];
        tn += l->leaf_lines[i];
    }
    if (tb != l->text_len) {
        snprintf(out, cap, "L8 sum(leaf_bytes)=%u != text_len=%u", tb, l->text_len); return false;
    }
    if (tn != l->nlines) {
        snprintf(out, cap, "L9 sum(leaf_lines)=%u != nlines=%u", tn, l->nlines); return false;
    }
    for (g = 0u; g < l->ngrp; ++g) {
        uint32_t end = (g + 1u) * MU_LOOM_GROUP; if (end > l->nleaf) end = l->nleaf;
        b = 0u; n = 0u;
        for (i = g * MU_LOOM_GROUP; i < end; ++i) { b += l->leaf_bytes[i]; n += l->leaf_lines[i]; }
        if (b != l->grp_bytes[g] || n != l->grp_lines[g]) {
            snprintf(out, cap, "L10 group %u: (%u,%u) != (%u,%u)", g,
                     l->grp_bytes[g], l->grp_lines[g], b, n); return false;
        }
    }
    for (s = 0u; s < l->nsup; ++s) {
        uint32_t end = (s + 1u) * MU_LOOM_SUPER; if (end > l->ngrp) end = l->ngrp;
        gb = 0u; gn = 0u;
        for (i = s * MU_LOOM_SUPER; i < end; ++i) { gb += l->grp_bytes[i]; gn += l->grp_lines[i]; }
        if (gb != l->sup_bytes[s] || gn != l->sup_lines[s]) {
            snprintf(out, cap, "L11 super %u mismatch", s); return false;
        }
    }
    /* the newline count must agree with the line count */
    {
        uint32_t nl = 0u;
        for (i = 0u; i < l->nleaf; ++i)
            nl += mu_loom__count_nl(MU_LOOM_TEXT(l, l->leaf_slot[i]), l->leaf_bytes[i]);
        if (nl + 1u != l->nlines) {
            snprintf(out, cap, "L12 newlines %u + 1 != nlines %u", nl, l->nlines); return false;
        }
    }
    (void)stored;
    snprintf(out, cap, "ok");
    return true;
}

MU_INLINE bool mu_loom_verify(mu_loom *l)
{
    char buf[256];
    return mu_loom_verify_report(l, buf, sizeof buf);
}

MU_END_EXTERN_C

#endif /* MU_LOOM_H */
