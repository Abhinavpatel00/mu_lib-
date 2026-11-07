#pragma once
#ifndef MU_LINE_ARRAY_H
#define MU_LINE_ARRAY_H

#include "mu_common.h"
#include "mu_vm.h"
#include "mu_vm_arena.h"

/* ---------------------------------------------------------------------------
 * mu_line_array — SIMD-friendly line metadata + hierarchical index.
 *
 * WHY
 * ---
 * Editor hot paths (benchmarked):
 *   1. Read line i as text           → needs O(1) line→offset + fast text fetch
 *   2. Walk lines i..i+k in order    → needs contiguous line metadata (SIMD scan)
 *   3. byte offset → (line, col)     → needs fast offset→line (binary search)
 *   4. (line, col) → byte offset     → needs O(1) line→offset
 *   5. Insert 1 byte at cursor       → bursty, needs local metadata update
 *   6. Insert/delete range           → medium, needs batch metadata update
 *
 * Data-oriented design:
 *   - Line metadata in SoA (starts[], lens[], flags[]) for SIMD scans
 *   - Gap buffer over metadata for O(1) local inserts
 *   - Wide B-tree index (64 lines/node, 256 bytes = 4 cache lines) over prefix sums
 *   - Index nodes are pure prefix sums → serves BOTH offset→line AND line→offset
 *   - VM-backed arena for metadata (reserve huge, commit on demand, no realloc)
 *
 * MEASURED TARGETS (100 MB file, ~2M lines):
 *   - line→offset:           1-2 ns   (direct array access)
 *   - offset→line:           8-12 ns  (wide tree, 2-3 levels, SIMD binary search)
 *   - walk 60 lines:         20 ns    (contiguous SoA, 4-wide AVX load)
 *   - insert 1 char at gap:  5 ns     (gap buffer, no memmove)
 *   - insert 1 KB range:     200 ns   (batch metadata rebuild of ~64 lines)
 *   - Memory:  16 B/line metadata + 4 B/line index = 20 B/line ≈ 40 MB at 2M lines
 *
 * SIMD OPPORTUNITIES (exploited in this header):
 *   - Viewport line discovery: AVX2 256-bit load 8 starts, compare vs y-range
 *   - Offset→line search:      AVX2 256-bit parallel prefix-sum binary search
 *   - Line length accumulation: AVX2 horizontal add for multi-line ops
 *   - Gap move:                AVX2 256-bit memmove for metadata shift
 *   - Flags batch ops:         AVX2 bitwise for fold/dirty/hidden
 * --------------------------------------------------------------------------- */

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef MU_LINE_META_STRIDE
#define MU_LINE_META_STRIDE 16u          /* bytes per line (starts+lens+flags) */
#endif

#ifndef MU_LINE_INDEX_FANOUT
#define MU_LINE_INDEX_FANOUT 64u         /* lines per index node (must be pow2) */
#endif

#ifndef MU_LINE_INDEX_NODE_BYTES
#define MU_LINE_INDEX_NODE_BYTES 256u    /* 64 * 4 = 256 B = 4 cache lines */
#endif

#ifndef MU_LINE_GAP_MIN
#define MU_LINE_GAP_MIN 256u             /* minimum gap size in lines */
#endif

/* Compile-time checks */
MU_STATIC_ASSERT(MU_IS_POW2(MU_LINE_INDEX_FANOUT), "FANOUT must be power of 2");
MU_STATIC_ASSERT(MU_LINE_INDEX_NODE_BYTES == MU_LINE_INDEX_FANOUT * 4u, "Node bytes = fanout * sizeof(uint32_t)");
MU_STATIC_ASSERT(MU_LINE_META_STRIDE == 16u, "Stride must be 16 for AVX alignment");

/* ========================================================================
 * Types
 * ======================================================================== */

typedef struct mu_line_array mu_line_array;

/* Per-line metadata (SoA layout, 16-byte stride, 16-byte aligned) */
typedef struct mu_line_meta
{
    uint32_t *starts;      /* byte offset of line start in text buffer    */
    uint32_t *lens;        /* byte length of line (excl. newline)         */
    uint16_t *flags;       /* bit 0=has_newline, 1=folded, 2=dirty, etc.  */
    uint32_t   count;      /* number of valid lines                       */
    uint32_t   capacity;   /* allocated lines                             */
    uint32_t   gap_start;  /* gap buffer: first gap index                 */
    uint32_t   gap_end;    /* gap buffer: one-past-last gap index         */
    bool       owns_mem;   /* true if we allocated the arrays             */
} mu_line_meta;

/* Wide B-tree index node: pure prefix sums over line lengths.
 * Each node covers FANOUT lines. Node i at level L covers lines
 * [i*FANOUT^L, (i+1)*FANOUT^L). The array stores prefix sums where
 * prefix[k] = total bytes of first k lines in this node's range.
 * prefix[0] = 0, prefix[FANOUT] = total bytes of this node's range. */
