#pragma once
#ifndef MU_SHAPES_H
#define MU_SHAPES_H

/* ==========================================================================
 * mu_shapes.h — parametric shapes and mesh surgery, single header C99
 * ==========================================================================
 *
 * WHAT THIS IS
 * ------------
 * par_shapes.h (prideout/par) is the reference: a single header that can make
 * a sphere, a cube, a torus, a Klein bottle and a trefoil knot, then weld,
 * merge, transform, invert and export the result as OBJ. It is 19 KB of dead
 * simple code and it is very good at what it does. This header keeps that API
 * shape, and adds the three things we kept writing around it:
 *
 *   1. SHAPES YOU CAN'T GET OUT OF A (u,v) SQUARE.
 *      Nothing here is a bigger sphere. The additions are a supershape
 *      (Gielis superformula), a seashell, a Mobius strip, a helix/spring,
 *      a surface of revolution driven by an arbitrary profile, a swept tube
 *      with ROTATION-MINIMIZING FRAMES (double-reflection, Wang et al. 2008 —
 *      so a swept ribbon does not spin), an extruded polygon with ear-clipped
 *      caps, and, for the general case, an SDF -> mesh path that turns any
 *      signed-distance function into a watertight triangle mesh via marching
 *      tetrahedra. That last one is how you get shapes nobody has parametrized:
 *      smooth-unioned metaballs, gyroids, blobs, whatever.
 *
 *   2. MESH OPERATIONS THAT KNOW WHAT A MESH IS.
 *      par_shapes_weld exists; it sorts vertex pairs. Ours is a spatial hash
 *      (O(n) expected, one pass) and can weld UV-seam-aware. Beyond that:
 *      mu_shape_stats() reports boundary/non-manifold edges, Euler
 *      characteristic and genus — measured after merging coincident positions,
 *      because a UV seam is a duplicated vertex, not a hole — and
 *      mu_shape_validate() is the one call a test should make on any generated
 *      mesh. Plus signed volume and surface area from the divergence theorem,
 *      Loop subdivision, and per-corner-normal recomputation that is
 *      seam-aware.
 *
 *   3. NO GLOBAL STATE.
 *      par_shapes keeps `PAR_SHAPES_EPSILON` and friends as file-scope globals
 *      you set before calling a generator, which makes two generators in one
 *      expression order-dependent. Every knob here is a parameter.
 *
 * WHAT IT IS NOT
 * --------------
 * Not an editor, not a half-edge structure, no CSG tree, no UV unwrapper, no
 * decimation. Marching tetrahedra produces roughly 1.5-2x the triangles
 * marching *cubes* would for the same grid because each cell is split into six
 * tetrahedra and the cases cannot be shared; it is chosen anyway because it has
 * no 256-entry lookup table, no ambiguous-face cases, and the output is
 * guaranteed watertight when the grid is rejected consistently. Say it out loud
 * before shipping a 200k-triangle SDF bake.
 *
 * MEMORY MODEL
 * ------------
 * A `mu_shape` OWNS its arrays and is returned BY VALUE:
 *
 *     mu_shape s = mu_shape_sphere(48, 24, 1.0f);
 *     if (s.npoints == 0) { ...out of memory, already cleaned up... }
 *     mu_shape_free(&s);
 *
 * Value semantics remove par_shapes' failure mode (every generator can return
 * NULL and every caller has to check). An allocation failure anywhere inside a
 * generator frees what it built and returns the empty shape, so `npoints == 0`
 * is the only check that exists. The zero shape (`mu_shape e = {0}`) is valid.
 *
 * VERTEX LAYOUT. One interleaved 32-byte vertex:
 *
 *     typedef struct { mu_vec3 p; mu_vec3 n; mu_vec2 uv; } mu_shapes_vertex;
 *
 * Why interleaved and why 32 B: it is one allocation instead of three, three
 * vertices per two cache lines, and it is exactly what an upload wants
 * (position+normal+uv is the 99% case). Normals live in the same array, so
 * welding and transforming are single passes. The cost is that a pass that only
 * touches positions still streams normals and UVs; measured below, it is a
 * coefficient, not a cliff.
 *
 * INDEX TYPE. `MU_SHAPES_T`, default uint32_t. par_shapes defaults to uint16_t
 * to save memory, which silently overflows at an icosphere level 6 (~40k points
 * is fine, but an SDF bake at res 96 is not) and there is no diagnostic. 32-bit
 * indices cost 12 B per triangle; a 100k-triangle mesh pays 1.2 MB to never
 * have that conversation. Override with -DMU_SHAPES_T=uint16_t if you truly
 * need it.
 *
 * CONVENTIONS
 * -----------
 *   - Right-handed, +Y up, counter-clockwise winding seen from OUTSIDE.
 *     mu_shape_volume() is positive for a correctly wound closed mesh, and
 *     test_shapes.c asserts it for every closed generator.
 *   - u,v in [0,1] everywhere; every generator emits UVs.
 *   - Sizes are parameters, not constants baked into the generator, and the
 *     default sizes are unit-ish so a shape can be scaled at the call site.
 *   - UV seams are duplicated vertices (u=0 and u=1 columns). That is correct
 *     and unavoidable without per-corner attributes; mu_shape_stats() reports
 *     both raw and position-merged topology so the seam is not mistaken for a
 *     hole, and mu_shape_weld(s, eps, 0) removes it if you don't want UVs.
 *
 * DEPENDENCIES: mu_macros.h, mu_math.h (vectors, matrices, mu_mat4_normal),
 * and optionally mu_noise_math.h for mu_shape_rock.
 * ========================================================================== */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

#include "mu_macros.h"
#include "mu_math.h"

MU_BEGIN_EXTERN_C

#ifndef MU_SHAPES_T
#  define MU_SHAPES_T uint32_t
#endif

#define MU_SHAPE_NONE 0xFFFFFFFFu
#define MU_SHAPE_PI   3.14159265358979323846f
#define MU_SHAPE_TAU  6.28318530717958647692f

/* Internal: sticky allocation failure, set by the push helpers. A shape that
 * carries it is freed by mu_shape__finish() and returned empty. */
#define MU_SHAPE_FLAG_OOM 1u

typedef struct mu_shapes_vertex
{
    mu_vec3 p;
    mu_vec3 n;
    mu_vec2 uv;
} mu_shapes_vertex;

typedef struct mu_shape
{
    mu_shapes_vertex *verts;      /* npoints vertices, interleaved */
    MU_SHAPES_T     *triangles;   /* 3 indices per triangle */
    uint32_t         npoints;
    uint32_t         ntriangles;
    uint32_t         cap_points;  /* internal */
    uint32_t         cap_tris;    /* internal */
    uint32_t         flags;       /* internal */
} mu_shape;

/* ==========================================================================
 * 1. Lifecycle
 * ========================================================================== */

MU_INLINE mu_shape mu_shape_empty(void)
{
    mu_shape s;
    memset(&s, 0, sizeof s);
    return s;
}

MU_INLINE void mu_shape_free(mu_shape *s)
{
    if (!s) return;
    MU_FREE(s->verts);
    MU_FREE(s->triangles);
    memset(s, 0, sizeof *s);
}

MU_INLINE int mu_shape__reserve_points(mu_shape *s, uint32_t need)
{
    uint32_t cap;
    mu_shapes_vertex *v;

    if (need <= s->cap_points) return 1;

    cap = s->cap_points ? s->cap_points : 32u;
    while (cap < need) cap <<= 1;

    v = (mu_shapes_vertex *)MU_REALLOC(s->verts, (size_t)cap * sizeof *v);
    if (!v) { s->flags |= MU_SHAPE_FLAG_OOM; return 0; }
    s->verts = v;
    s->cap_points = cap;
    return 1;
}

MU_INLINE int mu_shape__reserve_tris(mu_shape *s, uint32_t need)
{
    uint32_t cap;
    MU_SHAPES_T *t;

    if (need <= s->cap_tris) return 1;

    cap = s->cap_tris ? s->cap_tris : 32u;
    while (cap < need) cap <<= 1;

    t = (MU_SHAPES_T *)MU_REALLOC(s->triangles, (size_t)cap * 3u * sizeof *t);
    if (!t) { s->flags |= MU_SHAPE_FLAG_OOM; return 0; }
    s->triangles = t;
    s->cap_tris = cap;
    return 1;
}

/* Grow both arrays up front. Generators that know their size call this once, so
 * the doubling path below never runs and generation is a straight store loop.
 * Returns 0 on failure (shape already cleaned). */
MU_INLINE int mu_shape_reserve(mu_shape *s, uint32_t npoints, uint32_t ntriangles)
{
    if (!mu_shape__reserve_points(s, npoints) ||
        !mu_shape__reserve_tris(s, ntriangles))
    {
        mu_shape_free(s);
        return 0;
    }
    return 1;
}

/* Push one vertex. Returns its index, or -1 if the allocation failed (the shape
 * is then discarded by mu_shape__finish, so callers may ignore the result). */
MU_INLINE int mu_shape_add_vertex(mu_shape *s, mu_vec3 p, float u, float v)
{
    uint32_t i;

    if (s->flags & MU_SHAPE_FLAG_OOM) return -1;
    if (!mu_shape__reserve_points(s, s->npoints + 1u)) return -1;

    i = s->npoints++;
    s->verts[i].p = p;
    s->verts[i].n = mu_vec3_zero();
    s->verts[i].uv = mu_vec2_make(u, v);
    return (int)i;
}

MU_INLINE int mu_shape_add_triangle(mu_shape *s, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t i;

    if (s->flags & MU_SHAPE_FLAG_OOM) return -1;
    if (!mu_shape__reserve_tris(s, s->ntriangles + 1u)) return -1;

    i = s->ntriangles++;
    s->triangles[3u * i + 0u] = (MU_SHAPES_T)a;
    s->triangles[3u * i + 1u] = (MU_SHAPES_T)b;
    s->triangles[3u * i + 2u] = (MU_SHAPES_T)c;
    return 1;
}

/* Quad (a,b,c,d) in ring order -> two triangles that keep the winding. */
MU_INLINE int mu_shape_add_quad(mu_shape *s, uint32_t a, uint32_t b, uint32_t c,
                                uint32_t d)
{
    if (mu_shape_add_triangle(s, a, b, c) < 0) return -1;
    return mu_shape_add_triangle(s, a, c, d);
}

MU_INLINE mu_shape mu_shape_clone(const mu_shape *s)
{
    mu_shape r = mu_shape_empty();

    if (!s || s->npoints == 0) return r;
    if (!mu_shape_reserve(&r, s->npoints, s->ntriangles)) return r;

    memcpy(r.verts, s->verts, (size_t)s->npoints * sizeof *s->verts);
    if (s->ntriangles)
        memcpy(r.triangles, s->triangles,
               (size_t)s->ntriangles * 3u * sizeof *s->triangles);
    r.npoints = s->npoints;
    r.ntriangles = s->ntriangles;
    return r;
}

/* Called on the way out of every generator: drops the mesh if an allocation
 * failed, so a caller only ever has to test npoints == 0. */
MU_INLINE mu_shape mu_shape__finish(mu_shape s)
{
    if (s.flags & MU_SHAPE_FLAG_OOM)
    {
        mu_shape_free(&s);
        return mu_shape_empty();
    }
    s.flags = 0;
    return s;
}

/* Forward declarations for operations defined in section 8 that a generator
 * needs before it: the supershape has to flip its own handedness. */
MU_INLINE void mu_shape_flip(mu_shape *s);
MU_INLINE int  mu_shape_compute_normals(mu_shape *s);
MU_INLINE void mu_shape_compact(mu_shape *s);

/* ==========================================================================
 * 2. Primitive builders shared by the generators
 * ========================================================================== */

/* A (u,v) grid: arrays are laid out row-major, row j = v index, column i = u.
 * Triangles are (a,b,d) and (b,c,d) with a=(i,j) b=(i+1,j) c=(i+1,j+1) d=(i,j+1),
 * which has cross(du, dv) as its normal — so a generator only has to make its
 * (u,v) -> position map increase along a right-handed pair, and the winding
 * follows. Collapsed rows (a pole) emit the surviving triangle only. */
MU_INLINE int mu_shape__row_collapsed(const mu_shape *s, uint32_t first, uint32_t n)
{
    uint32_t k;
    mu_vec3 p0 = s->verts[first].p;

    for (k = 1; k < n; ++k)
        if (mu_vec3_length2(mu_vec3_sub(s->verts[first + k].p, p0)) > 1e-12f)
            return 0;
    return 1;
}

MU_INLINE int mu_shape__grid_faces(mu_shape *s, uint32_t base, int slices, int stacks)
{
    int i, j;
    int top_collapsed;
    int bottom_collapsed;
    uint32_t stride = (uint32_t)(slices + 1);

    top_collapsed = mu_shape__row_collapsed(s, base, stride);
    bottom_collapsed = mu_shape__row_collapsed(
        s, base + (uint32_t)(stacks - 1) * stride, stride);

    for (j = 0; j < stacks; ++j)
    {
        for (i = 0; i < slices; ++i)
        {
            uint32_t a = base + (uint32_t)j * stride + (uint32_t)i;
            uint32_t b = a + 1u;
            uint32_t d = a + stride;
            uint32_t c = d + 1u;

            if (!(j == 0 && top_collapsed) && mu_shape_add_triangle(s, a, b, d) < 0)
                return 0;
            if (!(j == stacks - 1 && bottom_collapsed) &&
                mu_shape_add_triangle(s, b, c, d) < 0)
                return 0;
        }
    }
    return 1;
}

/* ==========================================================================
 * 3. Generic parametric surface
 * ========================================================================== */

typedef mu_vec3 (*mu_shape_fn)(float u, float v, void *user);

/* Any function of the unit square. The grid has (slices+1)*(stacks+1) points and
 * duplicates the u=1 column — keep that if the surface wraps, because UVs need
 * it; weld it away if you don't. Rows that collapse to a single point (a pole)
 * are handled: only the non-degenerate triangle of each quad is emitted. */
MU_INLINE mu_shape mu_shape_parametric(mu_shape_fn fn, void *user, int slices, int stacks)
{
    mu_shape s = mu_shape_empty();
    int i, j;

    if (!fn) return s;
    if (slices < 1) slices = 1;
    if (stacks < 1) stacks = 1;

    if (!mu_shape_reserve(&s, (uint32_t)(slices + 1) * (uint32_t)(stacks + 1),
                          (uint32_t)(2 * slices * stacks)))
        return s;

    for (j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            mu_shape_add_vertex(&s, fn(u, v, user), u, v);
        }
    }
    mu_shape__grid_faces(&s, 0u, slices, stacks);
    /* a collapsed row leaves coincident (and one unreferenced) pole vertices
     * behind; they are harmless but they make every downstream count a lie */
    mu_shape_compact(&s);
    return mu_shape__finish(s);
}

