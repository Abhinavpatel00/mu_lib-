/* test_wfc.c — correctness gate for mu/mu_wfc.h
 *
 * There is no cglm to diff against here, so the oracle is different: a WFC
 * output is correct iff it is a VALID tiling under the same constraint table
 * the solver was given, every cell is decided, and the same seed reproduces it
 * bit for bit. Every test below therefore ends in one of:
 *
 *   - check_valid():   for every cell and direction, the neighbour's tile is in
 *                      the allow-mask of this cell's tile. This is the property
 *                      that makes the output usable; a solver that produces
 *                      pretty-but-illegal grids fails here.
 *   - check_determinism(): solve twice from the same seed, diff all cells.
 *   - check_windows(): for the overlapping model, every n x n window of the
 *                      output must occur in the input sample -- i.e. the output
 *                      is a valid recombination of what it was shown.
 *
 * Exits non-zero on the first failure summary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "mu_wfc.h"

static int g_checks = 0;
static int g_fails  = 0;

static void check_i(const char *what, long long got, long long want)
{
    ++g_checks;
    if (got != want)
    {
        ++g_fails;
        if (g_fails <= 20)
            printf("  FAIL %-46s got=%lld want=%lld\n", what, got, want);
    }
}

/* ---------------------------------------------------------------- helpers */

static int check_valid(const mu_wfc *wf, const char *what)
{
    uint32_t c, d;
    int      bad = 0;

    for (c = 0; c < wf->cells; ++c)
    {
        const uint32_t t = wf->result[c];

        if (t == MU_WFC_NONE)
        {
            ++g_checks; ++g_fails;
            if (g_fails <= 20) printf("  FAIL %-46s cell %u undecided\n", what, c);
            return 0;
        }
        for (d = 0; d < wf->dirs; ++d)
        {
            const uint32_t n = wf->neighbor[(size_t)c * wf->dirs + d];
            const uint32_t nt = (n == MU_WFC_NONE) ? MU_WFC_NONE : wf->result[n];

            ++g_checks;
            if (n == MU_WFC_NONE) continue;
            if (!mu_wfc_is_allowed(wf, d, t, nt))
            {
                ++g_fails;
                ++bad;
                if (g_fails <= 20)
                    printf("  FAIL %-46s cell %u tile %u -> dir %u cell %u tile %u ILLEGAL\n",
                           what, c, t, d, n, nt);
            }
        }
    }
    return bad == 0;
}

/* solve twice from an identical setup and require bit-identical grids */
static int check_determinism(mu_wfc *wf, uint32_t *scratch, const char *what)
{
    int rc1, rc2;

    rc1 = mu_wfc_solve(wf, 8);
    memcpy(scratch, wf->result, (size_t)wf->cells * 4u);
    mu_wfc_reset(wf, wf->seed);
    rc2 = mu_wfc_solve(wf, 8);

    check_i(what, rc1, rc2);
    check_i(what, memcmp(scratch, wf->result, (size_t)wf->cells * 4u), 0);
    return rc1 == MU_WFC_DONE;
}

/* Every n*n window of the OUTPUT IMAGE must exist in the pattern set.
 *
 * The subtlety that cost me a debugging session: a cell holds a pattern, but
 * the pixel it contributes to the output image is that pattern's top-left
 * element, `patterns[tile][0]`. So the window at (x,y) is assembled from the
 * pixels of the n*n CELLS at (x..x+n-1, y..y+n-1), not from their tile indices.
 * Comparing tile indices is meaningless -- neighbouring cells hold different
 * patterns by construction -- and reports garbage.
 *
 * With pairwise strip consistency (the rule mu_wfc_constrain_from_patterns
 * writes), the n*n pixel window at (x,y) must equal the pattern held by the
 * cell at (x,y) exactly: horizontal consistency propagates row 0 of that
 * pattern across the cells to its east, vertical consistency propagates it
 * south. This checks both the membership and that stronger identity. */
