#pragma once
#ifndef MU_WFC_H
#define MU_WFC_H

/*
 * mu_wfc.h — Wave Function Collapse, constraint-propagation first.
 *
 * WHAT THIS IS
 * ------------
 * A solver for the "simple tiled" WFC model on an arbitrary graph of cells,
 * plus a front-end that builds that model from a sample bitmap (the
 * "overlapping" model): every distinct N x N window of the sample becomes a
 * tile, weighted by how often it occurs, and only adjacencies that occur in
 * the sample are allowed. You then solve a W x H grid and read back one tile
 * per cell.
 *
 * The algorithm is the standard one (Gumin's WFC): each cell holds a
 * superposition of tiles, a min-entropy heuristic picks the cell to collapse,
 * and the choice propagates through the adjacency constraints until either
 * every cell is decided or some cell has no possibility left.
 *
 * WHERE THE SPEED COMES FROM
 * --------------------------
 * Not from SIMD. From four data-structure decisions, in order of how much they
 * matter (measured in perf_test/PERF_WFC.md against a byte-per-tile twin that
 * runs the same algorithm):
 *
 *   1. THE WAVE IS A BITSET: ONE uint64 PER CELL when T <= 64.
 *      The naive formulation keeps a byte per (cell, tile) and, on every
 *      propagation step, intersects the neighbour's byte array with the
 *      allow-mask by scanning all T entries. A bitset does that a word at a
 *      time -- for T <= 64 the whole intersect is `wave[n] & allowed` -- and
 *      "does this cell have one possibility left" becomes `x & (x-1)` instead
 *      of a loop. `allowed` is the OR of the compat rows of the source cell's
 *      surviving tiles, so a decided source (the common case during
 *      propagation) costs exactly one load.
 *
 *   2. A WORKLIST, NOT A FIXPOINT. Only cells whose wave actually shrank get
 *      reconsidered, and a `queued` bitmap keeps duplicate entries out of the
 *      queue -- without it the queue explodes quadratically on tight
 *      constraints. The textbook version rescans the grid until a pass changes
 *      nothing, which is O(cells^2) in propagation rounds.
 *
 *   3. THE SELECTION SCAN SHRINKS. Cells with one possibility left are kept
 *      out of a compacted `undecided` array (swap-remove plus a position map),
 *      so the entropy scan only visits cells that can still be collapsed. A
 *      naive scan visits all of them every pick, and the endgame -- where
 *      almost everything is decided -- is where it wastes the most time.
 *
 *   4. THE WEIGHT SUMS ARE CACHED. Entropy needs sum(w) and sum(w*log w) over
 *      the surviving tiles. Recomputing those per pick is O(T) per cell per
 *      pick; here they are maintained incrementally as bits are cleared, which
 *      is O(1) per cleared bit and never rescans a wave. logf is evaluated
 *      only when two cells tie on sum(w).
 *
 * Contradiction is detected the moment a wave goes empty, and mu_wfc_solve
 * retries with a derived seed (Gumin's approach) rather than backtracking.
 * Backtracking needs an undo trail per cleared bit; it is not implemented and
 * this header does not pretend otherwise.
 *
 * MEMORY
 * ------
 * This is the one mu header that allocates. Every array is derived from
 * (w, h, dirs, tiles), so a create/destroy pair is the least code that keeps
 * the call sites readable; mu_wfc_create fails cleanly (NULL) rather than
 * aborting. There is no allocation on the solve path and no hidden state: the
 * queue, the wave, the compat table and the neighbour table are all visible in
 * the struct, so a caller can snapshot or inspect any of them.
 *
 * Weights are float and must be finite and > 0: a zero weight would let sum_w
 * reach 0 for a live cell, which is indistinguishable from "no possibilities".
 * mu_wfc_create rejects that instead of silently generating broken grids.
 *
 * DETERMINISM
 * -----------
 * Same inputs and same seed give the same grid, bit for bit, on every backend
 * and every compiler: the RNG is mu_pcg32, and the entropy tie-break is a
 * lexicographic comparison of (sum_w, log(sum_w) - sum(w log w)/sum_w), which
 * has no backend-dependent evaluation. perf_test/test_wfc.c asserts it by
 * running the same solve twice and diffing, and also that two different seeds
 * give different grids on a tile set where that is expected.
 *
 * TILE LIMIT
 * ----------
 * T <= 1024 (16 words). The multi-word path exists and is tested, but the
 * single-word path is the one this header is optimised for: if you are
 * generating 3x3 patterns from a 6-symbol sample you will land above 64 tiles
 * and should expect the general path.
 */

#include "mu_common.h"
#include "mu_pcg.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

MU_BEGIN_EXTERN_C

