#ifndef MU_GEOMETRIC_ALGEBRA_H
#define MU_GEOMETRIC_ALGEBRA_H

/*
    mu_geometric_algebra.h
    ------------------------------------------------------------
    Standalone single-header C99 geometric algebra toolkit.

    Design target:
    - Rigid 2D PGA and rigid 3D PGA primitives (flat points, lines, planes)
    - 2D / 3D motors (dual-quaternion style) and flectors (reflection operators)
    - Conformal 2D and 3D primitives (round points, dipoles, circles, spheres)
    - Join (wedge) and meet (antiwedge) constructions used in Terathon math

    This header is intentionally verbose and heavily commented so that the math
    is visible near the code.

    --------------------------------------------------------------------------
    VISUAL MAP (types and basis layout)
    --------------------------------------------------------------------------

    Rigid 2D (PGA-like)

        FlatPoint2: p = x e1 + y e2 + z e3
        Line2:      g = x e23 + y e31 + z e12

        Join: p ^ q -> line
        Meet: g ^ h -> flat point

    Rigid 3D (PGA-like)

        FlatPoint3: p = x e1 + y e2 + z e3 + w e4
        Line3:      l = v_x e41 + v_y e42 + v_z e43 + m_x e23 + m_y e31 + m_z e12
        Plane3:     g = x e234 + y e314 + z e124 + w e321

        Join: p ^ q -> line, line ^ p -> plane
        Meet: g ^ h -> line, g ^ line -> point

    Conformal 2D

        RoundPoint2: (x,y,z,w)
        Dipole2:     line g + flat point p
        Circle2:     (w,x,y,z)

    Conformal 3D

        RoundPoint3: (x,y,z,w,u)
        Dipole3:     (v,m,p)
        Circle3:     (g,v,m)
        Sphere3:     (u,x,y,z,w)

    --------------------------------------------------------------------------
    CGLM NOTE
    --------------------------------------------------------------------------
    This file is standalone in the sense that it is a single header, but it
    intentionally uses cglm for low-level vector math (dot/cross/norm/add/sub)
    to reduce local boilerplate and keep hot-path primitives centralized in one
    math backend.
*/

#include <math.h>
#include <float.h>
#include <stdbool.h>

#include "mu_macros.h"

/*
    cglm exposes vec3s/vec4s members (.x, .y, .z) through anonymous structs.
    Under strict C99 cglm disables them by default, so ask for them before it
    is included; without this the header fails to compile in C99 mode.
*/
#if !defined(CGLM_USE_ANONYMOUS_STRUCT)
#  define CGLM_USE_ANONYMOUS_STRUCT 1
#elif CGLM_USE_ANONYMOUS_STRUCT == 0
#  error "mu_geometric_algebra.h needs cglm anonymous structs: define CGLM_USE_ANONYMOUS_STRUCT=1"
#endif

#if defined(MU_GA_CGLM_HEADER)
#include MU_GA_CGLM_HEADER
#elif defined(__has_include)
#if __has_include(<cglm/cglm.h>)
#include <cglm/cglm.h>
#include <cglm/struct.h>
#elif __has_include("../external/cglm/include/cglm/cglm.h")
#include "../external/cglm/include/cglm/cglm.h"
#include "../external/cglm/include/cglm/struct.h"
#elif __has_include("../../external/cglm/include/cglm/cglm.h")
#include "../../external/cglm/include/cglm/cglm.h"
#include "../../external/cglm/include/cglm/struct.h"
#else
#error "mu_geometric_algebra.h requires cglm. Define MU_GA_CGLM_HEADER to your cglm umbrella header path."
#endif
#else
#error "mu_geometric_algebra.h requires cglm. Compiler lacks __has_include; define MU_GA_CGLM_HEADER first."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* basic scalar helpers */
/* ------------------------------------------------------------------------- */

MU_INLINE float mu_ga_sqrt(float x) { return sqrtf(x); }
MU_INLINE float mu_ga_inv_sqrt(float x) { return 1.0f / sqrtf(x); }
MU_INLINE float mu_ga_abs(float x) { return fabsf(x); }
MU_INLINE float mu_ga_clamp_eps(float x) { return (mu_ga_abs(x) < 1.0e-8f) ? 0.0f : x; }

/* Constructors keep vector literals valid in both C99 and C++. */
MU_INLINE vec2s mu_ga_vec2(float x, float y)
{
    vec2s v;
    v.raw[0] = x;
    v.raw[1] = y;
    return v;
}

MU_INLINE vec3s mu_ga_vec3(float x, float y, float z)
{
    vec3s v;
    v.raw[0] = x;
    v.raw[1] = y;
    v.raw[2] = z;
    return v;
}

MU_INLINE vec4s mu_ga_vec4(float x, float y, float z, float w)
{
    vec4s v;
    v.raw[0] = x;
    v.raw[1] = y;
    v.raw[2] = z;
    v.raw[3] = w;
    return v;
}

MU_INLINE void mu_ga_cos_sin_half(float angle, float *c, float *s)
{
    float h = angle * 0.5f;
    *c = cosf(h);
    *s = sinf(h);
}

/* ------------------------------------------------------------------------- */
/* basic vectors */

MU_INLINE vec3s mu_ga_mat3_mulv(mat3s m, vec3s v)
{
    return mu_ga_vec3(
        m.col[0].x * v.x + m.col[1].x * v.y + m.col[2].x * v.z,
        m.col[0].y * v.x + m.col[1].y * v.y + m.col[2].y * v.z,
        m.col[0].z * v.x + m.col[1].z * v.y + m.col[2].z * v.z
    );
}

MU_INLINE vec4s mu_ga_mat4_mulv(mat4s m, vec4s v)
{
    return mu_ga_vec4(
        m.col[0].x * v.x + m.col[1].x * v.y + m.col[2].x * v.z + m.col[3].x * v.w,
        m.col[0].y * v.x + m.col[1].y * v.y + m.col[2].y * v.z + m.col[3].y * v.w,
        m.col[0].z * v.x + m.col[1].z * v.y + m.col[2].z * v.z + m.col[3].z * v.w,
        m.col[0].w * v.x + m.col[1].w * v.y + m.col[2].w * v.z + m.col[3].w * v.w
    );
}

/*
   In this representation, bivectors are encoded as a 3-float pseudovector.
   This is the same memory layout used by many PGA implementations:

       B = x e23 + y e31 + z e12

   so we can reuse cross-product arithmetic for wedge/antiwedge formulas.
*/
MU_INLINE vec3s mu_ga_wedge_v3_v3(vec3s a, vec3s b)
{
    return glms_vec3_cross(a, b);
}

/* ------------------------------------------------------------------------- */
/* rigid 2D */
/* ------------------------------------------------------------------------- */