static int check_windows(const mu_wfc *wf, const uint8_t *patterns, uint32_t tiles, uint32_t n,
                         const char *what)
{
    uint32_t x, y, a, i, j;
    int      bad = 0;

    for (y = 0; y + n <= wf->h; ++y)
    {
        for (x = 0; x + n <= wf->w; ++x)
        {
            uint8_t win[64];
            int     found = 0;

            for (j = 0; j < n; ++j)
                for (i = 0; i < n; ++i)
                {
                    const uint32_t t = mu_wfc_cell_tile(wf, x + i, y + j);

                    win[j * n + i] = patterns[(size_t)t * n * n];   /* pixel = pattern[0][0] */
                }

            for (a = 0; a < tiles; ++a)
                if (!memcmp(patterns + (size_t)a * n * n, win, n * n)) { found = 1; break; }

            ++g_checks;
            if (!found)
            {
                ++g_fails;
                ++bad;
                if (g_fails <= 20) printf("  FAIL %-46s window at (%u,%u) not in sample\n", what, x, y);
            }

            /* stronger: the window must be exactly the pattern of cell (x,y) */
            ++g_checks;
            if (memcmp(patterns + (size_t)mu_wfc_cell_tile(wf, x, y) * n * n, win, n * n))
            {
                ++g_fails;
                ++bad;
                if (g_fails <= 20)
                    printf("  FAIL %-46s window at (%u,%u) != its cell\'s pattern\n", what, x, y);
            }
        }
    }
    return bad == 0;
}

/* ------------------------------------------------------------- test cases */

static void test_checkerboard(void)
{
    const float w[2] = { 1.0f, 1.0f };
    mu_wfc     *wf = mu_wfc_create(32, 32, 4, 2, w, 0xABCDEFull);
    uint32_t   *scratch;
    uint32_t    d;

    check_i("checkerboard create", wf != NULL, 1);
    mu_wfc_neighbors_grid(wf, 0);
    for (d = 0; d < 4; ++d) mu_wfc_allow_symmetric(wf, d, 0, 1);

    scratch = (uint32_t *)malloc((size_t)wf->cells * 4u);
    check_i("checkerboard solve", check_determinism(wf, scratch, "checkerboard determinism"), 1);
    free(scratch);
    check_valid(wf, "checkerboard valid");

    /* The only 2-colouring of a non-wrapped grid is parity -- up to a global
     * flip, which the seed decides. Asserting one phase is a coin flip on the
     * RNG, so accept either rather than encoding this run's seed. */
    {
        uint32_t x, y;
        int      same = 1, flipped = 1;

        for (y = 0; y < wf->h; ++y)
            for (x = 0; x < wf->w; ++x)
            {
                const uint32_t t = mu_wfc_cell_tile(wf, x, y) & 1u;
                const uint32_t p = (x + y) & 1u;

                if (t != p) same = 0;
                if (t != (p ^ 1u)) flipped = 0;
            }
        check_i("checkerboard is parity (either phase)", same || flipped, 1);
    }
    mu_wfc_destroy(wf);
}

static void test_single_tile(void)
{
    const float w[1] = { 3.0f };
    mu_wfc     *wf = mu_wfc_create(5, 4, 4, 1, w, 7);

    check_i("single tile create", wf != NULL, 1);
    mu_wfc_neighbors_grid(wf, 0);
    mu_wfc_allow(wf, 0, 0, 0);      /* T == 1 still needs its self-adjacency */
    mu_wfc_allow(wf, 1, 0, 0);
    mu_wfc_allow(wf, 2, 0, 0);
    mu_wfc_allow(wf, 3, 0, 0);
    check_i("single tile already done", mu_wfc_done(wf), MU_WFC_DONE);
    check_i("single tile solve", mu_wfc_solve(wf, 0), MU_WFC_DONE);
    check_valid(wf, "single tile valid");
    check_i("single tile tile id", (long long)mu_wfc_cell_tile(wf, 3, 2), 0);
    mu_wfc_destroy(wf);
}