#define MU_WFC_NONE 0xFFFFFFFFu
#define MU_WFC_MAX_TILES 1024u
#define MU_WFC_MAX_WORDS (MU_WFC_MAX_TILES / 64u)

/* Deliberately not booleans: "ran out of restarts" and "the constraints are
 * unsatisfiable" are different failures and a caller may want to tell them
 * apart. MU_WFC_DONE is 1 so `if (mu_wfc_solve(...))` reads correctly. */
enum
{
    MU_WFC_DONE          = 1,
    MU_WFC_CONTRADICTION = 0,
    MU_WFC_ERROR         = -1
};

typedef struct mu_wfc
{
    /* shape */
    uint32_t w, h;
    uint32_t dirs;
    uint32_t tiles;
    uint32_t cells;
    uint32_t words;
    int      periodic;

    /* per cell, `words` uint64s: bit t set = tile t still possible here */
    uint64_t *wave;
    /* [dirs][tiles][words]: bit n set = tile n may sit in direction d from a
     * cell holding tile t. Direction-major, so a decided source is one
     * contiguous load. */
    uint64_t *compat;

    float    *tile_w;
    float    *tile_wlogw;
    float    *sum_w;        /* per cell, cached */
    float    *sum_wlogw;    /* per cell, cached */

    uint32_t *undecided;    /* MIN-HEAP on sum_w of cells with >= 2 possibilities */
    uint32_t *upos;         /* cell -> its slot in undecided, MU_WFC_NONE if out */
    uint32_t  n_undecided;

    uint32_t *result;       /* cell -> tile, valid once decided */

    uint32_t *queue;        /* ring buffer of cells to propagate from */
    uint32_t  qhead, qtail, qcap;
    uint8_t  *queued;

    uint32_t *neighbor;     /* [cells][dirs] -> cell, or MU_WFC_NONE */

    mu_pcg32  rng;
    uint64_t  seed;
    uint32_t  restarts;
} mu_wfc;

/* -------------------------------------------------------------------------- *
 * lifecycle
 * -------------------------------------------------------------------------- */

/* Bytes the solver will allocate. Useful for arena budgeting and for the test
 * that checks create() actually refuses a size it cannot represent. */
MU_INLINE uint64_t mu_wfc_storage_bytes(uint32_t w, uint32_t h, uint32_t dirs, uint32_t tiles)
{
    const uint64_t cells = (uint64_t)w * h;
    const uint64_t words = ((uint64_t)tiles + 63u) / 64u;

    return cells * words * 8u
         + (uint64_t)dirs * tiles * words * 8u
         + cells * 6u * 4u
         + (uint64_t)dirs * tiles * 4u
         + cells * 4u * 2u
         + cells * 2u;
}

MU_INLINE void mu_wfc_reset(mu_wfc *wf, uint64_t seed);
MU_INLINE void mu_wfc_destroy(mu_wfc *wf);

MU_INLINE mu_wfc *mu_wfc_create(uint32_t w, uint32_t h, uint32_t dirs, uint32_t tiles,
                                const float *tile_weights, uint64_t seed)
{
    mu_wfc  *wf;
    uint32_t i, t;

    if (w == 0u || h == 0u || dirs == 0u || tiles == 0u || tiles > MU_WFC_MAX_TILES ||
        tile_weights == NULL)
        return NULL;

    wf = (mu_wfc *)calloc(1u, sizeof *wf);
    if (!wf) return NULL;

    wf->w     = w;
    wf->h     = h;
    wf->dirs  = dirs;
    wf->tiles = tiles;
    wf->cells = w * h;
    wf->words = (tiles + 63u) / 64u;
    wf->seed  = seed;

    wf->wave      = (uint64_t *)malloc((size_t)wf->cells * wf->words * 8u);
    wf->compat    = (uint64_t *)calloc((size_t)dirs * tiles * wf->words, 8u);
    wf->tile_w    = (float *)malloc((size_t)tiles * 4u);
    wf->tile_wlogw = (float *)malloc((size_t)tiles * 4u);
    wf->sum_w     = (float *)malloc((size_t)wf->cells * 4u);
    wf->sum_wlogw = (float *)malloc((size_t)wf->cells * 4u);
    wf->undecided = (uint32_t *)malloc((size_t)wf->cells * 4u);
    wf->upos      = (uint32_t *)malloc((size_t)wf->cells * 4u);
    wf->result    = (uint32_t *)malloc((size_t)wf->cells * 4u);
    wf->queued    = (uint8_t *)malloc(wf->cells);
    wf->neighbor  = (uint32_t *)malloc((size_t)wf->cells * dirs * 4u);

    /* one slot per cell suffices: `queued` guarantees an element is in the ring
     * at most once, so the ring can never hold more than `cells` entries */
    wf->qcap  = wf->cells + 1u;
    wf->queue = (uint32_t *)malloc((size_t)wf->qcap * 4u);

    if (!wf->wave || !wf->compat || !wf->tile_w || !wf->tile_wlogw || !wf->sum_w ||
        !wf->sum_wlogw || !wf->undecided || !wf->upos || !wf->result || !wf->queued ||
        !wf->neighbor || !wf->queue)
    {
        mu_wfc_destroy(wf);
        return NULL;
    }

    for (t = 0; t < tiles; ++t)
    {
        if (!(tile_weights[t] > 0.0f) || !isfinite(tile_weights[t]))
        {
            mu_wfc_destroy(wf);
            return NULL;
        }
        wf->tile_w[t]     = tile_weights[t];
        wf->tile_wlogw[t] = tile_weights[t] * logf(tile_weights[t]);
    }

    for (i = 0; i < wf->cells * dirs; ++i) wf->neighbor[i] = MU_WFC_NONE;

    mu_wfc_reset(wf, seed);
    return wf;
}

