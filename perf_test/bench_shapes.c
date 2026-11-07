/* bench_shapes.c — what generation and mesh surgery actually cost
 *
 * Every row is a whole operation on a whole mesh, because that is the size at
 * which shapes are used: a shape is generated once and welded once. Reporting
 * "ns per vertex" for a generator would hide the trig, and reporting "ms per
 * bake" for a 20-triangle shape would measure the clock.
 *
 *   make -C perf_test shapes-bench
 *   ./bench_shapes --filter=sdf --samples=21
 *   ./bench_shapes --list
 *
 * Payload sizes are in the row names so a number is never quoted without the
 * thing it was measured on. The par_shapes.h comparison lives in
 * bench_shapes_vs_par.c + shapes_vs_par.sh.
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"          /* first: sets _POSIX_C_SOURCE */

#include "mu_shapes.h"

#include <math.h>
#include <string.h>

static volatile uint32_t g_sink;

/* ---------------------------------------------------------------------------
 * Shared subjects. Built on first use, so they cost nothing for a filtered run,
 * and never inside a timed loop.
 * ------------------------------------------------------------------------- */

static int g_have_big;
static mu_shape g_big;          /* 512x256 UV sphere: 131 586 points, a UV seam
                                 * and two poles for the weld to find */
static int g_have_ico4;
static mu_shape g_ico4;         /* icosphere level 4: 5120 triangles */

static mu_shape *subject_big(void)
{
    if (!g_have_big) { g_big = mu_shape_sphere(512, 256, 1.0f); g_have_big = 1; }
    return &g_big;
}

static mu_shape *subject_ico4(void)
{
    if (!g_have_ico4) { g_ico4 = mu_shape_icosphere(4, 1.0f); g_have_ico4 = 1; }
    return &g_ico4;
}

static float sdf_gyroid(mu_vec3 p, void *user)
{
    float s = *(const float *)user;
    return sinf(p.x * s) * cosf(p.y * s) + sinf(p.y * s) * cosf(p.z * s) +
           sinf(p.z * s) * cosf(p.x * s);
}

/* ---------- generation ---------- */

MU_BENCH(gen, uv_sphere_64x32)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_sphere(64, 32, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, uv_sphere_256x128)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_sphere(256, 128, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, icosphere_level4)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_icosphere(4, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, icosphere_level5)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_icosphere(5, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, supershape_48x24)
{
    mu_shape_superformula f = mu_shape_superformula_star();
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_supershape(&f, 48, 24, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, torus_128x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_torus(128, 64, 0.3f, 1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, tube_256x12)
{
    mu_vec3 path[256];
    uint32_t i;

    /* deterministic zig-zag: the shape does not matter, the frame pass does */
    for (i = 0; i < 256; ++i)
    {
        float t = (float)i / 255.0f;
        path[i] = mu_vec3_make(sinf(t * 6.0f), t * 4.0f, cosf(t * 5.0f));
    }

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_tube(path, 256, 0.2f, 12, 0, 1);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, extrude_star_64)
{
    mu_vec2 poly[64];
    int j;
    uint32_t i;

    for (j = 0; j < 64; ++j)
    {
        float a = MU_SHAPE_TAU * (float)j / 64.0f;
        float r = (j & 1) ? 0.6f : 1.0f;
        poly[j] = mu_vec2_make(r * cosf(a), r * sinf(a));
    }

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_extrude(poly, 64, 1.0f, 0.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, lsystem_bush5)
{
    static const char *const bush[] = { "X=F[+X][-X]FX", "F=FF" };
    mu_lsystem_config c;
    uint32_t i;

    memset(&c, 0, sizeof c);
    c.axiom = "X";
    c.iterations = 5;          /* 4992 points, 6752 triangles */
    c.slices = 8;
    c.radius = 0.02f;
    c.length = 0.2f;
    c.angle = 25.7f;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_lsystem(&c, bush, 2);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(gen, platonic_dodecahedron)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_dodecahedron(1.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

/* ---------- SDF bakes (the expensive ones) ---------- */

MU_BENCH(sdf, sphere_res32)
{
    mu_sdf_sphere_t sp;
    uint32_t i;

    sp.c = mu_vec3_make(0.0f, 0.0f, 0.0f);
    sp.r = 1.0f;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_from_sdf(mu_sdf_sphere, &sp,
                                       mu_vec3_make(-1.4f, -1.4f, -1.4f),
                                       mu_vec3_make(1.4f, 1.4f, 1.4f), 32, 0.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

MU_BENCH(sdf, gyroid_res24)
{
    float amp = 3.0f;
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape s = mu_shape_from_sdf(sdf_gyroid, &amp,
                                       mu_vec3_make(-2.0f, -2.0f, -2.0f),
                                       mu_vec3_make(2.0f, 2.0f, 2.0f), 24, 0.0f);
        g_sink += s.ntriangles;
        mu_shape_free(&s);
    }
}

/* ---------- surgery ---------- */

/* The clone row exists so the reader can subtract it from the weld row: weld
 * destroys duplicates, so the subject has to be rebuilt every iteration. */
MU_BENCH(op, clone_sphere_512x256)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape c = mu_shape_clone(subject_big());
        g_sink += c.npoints;
        mu_shape_free(&c);
    }
}

MU_BENCH(op, weld_uv_sphere_512x256)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape c = mu_shape_clone(subject_big());
        mu_shape_weld(&c, 1e-4f, MU_SHAPE_WELD_NONE);
        g_sink += c.npoints;
        mu_shape_free(&c);
    }
}

MU_BENCH(op, compute_normals_sphere_512x256)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape_compute_normals(subject_big());
        MU_BENCH_DO_NOT_OPTIMIZE(g_big.verts[0].n.x);
    }
}