/* nothing may follow anything: the first collapse must contradict at once */
static void test_contradiction(void)
{
    const float w[2] = { 1.0f, 1.0f };
    mu_wfc     *wf = mu_wfc_create(4, 4, 4, 2, w, 99);

    check_i("contradiction create", wf != NULL, 1);
    mu_wfc_neighbors_grid(wf, 0);
    /* compat table left empty on purpose */
    check_i("contradiction detected", mu_wfc_solve(wf, 0), MU_WFC_CONTRADICTION);
    check_i("contradiction not done", mu_wfc_done(wf), MU_WFC_CONTRADICTION);
    mu_wfc_destroy(wf);
}

/* A ring of cells: E of the last cell wraps around to the first, so a rule that
 * forbids same-tile adjacency has to hold across the seam too.
 *
 * Two things this test pins down, both of which are easy to get wrong:
 *   - the ring needs an EVEN number of cells for a 2-colouring and a multiple
 *     of the cycle length for an n-colouring; a 16-ring with a 3-cycle has no
 *     solution, and "the solver failed" would be the correct behaviour, not a
 *     bug. 15 cells, 3 tiles.
 *   - a 1-row PERIODIC 4-direction grid makes each cell its own south and north
 *     neighbour (h == 1 wraps onto itself), which turns "must differ from its
 *     neighbour" into "must differ from itself": unsatisfiable. So the ring is
 *     built with dirs == 2 through the custom table, not through the grid
 *     helper. */
static void test_periodic_ring(void)
{
    const float w[3] = { 1.0f, 1.0f, 1.0f };
    const int   cells = 15;                 /* 15 % 3 == 0: the cycle closes */
    mu_wfc     *wf = mu_wfc_create((uint32_t)cells, 1, 2, 3, w, 4242);
    uint32_t   *nb = mu_wfc_neighbor_table(wf);
    int         i;
    int         cycle_ok = 1;

    check_i("ring create", wf != NULL, 1);
    for (i = 0; i < cells; ++i)
    {
        nb[(size_t)i * 2 + 0] = (uint32_t)((i + 1) % cells);          /* E */
        nb[(size_t)i * 2 + 1] = (uint32_t)((i + cells - 1) % cells);  /* W */
    }
    for (i = 0; i < 3; ++i) mu_wfc_allow_symmetric(wf, 0, (uint32_t)i, (uint32_t)((i + 1) % 3));

    check_i("ring solve", mu_wfc_solve(wf, 16), MU_WFC_DONE);
    check_valid(wf, "ring valid");

    for (i = 0; i < cells; ++i)
        if (mu_wfc_cell_tile(wf, (uint32_t)((i + 1) % cells), 0) !=
            (mu_wfc_cell_tile(wf, (uint32_t)i, 0) + 1u) % 3u) cycle_ok = 0;
    check_i("ring cycles through all tiles across the seam", cycle_ok, 1);
    mu_wfc_destroy(wf);
}

static void test_8dir(void)
{
    const float w[2] = { 1.0f, 1.0f };
    mu_wfc     *wf = mu_wfc_create(24, 24, 8, 2, w, 31337);
    uint32_t    d;

    check_i("8-dir create", wf != NULL, 1);
    mu_wfc_neighbors_grid(wf, 0);
    /* Orthogonal directions alternate; diagonals are unconstrained on purpose.
     * Alternating on a diagonal would demand that a cell differ from itself
     * (diagonals share parity), which is unsatisfiable -- a classic mistake
     * when moving a 4-direction rule set to 8. */
    for (d = 0; d < 8; d += 2) mu_wfc_allow_symmetric(wf, d, 0, 1);
    for (d = 1; d < 8; d += 2)
    {
        mu_wfc_allow_symmetric(wf, d, 0, 0); mu_wfc_allow_symmetric(wf, d, 0, 1);
        mu_wfc_allow_symmetric(wf, d, 1, 0); mu_wfc_allow_symmetric(wf, d, 1, 1);
    }
    check_i("8-dir solve", mu_wfc_solve(wf, 8), MU_WFC_DONE);
    check_valid(wf, "8-dir valid");
    mu_wfc_destroy(wf);
}

/* custom topology: a path where cell i's neighbour is i+1 in both directions
 * of a 2-direction model. Exercises the "caller fills the table" path. */
