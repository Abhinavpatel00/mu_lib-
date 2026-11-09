/* mu/sdf.slang — signed distance primitives, operators and raymarch helpers.
 * ============================================================================
 *  Two layers, deliberately separate:
 *
 *    PRIMITIVES / OPS  plain functions on float2/float3. Zero state, zero
 *                      abstraction, safe to fold into an expression.
 *    FIELD HELPERS     generics over `IMuSdf3`. Define a struct with one
 *                      `dist()` method (there are four ready-made ones below)
 *                      and normals, raymarching, soft shadows and AO all work
 *                      on it without virtual dispatch, function pointers or
 *                      dynamic specialization. Slang monomorphizes the type.
 *
 *  The tetrahedron gradient costs FOUR field evals rather than six (the
 *  central-difference form), and the soft-shadow march is the standard
 *  "minimum h/t" cone-tracing approximation: no extra shadow map, one field
 *  eval per step.
 *
 *  Skill-applied rules (vulkan-opt 10, vulkan-shader-opt 02/08):
 *    - No sign(): OpExtInst FSign costs a +Inf multiply + clamp in ACO. The
 *      three 2D primitives need only a sign TEST, so they use mu_sign_nz()
 *      (one OpSelect). At the zero crossing all three already evaluate to 0,
 *      so +/-1 vs 0 is not observable; verified on a grid in validate_math.py.
 *    - Repetition folds must not hide a per-component divide inside a march.
 *      Pass the reciprocal (uniform, computed once per wave) instead.
 *    - A sphere trace converges geometrically: the eps-tail is pure cost and
 *      makes derivative-reconstructed normals noisy. mu_sdf_last_step()
 *      closes the geometric series in one division; mu_sdf_cone_eps() is
 *      scale-relative (>= f32 ulp of t).
 * ==========================================================================*/
#ifndef MU_SDF_SLANG
#define MU_SDF_SLANG

#include "common.slang"

/* ===========================================================================
 * 1. Boolean and smoothing operators
 * =========================================================================*/
float mu_sd_union(float a, float b)        { return min(a, b); }
float mu_sd_subtract(float a, float b)     { return max(-a, b); }   /* b minus a */
float mu_sd_intersect(float a, float b)    { return max(a, b); }

/* Polynomial smooth minimum. `k` is the blend radius; the result is also a
 * valid (though slightly contracted) distance field. Branch-free. */
float mu_sd_smin(float a, float b, float k)
{
    float h = mu_saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}
/* Cubic smooth min: preserves distances better than the polynomial form where
 * the two surfaces are far apart. */
float mu_sd_smin_cubic(float a, float b, float k)
{
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * k * (1.0 / 6.0);
}
float mu_sd_smax(float a, float b, float k) { return -mu_sd_smin(-a, -b, k); }

/* Shell ("onion") of a field: repeated thin surfaces, useful for force fields. */
float mu_sd_onion(float d, float thickness) { return abs(d) - thickness; }
/* Rounding: inflate a sharp primitive by `r` (subtract after evaluating). */
float mu_sd_round(float d, float r)         { return d - r; }

/* ===========================================================================
 * 2. Domain transforms
 * =========================================================================*/
/* Rotate by `cs = (cos a, sin a)` from mu_sincos(a). Taking an angle here would
 * hide two quarter-rate ops in a helper; make the caller pay them explicitly. */
float2 mu_sd_rotate2_cs(float2 p, float2 cs)
{
    return float2(mu_mad(p.x, cs.x, -p.y * cs.y),
                   mu_mad(p.x, cs.y, p.y * cs.x));   /* 2 Fma, 2 mul */
}
/* Infinite repetition: fold p into the cell centred at 0. `c` is the cell size. */
/* `p / c` where c is a float3 is THREE OpFDiv lanes = 3 v_rcp_f32 per field
 * eval. Inside a march that is per-step waste. The _inv forms take the
 * reciprocal (uniform) and emit OpExtInst Fma + Floor + FSub with no divide. */
