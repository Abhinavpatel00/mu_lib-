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
    MATH BACKEND
    --------------------------------------------------------------------------
    This header used to require cglm for mu_vec2/mu_vec3/mu_vec4 and for
    dot/cross/norm. It now uses mu_math.h, which is part of this library: the
    only thing it ever needed from cglm was the three vector structs and their
    members, and mu_vec2/mu_vec3/mu_vec4 are those structs. No external include,
    no CGLM_USE_ANONYMOUS_STRUCT dance, and the types interop with the rest of
    the library for free.
*/

#include <math.h>
#include "mu_macros.h"
#include "mu_math.h"

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
MU_INLINE void mu_ga_vec2(mu_vec2 out, float x, float y)
{
    mu_vec2_make(out, x, y);
}

MU_INLINE void mu_ga_vec3(mu_vec3 out, float x, float y, float z)
{
    mu_vec3_make(out, x, y, z);
}

MU_INLINE void mu_ga_vec4(mu_vec4 out, float x, float y, float z, float w)
{
    mu_vec4_make(out, x, y, z, w);
}

MU_INLINE void mu_ga_cos_sin_half(float angle, float *c, float *s)
{
    float h = angle * 0.5f;
    *c = cosf(h);
    *s = sinf(h);
}

/* ------------------------------------------------------------------------- */
/* basic vectors */

MU_INLINE void mu_ga_mat3_mulv(mu_vec3 out, mu_mat3 m, mu_vec3 v)
{
    float y = m[1] * v[0] + m[4] * v[1] + m[7] * v[2];
    float z = m[2] * v[0] + m[5] * v[1] + m[8] * v[2];
    float x = m[0] * v[0] + m[3] * v[1] + m[6] * v[2];
    mu_vec3_make(out, x, y, z);
}

MU_INLINE void mu_ga_mat4_mulv(mu_vec4 out, mu_mat4 m, mu_vec4 v)
{
    float x = m[0] * v[0] + m[4] * v[1] + m[8]  * v[2] + m[12] * v[3];
    float y = m[1] * v[0] + m[5] * v[1] + m[9]  * v[2] + m[13] * v[3];
    float z = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3];
    float w = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3];
    mu_vec4_make(out, x, y, z, w);
}