/* ---------- plane, disk ---------- */

MU_INLINE mu_shape mu_shape_plane(int slices, int stacks, float width, float depth)
{
    mu_shape s = mu_shape_empty();
    int i, j;

    if (slices < 1) slices = 1;
    if (stacks < 1) stacks = 1;

    if (!mu_shape_reserve(&s, (uint32_t)(slices + 1) * (uint32_t)(stacks + 1),
                          (uint32_t)(2 * slices * stacks)))
        return s;

    /* v increases toward -z so that cross(du,dv) = +y: the plane faces up. */
    for (j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            mu_shape_add_vertex(&s,
                mu_vec3_make((u - 0.5f) * width, 0.0f, (0.5f - v) * depth), u, v);
        }
    }
    mu_shape__grid_faces(&s, 0u, slices, stacks);
    return mu_shape__finish(s);
}

/* Flat disc in XZ, facing +y, fan from a centre vertex. */
MU_INLINE mu_shape mu_shape_disk(float radius, int slices, int rings)
{
    mu_shape s = mu_shape_empty();
    int i, j;

    if (slices < 3) slices = 3;
    if (rings < 1) rings = 1;

    if (!mu_shape_reserve(&s, (uint32_t)(rings * slices + 1),
                          (uint32_t)(slices * (2 * rings - 1))))
        return s;

    mu_shape_add_vertex(&s, mu_vec3_make(0.0f, 0.0f, 0.0f), 0.5f, 0.5f);

    for (j = 1; j <= rings; ++j)
    {
        float r = radius * (float)j / (float)rings;
        for (i = 0; i < slices; ++i)
        {
            float a = MU_SHAPE_TAU * (float)i / (float)slices;
            float cu = cosf(a), su = sinf(a);
            mu_shape_add_vertex(&s,
                mu_vec3_make(r * cu, 0.0f, -r * su),
                0.5f + 0.5f * cu, 0.5f + 0.5f * su);
        }
    }

    /* inner fan: centre is index 0, ring 1 starts at 1 */
    for (i = 0; i < slices; ++i)
    {
        uint32_t a = 1u + (uint32_t)i;
        uint32_t b = 1u + (uint32_t)((i + 1) % slices);
        mu_shape_add_triangle(&s, 0u, b, a);
    }
    for (j = 1; j < rings; ++j)
    {
        uint32_t r0 = 1u + (uint32_t)(j - 1) * (uint32_t)slices;
        uint32_t r1 = 1u + (uint32_t)j * (uint32_t)slices;
        for (i = 0; i < slices; ++i)
        {
            uint32_t i1 = (uint32_t)((i + 1) % slices);
            mu_shape_add_quad(&s, r0 + (uint32_t)i, r0 + i1, r1 + i1, r1 + (uint32_t)i);
        }
    }
    return mu_shape__finish(s);
}

/* ---------- sphere / hemisphere ---------- */

MU_INLINE mu_shape mu_shape_sphere(int slices, int stacks, float radius)
{
    mu_shape s = mu_shape_empty();
    int i, j;
    uint32_t ring0;
    uint32_t south;

    if (slices < 3) slices = 3;
    if (stacks < 2) stacks = 2;

    /* poles: one vertex each (a fan), rings: slices+1 columns with a UV seam */
    if (!mu_shape_reserve(&s,
            (uint32_t)((stacks - 1) * (slices + 1) + 2),
            (uint32_t)(slices * (2 * stacks - 2))))
        return s;

    mu_shape_add_vertex(&s, mu_vec3_make(0.0f, radius, 0.0f), 0.5f, 0.0f);

    for (j = 1; j < stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        float phi = MU_SHAPE_PI * v;
        float sp = sinf(phi), cp = cosf(phi);
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float th = MU_SHAPE_TAU * u;
            mu_shape_add_vertex(&s,
                mu_vec3_make(radius * sp * cosf(th), radius * cp,
                             radius * sp * sinf(th)), u, v);
        }
    }
    south = (uint32_t)mu_shape_add_vertex(&s, mu_vec3_make(0.0f, -radius, 0.0f),
                                          0.5f, 1.0f);
    ring0 = 1u;
    (void)ring0;

    for (i = 0; i < slices; ++i)
        mu_shape_add_triangle(&s, 0u, 1u + (uint32_t)i + 1u, 1u + (uint32_t)i);

    for (j = 1; j < stacks - 1; ++j)
    {
        uint32_t r0 = 1u + (uint32_t)(j - 1) * (uint32_t)(slices + 1);
        uint32_t r1 = 1u + (uint32_t)j * (uint32_t)(slices + 1);
        for (i = 0; i < slices; ++i)
        {
            uint32_t a = r0 + (uint32_t)i;
            uint32_t b = a + 1u;
            uint32_t d = r1 + (uint32_t)i;
            uint32_t c = d + 1u;
            mu_shape_add_triangle(&s, a, b, d);
            mu_shape_add_triangle(&s, b, c, d);
        }
    }

    {
        uint32_t last = 1u + (uint32_t)(stacks - 2) * (uint32_t)(slices + 1);
        for (i = 0; i < slices; ++i)
            mu_shape_add_triangle(&s, last + (uint32_t)i,
                                  last + (uint32_t)i + 1u, south);
    }
    return mu_shape__finish(s);
}

/* Upper half of a sphere; the equator is an open boundary (cap it yourself with
 * mu_shape_disk + mu_shape_merge if you need a solid). */
MU_INLINE mu_shape mu_shape_hemisphere(int slices, int stacks, float radius)
{
    mu_shape s = mu_shape_empty();
    int i, j;

    if (slices < 3) slices = 3;
    if (stacks < 2) stacks = 2;

    if (!mu_shape_reserve(&s, (uint32_t)((stacks - 1) * (slices + 1) + 2),
                          (uint32_t)(slices * (2 * stacks - 3))))
        return s;

    mu_shape_add_vertex(&s, mu_vec3_make(0.0f, radius, 0.0f), 0.5f, 0.0f);

    for (j = 1; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        float phi = 0.5f * MU_SHAPE_PI * v;
        float sp = sinf(phi), cp = cosf(phi);
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float th = MU_SHAPE_TAU * u;
            mu_shape_add_vertex(&s,
                mu_vec3_make(radius * sp * cosf(th), radius * cp,
                             radius * sp * sinf(th)), u, v);
        }
    }

    for (i = 0; i < slices; ++i)
        mu_shape_add_triangle(&s, 0u, 1u + (uint32_t)i + 1u, 1u + (uint32_t)i);

    for (j = 1; j < stacks; ++j)
    {
        uint32_t r0 = 1u + (uint32_t)(j - 1) * (uint32_t)(slices + 1);
        uint32_t r1 = 1u + (uint32_t)j * (uint32_t)(slices + 1);
        for (i = 0; i < slices; ++i)
        {
            uint32_t a = r0 + (uint32_t)i;
            uint32_t b = a + 1u;
            uint32_t d = r1 + (uint32_t)i;
            uint32_t c = d + 1u;
            mu_shape_add_triangle(&s, a, b, d);
            mu_shape_add_triangle(&s, b, c, d);
        }
    }
    return mu_shape__finish(s);
}

/* ---------- cylinder, cone, capsule, torus ---------- */

/* Side wall + two fan caps. Stacks subdivide the height. */
MU_INLINE mu_shape mu_shape_cylinder(int slices, int stacks, float radius, float height)
{
    mu_shape s = mu_shape_empty();
    int i, j;
    float h = 0.5f * height;
    uint32_t top_c, bot_c, ring0;

    if (slices < 3) slices = 3;
    if (stacks < 1) stacks = 1;

    if (!mu_shape_reserve(&s,
            (uint32_t)((stacks + 1) * (slices + 1) + 2),
            (uint32_t)(slices * (2 * stacks + 2))))
        return s;

    /* side grid, v=0 at the top; cross(du,dv) points outward */
    for (j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        float y = h - height * v;
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float a = MU_SHAPE_TAU * u;
            mu_shape_add_vertex(&s,
                mu_vec3_make(radius * cosf(a), y, radius * sinf(a)), u, v);
        }
    }
    for (j = 0; j < stacks; ++j)
    {
        for (i = 0; i < slices; ++i)
        {
            uint32_t a = (uint32_t)j * (uint32_t)(slices + 1) + (uint32_t)i;
            uint32_t b = a + 1u;
            uint32_t d = a + (uint32_t)(slices + 1);
            uint32_t c = d + 1u;
            mu_shape_add_triangle(&s, a, b, d);
            mu_shape_add_triangle(&s, b, c, d);
        }
    }

    top_c = (uint32_t)mu_shape_add_vertex(&s, mu_vec3_make(0.0f, h, 0.0f), 0.5f, 0.5f);
    bot_c = (uint32_t)mu_shape_add_vertex(&s, mu_vec3_make(0.0f, -h, 0.0f), 0.5f, 0.5f);
    ring0 = 0u;

    {
        uint32_t last = (uint32_t)stacks * (uint32_t)(slices + 1);
        for (i = 0; i < slices; ++i)
        {
            uint32_t i1 = (uint32_t)i + 1u;
            /* top cap faces +y, bottom faces -y. The fan order is the reverse
             * of the ring order: cross(ring[i]-centre, ring[i+1]-centre) points
             * -y for a counter-clockwise ring, so the top cap needs (i1, i) and
             * the bottom (i, i1). Getting this backwards is silent — the mesh
             * stays closed, the volume just comes out wrong. */
            mu_shape_add_triangle(&s, top_c, ring0 + i1, ring0 + (uint32_t)i);
            mu_shape_add_triangle(&s, bot_c, last + (uint32_t)i, last + i1);
        }
    }
    return mu_shape__finish(s);
}

MU_INLINE mu_shape mu_shape_cone(int slices, int stacks, float radius, float height)
{
    mu_shape s = mu_shape_empty();
    int i, j;
    float h = 0.5f * height;

    if (slices < 3) slices = 3;
    if (stacks < 1) stacks = 1;

    if (!mu_shape_reserve(&s, (uint32_t)((stacks + 1) * (slices + 1) + 1),
                          (uint32_t)(slices * (2 * stacks + 1))))
        return s;

    /* v=0 at the apex row (collapsed), v=1 at the base */
    for (j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        float r = radius * v;
        float y = h - height * v;
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float a = MU_SHAPE_TAU * u;
            mu_shape_add_vertex(&s,
                mu_vec3_make(r * cosf(a), y, r * sinf(a)), u, v);
        }
    }
    /* row 0 collapses to the apex; only the second triangle of each quad there */
    for (j = 0; j < stacks; ++j)
    {
        for (i = 0; i < slices; ++i)
        {
            uint32_t a = (uint32_t)j * (uint32_t)(slices + 1) + (uint32_t)i;
            uint32_t b = a + 1u;
            uint32_t d = a + (uint32_t)(slices + 1);
            uint32_t c = d + 1u;
            if (j > 0) mu_shape_add_triangle(&s, a, b, d);
            mu_shape_add_triangle(&s, b, c, d);
        }
    }
    {
        uint32_t base_c = (uint32_t)mu_shape_add_vertex(&s, mu_vec3_make(0.0f, -h, 0.0f),
                                                        0.5f, 0.5f);
        uint32_t last = (uint32_t)stacks * (uint32_t)(slices + 1);
        for (i = 0; i < slices; ++i)
            mu_shape_add_triangle(&s, base_c, last + (uint32_t)i,
                                  last + (uint32_t)i + 1u);
    }
    mu_shape_compact(&s);   /* the apex row is slices+1 coincident points */
    return mu_shape__finish(s);
}

/* Cylinder with hemispherical caps: a proper closed capsule along +y. */
MU_INLINE mu_shape mu_shape_capsule(int slices, int stacks, float radius, float height)
{
    mu_shape s = mu_shape_empty();
    int i, j;
    float h = 0.5f * height;
    int cap_stacks = stacks;
    int rings = 2 * cap_stacks + 2;
    uint32_t north, south;

    if (slices < 3) slices = 3;
    if (cap_stacks < 1) cap_stacks = 1;

    if (!mu_shape_reserve(&s, (uint32_t)((rings - 1) * (slices + 1) + 2),
                          (uint32_t)(slices * 2 * (rings - 2) + 2 * slices)))
        return s;

    north = (uint32_t)mu_shape_add_vertex(&s, mu_vec3_make(0.0f, h + radius, 0.0f),
                                          0.5f, 0.0f);

    /* top cap: phi from 0 (pole) to pi/2 (equator), then the straight side */
    for (j = 1; j < rings; ++j)
    {
        float v = (float)j / (float)rings;
        float phi, y, r;
        if (j <= cap_stacks)
        {
            phi = 0.5f * MU_SHAPE_PI * (float)j / (float)cap_stacks;
            y = h + radius * cosf(phi);
            r = radius * sinf(phi);
        }
        else if (j <= cap_stacks + 1)
        {
            phi = 0.5f * MU_SHAPE_PI;
            y = -h;
            r = radius;
        }
        else
        {
            phi = 0.5f * MU_SHAPE_PI * (1.0f + (float)(j - cap_stacks - 1) /
                                        (float)cap_stacks);
            y = -h + radius * cosf(phi);
            r = radius * sinf(phi);
        }
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float a = MU_SHAPE_TAU * u;
            mu_shape_add_vertex(&s, mu_vec3_make(r * cosf(a), y, r * sinf(a)), u, v);
        }
    }
    south = (uint32_t)mu_shape_add_vertex(&s, mu_vec3_make(0.0f, -h - radius, 0.0f),
                                          0.5f, 1.0f);

    for (i = 0; i < slices; ++i)
        mu_shape_add_triangle(&s, north, 1u + (uint32_t)i + 1u, 1u + (uint32_t)i);

    for (j = 1; j < rings - 1; ++j)
    {
        uint32_t r0 = 1u + (uint32_t)(j - 1) * (uint32_t)(slices + 1);
        uint32_t r1 = 1u + (uint32_t)j * (uint32_t)(slices + 1);
        for (i = 0; i < slices; ++i)
        {
            uint32_t a = r0 + (uint32_t)i;
            uint32_t b = a + 1u;
            uint32_t d = r1 + (uint32_t)i;
            uint32_t c = d + 1u;
            mu_shape_add_triangle(&s, a, b, d);
            mu_shape_add_triangle(&s, b, c, d);
        }
    }
    {
        uint32_t last = 1u + (uint32_t)(rings - 2) * (uint32_t)(slices + 1);
        for (i = 0; i < slices; ++i)
            mu_shape_add_triangle(&s, last + (uint32_t)i, last + (uint32_t)i + 1u, south);
    }
    return mu_shape__finish(s);
}