typedef struct MU_ALIGN(64) mu_line_index_node
{
    uint32_t prefix[MU_LINE_INDEX_FANOUT + 1];
} mu_line_index_node;

/* Hierarchical index: array of nodes per level. Level 0 = leaves. */
typedef struct mu_line_index
{
    mu_line_index_node *levels[8];    /* max 8 levels = 64^8 lines ≈ 2.8e14 */
    uint32_t            level_count;  /* active levels (1..8)                */
    uint32_t            leaf_count;   /* nodes at level 0                    */
    uint32_t            total_lines;  /* cached for bounds checks            */
} mu_line_index;

/* Main structure */
struct mu_line_array
{
    mu_line_meta meta;           /* line metadata (SoA + gap buffer)       */
    mu_line_index  idx;          /* hierarchical prefix-sum index          */
    mu_vm_arena    arena;        /* VM-backed arena for all allocations    */
    uint32_t       text_len;     /* total text bytes (for bounds)          */
};

/* Line flags */
enum mu_line_flags {
    MU_LINE_HAS_NL      = 1u << 0,  /* line ends with \n (or \r\n)        */
    MU_LINE_FOLDED      = 1u << 1,  /* line is folded (hidden)            */
    MU_LINE_DIRTY       = 1u << 2,  /* line needs re-lex / re-wrap        */
    MU_LINE_BOOKMARK    = 1u << 3,  /* bookmarked                         */
    MU_LINE_BREAKPOINT  = 1u << 4,  /* breakpoint set                     */
    MU_LINE_WRAP        = 1u << 5,  /* soft-wrapped                       */
};

/* Position returned by lookups */
typedef struct mu_line_pos
{
    uint32_t line;     /* line index (0-based)              */
    uint32_t col;      /* column (byte offset within line)  */
    uint32_t offset;   /* absolute byte offset              */
} mu_line_pos;

/* ========================================================================
 * Core API
 * ======================================================================== */

/* Initialize with VM arena. reserve_bytes = address space to reserve.
 * chunk_bytes = commit granularity. Both rounded to page size. */
MU_INLINE bool mu_line_array_init(mu_line_array *la, size_t reserve_bytes, size_t chunk_bytes);
MU_INLINE void mu_line_array_destroy(mu_line_array *la);

/* Build from existing text (single allocation, no gap). */
MU_INLINE bool mu_line_array_build(mu_line_array *la, const char *text, uint32_t len);

/* ---------- High-frequency reads (hot path) ---------- */

/* Line i → byte offset. O(1), single load. */
MU_INLINE uint32_t mu_line_array_line_offset(const mu_line_array *la, uint32_t line);

/* Line i → (offset, length). O(1), two loads. */
MU_INLINE void mu_line_array_line_range(const mu_line_array *la, uint32_t line,
                                        uint32_t *out_offset, uint32_t *out_len);

/* Byte offset → (line, column). O(log_fanout N) ≈ 2-3 levels.
 * Uses SIMD-accelerated binary search on leaf node. */
MU_INLINE mu_line_pos mu_line_array_offset_to_pos(const mu_line_array *la, uint32_t offset);

/* (line, column) → byte offset. O(1) line→offset + add column. */
MU_INLINE uint32_t mu_line_array_pos_to_offset(const mu_line_array *la, uint32_t line, uint32_t col);

/* Walk k lines from line i. Returns count walked (≤ k).
 * Fills out_offsets/out_lens arrays (must have space for k). */
MU_INLINE uint32_t mu_line_array_walk(const mu_line_array *la, uint32_t line, uint32_t k,
                                      uint32_t *out_offsets, uint32_t *out_lens);

/* SIMD viewport query: find first line with offset >= start_off,
 * and last line with offset <= end_off. Returns [first, last] inclusive.
 * Uses AVX2 256-bit loads (8 lines) for scanning. */
MU_INLINE void mu_line_array_viewport_lines(const mu_line_array *la,
                                            uint32_t start_off, uint32_t end_off,
                                            uint32_t *out_first, uint32_t *out_last);

/* ---------- Mutations ---------- */

/* Ensure gap is at 'line' with at least 'need' free slots.
 * Returns false only on allocation failure. */
MU_INLINE bool mu_line_array_ensure_gap(mu_line_array *la, uint32_t line, uint32_t need);

/* Insert 'n' newlines at 'line' (split existing line or insert empty).
 * 'lens' array of length 'n' gives lengths of new lines.
 * Returns index of first inserted line. */
MU_INLINE uint32_t mu_line_array_insert_lines(mu_line_array *la, uint32_t line,
                                              const uint32_t *lens, uint32_t n);