static void test_custom_topology(void)
{
    const float w[2] = { 1.0f, 1.0f };
    const int   cells = 64;
    mu_wfc     *wf = mu_wfc_create((uint32_t)cells, 1, 2, 2, w, 555);
    uint32_t   *nb = mu_wfc_neighbor_table(wf);
    int         i;

    check_i("custom create", wf != NULL, 1);
    for (i = 0; i < cells; ++i)
    {
        nb[(size_t)i * 2 + 0] = (uint32_t)((i + 1) % cells);   /* forward */
        nb[(size_t)i * 2 + 1] = (uint32_t)((i + cells - 1) % cells); /* back */
    }
    for (i = 0; i < 2; ++i) mu_wfc_allow_symmetric(wf, (uint32_t)i, 0, 1);

    /* NB: the grid layout is 64x1, so `neighbor` is indexed [cell][dir] with
     * dirs = 2 -- the table above matches that. */
    check_i("custom solve", mu_wfc_solve(wf, 8), MU_WFC_DONE);

    {
        int ok = 1;
        for (i = 0; i < cells; ++i)
            if (wf->result[i] == wf->result[(size_t)((i + 1) % cells)]) ok = 0;
        check_i("custom is a proper 2-colouring", ok, 1);
    }
    mu_wfc_destroy(wf);
}

/* more than 64 tiles: forces the multi-word propagation path */
static void test_multiword(void)
{
    const uint32_t T = 100u;
    float         *weights = (float *)malloc(T * 4u);
    mu_wfc        *wf;
    uint32_t       t;

    for (t = 0; t < T; ++t) weights[t] = 1.0f + (float)(t % 3u);
    wf = mu_wfc_create(20, 20, 4, T, weights, 0xF00Dull);
    check_i("multiword create", wf != NULL, 1);
    check_i("multiword words", wf->words, 2);
    mu_wfc_neighbors_grid(wf, 0);
    /* Symmetric relation: neighbouring tiles are equal or differ by one, in
     * either order. Written through allow_symmetric so the reverse direction
     * is by construction the same relation on the unordered pair -- writing
     * both directions by hand is how you get an unsatisfiable table. */
    for (t = 0; t < T; ++t)
    {
        uint32_t d;
        for (d = 0; d < 4; ++d)
        {
            mu_wfc_allow_symmetric(wf, d, t, t);
            mu_wfc_allow_symmetric(wf, d, t, (t + 1u) % T);
        }
    }
    check_i("multiword solve", mu_wfc_solve(wf, 16), MU_WFC_DONE);
    check_valid(wf, "multiword valid");
    free(weights);
    mu_wfc_destroy(wf);
}

/* overlapping model end to end: sample in, valid recombination out */
static void test_overlapping(void)
{
    enum { SW = 24, SH = 24, N = 3 };
    uint8_t  sample[SW * SH];
    uint8_t *patterns;
    float   *weights;
    uint32_t tiles, maxw = 0, i;
    mu_wfc  *wf;
    int      x, y;

    /* structured sample: 3 symbols in 2x2 blocks with a diagonal stripe, so
     * the pattern set is small, satisfiable, and not trivially uniform */
    for (y = 0; y < SH; ++y)
        for (x = 0; x < SW; ++x)
        {
            const int b = ((x / 2) + (y / 2)) % 3;

            sample[y * SW + x] = (uint8_t)(((x + y) % 7 == 0) ? 2 : b);
        }

    tiles = mu_wfc_pattern_count(sample, SW, SH, N, &maxw);
    check_i("overlapping: found patterns", tiles > 1, 1);
    check_i("overlapping: count <= bound", tiles <= maxw, 1);

    patterns = (uint8_t *)malloc((size_t)tiles * N * N);
    weights  = (float *)calloc(tiles, 4u);
    check_i("overlapping: extract", mu_wfc_extract_patterns(sample, SW, SH, N, patterns, weights, tiles), tiles);

    /* weights must sum to the number of windows, which is what makes them
     * occurrence counts and not arbitrary numbers */
    {
        float    sum = 0.0f;
        uint32_t windows = (uint32_t)((SW - N + 1) * (SH - N + 1));

        for (i = 0; i < tiles; ++i) sum += weights[i];
        check_i("overlapping: weights sum to window count", (long long)lroundf(sum), windows);
    }

    wf = mu_wfc_create(32, 32, 4, tiles, weights, 0x1234ull);
    check_i("overlapping: create", wf != NULL, 1);
    mu_wfc_neighbors_grid(wf, 0);
    check_i("overlapping: constrain", mu_wfc_constrain_from_patterns(wf, patterns, N), MU_WFC_DONE);
    check_i("overlapping: solve", mu_wfc_solve(wf, 16), MU_WFC_DONE);
    check_valid(wf, "overlapping valid");
    check_windows(wf, patterns, tiles, N, "overlapping windows are from the sample");

    printf("  note overlapping: %u patterns from a %dx%d sample with %ux%u windows -> %ux%u grid\n",
           tiles, SW, SH, N, N, wf->w, wf->h);
    mu_wfc_destroy(wf);
    free(patterns);
    free(weights);
}