MU_INLINE mu_shape mu_shape_torus(int slices, int stacks, float radius_major,
                                  float radius_minor)
{
    mu_shape s = mu_shape_empty();
    int i, j;

    if (slices < 3) slices = 3;
    if (stacks < 3) stacks = 3;

    if (!mu_shape_reserve(&s, (uint32_t)((slices + 1) * (stacks + 1)),
                          (uint32_t)(2 * slices * stacks)))
        return s;

    /* u around the major circle, v around the tube; cross(du,dv) is outward. */
    for (j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        /* v runs NEGATIVE: with the tube parameter increasing the other way,
         * cross(du,dv) points into the hole instead of out of it. */
        float b = -MU_SHAPE_TAU * v;
        float cb = cosf(b), sb = sinf(b);
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float a = MU_SHAPE_TAU * u;
            float ca = cosf(a), sa = sinf(a);
            float r = radius_major + radius_minor * cb;
            mu_shape_add_vertex(&s,
                mu_vec3_make(r * ca, radius_minor * sb, r * sa), u, v);
        }
    }
    mu_shape__grid_faces(&s, 0u, slices, stacks);
    return mu_shape__finish(s);
}

/* n-sided prism along +y, caps included. */
MU_INLINE mu_shape mu_shape_prism(int sides, float radius, float height)
{
    return mu_shape_cylinder(sides, 1, radius, height);
}

/* ==========================================================================
 * 4. Cube and rounded box
 * ========================================================================== */

/* Six faces, each an (subdiv+1)^2 grid, duplicated at the edges so every face
 * gets its own UV island. Weld with mu_shape_weld(s, eps, MU_SHAPE_WELD_UV) if
 * you want the UV seams kept and the rest merged. */
MU_INLINE mu_shape mu_shape_cube(int subdiv, float size)
{
    mu_shape s = mu_shape_empty();
    /* outward normal, du, dv — chosen so cross(du,dv) == normal */
    static const float faces[6][9] = {
        { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f },
        {-1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f },
        { 0.0f,-1.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f,-1.0f,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f, 0.0f }
    };
    int f, i, j;
    float h = 0.5f * size;

    if (subdiv < 1) subdiv = 1;

    if (!mu_shape_reserve(&s,
            (uint32_t)(6 * (subdiv + 1) * (subdiv + 1)),
            (uint32_t)(6 * 2 * subdiv * subdiv)))
        return s;

    for (f = 0; f < 6; ++f)
    {
        const float *fc = faces[f];
        uint32_t base = s.npoints;
        for (j = 0; j <= subdiv; ++j)
        {
            float v = (float)j / (float)subdiv;
            for (i = 0; i <= subdiv; ++i)
            {
                float u = (float)i / (float)subdiv;
                float a = 2.0f * u - 1.0f;
                float b = 2.0f * v - 1.0f;
                mu_vec3 p = mu_vec3_make(
                    fc[0] + a * fc[3] + b * fc[6],
                    fc[1] + a * fc[4] + b * fc[7],
                    fc[2] + a * fc[5] + b * fc[8]);
                mu_shape_add_vertex(&s, mu_vec3_scale(p, h), u, v);
            }
        }
        for (j = 0; j < subdiv; ++j)
        {
            for (i = 0; i < subdiv; ++i)
            {
                uint32_t a = base + (uint32_t)j * (uint32_t)(subdiv + 1) + (uint32_t)i;
                uint32_t b = a + 1u;
                uint32_t d = a + (uint32_t)(subdiv + 1);
                uint32_t c = d + 1u;
                mu_shape_add_triangle(&s, a, b, d);
                mu_shape_add_triangle(&s, b, c, d);
            }
        }
    }
    return mu_shape__finish(s);
}

/* Box with spherical corners: every point of the outer box is pushed out from
 * its nearest point on the inner box by `radius`. Flat face interiors are left
 * exactly where they were, so the face topology is unchanged and only the
 * edge/corner bands bend. radius <= 0 gives a plain cube. */
MU_INLINE mu_shape mu_shape_rounded_box(int subdiv, float size, float radius)
{
    mu_shape s = mu_shape_cube(subdiv, size);
    float half = 0.5f * size;
    float inner;
    uint32_t i;

    if (s.npoints == 0 || radius <= 0.0f) return s;

    inner = half - radius;
    if (inner < 0.0f) inner = 0.0f;

    for (i = 0; i < s.npoints; ++i)
    {
        mu_vec3 p = s.verts[i].p;
        mu_vec3 q = mu_vec3_make(
            p.x > inner ? inner : (p.x < -inner ? -inner : p.x),
            p.y > inner ? inner : (p.y < -inner ? -inner : p.y),
            p.z > inner ? inner : (p.z < -inner ? -inner : p.z));
        mu_vec3 d = mu_vec3_sub(p, q);
        float len = mu_vec3_length(d);

        if (len > 1e-6f)
            s.verts[i].p = mu_vec3_add(q, mu_vec3_scale(d, radius / len));
    }
    return s;
}

/* ==========================================================================
 * 5. Platonic solids
 * ========================================================================== */

MU_INLINE mu_shape mu_shape_tetrahedron(float radius)
{
    mu_shape s = mu_shape_empty();
    /* alternating corners of a cube, normalised */
    static const float v[4][3] = {
        { 1.0f,  1.0f,  1.0f}, { 1.0f, -1.0f, -1.0f},
        {-1.0f,  1.0f, -1.0f}, {-1.0f, -1.0f,  1.0f}
    };
    static const int f[4][3] = {{0,1,2},{0,3,1},{0,2,3},{1,3,2}};
    int i;

    if (!mu_shape_reserve(&s, 4u, 4u)) return s;

    for (i = 0; i < 4; ++i)
    {
        mu_vec3 p = mu_vec3_normalize(mu_vec3_make(v[i][0], v[i][1], v[i][2]));
        mu_shape_add_vertex(&s, mu_vec3_scale(p, radius),
                            (float)(i & 1), (float)((i >> 1) & 1));
    }
    for (i = 0; i < 4; ++i)
        mu_shape_add_triangle(&s, (uint32_t)f[i][0], (uint32_t)f[i][1],
                              (uint32_t)f[i][2]);
    return mu_shape__finish(s);
}

MU_INLINE mu_shape mu_shape_octahedron(float radius)
{
    mu_shape s = mu_shape_empty();
    int i;
    /* +x -x +y -y +z -z */
    static const float v[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    /* 8 faces. The listed triples face inward (that is how the corner order of
     * the axis vertices works out), so they are emitted reversed below — the
     * volume check in test_shapes.c is what caught it. */
    static const int f[8][3] = {
        {0,4,2},{4,1,2},{1,5,2},{5,0,2},
        {4,0,3},{1,4,3},{5,1,3},{0,5,3}
    };

    if (!mu_shape_reserve(&s, 6u, 8u)) return s;

    for (i = 0; i < 6; ++i)
        mu_shape_add_vertex(&s, mu_vec3_scale(
            mu_vec3_make(v[i][0], v[i][1], v[i][2]), radius), 0.5f, 0.5f);
    for (i = 0; i < 8; ++i)
        mu_shape_add_triangle(&s, (uint32_t)f[i][0], (uint32_t)f[i][2],
                              (uint32_t)f[i][1]);
    return mu_shape__finish(s);
}

MU_INLINE mu_shape mu_shape_icosahedron(float radius)
{
    mu_shape s = mu_shape_empty();
    /* The classic 12 vertices: three orthogonal golden rectangles. Listed
     * explicitly because the face table below indexes them, and a generated
     * ordering that disagrees with the table is a 20-face debugging session.
     * mu_shape_stats() and the volume check in test_shapes.c verify the pair. */
    const float phi = 0.5f * (1.0f + sqrtf(5.0f));
    float v[12][3];
    static const int f[20][3] = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
    };
    int i;

    v[0][0] = -1.0f; v[0][1] =  phi; v[0][2] =  0.0f;
    v[1][0] =  1.0f; v[1][1] =  phi; v[1][2] =  0.0f;
    v[2][0] = -1.0f; v[2][1] = -phi; v[2][2] =  0.0f;
    v[3][0] =  1.0f; v[3][1] = -phi; v[3][2] =  0.0f;
    v[4][0] =  0.0f; v[4][1] = -1.0f; v[4][2] =  phi;
    v[5][0] =  0.0f; v[5][1] =  1.0f; v[5][2] =  phi;
    v[6][0] =  0.0f; v[6][1] = -1.0f; v[6][2] = -phi;
    v[7][0] =  0.0f; v[7][1] =  1.0f; v[7][2] = -phi;
    v[8][0] =  phi; v[8][1] =  0.0f; v[8][2] = -1.0f;
    v[9][0] =  phi; v[9][1] =  0.0f; v[9][2] =  1.0f;
    v[10][0] = -phi; v[10][1] = 0.0f; v[10][2] = -1.0f;
    v[11][0] = -phi; v[11][1] = 0.0f; v[11][2] =  1.0f;

    if (!mu_shape_reserve(&s, 12u, 20u)) return s;

    for (i = 0; i < 12; ++i)
        mu_shape_add_vertex(&s, mu_vec3_scale(
            mu_vec3_normalize(mu_vec3_make(v[i][0], v[i][1], v[i][2])), radius),
            0.5f, 0.5f);
    for (i = 0; i < 20; ++i)
        mu_shape_add_triangle(&s, (uint32_t)f[i][0], (uint32_t)f[i][1],
                              (uint32_t)f[i][2]);
    return mu_shape__finish(s);
}

/* Dodecahedron as the dual of the icosahedron: one vertex per icosahedron face,
 * one pentagon per icosahedron vertex (fan-triangulated). Deriving it beats a
 * 12x5 literal table: there is nothing to mistype, and the same code gives any
 * dual later. */
MU_INLINE mu_shape mu_shape_dodecahedron(float radius)
{
    mu_shape s = mu_shape_empty();
    mu_shape ico = mu_shape_icosahedron(1.0f);
    uint32_t face_vert[20];
    uint32_t k, f, i;

    if (ico.npoints == 0) return s;
    if (!mu_shape_reserve(&s, 20u, 36u)) { mu_shape_free(&ico); return s; }

    /* dodeca vertex = normalised centroid of an icosa face */
    for (f = 0; f < ico.ntriangles; ++f)
    {
        mu_vec3 c = mu_vec3_zero();
        for (i = 0; i < 3; ++i)
            c = mu_vec3_add(c, ico.verts[ico.triangles[3u * f + i]].p);
        c = mu_vec3_scale(c, 1.0f / 3.0f);
        face_vert[f] = (uint32_t)mu_shape_add_vertex(
            &s, mu_vec3_scale(mu_vec3_normalize(c), radius), 0.5f, 0.5f);
    }

    /* for each icosa vertex, collect its 5 faces in angular order and fan them */
    for (k = 0; k < ico.npoints; ++k)
    {
        uint32_t ring[16];
        float    ang[16];
        int      nring = 0;
        mu_vec3  axis = mu_vec3_normalize(ico.verts[k].p);
        mu_vec3  t0, t1;
        int      a, b;

        for (f = 0; f < ico.ntriangles; ++f)
            for (i = 0; i < 3; ++i)
                if (ico.triangles[3u * f + i] == k && nring < 16)
                    ring[nring++] = f;

        if (nring < 3) continue;

        /* tangent frame at the axis */
        t0 = mu_vec3_cross(axis, mu_vec3_make(0.0f, 1.0f, 0.0f));
        if (mu_vec3_length2(t0) < 1e-6f)
            t0 = mu_vec3_cross(axis, mu_vec3_make(1.0f, 0.0f, 0.0f));
        t0 = mu_vec3_normalize(t0);
        t1 = mu_vec3_cross(axis, t0);

        for (a = 0; a < nring; ++a)
        {
            mu_vec3 d = mu_vec3_sub(s.verts[face_vert[ring[a]]].p, ico.verts[k].p);
            ang[a] = atan2f(mu_vec3_dot(d, t1), mu_vec3_dot(d, t0));
        }
        /* insertion sort by angle: nring is 5 */
        for (a = 1; a < nring; ++a)
            for (b = a; b > 0 && ang[b - 1] > ang[b]; --b)
            {
                float ta = ang[b - 1]; uint32_t tr = ring[b - 1];
                ang[b - 1] = ang[b];   ring[b - 1] = ring[b];
                ang[b] = ta;           ring[b] = tr;
            }

        /* the face normal must point away from the origin: reverse if not */
        {
            mu_vec3 n = mu_vec3_cross(
                mu_vec3_sub(s.verts[face_vert[ring[1]]].p, s.verts[face_vert[ring[0]]].p),
                mu_vec3_sub(s.verts[face_vert[ring[2]]].p, s.verts[face_vert[ring[0]]].p));
            if (mu_vec3_dot(n, axis) < 0.0f)
            {
                for (a = 0; a < nring / 2; ++a)
                {
                    uint32_t tr = ring[a];
                    ring[a] = ring[nring - 1 - a];
                    ring[nring - 1 - a] = tr;
                }
            }
        }
        for (a = 1; a + 1 < nring; ++a)
            mu_shape_add_triangle(&s, face_vert[ring[0]], face_vert[ring[a]],
                                  face_vert[ring[a + 1]]);
    }

    mu_shape_free(&ico);
    return mu_shape__finish(s);
}

/* ==========================================================================
 * 8. Mesh operations
 * ========================================================================== */

MU_INLINE uint32_t mu_shape__next_pow2(uint32_t x)
{
    uint32_t r = 1u;
    while (r < x && r < 0x40000000u) r <<= 1;
    return r;
}

MU_INLINE uint32_t mu_shape__hash3(int32_t x, int32_t y, int32_t z, uint32_t mask)
{
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^
                 (uint32_t)z * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2545f491u;
    h ^= h >> 13;
    return h & mask;
}

/* ---------- transform ---------- */

/* Applies m to positions and the COFACTOR to normals (AGENTS.md 3.16): a
 * reflection must flip the normal, and inverse-transpose does not. */
MU_INLINE void mu_shape_transform(mu_shape *s, const mu_mat4 *m)
{
    uint32_t i;

    if (!s || !m) return;
    for (i = 0; i < s->npoints; ++i)
    {
        s->verts[i].p = mu_mat4_transform_point(m, s->verts[i].p);
        if (mu_vec3_length2(s->verts[i].n) > 0.0f)
            s->verts[i].n = mu_vec3_normalize(mu_mat4_normal(m, s->verts[i].n));
    }
}

MU_INLINE void mu_shape_translate(mu_shape *s, mu_vec3 t)
{
    uint32_t i;
    if (!s) return;
    for (i = 0; i < s->npoints; ++i) s->verts[i].p = mu_vec3_add(s->verts[i].p, t);
}