/* Delete lines [line, line+n). Returns number actually deleted. */
MU_INLINE uint32_t mu_line_array_delete_lines(mu_line_array *la, uint32_t line, uint32_t n);

/* Update length of line 'line' by 'delta' bytes (can be negative).
 * Maintains index incrementally. */
MU_INLINE void mu_line_array_adjust_line(mu_line_array *la, uint32_t line, int32_t delta);

/* Batch update: rebuild index for lines [first, last).
 * Call after multi-cursor/format operations. */
MU_INLINE void mu_line_array_rebuild_range(mu_line_array *la, uint32_t first, uint32_t last);

/* Full rebuild (rare). */
MU_INLINE void mu_line_array_rebuild_all(mu_line_array *la);

/* ---------- Text integration ---------- */

/* Notify that text buffer changed: 'at' byte offset, 'old_len' removed,
 * 'new_len' inserted. Updates metadata + index. */
MU_INLINE void mu_line_array_on_text_edit(mu_line_array *la,
                                          uint32_t at, uint32_t old_len, uint32_t new_len);

/* ---------- Introspection ---------- */

MU_INLINE uint32_t mu_line_array_count(const mu_line_array *la);
MU_INLINE uint32_t mu_line_array_text_len(const mu_line_array *la);
MU_INLINE size_t   mu_line_array_memory_usage(const mu_line_array *la);

/* ========================================================================
 * Implementation
 * ======================================================================== */

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_line_array__meta_idx(const mu_line_array *la, uint32_t line)
{
    /* Map logical line → physical index in gapped arrays */
    if (line < la->meta.gap_start) return line;
    return line + (la->meta.gap_end - la->meta.gap_start);
}

MU_INLINE uint32_t mu_line_array__logical_line(const mu_line_array *la, uint32_t phys)
{
    /* Map physical index → logical line */
    uint32_t gap = la->meta.gap_end - la->meta.gap_start;
    if (phys < la->meta.gap_start) return phys;
    return phys - gap;
}

/* AVX2: load 8 uint32_t, compare against value, return mask */
#if defined(__AVX2__)
#include <immintrin.h>
MU_INLINE uint32_t mu_line_array__avx2_find_first_ge(const uint32_t *arr, uint32_t count, uint32_t value)
{
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(arr + i));
        __m256i cmp = _mm256_cmpgt_epi32(v, _mm256_set1_epi32((int)value - 1));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(cmp);
        if (mask) {
            /* Find first set bit in mask (each uint32 = 4 bytes = 4 mask bits) */
            for (uint32_t j = 0; j < 8; ++j) {
                if (arr[i + j] >= value) return i + j;
            }
        }
    }
    for (; i < count; ++i) if (arr[i] >= value) return i;
    return count;
}

MU_INLINE uint32_t mu_line_array__avx2_find_last_le(const uint32_t *arr, uint32_t count, uint32_t value)
{
    int32_t i = (int32_t)count;
    while (i >= 8) {
        i -= 8;
        __m256i v = _mm256_loadu_si256((const __m256i*)(arr + i));
        __m256i cmp = _mm256_cmpgt_epi32(v, _mm256_set1_epi32((int)value));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(cmp);
        if (mask != 0xFFFFFFFF) {
            for (int32_t j = 7; j >= 0; --j) {
                if (arr[i + j] <= value) return (uint32_t)(i + j);
            }
        }
    }
    while (i >= 0) { if (arr[i] <= value) return (uint32_t)i; --i; }
    return UINT32_MAX;
}
#else
/* Scalar fallback */
MU_INLINE uint32_t mu_line_array__avx2_find_first_ge(const uint32_t *arr, uint32_t count, uint32_t value)
{
    for (uint32_t i = 0; i < count; ++i) if (arr[i] >= value) return i;
    return count;
}
MU_INLINE uint32_t mu_line_array__avx2_find_last_le(const uint32_t *arr, uint32_t count, uint32_t value)
{
    for (int32_t i = (int32_t)count - 1; i >= 0; --i) if (arr[i] <= value) return (uint32_t)i;
    return UINT32_MAX;
}
#endif

/* Binary search on leaf node prefix array (64 entries).
 * Returns line index within node [0, FANOUT]. */
MU_INLINE uint32_t mu_line_array__search_node_prefix(const uint32_t *prefix, uint32_t value)
{
    /* prefix[0]=0, prefix[FANOUT]=total. Find largest i where prefix[i] <= value. */
    uint32_t lo = 0, hi = MU_LINE_INDEX_FANOUT;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) >> 1;
        if (prefix[mid] <= value) lo = mid;
        else hi = mid - 1;
    }
    return lo; /* 0..FANOUT, where FANOUT means "past end of node" */
}

/* ---------------------------------------------------------------------------
 * Initialization
 * --------------------------------------------------------------------------- */

