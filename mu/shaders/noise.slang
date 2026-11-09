/* mu/noise.slang — procedural noise for surfaces, volumes and motion.
 * ============================================================================
 *  Every generator is built on the integer PCG3D hash in common.slang: no
 *  sin(dot()) hash (that is a quarter-rate op AND loses precision at large
 *  coordinates), no lookup texture, no state. Shared integer hashing also
 *  means the value/gradient/simplex/worley variants agree on the same cell
 *  lattice, so mixing them does not produce seams.
 *
 *  Cost notes:
 *   - value noise   : 4 (2D) or 8 (3D) hashes + a few FMAs, 0 quarter-rate ops
 *   - gradient noise: +1 rsqrt per corner (4 / 8 quarter-rate ops)
 *   - simplex2      : 3 corners, +3 rsqrt, cheaper than gradient noise in 2D
 *   - worley        : 9 cells, 1 rsqrt per cell (9 quarter-rate ops)
 *   - curl          : 4 value-noise evals; do not call it per pixel per light
 *   - fbm4          : 4 value-noise evals, unrolled — a predictable 16 hashes
 * ==========================================================================*/
#ifndef MU_NOISE_SLANG
#define MU_NOISE_SLANG

#include "common.slang"

/* -------------------------------------------------------------- hashing */
float mu_hash_u2(uint2 p)              { return mu_hash01(uint3(p, 0u)); }
float2 mu_hash2_u2(uint2 p)            { return mu_hash02(uint3(p, 0u)); }
float mu_hash_u3(uint3 p)              { return mu_hash01(p); }
float2 mu_hash2_u3(uint3 p)            { return mu_hash02(p); }
float3 mu_hash3_u3(uint3 p)            { return mu_hash03(p); }

/* Integer-grid hash. `int2` casts wrap negatives to large uints, which is
 * exactly what we want: no special case for negative cell coordinates. */
float mu_cell_hash(int2 c)             { return mu_hash_u2(uint2(c)); }
float2 mu_cell_hash2(int2 c)           { return mu_hash2_u2(uint2(c)); }
float mu_cell_hash3(int3 c)            { return mu_hash_u3(uint3(c)); }

/* Normalized random gradient: 1 rsqrt (quarter-rate) per corner. */
float2 mu_grad2(int2 c)
{
    float2 h = mu_cell_hash2(c) * 2.0 - 1.0;
    return h * rsqrt(max(dot(h, h), 1e-8));       /* 1 quarter-rate rsqrt */
}
float3 mu_grad3(int3 c)
{
    float3 h = mu_hash3_u3(uint3(c)) * 2.0 - 1.0;
    return h * rsqrt(max(dot(h, h), 1e-8));       /* 1 quarter-rate rsqrt */
}

/* ---------------------------------------------------------- value noise */
float mu_value_noise2(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);           /* smooth, no trig */
    float a = mu_cell_hash(int2(i) + int2(0, 0));
    float b = mu_cell_hash(int2(i) + int2(1, 0));
    float c = mu_cell_hash(int2(i) + int2(0, 1));
    float d = mu_cell_hash(int2(i) + int2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y) * 2.0 - 1.0;
}

float mu_value_noise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    int3 c = int3(i);
    float n000 = mu_cell_hash3(c + int3(0,0,0));
    float n100 = mu_cell_hash3(c + int3(1,0,0));
    float n010 = mu_cell_hash3(c + int3(0,1,0));
    float n110 = mu_cell_hash3(c + int3(1,1,0));
    float n001 = mu_cell_hash3(c + int3(0,0,1));
    float n101 = mu_cell_hash3(c + int3(1,0,1));
    float n011 = mu_cell_hash3(c + int3(0,1,1));
    float n111 = mu_cell_hash3(c + int3(1,1,1));
    float x00 = lerp(n000, n100, u.x), x10 = lerp(n010, n110, u.x);
    float x01 = lerp(n001, n101, u.x), x11 = lerp(n011, n111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z) * 2.0 - 1.0;
}