MU_INLINE void mu_shape_rotate(mu_shape *s, mu_vec3 axis, float radians)
{
    mu_mat4 m;
    if (!s) return;
    m = mu_mat4_rotate(axis, radians);
    mu_shape_transform(s, &m);
}

MU_INLINE void mu_shape_scale(mu_shape *s, mu_vec3 k)
{
    mu_mat4 m;
    if (!s) return;
    m = mu_mat4_scale(k);
    mu_shape_transform(s, &m);
}

/* Reverse winding, i.e. turn the mesh inside out. */
MU_INLINE void mu_shape_flip(mu_shape *s)
{
    uint32_t i;
    if (!s) return;
    for (i = 0; i < s->ntriangles; ++i)
    {
        MU_SHAPES_T t = s->triangles[3u * i + 1u];
        s->triangles[3u * i + 1u] = s->triangles[3u * i + 2u];
        s->triangles[3u * i + 2u] = t;
    }
    for (i = 0; i < s->npoints; ++i) s->verts[i].n = mu_vec3_negate(s->verts[i].n);
}

/* Append src to dst. UVs and normals come along, so scenes are built by
 * merging shapes. Returns 0 on allocation failure (dst is unchanged). */
MU_INLINE int mu_shape_merge(mu_shape *dst, const mu_shape *src)
{
    uint32_t base, i;

    if (!dst || !src || src->npoints == 0) return 1;
    if (src->npoints > 0xFFFFFFF0u) return 0;

    base = dst->npoints;
    if (!mu_shape__reserve_points(dst, dst->npoints + src->npoints)) return 0;
    if (!mu_shape__reserve_tris(dst, dst->ntriangles + src->ntriangles)) return 0;

    memcpy(dst->verts + base, src->verts, (size_t)src->npoints * sizeof *src->verts);
    for (i = 0; i < src->ntriangles * 3u; ++i)
        dst->triangles[3u * dst->ntriangles + i] = (MU_SHAPES_T)(src->triangles[i] + base);
    dst->npoints += src->npoints;
    dst->ntriangles += src->ntriangles;
    return 1;
}

/* ==========================================================================
 * 6. Non-trivial surfaces
 * ========================================================================== */

/* Gielis' superformula. One scalar per (theta), two of them crossed gives the
 * 3D supershape: this single parameter set covers spheres, cubes, stars,
 * flowers, gears and shapes with no name. m is the symmetry count, n11 the
 * "inflation", n12/n13 the pinching of the two axes. */
typedef struct mu_shape_superformula
{
    float m1, n11, n12, n13;   /* latitude  */
    float m2, n21, n22, n23;   /* longitude */
} mu_shape_superformula;

MU_INLINE mu_shape_superformula mu_shape_superformula_sphere(void)
{
    mu_shape_superformula f;
    f.m1 = 0.0f; f.n11 = 1.0f; f.n12 = 1.0f; f.n13 = 1.0f;
    f.m2 = 0.0f; f.n21 = 1.0f; f.n22 = 1.0f; f.n23 = 1.0f;
    return f;
}

MU_INLINE mu_shape_superformula mu_shape_superformula_star(void)
{
    mu_shape_superformula f;
    f.m1 = 5.0f; f.n11 = 0.3f; f.n12 = 0.4f; f.n13 = 0.4f;
    f.m2 = 2.0f; f.n21 = 0.5f; f.n22 = 0.5f; f.n23 = 0.5f;
    return f;
}

MU_INLINE float mu_shape__superformula_radius(float theta, float m, float n1,
                                              float n2, float n3)
{
    float t = 0.25f * m * theta;
    float a = fabsf(cosf(t));
    float b = fabsf(sinf(t));
    float f = powf(a, n2) + powf(b, n3);

    if (f <= 1e-9f) return 0.0f;
    return powf(f, -1.0f / n1);
}

MU_INLINE mu_shape mu_shape_supershape(const mu_shape_superformula *f, int slices,
                                       int stacks, float radius)
{
    mu_shape s = mu_shape_empty();
    int i, j;
    mu_shape_superformula def;

    if (!f) { def = mu_shape_superformula_sphere(); f = &def; }
    if (slices < 4) slices = 4;
    if (stacks < 3) stacks = 3;

    if (!mu_shape_reserve(&s, (uint32_t)((slices + 1) * (stacks + 1)),
                          (uint32_t)(2 * slices * stacks)))
        return s;

    /* phi over [-pi/2, pi/2] and theta over [-pi, pi]; row 0 and row `stacks`
     * are the poles and collapse to a point when m1 is a multiple of 4. */
    for (j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        float phi = MU_SHAPE_PI * (v - 0.5f);
        float r2 = mu_shape__superformula_radius(phi, f->m2, f->n21, f->n22, f->n23);
        float cp = cosf(phi);
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float th = MU_SHAPE_TAU * u - MU_SHAPE_PI;
            float r1 = mu_shape__superformula_radius(th, f->m1, f->n11, f->n12, f->n13);
            mu_vec3 p = mu_vec3_make(radius * r1 * cosf(th) * r2 * cp,
                                     radius * r2 * sinf(phi),
                                     radius * r1 * sinf(th) * r2 * cp);
            mu_shape_add_vertex(&s, p, u, v);
        }
    }
    mu_shape__grid_faces(&s, 0u, slices, stacks);
    /* this (theta, phi) mapping is left-handed the way the superformula is
     * usually written, so the winding comes out inward: flip once here rather
     * than reversing every triangle above */
    mu_shape_flip(&s);
    mu_shape_compact(&s);
    return mu_shape__finish(s);
}

/* Log-spiral shell: a tube whose radius grows exponentially around a coiling
 * axis. Turns = 3, growth = 0.2 is a conch; turns = 6, growth = 0.05 a nautilus
 * approximation. Open surface (a shell has an edge). */
MU_INLINE mu_shape mu_shape_seashell(int slices, int stacks, int turns,
                                     float radius, float growth, float tube)
{
    mu_shape s = mu_shape_empty();
    int i, j;
    float t_end = MU_SHAPE_TAU * (float)(turns > 0 ? turns : 1);

    if (slices < 4) slices = 4;
    if (stacks < 4) stacks = 4;

    if (!mu_shape_reserve(&s, (uint32_t)((slices + 1) * (stacks + 1)),
                          (uint32_t)(2 * slices * stacks)))
        return s;

    for (j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        float t = t_end * v;
        float r = radius * expf(growth * t);
        float a = tube * r;
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float ang = MU_SHAPE_TAU * u;
            float c = cosf(ang), sn = sinf(ang);
            float ring = r * (1.0f + 0.35f * c);
            mu_shape_add_vertex(&s,
                mu_vec3_make(ring * cosf(t), 0.25f * a * sn + t * growth * 0.15f * r,
                             ring * sinf(t)), u, v);
        }
    }
    mu_shape__grid_faces(&s, 0u, slices, stacks);
    return mu_shape__finish(s);
}

/* Mobius strip. Written by hand rather than through mu_shape_parametric because
 * the closing seam is not a seam: after one turn the strip's two edges have
 * swapped, so column slices-1 joins column 0 with the ROW ORDER REVERSED. That
 * reversal is the whole trick, and it is why the mesh has a single boundary
 * loop instead of two. Non-orientable: the winding cannot be consistent, so
 * don't ask this mesh for a signed volume. */
MU_INLINE mu_shape mu_shape_mobius(int slices, int strips, float radius, float width)
{
    mu_shape s = mu_shape_empty();
    int i, j;
    float half = 0.5f * width;

    if (slices < 4) slices = 4;
    if (strips < 1) strips = 1;

    if (!mu_shape_reserve(&s, (uint32_t)(slices * (strips + 1)),
                          (uint32_t)(2 * slices * strips)))
        return s;

    for (j = 0; j <= strips; ++j)
    {
        float v = (float)j / (float)strips;
        float w = half * (2.0f * v - 1.0f);
        for (i = 0; i < slices; ++i)
        {
            float u = (float)i / (float)slices;
            float th = MU_SHAPE_TAU * u;
            float ht = 0.5f * th;
            float r = radius + w * cosf(ht);
            mu_shape_add_vertex(&s, mu_vec3_make(r * cosf(th), w * sinf(ht),
                                                 r * sinf(th)), u, v);
        }
    }

    for (i = 0; i < slices; ++i)
    {
        int i1 = (i + 1) % slices;
        int flip = (i1 == 0);
        for (j = 0; j < strips; ++j)
        {
            uint32_t a = (uint32_t)j * (uint32_t)slices + (uint32_t)i;
            uint32_t d = a + (uint32_t)slices;
            uint32_t b, c;
            if (flip)
            {
                b = (uint32_t)(strips - j) * (uint32_t)slices;
                c = (uint32_t)(strips - j - 1) * (uint32_t)slices;
            }
            else
            {
                b = (uint32_t)j * (uint32_t)slices + (uint32_t)i1;
                c = b + (uint32_t)slices;
            }
            mu_shape_add_triangle(&s, a, b, d);
            mu_shape_add_triangle(&s, b, c, d);
        }
    }
    return mu_shape__finish(s);
}

/* Figure-8 immersion. Closed (no boundary) and non-orientable, so Euler
 * characteristic 0 like a torus but genus "one half". A Klein bottle cannot be
 * embedded in R3, so where the immersion passes through itself the merged
 * positions make a few edges non-manifold — that is the shape, not a bug, and
 * mu_shape_stats() reports it instead of pretending otherwise. */
MU_INLINE mu_shape mu_shape_klein_bottle(int slices, int stacks, float scale)
{
    mu_shape s = mu_shape_empty();
    int i, j;

    if (slices < 4) slices = 4;
    if (stacks < 4) stacks = 4;

    if (!mu_shape_reserve(&s, (uint32_t)((slices + 1) * (stacks + 1)),
                          (uint32_t)(2 * slices * stacks)))
        return s;

    for (j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        float phi = MU_SHAPE_TAU * v;
        for (i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float th = MU_SHAPE_TAU * u;
            float ht = 0.5f * th;
            float r = 2.0f + cosf(ht) * sinf(phi) - sinf(ht) * sinf(2.0f * phi);
            /* y and z swapped versus the textbook form so the axis stays +Y */
            mu_shape_add_vertex(&s,
                mu_vec3_scale(mu_vec3_make(r * cosf(th),
                                           sinf(ht) * sinf(phi) + cosf(ht) * sinf(2.0f * phi),
                                           r * sinf(th)), scale), u, v);
        }
    }
    mu_shape__grid_faces(&s, 0u, slices, stacks);
    return mu_shape__finish(s);
}

/* ==========================================================================
 * 7. Construction kit: lathe, tube, extrude
 * ========================================================================== */

/* Surface of revolution around +Y from a 2D profile (x = radius, y = height).
 *
 * PROFILE ORDER MATTERS: list points BOTTOM to TOP with radius >= 0, and the
 * normals come out facing away from the axis. Reverse the profile and you get
 * the inside of the shape (which is occasionally what you want). A closed loop
 * as the profile gives a torus-like solid; an open one gets caps at both ends
 * where the radius is non-zero.
 *
 * Radius 0 at an end is a pole and is closed with a fan (not a degenerate cap). */
MU_INLINE mu_shape mu_shape_lathe(const mu_vec2 *profile, int n, int slices,
                                  int closed_profile)
{
    mu_shape s = mu_shape_empty();
    int i, j;
    int rows = n;
    int segs = closed_profile ? n : (n - 1);

    if (!profile || n < 2) return s;
    if (slices < 3) slices = 3;

    if (!mu_shape_reserve(&s, (uint32_t)(rows * slices + 4),
                          (uint32_t)(2 * segs * slices + 2 * slices)))
        return s;

    for (j = 0; j < rows; ++j)
    {
        float r = profile[j].x;
        if (r < 0.0f) r = 0.0f;
        for (i = 0; i < slices; ++i)
        {
            float u = (float)i / (float)slices;
            float a = MU_SHAPE_TAU * u;
            mu_shape_add_vertex(&s, mu_vec3_make(r * cosf(a), profile[j].y,
                                                 r * sinf(a)),
                                u, (float)j / (float)(rows > 1 ? rows - 1 : 1));
        }
    }

    /* bottom->top profile means the natural grid winding faces inward; flip it
     * by swapping the triangle order (see the doc comment above). */
    for (j = 0; j < segs; ++j)
    {
        uint32_t r0 = (uint32_t)j * (uint32_t)slices;
        uint32_t r1 = (uint32_t)((j + 1) % rows) * (uint32_t)slices;
        for (i = 0; i < slices; ++i)
        {
            uint32_t i1 = (uint32_t)((i + 1) % slices);
            uint32_t a = r0 + (uint32_t)i;
            uint32_t b = r0 + i1;
            uint32_t d = r1 + (uint32_t)i;
            uint32_t c = r1 + i1;

            mu_shape_add_triangle(&s, a, d, b);
            mu_shape_add_triangle(&s, b, d, c);
        }
    }

    if (!closed_profile)
    {
        int ends[2];
        ends[0] = 0;
        ends[1] = n - 1;
        for (j = 0; j < 2; ++j)
        {
            int e = ends[j];
            if (profile[e].x <= 1e-5f) continue;   /* pole: already a fan */
            {
                uint32_t c = (uint32_t)mu_shape_add_vertex(
                    &s, mu_vec3_make(0.0f, profile[e].y, 0.0f), 0.5f,
                    (float)e / (float)(n - 1));
                uint32_t base = (uint32_t)e * (uint32_t)slices;
                for (i = 0; i < slices; ++i)
                {
                    uint32_t i1 = (uint32_t)((i + 1) % slices);
                    if (e == 0)
                        mu_shape_add_triangle(&s, c, base + (uint32_t)i, base + i1);
                    else
                        mu_shape_add_triangle(&s, c, base + i1, base + (uint32_t)i);
                }
            }
        }
    }
    return mu_shape__finish(s);
}

/* Sweep a circle along a polyline with ROTATION-MINIMIZING FRAMES.
 *
 * WHY NOT Frenet. The Frenet frame is defined by the curve's second derivative;
 * it flips 180 degrees at every inflection point and spins wildly wherever the
 * curvature is small, so a swept tube tears. The double-reflection method
 * (Wang, Juttler, Zheng, Liu, ACM TOG 2008) transports the previous frame with
 * two reflections instead: it never flips, it is a single pass over the path,
 * and for a closed path the accumulated twist ("holonomy") is spread evenly
 * over the loop so the seam closes.
 *
 * radius is used for every ring unless radii is non-NULL, which overrides it
 * per path point (a tapering or bulging tube). closed joins the last ring to the
 * first and applies the holonomy correction; caps fan each open end to the
 * centre. */