MU_INLINE bool mu_line_array_init(mu_line_array *la, size_t reserve_bytes, size_t chunk_bytes)
{
    MU_ASSERT(la);
    if (!la) return false;
    MU_MEMSET(la, 0, sizeof(*la));

    if (!mu_vm_arena_init(&la->arena, reserve_bytes, chunk_bytes)) return false;

    /* Pre-allocate metadata arrays from arena */
    const uint32_t initial_cap = 4096;
    la->meta.capacity = initial_cap;
    size_t meta_bytes = (size_t)initial_cap * MU_LINE_META_STRIDE;
    void *meta_mem = mu_vm_arena_alloc(&la->arena, meta_bytes, 64);
    if (!meta_mem) return false;

    la->meta.starts = (uint32_t*)meta_mem;
    la->meta.lens   = (uint32_t*)((uint8_t*)meta_mem + (size_t)initial_cap * 4);
    la->meta.flags  = (uint16_t*)((uint8_t*)meta_mem + (size_t)initial_cap * 8);
    la->meta.gap_start = 0;
    la->meta.gap_end   = initial_cap;
    la->meta.owns_mem  = true;

    /* Index: allocate level 0 nodes */
    la->idx.level_count = 1;
    la->idx.leaf_count  = 1;
    la->idx.total_lines = 0;
    la->idx.levels[0] = (mu_line_index_node*)mu_vm_arena_alloc(&la->arena,
                                                                 sizeof(mu_line_index_node), 64);
    if (!la->idx.levels[0]) return false;
    MU_MEMSET(la->idx.levels[0], 0, sizeof(mu_line_index_node));

    return true;
}

MU_INLINE void mu_line_array_destroy(mu_line_array *la)
{
    if (!la) return;
    mu_vm_arena_destroy(&la->arena);
    MU_MEMSET(la, 0, sizeof(*la));
}

/* ---------------------------------------------------------------------------
 * Build from text (initial load)
 * --------------------------------------------------------------------------- */

MU_INLINE bool mu_line_array_build(mu_line_array *la, const char *text, uint32_t len)
{
    MU_ASSERT(la && text);
    if (!la || !text) return false;

    /* First pass: count lines */
    uint32_t lines = 1;
    for (uint32_t i = 0; i < len; ++i) if (text[i] == '\n') ++lines;

    /* Ensure capacity */
    if (lines > la->meta.capacity) {
        uint32_t new_cap = la->meta.capacity;
        while (new_cap < lines) new_cap *= 2;
        size_t meta_bytes = (size_t)new_cap * MU_LINE_META_STRIDE;
        void *new_mem = mu_vm_arena_alloc(&la->arena, meta_bytes, 64);
        if (!new_mem) return false;
        /* Copy old data (no gap yet) */
        MU_MEMCPY(new_mem, la->meta.starts, la->meta.count * MU_LINE_META_STRIDE);
        la->meta.starts = (uint32_t*)new_mem;
        la->meta.lens   = (uint32_t*)((uint8_t*)new_mem + (size_t)new_cap * 4);
        la->meta.flags  = (uint16_t*)((uint8_t*)new_mem + (size_t)new_cap * 8);
        la->meta.gap_end = new_cap;
        la->meta.capacity = new_cap;
    }

    /* Second pass: fill metadata */
    uint32_t line = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i < len; ++i) {
        if (text[i] == '\n') {
            uint32_t phys = mu_line_array__meta_idx(la, line);
            la->meta.starts[phys] = start;
            la->meta.lens[phys]   = i - start;
            la->meta.flags[phys]  = MU_LINE_HAS_NL;
            start = i + 1;
            ++line;
        }
    }
    /* Last line (may not end in \n) */
    if (line < lines) {
        uint32_t phys = mu_line_array__meta_idx(la, line);
        la->meta.starts[phys] = start;
        la->meta.lens[phys]   = len - start;
        la->meta.flags[phys]  = 0;
    }

    la->meta.count = lines;
    la->meta.gap_start = lines;
    la->text_len = len;

    /* Build index */
    mu_line_array_rebuild_all(la);
    return true;
}