float3 mu_sd_repeat3_inv(float3 p, float3 inv_c, float3 c)
{
    return frac(mu_mad(p, inv_c, float3(0.5))) * c - 0.5 * c;
}
float2 mu_sd_repeat2_inv(float2 p, float2 inv_c, float2 c)
{
    return frac(mu_mad(p, inv_c, float2(0.5))) * c - 0.5 * c;
}
/* Uniform cell size: one SCALAR reciprocal for the whole wave. */
float3 mu_sd_repeat3_c(float3 p, float c) { return mu_sd_repeat3_inv(p, float3(mu_rcp(c)), float3(c)); }
float2 mu_sd_repeat2_c(float2 p, float c) { return mu_sd_repeat2_inv(p, float2(mu_rcp(c)), float2(c)); }
float3 mu_sd_repeat3(float3 p, float3 c)  { return mu_sd_repeat3_inv(p, float3(mu_rcp(c.x), mu_rcp(c.y), mu_rcp(c.z)), c); }
float2 mu_sd_repeat2(float2 p, float2 c)  { return mu_sd_repeat2_inv(p, float2(mu_rcp(c.x), mu_rcp(c.y)), c); }
/* Limited repetition (Inigo Quilez): clamp the cell index. */
float3 mu_sd_repeat_limited3(float3 p, float3 c, float3 limit)
{
    return p - c * clamp(round(mu_mad(p, float3(mu_rcp(c.x), mu_rcp(c.y), mu_rcp(c.z)), float3(0.0))), -limit, limit);
}
/* Polar fold for radial symmetry: returns (radius, angle). */
float2 mu_sd_polar(float2 p, float repetitions)
{
    float a = mu_atan2_fast(p.y, p.x);       /* ~19 ALU, NO 109-instruction atan2 */
    float r = length(p);                     /* 1 quarter-rate rsqrt */
    float sector = MU_2PI / repetitions;
    return float2(r, frac(a / sector) * sector - 0.5 * sector);
}

/* ===========================================================================
 * 3. 2D primitives
 * =========================================================================*/
float mu_sd_circle2(float2 p, float r)      { return length(p) - r; }
float mu_sd_box2(float2 p, float2 b)
{
    float2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}
float mu_sd_round_box2(float2 p, float2 b, float r) { return mu_sd_box2(p, b - r) - r; }
float mu_sd_segment2(float2 p, float2 a, float2 b)
{
    float2 pa = p - a, ba = b - a;
    float h = mu_saturate(dot(pa, ba) / max(dot(ba, ba), MU_EPS_SQ));
    return length(pa - ba * h);
}
float mu_sd_triangle2(float2 p, float2 a, float2 b, float2 c)
{
    /* IQ's isosceles triangle; returns distance to edges (sign outside) */
    float2 e0 = b - a, e1 = c - b, e2 = a - c;
    float2 v0 = p - a, v1 = p - b, v2 = p - c;
    float  sg  = mu_sign_nz(e0.x * e2.y - e0.y * e2.x);  /* winding: OpSelect, not FSign */
    float2 pq0 = v0 - e0 * clamp(dot(v0,e0)/dot(e0,e0), 0.0, 1.0);
    float2 pq1 = v1 - e1 * clamp(dot(v1,e1)/dot(e1,e1), 0.0, 1.0);
    float2 pq2 = v2 - e2 * clamp(dot(v2,e2)/dot(e2,e2), 0.0, 1.0);
    float2 d = min(min(float2(dot(pq0,pq0), sg*(v0.x*e0.y - v0.y*e0.x)),
                       float2(dot(pq1,pq1), sg*(v1.x*e1.y - v1.y*e1.x))),
                   float2(dot(pq2,pq2), sg*(v2.x*e2.y - v2.y*e2.x)));
    return -sqrt(d.x) * mu_sign_nz(d.y);
}
float mu_sd_hexagon2(float2 p, float r)
{
    const float3 k = float3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= float2(clamp(p.x, -k.z * r, k.z * r), r);
    return length(p) * mu_sign_nz(p.y);
}
float mu_sd_capsule2(float2 p, float2 a, float2 b, float r)
{
    return mu_sd_segment2(p, a, b) - r;
}
float mu_sd_arc2(float2 p, float2 sc, float ra, float rb)
{
    /* sc = (sin, cos) of the arc aperture; ra = radius, rb = thickness */
    p.x = abs(p.x);
    return select(sc.y * p.x > sc.x * p.y,
                  length(p - sc * ra) - rb,
                  abs(length(p) - ra) - rb);
}

/* Antialiased fill from a distance, given the pixel's d/dx estimate. */
float mu_sdf_fill2(float d, float aa)       { return 1.0 - mu_smooth01(mu_saturate(0.5 + 0.5 * d / max(aa, MU_EPS))); }
float mu_sdf_stroke2(float d, float width, float aa)
{
    return 1.0 - mu_smooth01(mu_saturate(0.5 + 0.5 * (abs(d) - width) / max(aa, MU_EPS)));
}

/* ===========================================================================
 * 4. 3D primitives (plain functions)
 * =========================================================================*/