typedef struct { float x, y, z; } mu_ga_flat_point2;
typedef struct { float x, y, z; } mu_ga_line2;

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_make(float x, float y, float z)
{
    mu_ga_flat_point2 p = {x,y,z};
    return p;
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_from_point(vec2s p)
{
    return mu_ga_flat_point2_make(p.x, p.y, 1.0f);
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_from_dir(vec2s v)
{
    return mu_ga_flat_point2_make(v.x, v.y, 0.0f);
}

MU_INLINE mu_ga_line2 mu_ga_line2_make(float x, float y, float z)
{
    mu_ga_line2 g = {x,y,z};
    return g;
}

MU_INLINE mu_ga_line2 mu_ga_line2_from_points(vec2s p, vec2s q)
{
    return mu_ga_line2_make(p.y - q.y, q.x - p.x, p.x * q.y - p.y * q.x);
}

MU_INLINE mu_ga_line2 mu_ga_line2_from_point_dir(vec2s p, vec2s v)
{
    return mu_ga_line2_make(-v.y, v.x, p.x * v.y - p.y * v.x);
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_unitize(mu_ga_flat_point2 p)
{
    float inv = 1.0f / p.z;
    return mu_ga_flat_point2_make(p.x * inv, p.y * inv, 1.0f);
}

MU_INLINE mu_ga_line2 mu_ga_line2_unitize(mu_ga_line2 g)
{
    float inv = mu_ga_inv_sqrt(g.x * g.x + g.y * g.y);
    return mu_ga_line2_make(g.x * inv, g.y * inv, g.z * inv);
}

MU_INLINE mu_ga_line2 mu_ga_line2_complement(mu_ga_flat_point2 p)
{
    return mu_ga_line2_make(-p.x, -p.y, -p.z);
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_complement(mu_ga_line2 g)
{
    return mu_ga_flat_point2_make(-g.x, -g.y, -g.z);
}

MU_INLINE mu_ga_line2 mu_ga_line2_bulk_dual(mu_ga_flat_point2 p)
{
    return mu_ga_line2_make(-p.x, -p.y, 0.0f);
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_bulk_dual(mu_ga_line2 g)
{
    return mu_ga_flat_point2_make(0.0f, 0.0f, -g.z);
}

MU_INLINE mu_ga_line2 mu_ga_line2_weight_dual(mu_ga_flat_point2 p)
{
    return mu_ga_line2_make(0.0f, 0.0f, -p.z);
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_weight_dual(mu_ga_line2 g)
{
    return mu_ga_flat_point2_make(-g.x, -g.y, 0.0f);
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_support(mu_ga_line2 g)
{
    return mu_ga_flat_point2_make(-g.x * g.z, -g.y * g.z, g.x * g.x + g.y * g.y);
}

MU_INLINE mu_ga_line2 mu_ga_line2_antisupport(mu_ga_flat_point2 p)
{
    return mu_ga_line2_make(-p.x * p.z, -p.y * p.z, p.x * p.x + p.y * p.y);
}

MU_INLINE float mu_ga_flat_point2_attitude(mu_ga_flat_point2 p) { return p.z; }
MU_INLINE vec2s mu_ga_line2_attitude(mu_ga_line2 g) { return mu_ga_vec2(g.y, -g.x); }

MU_INLINE float mu_ga_flat_point2_squared_bulk_norm(mu_ga_flat_point2 p) { return p.x*p.x + p.y*p.y; }
MU_INLINE float mu_ga_line2_squared_bulk_norm(mu_ga_line2 g) { return g.z*g.z; }
MU_INLINE float mu_ga_flat_point2_squared_weight_norm(mu_ga_flat_point2 p) { return p.z*p.z; }
MU_INLINE float mu_ga_line2_squared_weight_norm(mu_ga_line2 g) { return g.x*g.x + g.y*g.y; }

MU_INLINE float mu_ga_flat_point2_dot(mu_ga_flat_point2 a, mu_ga_flat_point2 b)
{
    return a.x * b.x + a.y * b.y;
}

MU_INLINE float mu_ga_line2_dot(mu_ga_line2 a, mu_ga_line2 b)
{
    return a.z * b.z;
}

MU_INLINE float mu_ga_flat_point2_antidot(mu_ga_flat_point2 a, mu_ga_flat_point2 b)
{
    return a.z * b.z;
}

MU_INLINE float mu_ga_line2_antidot(mu_ga_line2 a, mu_ga_line2 b)
{
    return a.x * b.x + a.y * b.y;
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_translate(mu_ga_flat_point2 p, vec2s t)
{
    return mu_ga_flat_point2_make(p.x + t.x * p.z, p.y + t.y * p.z, p.z);
}

MU_INLINE mu_ga_line2 mu_ga_line2_translate(mu_ga_line2 g, vec2s t)
{
    return mu_ga_line2_make(g.x, g.y, g.z - g.x * t.x - g.y * t.y);
}

MU_INLINE mu_ga_line2 mu_ga_wedge_flat_point2_flat_point2(mu_ga_flat_point2 p, mu_ga_flat_point2 q)
{
    return mu_ga_line2_make(
        p.y * q.z - p.z * q.y,
        p.z * q.x - p.x * q.z,
        p.x * q.y - p.y * q.x
    );
}

MU_INLINE mu_ga_line2 mu_ga_wedge_point2_point2(vec2s p, vec2s q)
{
    return mu_ga_line2_make(p.y - q.y, q.x - p.x, p.x * q.y - p.y * q.x);
}

MU_INLINE mu_ga_line2 mu_ga_wedge_point2_vec2(vec2s p, vec2s v)
{
    return mu_ga_line2_make(-v.y, v.x, p.x * v.y - p.y * v.x);
}

MU_INLINE mu_ga_flat_point2 mu_ga_antiwedge_line2_line2(mu_ga_line2 g, mu_ga_line2 h)
{
    return mu_ga_flat_point2_make(
        g.z * h.y - g.y * h.z,
        g.x * h.z - g.z * h.x,
        g.y * h.x - g.x * h.y
    );
}

MU_INLINE float mu_ga_antiwedge_point2_line2(vec2s p, mu_ga_line2 g)
{
    return -(p.x * g.x + p.y * g.y + g.z);
}

/*
    Projection on unitized line

        dist = n . p + c
        p_proj = p - n * dist

    Here antiwedge(point,line) = -(n.p + c), so
        p_proj = p + n * antiwedge(point,line)
*/
MU_INLINE vec2s mu_ga_project_point2_line2(vec2s p, mu_ga_line2 g)
{
    float a = mu_ga_antiwedge_point2_line2(p, g);
    return mu_ga_vec2(p.x + g.x * a, p.y + g.y * a);
}

MU_INLINE mu_ga_line2 mu_ga_antiproject_line2_point2(mu_ga_line2 g, vec2s p)
{
    return mu_ga_line2_make(g.x, g.y, -p.x * g.x - p.y * g.y);
}

/* ------------------------------------------------------------------------- */
/* rigid 3D */
/* ------------------------------------------------------------------------- */

typedef struct { float x, y, z, w; } mu_ga_flat_point3;
typedef struct { float vx, vy, vz, mx, my, mz; } mu_ga_line3;
typedef struct { float x, y, z, w; } mu_ga_plane3;

MU_INLINE mu_ga_flat_point3 mu_ga_flat_point3_make(float x, float y, float z, float w)
{
    mu_ga_flat_point3 p = {x,y,z,w};
    return p;
}

MU_INLINE mu_ga_flat_point3 mu_ga_flat_point3_from_point(vec3s p)
{
    return mu_ga_flat_point3_make(p.x, p.y, p.z, 1.0f);
}

MU_INLINE mu_ga_line3 mu_ga_line3_make(float vx, float vy, float vz, float mx, float my, float mz)
{
    mu_ga_line3 l = {vx,vy,vz,mx,my,mz};
    return l;
}

MU_INLINE mu_ga_plane3 mu_ga_plane3_make(float x, float y, float z, float w)
{
    mu_ga_plane3 g = {x,y,z,w};
    return g;
}

MU_INLINE mu_ga_line3 mu_ga_wedge_flat_point3_flat_point3(mu_ga_flat_point3 p, mu_ga_flat_point3 q)
{
    return mu_ga_line3_make(
        p.w * q.x - p.x * q.w,
        p.w * q.y - p.y * q.w,
        p.w * q.z - p.z * q.w,
        p.y * q.z - p.z * q.y,
        p.z * q.x - p.x * q.z,
        p.x * q.y - p.y * q.x
    );
}

MU_INLINE mu_ga_line3 mu_ga_wedge_point3_point3(vec3s p, vec3s q)
{
    return mu_ga_line3_make(
        q.x - p.x,
        q.y - p.y,
        q.z - p.z,
        p.y * q.z - p.z * q.y,
        p.z * q.x - p.x * q.z,
        p.x * q.y - p.y * q.x
    );
}

MU_INLINE mu_ga_line3 mu_ga_wedge_point3_vec3(vec3s p, vec3s v)
{
    return mu_ga_line3_make(
        v.x, v.y, v.z,
        p.y * v.z - p.z * v.y,
        p.z * v.x - p.x * v.z,
        p.x * v.y - p.y * v.x
    );
}

MU_INLINE mu_ga_plane3 mu_ga_wedge_line3_flat_point3(mu_ga_line3 l, mu_ga_flat_point3 p)
{
    return mu_ga_plane3_make(
        l.vy * p.z - l.vz * p.y + l.mx * p.w,
        l.vz * p.x - l.vx * p.z + l.my * p.w,
        l.vx * p.y - l.vy * p.x + l.mz * p.w,
       -l.mx * p.x - l.my * p.y - l.mz * p.z
    );
}

MU_INLINE mu_ga_plane3 mu_ga_wedge_line3_point3(mu_ga_line3 l, vec3s p)
{
    return mu_ga_plane3_make(
        l.vy * p.z - l.vz * p.y + l.mx,
        l.vz * p.x - l.vx * p.z + l.my,
        l.vx * p.y - l.vy * p.x + l.mz,
       -l.mx * p.x - l.my * p.y - l.mz * p.z
    );
}

MU_INLINE mu_ga_plane3 mu_ga_wedge_line3_vec3(mu_ga_line3 l, vec3s v)
{
    return mu_ga_plane3_make(
        l.vy * v.z - l.vz * v.y,
        l.vz * v.x - l.vx * v.z,
        l.vx * v.y - l.vy * v.x,
       -(l.mx * v.x + l.my * v.y + l.mz * v.z)
    );
}

MU_INLINE mu_ga_line3 mu_ga_antiwedge_plane3_plane3(mu_ga_plane3 g, mu_ga_plane3 h)
{
    return mu_ga_line3_make(
        g.z * h.y - g.y * h.z,
        g.x * h.z - g.z * h.x,
        g.y * h.x - g.x * h.y,
        g.x * h.w - g.w * h.x,
        g.y * h.w - g.w * h.y,
        g.z * h.w - g.w * h.z
    );
}

MU_INLINE mu_ga_flat_point3 mu_ga_antiwedge_plane3_line3(mu_ga_plane3 g, mu_ga_line3 l)
{
    return mu_ga_flat_point3_make(
        l.my * g.z - l.mz * g.y + l.vx * g.w,
        l.mz * g.x - l.mx * g.z + l.vy * g.w,
        l.mx * g.y - l.my * g.x + l.vz * g.w,
       -(l.vx * g.x + l.vy * g.y + l.vz * g.z)
    );
}

MU_INLINE float mu_ga_antiwedge_line3_line3(mu_ga_line3 k, mu_ga_line3 l)
{
    return -(
        (k.vx * l.mx + k.vy * l.my + k.vz * l.mz) +
        (k.mx * l.vx + k.my * l.vy + k.mz * l.vz)
    );
}

MU_INLINE float mu_ga_antiwedge_flat_point3_plane3(mu_ga_flat_point3 p, mu_ga_plane3 g)
{
    return p.x * g.x + p.y * g.y + p.z * g.z + p.w * g.w;
}

MU_INLINE float mu_ga_antiwedge_point3_plane3(vec3s p, mu_ga_plane3 g)
{
    return p.x * g.x + p.y * g.y + p.z * g.z + g.w;
}

MU_INLINE float mu_ga_antiwedge_vec3_plane3(vec3s v, mu_ga_plane3 g)
{
    return v.x * g.x + v.y * g.y + v.z * g.z;
}

MU_INLINE mu_ga_flat_point3 mu_ga_flat_point3_translate(mu_ga_flat_point3 p, vec3s t)
{
    return mu_ga_flat_point3_make(p.x + t.x * p.w, p.y + t.y * p.w, p.z + t.z * p.w, p.w);
}

MU_INLINE mu_ga_line3 mu_ga_line3_translate(mu_ga_line3 l, vec3s t)
{
    vec3s v = mu_ga_vec3(l.vx, l.vy, l.vz);
    vec3s m = mu_ga_vec3(l.mx, l.my, l.mz);
    vec3s mm = glms_vec3_add(m, glms_vec3_cross(t, v));
    return mu_ga_line3_make(l.vx, l.vy, l.vz, mm.x, mm.y, mm.z);
}

MU_INLINE mu_ga_plane3 mu_ga_plane3_translate(mu_ga_plane3 g, vec3s t)
{
    return mu_ga_plane3_make(g.x, g.y, g.z, g.w - g.x * t.x - g.y * t.y - g.z * t.z);
}

MU_INLINE vec3s mu_ga_project_point3_line3(vec3s p, mu_ga_line3 l)
{
    vec3s v = mu_ga_vec3(l.vx, l.vy, l.vz);
    vec3s m = mu_ga_vec3(l.mx, l.my, l.mz);
    float d = glms_vec3_dot(v, p);
    vec3s c = glms_vec3_cross(v, m);
    return mu_ga_vec3(d * v.x + c.x, d * v.y + c.y, d * v.z + c.z);
}

MU_INLINE vec3s mu_ga_project_point3_plane3(vec3s p, mu_ga_plane3 g)
{
    float dist = mu_ga_antiwedge_point3_plane3(p, g);
    return mu_ga_vec3(p.x - g.x * dist, p.y - g.y * dist, p.z - g.z * dist);
}

MU_INLINE mu_ga_line3 mu_ga_project_line3_plane3(mu_ga_line3 l, mu_ga_plane3 g)
{
    vec3s n = mu_ga_vec3(g.x, g.y, g.z);
    vec3s v = mu_ga_vec3(l.vx, l.vy, l.vz);
    vec3s m = mu_ga_vec3(l.mx, l.my, l.mz);

    vec3s vproj = glms_vec3_sub(v, glms_vec3_scale(n, glms_vec3_dot(n, v)));
    vec3s mproj = glms_vec3_sub(glms_vec3_scale(n, glms_vec3_dot(n, m)), glms_vec3_scale(vproj, g.w));

    return mu_ga_line3_make(vproj.x, vproj.y, vproj.z, mproj.x, mproj.y, mproj.z);
}

MU_INLINE mu_ga_line3 mu_ga_antiproject_line3_point3(mu_ga_line3 l, vec3s p)
{
    return mu_ga_wedge_point3_vec3(p, mu_ga_vec3(l.vx, l.vy, l.vz));
}

MU_INLINE mu_ga_plane3 mu_ga_antiproject_plane3_point3(mu_ga_plane3 g, vec3s p)
{
    return mu_ga_plane3_make(g.x, g.y, g.z, -(g.x * p.x + g.y * p.y + g.z * p.z));
}

/* ------------------------------------------------------------------------- */
/* motor / flector 2D */
/* ------------------------------------------------------------------------- */

typedef struct { float x, y, z, w; } mu_ga_motor2;
typedef struct { float x, y, z, w; } mu_ga_flector2;

MU_INLINE mu_ga_motor2 mu_ga_motor2_identity(void) { mu_ga_motor2 q = {0,0,0,1}; return q; }

MU_INLINE mu_ga_motor2 mu_ga_motor2_make(float x, float y, float z, float w)
{
    mu_ga_motor2 q = {x,y,z,w};
    return q;
}

MU_INLINE mu_ga_motor2 mu_ga_motor2_unitize(mu_ga_motor2 q)
{
    float inv = mu_ga_inv_sqrt(q.z * q.z + q.w * q.w);
    return mu_ga_motor2_make(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
}

MU_INLINE mu_ga_motor2 mu_ga_motor2_make_rotation(float angle, vec2s center)
{
    float c, s;
    mu_ga_cos_sin_half(angle, &c, &s);
    return mu_ga_motor2_make(center.x * s, center.y * s, s, c);
}

MU_INLINE mu_ga_motor2 mu_ga_motor2_make_translation(vec2s offset)
{
    return mu_ga_motor2_make(offset.y * -0.5f, offset.x * 0.5f, 0.0f, 1.0f);
}

MU_INLINE mu_ga_motor2 mu_ga_motor2_mul(mu_ga_motor2 a, mu_ga_motor2 b)
{
    return mu_ga_motor2_make(
        a.x * b.w + b.x * a.w + a.y * b.z - a.z * b.y,
        a.y * b.w + b.y * a.w + a.z * b.x - a.x * b.z,
        a.z * b.w + a.w * b.z,
        a.w * b.w - a.z * b.z
    );
}

MU_INLINE mat3s mu_ga_motor2_to_xform(mu_ga_motor2 q)
{
    float m00 = 1.0f - q.z * q.z * 2.0f;
    float m01 = q.z * q.w * -2.0f;
    float m02 = (q.x * q.z + q.y * q.w) * 2.0f;
    float m10 = q.z * q.w * 2.0f;
    float m11 = 1.0f - q.z * q.z * 2.0f;
    float m12 = (q.y * q.z - q.x * q.w) * 2.0f;

    mat3s m;
    m.col[0] = mu_ga_vec3(m00, m10, 0.0f);
    m.col[1] = mu_ga_vec3(m01, m11, 0.0f);
    m.col[2] = mu_ga_vec3(m02, m12, 1.0f);
    return m;
}

MU_INLINE mat3s mu_ga_motor2_to_inverse_xform(mu_ga_motor2 q)
{
    float m00 = 1.0f - q.z * q.z * 2.0f;
    float m01 = q.z * q.w * 2.0f;
    float m02 = (q.x * q.z - q.y * q.w) * 2.0f;
    float m10 = q.z * q.w * -2.0f;
    float m11 = 1.0f - q.z * q.z * 2.0f;
    float m12 = (q.y * q.z + q.x * q.w) * 2.0f;

    mat3s m;
    m.col[0] = mu_ga_vec3(m00, m10, 0.0f);
    m.col[1] = mu_ga_vec3(m01, m11, 0.0f);
    m.col[2] = mu_ga_vec3(m02, m12, 1.0f);
    return m;
}

MU_INLINE vec2s mu_ga_transform_vec2_motor(vec2s v, mu_ga_motor2 q)
{
    mat3s m = mu_ga_motor2_to_xform(q);
    vec3s r = mu_ga_mat3_mulv(m, mu_ga_vec3(v.x, v.y, 0.0f));
    return mu_ga_vec2(r.x, r.y);
}

MU_INLINE vec2s mu_ga_transform_point2_motor(vec2s p, mu_ga_motor2 q)
{
    mat3s m = mu_ga_motor2_to_xform(q);
    vec3s r = mu_ga_mat3_mulv(m, mu_ga_vec3(p.x, p.y, 1.0f));
    return mu_ga_vec2(r.x, r.y);
}

MU_INLINE mu_ga_flat_point2 mu_ga_transform_flat_point2_motor(mu_ga_flat_point2 p, mu_ga_motor2 q)
{
    mat3s m = mu_ga_motor2_to_xform(q);
    vec3s r = mu_ga_mat3_mulv(m, mu_ga_vec3(p.x / p.z, p.y / p.z, 1.0f));
    return mu_ga_flat_point2_make(r.x * p.z, r.y * p.z, p.z);
}

MU_INLINE mu_ga_line2 mu_ga_transform_line2_motor(mu_ga_line2 g, mu_ga_motor2 q)
{
    mat3s m = mu_ga_motor2_to_xform(q);
    vec3s n = mu_ga_mat3_mulv(m, mu_ga_vec3(g.x, g.y, 0.0f));
    float m02 = m.col[2].x;
    float m12 = m.col[2].y;
    float c = g.z - (n.x * m02 + n.y * m12);
    return mu_ga_line2_make(n.x, n.y, c);
}

MU_INLINE mu_ga_motor2 mu_ga_motor2_set_xform(mat3s M)
{
    float m00 = M.col[0].x;
    float m10 = M.col[0].y;
    float m02 = M.col[2].x;
    float m12 = M.col[2].y;
    mu_ga_motor2 q;
    if (m00 < 1.0f) {
        q.z = mu_ga_sqrt(0.5f - m00 * 0.5f);
        q.w = m10 * 0.5f / q.z;

        m02 *= 0.5f;
        m12 *= 0.5f;

        q.x = q.z * m02 - q.w * m12;
        q.y = q.w * m02 + q.z * m12;
    } else {
        q.x = m12 * -0.5f;
        q.y = m02 * 0.5f;
        q.z = 0.0f;
        q.w = 1.0f;
    }
    return q;
}

MU_INLINE mu_ga_flector2 mu_ga_flector2_make(float x, float y, float z, float w)
{
    mu_ga_flector2 f = {x,y,z,w};
    return f;
}

MU_INLINE mu_ga_flector2 mu_ga_flector2_unitize(mu_ga_flector2 f)
{
    float inv = mu_ga_inv_sqrt(f.x * f.x + f.y * f.y);
    return mu_ga_flector2_make(f.x * inv, f.y * inv, f.z * inv, f.w * inv);
}

MU_INLINE mu_ga_flector2 mu_ga_flector2_make_transflection(vec2s offset, mu_ga_line2 line_unit)
{
    return mu_ga_flector2_make(
        line_unit.x,
        line_unit.y,
        (offset.x * line_unit.x + offset.y * line_unit.y) * 0.5f + line_unit.z,
        (offset.y * line_unit.x - offset.x * line_unit.y) * 0.5f
    );
}

MU_INLINE mat3s mu_ga_flector2_to_xform(mu_ga_flector2 f)
{
    float m00 = 1.0f - f.x * f.x * 2.0f;
    float m01 = f.x * f.y * -2.0f;
    float m02 = (f.x * f.z + f.y * f.w) * -2.0f;
    float m10 = f.x * f.y * -2.0f;
    float m11 = 1.0f - f.y * f.y * 2.0f;
    float m12 = (f.y * f.z - f.x * f.w) * -2.0f;

    mat3s m;
    m.col[0] = mu_ga_vec3(m00, m10, 0.0f);
    m.col[1] = mu_ga_vec3(m01, m11, 0.0f);
    m.col[2] = mu_ga_vec3(m02, m12, 1.0f);
    return m;
}

MU_INLINE mat3s mu_ga_flector2_to_inverse_xform(mu_ga_flector2 f)
{
    float m00 = 1.0f - f.x * f.x * 2.0f;
    float m01 = f.x * f.y * -2.0f;
    float m02 = (f.x * f.z - f.y * f.w) * -2.0f;
    float m10 = f.x * f.y * -2.0f;
    float m11 = 1.0f - f.y * f.y * 2.0f;
    float m12 = (f.y * f.z + f.x * f.w) * -2.0f;

    mat3s m;
    m.col[0] = mu_ga_vec3(m00, m10, 0.0f);
    m.col[1] = mu_ga_vec3(m01, m11, 0.0f);
    m.col[2] = mu_ga_vec3(m02, m12, 1.0f);
    return m;
}

MU_INLINE mu_ga_motor2 mu_ga_flector2_mul_flector2(mu_ga_flector2 a, mu_ga_flector2 b)
{
    return mu_ga_motor2_make(
        a.z * b.y - a.y * b.z - a.x * b.w - b.x * a.w,
        a.x * b.z - a.z * b.x - a.y * b.w - b.y * a.w,
        a.y * b.x - a.x * b.y,
        a.x * b.x + a.y * b.y
    );
}

MU_INLINE mu_ga_flector2 mu_ga_flector2_mul_motor2(mu_ga_flector2 a, mu_ga_motor2 b)
{
    return mu_ga_flector2_make(
        a.x * b.w + a.y * b.z,
        a.y * b.w - a.x * b.z,
        a.x * b.y - a.y * b.x + a.z * b.w + b.z * a.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    );
}

MU_INLINE mu_ga_flector2 mu_ga_motor2_mul_flector2(mu_ga_motor2 a, mu_ga_flector2 b)
{
    return mu_ga_flector2_make(
        a.w * b.x - a.z * b.y,
        a.w * b.y + a.z * b.x,
        a.x * b.y - a.y * b.x + a.z * b.w + b.z * a.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    );
}

MU_INLINE vec2s mu_ga_transform_vec2_flector(vec2s v, mu_ga_flector2 f)
{
    mat3s m = mu_ga_flector2_to_xform(f);
    vec3s r = mu_ga_mat3_mulv(m, mu_ga_vec3(v.x, v.y, 0.0f));
    return mu_ga_vec2(r.x, r.y);
}

MU_INLINE vec2s mu_ga_transform_point2_flector(vec2s p, mu_ga_flector2 f)
{
    mat3s m = mu_ga_flector2_to_xform(f);
    vec3s r = mu_ga_mat3_mulv(m, mu_ga_vec3(p.x, p.y, 1.0f));
    return mu_ga_vec2(r.x, r.y);
}

MU_INLINE mu_ga_flat_point2 mu_ga_transform_flat_point2_flector(mu_ga_flat_point2 p, mu_ga_flector2 f)
{
    vec3s r = mu_ga_mat3_mulv(mu_ga_flector2_to_xform(f), mu_ga_vec3(p.x / p.z, p.y / p.z, 1.0f));
    return mu_ga_flat_point2_make(r.x * p.z, r.y * p.z, p.z);
}

MU_INLINE mu_ga_line2 mu_ga_transform_line2_flector(mu_ga_line2 g, mu_ga_flector2 f)
{
    mat3s m = mu_ga_flector2_to_xform(f);
    vec3s n = mu_ga_mat3_mulv(m, mu_ga_vec3(g.x, g.y, 0.0f));
    float m02 = m.col[2].x;
    float m12 = m.col[2].y;
    float c = g.z - (n.x * m02 + n.y * m12);
    return mu_ga_line2_make(n.x, n.y, c);
}

/* ------------------------------------------------------------------------- */
/* motor / flector 3D */
/* ------------------------------------------------------------------------- */
typedef struct { float vx, vy, vz, vw, mx, my, mz, mw; } mu_ga_motor3;
typedef struct { float px, py, pz, pw, gx, gy, gz, gw; } mu_ga_flector3;

MU_INLINE mu_ga_motor3 mu_ga_motor3_identity(void)
{
    mu_ga_motor3 q = {0,0,0,1,0,0,0,0};
    return q;
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_make(float vx, float vy, float vz, float vw, float mx, float my, float mz, float mw)
{
    mu_ga_motor3 q = {vx,vy,vz,vw,mx,my,mz,mw};
    return q;
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_unitize(mu_ga_motor3 q)
{
    float inv = mu_ga_inv_sqrt(q.vx*q.vx + q.vy*q.vy + q.vz*q.vz + q.vw*q.vw);
    return mu_ga_motor3_make(q.vx*inv, q.vy*inv, q.vz*inv, q.vw*inv, q.mx*inv, q.my*inv, q.mz*inv, q.mw*inv);
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_make_rotation(float angle, vec3s axis_unit)
{
    float c, s;
    mu_ga_cos_sin_half(angle, &c, &s);
    return mu_ga_motor3_make(axis_unit.x * s, axis_unit.y * s, axis_unit.z * s, c, 0.0f, 0.0f, 0.0f, 0.0f);
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_make_translation(vec3s offset)
{
    return mu_ga_motor3_make(0.0f, 0.0f, 0.0f, 1.0f, offset.x * 0.5f, offset.y * 0.5f, offset.z * 0.5f, 0.0f);
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_make_screw(float angle, mu_ga_line3 axis_unit, float disp)
{
    float c, s;
    mu_ga_cos_sin_half(angle, &c, &s);
    disp *= 0.5f;
    return mu_ga_motor3_make(
        axis_unit.vx * s,
        axis_unit.vy * s,
        axis_unit.vz * s,
        c,
        disp * axis_unit.vx * c + axis_unit.mx * s,
        disp * axis_unit.vy * c + axis_unit.my * s,
        disp * axis_unit.vz * c + axis_unit.mz * s,
        -disp * s
    );
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_mul(mu_ga_motor3 a, mu_ga_motor3 b)
{
    return mu_ga_motor3_make(
        a.vw * b.vx + a.vx * b.vw + a.vy * b.vz - a.vz * b.vy,
        a.vw * b.vy + a.vy * b.vw + a.vz * b.vx - a.vx * b.vz,
        a.vw * b.vz + a.vz * b.vw + a.vx * b.vy - a.vy * b.vx,
        a.vw * b.vw - a.vx * b.vx - a.vy * b.vy - a.vz * b.vz,
        a.mw * b.vx + a.mx * b.vw + a.my * b.vz - a.mz * b.vy + b.mw * a.vx + b.mx * a.vw - b.my * a.vz + b.mz * a.vy,
        a.mw * b.vy - a.mx * b.vz + a.my * b.vw + a.mz * b.vx + b.mw * a.vy + b.mx * a.vz + b.my * a.vw - b.mz * a.vx,
        a.mw * b.vz + a.mx * b.vy - a.my * b.vx + a.mz * b.vw + b.mw * a.vz - b.mx * a.vy + b.my * a.vx + b.mz * a.vw,
        a.mw * b.vw - a.mx * b.vx - a.my * b.vy - a.mz * b.vz + b.mw * a.vw - b.mx * a.vx - b.my * a.vy - b.mz * a.vz
    );
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_sqrt(mu_ga_motor3 q)
{
    float b = mu_ga_inv_sqrt(q.vw * 2.0f + 2.0f);
    float a = -q.mw * (b * b);
    return mu_ga_motor3_make(
        q.vx * b,
        q.vy * b,
        q.vz * b,
        q.vw * b + b,
        (q.vx * a + q.mx) * b,
        (q.vy * a + q.my) * b,
        (q.vz * a + q.mz) * b,
        q.mw * (b * 0.5f)
    );
}

/*
    Convert motor to affine transform [R|t].

    Math structure:
        rotor part v = (vx,vy,vz,vw)
        screw part m = (mx,my,mz,mw)

    Rotation block uses quaternion matrix expansion.
    Translation block comes from dual part coupling.
*/
MU_INLINE mat4s mu_ga_motor3_to_xform(mu_ga_motor3 q)
{
    float vx2 = q.vx * q.vx;
    float vy2 = q.vy * q.vy;
    float vz2 = q.vz * q.vz;

    float A00 = 1.0f - (vy2 + vz2) * 2.0f;
    float A11 = 1.0f - (vz2 + vx2) * 2.0f;
    float A22 = 1.0f - (vx2 + vy2) * 2.0f;
    float A01 = q.vx * q.vy;
    float A02 = q.vz * q.vx;
    float A12 = q.vy * q.vz;
    float A03 = q.vy * q.mz - q.vz * q.my;
    float A13 = q.vz * q.mx - q.vx * q.mz;
    float A23 = q.vx * q.my - q.vy * q.mx;

    float B01 = q.vz * q.vw;
    float B20 = q.vy * q.vw;
    float B12 = q.vx * q.vw;
    float B03 = q.mx * q.vw - q.vx * q.mw;
    float B13 = q.my * q.vw - q.vy * q.mw;
    float B23 = q.mz * q.vw - q.vz * q.mw;

    float m00 = A00;
    float m01 = (A01 - B01) * 2.0f;
    float m02 = (A02 + B20) * 2.0f;
    float m03 = (A03 + B03) * 2.0f;
    float m10 = (A01 + B01) * 2.0f;
    float m11 = A11;
    float m12 = (A12 - B12) * 2.0f;
    float m13 = (A13 + B13) * 2.0f;
    float m20 = (A02 - B20) * 2.0f;
    float m21 = (A12 + B12) * 2.0f;
    float m22 = A22;
    float m23 = (A23 + B23) * 2.0f;

    mat4s m;
    m.col[0] = mu_ga_vec4(m00, m10, m20, 0.0f);
    m.col[1] = mu_ga_vec4(m01, m11, m21, 0.0f);
    m.col[2] = mu_ga_vec4(m02, m12, m22, 0.0f);
    m.col[3] = mu_ga_vec4(m03, m13, m23, 1.0f);
    return m;
}

MU_INLINE mat4s mu_ga_motor3_to_inverse_xform(mu_ga_motor3 q)
{
    float vx2 = q.vx * q.vx;
    float vy2 = q.vy * q.vy;
    float vz2 = q.vz * q.vz;

    float A00 = 1.0f - (vy2 + vz2) * 2.0f;
    float A11 = 1.0f - (vz2 + vx2) * 2.0f;
    float A22 = 1.0f - (vx2 + vy2) * 2.0f;
    float A01 = q.vx * q.vy;
    float A02 = q.vz * q.vx;
    float A12 = q.vy * q.vz;
    float A03 = q.vy * q.mz - q.vz * q.my;
    float A13 = q.vz * q.mx - q.vx * q.mz;
    float A23 = q.vx * q.my - q.vy * q.mx;

    float B01 = q.vz * q.vw;
    float B20 = q.vy * q.vw;
    float B12 = q.vx * q.vw;
    float B03 = q.mx * q.vw - q.vx * q.mw;
    float B13 = q.my * q.vw - q.vy * q.mw;
    float B23 = q.mz * q.vw - q.vz * q.mw;

    float m00 = A00;
    float m01 = (A01 + B01) * 2.0f;
    float m02 = (A02 - B20) * 2.0f;
    float m03 = (A03 - B03) * 2.0f;
    float m10 = (A01 - B01) * 2.0f;
    float m11 = A11;
    float m12 = (A12 + B12) * 2.0f;
    float m13 = (A13 - B13) * 2.0f;
    float m20 = (A02 + B20) * 2.0f;
    float m21 = (A12 - B12) * 2.0f;
    float m22 = A22;
    float m23 = (A23 - B23) * 2.0f;

    mat4s m;
    m.col[0] = mu_ga_vec4(m00, m10, m20, 0.0f);
    m.col[1] = mu_ga_vec4(m01, m11, m21, 0.0f);
    m.col[2] = mu_ga_vec4(m02, m12, m22, 0.0f);
    m.col[3] = mu_ga_vec4(m03, m13, m23, 1.0f);
    return m;
}
MU_INLINE versors mu_ga_quat_from_mat3(mat3s m)
{
    versors q;
    float trace = m.col[0].x + m.col[1].y + m.col[2].z;

    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m.col[1].z - m.col[2].y) / s;
        q.y = (m.col[2].x - m.col[0].z) / s;
        q.z = (m.col[0].y - m.col[1].x) / s;
    } else if (m.col[0].x > m.col[1].y && m.col[0].x > m.col[2].z) {
        float s = sqrtf(1.0f + m.col[0].x - m.col[1].y - m.col[2].z) * 2.0f;
        q.w = (m.col[1].z - m.col[2].y) / s;
        q.x = 0.25f * s;
        q.y = (m.col[1].x + m.col[0].y) / s;
        q.z = (m.col[2].x + m.col[0].z) / s;
    } else if (m.col[1].y > m.col[2].z) {
        float s = sqrtf(1.0f + m.col[1].y - m.col[0].x - m.col[2].z) * 2.0f;
        q.w = (m.col[2].x - m.col[0].z) / s;
        q.x = (m.col[1].x + m.col[0].y) / s;
        q.y = 0.25f * s;
        q.z = (m.col[2].y + m.col[1].z) / s;
    } else {
        float s = sqrtf(1.0f + m.col[2].z - m.col[0].x - m.col[1].y) * 2.0f;
        q.w = (m.col[0].y - m.col[1].x) / s;
        q.x = (m.col[2].x + m.col[0].z) / s;
        q.y = (m.col[2].y + m.col[1].z) / s;
        q.z = 0.25f * s;
    }
    return q;
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_set_xform(mat4s M)
{
    mat3s r;
    r.col[0] = mu_ga_vec3(M.col[0].x, M.col[0].y, M.col[0].z);
    r.col[1] = mu_ga_vec3(M.col[1].x, M.col[1].y, M.col[1].z);
    r.col[2] = mu_ga_vec3(M.col[2].x, M.col[2].y, M.col[2].z);

    versors v = mu_ga_quat_from_mat3(r);

    float tx = M.col[3].x * 0.5f;
    float ty = M.col[3].y * 0.5f;
    float tz = M.col[3].z * 0.5f;

    float mx =  v.w * tx + v.z * ty - v.y * tz;
    float my =  v.w * ty + v.x * tz - v.z * tx;
    float mz =  v.w * tz + v.y * tx - v.x * ty;
    float mw = -v.x * tx - v.y * ty - v.z * tz;

    return mu_ga_motor3_make(v.x, v.y, v.z, v.w, mx, my, mz, mw);
}

MU_INLINE vec3s mu_ga_transform_vec3_motor(vec3s v, mu_ga_motor3 q)
{
    mat4s m = mu_ga_motor3_to_xform(q);
    vec4s r = mu_ga_mat4_mulv(m, mu_ga_vec4(v.x, v.y, v.z, 0.0f));
    return mu_ga_vec3(r.x, r.y, r.z);
}

MU_INLINE vec3s mu_ga_transform_point3_motor(vec3s p, mu_ga_motor3 q)
{
    mat4s m = mu_ga_motor3_to_xform(q);
    vec4s r = mu_ga_mat4_mulv(m, mu_ga_vec4(p.x, p.y, p.z, 1.0f));
    return mu_ga_vec3(r.x, r.y, r.z);
}

MU_INLINE mu_ga_flat_point3 mu_ga_transform_flat_point3_motor(mu_ga_flat_point3 p, mu_ga_motor3 q)
{
    vec3s ep = mu_ga_vec3(p.x / p.w, p.y / p.w, p.z / p.w);
    vec3s tp = mu_ga_transform_point3_motor(ep, q);
    return mu_ga_flat_point3_make(tp.x * p.w, tp.y * p.w, tp.z * p.w, p.w);
}

MU_INLINE mu_ga_line3 mu_ga_transform_line3_motor(mu_ga_line3 l, mu_ga_motor3 q)
{
    mat4s m = mu_ga_motor3_to_xform(q);
    vec4s v4 = mu_ga_mat4_mulv(m, mu_ga_vec4(l.vx, l.vy, l.vz, 0.0f));
    vec4s mm4 = mu_ga_mat4_mulv(m, mu_ga_vec4(l.mx, l.my, l.mz, 0.0f));
    vec3s v = mu_ga_vec3(v4.x, v4.y, v4.z);
    vec3s mm = mu_ga_vec3(mm4.x, mm4.y, mm4.z);
    vec3s t = mu_ga_vec3(m.col[3].x, m.col[3].y, m.col[3].z);
    vec3s mp = glms_vec3_add(mm, glms_vec3_cross(t, v));
    return mu_ga_line3_make(v.x, v.y, v.z, mp.x, mp.y, mp.z);
}

MU_INLINE mu_ga_plane3 mu_ga_transform_plane3_motor(mu_ga_plane3 g, mu_ga_motor3 q)
{
    mat4s m = mu_ga_motor3_to_xform(q);
    vec4s n4 = mu_ga_mat4_mulv(m, mu_ga_vec4(g.x, g.y, g.z, 0.0f));
    vec3s n = mu_ga_vec3(n4.x, n4.y, n4.z);
    float d = g.w - (n.x * m.col[3].x + n.y * m.col[3].y + n.z * m.col[3].z);
    return mu_ga_plane3_make(n.x, n.y, n.z, d);
}

MU_INLINE mu_ga_flector3 mu_ga_flector3_make(float px, float py, float pz, float pw, float gx, float gy, float gz, float gw)
{
    mu_ga_flector3 f = {px,py,pz,pw,gx,gy,gz,gw};
    return f;
}

MU_INLINE mu_ga_flector3 mu_ga_flector3_unitize(mu_ga_flector3 f)
{
    float inv = mu_ga_inv_sqrt(f.pw*f.pw + f.gx*f.gx + f.gy*f.gy + f.gz*f.gz);
    return mu_ga_flector3_make(f.px*inv,f.py*inv,f.pz*inv,f.pw*inv,f.gx*inv,f.gy*inv,f.gz*inv,f.gw*inv);
}

MU_INLINE mu_ga_flector3 mu_ga_flector3_make_transflection(vec3s offset, mu_ga_plane3 plane_unit)
{
    return mu_ga_flector3_make(
        (offset.y * plane_unit.z - offset.z * plane_unit.y) * 0.5f,
        (offset.z * plane_unit.x - offset.x * plane_unit.z) * 0.5f,
        (offset.x * plane_unit.y - offset.y * plane_unit.x) * 0.5f,
        0.0f,
        plane_unit.x,
        plane_unit.y,
        plane_unit.z,
        plane_unit.w - (offset.x * plane_unit.x + offset.y * plane_unit.y + offset.z * plane_unit.z) * 0.5f
    );
}

MU_INLINE mu_ga_flector3 mu_ga_flector3_make_rotoreflection_line(float angle, mu_ga_line3 axis_unit, mu_ga_plane3 plane_unit)
{
    float c, s;
    mu_ga_cos_sin_half(angle, &c, &s);

    float vx = axis_unit.vx * s;
    float vy = axis_unit.vy * s;
    float vz = axis_unit.vz * s;
    float mx = axis_unit.mx * s;
    float my = axis_unit.my * s;
    float mz = axis_unit.mz * s;

    return mu_ga_flector3_make(
        vx * plane_unit.w + my * plane_unit.z - mz * plane_unit.y,
        vy * plane_unit.w + mz * plane_unit.x - mx * plane_unit.z,
        vz * plane_unit.w + mx * plane_unit.y - my * plane_unit.x,
       -vx * plane_unit.x - vy * plane_unit.y - vz * plane_unit.z,
        c * plane_unit.x + vy * plane_unit.z - vz * plane_unit.y,
        c * plane_unit.y + vz * plane_unit.x - vx * plane_unit.z,
        c * plane_unit.z + vx * plane_unit.y - vy * plane_unit.x,
        c * plane_unit.w - mx * plane_unit.x - my * plane_unit.y - mz * plane_unit.z
    );
}

MU_INLINE mat4s mu_ga_flector3_to_xform(mu_ga_flector3 f)
{
    float gx2 = f.gx * f.gx;
    float gy2 = f.gy * f.gy;
    float gz2 = f.gz * f.gz;

    float A00 = (gy2 + gz2) * 2.0f - 1.0f;
    float A11 = (gz2 + gx2) * 2.0f - 1.0f;
    float A22 = (gx2 + gy2) * 2.0f - 1.0f;
    float A01 = f.gx * f.gy * -2.0f;
    float A02 = f.gz * f.gx * -2.0f;
    float A12 = f.gy * f.gz * -2.0f;
    float A03 = f.px * f.pw - f.gx * f.gw;
    float A13 = f.py * f.pw - f.gy * f.gw;
    float A23 = f.pz * f.pw - f.gz * f.gw;

    float B01 = f.gz * f.pw * 2.0f;
    float B20 = f.gy * f.pw * 2.0f;
    float B12 = f.gx * f.pw * 2.0f;
    float B03 = f.gy * f.pz - f.gz * f.py;
    float B13 = f.gz * f.px - f.gx * f.pz;
    float B23 = f.gx * f.py - f.gy * f.px;

    float m00 = A00;
    float m01 = A01 + B01;
    float m02 = A02 - B20;
    float m03 = (A03 + B03) * 2.0f;
    float m10 = A01 - B01;
    float m11 = A11;
    float m12 = A12 + B12;
    float m13 = (A13 + B13) * 2.0f;
    float m20 = A02 + B20;
    float m21 = A12 - B12;
    float m22 = A22;
    float m23 = (A23 + B23) * 2.0f;

    mat4s m;
    m.col[0] = mu_ga_vec4(m00, m10, m20, 0.0f);
    m.col[1] = mu_ga_vec4(m01, m11, m21, 0.0f);
    m.col[2] = mu_ga_vec4(m02, m12, m22, 0.0f);
    m.col[3] = mu_ga_vec4(m03, m13, m23, 1.0f);
    return m;
}

MU_INLINE mat4s mu_ga_flector3_to_inverse_xform(mu_ga_flector3 f)
{
    float gx2 = f.gx * f.gx;
    float gy2 = f.gy * f.gy;
    float gz2 = f.gz * f.gz;

    float A00 = (gy2 + gz2) * 2.0f - 1.0f;
    float A11 = (gz2 + gx2) * 2.0f - 1.0f;
    float A22 = (gx2 + gy2) * 2.0f - 1.0f;
    float A01 = f.gx * f.gy * -2.0f;
    float A02 = f.gz * f.gx * -2.0f;
    float A12 = f.gy * f.gz * -2.0f;
    float A03 = f.px * f.pw - f.gx * f.gw;
    float A13 = f.py * f.pw - f.gy * f.gw;
    float A23 = f.pz * f.pw - f.gz * f.gw;

    float B01 = f.gz * f.pw * 2.0f;
    float B20 = f.gy * f.pw * 2.0f;
    float B12 = f.gx * f.pw * 2.0f;
    float B03 = f.gy * f.pz - f.gz * f.py;
    float B13 = f.gz * f.px - f.gx * f.pz;
    float B23 = f.gx * f.py - f.gy * f.px;

    float m00 = A00;
    float m01 = A01 - B01;
    float m02 = A02 + B20;
    float m03 = (A03 - B03) * 2.0f;
    float m10 = A01 + B01;
    float m11 = A11;
    float m12 = A12 - B12;
    float m13 = (A13 - B13) * 2.0f;
    float m20 = A02 - B20;
    float m21 = A12 + B12;
    float m22 = A22;
    float m23 = (A23 - B23) * 2.0f;

    mat4s m;
    m.col[0] = mu_ga_vec4(m00, m10, m20, 0.0f);
    m.col[1] = mu_ga_vec4(m01, m11, m21, 0.0f);
    m.col[2] = mu_ga_vec4(m02, m12, m22, 0.0f);
    m.col[3] = mu_ga_vec4(m03, m13, m23, 1.0f);
    return m;
}

MU_INLINE mu_ga_motor3 mu_ga_flector3_mul_flector3(mu_ga_flector3 a, mu_ga_flector3 b)
{
    return mu_ga_motor3_make(
        a.gz * b.gy - a.gy * b.gz - a.gx * b.pw - a.pw * b.gx,
        a.gx * b.gz - a.gz * b.gx - a.gy * b.pw - a.pw * b.gy,
        a.gy * b.gx - a.gx * b.gy - a.gz * b.pw - a.pw * b.gz,
        a.gx * b.gx + a.gy * b.gy + a.gz * b.gz - a.pw * b.pw,

        a.pz * b.gy - a.py * b.gz + a.gy * b.pz - a.gz * b.py + a.gx * b.gw - a.gw * b.gx + a.pw * b.px - a.px * b.pw,
        a.px * b.gz - a.pz * b.gx + a.gz * b.px - a.gx * b.pz + a.gy * b.gw - a.gw * b.gy + a.pw * b.py - a.py * b.pw,
        a.py * b.gx - a.px * b.gy + a.gx * b.py - a.gy * b.px + a.gz * b.gw - a.gw * b.gz + a.pw * b.pz - a.pz * b.pw,
        a.px * b.gx + a.py * b.gy + a.pz * b.gz + a.pw * b.gw - a.gx * b.px - a.gy * b.py - a.gz * b.pz - a.gw * b.pw
    );
}

MU_INLINE vec3s mu_ga_transform_vec3_flector(vec3s v, mu_ga_flector3 f)
{
    mat4s m = mu_ga_flector3_to_xform(f);
    vec4s r = mu_ga_mat4_mulv(m, mu_ga_vec4(v.x, v.y, v.z, 0.0f));
    return mu_ga_vec3(r.x, r.y, r.z);
}

MU_INLINE vec3s mu_ga_transform_point3_flector(vec3s p, mu_ga_flector3 f)
{
    mat4s m = mu_ga_flector3_to_xform(f);
    vec4s r = mu_ga_mat4_mulv(m, mu_ga_vec4(p.x, p.y, p.z, 1.0f));
    return mu_ga_vec3(r.x, r.y, r.z);
}

MU_INLINE mu_ga_flat_point3 mu_ga_transform_flat_point3_flector(mu_ga_flat_point3 p, mu_ga_flector3 f)
{
    vec3s ep = mu_ga_vec3(p.x / p.w, p.y / p.w, p.z / p.w);
    vec3s tp = mu_ga_transform_point3_flector(ep, f);
    return mu_ga_flat_point3_make(tp.x * p.w, tp.y * p.w, tp.z * p.w, p.w);
}

MU_INLINE mu_ga_line3 mu_ga_transform_line3_flector(mu_ga_line3 l, mu_ga_flector3 f)
{
    mat4s m = mu_ga_flector3_to_xform(f);
    vec4s v4 = mu_ga_mat4_mulv(m, mu_ga_vec4(l.vx, l.vy, l.vz, 0.0f));
    vec4s mm4 = mu_ga_mat4_mulv(m, mu_ga_vec4(l.mx, l.my, l.mz, 0.0f));
    vec3s v = mu_ga_vec3(v4.x, v4.y, v4.z);
    vec3s mm = mu_ga_vec3(mm4.x, mm4.y, mm4.z);
    vec3s t = mu_ga_vec3(m.col[3].x, m.col[3].y, m.col[3].z);
    vec3s mp = glms_vec3_add(mm, glms_vec3_cross(t, v));
    return mu_ga_line3_make(v.x, v.y, v.z, mp.x, mp.y, mp.z);
}

MU_INLINE mu_ga_plane3 mu_ga_transform_plane3_flector(mu_ga_plane3 g, mu_ga_flector3 f)
{
    mat4s m = mu_ga_flector3_to_xform(f);
    vec4s n4 = mu_ga_mat4_mulv(m, mu_ga_vec4(g.x, g.y, g.z, 0.0f));
    vec3s n = mu_ga_vec3(n4.x, n4.y, n4.z);
    float d = g.w - (n.x * m.col[3].x + n.y * m.col[3].y + n.z * m.col[3].z);
    return mu_ga_plane3_make(n.x, n.y, n.z, d);
}

/* ------------------------------------------------------------------------- */
/* conformal 2D */
/* ------------------------------------------------------------------------- */

typedef struct { float x, y, z, w; } mu_ga_round_point2;
typedef struct { float gx, gy, gz, px, py, pz; } mu_ga_dipole2;
typedef struct { float w, x, y, z; } mu_ga_circle2;

MU_INLINE mu_ga_round_point2 mu_ga_round_point2_make(float x, float y, float z, float w)
{
    mu_ga_round_point2 a = {x,y,z,w};
    return a;
}

MU_INLINE mu_ga_round_point2 mu_ga_round_point2_from_point(vec2s p)
{
    return mu_ga_round_point2_make(p.x, p.y, 1.0f, (p.x * p.x + p.y * p.y) * 0.5f);
}

MU_INLINE mu_ga_dipole2 mu_ga_dipole2_make(float gx, float gy, float gz, float px, float py, float pz)
{
    mu_ga_dipole2 d = {gx,gy,gz,px,py,pz};
    return d;
}

MU_INLINE mu_ga_circle2 mu_ga_circle2_make(float w, float x, float y, float z)
{
    mu_ga_circle2 c = {w,x,y,z};
    return c;
}

MU_INLINE mu_ga_circle2 mu_ga_dual_round_point2(mu_ga_round_point2 a)
{
    return mu_ga_circle2_make(-a.z, a.x, a.y, -a.w);
}

MU_INLINE mu_ga_dipole2 mu_ga_dual_dipole2(mu_ga_dipole2 d)
{
    return mu_ga_dipole2_make(d.gy, -d.gx, -d.pz, -d.py, d.px, -d.gz);
}

MU_INLINE mu_ga_round_point2 mu_ga_dual_circle2(mu_ga_circle2 c)
{
    return mu_ga_round_point2_make(c.x, c.y, -c.w, -c.z);
}

MU_INLINE float mu_ga_round_point2_squared_radius_norm(mu_ga_round_point2 a)
{
    return a.z * a.w * 2.0f - a.x * a.x - a.y * a.y;
}

MU_INLINE float mu_ga_dipole2_squared_radius_norm(mu_ga_dipole2 d)
{
    return d.pz * d.pz - d.gz * d.gz - (d.gx * d.py - d.gy * d.px) * 2.0f;
}

MU_INLINE float mu_ga_circle2_squared_radius_norm(mu_ga_circle2 c)
{
    return c.x * c.x + c.y * c.y - c.z * c.w * 2.0f;
}

MU_INLINE mu_ga_round_point2 mu_ga_center_round_point2(mu_ga_round_point2 a)
{
    return mu_ga_round_point2_make(a.x * a.z, a.y * a.z, a.z * a.z, a.z * a.w);
}

MU_INLINE mu_ga_round_point2 mu_ga_center_dipole2(mu_ga_dipole2 d)
{
    return mu_ga_round_point2_make(
        -d.gy * d.pz - d.gx * d.gz,
         d.gx * d.pz - d.gy * d.gz,
         d.gx * d.gx + d.gy * d.gy,
         d.pz * d.pz - d.gx * d.py + d.gy * d.px
    );
}

MU_INLINE mu_ga_round_point2 mu_ga_center_circle2(mu_ga_circle2 c)
{
    return mu_ga_round_point2_make(-c.x * c.w, -c.y * c.w, c.w * c.w, c.x * c.x + c.y * c.y - c.z * c.w);
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_center_dipole2(mu_ga_dipole2 d)
{
    return mu_ga_flat_point2_make(
        -d.gx * d.gz - d.gy * d.pz,
         d.gx * d.pz - d.gy * d.gz,
         d.gx * d.gx + d.gy * d.gy
    );
}

MU_INLINE mu_ga_circle2 mu_ga_container_round_point2(mu_ga_round_point2 a)
{
    return mu_ga_circle2_make(-a.z * a.z, a.x * a.z, a.y * a.z, a.z * a.w - a.x * a.x - a.y * a.y);
}

MU_INLINE mu_ga_circle2 mu_ga_container_dipole2(mu_ga_dipole2 d)
{
    return mu_ga_circle2_make(
        -d.gx * d.gx - d.gy * d.gy,
        -d.gx * d.gz - d.gy * d.pz,
         d.gx * d.pz - d.gy * d.gz,
         d.gy * d.px - d.gx * d.py - d.gz * d.gz
    );
}

MU_INLINE mu_ga_dipole2 mu_ga_partner_dipole2(mu_ga_dipole2 d)
{
    float gzpz = d.gz * d.pz;
    float gxy2 = -d.gx * d.gx - d.gy * d.gy;
    float f = d.pz * d.pz - d.gz * d.gz + d.gy * d.px - d.gx * d.py;

    return mu_ga_dipole2_make(
        gxy2 * d.gx,
        gxy2 * d.gy,
        gxy2 * d.gz,
        gzpz * d.gx + f * d.gy,
        gzpz * d.gy - f * d.gx,
        gxy2 * d.pz
    );
}

/* join */
MU_INLINE mu_ga_dipole2 mu_ga_wedge_round_point2_round_point2(mu_ga_round_point2 a, mu_ga_round_point2 b)
{
    return mu_ga_dipole2_make(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
        a.w * b.x - a.x * b.w,
        a.w * b.y - a.y * b.w,
        a.w * b.z - a.z * b.w
    );
}

MU_INLINE mu_ga_line2 mu_ga_wedge_flat_point2_round_point2(mu_ga_flat_point2 p, mu_ga_round_point2 a)
{
    return mu_ga_line2_make(
        p.y * a.z - p.z * a.y,
        p.z * a.x - p.x * a.z,
        p.x * a.y - p.y * a.x
    );
}

MU_INLINE mu_ga_line2 mu_ga_wedge_point2_round_point2(vec2s p, mu_ga_round_point2 a)
{
    return mu_ga_line2_make(
        p.y * a.z - a.y,
        a.x - p.x * a.z,
        p.x * a.y - p.y * a.x
    );
}

MU_INLINE mu_ga_circle2 mu_ga_wedge_dipole2_round_point2(mu_ga_dipole2 d, mu_ga_round_point2 a)
{
    return mu_ga_circle2_make(
        -d.gx * a.x - d.gy * a.y - d.gz * a.z,
         d.py * a.z - d.pz * a.y + d.gx * a.w,
         d.pz * a.x - d.px * a.z + d.gy * a.w,
         d.px * a.y - d.py * a.x + d.gz * a.w
    );
}

/* meet */
MU_INLINE mu_ga_dipole2 mu_ga_antiwedge_circle2_circle2(mu_ga_circle2 c, mu_ga_circle2 o)
{
    return mu_ga_dipole2_make(
        c.x * o.w - c.w * o.x,
        c.y * o.w - c.w * o.y,
        c.z * o.w - c.w * o.z,
        c.z * o.y - c.y * o.z,
        c.x * o.z - c.z * o.x,
        c.y * o.x - c.x * o.y
    );
}

MU_INLINE mu_ga_dipole2 mu_ga_antiwedge_circle2_line2(mu_ga_circle2 c, mu_ga_line2 g)
{
    return mu_ga_dipole2_make(
        -c.w * g.x,
        -c.w * g.y,
        -c.w * g.z,
         c.z * g.y - c.y * g.z,
         c.x * g.z - c.z * g.x,
         c.y * g.x - c.x * g.y
    );
}

MU_INLINE mu_ga_round_point2 mu_ga_antiwedge_circle2_dipole2(mu_ga_circle2 c, mu_ga_dipole2 d)
{
    return mu_ga_round_point2_make(
        c.z * d.gy - c.y * d.gz + c.w * d.px,
        c.x * d.gz - c.z * d.gx + c.w * d.py,
        c.y * d.gx - c.x * d.gy + c.w * d.pz,
       -c.x * d.px - c.y * d.py - c.z * d.pz
    );
}

MU_INLINE mu_ga_round_point2 mu_ga_antiwedge_line2_dipole2(mu_ga_line2 g, mu_ga_dipole2 d)
{
    return mu_ga_round_point2_make(
        g.z * d.gy - g.y * d.gz,
        g.x * d.gz - g.z * d.gx,
        g.y * d.gx - g.x * d.gy,
       -g.x * d.px - g.y * d.py - g.z * d.pz
    );
}

MU_INLINE mu_ga_round_point2 mu_ga_antiwedge_circle2_flat_point2(mu_ga_circle2 c, mu_ga_flat_point2 p)
{
    return mu_ga_round_point2_make(c.w * p.x, c.w * p.y, c.w * p.z, -c.x * p.x - c.y * p.y - c.z * p.z);
}

MU_INLINE mu_ga_round_point2 mu_ga_antiwedge_circle2_point2(mu_ga_circle2 c, vec2s p)
{
    return mu_ga_round_point2_make(c.w * p.x, c.w * p.y, c.w, -c.x * p.x - c.y * p.y - c.z);
}

/* ------------------------------------------------------------------------- */
/* conformal 3D */
/* ------------------------------------------------------------------------- */

typedef struct { float x, y, z, w, u; } mu_ga_round_point3;
typedef struct { float vx, vy, vz, mx, my, mz, px, py, pz, pw; } mu_ga_dipole3;
typedef struct { float gx, gy, gz, gw, vx, vy, vz, mx, my, mz; } mu_ga_circle3;
typedef struct { float u, x, y, z, w; } mu_ga_sphere3;

MU_INLINE mu_ga_round_point3 mu_ga_round_point3_make(float x, float y, float z, float w, float u)
{
    mu_ga_round_point3 a = {x,y,z,w,u};
    return a;
}

MU_INLINE mu_ga_round_point3 mu_ga_round_point3_from_point(vec3s p)
{
    return mu_ga_round_point3_make(p.x, p.y, p.z, 1.0f, (p.x*p.x + p.y*p.y + p.z*p.z) * 0.5f);
}

MU_INLINE mu_ga_dipole3 mu_ga_dipole3_make(float vx,float vy,float vz,float mx,float my,float mz,float px,float py,float pz,float pw)
{
    mu_ga_dipole3 d = {vx,vy,vz,mx,my,mz,px,py,pz,pw};
    return d;
}

MU_INLINE mu_ga_circle3 mu_ga_circle3_make(float gx,float gy,float gz,float gw,float vx,float vy,float vz,float mx,float my,float mz)
{
    mu_ga_circle3 c = {gx,gy,gz,gw,vx,vy,vz,mx,my,mz};
    return c;
}

MU_INLINE mu_ga_sphere3 mu_ga_sphere3_make(float u,float x,float y,float z,float w)
{
    mu_ga_sphere3 s = {u,x,y,z,w};
    return s;
}

MU_INLINE mu_ga_sphere3 mu_ga_dual_round_point3(mu_ga_round_point3 a)
{
    return mu_ga_sphere3_make(-a.w, a.x, a.y, a.z, -a.u);
}

MU_INLINE mu_ga_circle3 mu_ga_dual_dipole3(mu_ga_dipole3 d)
{
    return mu_ga_circle3_make(-d.vx, -d.vy, -d.vz, d.pw, -d.mx, -d.my, -d.mz, -d.px, -d.py, -d.pz);
}

MU_INLINE mu_ga_dipole3 mu_ga_dual_circle3(mu_ga_circle3 c)
{
    return mu_ga_dipole3_make(c.gx, c.gy, c.gz, c.vx, c.vy, c.vz, c.mx, c.my, c.mz, -c.gw);
}

MU_INLINE mu_ga_round_point3 mu_ga_dual_sphere3(mu_ga_sphere3 s)
{
    return mu_ga_round_point3_make(-s.x, -s.y, -s.z, s.u, s.w);
}

MU_INLINE mu_ga_round_point3 mu_ga_center_dipole3(mu_ga_dipole3 d)
{
    return mu_ga_round_point3_make(
        d.vy * d.mz - d.vz * d.my + d.vx * d.pw,
        d.vz * d.mx - d.vx * d.mz + d.vy * d.pw,
        d.vx * d.my - d.vy * d.mx + d.vz * d.pw,
        d.vx * d.vx + d.vy * d.vy + d.vz * d.vz,
        d.pw * d.pw - d.vx * d.px - d.vy * d.py - d.vz * d.pz
    );
}

MU_INLINE mu_ga_round_point3 mu_ga_center_circle3(mu_ga_circle3 c)
{
    return mu_ga_round_point3_make(
        c.gy * c.vz - c.gz * c.vy - c.gx * c.gw,
        c.gz * c.vx - c.gx * c.vz - c.gy * c.gw,
        c.gx * c.vy - c.gy * c.vx - c.gz * c.gw,
        c.gx * c.gx + c.gy * c.gy + c.gz * c.gz,
        c.vx * c.vx + c.vy * c.vy + c.vz * c.vz + c.gx * c.mx + c.gy * c.my + c.gz * c.mz
    );
}

MU_INLINE mu_ga_round_point3 mu_ga_center_sphere3(mu_ga_sphere3 s)
{
    return mu_ga_round_point3_make(-s.x * s.u, -s.y * s.u, -s.z * s.u, s.u * s.u, s.x * s.x + s.y * s.y + s.z * s.z - s.w * s.u);
}

MU_INLINE mu_ga_flat_point3 mu_ga_flat_center_dipole3(mu_ga_dipole3 d)
{
    return mu_ga_flat_point3_make(
        d.vy * d.mz - d.vz * d.my + d.vx * d.pw,
        d.vz * d.mx - d.vx * d.mz + d.vy * d.pw,
        d.vx * d.my - d.vy * d.mx + d.vz * d.pw,
        d.vx * d.vx + d.vy * d.vy + d.vz * d.vz
    );
}

MU_INLINE mu_ga_flat_point3 mu_ga_flat_center_circle3(mu_ga_circle3 c)
{
    return mu_ga_flat_point3_make(
        c.gy * c.vz - c.gz * c.vy - c.gx * c.gw,
        c.gz * c.vx - c.gx * c.vz - c.gy * c.gw,
        c.gx * c.vy - c.gy * c.vx - c.gz * c.gw,
        c.gx * c.gx + c.gy * c.gy + c.gz * c.gz
    );
}

MU_INLINE mu_ga_sphere3 mu_ga_container_dipole3(mu_ga_dipole3 d)
{
    return mu_ga_sphere3_make(
        d.vx * d.vx + d.vy * d.vy + d.vz * d.vz,
        d.vz * d.my - d.vy * d.mz - d.vx * d.pw,
        d.vx * d.mz - d.vz * d.mx - d.vy * d.pw,
        d.vy * d.mx - d.vx * d.my - d.vz * d.pw,
        d.mx * d.mx + d.my * d.my + d.mz * d.mz + d.vx * d.px + d.vy * d.py + d.vz * d.pz
    );
}

MU_INLINE mu_ga_sphere3 mu_ga_container_circle3(mu_ga_circle3 c)
{
    return mu_ga_sphere3_make(
        -c.gx * c.gx - c.gy * c.gy - c.gz * c.gz,
         c.gy * c.vz - c.gz * c.vy - c.gw * c.gx,
         c.gz * c.vx - c.gx * c.vz - c.gw * c.gy,
         c.gx * c.vy - c.gy * c.vx - c.gw * c.gz,
         c.gx * c.mx + c.gy * c.my + c.gz * c.mz - c.gw * c.gw
    );
}

MU_INLINE float mu_ga_round_point3_squared_radius_norm(mu_ga_round_point3 a)
{
    return a.w * a.u * 2.0f - a.x * a.x - a.y * a.y - a.z * a.z;
}

MU_INLINE float mu_ga_dipole3_squared_radius_norm(mu_ga_dipole3 d)
{
    return d.pw * d.pw - d.mx*d.mx - d.my*d.my - d.mz*d.mz - (d.px*d.vx + d.py*d.vy + d.pz*d.vz) * 2.0f;
}

MU_INLINE float mu_ga_circle3_squared_radius_norm(mu_ga_circle3 c)
{
    return c.vx*c.vx + c.vy*c.vy + c.vz*c.vz + (c.gx*c.mx + c.gy*c.my + c.gz*c.mz) * 2.0f - c.gw*c.gw;
}

MU_INLINE float mu_ga_sphere3_squared_radius_norm(mu_ga_sphere3 s)
{
    return s.x*s.x + s.y*s.y + s.z*s.z - s.w*s.u*2.0f;
}

/* join */
MU_INLINE mu_ga_dipole3 mu_ga_wedge_round_point3_round_point3(mu_ga_round_point3 a, mu_ga_round_point3 b)
{
    return mu_ga_dipole3_make(
        a.w*b.x - a.x*b.w,
        a.w*b.y - a.y*b.w,
        a.w*b.z - a.z*b.w,
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x,
        a.x*b.u - a.u*b.x,
        a.y*b.u - a.u*b.y,
        a.z*b.u - a.u*b.z,
        a.w*b.u - a.u*b.w
    );
}

MU_INLINE mu_ga_line3 mu_ga_wedge_flat_point3_round_point3(mu_ga_flat_point3 p, mu_ga_round_point3 a)
{
    return mu_ga_line3_make(
        p.x*a.w - p.w*a.x,
        p.y*a.w - p.w*a.y,
        p.z*a.w - p.w*a.z,
        p.z*a.y - p.y*a.z,
        p.x*a.z - p.z*a.x,
        p.y*a.x - p.x*a.y
    );
}

MU_INLINE mu_ga_line3 mu_ga_wedge_point3_round_point3(vec3s p, mu_ga_round_point3 a)
{
    return mu_ga_line3_make(
        p.x*a.w - a.x,
        p.y*a.w - a.y,
        p.z*a.w - a.z,
        p.z*a.y - p.y*a.z,
        p.x*a.z - p.z*a.x,
        p.y*a.x - p.x*a.y
    );
}

MU_INLINE mu_ga_circle3 mu_ga_wedge_dipole3_round_point3(mu_ga_dipole3 d, mu_ga_round_point3 a)
{
    return mu_ga_circle3_make(
        d.vy*a.z - d.vz*a.y + d.mx*a.w,
        d.vz*a.x - d.vx*a.z + d.my*a.w,
        d.vx*a.y - d.vy*a.x + d.mz*a.w,
       -d.mx*a.x - d.my*a.y - d.mz*a.z,

        d.px*a.w - d.pw*a.x + d.vx*a.u,
        d.py*a.w - d.pw*a.y + d.vy*a.u,
        d.pz*a.w - d.pw*a.z + d.vz*a.u,

        d.pz*a.y - d.py*a.z + d.mx*a.u,
        d.px*a.z - d.pz*a.x + d.my*a.u,
        d.py*a.x - d.px*a.y + d.mz*a.u
    );
}

MU_INLINE mu_ga_plane3 mu_ga_wedge_line3_round_point3(mu_ga_line3 l, mu_ga_round_point3 a)
{
    return mu_ga_plane3_make(
        l.vz*a.y - l.vy*a.z - l.mx*a.w,
        l.vx*a.z - l.vz*a.x - l.my*a.w,
        l.vy*a.x - l.vx*a.y - l.mz*a.w,
        l.mx*a.x + l.my*a.y + l.mz*a.z
    );
}

MU_INLINE mu_ga_plane3 mu_ga_wedge_dipole3_flat_point3(mu_ga_dipole3 d, mu_ga_flat_point3 p)
{
    return mu_ga_plane3_make(
        d.vy*p.z - d.vz*p.y + d.mx*p.w,
        d.vz*p.x - d.vx*p.z + d.my*p.w,
        d.vx*p.y - d.vy*p.x + d.mz*p.w,
       -d.mx*p.x - d.my*p.y - d.mz*p.z
    );
}

MU_INLINE mu_ga_plane3 mu_ga_wedge_dipole3_point3(mu_ga_dipole3 d, vec3s p)
{
    return mu_ga_plane3_make(
        d.vy*p.z - d.vz*p.y + d.mx,
        d.vz*p.x - d.vx*p.z + d.my,
        d.vx*p.y - d.vy*p.x + d.mz,
       -d.mx*p.x - d.my*p.y - d.mz*p.z
    );
}

MU_INLINE mu_ga_sphere3 mu_ga_wedge_circle3_round_point3(mu_ga_circle3 c, mu_ga_round_point3 a)
{
    return mu_ga_sphere3_make(
        -c.gx*a.x - c.gy*a.y - c.gz*a.z - c.gw*a.w,
         c.vz*a.y - c.vy*a.z - c.mx*a.w + c.gx*a.u,
         c.vx*a.z - c.vz*a.x - c.my*a.w + c.gy*a.u,
         c.vy*a.x - c.vx*a.y - c.mz*a.w + c.gz*a.u,
         c.mx*a.x + c.my*a.y + c.mz*a.z + c.gw*a.u
    );
}

MU_INLINE mu_ga_sphere3 mu_ga_wedge_dipole3_dipole3(mu_ga_dipole3 d, mu_ga_dipole3 f)
{
    return mu_ga_sphere3_make(
        -d.mx*f.vx - d.my*f.vy - d.mz*f.vz - d.vx*f.mx - d.vy*f.my - d.vz*f.mz,
         d.pz*f.vy - d.py*f.vz + d.vy*f.pz - d.vz*f.py + d.mx*f.pw + d.pw*f.mx,
         d.px*f.vz - d.pz*f.vx + d.vz*f.px - d.vx*f.pz + d.my*f.pw + d.pw*f.my,
         d.py*f.vx - d.px*f.vy + d.vx*f.py - d.vy*f.px + d.mz*f.pw + d.pw*f.mz,
        -d.mx*f.px - d.my*f.py - d.mz*f.pz - d.px*f.mx - d.py*f.my - d.pz*f.mz
    );
}

/* meet */
MU_INLINE mu_ga_circle3 mu_ga_antiwedge_sphere3_sphere3(mu_ga_sphere3 s, mu_ga_sphere3 t)
{
    return mu_ga_circle3_make(
        s.u*t.x - s.x*t.u,
        s.u*t.y - s.y*t.u,
        s.u*t.z - s.z*t.u,
        s.u*t.w - s.w*t.u,
        s.z*t.y - s.y*t.z,
        s.x*t.z - s.z*t.x,
        s.y*t.x - s.x*t.y,
        s.x*t.w - s.w*t.x,
        s.y*t.w - s.w*t.y,
        s.z*t.w - s.w*t.z
    );
}

MU_INLINE mu_ga_circle3 mu_ga_antiwedge_sphere3_plane3(mu_ga_sphere3 s, mu_ga_plane3 g)
{
    return mu_ga_circle3_make(
        s.u*g.x, s.u*g.y, s.u*g.z, s.u*g.w,
        s.z*g.y - s.y*g.z,
        s.x*g.z - s.z*g.x,
        s.y*g.x - s.x*g.y,
        s.x*g.w - s.w*g.x,
        s.y*g.w - s.w*g.y,
        s.z*g.w - s.w*g.z
    );
}

MU_INLINE mu_ga_dipole3 mu_ga_antiwedge_sphere3_circle3(mu_ga_sphere3 s, mu_ga_circle3 c)
{
    return mu_ga_dipole3_make(
        s.y*c.gz - s.z*c.gy + s.u*c.vx,
        s.z*c.gx - s.x*c.gz + s.u*c.vy,
        s.x*c.gy - s.y*c.gx + s.u*c.vz,

        s.w*c.gx - s.x*c.gw + s.u*c.mx,
        s.w*c.gy - s.y*c.gw + s.u*c.my,
        s.w*c.gz - s.z*c.gw + s.u*c.mz,

        s.z*c.my - s.y*c.mz + s.w*c.vx,
        s.x*c.mz - s.z*c.mx + s.w*c.vy,
        s.y*c.mx - s.x*c.my + s.w*c.vz,
       -s.x*c.vx - s.y*c.vy - s.z*c.vz
    );
}

MU_INLINE mu_ga_dipole3 mu_ga_antiwedge_plane3_circle3(mu_ga_plane3 g, mu_ga_circle3 c)
{
    return mu_ga_dipole3_make(
        g.y*c.gz - g.z*c.gy,
        g.z*c.gx - g.x*c.gz,
        g.x*c.gy - g.y*c.gx,

        g.w*c.gx - g.x*c.gw,
        g.w*c.gy - g.y*c.gw,
        g.w*c.gz - g.z*c.gw,

        g.z*c.my - g.y*c.mz + g.w*c.vx,
        g.x*c.mz - g.z*c.mx + g.w*c.vy,
        g.y*c.mx - g.x*c.my + g.w*c.vz,
       -g.x*c.vx - g.y*c.vy - g.z*c.vz
    );
}

MU_INLINE mu_ga_dipole3 mu_ga_antiwedge_sphere3_line3(mu_ga_sphere3 s, mu_ga_line3 l)
{
    return mu_ga_dipole3_make(
        s.u*l.vx, s.u*l.vy, s.u*l.vz,
        s.u*l.mx, s.u*l.my, s.u*l.mz,
        s.z*l.my - s.y*l.mz + s.w*l.vx,
        s.x*l.mz - s.z*l.mx + s.w*l.vy,
        s.y*l.mx - s.x*l.my + s.w*l.vz,
       -s.x*l.vx - s.y*l.vy - s.z*l.vz
    );
}

MU_INLINE mu_ga_round_point3 mu_ga_antiwedge_circle3_circle3(mu_ga_circle3 c, mu_ga_circle3 o)
{
    return mu_ga_round_point3_make(
        c.gz*o.my - c.gy*o.mz + c.my*o.gz - c.mz*o.gy + c.gw*o.vx + c.vx*o.gw,
        c.gx*o.mz - c.gz*o.mx + c.mz*o.gx - c.mx*o.gz + c.gw*o.vy + c.vy*o.gw,
        c.gy*o.mx - c.gx*o.my + c.mx*o.gy - c.my*o.gx + c.gw*o.vz + c.vz*o.gw,
       -c.gx*o.vx - c.gy*o.vy - c.gz*o.vz - c.vx*o.gx - c.vy*o.gy - c.vz*o.gz,
       -c.mx*o.vx - c.my*o.vy - c.mz*o.vz - c.vx*o.mx - c.vy*o.my - c.vz*o.mz
    );
}

MU_INLINE mu_ga_round_point3 mu_ga_antiwedge_circle3_line3(mu_ga_circle3 c, mu_ga_line3 l)
{
    return mu_ga_round_point3_make(
        c.gz*l.my - c.gy*l.mz + c.gw*l.vx,
        c.gx*l.mz - c.gz*l.mx + c.gw*l.vy,
        c.gy*l.mx - c.gx*l.my + c.gw*l.vz,
       -c.gx*l.vx - c.gy*l.vy - c.gz*l.vz,
       -c.mx*l.vx - c.my*l.vy - c.mz*l.vz - c.vx*l.mx - c.vy*l.my - c.vz*l.mz
    );
}

MU_INLINE mu_ga_round_point3 mu_ga_antiwedge_sphere3_dipole3(mu_ga_sphere3 s, mu_ga_dipole3 d)
{
    return mu_ga_round_point3_make(
        s.y*d.mz - s.z*d.my + s.u*d.px - s.w*d.vx,
        s.z*d.mx - s.x*d.mz + s.u*d.py - s.w*d.vy,
        s.x*d.my - s.y*d.mx + s.u*d.pz - s.w*d.vz,
        s.x*d.vx + s.y*d.vy + s.z*d.vz + s.u*d.pw,
       -s.x*d.px - s.y*d.py - s.z*d.pz - s.w*d.pw
    );
}

MU_INLINE mu_ga_round_point3 mu_ga_antiwedge_plane3_dipole3(mu_ga_plane3 g, mu_ga_dipole3 d)
{
    return mu_ga_round_point3_make(
        g.y*d.mz - g.z*d.my - g.w*d.vx,
        g.z*d.mx - g.x*d.mz - g.w*d.vy,
        g.x*d.my - g.y*d.mx - g.w*d.vz,
        g.x*d.vx + g.y*d.vy + g.z*d.vz,
       -g.x*d.px - g.y*d.py - g.z*d.pz - g.w*d.pw
    );
}

MU_INLINE mu_ga_round_point3 mu_ga_antiwedge_sphere3_flat_point3(mu_ga_sphere3 s, mu_ga_flat_point3 p)
{
    return mu_ga_round_point3_make(s.u*p.x, s.u*p.y, s.u*p.z, s.u*p.w, -s.x*p.x - s.y*p.y - s.z*p.z - s.w*p.w);
}

MU_INLINE mu_ga_round_point3 mu_ga_antiwedge_sphere3_point3(mu_ga_sphere3 s, vec3s p)
{
    return mu_ga_round_point3_make(s.u*p.x, s.u*p.y, s.u*p.z, s.u, -s.x*p.x - s.y*p.y - s.z*p.z - s.w);
}

/* ------------------------------------------------------------------------- */
/* tiny usage examples */
/* ------------------------------------------------------------------------- */

/*
    Example A: rigid 3D line from two points and projection

        vec3s p0 = mu_ga_vec3(0.0f, 0.0f, 0.0f);
        vec3s p1 = mu_ga_vec3(1.0f, 1.0f, 0.0f);
        mu_ga_line3 l = mu_ga_wedge_point3_point3(p0, p1);

        vec3s q = mu_ga_vec3(0.4f, 2.0f, 0.0f);
        vec3s pr = mu_ga_project_point3_line3(q, mu_ga_line3_make(
                              l.vx, l.vy, l.vz, l.mx, l.my, l.mz));

    Example B: 3D motor transform (rotation + translation)

        vec3s axis = mu_ga_vec3(0.0f, 0.0f, 1.0f);
        mu_ga_motor3 R = mu_ga_motor3_make_rotation(1.0f, axis);
        mu_ga_motor3 T = mu_ga_motor3_make_translation(mu_ga_vec3(3.0f, 0.0f, 0.0f));
        mu_ga_motor3 Q = mu_ga_motor3_mul(T, R);

        vec3s p = mu_ga_vec3(1.0f, 0.0f, 0.0f);
        vec3s p2 = mu_ga_transform_point3_motor(p, Q);

    Example C: conformal meet (sphere ^ plane -> circle)

        mu_ga_sphere3 s = mu_ga_sphere3_make(-1, 0,0,0, -4);  // centered sphere form
        mu_ga_plane3  g = mu_ga_plane3_make(0,1,0,0);          // y = 0 plane
        mu_ga_circle3 c = mu_ga_antiwedge_sphere3_plane3(s, g);
*/

#ifdef __cplusplus
}
#endif

#endif