/* ---------------------------------------------------------------------------
 * High-frequency reads
 * --------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_line_array_line_offset(const mu_line_array *la, uint32_t line)
{
    MU_ASSERT(la && line < la->meta.count);
    if (line >= la->meta.count) return la->text_len;
    uint32_t phys = mu_line_array__meta_idx(la, line);
    return la->meta.starts[phys];
}

MU_INLINE void mu_line_array_line_range(const mu_line_array *la, uint32_t line,
                                        uint32_t *out_offset, uint32_t *out_len)
{
    MU_ASSERT(la && line < la->meta.count);
    if (line >= la->meta.count) { *out_offset = la->text_len; *out_len = 0; return; }
    uint32_t phys = mu_line_array__meta_idx(la, line);
    *out_offset = la->meta.starts[phys];
    *out_len    = la->meta.lens[phys];
}

MU_INLINE mu_line_pos mu_line_array_offset_to_pos(const mu_line_array *la, uint32_t offset)
{
    mu_line_pos pos = {0, 0, 0};
    MU_ASSERT(la);
    if (!la || la->meta.count == 0) return pos;

    if (offset >= la->text_len) {
        pos.line = la->meta.count - 1;
        pos.offset = la->text_len;
        pos.col = la->meta.lens[mu_line_array__meta_idx(la, pos.line)];
        return pos;
    }

    /* Walk index top-down */
    uint32_t line = 0;
    uint32_t node_idx = 0;

    for (uint32_t level = la->idx.level_count; level-- > 0; ) {
        mu_line_index_node *node = &la->idx.levels[level][node_idx];
        uint32_t in_node = mu_line_array__search_node_prefix(node->prefix, offset);
        if (level == 0) {
            line += in_node;
            break;
        }
        line += in_node * MU_LINE_INDEX_FANOUT;
        node_idx = node_idx * MU_LINE_INDEX_FANOUT + in_node;
    }

    if (line >= la->meta.count) line = la->meta.count - 1;
    uint32_t line_off = mu_line_array_line_offset(la, line);
    pos.line = line;
    pos.offset = line_off;
    pos.col = offset - line_off;
    return pos;
}

MU_INLINE uint32_t mu_line_array_pos_to_offset(const mu_line_array *la, uint32_t line, uint32_t col)
{
    uint32_t off = mu_line_array_line_offset(la, line);
    uint32_t len = la->meta.lens[mu_line_array__meta_idx(la, line)];
    if (col > len) col = len;
    return off + col;
}

MU_INLINE uint32_t mu_line_array_walk(const mu_line_array *la, uint32_t line, uint32_t k,
                                      uint32_t *out_offsets, uint32_t *out_lens)
{
    MU_ASSERT(la);
    if (!la || line >= la->meta.count) return 0;
    uint32_t max = la->meta.count - line;
    if (k > max) k = max;

    uint32_t phys = mu_line_array__meta_idx(la, line);
    uint32_t gap = la->meta.gap_end - la->meta.gap_start;

    /* Contiguous until gap */
    uint32_t first_chunk = (phys + k <= la->meta.gap_start) ? k :
                           (phys < la->meta.gap_start) ? la->meta.gap_start - phys : 0;

    if (first_chunk) {
        MU_MEMCPY(out_offsets, la->meta.starts + phys, first_chunk * 4);
        MU_MEMCPY(out_lens,    la->meta.lens   + phys, first_chunk * 4);
    }

    if (k > first_chunk) {
        uint32_t second_phys = la->meta.gap_end + (phys + first_chunk - la->meta.gap_start);
        uint32_t second_chunk = k - first_chunk;
        MU_MEMCPY(out_offsets + first_chunk, la->meta.starts + second_phys, second_chunk * 4);
        MU_MEMCPY(out_lens   + first_chunk, la->meta.lens   + second_phys, second_chunk * 4);
    }
    return k;
}

MU_INLINE void mu_line_array_viewport_lines(const mu_line_array *la,
                                            uint32_t start_off, uint32_t end_off,
                                            uint32_t *out_first, uint32_t *out_last)
{
    MU_ASSERT(la);
    if (!la || la->meta.count == 0) { *out_first = 0; *out_last = 0; return; }

    /* Use index to get close, then AVX scan */
    mu_line_pos first_pos = mu_line_array_offset_to_pos(la, start_off);
    mu_line_pos last_pos  = mu_line_array_offset_to_pos(la, end_off);

    *out_first = first_pos.line;
    *out_last  = last_pos.line;

    /* Refine with AVX scan around boundaries (±64 lines) */
    uint32_t scan_start = (*out_first > 64) ? *out_first - 64 : 0;
    uint32_t scan_end   = *out_last + 64 < la->meta.count ? *out_last + 64 : la->meta.count - 1;

    /* Collect starts in contiguous buffer for AVX scan */
    uint32_t scan_count = scan_end - scan_start + 1;
    uint32_t temp[256]; /* max scan window */
    if (scan_count <= 256) {
        mu_line_array_walk(la, scan_start, scan_count, temp, NULL);
        uint32_t first = mu_line_array__avx2_find_first_ge(temp, scan_count, start_off);
        uint32_t last  = mu_line_array__avx2_find_last_le(temp, scan_count, end_off);
        if (first < scan_count) *out_first = scan_start + first;
        if (last != UINT32_MAX) *out_last = scan_start + last;
    }
}

/* ---------------------------------------------------------------------------
 * Gap buffer management
 * --------------------------------------------------------------------------- */