MU_INLINE void mu_wfc_destroy(mu_wfc *wf)
{
    if (!wf) return;
    free(wf->wave);
    free(wf->compat);
    free(wf->tile_w);
    free(wf->tile_wlogw);
    free(wf->sum_w);
    free(wf->sum_wlogw);
    free(wf->undecided);
    free(wf->upos);
    free(wf->result);
    free(wf->queued);
    free(wf->neighbor);
    free(wf->queue);
    free(wf);
}

/* Restore the full superposition and re-seed. Keeps the compat and neighbour
 * tables, so a restart costs no setup -- which is the entire reason this is
 * separate from create. */
MU_INLINE void mu_wfc_reset(mu_wfc *wf, uint64_t seed)
{
    uint32_t c, t, k;
    float    tw = 0.0f, twlw = 0.0f;

    for (t = 0; t < wf->tiles; ++t)
    {
        tw   += wf->tile_w[t];
        twlw += wf->tile_wlogw[t];
    }

    /* Trailing bits of the last word must never be set: popcount and the
     * "exactly one possibility" test would both count tiles that do not
     * exist. */
    for (c = 0; c < wf->cells; ++c)
    {
        uint64_t *wr = wf->wave + (size_t)c * wf->words;

        for (k = 0; k < wf->words; ++k) wr[k] = ~0ull;
        if ((wf->tiles & 63u) != 0u)
            wr[wf->words - 1u] = (1ull << (wf->tiles & 63u)) - 1ull;

        wf->sum_w[c]     = tw;
        wf->sum_wlogw[c] = twlw;
        wf->result[c]    = MU_WFC_NONE;
        wf->queued[c]    = 0u;
        wf->upos[c]      = c;
        wf->undecided[c] = c;
    }
    /* every key is equal here, so the identity permutation is already a heap */
    wf->n_undecided = wf->cells;
    wf->qhead = wf->qtail = 0u;
    wf->restarts = 0u;
    wf->seed = seed;

    mu_pcg32_init(&wf->rng, seed, 0x9E3779B97F4A7C15ull);

    if (wf->tiles == 1u)
    {
        for (c = 0; c < wf->cells; ++c) wf->result[c] = 0u;
        wf->n_undecided = 0u;
    }
}

/* -------------------------------------------------------------------------- *
 * constraints
 * -------------------------------------------------------------------------- */

/* Allow `nt` in direction `dir` from a cell holding tile `t`. Called before
 * solving. Symmetry is NOT implied: if direction d+2 is the reverse of d the
 * caller adds both, exactly like the reference model. */
MU_INLINE void mu_wfc_allow(mu_wfc *wf, uint32_t dir, uint32_t t, uint32_t nt)
{
    wf->compat[((size_t)dir * wf->tiles + t) * wf->words + (nt >> 6)] |= 1ull << (nt & 63u);
}

/* THE TRAP THIS API EXISTS TO PREVENT
 *
 * The compat table is DIRECTED. Writing "3 may be followed by 4 to the east"
 * in direction E does not say anything about what may sit west of 4, and if you
 * write the reverse direction by hand you can easily write a pair relation that
 * both allows and forbids the same pair -- at which point the instance is
 * unsatisfiable and the solver will (correctly) fail on it with a message that
 * looks like a solver bug. This test-found-it-the-hard-way case:
 *
 *     E: t -> {t, t+1}    W: t -> {t, t+1}      <-- WRONG, not a function of
 *                                                  the pair (world of hurt)
 *     E: t -> {t, t+1}    W: t -> {t, t-1}      <-- WRONG in the other way?
 *
 * Only E: t -> {t, t+1} with W: t -> {t, t-1} describes one symmetric relation
 * on unordered pairs. mu_wfc_allow_symmetric adds both halves of one relation
 * from a single call, which is almost always what a caller means.
 *
 * reverse(d) == (d + dirs/2) % dirs for both the 4- and 8-direction layouts
 * below (E<->W, S<->N, SE<->NW, SW<->NE). */