/* ------------------------------------------------------- gradient noise */
/* Perlin-style gradient noise, 2D. 4 rsqrt (one per corner). */
float mu_gradient_noise2(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);  /* quintic */
    int2 c = int2(i);
    float a = dot(mu_grad2(c + int2(0,0)), f - float2(0,0));
    float b = dot(mu_grad2(c + int2(1,0)), f - float2(1,0));
    float d = dot(mu_grad2(c + int2(0,1)), f - float2(0,1));
    float e = dot(mu_grad2(c + int2(1,1)), f - float2(1,1));
    return lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y);
}

/* 3D gradient noise, 8 rsqrt. Prefer mu_value_noise3 for fog volumes. */
float mu_gradient_noise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    int3 c = int3(i);
    float n000 = dot(mu_grad3(c + int3(0,0,0)), f - float3(0,0,0));
    float n100 = dot(mu_grad3(c + int3(1,0,0)), f - float3(1,0,0));
    float n010 = dot(mu_grad3(c + int3(0,1,0)), f - float3(0,1,0));
    float n110 = dot(mu_grad3(c + int3(1,1,0)), f - float3(1,1,0));
    float n001 = dot(mu_grad3(c + int3(0,0,1)), f - float3(0,0,1));
    float n101 = dot(mu_grad3(c + int3(1,0,1)), f - float3(1,0,1));
    float n011 = dot(mu_grad3(c + int3(0,1,1)), f - float3(0,1,1));
    float n111 = dot(mu_grad3(c + int3(1,1,1)), f - float3(1,1,1));
    float x00 = lerp(n000, n100, u.x), x10 = lerp(n010, n110, u.x);
    float x01 = lerp(n001, n101, u.x), x11 = lerp(n011, n111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

/* ------------------------------------------------------------ simplex 2D */
/* Gustavson's simplex, branchless triangle choice via select(). 3 corners
 * instead of 4, 3 rsqrt, no trig, no table. */
float mu_simplex2(float2 v)
{
    const float F2 = 0.3660254037844386;   /* 0.5*(sqrt(3)-1)   */
    const float G2 = 0.21132486540518713;  /* (3-sqrt(3))/6     */
    float  s  = (v.x + v.y) * F2;
    float2 i  = floor(v + s);
    float  t  = (i.x + i.y) * G2;
    float2 p0 = v - (i - t);
    float2 i1 = select(p0.x > p0.y, float2(1.0, 0.0), float2(0.0, 1.0));
    float2 p1 = p0 - i1 + G2;
    float2 p2 = p0 - 1.0 + 2.0 * G2;

    float3 m = max(float3(0.5) - float3(dot(p0,p0), dot(p1,p1), dot(p2,p2)), float3(0.0));
    m = m * m; m = m * m;                  /* m^4, multiply form */
    float3 g = float3(dot(mu_grad2(int2(i)),          p0),
                      dot(mu_grad2(int2(i + i1)),     p1),
                      dot(mu_grad2(int2(i + 1.0)),    p2));
    return 130.0 * dot(m, g);
}

/* -------------------------------------------------------------- worley */
static const float2 MU_WORLEY_NEIGHBOURS[9] = {
    float2(-1,-1), float2(0,-1), float2(1,-1),
    float2(-1, 0), float2(0, 0), float2(1, 0),
    float2(-1, 1), float2(0, 1), float2(1, 1)
};

/* F1 and F2 in one 9-cell scan. `jitter` = 1 gives a full Voronoi cell.
 * 9 rsqrt (one per cell); no branch thanks to the min/max pair. */
float2 mu_worley2_f1f2(float2 p, float jitter)
{
    float2 ip = floor(p);
    float2 fp = frac(p);
    float f1 = 8.0, f2 = 8.0;
    [ForceUnroll]
    for (int k = 0; k < 9; ++k)
    {
        float2 g  = MU_WORLEY_NEIGHBOURS[k];
        float2 o  = mu_cell_hash2(int2(ip) + int2(g)) * jitter;
        float2 r  = g + o - fp;
        float  d  = sqrt(dot(r, r));       /* 1 quarter-rate sqrt */
        f2 = min(f2, max(d, f1));
        f1 = min(f1, d);
    }
    return float2(f1, f2);
}
float mu_worley2(float2 p, float jitter) { return mu_worley2_f1f2(p, jitter).x; }
/* Cell border intensity: 0 at the seed, 1 on the F1/F2 boundary. */
float mu_worley_edge(float2 f1f2) { return mu_saturate(f1f2.y - f1f2.x); }

/* ---------------------------------------------------------- curl noise */
/* 2D divergence-free field from a scalar potential. 4 value-noise evals
 * (16 PCG hashes). `eps` should be ~1/256 of the feature size. */
float2 mu_curl_noise2(float2 p, float eps)
{
    float inv = 0.5 / eps;
    float dpdy = (mu_value_noise2(p + float2(0.0, eps)) - mu_value_noise2(p - float2(0.0, eps))) * inv;
    float dpdx = (mu_value_noise2(p + float2(eps, 0.0)) - mu_value_noise2(p - float2(eps, 0.0))) * inv;
    return float2(dpdy, -dpdx);
}
/* 3D curl from three offset potentials: 6 value-noise3 evals. */
float3 mu_curl_noise3(float3 p, float eps)
{
    float inv = 0.5 / eps;
    float3 o1 = float3(31.4, 17.7, 43.2), o2 = float3(11.1, 83.2, 7.9);
    float a_y = mu_value_noise3(p + float3(0,eps,0) + o1) - mu_value_noise3(p - float3(0,eps,0) + o1);
    float a_z = mu_value_noise3(p + float3(0,0,eps) + o1) - mu_value_noise3(p - float3(0,0,eps) + o1);
    float b_x = mu_value_noise3(p + float3(eps,0,0) + o2) - mu_value_noise3(p - float3(eps,0,0) + o2);
    float b_z = mu_value_noise3(p + float3(0,0,eps) + o2) - mu_value_noise3(p - float3(0,0,eps) + o2);
    return float3(a_z - b_z, -a_y, b_x) * inv;
}

/* ---------------------------------------------------- fractal wrappers */
/* Fixed rotation between octaves kills the axis-aligned look without a trig
 * call: (0.8, 0.6, -0.6, 0.8) is a unit rotation. */
#define MU_FBM_ROT float2x2(0.8, 0.6, -0.6, 0.8)

float mu_fbm4(float2 p, float lacunarity, float gain)
{
    float a = 0.5, sum = 0.0, norm = 0.0;
    [ForceUnroll]
    for (int i = 0; i < 4; ++i)
    {
        sum  += a * mu_value_noise2(p);
        norm += a;
        p     = mul(MU_FBM_ROT, p) * lacunarity + 11.7;
        a    *= gain;
    }
    return sum / norm;
}
/* Ridged: abs() then inverted, sharp creases — good for terrain and lightning. */
float mu_ridged4(float2 p, float lacunarity, float gain)
{
    float a = 0.5, sum = 0.0, norm = 0.0;
    [ForceUnroll]
    for (int i = 0; i < 4; ++i)
    {
        float n = 1.0 - abs(mu_value_noise2(p));
        sum  += a * n * n;
        norm += a;
        p     = mul(MU_FBM_ROT, p) * lacunarity + 11.7;
        a    *= gain;
    }
    return sum / norm;
}
/* Turbulence: sum of |noise|, the classic marble/fire distortion field. */
float mu_turbulence4(float2 p, float lacunarity, float gain)
{
    float a = 0.5, sum = 0.0, norm = 0.0;
    [ForceUnroll]
    for (int i = 0; i < 4; ++i)
    {
        sum  += a * abs(mu_value_noise2(p));
        norm += a;
        p     = mul(MU_FBM_ROT, p) * lacunarity + 11.7;
        a    *= gain;
    }
    return sum / norm;
}
/* Domain warp: 2 extra noise evals turn grid-like fbm into swirling flow. */
float2 mu_domain_warp2(float2 p, float strength)
{
    return p + strength * float2(mu_value_noise2(p), mu_value_noise2(p + float2(5.2, 1.3)));
}
/* Marble: banded sine of a warped coordinate (1 quarter-rate sin). */
float mu_marble(float2 p, float frequency, float warp)
{
    float q = mu_fbm4(mu_domain_warp2(p, warp), 2.0, 0.5);
    return mu_mad(0.5, sin(mu_mad(frequency, p.x, q * 6.0)), 0.5);  /* 1 quarter-rate sin, 2 Fma */
}

#endif /* MU_NOISE_SLANG */