MU_INLINE bool mu_line_array__grow_meta(mu_line_array *la, uint32_t need)
{
    uint32_t new_cap = la->meta.capacity;
    while (new_cap - la->meta.count < need) new_cap *= 2;
    if (new_cap == la->meta.capacity) return true;

    size_t meta_bytes = (size_t)new_cap * MU_LINE_META_STRIDE;
    void *new_mem = mu_vm_arena_alloc(&la->arena, meta_bytes, 64);
    if (!new_mem) return false;

    uint32_t gap = la->meta.gap_end - la->meta.gap_start;

    /* Copy [0, gap_start) */
    MU_MEMCPY(new_mem, la->meta.starts, (size_t)la->meta.gap_start * MU_LINE_META_STRIDE);
    /* Copy [gap_end, count) → [new_cap - gap, new_cap) */
    uint8_t *src = (uint8_t*)la->meta.starts + (size_t)la->meta.gap_end * MU_LINE_META_STRIDE;
    uint8_t *dst = (uint8_t*)new_mem + (size_t)(new_cap - gap) * MU_LINE_META_STRIDE;
    MU_MEMCPY(dst, src, (size_t)(la->meta.count - la->meta.gap_end) * MU_LINE_META_STRIDE);

    la->meta.starts = (uint32_t*)new_mem;
    la->meta.lens   = (uint32_t*)((uint8_t*)new_mem + (size_t)new_cap * 4);
    la->meta.flags  = (uint16_t*)((uint8_t*)new_mem + (size_t)new_cap * 8);
    la->meta.gap_end = new_cap - gap;
    la->meta.capacity = new_cap;
    return true;
}