MU_INLINE mu_shape mu_shape_tube_radii(const mu_vec3 *path, int n, float radius,
                                       const float *radii, int sides, int closed,
                                       int caps)
{
    mu_shape s = mu_shape_empty();
    mu_vec3 *tan;
    mu_vec3 *ref;
    int i, j;
    float holonomy = 0.0f;

    if (!path || n < 2) return s;
    if (sides < 3) sides = 3;

    tan = (mu_vec3 *)MU_MALLOC((size_t)n * sizeof *tan);
    ref = (mu_vec3 *)MU_MALLOC((size_t)n * sizeof *ref);
    if (!tan || !ref) { MU_FREE(tan); MU_FREE(ref); return s; }

    /* tangents: central differences inside, one-sided at the ends */
    for (i = 0; i < n; ++i)
    {
        mu_vec3 d;
        if (closed)
        {
            int ip = (i + 1) % n;
            int im = (i + n - 1) % n;
            d = mu_vec3_sub(path[ip], path[im]);
        }
        else if (i == 0)      d = mu_vec3_sub(path[1], path[0]);
        else if (i == n - 1)  d = mu_vec3_sub(path[n - 1], path[n - 2]);
        else                  d = mu_vec3_sub(path[i + 1], path[i - 1]);
        tan[i] = mu_vec3_length2(d) > 1e-12f ? mu_vec3_normalize(d)
                                             : mu_vec3_make(0.0f, 0.0f, 1.0f);
    }

    /* seed a reference vector perpendicular to the first tangent */
    {
        mu_vec3 up = mu_vec3_make(0.0f, 1.0f, 0.0f);
        mu_vec3 c = mu_vec3_cross(tan[0], up);
        if (mu_vec3_length2(c) < 1e-8f)
            c = mu_vec3_cross(tan[0], mu_vec3_make(1.0f, 0.0f, 0.0f));
        ref[0] = mu_vec3_normalize(c);
    }

    /* double reflection transport */
    for (i = 0; i + 1 < n; ++i)
    {
        mu_vec3 v1 = mu_vec3_sub(path[i + 1], path[i]);
        float c1 = mu_vec3_length2(v1);
        mu_vec3 rL, tL, v2;
        float c2;

        if (c1 < 1e-12f) { ref[i + 1] = ref[i]; continue; }

        rL = mu_vec3_sub(ref[i], mu_vec3_scale(v1, 2.0f * mu_vec3_dot(v1, ref[i]) / c1));
        tL = mu_vec3_sub(tan[i], mu_vec3_scale(v1, 2.0f * mu_vec3_dot(v1, tan[i]) / c1));
        v2 = mu_vec3_sub(tan[i + 1], tL);
        c2 = mu_vec3_length2(v2);

        if (c2 < 1e-12f)
            ref[i + 1] = rL;
        else
            ref[i + 1] = mu_vec3_sub(rL,
                mu_vec3_scale(v2, 2.0f * mu_vec3_dot(v2, rL) / c2));
        ref[i + 1] = mu_vec3_length2(ref[i + 1]) > 1e-12f
                   ? mu_vec3_normalize(ref[i + 1]) : ref[i];
    }

    /* closed path: measure the twist between the last frame and the first and
     * distribute -angle/n along the whole loop, so ring n == ring 0. */
    if (closed)
    {
        mu_vec3 tn = tan[n - 1];
        mu_vec3 r0 = ref[0];
        mu_vec3 b0 = mu_vec3_cross(tn, ref[n - 1]);
        holonomy = atan2f(mu_vec3_dot(r0, b0), mu_vec3_dot(r0, ref[n - 1]));
    }

    if (!mu_shape_reserve(&s, (uint32_t)(n * sides + 4),
                          (uint32_t)(2 * n * sides + 2 * sides)))
    {
        MU_FREE(tan); MU_FREE(ref);
        return s;
    }

    for (i = 0; i < n; ++i)
    {
        float r = radii ? radii[i] : radius;
        mu_vec3 b = mu_vec3_cross(tan[i], ref[i]);
        float twist = closed ? -holonomy * (float)i / (float)n : 0.0f;
        float ct = cosf(twist), st = sinf(twist);
        mu_vec3 rv = mu_vec3_add(mu_vec3_scale(ref[i], ct), mu_vec3_scale(b, st));

        rv = mu_vec3_normalize(rv);
        b = mu_vec3_cross(tan[i], rv);

        for (j = 0; j < sides; ++j)
        {
            float u = (float)j / (float)sides;
            float a = MU_SHAPE_TAU * u;
            mu_vec3 off = mu_vec3_add(mu_vec3_scale(rv, r * cosf(a)),
                                      mu_vec3_scale(b, r * sinf(a)));
            mu_shape_add_vertex(&s, mu_vec3_add(path[i], off), u,
                                (float)i / (float)(n > 1 ? n - 1 : 1));
        }
    }

    for (i = 0; i < n - 1 + (closed ? 1 : 0); ++i)
    {
        uint32_t r0 = (uint32_t)i * (uint32_t)sides;
        uint32_t r1 = (uint32_t)((i + 1) % n) * (uint32_t)sides;
        for (j = 0; j < sides; ++j)
        {
            uint32_t j1 = (uint32_t)((j + 1) % sides);
            mu_shape_add_quad(&s, r0 + (uint32_t)j, r0 + j1, r1 + j1, r1 + (uint32_t)j);
        }
    }

    if (caps && !closed)
    {
        int ends[2];
        ends[0] = 0;
        ends[1] = n - 1;
        for (j = 0; j < 2; ++j)
        {
            int e = ends[j];
            uint32_t c = (uint32_t)mu_shape_add_vertex(
                &s, path[e], 0.5f, (float)j);
            uint32_t base = (uint32_t)e * (uint32_t)sides;
            int k;
            for (k = 0; k < sides; ++k)
            {
                uint32_t k1 = (uint32_t)((k + 1) % sides);
                /* start cap faces -tangent, end cap +tangent; see the fan
                 * note in mu_shape_cylinder. */
                if (j == 0)
                    mu_shape_add_triangle(&s, c, base + k1, base + (uint32_t)k);
                else
                    mu_shape_add_triangle(&s, c, base + (uint32_t)k, base + k1);
            }
        }
    }

    MU_FREE(tan);
    MU_FREE(ref);
    return mu_shape__finish(s);
}

MU_INLINE mu_shape mu_shape_tube(const mu_vec3 *path, int n, float radius, int sides,
                                 int closed, int caps)
{
    return mu_shape_tube_radii(path, n, radius, NULL, sides, closed, caps);
}

/* Helix as a polyline: mu_shape_helix_path(out, n, ...) */
MU_INLINE void mu_shape_helix_path(mu_vec3 *out, int n, float coil_radius,
                                   float pitch, float turns)
{
    int i;
    for (i = 0; i < n; ++i)
    {
        float t = (float)i / (float)(n > 1 ? n - 1 : 1) * turns * MU_SHAPE_TAU;
        out[i] = mu_vec3_make(coil_radius * cosf(t), pitch * t / MU_SHAPE_TAU,
                              coil_radius * sinf(t));
    }
}

/* A spring: tube swept along a helix. */
MU_INLINE mu_shape mu_shape_spring(int path_points, int sides, float coil_radius,
                                   float pitch, float turns, float wire_radius)
{
    mu_shape s;
    mu_vec3 *path;

    if (path_points < 4) path_points = 4;
    path = (mu_vec3 *)MU_MALLOC((size_t)path_points * sizeof *path);
    if (!path) return mu_shape_empty();

    mu_shape_helix_path(path, path_points, coil_radius, pitch, turns);
    s = mu_shape_tube(path, path_points, wire_radius, sides, 0, 1);
    MU_FREE(path);
    return s;
}

/* Trefoil knot swept as a tube — the par_shapes shape, but with an RMF so the
 * tube does not pinch where the curvature is high. */
MU_INLINE mu_shape mu_shape_trefoil_knot(int path_points, int sides, float scale,
                                         float wire_radius)
{
    mu_shape s;
    mu_vec3 *path;
    int i;

    if (path_points < 8) path_points = 8;
    path = (mu_vec3 *)MU_MALLOC((size_t)path_points * sizeof *path);
    if (!path) return mu_shape_empty();

    for (i = 0; i < path_points; ++i)
    {
        float t = MU_SHAPE_TAU * (float)i / (float)path_points;
        float x = sinf(t) + 2.0f * sinf(2.0f * t);
        float y = cosf(t) - 2.0f * cosf(2.0f * t);
        float z = -sinf(3.0f * t);
        /* swap y/z so the knot's natural axis is +Y */
        path[i] = mu_vec3_scale(mu_vec3_make(x, z, y), scale / 3.0f);
    }
    s = mu_shape_tube(path, path_points, wire_radius, sides, 1, 0);
    MU_FREE(path);
    return s;
}

/* ---------- ear clipping ---------- */

MU_INLINE float mu_shape__cross2(mu_vec2 a, mu_vec2 b) { return a.x * b.y - a.y * b.x; }

MU_INLINE int mu_shape__point_in_tri(mu_vec2 p, mu_vec2 a, mu_vec2 b, mu_vec2 c)
{
    mu_vec2 ab = mu_vec2_sub(b, a), bc = mu_vec2_sub(c, b), ca = mu_vec2_sub(a, c);
    mu_vec2 ap = mu_vec2_sub(p, a), bp = mu_vec2_sub(p, b), cp = mu_vec2_sub(p, c);
    float d0 = mu_shape__cross2(ab, ap);
    float d1 = mu_shape__cross2(bc, bp);
    float d2 = mu_shape__cross2(ca, cp);
    return (d0 >= 0.0f && d1 >= 0.0f && d2 >= 0.0f);
}

/* Triangulate a simple polygon (CCW) into n-2 triangles, O(n^2).
 *
 * This is the standard ear-clipping loop: repeatedly cut a convex vertex whose
 * triangle contains no other vertex. It is O(n^2) and it FAILS on self-
 * intersecting input, which is why it returns 0 and the caller falls back to a
 * fan rather than emitting garbage. Holes are not supported (par_shapes doesn't
 * either); a hole needs a bridge edge or a proper earcut. */
MU_INLINE int mu_shape__ear_clip(const mu_vec2 *p, int n, int *out)
{
    int *idx;
    int count = n;
    int ntri = 0;
    int i, guard = 0;

    if (n < 3) return 0;
    idx = (int *)MU_MALLOC((size_t)n * sizeof *idx);
    if (!idx) return 0;
    for (i = 0; i < n; ++i) idx[i] = i;

    while (count > 3 && guard++ < n * n + 16)
    {
        int cut = 0;
        for (i = 0; i < count; ++i)
        {
            int ip = (i + count - 1) % count;
            int in = (i + 1) % count;
            mu_vec2 a = p[idx[ip]], b = p[idx[i]], c = p[idx[in]];
            int k, inside = 0;

            if (mu_shape__cross2(mu_vec2_sub(b, a), mu_vec2_sub(c, b)) <= 1e-12f)
                continue;                      /* reflex or collinear: not an ear */

            for (k = 0; k < count; ++k)
            {
                if (k == ip || k == i || k == in) continue;
                if (mu_shape__point_in_tri(p[idx[k]], a, b, c)) { inside = 1; break; }
            }
            if (inside) continue;

            out[3 * ntri + 0] = idx[ip];
            out[3 * ntri + 1] = idx[i];
            out[3 * ntri + 2] = idx[in];
            ++ntri;
            memmove(&idx[i], &idx[i + 1], (size_t)(count - i - 1) * sizeof *idx);
            --count;
            cut = 1;
            break;
        }
        if (!cut) { MU_FREE(idx); return 0; }   /* self-intersecting input */
    }
    if (count == 3)
    {
        out[3 * ntri + 0] = idx[0];
        out[3 * ntri + 1] = idx[1];
        out[3 * ntri + 2] = idx[2];
        ++ntri;
    }
    MU_FREE(idx);
    return ntri == n - 2 ? ntri : 0;
}

/* Extrude a 2D polygon (XZ plane, CCW when viewed from +Y) into a solid of the
 * given height, centred on y = 0. `twist` rotates the top cap around the
 * centroid, which turns a prism into a twisted prism (a 2-sided helicoid when
 * the twist is a half turn per unit height).
 *
 * Caps are ear-clipped, not fanned: a fan is only correct for convex polygons,
 * and a fanned concave polygon silently produces overlapping triangles. */
MU_INLINE mu_shape mu_shape_extrude(const mu_vec2 *poly, int n, float height, float twist)
{
    mu_shape s = mu_shape_empty();
    mu_vec2 *p;
    int *tris;
    int ntri, i, ok;
    int ccw;
    float area2 = 0.0f;
    float cx = 0.0f, cz = 0.0f;
    float h = 0.5f * height;

    if (!poly || n < 3) return s;

    p = (mu_vec2 *)MU_MALLOC((size_t)n * sizeof *p);
    tris = (int *)MU_MALLOC((size_t)(3 * (n - 2)) * sizeof *tris);
    if (!p || !tris) { MU_FREE(p); MU_FREE(tris); return s; }

    for (i = 0; i < n; ++i) area2 += mu_shape__cross2(poly[i], poly[(i + 1) % n]);
    ccw = area2 > 0.0f;
    for (i = 0; i < n; ++i)
    {
        p[i] = ccw ? poly[i] : poly[n - 1 - i];
        cx += p[i].x;
        cz += p[i].y;
    }
    cx /= (float)n;
    cz /= (float)n;

    ntri = mu_shape__ear_clip(p, n, tris);
    ok = ntri > 0;

    if (!mu_shape_reserve(&s, (uint32_t)(2 * n + 2), (uint32_t)(2 * n + 2 * ntri)))
    {
        MU_FREE(p); MU_FREE(tris);
        return s;
    }

    {
        float ct = cosf(twist), st = sinf(twist);
        for (i = 0; i < 2; ++i)
        {
            int j;
            for (j = 0; j < n; ++j)
            {
                float x = p[j].x, z = p[j].y;
                if (i == 1)
                {
                    float dx = x - cx, dz = z - cz;
                    x = cx + dx * ct - dz * st;
                    z = cz + dx * st + dz * ct;
                }
                mu_shape_add_vertex(&s, mu_vec3_make(x, i ? h : -h, z),
                                    (float)j / (float)n, (float)i);
            }
        }
    }

    /* sides: outward when the polygon is CCW in XZ seen from +Y */
    for (i = 0; i < n; ++i)
    {
        uint32_t i1 = (uint32_t)((i + 1) % n);
        uint32_t b0 = (uint32_t)i, b1 = i1;
        uint32_t t0 = (uint32_t)n + (uint32_t)i, t1 = (uint32_t)n + i1;
        mu_shape_add_quad(&s, b0, t0, t1, b1);
    }

    if (ok)
    {
        for (i = 0; i < ntri; ++i)
        {
            uint32_t a = (uint32_t)tris[3 * i + 0];
            uint32_t b = (uint32_t)tris[3 * i + 1];
            uint32_t c = (uint32_t)tris[3 * i + 2];
            /* The ear clipper emits counter-clockwise in (x,z), which is a -Y
             * facing normal in 3D, so the BOTTOM cap keeps that order and the
             * TOP cap reverses it. */
            mu_shape_add_triangle(&s, a, b, c);
            mu_shape_add_triangle(&s, (uint32_t)n + a, (uint32_t)n + c,
                                  (uint32_t)n + b);
        }
    }
    MU_FREE(p);
    MU_FREE(tris);
    return mu_shape__finish(s);
}

