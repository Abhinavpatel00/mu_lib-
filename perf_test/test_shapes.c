/* test_shapes.c — correctness gate for mu/mu_shapes.h
 *
 * The point of this file is that "it looked fine in Blender" is not evidence.
 * Every generator is checked for the properties that actually matter and can be
 * checked without a human:
 *
 *   1. mu_shape_validate(): indexes in range, no NaN, no zero-area triangles.
 *   2. Topology from mu_shape_stats(): closed shapes have zero boundary edges,
 *      the right Euler characteristic and the right genus. This is computed after
 *      merging coincident positions, so a UV seam is not mistaken for a hole.
 *   3. Signed volume and surface area against ANALYTIC values, which is what
 *      catches an inside-out or half-inverted mesh: a mesh with one flipped cap
 *      is still closed and still has Euler characteristic 2, but its volume is
 *      wrong. Six generators were shipped inverted before this gate existed.
 *   4. Every operation (weld, unweld, merge, flip, subdivide, extrude, las) is
 *      checked for invariants rather than for output.
 *
 * Build: cc -O2 -std=c11 -I.. -I../mu test_shapes.c -lm
 */

#include "mu_shapes.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_checks = 0;
static int g_fail = 0;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond)
    {
        printf("  FAIL %s\n", what);
        ++g_fail;
    }
}

static void check_near(const char *what, double got, double want, double rel)
{
    double tol = fabs(want) * rel + 1e-9;
    ++g_checks;
    if (fabs(got - want) > tol)
    {
        printf("  FAIL %-46s got %.6f want %.6f (+-%.1f%%)\n", what, got, want,
               rel * 100.0);
        ++g_fail;
    }
}

/* Every generator goes through this: valid, and the shape is not empty. */
static mu_shape_stats examine(const char *name, mu_shape s)
{
    mu_shape_stats st;
    char err[128];

    if (!mu_shape_validate(&s, err, sizeof err))
    {
        printf("  FAIL %-20s validate: %s\n", name, err);
        ++g_fail;
    }
    ++g_checks;

    if (s.npoints == 0 || s.ntriangles == 0)
    {
        printf("  FAIL %-20s empty\n", name);
        ++g_fail;
        memset(&st, 0, sizeof st);
        return st;
    }
    if (!mu_shape_stats_compute(&s, 0.0f, &st))
    {
        printf("  FAIL %-20s stats failed\n", name);
        ++g_fail;
    }
    ++g_checks;
    return st;
}

/* A closed, orientable solid of genus g. */
static void expect_solid(const char *name, mu_shape s, int genus)
{
    mu_shape_stats st = examine(name, s);

    check(st.boundary_edges == 0, name);
    check(st.nonmanifold_edges == 0, name);
    check(st.inconsistent_edges == 0, name);
    check(st.watertight == 1, name);
    if (st.boundary_edges || st.nonmanifold_edges || st.inconsistent_edges)
        printf("    (%s: bnd %u nm %u inc %u)\n", name, st.boundary_edges,
               st.nonmanifold_edges, st.inconsistent_edges);
    check(st.euler == 2 - 2 * genus, name);
    check(st.genus == genus, name);
    if (st.euler != 2 - 2 * genus || st.genus != genus)
        printf("    (%s: euler %d want %d, genus %d want %d)\n", name, st.euler,
               2 - 2 * genus, st.genus, genus);
    check(st.volume > 0.0f, name);            /* outward winding, not analytic */
    if (st.volume <= 0.0f)
        printf("    (%s: signed volume %.4f <= 0, mesh is inside out)\n", name,
               (double)st.volume);
    mu_shape_free(&s);
}

/* Normals must be unit length and point away from the centroid: the cheapest
 * possible "did I get the winding right everywhere" test that does not need the
 * volume. */
static void check_normals(mu_shape *s, const char *what)
{
    uint32_t i;
    int bad_unit = 0, bad_dir = 0;
    mu_vec3 c = {0};

    for (i = 0; i < s->npoints; ++i) mu_vec3_add(c, c, s->verts[i].p);
    mu_vec3_scale(c, c, 1.0f / (float)s->npoints);

    for (i = 0; i < s->npoints; ++i)
    {
        float len = mu_vec3_length(s->verts[i].n);
        mu_vec3 d; mu_vec3_sub(d, s->verts[i].p, c);
        if (fabsf(len - 1.0f) > 1e-3f && len > 0.0f) ++bad_unit;
        if (mu_vec3_length2(d) > 0.04f && mu_vec3_dot(d, s->verts[i].n) < 0.0f)
            ++bad_dir;
    }
    ++g_checks;
    if (bad_unit || bad_dir)
    {
        printf("  FAIL normals[%s]: %d not unit, %d point inward\n", what, bad_unit,
               bad_dir);
        ++g_fail;
    }
}

/* UVs are expected in [0,1]; a generator that forgets to write them shows up
 * here as zeroes rather than as a mystery in a shader. */
static void check_uvs(const mu_shape *s, const char *what)
{
    uint32_t i;
    int bad = 0;

    for (i = 0; i < s->npoints; ++i)
    {
        float u = s->verts[i].uv[0], v = s->verts[i].uv[1];
        if (!(u >= -1e-4f && u <= 1.0001f && v >= -1e-4f && v <= 1.0001f)) ++bad;
    }
    ++g_checks;
    if (bad)
    {
        printf("  FAIL uvs[%s]: %d outside [0,1]\n", what, bad);
        ++g_fail;
    }
}

/* ---------- SDFs used by the marching-tetrahedra tests ---------- */

typedef struct { mu_vec3 c; float r; } blob_t;

static float sdf_two_blobs(mu_vec3 p, void *user)
{
    const blob_t *b = (const blob_t *)user;
    mu_sdf_sphere_t a, c;

    mu_vec3_make(a.c, -0.55f, 0.0f, 0.0f); a.r = b->r;
    mu_vec3_make(c.c, 0.55f, 0.0f, 0.0f);  c.r = b->r;
    return mu_sdf_smin(mu_sdf_sphere(p, &a), mu_sdf_sphere(p, &c), b->c[0]);
}