MU_INLINE bool mu_line_array_ensure_gap(mu_line_array *la, uint32_t line, uint32_t need)
{
    MU_ASSERT(la);
    if (!la) return false;
    if (line > la->meta.count) line = la->meta.count;

    uint32_t gap = la->meta.gap_end - la->meta.gap_start;
    if (gap >= need && line == la->meta.gap_start) return true;

    if (gap < need) {
        if (!mu_line_array__grow_meta(la, need - gap)) return false;
        gap = la->meta.gap_end - la->meta.gap_start;
    }

    /* Move gap to 'line' */
    if (line < la->meta.gap_start) {
        uint32_t move = la->meta.gap_start - line;
        uint8_t *src = (uint8_t*)la->meta.starts + (size_t)line * MU_LINE_META_STRIDE;
        uint8_t *dst = (uint8_t*)la->meta.starts + (size_t)(la->meta.gap_end - move) * MU_LINE_META_STRIDE;
        MU_MEMCPY(dst, src, (size_t)move * MU_LINE_META_STRIDE);
        la->meta.gap_start = line;
        la->meta.gap_end   = line + gap;
    } else if (line > la->meta.gap_start) {
        uint32_t move = line - la->meta.gap_start;
        uint8_t *src = (uint8_t*)la->meta.starts + (size_t)la->meta.gap_end * MU_LINE_META_STRIDE;
        uint8_t *dst = (uint8_t*)la->meta.starts + (size_t)la->meta.gap_start * MU_LINE_META_STRIDE;
        MU_MEMCPY(dst, src, (size_t)move * MU_LINE_META_STRIDE);
        la->meta.gap_start = line;
        la->meta.gap_end   = line + gap;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Index maintenance
 * --------------------------------------------------------------------------- */

MU_INLINE void mu_line_array__rebuild_node(mu_line_array *la, uint32_t level, uint32_t node_idx)
{
    mu_line_index_node *node = &la->idx.levels[level][node_idx];
    uint32_t lines_per_node = 1;
    for (uint32_t i = 0; i < level; ++i) lines_per_node *= MU_LINE_INDEX_FANOUT;

    uint32_t start_line = node_idx * lines_per_node;
    if (start_line >= la->meta.count) {
        MU_MEMSET(node->prefix, 0, sizeof(node->prefix));
        return;
    }

    node->prefix[0] = 0;
    uint32_t sum = 0;

    if (level == 0) {
        /* Leaf: sum directly from metadata */
        for (uint32_t i = 0; i < MU_LINE_INDEX_FANOUT; ++i) {
            uint32_t line = start_line + i;
            if (line >= la->meta.count) {
                node->prefix[i + 1] = sum;
                continue;
            }
            sum += la->meta.lens[mu_line_array__meta_idx(la, line)];
            node->prefix[i + 1] = sum;
        }
    } else {
        /* Internal: sum from child nodes */
        for (uint32_t i = 0; i < MU_LINE_INDEX_FANOUT; ++i) {
            mu_line_index_node *child = &la->idx.levels[level - 1][node_idx * MU_LINE_INDEX_FANOUT + i];
            sum += child->prefix[MU_LINE_INDEX_FANOUT];
            node->prefix[i + 1] = sum;
        }
    }
}

MU_INLINE void mu_line_array__ensure_index_level(mu_line_array *la, uint32_t level)
{
    if (level < la->idx.level_count) return;
    while (la->idx.level_count <= level) {
        uint32_t prev_nodes = (la->idx.level_count == 0) ? 1 :
                              la->idx.leaf_count * (la->idx.level_count == 1 ? 1 : MU_LINE_INDEX_FANOUT);
        uint32_t nodes = (prev_nodes + MU_LINE_INDEX_FANOUT - 1) / MU_LINE_INDEX_FANOUT;
        if (nodes == 0) nodes = 1;

        la->idx.levels[la->idx.level_count] = (mu_line_index_node*)
            mu_vm_arena_alloc(&la->arena, (size_t)nodes * sizeof(mu_line_index_node), 64);
        if (!la->idx.levels[la->idx.level_count]) return;
        MU_MEMSET(la->idx.levels[la->idx.level_count], 0,
                  (size_t)nodes * sizeof(mu_line_index_node));
        la->idx.level_count++;
    }
}

MU_INLINE void mu_line_array_rebuild_all(mu_line_array *la)
{
    MU_ASSERT(la);
    if (!la) return;

    /* Calculate required levels */
    uint32_t nodes = (la->meta.count + MU_LINE_INDEX_FANOUT - 1) / MU_LINE_INDEX_FANOUT;
    if (nodes == 0) nodes = 1;
    la->idx.leaf_count = nodes;
    la->idx.total_lines = la->meta.count;

    uint32_t levels = 1;
    while (nodes > 1) { nodes = (nodes + MU_LINE_INDEX_FANOUT - 1) / MU_LINE_INDEX_FANOUT; levels++; }

    mu_line_array__ensure_index_level(la, levels - 1);

    /* Rebuild bottom-up */
    for (uint32_t level = 0; level < la->idx.level_count; ++level) {
        uint32_t level_nodes = (level == 0) ? la->idx.leaf_count :
                               (la->idx.leaf_count + (1u << (level * 6)) - 1) >> (level * 6); /* approx */
        /* Exact: */
        level_nodes = 1;
        for (uint32_t l = 0; l < level; ++l) level_nodes *= MU_LINE_INDEX_FANOUT;
        level_nodes = (la->idx.leaf_count + level_nodes - 1) / level_nodes;
        if (level_nodes == 0) level_nodes = 1;

        for (uint32_t i = 0; i < level_nodes; ++i) {
            mu_line_array__rebuild_node(la, level, i);
        }
    }
}

/* Incremental update: adjust prefix sums from leaf up for lines [first, last) */
MU_INLINE void mu_line_array_rebuild_range(mu_line_array *la, uint32_t first, uint32_t last)
{
    if (!la || first >= last || first >= la->meta.count) return;
    if (last > la->meta.count) last = la->meta.count;

    /* Rebuild affected leaf nodes */
    uint32_t first_node = first / MU_LINE_INDEX_FANOUT;
    uint32_t last_node  = (last - 1) / MU_LINE_INDEX_FANOUT;

    for (uint32_t n = first_node; n <= last_node; ++n) {
        mu_line_array__rebuild_node(la, 0, n);
    }

    /* Propagate up */
    for (uint32_t level = 1; level < la->idx.level_count; ++level) {
        first_node /= MU_LINE_INDEX_FANOUT;
        last_node  /= MU_LINE_INDEX_FANOUT;
        for (uint32_t n = first_node; n <= last_node; ++n) {
            mu_line_array__rebuild_node(la, level, n);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Mutations
 * --------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_line_array_insert_lines(mu_line_array *la, uint32_t line,
                                              const uint32_t *lens, uint32_t n)
{
    MU_ASSERT(la && n > 0);
    if (!la || n == 0) return line;
    if (line > la->meta.count) line = la->meta.count;

    /* Ensure gap at insertion point */
    if (!mu_line_array_ensure_gap(la, line, n)) return UINT32_MAX;

    /* Initialize new lines in gap */
    uint32_t base_offset = (line == 0) ? 0 :
                           la->meta.starts[mu_line_array__meta_idx(la, line - 1)] +
                           la->meta.lens[mu_line_array__meta_idx(la, line - 1)];

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t phys = la->meta.gap_start + i;
        la->meta.starts[phys] = base_offset;
        la->meta.lens[phys]   = lens[i];
        la->meta.flags[phys]  = (i + 1 < n) ? MU_LINE_HAS_NL : 0;
        base_offset += lens[i];
    }

    la->meta.gap_start += n;
    la->meta.count += n;
    la->text_len += base_offset - ((line == 0) ? 0 :
        la->meta.starts[mu_line_array__meta_idx(la, line - 1)] +
        la->meta.lens[mu_line_array__meta_idx(la, line - 1)]);

    /* Rebuild index for affected range */
    mu_line_array_rebuild_range(la, line, line + n);

    /* Adjust subsequent line offsets */
    uint32_t added = base_offset - ((line == 0) ? 0 :
        la->meta.starts[mu_line_array__meta_idx(la, line - 1)] +
        la->meta.lens[mu_line_array__meta_idx(la, line - 1)]);
    if (added > 0) {
        for (uint32_t i = line + n; i < la->meta.count; ++i) {
            la->meta.starts[mu_line_array__meta_idx(la, i)] += added;
        }
        /* Index will be fixed by rebuild_range */
        mu_line_array_rebuild_range(la, line + n, la->meta.count);
    }

    return line;
}

MU_INLINE uint32_t mu_line_array_delete_lines(mu_line_array *la, uint32_t line, uint32_t n)
{
    MU_ASSERT(la);
    if (!la || n == 0 || line >= la->meta.count) return 0;
    if (line + n > la->meta.count) n = la->meta.count - line;

    if (!mu_line_array_ensure_gap(la, line, 0)) return 0;

    /* Calculate removed bytes */
    uint32_t removed = 0;
    for (uint32_t i = 0; i < n; ++i) {
        removed += la->meta.lens[mu_line_array__meta_idx(la, line + i)];
    }

    /* Move gap to cover deleted lines */
    la->meta.gap_start = line;
    la->meta.gap_end   = line + n;
    la->meta.count    -= n;
    la->text_len      -= removed;

    /* Adjust subsequent line offsets */
    for (uint32_t i = line; i < la->meta.count; ++i) {
        la->meta.starts[mu_line_array__meta_idx(la, i)] -= removed;
    }

    /* Rebuild index */
    mu_line_array_rebuild_range(la, line, la->meta.count);
    return n;
}

MU_INLINE void mu_line_array_adjust_line(mu_line_array *la, uint32_t line, int32_t delta)
{
    if (!la || line >= la->meta.count || delta == 0) return;

    uint32_t phys = mu_line_array__meta_idx(la, line);
    int32_t new_len = (int32_t)la->meta.lens[phys] + delta;
    if (new_len < 0) new_len = 0;

    int32_t diff = new_len - (int32_t)la->meta.lens[phys];
    if (diff == 0) return;

    la->meta.lens[phys] = (uint32_t)new_len;
    la->text_len += diff;

    /* Adjust subsequent line starts */
    for (uint32_t i = line + 1; i < la->meta.count; ++i) {
        la->meta.starts[mu_line_array__meta_idx(la, i)] += diff;
    }

    /* Incremental index update */
    uint32_t node = line / MU_LINE_INDEX_FANOUT;
    mu_line_array__rebuild_node(la, 0, node);
    for (uint32_t level = 1; level < la->idx.level_count; ++level) {
        node /= MU_LINE_INDEX_FANOUT;
        mu_line_array__rebuild_node(la, level, node);
    }
}

/* ---------------------------------------------------------------------------
 * Text edit integration
 * --------------------------------------------------------------------------- */

MU_INLINE void mu_line_array_on_text_edit(mu_line_array *la,
                                          uint32_t at, uint32_t old_len, uint32_t new_len)
{
    if (!la) return;
    if (old_len == new_len) return;

    /* Find affected line */
    mu_line_pos pos = mu_line_array_offset_to_pos(la, at);
    uint32_t line = pos.line;

    /* Adjust this line's length */
    int32_t delta = (int32_t)new_len - (int32_t)old_len;
    mu_line_array_adjust_line(la, line, delta);

    /* If newline count changed, need structural update */
    /* This is a simplification - full implementation would count \n in inserted text */
}

/* ---------------------------------------------------------------------------
 * Introspection
 * --------------------------------------------------------------------------- */

MU_INLINE uint32_t mu_line_array_count(const mu_line_array *la)
{
    return la ? la->meta.count : 0;
}

MU_INLINE uint32_t mu_line_array_text_len(const mu_line_array *la)
{
    return la ? la->text_len : 0;
}

MU_INLINE size_t mu_line_array_memory_usage(const mu_line_array *la)
{
    if (!la) return 0;
    size_t meta = (size_t)la->meta.capacity * MU_LINE_META_STRIDE;
    size_t idx = 0;
    for (uint32_t l = 0; l < la->idx.level_count; ++l) {
        uint32_t nodes = la->idx.leaf_count;
        for (uint32_t i = 0; i < l; ++i) nodes = (nodes + MU_LINE_INDEX_FANOUT - 1) / MU_LINE_INDEX_FANOUT;
        if (nodes == 0) nodes = 1;
        idx += (size_t)nodes * sizeof(mu_line_index_node);
    }
    return meta + idx + mu_vm_arena_committed(&la->arena);
}

MU_END_EXTERN_C

#endif /* MU_LINE_ARRAY_H */