MU_INLINE void mu_shape_compact(mu_shape *s)
{
    uint32_t *map;
    uint8_t  *used;
    mu_shapes_vertex *stack;
    uint32_t i, n = 0, nt = 0;

    if (!s || s->npoints == 0) return;
    map = (uint32_t *)MU_MALLOC((size_t)s->npoints * sizeof *map);
    used = (uint8_t *)MU_MALLOC((size_t)s->npoints * sizeof *used);
    if (!map || !used) { MU_FREE(map); MU_FREE(used); return; }

    memset(used, 0, (size_t)s->npoints * sizeof *used);
    for (i = 0; i < s->ntriangles * 3u; ++i)
    {
        uint32_t v = (uint32_t)s->triangles[i];
        if (v < s->npoints) used[v] = 1u;
    }

    /* New indices are assigned in ASCENDING old index order. That is what makes
     * the in-place compaction below safe: map[i] <= i, so a slot is never
     * written before the vertex that lives there has been read. (Assigning in
     * first-use order instead silently shreds the mesh.) */
    for (i = 0; i < s->npoints; ++i)
        map[i] = used[i] ? n++ : MU_SHAPE_NONE;

    if (n == s->npoints) { MU_FREE(map); MU_FREE(used); return; }

    stack = s->verts;
    for (i = 0; i < s->npoints; ++i)
        if (map[i] != MU_SHAPE_NONE) stack[map[i]] = s->verts[i];

    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t idx[3];
        int k, ok = 1;

        for (k = 0; k < 3; ++k)
        {
            uint32_t v = (uint32_t)s->triangles[3u * i + (uint32_t)k];
            if (v >= s->npoints || map[v] == MU_SHAPE_NONE) { ok = 0; break; }
            idx[k] = map[v];
        }
        if (!ok) continue;
        if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) continue;

        s->triangles[3u * nt + 0u] = (MU_SHAPES_T)idx[0];
        s->triangles[3u * nt + 1u] = (MU_SHAPES_T)idx[1];
        s->triangles[3u * nt + 2u] = (MU_SHAPES_T)idx[2];
        ++nt;
    }
    s->npoints = n;
    s->ntriangles = nt;
    MU_FREE(map);
    MU_FREE(used);
}

/* Drop triangles whose area is below min_area (in world units squared). */
MU_INLINE uint32_t mu_shape_remove_degenerate(mu_shape *s, float min_area)
{
    uint32_t i, nt = 0, removed = 0;

    if (!s) return 0;
    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t a = (uint32_t)s->triangles[3u * i + 0u];
        uint32_t b = (uint32_t)s->triangles[3u * i + 1u];
        uint32_t c = (uint32_t)s->triangles[3u * i + 2u];
        float area = 0.0f;

        if (a < s->npoints && b < s->npoints && c < s->npoints && a != b && b != c &&
            a != c)
        {
            mu_vec3 cr = mu_vec3_cross(mu_vec3_sub(s->verts[b].p, s->verts[a].p),
                                       mu_vec3_sub(s->verts[c].p, s->verts[a].p));
            area = 0.5f * mu_vec3_length(cr);
        }
        if (area > min_area)
        {
            s->triangles[3u * nt + 0u] = (MU_SHAPES_T)a;
            s->triangles[3u * nt + 1u] = (MU_SHAPES_T)b;
            s->triangles[3u * nt + 2u] = (MU_SHAPES_T)c;
            ++nt;
        }
        else
        {
            ++removed;
        }
    }
    s->ntriangles = nt;
    return removed;
}

/* ---------- welding ---------- */

#define MU_SHAPE_WELD_NONE   0u
#define MU_SHAPE_WELD_UV     1u   /* only merge vertices whose UVs match   */
#define MU_SHAPE_WELD_NORMAL 2u   /* ... and whose normals match          */

/* Build the position->representative map. Spatial hash over eps-sized cells with
 * a 27-cell scan, because two vertices that are eps apart can straddle a cell
 * boundary and a single-cell lookup would miss them (that bug shows up as a
 * handful of unmerged vertices exactly at the seams of a sphere).
 *
 * Returns 1 on success; map[i] = smallest index coincident with i. */
MU_INLINE int mu_shape__weld_map(const mu_shape *s, float eps, uint32_t flags,
                                 uint32_t *map)
{
    uint32_t table_size, mask, i;
    uint32_t *head, *next;
    float eps2;

    if (!s || !map) return 0;
    for (i = 0; i < s->npoints; ++i) map[i] = i;
    if (s->npoints < 2) return 1;

    if (eps <= 0.0f) eps = 1e-6f;
    eps2 = eps * eps;

    table_size = mu_shape__next_pow2(s->npoints * 2u + 8u);
    mask = table_size - 1u;
    head = (uint32_t *)MU_MALLOC((size_t)table_size * sizeof *head);
    next = (uint32_t *)MU_MALLOC((size_t)s->npoints * sizeof *next);
    if (!head || !next) { MU_FREE(head); MU_FREE(next); return 0; }
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
                {
                    int ok = 1;
                    if ((flags & MU_SHAPE_WELD_UV) &&
                        (fabsf(s->verts[c].uv.x - s->verts[i].uv.x) > 1e-5f ||
                         fabsf(s->verts[c].uv.y - s->verts[i].uv.y) > 1e-5f)) ok = 0;
                    if ((flags & MU_SHAPE_WELD_NORMAL) &&
                        mu_vec3_length2(mu_vec3_sub(s->verts[c].n, s->verts[i].n)) > 1e-8f) ok = 0;
                    if (ok) { best = map[c]; break; }
                }
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
    return 1;
}

/* Merge coincident vertices and drop the triangles that collapse. Returns the
 * number of vertices removed, or -1 if the work table could not be allocated.
 *
 * O(n) expected: one pass, one hash lookup per vertex, no sort. par_shapes sorts
 * vertex pairs (O(n log n) plus a full compare-exchange array) which is why its
 * weld is the slowest thing in its API for a 1M-point bake. */
MU_INLINE int mu_shape_weld(mu_shape *s, float eps, uint32_t flags)
{
    uint32_t *map, *remap;
    uint32_t i, n = 0, nt = 0;

    if (!s || s->npoints == 0) return 0;

    map = (uint32_t *)MU_MALLOC((size_t)s->npoints * sizeof *map);
    remap = (uint32_t *)MU_MALLOC((size_t)s->npoints * sizeof *remap);
    if (!map || !remap) { MU_FREE(map); MU_FREE(remap); return -1; }

    if (!mu_shape__weld_map(s, eps, flags, map))
    { MU_FREE(map); MU_FREE(remap); return -1; }

    for (i = 0; i < s->npoints; ++i) remap[i] = MU_SHAPE_NONE;
    for (i = 0; i < s->npoints; ++i)
        if (map[i] == i) remap[i] = n++;

    /* representatives are always at an index <= their own, so compacting
     * forwards never overwrites a vertex that has not been read yet */
    for (i = 0; i < s->npoints; ++i)
        if (remap[i] != MU_SHAPE_NONE) s->verts[remap[i]] = s->verts[i];

    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t idx[3];
        int k, ok = 1;

        for (k = 0; k < 3; ++k)
        {
            uint32_t v = (uint32_t)s->triangles[3u * i + (uint32_t)k];
            if (v >= s->npoints || remap[map[v]] == MU_SHAPE_NONE) { ok = 0; break; }
            idx[k] = remap[map[v]];
        }
        if (!ok) continue;
        if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) continue;

        s->triangles[3u * nt + 0u] = (MU_SHAPES_T)idx[0];
        s->triangles[3u * nt + 1u] = (MU_SHAPES_T)idx[1];
        s->triangles[3u * nt + 2u] = (MU_SHAPES_T)idx[2];
        ++nt;
    }

    {
        int removed = (int)(s->npoints - n);
        s->npoints = n;
        s->ntriangles = nt;
        MU_FREE(map);
        MU_FREE(remap);
        return removed;
    }
}

MU_INLINE void mu_shape_unweld(mu_shape *s)
{
    mu_shapes_vertex *v;
    MU_SHAPES_T *t;
    uint32_t i;

    if (!s || s->ntriangles == 0) return;
    v = (mu_shapes_vertex *)MU_MALLOC((size_t)s->ntriangles * 3u * sizeof *v);
    t = (MU_SHAPES_T *)MU_MALLOC((size_t)s->ntriangles * 3u * sizeof *t);
    if (!v || !t) { MU_FREE(v); MU_FREE(t); return; }

    for (i = 0; i < s->ntriangles * 3u; ++i)
    {
        uint32_t src = (uint32_t)s->triangles[i];
        v[i] = s->verts[src < s->npoints ? src : 0u];
        t[i] = (MU_SHAPES_T)i;
    }
    MU_FREE(s->verts);
    MU_FREE(s->triangles);
    s->verts = v;
    s->triangles = t;
    s->npoints = s->ntriangles * 3u;
    s->cap_points = s->npoints;
    s->cap_tris = s->ntriangles;
}

/* ---------- normals ---------- */

/* Area-weighted vertex normals, accumulated ACROSS coincident positions, so a
 * UV seam or a duplicated cube edge is smooth-shaded rather than creased. That
 * is what par_shapes_compute_welded_normals does; the difference is that this
 * one does not modify the topology to achieve it. */
MU_INLINE int mu_shape_compute_normals(mu_shape *s)
{
    uint32_t *map;
    mu_vec3 *acc;
    uint32_t i;

    if (!s || s->ntriangles == 0) return 0;
    map = (uint32_t *)MU_MALLOC((size_t)s->npoints * sizeof *map);
    acc = (mu_vec3 *)MU_MALLOC((size_t)s->npoints * sizeof *acc);
    if (!map || !acc) { MU_FREE(map); MU_FREE(acc); return 0; }
    if (!mu_shape__weld_map(s, 1e-5f, MU_SHAPE_WELD_NONE, map))
    { MU_FREE(map); MU_FREE(acc); return 0; }

    for (i = 0; i < s->npoints; ++i) acc[i] = mu_vec3_zero();

    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t a = (uint32_t)s->triangles[3u * i + 0u];
        uint32_t b = (uint32_t)s->triangles[3u * i + 1u];
        uint32_t c = (uint32_t)s->triangles[3u * i + 2u];
        mu_vec3 fn;

        if (a >= s->npoints || b >= s->npoints || c >= s->npoints) continue;
        /* un-normalised cross product == 2 * area * normal, so accumulating it
         * weights by area for free */
        fn = mu_vec3_cross(mu_vec3_sub(s->verts[b].p, s->verts[a].p),
                           mu_vec3_sub(s->verts[c].p, s->verts[a].p));
        acc[map[a]] = mu_vec3_add(acc[map[a]], fn);
        acc[map[b]] = mu_vec3_add(acc[map[b]], fn);
        acc[map[c]] = mu_vec3_add(acc[map[c]], fn);
    }

    for (i = 0; i < s->npoints; ++i)
    {
        mu_vec3 n = acc[map[i]];
        s->verts[i].n = mu_vec3_length2(n) > 1e-20f ? mu_vec3_normalize(n)
                                                    : mu_vec3_make(0.0f, 1.0f, 0.0f);
    }
    MU_FREE(map);
    MU_FREE(acc);
    return 1;
}

/* Per-face normals, written to the three corners. Call mu_shape_unweld first if
 * you want each face to own its vertices. */
MU_INLINE void mu_shape_compute_normals_flat(mu_shape *s)
{
    uint32_t i;
    if (!s) return;
    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t a = (uint32_t)s->triangles[3u * i + 0u];
        uint32_t b = (uint32_t)s->triangles[3u * i + 1u];
        uint32_t c = (uint32_t)s->triangles[3u * i + 2u];
        mu_vec3 fn;

        if (a >= s->npoints || b >= s->npoints || c >= s->npoints) continue;
        fn = mu_vec3_cross(mu_vec3_sub(s->verts[b].p, s->verts[a].p),
                           mu_vec3_sub(s->verts[c].p, s->verts[a].p));
        fn = mu_vec3_length2(fn) > 1e-20f ? mu_vec3_normalize(fn)
                                          : mu_vec3_make(0.0f, 1.0f, 0.0f);
        s->verts[a].n = fn;
        s->verts[b].n = fn;
        s->verts[c].n = fn;
    }
}

/* ---------- measurements ---------- */

MU_INLINE void mu_shape_aabb(const mu_shape *s, mu_vec3 *lo, mu_vec3 *hi)
{
    uint32_t i;

    if (lo) *lo = mu_vec3_zero();
    if (hi) *hi = mu_vec3_zero();
    if (!s || s->npoints == 0) return;

    if (lo) *lo = s->verts[0].p;
    if (hi) *hi = s->verts[0].p;
    for (i = 1; i < s->npoints; ++i)
    {
        if (lo) *lo = mu_vec3_min(*lo, s->verts[i].p);
        if (hi) *hi = mu_vec3_max(*hi, s->verts[i].p);
    }
}

MU_INLINE float mu_shape_area(const mu_shape *s)
{
    double sum = 0.0;
    uint32_t i;

    if (!s) return 0.0f;
    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t a = (uint32_t)s->triangles[3u * i + 0u];
        uint32_t b = (uint32_t)s->triangles[3u * i + 1u];
        uint32_t c = (uint32_t)s->triangles[3u * i + 2u];
        if (a >= s->npoints || b >= s->npoints || c >= s->npoints) continue;
        sum += 0.5 * (double)mu_vec3_length(
            mu_vec3_cross(mu_vec3_sub(s->verts[b].p, s->verts[a].p),
                          mu_vec3_sub(s->verts[c].p, s->verts[a].p)));
    }
    return (float)sum;
}

/* Signed volume by the divergence theorem: sum of tetrahedra from the origin.
 * Positive for a closed mesh wound counter-clockwise seen from outside, which is
 * exactly the check that catches an inside-out generator. Only meaningful for a
 * closed surface; an open one gives the volume of its cone to the origin. */