MU_BENCH(op, stats_sphere_512x256)
{
    mu_shape_stats st;
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape_stats_compute(subject_big(), 0.0f, &st);
        MU_BENCH_DO_NOT_OPTIMIZE(st.euler);
    }
}

MU_BENCH(op, validate_sphere_512x256)
{
    char err[128];
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        g_sink += (uint32_t)mu_shape_validate(subject_big(), err, sizeof err);
    }
}

MU_BENCH(op, clone_icosphere4)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape c = mu_shape_clone(subject_ico4());
        g_sink += c.npoints;
        mu_shape_free(&c);
    }
}

/* The weld MAP only, on a preallocated map: the exact counterpart of
 * ref.weld_27probe below, with no clone and no topology rebuild, so the A/B
 * ratio measures the neighbourhood search and nothing else. */
MU_BENCH(op, weld_map_512x256)
{
    mu_shape *big = subject_big();
    uint32_t *map;
    uint32_t i;

    map = (uint32_t *)MU_MALLOC((size_t)big->npoints * sizeof *map);
    if (!map) return;
    map[0] = 0;

    MU_BENCH_ITERATE(i)
    {
        mu_shape__weld_map(big, 1e-4f, MU_SHAPE_WELD_NONE, map);
        g_sink += map[0];
    }
    MU_FREE(map);
}

/* Clone included, subtract clone_icosphere4 (2% of the row).
 *
 * This row is also the regression test for the edge-hash bug: hashing the
 * key+1 offset instead of the raw key started every probe in the wrong bucket
 * and made subdivision O(n^2). Level 5 went from 175.9 ms to 5.1 ms. */
MU_BENCH(op, loop_subdivide_icosphere4)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape c = mu_shape_clone(subject_ico4());
        mu_shape_loop_subdivide(&c, 1);
        g_sink += c.ntriangles;
        mu_shape_free(&c);
    }
}


/* ---------------------------------------------------------------------------
 * Frozen pre-fix references. These are copies of algorithms that were in
 * mu_shapes.h and were replaced; they are kept ONLY so the replacement can be
 * measured in the same process, on the same mesh, in the same run. Do not
 * update them. Their row names start with ref. and the numbers are evidence,
 * not API.
 * ------------------------------------------------------------------------- */