/*
   In this representation, bivectors are encoded as a 3-float pseudovector.
   This is the same memory layout used by many PGA implementations:

       B = x e23 + y e31 + z e12

   so we can reuse cross-product arithmetic for wedge/antiwedge formulas.
*/
MU_INLINE void mu_ga_wedge_v3_v3(mu_vec3 out, mu_vec3 a, mu_vec3 b)
{
    mu_vec3_cross(out, a, b);
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

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_from_point(mu_vec2 p)
{
    return mu_ga_flat_point2_make(p[0], p[1], 1.0f);
}

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_from_dir(mu_vec2 v)
{
    return mu_ga_flat_point2_make(v[0], v[1], 0.0f);
}

MU_INLINE mu_ga_line2 mu_ga_line2_make(float x, float y, float z)
{
    mu_ga_line2 g = {x,y,z};
    return g;
}

MU_INLINE mu_ga_line2 mu_ga_line2_from_points(mu_vec2 p, mu_vec2 q)
{
    return mu_ga_line2_make(p[1] - q[1], q[0] - p[0], p[0] * q[1] - p[1] * q[0]);
}

MU_INLINE mu_ga_line2 mu_ga_line2_from_point_dir(mu_vec2 p, mu_vec2 v)
{
    return mu_ga_line2_make(-v[1], v[0], p[0] * v[1] - p[1] * v[0]);
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
MU_INLINE void mu_ga_line2_attitude(mu_vec2 out, mu_ga_line2 g)
{
    mu_ga_vec2(out, g.y, -g.x);
}

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

MU_INLINE mu_ga_flat_point2 mu_ga_flat_point2_translate(mu_ga_flat_point2 p, mu_vec2 t)
{
    return mu_ga_flat_point2_make(p.x + t[0] * p.z, p.y + t[1] * p.z, p.z);
}

MU_INLINE mu_ga_line2 mu_ga_line2_translate(mu_ga_line2 g, mu_vec2 t)
{
    return mu_ga_line2_make(g.x, g.y, g.z - g.x * t[0] - g.y * t[1]);
}

MU_INLINE mu_ga_line2 mu_ga_wedge_flat_point2_flat_point2(mu_ga_flat_point2 p, mu_ga_flat_point2 q)
{
    return mu_ga_line2_make(
        p.y * q.z - p.z * q.y,
        p.z * q.x - p.x * q.z,
        p.x * q.y - p.y * q.x
    );
}

MU_INLINE mu_ga_line2 mu_ga_wedge_point2_point2(mu_vec2 p, mu_vec2 q)
{
    return mu_ga_line2_make(p[1] - q[1], q[0] - p[0], p[0] * q[1] - p[1] * q[0]);
}

MU_INLINE mu_ga_line2 mu_ga_wedge_point2_vec2(mu_vec2 p, mu_vec2 v)
{
    return mu_ga_line2_make(-v[1], v[0], p[0] * v[1] - p[1] * v[0]);
}

MU_INLINE mu_ga_flat_point2 mu_ga_antiwedge_line2_line2(mu_ga_line2 g, mu_ga_line2 h)
{
    return mu_ga_flat_point2_make(
        g.z * h.y - g.y * h.z,
        g.x * h.z - g.z * h.x,
        g.y * h.x - g.x * h.y
    );
}

MU_INLINE float mu_ga_antiwedge_point2_line2(mu_vec2 p, mu_ga_line2 g)
{
    return -(p[0] * g.x + p[1] * g.y + g.z);
}

/*
    Projection on unitized line

        dist = n . p + c
        p_proj = p - n * dist

    Here antiwedge(point,line) = -(n.p + c), so
        p_proj = p + n * antiwedge(point,line)
*/
MU_INLINE void mu_ga_project_point2_line2(mu_vec2 out, mu_vec2 p, mu_ga_line2 g)
{
    float a = mu_ga_antiwedge_point2_line2(p, g);
    mu_ga_vec2(out, p[0] + g.x * a, p[1] + g.y * a);
}

MU_INLINE mu_ga_line2 mu_ga_antiproject_line2_point2(mu_ga_line2 g, mu_vec2 p)
{
    return mu_ga_line2_make(g.x, g.y, -p[0] * g.x - p[1] * g.y);
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

MU_INLINE mu_ga_flat_point3 mu_ga_flat_point3_from_point(mu_vec3 p)
{
    return mu_ga_flat_point3_make(p[0], p[1], p[2], 1.0f);
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

MU_INLINE mu_ga_line3 mu_ga_wedge_point3_point3(mu_vec3 p, mu_vec3 q)
{
    return mu_ga_line3_make(
        q[0] - p[0],
        q[1] - p[1],
        q[2] - p[2],
        p[1] * q[2] - p[2] * q[1],
        p[2] * q[0] - p[0] * q[2],
        p[0] * q[1] - p[1] * q[0]
    );
}

MU_INLINE mu_ga_line3 mu_ga_wedge_point3_vec3(mu_vec3 p, mu_vec3 v)
{
    return mu_ga_line3_make(
        v[0], v[1], v[2],
        p[1] * v[2] - p[2] * v[1],
        p[2] * v[0] - p[0] * v[2],
        p[0] * v[1] - p[1] * v[0]
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

MU_INLINE mu_ga_plane3 mu_ga_wedge_line3_point3(mu_ga_line3 l, mu_vec3 p)
{
    return mu_ga_plane3_make(
        l.vy * p[2] - l.vz * p[1] + l.mx,
        l.vz * p[0] - l.vx * p[2] + l.my,
        l.vx * p[1] - l.vy * p[0] + l.mz,
       -l.mx * p[0] - l.my * p[1] - l.mz * p[2]
    );
}

MU_INLINE mu_ga_plane3 mu_ga_wedge_line3_vec3(mu_ga_line3 l, mu_vec3 v)
{
    return mu_ga_plane3_make(
        l.vy * v[2] - l.vz * v[1],
        l.vz * v[0] - l.vx * v[2],
        l.vx * v[1] - l.vy * v[0],
       -(l.mx * v[0] + l.my * v[1] + l.mz * v[2])
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

MU_INLINE float mu_ga_antiwedge_point3_plane3(mu_vec3 p, mu_ga_plane3 g)
{
    return p[0] * g.x + p[1] * g.y + p[2] * g.z + g.w;
}

MU_INLINE float mu_ga_antiwedge_vec3_plane3(mu_vec3 v, mu_ga_plane3 g)
{
    return v[0] * g.x + v[1] * g.y + v[2] * g.z;
}

MU_INLINE mu_ga_flat_point3 mu_ga_flat_point3_translate(mu_ga_flat_point3 p, mu_vec3 t)
{
    return mu_ga_flat_point3_make(p.x + t[0] * p.w, p.y + t[1] * p.w, p.z + t[2] * p.w, p.w);
}

MU_INLINE mu_ga_line3 mu_ga_line3_translate(mu_ga_line3 l, mu_vec3 t)
{
    mu_vec3 v = {l.vx, l.vy, l.vz};
    mu_vec3 m = {l.mx, l.my, l.mz};
    mu_vec3 cross0;
    mu_vec3_cross(cross0, t, v);
    mu_vec3 mm;
    mu_vec3_add(mm, m, cross0);
    return mu_ga_line3_make(l.vx, l.vy, l.vz, mm[0], mm[1], mm[2]);
}

MU_INLINE mu_ga_plane3 mu_ga_plane3_translate(mu_ga_plane3 g, mu_vec3 t)
{
    return mu_ga_plane3_make(g.x, g.y, g.z, g.w - g.x * t[0] - g.y * t[1] - g.z * t[2]);
}

MU_INLINE void mu_ga_project_point3_line3(mu_vec3 out, mu_vec3 p, mu_ga_line3 l)
{
    mu_vec3 v = {l.vx, l.vy, l.vz};
    mu_vec3 m = {l.mx, l.my, l.mz};
    float d = mu_vec3_dot(v, p);
    mu_vec3 c;
    mu_vec3_cross(c, v, m);
    mu_ga_vec3(out, d * v[0] + c[0], d * v[1] + c[1], d * v[2] + c[2]);
}

MU_INLINE void mu_ga_project_point3_plane3(mu_vec3 out, mu_vec3 p, mu_ga_plane3 g)
{
    float dist = mu_ga_antiwedge_point3_plane3(p, g);
    mu_ga_vec3(out, p[0] - g.x * dist, p[1] - g.y * dist, p[2] - g.z * dist);
}

MU_INLINE mu_ga_line3 mu_ga_project_line3_plane3(mu_ga_line3 l, mu_ga_plane3 g)
{
    mu_vec3 n = {g.x, g.y, g.z};
    mu_vec3 v = {l.vx, l.vy, l.vz};
    mu_vec3 m = {l.mx, l.my, l.mz};

    mu_vec3 scaled2;
    mu_vec3_scale(scaled2, n, mu_vec3_dot(n, v));
    mu_vec3 vproj;
    mu_vec3_sub(vproj, v, scaled2);
    mu_vec3 scaled4;
    mu_vec3_scale(scaled4, n, mu_vec3_dot(n, m));
    mu_vec3 scaled5;
    mu_vec3_scale(scaled5, vproj, g.w);
    mu_vec3 mproj;
    mu_vec3_sub(mproj, scaled4, scaled5);

    return mu_ga_line3_make(vproj[0], vproj[1], vproj[2], mproj[0], mproj[1], mproj[2]);
}

MU_INLINE mu_ga_line3 mu_ga_antiproject_line3_point3(mu_ga_line3 l, mu_vec3 p)
{
    return mu_ga_wedge_point3_vec3(p, (mu_vec3){l.vx, l.vy, l.vz});
}

MU_INLINE mu_ga_plane3 mu_ga_antiproject_plane3_point3(mu_ga_plane3 g, mu_vec3 p)
{
    return mu_ga_plane3_make(g.x, g.y, g.z, -(g.x * p[0] + g.y * p[1] + g.z * p[2]));
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

MU_INLINE mu_ga_motor2 mu_ga_motor2_make_rotation(float angle, mu_vec2 center)
{
    float c, s;
    mu_ga_cos_sin_half(angle, &c, &s);
    return mu_ga_motor2_make(center[0] * s, center[1] * s, s, c);
}

MU_INLINE mu_ga_motor2 mu_ga_motor2_make_translation(mu_vec2 offset)
{
    return mu_ga_motor2_make(offset[1] * -0.5f, offset[0] * 0.5f, 0.0f, 1.0f);
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

MU_INLINE void mu_ga_motor2_to_xform(mu_mat3 out, mu_ga_motor2 q)
{
    float m00 = 1.0f - q.z * q.z * 2.0f;
    float m01 = q.z * q.w * -2.0f;
    float m02 = (q.x * q.z + q.y * q.w) * 2.0f;
    float m10 = q.z * q.w * 2.0f;
    float m11 = 1.0f - q.z * q.z * 2.0f;
    float m12 = (q.y * q.z - q.x * q.w) * 2.0f;

    out[0] = m00;
    out[1] = m10;
    out[2] = 0.0f;
    out[3] = m01;
    out[4] = m11;
    out[5] = 0.0f;
    out[6] = m02;
    out[7] = m12;
    out[8] = 1.0f;
}

MU_INLINE void mu_ga_motor2_to_inverse_xform(mu_mat3 out, mu_ga_motor2 q)
{
    float m00 = 1.0f - q.z * q.z * 2.0f;
    float m01 = q.z * q.w * 2.0f;
    float m02 = (q.x * q.z - q.y * q.w) * 2.0f;
    float m10 = q.z * q.w * -2.0f;
    float m11 = 1.0f - q.z * q.z * 2.0f;
    float m12 = (q.y * q.z + q.x * q.w) * 2.0f;

    out[0] = m00;
    out[1] = m10;
    out[2] = 0.0f;
    out[3] = m01;
    out[4] = m11;
    out[5] = 0.0f;
    out[6] = m02;
    out[7] = m12;
    out[8] = 1.0f;
}

MU_INLINE void mu_ga_transform_vec2_motor(mu_vec2 out, mu_vec2 v, mu_ga_motor2 q)
{
    mu_mat3 m;
    mu_ga_motor2_to_xform(m, q);
    mu_vec3 homogeneous = {v[0], v[1], 0.0f};
    mu_vec3 r;
    mu_ga_mat3_mulv(r, m, homogeneous);
    mu_ga_vec2(out, r[0], r[1]);
}

MU_INLINE void mu_ga_transform_point2_motor(mu_vec2 out, mu_vec2 p, mu_ga_motor2 q)
{
    mu_mat3 m;
    mu_ga_motor2_to_xform(m, q);
    mu_vec3 homogeneous = {p[0], p[1], 1.0f};
    mu_vec3 r;
    mu_ga_mat3_mulv(r, m, homogeneous);
    mu_ga_vec2(out, r[0], r[1]);
}

MU_INLINE mu_ga_flat_point2 mu_ga_transform_flat_point2_motor(mu_ga_flat_point2 p, mu_ga_motor2 q)
{
    mu_mat3 m;
    mu_ga_motor2_to_xform(m, q);
    mu_vec3 r;
    mu_ga_mat3_mulv(r, m, (mu_vec3){p.x / p.z, p.y / p.z, 1.0f});
    return mu_ga_flat_point2_make(r[0] * p.z, r[1] * p.z, p.z);
}

MU_INLINE mu_ga_line2 mu_ga_transform_line2_motor(mu_ga_line2 g, mu_ga_motor2 q)
{
    mu_mat3 m;
    mu_ga_motor2_to_xform(m, q);
    mu_vec3 n;
    mu_ga_mat3_mulv(n, m, (mu_vec3){g.x, g.y, 0.0f});
    float m02 = m[6];
    float m12 = m[7];
    float c = g.z - (n[0] * m02 + n[1] * m12);
    return mu_ga_line2_make(n[0], n[1], c);
}

MU_INLINE mu_ga_motor2 mu_ga_motor2_set_xform(mu_mat3 M)
{
    float m00 = M[0];
    float m10 = M[1];
    float m02 = M[6];
    float m12 = M[7];
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

MU_INLINE mu_ga_flector2 mu_ga_flector2_make_transflection(mu_vec2 offset, mu_ga_line2 line_unit)
{
    return mu_ga_flector2_make(
        line_unit.x,
        line_unit.y,
        (offset[0] * line_unit.x + offset[1] * line_unit.y) * 0.5f + line_unit.z,
        (offset[1] * line_unit.x - offset[0] * line_unit.y) * 0.5f
    );
}

MU_INLINE void mu_ga_flector2_to_xform(mu_mat3 out, mu_ga_flector2 f)
{
    float m00 = 1.0f - f.x * f.x * 2.0f;
    float m01 = f.x * f.y * -2.0f;
    float m02 = (f.x * f.z + f.y * f.w) * -2.0f;
    float m10 = f.x * f.y * -2.0f;
    float m11 = 1.0f - f.y * f.y * 2.0f;
    float m12 = (f.y * f.z - f.x * f.w) * -2.0f;

    out[0] = m00;
    out[1] = m10;
    out[2] = 0.0f;
    out[3] = m01;
    out[4] = m11;
    out[5] = 0.0f;
    out[6] = m02;
    out[7] = m12;
    out[8] = 1.0f;
}

MU_INLINE void mu_ga_flector2_to_inverse_xform(mu_mat3 out, mu_ga_flector2 f)
{
    float m00 = 1.0f - f.x * f.x * 2.0f;
    float m01 = f.x * f.y * -2.0f;
    float m02 = (f.x * f.z - f.y * f.w) * -2.0f;
    float m10 = f.x * f.y * -2.0f;
    float m11 = 1.0f - f.y * f.y * 2.0f;
    float m12 = (f.y * f.z + f.x * f.w) * -2.0f;

    out[0] = m00;
    out[1] = m10;
    out[2] = 0.0f;
    out[3] = m01;
    out[4] = m11;
    out[5] = 0.0f;
    out[6] = m02;
    out[7] = m12;
    out[8] = 1.0f;
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

MU_INLINE void mu_ga_transform_vec2_flector(mu_vec2 out, mu_vec2 v, mu_ga_flector2 q)
{
    mu_mat3 m;
    mu_ga_flector2_to_xform(m, q);
    mu_vec3 homogeneous = {v[0], v[1], 0.0f};
    mu_vec3 r;
    mu_ga_mat3_mulv(r, m, homogeneous);
    mu_ga_vec2(out, r[0], r[1]);
}

MU_INLINE void mu_ga_transform_point2_flector(mu_vec2 out, mu_vec2 p, mu_ga_flector2 q)
{
    mu_mat3 m;
    mu_ga_flector2_to_xform(m, q);
    mu_vec3 homogeneous = {p[0], p[1], 1.0f};
    mu_vec3 r;
    mu_ga_mat3_mulv(r, m, homogeneous);
    mu_ga_vec2(out, r[0], r[1]);
}

MU_INLINE mu_ga_flat_point2 mu_ga_transform_flat_point2_flector(mu_ga_flat_point2 p, mu_ga_flector2 f)
{
    mu_mat3 m;
    mu_ga_flector2_to_xform(m, f);
    mu_vec3 r;
    mu_ga_mat3_mulv(r, m, (mu_vec3){p.x / p.z, p.y / p.z, 1.0f});
    return mu_ga_flat_point2_make(r[0] * p.z, r[1] * p.z, p.z);
}

MU_INLINE mu_ga_line2 mu_ga_transform_line2_flector(mu_ga_line2 g, mu_ga_flector2 f)
{
    mu_mat3 m;
    mu_ga_flector2_to_xform(m, f);
    mu_vec3 n;
    mu_ga_mat3_mulv(n, m, (mu_vec3){g.x, g.y, 0.0f});
    float m02 = m[6];
    float m12 = m[7];
    float c = g.z - (n[0] * m02 + n[1] * m12);
    return mu_ga_line2_make(n[0], n[1], c);
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

MU_INLINE mu_ga_motor3 mu_ga_motor3_make_rotation(float angle, mu_vec3 axis_unit)
{
    float c, s;
    mu_ga_cos_sin_half(angle, &c, &s);
    return mu_ga_motor3_make(axis_unit[0] * s, axis_unit[1] * s, axis_unit[2] * s, c, 0.0f, 0.0f, 0.0f, 0.0f);
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_make_translation(mu_vec3 offset)
{
    return mu_ga_motor3_make(0.0f, 0.0f, 0.0f, 1.0f, offset[0] * 0.5f, offset[1] * 0.5f, offset[2] * 0.5f, 0.0f);
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
MU_INLINE void mu_ga_motor3_to_xform(mu_mat4 out, mu_ga_motor3 q)
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

    out[0] = m00;
    out[1] = m10;
    out[2] = m20;
    out[3] = 0.0f;
    out[4] = m01;
    out[5] = m11;
    out[6] = m21;
    out[7] = 0.0f;
    out[8] = m02;
    out[9] = m12;
    out[10] = m22;
    out[11] = 0.0f;
    out[12] = m03;
    out[13] = m13;
    out[14] = m23;
    out[15] = 1.0f;
}

MU_INLINE void mu_ga_motor3_to_inverse_xform(mu_mat4 out, mu_ga_motor3 q)
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

    out[0] = m00;
    out[1] = m10;
    out[2] = m20;
    out[3] = 0.0f;
    out[4] = m01;
    out[5] = m11;
    out[6] = m21;
    out[7] = 0.0f;
    out[8] = m02;
    out[9] = m12;
    out[10] = m22;
    out[11] = 0.0f;
    out[12] = m03;
    out[13] = m13;
    out[14] = m23;
    out[15] = 1.0f;
}
MU_INLINE void mu_ga_quat_from_mat3(mu_quat out, mu_mat3 m)
{
    float trace = m[0] + m[4] + m[8];

    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        out[3] = 0.25f * s;
        out[0] = (m[5] - m[7]) / s;
        out[1] = (m[6] - m[2]) / s;
        out[2] = (m[1] - m[3]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        out[3] = (m[5] - m[7]) / s;
        out[0] = 0.25f * s;
        out[1] = (m[3] + m[1]) / s;
        out[2] = (m[6] + m[2]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        out[3] = (m[6] - m[2]) / s;
        out[0] = (m[3] + m[1]) / s;
        out[1] = 0.25f * s;
        out[2] = (m[7] + m[5]) / s;
    } else {
        float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        out[3] = (m[1] - m[3]) / s;
        out[0] = (m[6] + m[2]) / s;
        out[1] = (m[7] + m[5]) / s;
        out[2] = 0.25f * s;
    }
}

MU_INLINE mu_ga_motor3 mu_ga_motor3_set_xform(mu_mat4 M)
{
    mu_mat3 r = {M[0], M[1], M[2], M[4], M[5], M[6], M[8], M[9], M[10]};

    mu_quat v;
    mu_ga_quat_from_mat3(v, r);

    float tx = M[12] * 0.5f;
    float ty = M[13] * 0.5f;
    float tz = M[14] * 0.5f;

    float mx =  v[3] * tx + v[2] * ty - v[1] * tz;
    float my =  v[3] * ty + v[0] * tz - v[2] * tx;
    float mz =  v[3] * tz + v[1] * tx - v[0] * ty;
    float mw = -v[0] * tx - v[1] * ty - v[2] * tz;

    return mu_ga_motor3_make(v[0], v[1], v[2], v[3], mx, my, mz, mw);
}

MU_INLINE void mu_ga_transform_vec3_motor(mu_vec3 out, mu_vec3 v, mu_ga_motor3 q)
{
    mu_mat4 m;
    mu_ga_motor3_to_xform(m, q);
    mu_vec4 homogeneous = {v[0], v[1], v[2], 0.0f};
    mu_vec4 r;
    mu_ga_mat4_mulv(r, m, homogeneous);
    mu_ga_vec3(out, r[0], r[1], r[2]);
}

MU_INLINE void mu_ga_transform_point3_motor(mu_vec3 out, mu_vec3 p, mu_ga_motor3 q)
{
    mu_mat4 m;
    mu_ga_motor3_to_xform(m, q);
    mu_vec4 homogeneous = {p[0], p[1], p[2], 1.0f};
    mu_vec4 r;
    mu_ga_mat4_mulv(r, m, homogeneous);
    mu_ga_vec3(out, r[0], r[1], r[2]);
}

MU_INLINE mu_ga_flat_point3 mu_ga_transform_flat_point3_motor(mu_ga_flat_point3 p, mu_ga_motor3 q)
{
    mu_vec3 ep = {p.x / p.w, p.y / p.w, p.z / p.w};
    mu_vec3 tp;
    mu_ga_transform_point3_motor(tp, ep, q);
    return mu_ga_flat_point3_make(tp[0] * p.w, tp[1] * p.w, tp[2] * p.w, p.w);
}

MU_INLINE mu_ga_line3 mu_ga_transform_line3_motor(mu_ga_line3 l, mu_ga_motor3 q)
{
    mu_mat4 m;
    mu_ga_motor3_to_xform(m, q);
    mu_vec4 v4;
    mu_ga_mat4_mulv(v4, m, (mu_vec4){l.vx, l.vy, l.vz, 0.0f});
    mu_vec4 mm4;
    mu_ga_mat4_mulv(mm4, m, (mu_vec4){l.mx, l.my, l.mz, 0.0f});
    mu_vec3 v = {v4[0], v4[1], v4[2]};
    mu_vec3 mm = {mm4[0], mm4[1], mm4[2]};
    mu_vec3 t = {m[12], m[13], m[14]};
    mu_vec3 cross7;
    mu_vec3_cross(cross7, t, v);
    mu_vec3 mp;
    mu_vec3_add(mp, mm, cross7);
    return mu_ga_line3_make(v[0], v[1], v[2], mp[0], mp[1], mp[2]);
}

MU_INLINE mu_ga_plane3 mu_ga_transform_plane3_motor(mu_ga_plane3 g, mu_ga_motor3 q)
{
    mu_mat4 m;
    mu_ga_motor3_to_xform(m, q);
    mu_vec4 n4;
    mu_ga_mat4_mulv(n4, m, (mu_vec4){g.x, g.y, g.z, 0.0f});
    mu_vec3 n = {n4[0], n4[1], n4[2]};
    float d = g.w - (n[0] * m[12] + n[1] * m[13] + n[2] * m[14]);
    return mu_ga_plane3_make(n[0], n[1], n[2], d);
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

MU_INLINE mu_ga_flector3 mu_ga_flector3_make_transflection(mu_vec3 offset, mu_ga_plane3 plane_unit)
{
    return mu_ga_flector3_make(
        (offset[1] * plane_unit.z - offset[2] * plane_unit.y) * 0.5f,
        (offset[2] * plane_unit.x - offset[0] * plane_unit.z) * 0.5f,
        (offset[0] * plane_unit.y - offset[1] * plane_unit.x) * 0.5f,
        0.0f,
        plane_unit.x,
        plane_unit.y,
        plane_unit.z,
        plane_unit.w - (offset[0] * plane_unit.x + offset[1] * plane_unit.y + offset[2] * plane_unit.z) * 0.5f
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

MU_INLINE void mu_ga_flector3_to_xform(mu_mat4 out, mu_ga_flector3 f)
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

    out[0] = m00;
    out[1] = m10;
    out[2] = m20;
    out[3] = 0.0f;
    out[4] = m01;
    out[5] = m11;
    out[6] = m21;
    out[7] = 0.0f;
    out[8] = m02;
    out[9] = m12;
    out[10] = m22;
    out[11] = 0.0f;
    out[12] = m03;
    out[13] = m13;
    out[14] = m23;
    out[15] = 1.0f;
}

MU_INLINE void mu_ga_flector3_to_inverse_xform(mu_mat4 out, mu_ga_flector3 f)
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

    out[0] = m00;
    out[1] = m10;
    out[2] = m20;
    out[3] = 0.0f;
    out[4] = m01;
    out[5] = m11;
    out[6] = m21;
    out[7] = 0.0f;
    out[8] = m02;
    out[9] = m12;
    out[10] = m22;
    out[11] = 0.0f;
    out[12] = m03;
    out[13] = m13;
    out[14] = m23;
    out[15] = 1.0f;
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

MU_INLINE void mu_ga_transform_vec3_flector(mu_vec3 out, mu_vec3 v, mu_ga_flector3 q)
{
    mu_mat4 m;
    mu_ga_flector3_to_xform(m, q);
    mu_vec4 homogeneous = {v[0], v[1], v[2], 0.0f};
    mu_vec4 r;
    mu_ga_mat4_mulv(r, m, homogeneous);
    mu_ga_vec3(out, r[0], r[1], r[2]);
}

MU_INLINE void mu_ga_transform_point3_flector(mu_vec3 out, mu_vec3 p, mu_ga_flector3 q)
{
    mu_mat4 m;
    mu_ga_flector3_to_xform(m, q);
    mu_vec4 homogeneous = {p[0], p[1], p[2], 1.0f};
    mu_vec4 r;
    mu_ga_mat4_mulv(r, m, homogeneous);
    mu_ga_vec3(out, r[0], r[1], r[2]);
}

MU_INLINE mu_ga_flat_point3 mu_ga_transform_flat_point3_flector(mu_ga_flat_point3 p, mu_ga_flector3 f)
{
    mu_vec3 ep = {p.x / p.w, p.y / p.w, p.z / p.w};
    mu_vec3 tp;
    mu_ga_transform_point3_flector(tp, ep, f);
    return mu_ga_flat_point3_make(tp[0] * p.w, tp[1] * p.w, tp[2] * p.w, p.w);
}

MU_INLINE mu_ga_line3 mu_ga_transform_line3_flector(mu_ga_line3 l, mu_ga_flector3 f)
{
    mu_mat4 m;
    mu_ga_flector3_to_xform(m, f);
    mu_vec4 v4;
    mu_ga_mat4_mulv(v4, m, (mu_vec4){l.vx, l.vy, l.vz, 0.0f});
    mu_vec4 mm4;
    mu_ga_mat4_mulv(mm4, m, (mu_vec4){l.mx, l.my, l.mz, 0.0f});
    mu_vec3 v = {v4[0], v4[1], v4[2]};
    mu_vec3 mm = {mm4[0], mm4[1], mm4[2]};
    mu_vec3 t = {m[12], m[13], m[14]};
    mu_vec3 cross9;
    mu_vec3_cross(cross9, t, v);
    mu_vec3 mp;
    mu_vec3_add(mp, mm, cross9);
    return mu_ga_line3_make(v[0], v[1], v[2], mp[0], mp[1], mp[2]);
}

MU_INLINE mu_ga_plane3 mu_ga_transform_plane3_flector(mu_ga_plane3 g, mu_ga_flector3 f)
{
    mu_mat4 m;
    mu_ga_flector3_to_xform(m, f);
    mu_vec4 n4;
    mu_ga_mat4_mulv(n4, m, (mu_vec4){g.x, g.y, g.z, 0.0f});
    mu_vec3 n = {n4[0], n4[1], n4[2]};
    float d = g.w - (n[0] * m[12] + n[1] * m[13] + n[2] * m[14]);
    return mu_ga_plane3_make(n[0], n[1], n[2], d);
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

MU_INLINE mu_ga_round_point2 mu_ga_round_point2_from_point(mu_vec2 p)
{
    return mu_ga_round_point2_make(p[0], p[1], 1.0f, (p[0] * p[0] + p[1] * p[1]) * 0.5f);
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

MU_INLINE mu_ga_line2 mu_ga_wedge_point2_round_point2(mu_vec2 p, mu_ga_round_point2 a)
{
    return mu_ga_line2_make(
        p[1] * a.z - a.y,
        a.x - p[0] * a.z,
        p[0] * a.y - p[1] * a.x
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

MU_INLINE mu_ga_round_point2 mu_ga_antiwedge_circle2_point2(mu_ga_circle2 c, mu_vec2 p)
{
    return mu_ga_round_point2_make(c.w * p[0], c.w * p[1], c.w, -c.x * p[0] - c.y * p[1] - c.z);
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

MU_INLINE mu_ga_round_point3 mu_ga_round_point3_from_point(mu_vec3 p)
{
    return mu_ga_round_point3_make(p[0], p[1], p[2], 1.0f, (p[0]*p[0] + p[1]*p[1] + p[2]*p[2]) * 0.5f);
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

MU_INLINE mu_ga_line3 mu_ga_wedge_point3_round_point3(mu_vec3 p, mu_ga_round_point3 a)
{
    return mu_ga_line3_make(
        p[0]*a.w - a.x,
        p[1]*a.w - a.y,
        p[2]*a.w - a.z,
        p[2]*a.y - p[1]*a.z,
        p[0]*a.z - p[2]*a.x,
        p[1]*a.x - p[0]*a.y
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

MU_INLINE mu_ga_plane3 mu_ga_wedge_dipole3_point3(mu_ga_dipole3 d, mu_vec3 p)
{
    return mu_ga_plane3_make(
        d.vy*p[2] - d.vz*p[1] + d.mx,
        d.vz*p[0] - d.vx*p[2] + d.my,
        d.vx*p[1] - d.vy*p[0] + d.mz,
       -d.mx*p[0] - d.my*p[1] - d.mz*p[2]
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

MU_INLINE mu_ga_round_point3 mu_ga_antiwedge_sphere3_point3(mu_ga_sphere3 s, mu_vec3 p)
{
    return mu_ga_round_point3_make(s.u*p[0], s.u*p[1], s.u*p[2], s.u, -s.x*p[0] - s.y*p[1] - s.z*p[2] - s.w);
}

/* ------------------------------------------------------------------------- */
/* tiny usage examples */
/* ------------------------------------------------------------------------- */

/*
    Example A: rigid 3D line from two points and projection

        mu_vec3 p0 = {0.0f, 0.0f, 0.0f};
        mu_vec3 p1 = {1.0f, 1.0f, 0.0f};
        mu_ga_line3 l = mu_ga_wedge_point3_point3(p0, p1);

        mu_vec3 q = {0.4f, 2.0f, 0.0f};
        mu_vec3 pr;
        mu_ga_project_point3_line3(pr, q, mu_ga_line3_make(
                              l.vx, l.vy, l.vz, l.mx, l.my, l.mz));

    Example B: 3D motor transform (rotation + translation)

        mu_vec3 axis = {0.0f, 0.0f, 1.0f};
        mu_ga_motor3 R = mu_ga_motor3_make_rotation(1.0f, axis);
        mu_ga_motor3 T = mu_ga_motor3_make_translation((mu_vec3){3.0f, 0.0f, 0.0f});
        mu_ga_motor3 Q = mu_ga_motor3_mul(T, R);

        mu_vec3 p = {1.0f, 0.0f, 0.0f};
        mu_vec3 p2;
        mu_ga_transform_point3_motor(p2, p, Q);

    Example C: conformal meet (sphere ^ plane -> circle)

        mu_ga_sphere3 s = mu_ga_sphere3_make(-1, 0,0,0, -4);  // centered sphere form
        mu_ga_plane3  g = mu_ga_plane3_make(0,1,0,0);          // y = 0 plane
        mu_ga_circle3 c = mu_ga_antiwedge_sphere3_plane3(s, g);
*/

#ifdef __cplusplus
}
#endif

#endif