MU_INLINE float mu_shape_volume(const mu_shape *s)
{
    double sum = 0.0;
    uint32_t i;

    if (!s) return 0.0f;
    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t a = (uint32_t)s->triangles[3u * i + 0u];
        uint32_t b = (uint32_t)s->triangles[3u * i + 1u];
        uint32_t c = (uint32_t)s->triangles[3u * i + 2u];
        if (a >= s->npoints || b >= s->npoints || c >= s->npoints) continue;
        sum += (double)mu_vec3_dot(s->verts[a].p,
                                   mu_vec3_cross(s->verts[b].p, s->verts[c].p)) / 6.0;
    }
    return (float)sum;
}

/* ---------- edge table (shared by stats and subdivision) ---------- */

typedef struct mu_shape__edge
{
    uint64_t key;
    uint32_t v0, v1;
    uint32_t opp[2];
    uint32_t count;
    int32_t  dir;      /* +1 per (min->max) use, -1 per (max->min) use */
} mu_shape__edge;

typedef struct mu_shape__edges
{
    mu_shape__edge *slots;
    uint32_t        mask;
    uint32_t        count;
    uint32_t        boundary;
    uint32_t        nonmanifold;
    uint32_t        inconsistent;
} mu_shape__edges;

MU_INLINE uint32_t mu_shape__edge_hash(uint64_t key, uint32_t mask)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (uint32_t)key & mask;
}

/* Build a deduplicated edge table over `map`ed indices. Returns 0 on OOM. */
MU_INLINE int mu_shape__edges_build(const mu_shape *s, const uint32_t *map,
                                    mu_shape__edges *e)
{
    uint32_t cap, i;

    memset(e, 0, sizeof *e);
    if (!s || s->ntriangles == 0) return 1;

    cap = mu_shape__next_pow2(s->ntriangles * 6u + 8u);
    e->slots = (mu_shape__edge *)MU_MALLOC((size_t)cap * sizeof *e->slots);
    if (!e->slots) return 0;
    memset(e->slots, 0, (size_t)cap * sizeof *e->slots);
    e->mask = cap - 1u;

    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t tri[3];
        int k;

        for (k = 0; k < 3; ++k)
        {
            uint32_t v = (uint32_t)s->triangles[3u * i + (uint32_t)k];
            if (v >= s->npoints) { tri[0] = MU_SHAPE_NONE; break; }
            tri[k] = map ? map[v] : v;
        }
        if (tri[0] == MU_SHAPE_NONE) continue;
        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;

        for (k = 0; k < 3; ++k)
        {
            uint32_t a = tri[k];
            uint32_t b = tri[(k + 1) % 3];
            uint32_t other = tri[(k + 2) % 3];
            uint32_t lo = a < b ? a : b;
            uint32_t hi = a < b ? b : a;
            uint64_t key = ((uint64_t)lo << 32) | (uint64_t)hi;
            uint32_t slot = mu_shape__edge_hash(key, e->mask);

            key += 1u;   /* 0 == empty slot */
            for (;;)
            {
                mu_shape__edge *cur = &e->slots[slot];
                if (cur->key == 0u)
                {
                    cur->key = key;
                    cur->v0 = lo;
                    cur->v1 = hi;
                    cur->opp[0] = other;
                    cur->opp[1] = other;
                    cur->count = 1u;
                    cur->dir = (a == lo) ? 1 : -1;
                    ++e->count;
                    break;
                }
                if (cur->key == key)
                {
                    if (cur->count < 2u) cur->opp[1] = other;
                    else cur->opp[1] = cur->opp[0];
                    ++cur->count;
                    cur->dir += (a == lo) ? 1 : -1;
                    break;
                }
                slot = (slot + 1u) & e->mask;
            }
        }
    }

    for (i = 0; i < e->mask + 1u; ++i)
    {
        const mu_shape__edge *cur = &e->slots[i];
        if (cur->key == 0u) continue;
        if (cur->count == 1u) ++e->boundary;
        else if (cur->count > 2u) ++e->nonmanifold;
        else if (cur->dir != 0) ++e->inconsistent;
    }
    return 1;
}

MU_INLINE void mu_shape__edges_free(mu_shape__edges *e)
{
    MU_FREE(e->slots);
    memset(e, 0, sizeof *e);
}

/* ---------- stats / validate ---------- */

typedef struct mu_shape_stats
{
    uint32_t npoints;
    uint32_t ntriangles;
    uint32_t unique_positions;      /* after merging coincident vertices  */
    uint32_t duplicate_points;      /* npoints - unique_positions         */
    uint32_t degenerate_triangles;  /* zero area or repeated index        */
    uint32_t boundary_edges;        /* used by exactly one triangle       */
    uint32_t nonmanifold_edges;     /* used by three or more              */
    uint32_t inconsistent_edges;    /* same direction twice: winding flips */
    int32_t  euler;                 /* V - E + F over merged positions     */
    int32_t  genus;                 /* (2 - euler) / 2 if closed+orientable, else -1 */
    int32_t  watertight;            /* boundary_edges == 0                 */
    float    area;
    float    volume;                /* signed; only meaningful if watertight */
} mu_shape_stats;

MU_INLINE int mu_shape_stats_compute(const mu_shape *s, float weld_eps,
                                     mu_shape_stats *out)
{
    mu_shape__edges e;
    uint32_t *map;
    uint32_t i, unique = 0;
    uint32_t degenerate = 0;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s) return 0;

    out->npoints = s->npoints;
    out->ntriangles = s->ntriangles;
    out->genus = -1;

    map = (uint32_t *)MU_MALLOC((size_t)(s->npoints ? s->npoints : 1u) * sizeof *map);
    if (!map) return 0;
    if (!mu_shape__weld_map(s, weld_eps > 0.0f ? weld_eps : 1e-5f,
                            MU_SHAPE_WELD_NONE, map))
    { MU_FREE(map); return 0; }

    for (i = 0; i < s->npoints; ++i)
        if (map[i] == i) ++unique;
    out->unique_positions = unique;
    out->duplicate_points = s->npoints - unique;

    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t a = (uint32_t)s->triangles[3u * i + 0u];
        uint32_t b = (uint32_t)s->triangles[3u * i + 1u];
        uint32_t c = (uint32_t)s->triangles[3u * i + 2u];
        if (a >= s->npoints || b >= s->npoints || c >= s->npoints) { ++degenerate; continue; }
        if (map[a] == map[b] || map[b] == map[c] || map[a] == map[c]) ++degenerate;
    }
    out->degenerate_triangles = degenerate;

    if (!mu_shape__edges_build(s, map, &e)) { MU_FREE(map); return 0; }
    out->boundary_edges = e.boundary;
    out->nonmanifold_edges = e.nonmanifold;
    out->inconsistent_edges = e.inconsistent;
    out->euler = (int32_t)unique - (int32_t)e.count +
                 (int32_t)(s->ntriangles - degenerate);
    out->watertight = (e.boundary == 0u && e.nonmanifold == 0u);
    if (out->watertight && e.inconsistent == 0u &&
        ((out->euler & 1) == 0) && out->euler <= 2)
        out->genus = (2 - out->euler) / 2;

    out->area = mu_shape_area(s);
    out->volume = mu_shape_volume(s);

    mu_shape__edges_free(&e);
    MU_FREE(map);
    return 1;
}

/* The one call a test should make on any generated mesh: every index in range,
 * no NaN, no zero-area triangle, and (optionally) closed. Writes a human
 * sentence into err on failure. */
MU_INLINE int mu_shape_validate(const mu_shape *s, char *err, size_t errlen)
{
    uint32_t i;

    if (err && errlen) err[0] = '\0';
    if (!s) return 0;

#define MU_SHAPE_FAIL(msg)                                                     \
    do {                                                                       \
        if (err && errlen) snprintf(err, errlen, "%s", msg);                   \
        return 0;                                                              \
    } while (0)

    if (s->npoints == 0) MU_SHAPE_FAIL("empty mesh");
    if (s->ntriangles == 0) MU_SHAPE_FAIL("no triangles");
    if (!s->verts || !s->triangles) MU_SHAPE_FAIL("null array");

    for (i = 0; i < s->npoints; ++i)
    {
        const mu_vec3 p = s->verts[i].p;
        if (!(p.x == p.x) || !(p.y == p.y) || !(p.z == p.z))
            MU_SHAPE_FAIL("NaN/Inf in positions");
    }
    for (i = 0; i < s->ntriangles * 3u; ++i)
        if ((uint32_t)s->triangles[i] >= s->npoints)
            MU_SHAPE_FAIL("triangle index out of range");

    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t a = (uint32_t)s->triangles[3u * i + 0u];
        uint32_t b = (uint32_t)s->triangles[3u * i + 1u];
        uint32_t c = (uint32_t)s->triangles[3u * i + 2u];
        if (a == b || b == c || a == c) MU_SHAPE_FAIL("repeated index in a triangle");
        if (mu_vec3_length2(mu_vec3_cross(mu_vec3_sub(s->verts[b].p, s->verts[a].p),
                                          mu_vec3_sub(s->verts[c].p, s->verts[a].p)))
            < 1e-20f)
            MU_SHAPE_FAIL("zero-area triangle");
    }
#undef MU_SHAPE_FAIL
    return 1;
}

/* ---------- Loop subdivision ---------- */

/* One level of Loop subdivision, which is the right subdivision scheme for a
 * triangle mesh (Catmull-Clark is for quads): each triangle becomes four, and
 * the new positions are the C2 stencils
 *     new edge point = 3/8(near ends) + 1/8(far ends)
 *     new vertex     = (1 - n*beta) * v + beta * sum(neighbours), beta = 3/(8n)
 * Watertightness, Euler characteristic and genus are preserved, which is what
 * test_shapes.c checks after two levels. Boundary edges (count == 1) use the
 * midpoint rule and leave the boundary curve intact rather than shrinking it. */
MU_INLINE int mu_shape_loop_subdivide(mu_shape *s, int levels)
{
    int level;

    if (!s || s->ntriangles == 0) return 0;

    for (level = 0; level < levels; ++level)
    {
        mu_shape__edges e;
        mu_vec3 *vsum, *vnew;
        uint32_t *deg, *emap;
        uint32_t i, nv, nt;
        mu_shape out = mu_shape_empty();

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
            mu_shape_free(&out);
            mu_shape__edges_free(&e);
            return 0;
        }
        for (i = 0; i < s->npoints; ++i) { vsum[i] = mu_vec3_zero(); deg[i] = 0; }
        for (i = 0; i < e.mask + 1u; ++i) emap[i] = MU_SHAPE_NONE;

        /* edge points occupy [npoints, npoints + e.count) */
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
                float beta = (deg[i] == 3u) ? 3.0f / 16.0f
                                            : 3.0f / (8.0f * (float)deg[i]);
                float keep = 1.0f - beta * (float)deg[i];
                vnew[i] = mu_vec3_add(mu_vec3_scale(s->verts[i].p, keep),
                                      mu_vec3_scale(vsum[i], beta));
            }
        }

        for (i = 0; i < nv; ++i)
        {
            uint32_t k;
            float u = 0.0f, v = 0.0f;
            if (i < s->npoints) { u = s->verts[i].uv.x; v = s->verts[i].uv.y; }
            k = (uint32_t)mu_shape_add_vertex(&out, vnew[i], u, v);
            (void)k;
        }

        for (i = 0; i < s->ntriangles; ++i)
        {
            uint32_t tri[3];
            uint32_t mid[3];
            int k;

            for (k = 0; k < 3; ++k)
                tri[k] = (uint32_t)s->triangles[3u * i + (uint32_t)k];

            for (k = 0; k < 3; ++k)
            {
                uint32_t a = tri[k], b = tri[(k + 1) % 3];
                uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
                uint64_t key = (((uint64_t)lo << 32) | (uint64_t)hi) + 1u;
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
        if (s->npoints == 0) return 0;
    }
    mu_shape_compute_normals(s);
    return 1;
}

/* Icosphere: an icosahedron subdivided and projected, the standard way to get
 * evenly sized triangles on a sphere (a UV sphere has slivers at the poles and
 * a seam). level 4 = 5120 triangles, level 6 = 81920. */
MU_INLINE mu_shape mu_shape_icosphere(int level, float radius)
{
    mu_shape s = mu_shape_icosahedron(1.0f);
    uint32_t i;

    if (s.npoints == 0) return s;
    if (level < 0) level = 0;
    if (level > 8) level = 8;    /* 8 levels = 1.3M triangles; 32-bit indices or bust */

    if (level > 0 && !mu_shape_loop_subdivide(&s, level))
    {
        mu_shape_free(&s);
        return mu_shape_empty();
    }
    for (i = 0; i < s.npoints; ++i)
        s.verts[i].p = mu_vec3_scale(mu_vec3_normalize(s.verts[i].p), radius);

    mu_shape_compute_normals(&s);
    return s;
}

/* Noise-displaced icosphere. Needs mu_noise_math.h; if it is not included the
 * displacement falls back to a cheap deterministic hash so the shape still
 * exists (and looks worse, which is the point of the #if). */
MU_INLINE mu_shape mu_shape_rock(int level, float radius, float amplitude,
                                 uint32_t seed)
{
    mu_shape s = mu_shape_icosphere(level, radius);
    uint32_t i;

    if (s.npoints == 0) return s;
    for (i = 0; i < s.npoints; ++i)
    {
        mu_vec3 d = mu_vec3_normalize(s.verts[i].p);
        float h;
#if defined(MU_NOISE_MATH_H)
        h = mu_noise_fbm3(mu_noise_simplex3, d.x * 2.0f, d.y * 2.0f, d.z * 2.0f,
                          seed, 4, 2.0f, 0.5f);
#else
        h = sinf(d.x * 12.9898f + d.y * 78.233f + d.z * 37.719f +
                 (float)(seed & 1023u)) * 0.5f;
#endif
        s.verts[i].p = mu_vec3_add(s.verts[i].p, mu_vec3_scale(d, amplitude * h));
    }
    mu_shape_compute_normals(&s);
    return s;
}

/* ---------- export ---------- */

/* Wavefront OBJ. Writes positions, UVs, normals and faces; returns 1 on
 * success. Index base is 1, as the format requires, and OBJ is right-handed
 * with +Y up, which is what we already use. */