/* The 3x3x3 neighbourhood weld (before the two-cell-per-axis rewrite): 27 hash
 * probes per vertex into a multi-megabyte table, each one a cache miss. */
static void ref_weld_map_27(const mu_shape *s, float eps, uint32_t *map)
{
    uint32_t table_size, mask, i;
    uint32_t *head, *next;
    float eps2 = eps * eps;

    for (i = 0; i < s->npoints; ++i) map[i] = i;
    if (s->npoints < 2) return;

    table_size = mu_shape__next_pow2(s->npoints * 2u + 8u);
    mask = table_size - 1u;
    head = (uint32_t *)MU_MALLOC((size_t)table_size * sizeof *head);
    next = (uint32_t *)MU_MALLOC((size_t)s->npoints * sizeof *next);
    if (!head || !next) { MU_FREE(head); MU_FREE(next); return; }
    for (i = 0; i < table_size; ++i) head[i] = MU_SHAPE_NONE;

    for (i = 0; i < s->npoints; ++i)
    {
        int32_t cx = (int32_t)floorf(s->verts[i].p.x / eps);
        int32_t cy = (int32_t)floorf(s->verts[i].p.y / eps);
        int32_t cz = (int32_t)floorf(s->verts[i].p.z / eps);
        uint32_t best = i;
        int dx, dy, dz;

        for (dz = -1; dz <= 1 && best == i; ++dz)
        for (dy = -1; dy <= 1 && best == i; ++dy)
        for (dx = -1; dx <= 1 && best == i; ++dx)
        {
            uint32_t b = mu_shape__hash3(cx + dx, cy + dy, cz + dz, mask);
            uint32_t c = head[b];
            while (c != MU_SHAPE_NONE)
            {
                if (mu_vec3_length2(mu_vec3_sub(s->verts[c].p, s->verts[i].p)) <= eps2)
                { best = map[c]; break; }
                c = next[c];
            }
        }
        map[i] = best;

        {
            uint32_t b = mu_shape__hash3(cx, cy, cz, mask);
            next[i] = head[b];
            head[b] = i;
        }
    }
    MU_FREE(head);
    MU_FREE(next);
}

MU_BENCH(ref, weld_27probe)
{
    mu_shape *big = subject_big();
    uint32_t *map;
    uint32_t i;

    map = (uint32_t *)MU_MALLOC((size_t)big->npoints * sizeof *map);
    if (!map) return;
    map[0] = 0;
    MU_BENCH_ITERATE(i)
    {
        ref_weld_map_27(big, 1e-4f, map);
        g_sink += map[0];
    }
    MU_FREE(map);
}

/* Loop subdivision with the offset-key hash bug: the table stores key+1 so that
 * 0 can mean empty, and the old lookup hashed the ALREADY offset key, so every
 * probe started in the wrong bucket and walked until it found the right one.
 * Correct, and O(n^2). */