float mu_sd_plane(float3 p, float3 n, float h) { return dot(p, n) + h; }
float mu_sd_sphere(float3 p, float r)          { return length(p) - r; }
float mu_sd_box(float3 p, float3 b)
{
    float3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(mu_max3(q), 0.0);
}
float mu_sd_round_box(float3 p, float3 b, float r) { return mu_sd_box(p, b - r) - r; }
float mu_sd_torus(float3 p, float2 t)
{
    return length(float2(length(p.xz) - t.x, p.y)) - t.y;
}
float mu_sd_capsule(float3 p, float3 a, float3 b, float r)
{
    float3 pa = p - a, ba = b - a;
    float h = mu_saturate(dot(pa, ba) / max(dot(ba, ba), MU_EPS_SQ));
    return length(pa - ba * h) - r;
}
float mu_sd_cylinder(float3 p, float r, float h)
{
    float2 d = abs(float2(length(p.xz), p.y)) - float2(r, h);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}
/* IQ capped cone, exact. `height` is the half-height; r1 = radius at -h,
 * r2 = radius at +h. Set r2=0 for a pointy cone. */
float mu_sd_capped_cone(float3 p, float height, float r1, float r2)
{
    float2 q  = float2(length(p.xz), p.y);
    float2 k1 = float2(r2, height);
    float2 k2 = float2(r2 - r1, 2.0 * height);
    float2 ca = float2(q.x - min(q.x, select(q.y < 0.0, r1, r2)), abs(q.y) - height);
    float2 cb = q - k1 + k2 * mu_saturate(dot(k1 - q, k2) / max(dot(k2, k2), MU_EPS_SQ));
    float  sg = select(cb.x < 0.0 && ca.y < 0.0, -1.0, 1.0);
    return sg * sqrt(min(dot(ca, ca), dot(cb, cb)));   /* 1 quarter-rate sqrt */
}
/* Cheap L1 octahedron (an exact one needs a branch per face). The 1/sqrt(3)
 * normalization makes it a lower bound, which is safe for sphere tracing. */
float mu_sd_octahedron(float3 p, float s)
{
    p = abs(p);
    return (p.x + p.y + p.z - s) * 0.57735027;
}
/* ===========================================================================
 * 4b. Cone tracing / analytic last step  (vulkan-opt 10, Aaltonen GDC 2018)
 * =========================================================================*//*
 * Sphere tracing is FETCH bound: cost = steps/pixel x bytes/fetch. Two
 * independent fixes, both expressible without changing the field interface.
 *
 *  (1) Trace at the mip whose texel footprint matches the pixel cone:
 *      mip ~= log2(cone_width * t / voxel_size). Coarser levels are built
 *      from the distance field itself, NOT from a box downsample (averaging
 *      distances overestimates them and the march overshoots silhouettes).
 *  (2) A sphere trace converges geometrically and never actually reaches the
 *      surface. With the last two samples D, Dprev and q = D/Dprev, the
 *      remaining series sums to  D/(1-q) = D*Dprev/(Dprev-D). One division
 *      replaces the last 3-8 tiny fetches AND removes the per-pixel varying
 *      positional error that dFdx/dFdy turns into normal speckle.
 * ------------------------------------------------------------------------ */
/* Scale-relative epsilon: never below the f32 ulp of t. At t~100 the f32
 * spacing is 7.6e-6, so a fixed 1e-6 epsilon is pure cancellation. */
float mu_sdf_cone_eps(float t, float cone_width)
{
    return max(cone_width * t, 7.6e-6 * t);
}
/* Mip selection for a distance volume. `voxel_size` is the mip-0 texel width
 * in world units; result is a float LOD for textureLod/SampleLevel. */
float mu_sdf_cone_mip(float t, float cone_width, float voxel_size)
{
    return log2(max(cone_width * t, MU_EPS) * mu_rcp(max(voxel_size, MU_EPS)));
}
/* Closed-form remaining distance, with every guard the derivation needs.
 * Returns 0 when it is not safe to extrapolate (diverging or grazing ray). */
#define MU_SDF_MAX_EXTRAP 8.0   /* curvature / grazing guard; tune per scene */
float mu_sdf_last_step(float d, float dprev)
{
    float denom = dprev - d;                       /* dprev > d required */
    float rem   = d * dprev * mu_rcp(max(denom, MU_EPS));
    rem         = min(rem, dprev * MU_SDF_MAX_EXTRAP);
    return select(denom > 0.0, rem, 0.0);
}
/* The caller MUST still verify the extrapolated point with ONE extra field
 * fetch (skill 10 §2.3 guard 4): plane model can overshoot on convex
 * features. Fall back to the last valid t if the point is not inside. */
/* Trilinear tetrahedral gradient epsilon: ~0.5-1 voxel, NOT an arbitrary
 * 1e-4 world-space constant (that samples interpolation error, not field). */
