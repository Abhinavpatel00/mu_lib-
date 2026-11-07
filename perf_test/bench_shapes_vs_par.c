/* bench_shapes_vs_par.c — mu_shapes.h against par_shapes.h, same tessellation
 *
 * par_shapes.h is the library this one replaces, so it is the only honest
 * baseline. Both headers are single-file and both are compiled into this one
 * translation unit; the row names are identical across the two suites so
 * shapes_vs_par.sh can join them and report ratios.
 *
 * What the comparison is NOT:
 *   - It is not a quality comparison. The meshes differ in point count (par
 *     does not weld, so a 64x32 sphere is 2145 points to par and 1985 to us)
 *     and in topology (our cylinder carries per-vertex normals and a welded
 *     seam). Counts are printed by the script so a ratio is never quoted
 *     without the work it bought.
 *   - It is not the whole story. mu_shape_supershape, mu_shape_tube,
 *     mu_shape_from_sdf, mu_shape_loop_subdivide, mu_shape_stats and
 *     mu_shape_validate have no par_shapes equivalent at all; those are
 *     measured in bench_shapes.c, not here.
 *
 *   make -C perf_test shapes-par
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"

#define PAR_SHAPES_IMPLEMENTATION 1
#include "par_shapes.h"

#include "mu_shapes.h"

#include <math.h>
#include <string.h>

static volatile uint32_t g_sink;

/* ---------------------------------------------------------------------------
 * par_shapes.h rows
 * ------------------------------------------------------------------------- */

MU_BENCH(par, cone_64x32)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_cone(64, 32);
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, cylinder_64x32)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_cylinder(64, 32);
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, torus_128x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_torus(128, 64, 0.3f);
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, sphere_64x32)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_parametric_sphere(64, 32);
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, sphere_64x32_weld)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_parametric_sphere(64, 32);
        par_shapes_mesh *w = par_shapes_weld(m, 1e-4f, NULL);
        g_sink += (uint32_t)w->npoints;
        par_shapes_free_mesh(w);
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, subdivided_sphere_4)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_subdivided_sphere(4);
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, icosahedron)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_icosahedron();
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, dodecahedron)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_dodecahedron();
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, cube)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_cube();
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

MU_BENCH(par, disk_64)
{
    float center[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3] = { 0.0f, 1.0f, 0.0f };
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        par_shapes_mesh *m = par_shapes_create_disk(1.0f, 64, center, normal);
        g_sink += (uint32_t)m->ntriangles;
        par_shapes_free_mesh(m);
    }
}

/* ---------------------------------------------------------------------------
 * mu_shapes.h rows: same geometry, same tessellation
 * ------------------------------------------------------------------------- */

MU_BENCH(mu, cone_64x32)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_cone(64, 32, 1.0f, 2.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, cylinder_64x32)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_cylinder(64, 32, 1.0f, 2.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, torus_128x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_torus(128, 64, 1.0f, 0.3f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, sphere_64x32)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_sphere(64, 32, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, sphere_64x32_weld)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_sphere(64, 32, 1.0f);
        mu_shape_weld(&s, 1e-4f, MU_SHAPE_WELD_NONE);
        g_sink += s.npoints;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, subdivided_sphere_4)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_icosphere(4, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, icosahedron)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_icosahedron(1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, dodecahedron)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_dodecahedron(1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, cube)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_cube(1, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(mu, disk_64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_disk(1.0f, 64, 0);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH_MAIN()