static int ref_subdivide_offsetkey(mu_shape *s)
{
    mu_shape__edges e;
    mu_vec3 *vsum, *vnew;
    uint32_t *deg, *emap;
    uint32_t i, nv, nt;
    mu_shape out = mu_shape_empty();

    if (!s || s->ntriangles == 0) return 0;
    if (!mu_shape__edges_build(s, NULL, &e)) return 0;

    nv = s->npoints + e.count;
    nt = s->ntriangles * 4u;
    if (!mu_shape_reserve(&out, nv, nt)) { mu_shape__edges_free(&e); return 0; }

    vsum = (mu_vec3 *)MU_MALLOC((size_t)s->npoints * sizeof *vsum);
    vnew = (mu_vec3 *)MU_MALLOC((size_t)nv * sizeof *vnew);
    deg = (uint32_t *)MU_MALLOC((size_t)s->npoints * sizeof *deg);
    emap = (uint32_t *)MU_MALLOC((size_t)(e.mask + 1u) * sizeof *emap);
    if (!vsum || !vnew || !deg || !emap)
    {
        MU_FREE(vsum); MU_FREE(vnew); MU_FREE(deg); MU_FREE(emap);
        mu_shape_free(&out); mu_shape__edges_free(&e);
        return 0;
    }
    for (i = 0; i < s->npoints; ++i) { vsum[i] = mu_vec3_zero(); deg[i] = 0; }
    for (i = 0; i < e.mask + 1u; ++i) emap[i] = MU_SHAPE_NONE;

    {
        uint32_t k = s->npoints;
        for (i = 0; i < e.mask + 1u; ++i)
        {
            mu_shape__edge *cur = &e.slots[i];
            mu_vec3 p;
            if (cur->key == 0u) continue;
            vsum[cur->v0] = mu_vec3_add(vsum[cur->v0], s->verts[cur->v1].p);
            vsum[cur->v1] = mu_vec3_add(vsum[cur->v1], s->verts[cur->v0].p);
            ++deg[cur->v0];
            ++deg[cur->v1];
            if (cur->count == 2u)
                p = mu_vec3_add(
                        mu_vec3_scale(mu_vec3_add(s->verts[cur->v0].p,
                                                  s->verts[cur->v1].p), 0.375f),
                        mu_vec3_scale(mu_vec3_add(s->verts[cur->opp[0]].p,
                                                  s->verts[cur->opp[1]].p), 0.125f));
            else
                p = mu_vec3_scale(mu_vec3_add(s->verts[cur->v0].p,
                                              s->verts[cur->v1].p), 0.5f);
            vnew[k] = p;
            emap[i] = k;
            ++k;
        }
    }

    for (i = 0; i < s->npoints; ++i)
    {
        if (deg[i] == 0) { vnew[i] = s->verts[i].p; continue; }
        {
            float beta = (deg[i] == 3u) ? 3.0f / 16.0f : 3.0f / (8.0f * (float)deg[i]);
            float keep = 1.0f - beta * (float)deg[i];
            vnew[i] = mu_vec3_add(mu_vec3_scale(s->verts[i].p, keep),
                                  mu_vec3_scale(vsum[i], beta));
        }
    }

    for (i = 0; i < nv; ++i)
    {
        float u = 0.0f, v = 0.0f;
        if (i < s->npoints) { u = s->verts[i].uv.x; v = s->verts[i].uv.y; }
        mu_shape_add_vertex(&out, vnew[i], u, v);
    }

    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t tri[3], mid[3];
        int k;
        for (k = 0; k < 3; ++k) tri[k] = (uint32_t)s->triangles[3u * i + (uint32_t)k];
        for (k = 0; k < 3; ++k)
        {
            uint32_t a = tri[k], b = tri[(k + 1) % 3];
            uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
            uint64_t key = (((uint64_t)lo << 32) | (uint64_t)hi) + 1u;   /* the bug */
            uint32_t slot = mu_shape__edge_hash(key, e.mask);
            while (e.slots[slot].key != key) slot = (slot + 1u) & e.mask;
            mid[k] = emap[slot];
        }
        mu_shape_add_triangle(&out, tri[0], mid[0], mid[2]);
        mu_shape_add_triangle(&out, tri[1], mid[1], mid[0]);
        mu_shape_add_triangle(&out, tri[2], mid[2], mid[1]);
        mu_shape_add_triangle(&out, mid[0], mid[1], mid[2]);
    }

    MU_FREE(vsum); MU_FREE(vnew); MU_FREE(deg); MU_FREE(emap);
    mu_shape__edges_free(&e);
    mu_shape_free(s);
    *s = mu_shape__finish(out);
    return 1;
}

MU_BENCH(ref, subdivide_level4_offsetkey)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        mu_shape c = mu_shape_clone(subject_ico4());
        ref_subdivide_offsetkey(&c);
        g_sink += c.ntriangles;
        mu_shape_free(&c);
    }
}

MU_BENCH_MAIN()