/* allow_symmetric must produce a table where the reverse arc agrees */
static void test_symmetry_helper(void)
{
    const float w[2] = { 1.0f, 1.0f };
    mu_wfc     *wf = mu_wfc_create(2, 2, 4, 2, w, 1);
    uint32_t    d;

    for (d = 0; d < 4; ++d) mu_wfc_allow_symmetric(wf, d, 0, 1);
    for (d = 0; d < 4; ++d)
    {
        const uint32_t rev = (d + 2u) % 4u;

        check_i("allow_symmetric forward", mu_wfc_is_allowed(wf, d, 0, 1), 1);
        check_i("allow_symmetric reverse", mu_wfc_is_allowed(wf, rev, 1, 0), 1);
        check_i("allow_symmetric no spurious", mu_wfc_is_allowed(wf, d, 1, 0), 1);
        check_i("allow_symmetric self not added", mu_wfc_is_allowed(wf, d, 0, 0), 0);
    }
    mu_wfc_destroy(wf);
}

static void test_arg_validation(void)
{
    const float good[2] = { 1.0f, 1.0f };
    const float zero[2] = { 1.0f, 0.0f };
    const float neg[2]  = { 1.0f, -2.0f };

    check_i("reject 0 tiles", mu_wfc_create(4, 4, 4, 0, good, 1) == NULL, 1);
    check_i("reject 0 cells", mu_wfc_create(0, 4, 4, 2, good, 1) == NULL, 1);
    check_i("reject >1024 tiles", mu_wfc_create(4, 4, 4, MU_WFC_MAX_TILES + 1u, good, 1) == NULL, 1);
    check_i("reject zero weight", mu_wfc_create(4, 4, 4, 2, zero, 1) == NULL, 1);
    check_i("reject negative weight", mu_wfc_create(4, 4, 4, 2, neg, 1) == NULL, 1);
    check_i("reject null weights", mu_wfc_create(4, 4, 4, 2, NULL, 1) == NULL, 1);
    {
        mu_wfc *bad = mu_wfc_create(4, 4, 3, 2, good, 1);

        check_i("reject bad dirs for grid", mu_wfc_neighbors_grid(bad, 0), MU_WFC_ERROR);
        mu_wfc_destroy(bad);
    }
    check_i("destroy(NULL) is safe", (mu_wfc_destroy(NULL), 1), 1);
}

int main(void)
{
    printf("mu_wfc correctness gate\n");
    printf("----------------------------------------------------------\n");

    test_arg_validation();
    test_symmetry_helper();
    test_single_tile();
    test_checkerboard();
    test_contradiction();
    test_periodic_ring();
    test_8dir();
    test_custom_topology();
    test_multiword();
    test_overlapping();

    printf("----------------------------------------------------------\n");
    if (g_fails) { printf("FAILED: %d checks, %d failures\n", g_checks, g_fails); return 1; }
    printf("PASSED: %d checks, 0 failures\n", g_checks);
    return 0;
}