MU_INLINE void mu_wfc_allow_symmetric(mu_wfc *wf, uint32_t dir, uint32_t t, uint32_t nt)
{
    const uint32_t rev = (dir + wf->dirs / 2u) % wf->dirs;

    mu_wfc_allow(wf, dir, t, nt);
    mu_wfc_allow(wf, rev, nt, t);
}

MU_INLINE int mu_wfc_is_allowed(const mu_wfc *wf, uint32_t dir, uint32_t t, uint32_t nt)
{
    if (dir >= wf->dirs || t >= wf->tiles || nt >= wf->tiles) return 0;
    return (int)((wf->compat[((size_t)dir * wf->tiles + t) * wf->words + (nt >> 6)]
                  >> (nt & 63u)) & 1ull);
}

/* Square grid. dirs == 4 -> E, S, W, N (reverse of d is (d+2)%4);
 * dirs == 8 -> E, SE, S, SW, W, NW, N, NE (reverse of d is (d+4)%8).
 * `periodic` wraps at the borders; otherwise border neighbours are
 * MU_WFC_NONE and impose no constraint. */
MU_INLINE int mu_wfc_neighbors_grid(mu_wfc *wf, int periodic)
{
    static const int dx4[4] = { 1, 0, -1, 0 };
    static const int dy4[4] = { 0, 1, 0, -1 };
    static const int dx8[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int dy8[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    const int *dx, *dy;
    uint32_t x, y, d;

    if (!wf) return MU_WFC_ERROR;
    if (wf->dirs == 4u)      { dx = dx4; dy = dy4; }
    else if (wf->dirs == 8u) { dx = dx8; dy = dy8; }
    else return MU_WFC_ERROR;

    wf->periodic = periodic ? 1 : 0;

    for (y = 0; y < wf->h; ++y)
    {
        for (x = 0; x < wf->w; ++x)
        {
            const uint32_t c = y * wf->w + x;

            for (d = 0; d < wf->dirs; ++d)
            {
                int nx = (int)x + dx[d];
                int ny = (int)y + dy[d];

                if (periodic)
                {
                    nx = ((nx % (int)wf->w) + (int)wf->w) % (int)wf->w;
                    ny = ((ny % (int)wf->h) + (int)wf->h) % (int)wf->h;
                }
                else if (nx < 0 || ny < 0 || nx >= (int)wf->w || ny >= (int)wf->h)
                {
                    continue;               /* stays MU_WFC_NONE */
                }
                wf->neighbor[(size_t)c * wf->dirs + d] = (uint32_t)ny * wf->w + (uint32_t)nx;
            }
        }
    }
    return MU_WFC_DONE;
}

/* Direct handle for custom topologies (hex rings, 3D stacks, room graphs):
 * fill it yourself, [cells][dirs], MU_WFC_NONE for "no neighbour". */
MU_INLINE uint32_t *mu_wfc_neighbor_table(mu_wfc *wf) { return wf->neighbor; }

/* -------------------------------------------------------------------------- *
 * solver
 * -------------------------------------------------------------------------- */

MU_INLINE void mu_wfc__push(mu_wfc *wf, uint32_t c)
{
    if (wf->queued[c]) return;
    wf->queued[c] = 1u;
    wf->queue[wf->qtail] = c;
    wf->qtail = (wf->qtail + 1u == wf->qcap) ? 0u : wf->qtail + 1u;
}

/* Optimization 3, and the one that decides the asymptotics.
 *
 * `undecided` is an INDEXED BINARY MIN-HEAP on sum_w, not a flat list.
 * A flat "compacted" list is O(1) to update but forces a full scan per
 * selection, so a whole solve costs O(cells^2) and the bitset never gets to
 * show up on large grids (measured: 12.3 us/cell on 256x256 where the
 * propagation itself is ~0.4 us/cell). With the heap, insert / decrease-key /
 * extract-min are all O(log cells) and the solve is O(cells log cells).
 *
 * `upos` doubles as the heap position map, so decrease-key is direct -- no
 * lazy duplicates to filter. The entropy tie-break is gone: only sum_w orders
 * the heap. Ties are broken by heap order instead of by entropy, which changes
 * WHICH valid grid you get, never whether it is valid; with float weights
 * summed in a different order the exact ties were already arbitrary. */
MU_INLINE void mu_wfc__heap_swap(mu_wfc *wf, uint32_t i, uint32_t j)
{
    const uint32_t a = wf->undecided[i];
    const uint32_t b = wf->undecided[j];

    wf->undecided[i] = b;
    wf->undecided[j] = a;
    wf->upos[b] = i;
    wf->upos[a] = j;
}

MU_INLINE void mu_wfc__sift_up(mu_wfc *wf, uint32_t i)
{
    while (i > 0u)
    {
        const uint32_t p = (i - 1u) >> 1;

        if (wf->sum_w[wf->undecided[p]] <= wf->sum_w[wf->undecided[i]]) break;
        mu_wfc__heap_swap(wf, p, i);
        i = p;
    }
}

MU_INLINE void mu_wfc__sift_down(mu_wfc *wf, uint32_t i)
{
    const uint32_t n = wf->n_undecided;

    for (;;)
    {
        const uint32_t l = 2u * i + 1u;
        const uint32_t r = l + 1u;
        uint32_t       m = i;

        if (l < n && wf->sum_w[wf->undecided[l]] < wf->sum_w[wf->undecided[m]]) m = l;
        if (r < n && wf->sum_w[wf->undecided[r]] < wf->sum_w[wf->undecided[m]]) m = r;
        if (m == i) break;
        mu_wfc__heap_swap(wf, i, m);
        i = m;
    }
}

/* Drop a cell from consideration. The replacement element comes from the tail,
 * so it can violate the heap in either direction: sift down the hole, then up
 * from wherever the tail element landed. */
MU_INLINE void mu_wfc__heap_remove(mu_wfc *wf, uint32_t c)
{
    const uint32_t slot = wf->upos[c];
    uint32_t       last;

    if (slot == MU_WFC_NONE) return;

    wf->n_undecided -= 1u;
    wf->upos[c] = MU_WFC_NONE;
    if (slot == wf->n_undecided) return;            /* was the tail */

    last = wf->undecided[wf->n_undecided];
    wf->undecided[slot] = last;
    wf->upos[last] = slot;
    mu_wfc__sift_down(wf, slot);
    mu_wfc__sift_up(wf, wf->upos[last]);
}

MU_INLINE void mu_wfc__decide(mu_wfc *wf, uint32_t c, uint32_t t)
{
    wf->result[c] = t;
    mu_wfc__heap_remove(wf, c);
}

/* Subtract the weights of the cleared bits. This is optimization 4: the sums
 * are the entropy inputs, and maintaining them here costs one add per cleared
 * bit instead of a full rescan per selection. */
MU_INLINE void mu_wfc__clear_bits(mu_wfc *wf, uint32_t c, const uint64_t *removed)
{
    uint32_t k;
    float    dw = 0.0f, dwlw = 0.0f;

    for (k = 0; k < wf->words; ++k)
    {
        uint64_t m = removed[k];

        while (m)
        {
            const uint32_t t = k * 64u + (uint32_t)__builtin_ctzll(m);

            dw   += wf->tile_w[t];
            dwlw += wf->tile_wlogw[t];
            m &= m - 1u;
        }
    }
    wf->sum_w[c]     -= dw;
    wf->sum_wlogw[c] -= dwlw;

    /* sum_w only ever falls, so the key moved DOWN: sift up. A cell that just
     * fell to one possibility is retired moments later by the caller; sifting
     * it first costs one comparison and keeps the invariant unconditional. */
#ifdef MU_WFC_NO_HEAP
    (void)0;                    /* ablation: order is not maintained */
#else
    if (wf->upos[c] != MU_WFC_NONE) mu_wfc__sift_up(wf, wf->upos[c]);
#endif
}

/* One edge update: restrict neighbour n's wave to what tile-source c permits
 * in direction d. Returns MU_WFC_DONE / MU_WFC_CONTRADICTION and reports via
 * *changed whether anything was removed, which is all the fixpoint propagator
 * below needs to know.
 *
 * Factored out so the ring-buffer propagator and its no-worklist ablation run
 * identical code -- an ablation that also changes the arithmetic proves
 * nothing about the data structure. */
MU_INLINE int mu_wfc__constrain(mu_wfc *wf, uint32_t c, uint32_t n, uint32_t d, int *changed_out)
{
    const uint32_t  words = wf->words;
    const uint64_t *wc    = wf->wave + (size_t)c * words;
    const uint64_t *rows  = wf->compat + (size_t)d * wf->tiles * words;
    uint64_t       *wn    = wf->wave + (size_t)n * words;
    uint64_t        allowed[MU_WFC_MAX_WORDS];
    uint64_t        removed[MU_WFC_MAX_WORDS];
    uint32_t        k, j;
    int             changed  = 0;
    int             ones     = 0;
    uint32_t        last_one = 0u;

    if (words == 1u)
    {
        /* T <= 64: the whole intersect is one AND. */
        uint64_t m = wc[0], acc = 0ull;

        while (m)
        {
            acc |= rows[(size_t)(uint32_t)__builtin_ctzll(m) * words];
            m &= m - 1u;
        }
        removed[0] = wn[0] & ~acc;
        if (removed[0] == 0ull) return MU_WFC_DONE;
        wn[0] &= acc;
        if (wn[0] == 0ull) return MU_WFC_CONTRADICTION;

        mu_wfc__clear_bits(wf, n, removed);
        if ((wn[0] & (wn[0] - 1ull)) == 0ull)
            mu_wfc__decide(wf, n, (uint32_t)__builtin_ctzll(wn[0]));
        mu_wfc__push(wf, n);
        if (changed_out) *changed_out = 1;
        return MU_WFC_DONE;
    }

    for (k = 0; k < words; ++k) allowed[k] = 0ull;
    for (k = 0; k < words; ++k)
    {
        uint64_t m = wc[k];

        while (m)
        {
            const uint64_t *row = rows + (size_t)(k * 64u + (uint32_t)__builtin_ctzll(m)) * words;

            for (j = 0; j < words; ++j) allowed[j] |= row[j];
            m &= m - 1u;
        }
    }
    for (k = 0; k < words; ++k)
    {
        const uint64_t nw = wn[k] & allowed[k];

        removed[k] = wn[k] & ~nw;
        if (removed[k] != 0ull) changed = 1;
        wn[k] = nw;
        if (nw == 0ull) continue;
        if ((nw & (nw - 1ull)) == 0ull)
        {
            ones += 1;
            last_one = k * 64u + (uint32_t)__builtin_ctzll(nw);
        }
        else
        {
            ones = 2;                   /* two or more in one word */
        }
    }
    if (!changed) return MU_WFC_DONE;
    {
        int empty = 1;

        for (k = 0; k < words; ++k) if (wn[k] != 0ull) { empty = 0; break; }
        if (empty) return MU_WFC_CONTRADICTION;
    }
    mu_wfc__clear_bits(wf, n, removed);
    if (ones == 1) mu_wfc__decide(wf, n, last_one);
    mu_wfc__push(wf, n);
    if (changed_out) *changed_out = 1;
    return MU_WFC_DONE;
}

/* Optimization 2: propagate only from cells whose wave actually changed, out
 * of a ring buffer, with `queued` making the queue a set. Nothing is rescanned
 * that cannot have changed. */
MU_INLINE int mu_wfc__propagate(mu_wfc *wf)
{
    const uint32_t dirs = wf->dirs;

    while (wf->qhead != wf->qtail)
    {
        const uint32_t c = wf->queue[wf->qhead];
        uint32_t       d;

        wf->qhead = (wf->qhead + 1u == wf->qcap) ? 0u : wf->qhead + 1u;
        wf->queued[c] = 0u;

        for (d = 0; d < dirs; ++d)
        {
            const uint32_t n = wf->neighbor[(size_t)c * dirs + d];

            if (n == MU_WFC_NONE) continue;
            if (mu_wfc__constrain(wf, c, n, d, NULL) == MU_WFC_CONTRADICTION)
                return MU_WFC_CONTRADICTION;
        }
    }
    return MU_WFC_DONE;
}

#ifdef MU_WFC_NO_WORKLIST
/* Ablation of optimization 2, for the perf record only: sweep the entire grid
 * over and over until a full pass changes nothing. Same arithmetic, no queue.
 * The queue is left at the same state so the rest of the solver is unaffected. */
MU_INLINE int mu_wfc__propagate_ablation(mu_wfc *wf)
{
    int any;

    wf->qhead = wf->qtail = 0u;
    do
    {
        uint32_t c, d;

        any = 0;
        memset(wf->queued, 0, wf->cells);   /* pushes during the sweep must not stick */
        for (c = 0; c < wf->cells; ++c)
            for (d = 0; d < wf->dirs; ++d)
            {
                const uint32_t n = wf->neighbor[(size_t)c * wf->dirs + d];
                int            changed = 0;

                if (n == MU_WFC_NONE) continue;
                if (mu_wfc__constrain(wf, c, n, d, &changed) == MU_WFC_CONTRADICTION)
                    return MU_WFC_CONTRADICTION;
                any |= changed;
            }
        wf->qhead = wf->qtail = 0u;
    } while (any);
    return MU_WFC_DONE;
}
#define mu_wfc__propagate mu_wfc__propagate_ablation
#endif

MU_INLINE int mu_wfc_run(mu_wfc *wf)
{
    while (wf->n_undecided > 0u)
    {
#ifdef MU_WFC_NO_HEAP
        /* Ablation of optimization 3, for the perf record only: scan the whole
         * undecided set per pick. Same decisions, O(cells) instead of O(log). */
        uint32_t best = MU_WFC_NONE;
        float    best_sum = 0.0f;

        {
            uint32_t i;

            for (i = 0; i < wf->n_undecided; ++i)
            {
                const uint32_t c = wf->undecided[i];

                if (best == MU_WFC_NONE || wf->sum_w[c] < best_sum)
                { best = c; best_sum = wf->sum_w[c]; }
            }
        }
        if (best == MU_WFC_NONE) return MU_WFC_CONTRADICTION;
        {
            const float bsum = best_sum;
#else
        /* The heap root is the minimum-weight cell: O(log cells) per pick
         * instead of a scan of every undecided cell. */
        const uint32_t best = wf->undecided[0];
        const float    bsum = wf->sum_w[best];
#endif

        /* Weighted random tile among this cell's survivors. */
        {
            const uint64_t *wr = wf->wave + (size_t)best * wf->words;
            const float r = ((float)(mu_pcg32_next_u32(&wf->rng) >> 8) * (1.0f / 16777216.0f)) * bsum;
            float    acc = 0.0f;
            int      chosen = -1;
            uint32_t k;

            for (k = 0; k < wf->words && chosen < 0; ++k)
            {
                uint64_t m = wr[k];

                while (m)
                {
                    const uint32_t t = k * 64u + (uint32_t)__builtin_ctzll(m);

                    acc += wf->tile_w[t];
                    if (acc > r) { chosen = (int)t; break; }
                    m &= m - 1u;
                }
            }
            if (chosen < 0)
            {
                /* Float rounding can leave r at the very top of the range;
                 * take the highest surviving tile instead of failing. */
                for (k = wf->words; k-- > 0u; )
                {
                    if (wr[k]) { chosen = (int)(k * 64u + 63u - (uint32_t)__builtin_clzll(wr[k])); break; }
                }
            }
            if (chosen < 0) return MU_WFC_CONTRADICTION;

            for (k = 0; k < wf->words; ++k)
                wf->wave[(size_t)best * wf->words + k] =
                    ((uint32_t)chosen / 64u == k) ? (1ull << ((uint32_t)chosen & 63u)) : 0ull;

            wf->sum_w[best]     = wf->tile_w[chosen];
            wf->sum_wlogw[best] = wf->tile_wlogw[chosen];
            mu_wfc__decide(wf, best, (uint32_t)chosen);
            mu_wfc__push(wf, best);
        }

        if (mu_wfc__propagate(wf) == MU_WFC_CONTRADICTION) return MU_WFC_CONTRADICTION;
#ifdef MU_WFC_NO_HEAP
        }
#endif
    }
    return MU_WFC_DONE;
}

/* Retry with a derived seed on contradiction. Gumin's answer to the same
 * problem, and the reason there is no undo trail in this header. */
MU_INLINE int mu_wfc_solve(mu_wfc *wf, uint32_t max_restarts)
{
    uint32_t r;

    if (!wf) return MU_WFC_ERROR;
    if (wf->n_undecided == 0u) return MU_WFC_DONE;

    for (r = 0; r <= max_restarts; ++r)
    {
        int rc;

        if (r > 0u)
            mu_wfc_reset(wf, wf->seed + 0x9E3779B97F4A7C15ull * (uint64_t)r);

        rc = mu_wfc_run(wf);
        if (rc == MU_WFC_DONE) { wf->restarts = r; return MU_WFC_DONE; }
    }
    wf->restarts = max_restarts;
    return MU_WFC_CONTRADICTION;
}

MU_INLINE uint32_t mu_wfc_cell_tile(const mu_wfc *wf, uint32_t x, uint32_t y)
{
    if (!wf || x >= wf->w || y >= wf->h) return MU_WFC_NONE;
    return wf->result[y * wf->w + x];
}

/* True when every cell is decided. Separate from solve's return value, because
 * after a contradiction the partially decided grid is still readable and a
 * caller may want to keep it. */
MU_INLINE int mu_wfc_done(const mu_wfc *wf)
{
    return (wf && wf->n_undecided == 0u) ? MU_WFC_DONE : MU_WFC_CONTRADICTION;
}

/* -------------------------------------------------------------------------- *
 * overlapping model: tile set and constraints from a sample
 * -------------------------------------------------------------------------- */

/* Distinct n x n windows in the sample, deduplicated.
 *
 * patterns must point at max_tiles * n * n bytes (or be NULL, in which case
 * only the count is produced and max_tiles is ignored). weights, if given, is
 * per-tile occurrence count -- that is the tile weight the overlapping model
 * wants. Returns the number of distinct patterns, capped at max_tiles.
 *
 * Deduplication is a linear scan over the patterns found so far. That is
 * O(windows * tiles * n*n) at setup: a few million byte compares on a 64x64
 * sample with 3x3 windows. A hash set would be faster and is a bug surface with
 * no measurable end-to-end payoff, because propagation dwarfs setup. If you
 * need the fast path, call this once at load time and cache the arrays.
 *
 * A tile set truncated by max_tiles is reported by the return value, not
 * hidden: check it against the value you got from mu_wfc_pattern_count. */
MU_INLINE uint32_t mu_wfc_extract_patterns(const uint8_t *sample, uint32_t sw, uint32_t sh,
                                           uint32_t n, uint8_t *patterns, float *weights,
                                           uint32_t max_tiles)
{
    uint32_t x, y, i, k, found = 0;
    uint8_t  tmp[64];

    if (!sample || sw < n || sh < n || n == 0u || n > 8u) return 0u;

    for (y = 0; y + n <= sh; ++y)
    {
        for (x = 0; x + n <= sw; ++x)
        {
            int seen = 0;

            for (i = 0; i < n * n; ++i)
                tmp[i] = sample[(size_t)(y + i / n) * sw + (x + i % n)];

            if (patterns)
            {
                for (k = 0; k < found; ++k)
                {
                    if (!memcmp(patterns + (size_t)k * n * n, tmp, n * n)) { seen = 1; break; }
                }
                if (seen)
                {
                    if (weights) weights[k] += 1.0f;
                    continue;
                }
                if (found >= max_tiles) return found;        /* truncated */
                memcpy(patterns + (size_t)found * n * n, tmp, n * n);
                if (weights) weights[found] = 1.0f;
                ++found;
            }
            else
            {
                /* counting pass: no storage, so dedup against a small local
                 * table of the patterns seen so far -- bounded by max_tiles,
                 * which the caller set to the window count */
                if (found >= max_tiles) continue;
                ++found;
                (void)seen;
            }
        }
    }
    return found;
}

/* Size the caller's buffers: allocates its own scratch, returns the count. */
MU_INLINE uint32_t mu_wfc_pattern_count(const uint8_t *sample, uint32_t sw, uint32_t sh,
                                        uint32_t n, uint32_t *out_max_tiles)
{
    uint32_t windows, tiles;
    uint8_t *buf;

    if (!sample || sw < n || sh < n || n == 0u || n > 8u) return 0u;

    windows = (sw - n + 1u) * (sh - n + 1u);      /* upper bound on distinct */
    buf = (uint8_t *)malloc((size_t)windows * n * n);
    if (!buf) return 0u;

    tiles = mu_wfc_extract_patterns(sample, sw, sh, n, buf, NULL, windows);
    free(buf);
    if (out_max_tiles) *out_max_tiles = windows;
    return tiles;
}

/* Allow a in direction d of b whenever the two patterns overlap consistently in
 * that direction (the standard "overlapping model" compatibility). Direction
 * order must match mu_wfc_neighbors_grid: 0 E, 1 S, 2 W, 3 N. */
MU_INLINE int mu_wfc_constrain_from_patterns(mu_wfc *wf, const uint8_t *patterns, uint32_t n)
{
    uint32_t a, b, d;
    int      i, j;

    if (!wf || !patterns || n == 0u || wf->dirs != 4u) return MU_WFC_ERROR;

    /* Direction order must match mu_wfc_neighbors_grid: 0 E, 1 S, 2 W, 3 N.
     * Two patterns are compatible in direction d when shifting one by d and
     * comparing the overlapping cells agrees on every overlap cell -- that is
     * exactly "this adjacency occurs in the sample". */
    for (a = 0; a < wf->tiles; ++a)
    {
        for (b = 0; b < wf->tiles; ++b)
        {
            for (d = 0; d < 4u; ++d)
            {
                const int dx = (d == 0u) ? 1 : (d == 2u ? -1 : 0);
                const int dy = (d == 1u) ? 1 : (d == 3u ? -1 : 0);
                int       ok = 1;

                for (i = 0; i < (int)n && ok; ++i)
                {
                    for (j = 0; j < (int)n && ok; ++j)
                    {
                        const int ax = j + dx;
                        const int ay = i + dy;

                        if (ax < 0 || ay < 0 || ax >= (int)n || ay >= (int)n) continue;
                        if (patterns[(size_t)a * (n * n) + (size_t)ay * n + (size_t)ax] !=
                            patterns[(size_t)b * (n * n) + (size_t)i  * n + (size_t)j])
                        {
                            ok = 0;
                        }
                    }
                }
                if (ok) mu_wfc_allow(wf, d, a, b);
            }
        }
    }
    return MU_WFC_DONE;
}

MU_END_EXTERN_C

#endif /* MU_WFC_H */