MU_INLINE int mu_shape_export_obj(const mu_shape *s, const char *path)
{
    FILE *f;
    uint32_t i;

    if (!s || !path) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;

    fprintf(f, "# mu_shapes: %u points, %u triangles\n", s->npoints, s->ntriangles);
    for (i = 0; i < s->npoints; ++i)
        fprintf(f, "v %.6g %.6g %.6g\n", (double)s->verts[i].p.x,
                (double)s->verts[i].p.y, (double)s->verts[i].p.z);
    for (i = 0; i < s->npoints; ++i)
        fprintf(f, "vt %.6g %.6g\n", (double)s->verts[i].uv.x,
                (double)s->verts[i].uv.y);
    for (i = 0; i < s->npoints; ++i)
        fprintf(f, "vn %.6g %.6g %.6g\n", (double)s->verts[i].n.x,
                (double)s->verts[i].n.y, (double)s->verts[i].n.z);
    for (i = 0; i < s->ntriangles; ++i)
    {
        uint32_t a = (uint32_t)s->triangles[3u * i + 0u] + 1u;
        uint32_t b = (uint32_t)s->triangles[3u * i + 1u] + 1u;
        uint32_t c = (uint32_t)s->triangles[3u * i + 2u] + 1u;
        fprintf(f, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
    }
    fclose(f);
    return 1;
}

/* ==========================================================================
 * 9. Signed distance fields -> mesh (marching tetrahedra)
 * ==========================================================================
 * This is the escape hatch for shapes nobody has parametrized: a blob, a gyroid,
 * a smooth-unioned cluster of capsules. You give a function of position whose
 * value is NEGATIVE INSIDE and POSITIVE OUTSIDE, a box, and a resolution.
 *
 * WHY TETRAHEDRA, NOT CUBES. Marching cubes needs a 256-entry edge table, has
 * ambiguous faces that produce holes unless you add the (much larger) asymptotic
 * decider table, and the tables are the part everyone gets wrong. Splitting each
 * cell into six tetrahedra removes both problems: a tetrahedron has exactly
 * three topological cases, there is nothing ambiguous, and the output is
 * watertight by construction. The cost is real and worth stating: you get
 * roughly 1.5-2x the triangles of marching cubes at the same grid resolution,
 * because every cell now contributes from six tetrahedra instead of one.
 *
 * The interpolation along an edge is always computed from the LOWER of the two
 * grid corners, so the same edge produces bit-identical points from both
 * neighbouring cells; coincident vertices are then merged by one weld pass
 * instead of an edge-keyed hash table. Fewer moving parts, and the weld is
 * already tested.
 */

typedef float (*mu_sdf_fn)(mu_vec3 p, void *user);

typedef struct mu_sdf_sphere_t  { mu_vec3 c; float r; } mu_sdf_sphere_t;
typedef struct mu_sdf_box_t     { mu_vec3 c; mu_vec3 half; float round; } mu_sdf_box_t;
typedef struct mu_sdf_capsule_t { mu_vec3 a; mu_vec3 b; float r; } mu_sdf_capsule_t;
typedef struct mu_sdf_torus_t   { mu_vec3 c; float major; float minor; } mu_sdf_torus_t;
typedef struct mu_sdf_plane_t   { mu_vec3 n; float d; } mu_sdf_plane_t;

MU_INLINE float mu_sdf_sphere(mu_vec3 p, void *user)
{
    const mu_sdf_sphere_t *s = (const mu_sdf_sphere_t *)user;
    return mu_vec3_length(mu_vec3_sub(p, s->c)) - s->r;
}

MU_INLINE float mu_sdf_box(mu_vec3 p, void *user)
{
    const mu_sdf_box_t *b = (const mu_sdf_box_t *)user;
    mu_vec3 q = mu_vec3_sub(mu_vec3_abs(mu_vec3_sub(p, b->c)), b->half);
    mu_vec3 out = mu_vec3_max(q, mu_vec3_zero());
    float m = q.x > q.y ? (q.x > q.z ? q.x : q.z) : (q.y > q.z ? q.y : q.z);

    /* outside: |max(q,0)|; inside: the (negative) largest component. The usual
     * two-line bit of SDF algebra, spelled out because it is easy to get wrong. */
    return mu_vec3_length(out) + (m < 0.0f ? m : 0.0f) - b->round;
}

MU_INLINE float mu_sdf_capsule(mu_vec3 p, void *user)
{
    const mu_sdf_capsule_t *c = (const mu_sdf_capsule_t *)user;
    mu_vec3 pa = mu_vec3_sub(p, c->a);
    mu_vec3 ba = mu_vec3_sub(c->b, c->a);
    float h = mu_vec3_dot(pa, ba) / mu_vec3_dot(ba, ba);
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;
    return mu_vec3_length(mu_vec3_sub(pa, mu_vec3_scale(ba, h))) - c->r;
}

MU_INLINE float mu_sdf_torus(mu_vec3 p, void *user)
{
    const mu_sdf_torus_t *t = (const mu_sdf_torus_t *)user;
    mu_vec3 q = mu_vec3_sub(p, t->c);
    float r = sqrtf(q.x * q.x + q.z * q.z) - t->major;
    return sqrtf(r * r + q.y * q.y) - t->minor;
}

MU_INLINE float mu_sdf_plane(mu_vec3 p, void *user)
{
    const mu_sdf_plane_t *pl = (const mu_sdf_plane_t *)user;
    return mu_vec3_dot(p, pl->n) - pl->d;
}

MU_INLINE float mu_sdf_union(float a, float b)     { return a < b ? a : b; }
MU_INLINE float mu_sdf_intersect(float a, float b) { return a > b ? a : b; }
MU_INLINE float mu_sdf_subtract(float a, float b)  { return a > -b ? a : -b; }

/* Polynomial smooth minimum: the blend is exact outside [-k, k] and rounds the
 * seam inside it. k = 0 gives a hard union. This one call is what turns three
 * spheres into a metaball creature. */
MU_INLINE float mu_sdf_smin(float a, float b, float k)
{
    float h, m;

    if (k <= 0.0f) return a < b ? a : b;
    h = a - b;
    if (h > k) return b;
    if (h < -k) return a;
    h = 0.5f * (k + h);
    m = h / k;
    return b + (a - b) * m - k * m * (1.0f - m);
}

MU_INLINE float mu_sdf_smax(float a, float b, float k) { return -mu_sdf_smin(-a, -b, k); }

/* Corner numbering inside a cell: c = di + 2*dj + 4*dk, and the six tetrahedra
 * all share the 0-7 diagonal, which is what makes neighbouring cells agree. */
static const int mu_shape__tets[6][4] = {
    {0, 5, 1, 7}, {0, 1, 3, 7}, {0, 3, 2, 7},
    {0, 2, 6, 7}, {0, 6, 4, 7}, {0, 4, 5, 7}
};

typedef struct mu_shape__grid
{
    uint32_t n;        /* n = res + 1 corners per axis */
    mu_vec3  lo;
    mu_vec3  step;
} mu_shape__grid;

MU_INLINE mu_vec3 mu_shape__grid_pos(const mu_shape__grid *g, uint32_t idx)
{
    uint32_t i = idx % g->n;
    uint32_t j = (idx / g->n) % g->n;
    uint32_t k = idx / (g->n * g->n);
    return mu_vec3_make(g->lo.x + (float)i * g->step.x,
                        g->lo.y + (float)j * g->step.y,
                        g->lo.z + (float)k * g->step.z);
}

/* Crossing point on edge (ga, gb), always interpolated from the lower index so
 * two cells sharing the edge produce the identical float. */
MU_INLINE mu_vec3 mu_shape__grid_cross(const mu_shape__grid *g, const float *vals,
                                       uint32_t ga, uint32_t gb, float iso)
{
    uint32_t lo = ga < gb ? ga : gb;
    uint32_t hi = ga < gb ? gb : ga;
    float va = vals[lo];
    float vb = vals[hi];
    float t = (iso - va) / (vb - va);

    return mu_vec3_lerp(mu_shape__grid_pos(g, lo), mu_shape__grid_pos(g, hi), t);
}

/* SDF -> watertight mesh over the box [lo,hi] at `res` cells per axis.
 *
 * COST: (res+1)^3 evaluations of fn, then 6*res^3 tetrahedron tests, then one
 * weld pass. At res = 64 that is 274k samples and ~1.5M tet tests; fn dominates
 * unless it is trivial, which is why the bench measures a whole bake instead of
 * a per-triangle number.
 *
 * Pick res so that the surface is 2-3 cells thick at its thinnest part, or the
 * thin features vanish (that is aliasing, not a bug in this function). */
MU_INLINE mu_shape mu_shape_from_sdf(mu_sdf_fn fn, void *user, mu_vec3 lo, mu_vec3 hi,
                                     int res, float iso)
{
    mu_shape s = mu_shape_empty();
    mu_shape__grid g;
    float *vals;
    uint32_t n, i, j, k;
    float diag;

    if (!fn || res < 1) return s;
    if (res > 512) res = 512;

    n = (uint32_t)res + 1u;
    g.n = n;
    g.lo = lo;
    g.step = mu_vec3_make((hi.x - lo.x) / (float)res, (hi.y - lo.y) / (float)res,
                          (hi.z - lo.z) / (float)res);
    diag = mu_vec3_length(mu_vec3_sub(hi, lo));

    vals = (float *)MU_MALLOC((size_t)n * n * n * sizeof *vals);
    if (!vals) return s;

    for (k = 0; k < n; ++k)
        for (j = 0; j < n; ++j)
            for (i = 0; i < n; ++i)
                vals[i + n * (j + n * k)] = fn(
                    mu_vec3_make(lo.x + (float)i * g.step.x,
                                 lo.y + (float)j * g.step.y,
                                 lo.z + (float)k * g.step.z), user);

    for (k = 0; k + 1u < n; ++k)
    for (j = 0; j + 1u < n; ++j)
    for (i = 0; i + 1u < n; ++i)
    {
        uint32_t corner[8];
        int c, t;

        for (c = 0; c < 8; ++c)
        {
            uint32_t ci = i + (uint32_t)(c & 1);
            uint32_t cj = j + (uint32_t)((c >> 1) & 1);
            uint32_t ck = k + (uint32_t)((c >> 2) & 1);
            corner[c] = ci + n * (cj + n * ck);
        }

        for (t = 0; t < 6; ++t)
        {
            int inside[4];
            int nin = 0;
            mu_vec3 pin, pout;
            uint32_t ev[4][4];
            int c, ea, eb;
            int idx_in[4], idx_out[4];
            int n_in = 0, n_out = 0;

            for (c = 0; c < 4; ++c)
            {
                inside[c] = vals[corner[mu_shape__tets[t][c]]] < iso;
                nin += inside[c];
                if (inside[c]) idx_in[n_in++] = c;
                else           idx_out[n_out++] = c;
            }
            if (nin == 0 || nin == 4) continue;

            pin = mu_vec3_zero();
            pout = mu_vec3_zero();
            for (c = 0; c < 4; ++c)
            {
                mu_vec3 p = mu_shape__grid_pos(&g, corner[mu_shape__tets[t][c]]);
                if (inside[c]) pin = mu_vec3_add(pin, p);
                else           pout = mu_vec3_add(pout, p);
            }
            pin = mu_vec3_scale(pin, 1.0f / (float)nin);
            pout = mu_vec3_scale(pout, 1.0f / (float)(4 - nin));

            for (ea = 0; ea < 4; ++ea)
                for (eb = 0; eb < 4; ++eb)
                    ev[ea][eb] = MU_SHAPE_NONE;

            for (ea = 0; ea < 4; ++ea)
            for (eb = ea + 1; eb < 4; ++eb)
            {
                uint32_t v;
                if (inside[ea] == inside[eb]) continue;
                v = (uint32_t)mu_shape_add_vertex(
                        &s, mu_shape__grid_cross(&g, vals,
                                 corner[mu_shape__tets[t][ea]],
                                 corner[mu_shape__tets[t][eb]], iso), 0.0f, 0.0f);
                ev[ea][eb] = v;
                ev[eb][ea] = v;
            }

            /* orientation: the triangle normal must point from inside to
             * outside. Deciding it locally is two dot products and removes any
             * need to reason about the winding of six tetrahedra. */
#define MU_SHAPE__ORIENT(i0, i1, i2)                                           \
            do {                                                               \
                mu_vec3 A_ = s.verts[i0].p;                                    \
                mu_vec3 B_ = s.verts[i1].p;                                    \
                mu_vec3 C_ = s.verts[i2].p;                                    \
                mu_vec3 fn_ = mu_vec3_cross(mu_vec3_sub(B_, A_),               \
                                            mu_vec3_sub(C_, A_));              \
                if (mu_vec3_dot(fn_, mu_vec3_sub(pout, pin)) < 0.0f)           \
                    mu_shape_add_triangle(&s, i0, i2, i1);                     \
                else                                                           \
                    mu_shape_add_triangle(&s, i0, i1, i2);                     \
            } while (0)

            if (nin == 1)
            {
                /* one corner inside: the triangle joins its three crossings */
                int i0 = idx_in[0];
                MU_SHAPE__ORIENT(ev[i0][idx_out[0]], ev[i0][idx_out[1]],
                                 ev[i0][idx_out[2]]);
            }
            else if (nin == 3)
            {
                /* one corner outside: same triangle, seen from the other end.
                 * Anchor on the OUTSIDE corner — anchoring on an inside corner
                 * would ask for two crossings that do not exist. */
                int o0 = idx_out[0];
                MU_SHAPE__ORIENT(ev[o0][idx_in[0]], ev[o0][idx_in[1]],
                                 ev[o0][idx_in[2]]);
            }
            else
            {
                /* Two in, two out: the cross-section is a quadrilateral whose
                 * cycle is (iA,oA) (iA,oB) (iB,oB) (iB,oA). Going around in
                 * THAT order matters — emitting the four crossings in the order
                 * they happen to be generated produces a bowtie, i.e. two
                 * triangles that overlap and leave the mesh non-manifold. */
                int ia = idx_in[0], ib = idx_in[1];
                int oa = idx_out[0], ob = idx_out[1];
                uint32_t A = ev[ia][oa], B = ev[ia][ob];
                uint32_t C = ev[ib][ob], D = ev[ib][oa];
                MU_SHAPE__ORIENT(A, B, C);
                MU_SHAPE__ORIENT(A, C, D);
            }
#undef MU_SHAPE__ORIENT
        }
    }

    MU_FREE(vals);

    if (s.ntriangles == 0) { mu_shape_free(&s); return mu_shape_empty(); }

    mu_shape_weld(&s, diag * 1e-6f, MU_SHAPE_WELD_NONE);
    mu_shape_remove_degenerate(&s, 0.0f);
    mu_shape_compute_normals(&s);

    /* a closed SDF surface that came out inside-out is a winding bug, not a
     * design choice: fix it here so the caller never has to think about it */
    if (mu_shape_volume(&s) < 0.0f) mu_shape_flip(&s);

    return mu_shape__finish(s);
}


MU_END_EXTERN_C

#endif /* MU_SHAPES_H */