float mu_sdf_gradient_eps(float voxel_size) { return max(0.5 * voxel_size, 1e-5); }

/* ===========================================================================
 * 5. Field helpers — generic over IMuSdf3
 * =========================================================================*/
interface IMuSdf3 { float dist(float3 p); }

/* Four ready-made fields. Copy one and compose primitives in dist(). */
struct MuSdfSphere : IMuSdf3
{
    float3 center; float radius;
    float dist(float3 p) { return mu_sd_sphere(p - center, radius); }
};
struct MuSdfBox : IMuSdf3
{
    float3 center; float3 half_extent;
    float dist(float3 p) { return mu_sd_box(p - center, half_extent); }
};
struct MuSdfTorus : IMuSdf3
{
    float3 center; float2 radii;              /* major, minor */
    float dist(float3 p) { return mu_sd_torus(p - center, radii); }
};
struct MuSdfCapsule : IMuSdf3
{
    float3 a, b; float radius;
    float dist(float3 p) { return mu_sd_capsule(p, a, b, radius); }
};

/* Tetrahedron gradient: 4 field evals instead of 6. `e` is the epsilon; pick
 * it from the pixel footprint, not a global constant, or distant surfaces
 * get faceted normals. */
float3 mu_sdf_normal<T:IMuSdf3>(T f, float3 p, float e)
{
    const float k = 0.57735027;
    float2 h = float2(1.0, -1.0) * k * e;
    return mu_safe_normalize(
          float3(h.x, h.y, h.y) * f.dist(p + float3(h.x, h.y, h.y))
        + float3(h.y, h.x, h.y) * f.dist(p + float3(h.y, h.x, h.y))
        + float3(h.y, h.y, h.x) * f.dist(p + float3(h.y, h.y, h.x))
        + float3(h.x, h.x, h.x) * f.dist(p + float3(h.x, h.x, h.x)));
}

/* Sphere tracing. Returns hit distance, or `tmax` on a miss. `max_steps` is a
 * uniform in practice; the loop bound is runtime so it is NOT unrolled — that
 * is intentional, an unrolled 128-iteration march would explode the code size
 * and the I-cache (32 KiB per WGP on RDNA4). */
float mu_sdf_raymarch<T:IMuSdf3>(T f, float3 ro, float3 rd,
                                 float tmin, float tmax, uint max_steps, float eps)
{
    float t = tmin;
    for (uint i = 0u; i < max_steps; ++i)
    {
        float d = f.dist(ro + rd * t);
        if (d < eps) return t;
        t += d;
        if (t > tmax) break;
    }
    return tmax;
}
/* Same march but also returns the iteration count (normalized) for a debug
 * heat map or for adaptive step budgeting. */
float2 mu_sdf_raymarch_stats<T:IMuSdf3>(T f, float3 ro, float3 rd,
                                        float tmin, float tmax, uint max_steps, float eps)
{
    float t = tmin;
    uint it = 0u;
    for (uint i = 0u; i < max_steps; ++i)
    {
        float d = f.dist(ro + rd * t);
        if (d < eps) return float2(t, float(it) / float(max_steps));
        t += d; it = i;
        if (t > tmax) break;
    }
    return float2(tmax, float(it) / float(max_steps));
}

/* Cone-traced soft shadow. `k` ~ 8..32: higher = harder. One field eval per
 * step, no shadow map. The `min(res, k*h/t)` accumulation is the IQ form. */
float mu_sdf_soft_shadow<T:IMuSdf3>(T f, float3 ro, float3 rd,
                                    float tmin, float tmax, float k, uint steps)
{
    float res = 1.0, t = tmin;
    for (uint i = 0u; i < steps; ++i)
    {
        float h = f.dist(ro + rd * t);
        res = min(res, k * h / t);
        t += clamp(h, 0.01, 0.2);
        if (res < 0.005 || t > tmax) break;
    }
    return mu_saturate(res);
}

/* 5-tap ambient occlusion along the normal. Fixed count, fully unrolled —
 * this one IS worth unrolling because it is 5 iterations. */
float mu_sdf_ao<T:IMuSdf3>(T f, float3 p, float3 n, float step_size)
{
    float occ = 0.0, sca = 1.0;
    [ForceUnroll]
    for (int i = 0; i < 5; ++i)
    {
        float h = 0.01 + float(i) * step_size;
        float d = f.dist(p + n * h);
        occ += (h - d) * sca;
        sca *= 0.7;
    }
    return mu_saturate(1.0 - 1.5 * occ);
}

#endif /* MU_SDF_SLANG */