static float sdf_gyroid(mu_vec3 p, void *user)
{
    float s = user ? *(const float *)user : 2.0f;
    return sinf(p[0] * s) * cosf(p[1] * s) + sinf(p[1] * s) * cosf(p[2] * s) +
           sinf(p[2] * s) * cosf(p[0] * s);
}

int main(void)
{
    printf("mu_shapes correctness gate\n");
    /* ---- L-systems ---- */

    {
        static const char *const bush[]  = { "X=F[+X][-X]FX", "F=FF" };
        static const char *const stoch[] = { "A=F[+A][-A]A", "A=F[-A]A", "A=F[+A]" };
        static const char *const rev[]   = { "A=F[&A]/[\\A]!FA|FA", "A=F^[+A][-A]" };
        mu_lsystem_config c;
        mu_shape s3, s4, s5;
        mu_shape_stats st;

        memset(&c, 0, sizeof c);
        c.axiom = "X";
        c.slices = 8;
        c.radius = 0.05f;
        c.length = 0.2f;
        c.angle = 25.7f;

        c.iterations = 3; s3 = mu_shape_lsystem(&c, bush, 2);
        c.iterations = 4; s4 = mu_shape_lsystem(&c, bush, 2);
        c.iterations = 5; s5 = mu_shape_lsystem(&c, bush, 2);

        check(s3.ntriangles > 0 && s4.ntriangles > s3.ntriangles &&
              s5.ntriangles > s4.ntriangles,
              "lsystem: the same grammar grows with the iteration count");
        check(s3.npoints % 8u == 0, "lsystem: every ring has exactly `slices` points");

        /* a bracketed tube grammar has to come out manifold per run: junctions
         * are welded coincidence, but nothing may be non-manifold on its own */
        mu_shape_stats_compute(&s5, 0.0f, &st);
        check(st.nonmanifold_edges == 0, "lsystem: no non-manifold edges");
        check(st.inconsistent_edges == 0, "lsystem: consistent winding");
        check(st.boundary_edges > 0, "lsystem: a plant has open ends (not closed)");
        {
            char err[128];
            check(mu_shape_validate(&s5, err, sizeof err) == 1, "lsystem: validate passes");
        }

        /* it may not wander further than its own segment budget */
        {
            uint32_t i;
            float maxr = 0.0f;
            for (i = 0; i < s5.npoints; ++i)
            {
                float r = mu_vec3_length(s5.verts[i].p);
                if (r > maxr) maxr = r;
            }
            /* "F=FF" doubles every F per iteration and one X per iteration also
             * continues the chain, so the longest walk is bounded by
             * iterations * 2^iterations segments. Assert the budget, not a
             * guess: a turtle that fails to pop would blow straight through it. */
            check(maxr <= c.length * (float)(1u << 5) * 5.0f + c.radius,
                  "lsystem: stays inside the reach of its own segment budget");
            check(maxr > c.length, "lsystem: actually extends past one segment");
        }

        /* deterministic by default, stochastic only with a seed */
        {
            mu_lsystem_config d = c;
            mu_shape a1, a2, b1, b2;

            d.iterations = 4; d.seed = 0;
            a1 = mu_shape_lsystem(&d, bush, 2);
            a2 = mu_shape_lsystem(&d, bush, 2);
            check(a1.npoints == a2.npoints && a1.ntriangles == a2.ntriangles,
                  "lsystem: seed 0 is deterministic");

            d.seed = 7; d.axiom = "A";
            b1 = mu_shape_lsystem(&d, stoch, 3);
            b2 = mu_shape_lsystem(&d, stoch, 3);
            check(b1.npoints == b2.npoints && b1.ntriangles == b2.ntriangles,
                  "lsystem: the same seed reproduces the same tree");

            d.seed = 99;
            {
                mu_shape b3 = mu_shape_lsystem(&d, stoch, 3);
                /* counts can coincide between two trees, positions cannot:
                 * compare the geometry, not the tally */
                uint32_t i, n = b1.npoints < b3.npoints ? b1.npoints : b3.npoints;
                int different = b1.npoints != b3.npoints || b1.ntriangles != b3.ntriangles;

                for (i = 0; i < n && !different; ++i)
                {
                    mu_vec3 sub;
                    mu_vec3_sub(sub, b1.verts[i].p, b3.verts[i].p);
                    if (mu_vec3_length2(sub) > 1e-12f) different = 1;
                }
                check(different, "lsystem: a different seed grows a different tree");
                mu_shape_free(&b3);
            }
            mu_shape_free(&a1); mu_shape_free(&a2);
            mu_shape_free(&b1); mu_shape_free(&b2);
        }

        /* pitch + roll + retreat + radius shrink: the symbols that touch the
         * frame. This grammar used to emit zero-area quads, because the up
         * vector drifts toward the head and a guarded normalize collapses the
         * ring to radius 0. It must stay a valid mesh now. */
        {
            mu_lsystem_config d = c;
            char err[128];
            mu_shape r;
            d.axiom = "A"; d.iterations = 4;
            r = mu_shape_lsystem(&d, rev, 2);
            check(r.ntriangles > 0, "lsystem: pitch/roll/retreat grammar produces geometry");
            check(mu_shape_validate(&r, err, sizeof err) == 1,
                  "lsystem: pitch/roll/retreat grammar has no degenerate triangles");
            mu_shape_free(&r);
        }

        /* the rewrite cap is a cap, not a suggestion */
        {
            mu_lsystem_config d = c;
            mu_shape big;
            d.iterations = 40;
            d.max_symbols = 4096;
            big = mu_shape_lsystem(&d, bush, 2);
            check(big.ntriangles > 0 && big.ntriangles < 100000u,
                  "lsystem: 40 iterations stay bounded by max_symbols");
            mu_shape_free(&big);
        }

        /* no program is an empty shape, not a crash */
        check(mu_shape_lsystem(NULL, bush, 2).npoints == 0, "lsystem: null config is empty");
        check(mu_shape_lsystem(&c, NULL, 0).npoints == 0, "lsystem: null rules are empty");
        c.axiom = NULL;
        check(mu_shape_lsystem(&c, bush, 2).npoints == 0, "lsystem: null axiom is empty");

        mu_shape_free(&s3); mu_shape_free(&s4); mu_shape_free(&s5);
    }

    /* ---- internal consistency: two edge classifications must agree ---- */

    {
        /* mu_shape_stats_compute uses a count-only edge table (mu_shape__edge_counts)
         * because the full one costs 24 bytes a slot, while Loop subdivision needs
         * the full one for its opposite-corner data. Two implementations of the
         * same classification is exactly how a library starts lying, so compare
         * them on every kind of mesh the classifier distinguishes: closed, open,
         * non-manifold, and inconsistent. */
        mu_shape cases[4];
        const char *names[4] = { "parity_cube", "parity_open", "parity_nonmanifold",
                                 "parity_flipped" };
        int c;

        cases[0] = mu_shape_cube(1, 1.0f);
        cases[1] = mu_shape_plane(4, 4, 2.0f, 2.0f);   /* boundary everywhere */
        cases[2] = mu_shape_empty();              /* three triangles on one edge */
        mu_shape_add_vertex(&cases[2], (mu_vec3){0.0f, 0.0f, 0.0f}, 0.0f, 0.0f);
        mu_shape_add_vertex(&cases[2], (mu_vec3){1.0f, 0.0f, 0.0f}, 1.0f, 0.0f);
        mu_shape_add_vertex(&cases[2], (mu_vec3){0.0f, 1.0f, 0.0f}, 0.0f, 1.0f);
        mu_shape_add_vertex(&cases[2], (mu_vec3){0.0f, 0.0f, 1.0f}, 0.0f, 0.0f);
        mu_shape_add_triangle(&cases[2], 0, 1, 2);
        mu_shape_add_triangle(&cases[2], 0, 1, 3);
        mu_shape_add_triangle(&cases[2], 1, 0, 3);
        cases[3] = mu_shape_cube(1, 1.0f);        /* one face wound backwards */
        {
            MU_SHAPES_T t = cases[3].triangles[1];
            cases[3].triangles[1] = cases[3].triangles[2];
            cases[3].triangles[2] = t;
        }

        for (c = 0; c < 4; ++c)
        {
            mu_shape__edges e;
            mu_shape_stats st;
            uint32_t *map, b = 0, nm = 0, inc = 0, uniq = 0;
            char what[128];

            /* stats reports on the WELDED topology (a UV seam is a duplicated
             * vertex, not a hole), so the comparison has to use the same map -
             * an unwelded cube is 24 points and 24 boundary edges, a welded one
             * is 8 points and none. */
            map = (uint32_t *)malloc((size_t)cases[c].npoints * sizeof *map);
            mu_shape__weld_map(&cases[c], 1e-5f, MU_SHAPE_WELD_NONE, map);
            mu_shape_stats_compute(&cases[c], 0.0f, &st);
            mu_shape__edges_build(&cases[c], map, &e);
            mu_shape__edge_counts(&cases[c], map, &b, &nm, &inc, &uniq);

            snprintf(what, sizeof what, "%s: count-only edges match the full table",
                     names[c]);
            check(b == e.boundary && nm == e.nonmanifold && inc == e.inconsistent &&
                  uniq == e.count, what);
            snprintf(what, sizeof what, "%s: stats reports the same edges", names[c]);
            check(st.boundary_edges == b && st.nonmanifold_edges == nm &&
                  st.inconsistent_edges == inc, what);

            snprintf(what, sizeof what, "%s: classifications are the expected ones",
                     names[c]);
            if (c == 0) check(b == 0 && nm == 0 && inc == 0, what);
            if (c == 1) check(b == 16 && nm == 0 && inc == 0, what);
            if (c == 2) check(nm == 1 && inc == 0, what);
            if (c == 3) check(b == 0 && nm == 0 && inc == 3, what);

            free(map);
            mu_shape__edges_free(&e);
            mu_shape_free(&cases[c]);
        }
    }

    printf("--------------------------------\n");

    /* ---- primitives: analytic area and volume ---- */

    {
        mu_shape s = mu_shape_cube(3, 2.0f);
        mu_shape_stats st = examine("cube", mu_shape_clone(&s));
        check_near("cube area", st.area, 24.0, 1e-5);
        check_near("cube volume", st.volume, 8.0, 1e-5);
        mu_shape_free(&s);
        expect_solid("cube", mu_shape_cube(3, 2.0f), 0);
    }

    {
        /* icosphere converges to the sphere from below: at level 4 the area is
         * within 0.2% and the volume within 0.3%. */
        mu_shape s = mu_shape_icosphere(4, 1.0f);
        mu_shape_stats st = examine("icosphere4", mu_shape_clone(&s));
        check_near("icosphere area", st.area, 4.0 * MU_SHAPE_PI, 0.004);
        check_near("icosphere volume", st.volume, 4.0 / 3.0 * MU_SHAPE_PI, 0.004);
        check_normals(&s, "icosphere");
        check_uvs(&s, "icosphere");
        mu_shape_free(&s);
        expect_solid("icosphere", mu_shape_icosphere(3, 1.0f), 0);
    }

    {
        /* a UV sphere loses ~3% area at 16x8 (slivers at the poles), ~0.4% at
         * 64x32: measure that it converges instead of asserting one number */
        mu_shape coarse = mu_shape_sphere(16, 8, 1.0f);
        mu_shape fine = mu_shape_sphere(64, 32, 1.0f);
        mu_shape_stats sc = examine("sphere16", mu_shape_clone(&coarse));
        mu_shape_stats sf = examine("sphere64", mu_shape_clone(&fine));
        check_near("sphere64 area", sf.area, 4.0 * MU_SHAPE_PI, 0.01);
        check_near("sphere64 volume", sf.volume, 4.0 / 3.0 * MU_SHAPE_PI, 0.01);
        check(fabsf(sf.area - 4.0f * MU_SHAPE_PI) < fabsf(sc.area - 4.0f * MU_SHAPE_PI),
              "sphere: refinement reduces the area error");
        check(sc.duplicate_points > 0, "sphere: UV seam shows up as duplicates");
        check_normals(&fine, "sphere64");
        check_uvs(&fine, "sphere64");
        mu_shape_free(&coarse);
        mu_shape_free(&fine);
        expect_solid("sphere", mu_shape_sphere(24, 12, 1.0f), 0);
    }

    {
        mu_shape s = mu_shape_cylinder(64, 2, 1.0f, 2.0f);
        mu_shape_stats st = examine("cylinder", mu_shape_clone(&s));
        /* 64 sides: the polygon area is (n/2)sin(2pi/n) of the circle */
        check_near("cylinder volume", st.volume, MU_SHAPE_PI * 2.0, 0.01);
        /* two caps (2*pi*r^2) plus the side (2*pi*r*h) with r = 1, h = 2 */
        check_near("cylinder area", st.area, 6.0 * MU_SHAPE_PI, 0.01);
        mu_shape_free(&s);
        expect_solid("cylinder", mu_shape_cylinder(24, 1, 1.0f, 2.0f), 0);
    }

    {
        mu_shape s = mu_shape_cone(64, 1, 1.0f, 2.0f);
        mu_shape_stats st = examine("cone", mu_shape_clone(&s));
        check_near("cone volume", st.volume, MU_SHAPE_PI * 2.0 / 3.0, 0.01);
        /* slant height sqrt(1 + 4) */
        check_near("cone area", st.area,
                   MU_SHAPE_PI + MU_SHAPE_PI * sqrtf(5.0f), 0.01);
        mu_shape_free(&s);
        expect_solid("cone", mu_shape_cone(24, 1, 1.0f, 2.0f), 0);
    }

    {
        mu_shape s = mu_shape_capsule(64, 8, 1.0f, 2.0f);
        mu_shape_stats st = examine("capsule", mu_shape_clone(&s));
        check_near("capsule volume", st.volume,
                   MU_SHAPE_PI * 2.0 + 4.0 / 3.0 * MU_SHAPE_PI, 0.02);
        mu_shape_free(&s);
        expect_solid("capsule", mu_shape_capsule(24, 4, 1.0f, 2.0f), 0);
    }

    {
        mu_shape s = mu_shape_torus(64, 32, 1.0f, 0.3f);
        mu_shape_stats st = examine("torus", mu_shape_clone(&s));
        check_near("torus volume", st.volume, 2.0 * MU_SHAPE_PI * MU_SHAPE_PI *
                   0.09, 0.02);
        check_near("torus area", st.area, 4.0 * MU_SHAPE_PI * MU_SHAPE_PI * 0.3, 0.02);
        mu_shape_free(&s);
        expect_solid("torus", mu_shape_torus(32, 16, 1.0f, 0.3f), 1);
    }

    /* ---- platonic solids: exact volumes at circumradius 1 ---- */

    expect_solid("tetrahedron", mu_shape_tetrahedron(1.0f), 0);
    expect_solid("octahedron", mu_shape_octahedron(1.0f), 0);
    expect_solid("icosahedron", mu_shape_icosahedron(1.0f), 0);
    expect_solid("dodecahedron", mu_shape_dodecahedron(1.0f), 0);
    {
        mu_shape_stats st;

        st = examine("tetra", mu_shape_tetrahedron(1.0f));
        check_near("tetrahedron volume", st.volume, 8.0 / (9.0 * sqrt(3.0)), 1e-4);
        st = examine("octa", mu_shape_octahedron(1.0f));
        check_near("octahedron volume", st.volume, 4.0 / 3.0, 1e-4);
        st = examine("icosa", mu_shape_icosahedron(1.0f));
        check_near("icosahedron volume", st.volume, 2.5361505, 1e-4);
        st = examine("dodeca", mu_shape_dodecahedron(1.0f));
        check_near("dodecahedron volume", st.volume, 2.7851635, 1e-4);
        check(st.npoints == 20, "dodecahedron: 20 vertices (one per icosa face)");
        check(st.ntriangles == 36, "dodecahedron: 12 pentagons -> 36 triangles");
    }

    /* ---- rounded box: volume between the cube and the sphere-inscribed one ---- */

    {
        mu_shape a = mu_shape_cube(6, 2.0f);
        mu_shape b = mu_shape_rounded_box(6, 2.0f, 0.5f);
        mu_shape_stats sa = examine("cube6", mu_shape_clone(&a));
        mu_shape_stats sb = examine("rbox", mu_shape_clone(&b));

        check(sb.volume > 0.0f && sb.volume < sa.volume,
              "rounded box: volume between 0 and the cube");
        /* the round removes one octant-corner lune per corner: V = a^3 - 8 r^3
         * + 8*(pi/6) r^3 for the exact shape, minus the edges. Only bound it. */
        check(sb.volume > 0.6f * sa.volume, "rounded box: not over-rounded");
        check(sb.boundary_edges == 0, "rounded box stays closed");
        mu_shape_free(&a);
        mu_shape_free(&b);
        expect_solid("rounded_box", mu_shape_rounded_box(4, 2.0f, 0.3f), 0);
    }

    /* ---- advanced surfaces ---- */

    {
        mu_shape_stats st = examine("supershape", mu_shape_supershape(NULL, 24, 16, 1.0f));
        check(st.watertight, "supershape: closed");
        check_near("supershape volume", st.volume, 4.0 / 3.0 * MU_SHAPE_PI, 0.05);

        {
            mu_shape_superformula star = mu_shape_superformula_star();
            mu_shape ss = mu_shape_supershape(&star, 24, 16, 1.0f);
            mu_shape_stats sst = examine("supershape_star", mu_shape_clone(&ss));
            check(sst.watertight, "supershape(star): closed");
            check(sst.volume < st.volume, "supershape(star): more pinched than a sphere");
            mu_shape_free(&ss);
        }
    }

    {
        mu_shape m = mu_shape_mobius(48, 2, 1.0f, 0.6f);
        mu_shape_stats st = examine("mobius", mu_shape_clone(&m));
        /* a Mobius band: one boundary loop, chi = 0, not orientable */
        check(st.boundary_edges > 0, "mobius: has a boundary");
        check(st.euler == 0, "mobius: Euler characteristic 0");
        check(st.inconsistent_edges > 0,
              "mobius: a non-orientable surface must show inconsistent edges");
        check(st.genus == -1, "mobius: no genus is reported for an open surface");
        mu_shape_free(&m);
    }

    {
        mu_shape k = mu_shape_klein_bottle(24, 24, 1.0f);
        mu_shape_stats st = examine("klein", mu_shape_clone(&k));
        check(st.boundary_edges == 0, "klein: no boundary (it is a closed surface)");
        check(st.euler == 0, "klein: Euler characteristic 0");
        check(!st.watertight,
              "klein: non-manifold where the immersion self-intersects");
        mu_shape_free(&k);
    }

    {
        mu_shape sh = mu_shape_seashell(16, 40, 3, 1.0f, 0.15f, 0.4f);
        mu_shape_stats st = examine("seashell", mu_shape_clone(&sh));
        check(st.boundary_edges > 0, "seashell: open surface");
        check(st.euler == 0, "seashell: an open tube has Euler characteristic 0");
        mu_shape_free(&sh);
        expect_solid("spring", mu_shape_spring(120, 8, 1.0f, 0.4f, 2.0f, 0.1f), 0);
        expect_solid("trefoil", mu_shape_trefoil_knot(120, 8, 1.0f, 0.2f), 1);
    }

    /* ---- construction kit ---- */

    {
        /* A lathe profile that is exactly a 32-gon cylinder: same volume. */
        mu_vec2 prof[4];
        mu_shape a, b;
        mu_shape_stats sa, sb;

        memcpy(prof[0], (mu_vec2){0.0f, -1.0f}, sizeof(prof[0]));
        memcpy(prof[1], (mu_vec2){1.0f, -1.0f}, sizeof(prof[1]));
        memcpy(prof[2], (mu_vec2){1.0f, 1.0f}, sizeof(prof[2]));
        memcpy(prof[3], (mu_vec2){0.0f, 1.0f}, sizeof(prof[3]));

        a = mu_shape_lathe(prof, 4, 32, 0);
        b = mu_shape_cylinder(32, 1, 1.0f, 2.0f);
        sa = examine("lathe", mu_shape_clone(&a));
        sb = examine("cylinder32", mu_shape_clone(&b));
        check_near("lathe(cylinder profile) volume == cylinder volume",
                   sa.volume, sb.volume, 1e-4);
        check_near("lathe(cylinder profile) area == cylinder area", sa.area, sb.area,
                   1e-4);
        mu_shape_free(&a);
        mu_shape_free(&b);
        expect_solid("lathe", mu_shape_lathe(prof, 4, 16, 0), 0);
    }

    {
        /* Closed profile (a square in the r/y half-plane) = a torus: genus 1. */
        mu_vec2 prof[4];
        memcpy(prof[0], (mu_vec2){1.0f, -0.3f}, sizeof(prof[0]));
        memcpy(prof[1], (mu_vec2){1.3f, -0.3f}, sizeof(prof[1]));
        memcpy(prof[2], (mu_vec2){1.3f, 0.3f}, sizeof(prof[2]));
        memcpy(prof[3], (mu_vec2){1.0f, 0.3f}, sizeof(prof[3]));
        expect_solid("lathe_closed", mu_shape_lathe(prof, 4, 24, 1), 1);
    }

    {
        mu_vec3 path[16];
        int i;

        for (i = 0; i < 16; ++i)
        {
            float a = MU_SHAPE_TAU * (float)i / 16.0f;
            memcpy(path[i], (mu_vec3){cosf(a), 0.0f, sinf(a)}, sizeof(path[i]));
        }
        /* a closed path -> a torus (genus 1); an open capped one -> a solid */
        expect_solid("tube_closed", mu_shape_tube(path, 16, 0.25f, 12, 1, 0), 1);
        expect_solid("tube_capped", mu_shape_tube(path, 16, 0.25f, 12, 0, 1), 0);

        {
            /* variable radius: a cone-ish tube, volume between the two radii */
            float radii[16];
            mu_shape a, b, c;
            mu_shape_stats sa, sb, sc;
            for (i = 0; i < 16; ++i) radii[i] = 0.1f + 0.3f * (float)i / 15.0f;
            a = mu_shape_tube(path, 16, 0.1f, 12, 1, 0);
            b = mu_shape_tube(path, 16, 0.4f, 12, 1, 0);
            c = mu_shape_tube_radii(path, 16, 0.0f, radii, 12, 1, 0);
            sa = examine("tube_small", mu_shape_clone(&a));
            sb = examine("tube_big", mu_shape_clone(&b));
            sc = examine("tube_taper", mu_shape_clone(&c));
            check(sc.volume > sa.volume && sc.volume < sb.volume,
                  "variable radius tube volume lies between the two extremes");
            mu_shape_free(&a);
            mu_shape_free(&b);
            mu_shape_free(&c);
        }
    }

    {
        /* Extrusion: volume == polygon area * height, for a CONCAVE polygon too
         * (a fan cap would produce overlapping triangles and fail this). */
        mu_vec2 star[10];
        int i;
        double want;

        for (i = 0; i < 10; ++i)
        {
            float a = MU_SHAPE_TAU * (float)i / 10.0f;
            float r = (i & 1) ? 0.4f : 1.0f;
            memcpy(star[i], (mu_vec2){r * cosf(a), r * sinf(a)}, sizeof(star[i]));
        }
        want = 10.0 * 0.5 * 1.0 * 0.4 * sin(MU_SHAPE_TAU / 10.0) * 1.5;

        {
            mu_shape s = mu_shape_extrude(star, 10, 1.5f, 0.0f);
            mu_shape_stats st = examine("extrude_star", mu_shape_clone(&s));
            check_near("star prism volume == area * height", st.volume, want, 1e-3);
            check(st.watertight, "star prism: ear clipping closed the cap");
            mu_shape_free(&s);
        }
        expect_solid("extrude_star", mu_shape_extrude(star, 10, 1.5f, 0.0f), 0);

        {
            /* a square, straight and twisted: same volume, same topology */
            mu_vec2 sq[4];
            mu_shape a, b;
            mu_shape_stats sa, sb;
            memcpy(sq[0], (mu_vec2){-1.0f, -1.0f}, sizeof(sq[0]));
            memcpy(sq[1], (mu_vec2){1.0f, -1.0f}, sizeof(sq[1]));
            memcpy(sq[2], (mu_vec2){1.0f, 1.0f}, sizeof(sq[2]));
            memcpy(sq[3], (mu_vec2){-1.0f, 1.0f}, sizeof(sq[3]));

            a = mu_shape_extrude(sq, 4, 2.0f, 0.0f);
            b = mu_shape_extrude(sq, 4, 2.0f, 0.2f);
            sa = examine("extrude_sq", mu_shape_clone(&a));
            sb = examine("extrude_twist", mu_shape_clone(&b));
            check_near("square prism volume", sa.volume, 8.0, 1e-5);
            /* The sweep is ruled between rings: each corner travels a chord, not
             * an arc, so the enclosed volume is a couple of percent low and the
             * deficit shrinks as 1/rings (measured 7.72 at 2 rings, 7.82 at 3,
             * 7.97 at 16). A round profile barely notices: a 64-gon at the same
             * twist is 6.2616 vs 6.2731, i.e. 0.18%. Asserted as a band, and the
             * exact-algebraic statement (area * height) is only claimed at
             * twist == 0. */
            check(sb.volume > 0.94f * 8.0f && sb.volume <= 8.0f,
                  "twisted prism: volume is a few percent below area*height");
            check(sb.ntriangles > sa.ntriangles,
                  "twisted prism: subdivides into rings, not two warped quads");
            check(sb.watertight, "twisted prism: still closed");
            mu_shape_free(&a);
            mu_shape_free(&b);
        }
    }

    /* ---- SDF -> mesh ---- */

    {
        mu_sdf_sphere_t sp;
        mu_shape s;
        mu_shape_stats st;

        memcpy(sp.c, (mu_vec3){0.0f, 0.0f, 0.0f}, sizeof(sp.c));
        sp.r = 1.0f;
        s = mu_shape_from_sdf(mu_sdf_sphere, &sp, (mu_vec3){-1.4f, -1.4f, -1.4f},
                              (mu_vec3){1.4f, 1.4f, 1.4f}, 32, 0.0f);
        st = examine("sdf_sphere", mu_shape_clone(&s));
        check(st.watertight, "sdf sphere: watertight out of the box");
        check(st.euler == 2 && st.genus == 0, "sdf sphere: genus 0");
        check_near("sdf sphere area", st.area, 4.0 * MU_SHAPE_PI, 0.01);
        check_near("sdf sphere volume", st.volume, 4.0 / 3.0 * MU_SHAPE_PI, 0.01);
        check_normals(&s, "sdf sphere");
        check(st.volume > 0.0f, "sdf sphere: marching tets fixes its own winding");
        mu_shape_free(&s);

        {
            mu_sdf_torus_t to;
            memcpy(to.c, (mu_vec3){0.0f, 0.0f, 0.0f}, sizeof(to.c));
            to.major = 1.0f;
            to.minor = 0.35f;
            s = mu_shape_from_sdf(mu_sdf_torus, &to, (mu_vec3){-1.6f, -0.6f, -1.6f},
                                  (mu_vec3){1.6f, 0.6f, 1.6f}, 28, 0.0f);
            st = examine("sdf_torus", mu_shape_clone(&s));
            check(st.euler == 0 && st.genus == 1, "sdf torus: genus 1");
            check_near("sdf torus volume", st.volume,
                       2.0 * MU_SHAPE_PI * MU_SHAPE_PI * 1.0 * 0.1225, 0.03);
            mu_shape_free(&s);
        }

        {
            /* smooth union of two spheres = a peanut: one closed component,
             * volume between one and two spheres. */
            blob_t b;
            float v1;
            /* k = 0.2: the polynomial smin's zero set grows spurious components
             * once k approaches the feature radius (measured: k = 0.6 turns this
             * two-sphere blend into three closed components, Euler 6). */
            mu_vec3_make(b.c, 0.2f, 0.0f, 0.0f);   /* x = smooth-blend radius */
            b.r = 0.6f;

            s = mu_shape_from_sdf(sdf_two_blobs, &b, (mu_vec3){-1.6f, -1.0f, -1.0f},
                                  (mu_vec3){1.6f, 1.0f, 1.0f}, 32, 0.0f);
            st = examine("sdf_blobs", mu_shape_clone(&s));
            v1 = 4.0f / 3.0f * MU_SHAPE_PI * 0.216f;
            check(st.watertight, "sdf blobs: watertight");
            check(st.genus == 0, "sdf blobs: one component, genus 0");
            check(st.volume > v1 && st.volume < 2.0f * v1,
                  "sdf blobs: volume between one and two spheres");
            mu_shape_free(&s);
        }

        {
            /* A gyroid is the "no parametrization exists" case: an infinite
             * periodic surface, clipped by the box, so it must be OPEN. */
            float s2 = 3.0f;
            s = mu_shape_from_sdf(sdf_gyroid, &s2, (mu_vec3){-2.0f, -2.0f, -2.0f},
                                  (mu_vec3){2.0f, 2.0f, 2.0f}, 24, 0.0f);
            st = examine("sdf_gyroid", mu_shape_clone(&s));
            check(st.boundary_edges > 0, "sdf gyroid: open where it leaves the box");
            check(st.nonmanifold_edges == 0, "sdf gyroid: manifold");
            check(st.ntriangles > 1000, "sdf gyroid: actually produced geometry");
            mu_shape_free(&s);
        }
    }

    /* ---- operations ---- */

    {
        mu_shape s = mu_shape_sphere(24, 12, 1.0f);
        mu_shape_stats before = examine("weld_in", mu_shape_clone(&s));
        int removed = mu_shape_weld(&s, 1e-4f, MU_SHAPE_WELD_NONE);
        mu_shape_stats after = examine("weld_out", mu_shape_clone(&s));

        check(removed > 0, "weld: merged the UV seam");
        check(after.npoints == before.npoints - (uint32_t)removed,
              "weld: reported count matches the vertex delta");
        check(after.ntriangles == before.ntriangles, "weld: no triangle was lost");
        check(after.duplicate_points == 0, "weld: no coincident vertices remain");
        check(after.watertight == 1, "weld: still closed");
        check(after.euler == 2, "weld: Euler characteristic unchanged");
        check_near("weld: volume unchanged", after.volume, before.volume, 1e-5);
        mu_shape_free(&s);

        /* UV-aware welding must NOT merge the seam (u=0 and u=1 differ) */
        s = mu_shape_sphere(24, 12, 1.0f);
        check(mu_shape_weld(&s, 1e-4f, MU_SHAPE_WELD_UV) == 0,
              "weld(UV): a UV seam is preserved");
        mu_shape_free(&s);
    }

    {
        mu_shape s = mu_shape_cube(2, 2.0f);
        mu_shape unwelded;
        mu_shape_stats st;
        float v0 = mu_shape_volume(&s);

        mu_shape_unweld(&s);
        check(s.npoints == s.ntriangles * 3u, "unweld: one vertex per corner");
        check_near("unweld: volume unchanged", mu_shape_volume(&s), v0, 1e-5);
        mu_shape_unweld(&s);          /* idempotent-ish: unwind once more */
        check_near("unweld x2: volume unchanged", mu_shape_volume(&s), v0, 1e-5);

        st = examine("unweld", mu_shape_clone(&s));
        check(st.duplicate_points > 0, "unweld: everything is duplicated");

        unwelded = mu_shape_clone(&s);
        mu_shape_remove_degenerate(&unwelded, 1e-6f);
        check(unwelded.ntriangles == s.ntriangles,
              "remove_degenerate: keeps a good mesh intact");
        mu_shape_free(&unwelded);
        mu_shape_free(&s);
    }

    {
        mu_shape a = mu_shape_cube(1, 1.0f);
        mu_shape b = mu_shape_cube(1, 1.0f);
        uint32_t na = a.npoints, nb = b.npoints;
        float va = mu_shape_volume(&a);

        check(mu_shape_merge(&a, &b) == 1, "merge: succeeds");
        check(a.npoints == na + nb, "merge: vertex counts add");
        check(a.ntriangles == 2u * b.ntriangles, "merge: triangle counts add");
        check_near("merge: volume adds", mu_shape_volume(&a), 2.0 * va, 1e-5);
        check(mu_shape_validate(&a, NULL, 0), "merge: result is valid");
        mu_shape_free(&a);
        mu_shape_free(&b);
    }

    {
        mu_shape s = mu_shape_sphere(16, 8, 1.0f);
        float v = mu_shape_volume(&s);
        mu_shape_stats st;

        mu_shape_flip(&s);
        check_near("flip: volume changes sign", mu_shape_volume(&s), -v, 1e-5);
        st = examine("flip", mu_shape_clone(&s));
        check(st.inconsistent_edges == 0, "flip: still consistently wound");
        check(st.euler == 2, "flip: topology unchanged");
        mu_shape_free(&s);
    }

    {
        /* A positive scale: volume scales by the determinant, normals stay
         * outward, and nothing needs fixing. */
        mu_shape s = mu_shape_icosphere(2, 1.0f);
        mu_mat4 m;
        mu_mat4_scale(m, (mu_vec3){2.0f, 2.0f, 2.0f});
        mu_shape_stats st;
        float v0 = mu_shape_volume(&s);

        mu_shape_transform(&s, &m);
        st = examine("transform_scale", mu_shape_clone(&s));
        check_near("transform: volume scales with the determinant", st.volume,
                   8.0 * v0, 1e-4);
        check_near("transform: area scales with the square", st.area,
                   4.0 * 4.0 * MU_SHAPE_PI * 0.98, 0.05);
        check_normals(&s, "transform_scale");
        mu_shape_free(&s);
    }

    {
        /* A REFLECTION (det < 0). Mirroring swaps inside and outside, so an
         * outward-wound closed mesh becomes inward-wound: volume goes negative
         * and mu_shape_flip() is how you ask for it back. What must NOT happen
         * is the normals disagreeing with the winding, which is the difference
         * between the cofactor (correct) and inverse-transpose (silently
         * negates the z component of every normal under a reflection).
         *
         * This is the mu_math 3.16 rule, tested through the mesh path. */
        mu_shape s0 = mu_shape_icosphere(2, 1.0f);
        mu_shape s = mu_shape_clone(&s0);
        mu_mat4 m;
        mu_mat4_scale(m, (mu_vec3){-1.0f, 1.0f, 1.0f});
        mu_shape_stats st;
        float v0 = mu_shape_volume(&s0);
        uint32_t i, cofactor_mismatch = 0, invt_agrees = 0;

        mu_shape_transform(&s, &m);
        st = examine("transform_reflect", mu_shape_clone(&s));
        check_near("transform(reflect): |volume| preserved, sign flipped", st.volume,
                   -v0, 1e-4);
        check(st.watertight, "transform(reflect): still closed");

        /* the normal mu_shape_transform produced must be the cofactor one */
        for (i = 0; i < s.npoints; ++i)
        {
            mu_vec3 want, wn;
            mu_mat4_normal(wn, m, s0.verts[i].n);
            mu_vec3_normalize(want, wn);
            if (mu_vec3_dot(want, s.verts[i].n) < 0.999f) ++cofactor_mismatch;
        }
        check(cofactor_mismatch == 0,
              "transform(reflect): normals are the cofactor normals");

        /* and inverse-transpose would give the opposite of that on every vertex,
         * which is the 3.16 bug: mu_mat4_inverse's transpose, applied to n */
        for (i = 0; i < s.npoints; ++i)
        {
            mu_mat4 inv;
            mu_vec3 n_invt;
            const float *nv = s0.verts[i].n;
            mu_mat4_inverse(m, inv);
            /* column-major: what an inverse-transpose applied to n would give */
            mu_vec3_make(n_invt,
                         inv[0] * nv[0] + inv[4] * nv[1] + inv[8]  * nv[2],
                         inv[1] * nv[0] + inv[5] * nv[1] + inv[9]  * nv[2],
                         inv[2] * nv[0] + inv[6] * nv[1] + inv[10] * nv[2]);
            mu_vec3_normalize(n_invt, n_invt);
            if (mu_vec3_dot(n_invt, s.verts[i].n) > 0.999f) ++invt_agrees;
        }
        check(invt_agrees < s.npoints / 10,
              "transform(reflect): inverse-transpose disagrees (the 3.16 bug)");
        mu_shape_free(&s);
        mu_shape_free(&s0);
        mu_shape_free(&s);
    }

    {
        /* Loop subdivision invariants: V' = V + E and F' = 4F per level.
         *
         * NOTE on area: Loop's limit surface for a coarse icosahedron SHRINKS
         * (~65% of the original area after three levels) because the vertex rule
         * pulls every valence-5 vertex toward its neighbours. That is the scheme
         * working, not a bug — it is why mu_shape_icosphere() re-projects onto
         * the sphere after subdividing. So the assertions here are topological,
         * plus "it shrinks but stays a plausible closed surface", and the
         * analytic-area claim is made on the projected icosphere above. */
        mu_shape s = mu_shape_icosahedron(1.0f);
        mu_shape_stats s0 = examine("subdiv0", mu_shape_clone(&s));
        mu_shape_stats s1;
        int v0 = (int)s.npoints, f0 = (int)s.ntriangles;
        int e0;

        e0 = (s0.euler == 2) ? (v0 + f0 - 2) : 0;   /* V - E + F = 2 */
        check(e0 > 0, "subdivide: Euler relation holds before subdividing");

        check(mu_shape_loop_subdivide(&s, 1) == 1, "subdivide: one level runs");
        check((int)s.npoints == v0 + e0, "subdivide: V' == V + E");
        check((int)s.ntriangles == 4 * f0, "subdivide: F' == 4F");

        {
            s1 = examine("subdiv1", mu_shape_clone(&s));
            check(s1.watertight, "subdivide: still watertight");
            check(s1.euler == 2, "subdivide: Euler characteristic preserved");
            check(s1.volume > 0.0f, "subdivide: still outward-wound");
            check(s1.volume < 4.0f / 3.0f * MU_SHAPE_PI,
                  "subdivide: the limit surface sits inside the unit sphere");
        }
        check(mu_shape_loop_subdivide(&s, 2) == 1, "subdivide: three levels total");
        {
            mu_shape_stats s3 = examine("subdiv3", mu_shape_clone(&s));
            check(s3.euler == 2 && s3.watertight, "subdivide: stable over 3 levels");
            check(s3.area > 0.5f * s1.area,
                  "subdivide: the smoothed surface does not collapse");
        }
        mu_shape_free(&s);
    }

    {
        /* OBJ export: count the lines, and check the file is not empty. */
        mu_shape s = mu_shape_cube(1, 1.0f);
        FILE *f;
        char line[256];
        int vlines = 0, flines = 0;

        check(mu_shape_export_obj(&s, "shapes_out.obj") == 1, "export_obj: writes");
        f = fopen("shapes_out.obj", "rb");
        check(f != NULL, "export_obj: file opens");
        if (f)
        {
            while (fgets(line, sizeof line, f))
            {
                if (line[0] == 'v' && line[1] == ' ') ++vlines;
                if (line[0] == 'f' && line[1] == ' ') ++flines;
            }
            fclose(f);
        }
        check(vlines == (int)s.npoints, "export_obj: one v line per point");
        check(flines == (int)s.ntriangles, "export_obj: one f line per triangle");
        mu_shape_free(&s);
    }

    /* ---- validate must actually fail on broken input ---- */

    {
        mu_shape s = mu_shape_cube(1, 1.0f);
        char err[128];
        mu_shape bad = mu_shape_clone(&s);

        bad.triangles[0] = 9999;
        check(mu_shape_validate(&bad, err, sizeof err) == 0,
              "validate: catches an out-of-range index");
        check(err[0] != '\0', "validate: explains the failure");
        mu_shape_free(&bad);

        bad = mu_shape_clone(&s);
        bad.triangles[1] = bad.triangles[0];
        check(mu_shape_validate(&bad, err, sizeof err) == 0,
              "validate: catches a repeated index");
        mu_shape_free(&bad);

        bad = mu_shape_clone(&s);
        bad.verts[3].p[0] = NAN;
        check(mu_shape_validate(&bad, err, sizeof err) == 0,
              "validate: catches NaN");
        mu_shape_free(&bad);

        check(mu_shape_validate(&s, err, sizeof err) == 1, "validate: passes a good mesh");
        mu_shape_free(&s);
    }

    /* ---- stats on a deliberately half-inverted mesh ---- */

    {
        /* Flip one cap of a cube: the mesh stays closed (Euler 2) but its volume
         * is wrong. This is the exact failure mode the analytic volume checks
         * above exist for, so prove that the check can see it. */
        mu_shape s = mu_shape_cube(1, 1.0f);
        mu_shape_stats whole, broken;
        uint32_t i, start = s.ntriangles - 2;

        whole = examine("cube_ok", mu_shape_clone(&s));
        for (i = start; i < s.ntriangles; ++i)
        {
            MU_SHAPES_T t = s.triangles[3u * i + 1u];
            s.triangles[3u * i + 1u] = s.triangles[3u * i + 2u];
            s.triangles[3u * i + 2u] = t;
        }
        broken = examine("cube_broken", mu_shape_clone(&s));
        check(broken.euler == whole.euler, "half-inverted cube: still Euler 2");
        check(fabsf(broken.volume - whole.volume) > 0.3f * whole.volume,
              "half-inverted cube: volume check catches the flipped face");
        mu_shape_free(&s);
    }

    printf("--------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED", g_checks,
           g_fail);
    return g_fail ? 1 : 0;
}
