/* mu/common.slang — base math and utility for the mu shader library.
 * ============================================================================
 *  Rules this file obeys, and why (skills/vulkan-shader-opt):
 *
 *   1. Integer powers are written as multiplies. slangc 2026.21 at -O3 keeps
 *      pow(x,3.0) as `OpExtInst Pow` (verified in SPIR-V); x*x*x is two
 *      OpFMuls. A Pow on RDNA is exp2+log2, i.e. two quarter-rate ops.
 *   2. Every quarter-rate op (rsqrt, rcp, exp2, log2, sin/cos, atan2, pow)
 *      is named in a comment with its cheap replacement.
 *   3. Scalar `?:` emits OpBranchConditional + OpPhi in SPIR-V. `select()`
 *      emits OpSelect. All helpers here are branch-free: one OpSelect per
 *      decision, no control flow, no wave divergence.
 *   4. native atan2 is ONE OpExtInst Atan2 in SPIR-V but the driver expands
 *      it to a long sequence (~109 instructions on GCN/RDNA per
 *      skills/vulkan-shader-opt). SPIR-V op counts understate that cost.
 *      mu_atan2_fast() is 19 net SPIR-V arithmetic ops, 0 branches, with a
 *      measured 0.00021 rad (0.012 deg) max error over the full circle.
 *   5. Every constant here is used by a helper below.
 *   6. slangc -O3 does NOT form an FMA from `a*b+c` (verified: OpFMul +
 *      OpFAdd). `mad()` emits one `OpExtInst Fma` — one op in the module and
 *      one v_fma_f32 in ACO. Every polynomial and *scale+bias below uses it.
 *   7. `rcp()` emits `OpFDiv 1.0 x`, which NIR folds to `frcp`
 *      (nir_opt_algebraic.py:1917) and ACO to v_rcp_f32. NOTE: on this driver
 *      ac_nir.c:60 sets `lower_fdiv = true`, so a plain `a/b` is already
 *      fmul(a,frcp(b)); rcp() is a portability/intent win, NOT an ISA win on
 *      RADV. Hoisting a VECTOR divide to one scalar reciprocal IS a win.
 *   8. `sign()` is one OpExtInst FSign in SPIR-V but ACO's nir_op_fsign is a
 *      multiply by +Inf + a clamp (2 vector ops and a constant). Need a sign
 *      test? compare. Need the sign bit? use a bit operation.
 *   9. step/lerp is branch-free in SPIR-V, but RADV flattens an `if` with <=8
 *      ALU ops per side anyway (radv_shader.c:209-215). We use select for a
 *      guaranteed front-end form and for portable wave behaviour — do not
 *      claim it is fewer ISA instructions on RADV.
 *  10. An `_SRGB` image view decodes in the sampler for ZERO ALU; the exact
 *      transfer below exists only for procedural colours and swapchain writes.
 *      `pow(c,2.2)` is not sRGB and costs two quarter-rate ops (04 §3).
 *      Texture2D.Load is OpImageFetch: no sampler, no derivatives.
 * ==========================================================================*/
#ifndef MU_COMMON_SLANG
#define MU_COMMON_SLANG

#define MU_PI          3.14159265358979323846
#define MU_2PI         6.28318530717958647693
#define MU_INV_PI      0.31830988618379067154
#define MU_INV_2PI     0.15915494309189533577
#define MU_HALF_PI     1.57079632679489661923
#define MU_EPS         1e-6
#define MU_EPS_SQ      (1e-6 * 1e-6)
#define MU_GOLDEN_ANG  2.399963229728653
#define MU_LOG2_E      1.4426950408889634
#define MU_LN2         0.6931471805599453
/* README.md: the engine installs a negative-height viewport, so clip-space
 * y = +1 lands at uv.y = 0. Engines with positive-height viewport use (0.5,0.5). */
#define MU_NDC_TO_UV   float2(0.5, -0.5)

/* ------------------------------------------------------------------ scalar */
float  mu_saturate(float x)  { return clamp(x, 0.0, 1.0); }
float2 mu_saturate(float2 x) { return clamp(x, 0.0, 1.0); }
float3 mu_saturate(float3 x) { return clamp(x, 0.0, 1.0); }
float4 mu_saturate(float4 x) { return clamp(x, 0.0, 1.0); }

float mu_max3(float3 v) { return max(v.x, max(v.y, v.z)); }
float mu_min3(float3 v) { return min(v.x, min(v.y, v.z)); }
float mu_length2(float3 v) { return dot(v, v); }

/* --------------------------------------------- fused arithmetic (skill 08) */
/* slangc 2026.21, -target spirv -O3: `a*b+c` survives as OpFMul + OpFAdd;
 * `mad(a,b,c)` is ONE `OpExtInst Fma` (verified with spirv-dis). ACO emits
 * v_fma_f32 for both, but only the mad() spelling guarantees the IR contract
 * on every backend and saves one SPIR-V instruction here. */
float  mu_mad(float  a, float  b, float  c) { return mad(a, b, c); }
float2 mu_mad(float2 a, float2 b, float2 c) { return mad(a, b, c); }
float3 mu_mad(float3 a, float3 b, float3 c) { return mad(a, b, c); }
float4 mu_mad(float4 a, float4 b, float4 c) { return mad(a, b, c); }
/* Scalar-first form: a*s + c in one vector Fma. Collapse scalars before you
 * touch a vector (skill 02 §2). */
float2 mu_mads(float2 a, float s, float2 c) { return mad(a, float2(s), c); }
float3 mu_mads(float3 a, float s, float3 c) { return mad(a, float3(s), c); }
float4 mu_mads(float4 a, float s, float4 c) { return mad(a, float4(s), c); }
/* Half-texel rounding bias for every unorm/snorm encoder: x*scale + 0.5 in
 * ONE OpExtInst Fma. slangc will not do this from `x*scale + 0.5`. */
float  mu_round_bias(float  x, float scale) { return mad(x, scale, 0.5); }
float2 mu_round_bias(float2 x, float scale) { return mu_mads(x, scale, float2(0.5)); }
float3 mu_round_bias(float3 x, float scale) { return mu_mads(x, scale, float3(0.5)); }
float4 mu_round_bias(float4 x, float scale) { return mu_mads(x, scale, float4(0.5)); }

/* Explicit reciprocal. Slang rcp(x) -> `OpFDiv %float 1.0 x`; NIR rewrites
 * fdiv(1,a) -> frcp (nir_opt_algebraic.py:1917) and ACO selects v_rcp_f32.
 * HONEST LIMIT: ac_nir.c:60 sets lower_fdiv=true on RADV, so `a/b` also
 * becomes fmul(a,frcp(b)). Use mu_rcp to hoist a per-component divide to one
 * scalar reciprocal, and for portability — not to claim a RADV speedup. */
float mu_rcp(float x) { return rcp(x); }

/* sign() is a quarter-rate-looking trap: OpExtInst FSign in SPIR-V, but
 * v_mul_legacy_f32 + v_med3_f32 + a constant in ACO (skill 02 §4). */
float  mu_sign_nz(float x)  { return select(x >= 0.0, 1.0, -1.0); }  /* +/-1, never 0 */
float2 mu_sign_nz(float2 x) { return float2(mu_sign_nz(x.x), mu_sign_nz(x.y)); }
uint   mu_sign_bit(float x) { return asuint(x) & 0x80000000u; }
float  mu_flip_sign(float x, uint sbit)  { return asfloat(asuint(x) ^ sbit); }
float3 mu_flip_sign(float3 x, uint sbit) { return asfloat(asuint(x) ^ uint3(sbit)); }
/* Exact sign including 0, for the rare case that needs it: 2 compares,
 * 2 selects, no extended instruction. */
float  mu_sign_exact(float x) { return select(x > 0.0, 1.0, select(x < 0.0, -1.0, 0.0)); }

float mu_remap(float x, float lo, float hi)               { return (x - lo) / (hi - lo); }
float mu_remap_premul(float x, float lo, float inv_range) { return (x - lo) * inv_range; }
float mu_remap01(float x, float lo, float inv_range)      { return mu_saturate((x - lo) * inv_range); }

/* Branch-free smoothstep on an already-normalized [0,1] value: x2*(3-2x). */
float mu_smooth01(float x) { float t = mu_saturate(x); return t * t * (3.0 - 2.0 * t); }
float mu_linearstep(float e0, float e1, float x) { return mu_saturate((x - e0) / (e1 - e0)); }

/* Division guards. A divide that may be zero costs an OpSelect on top of the
 * reciprocal; prefer passing a scale you already know is positive and use the
 * multiply form (mu_safe_rcp(x) * a) once, outside the loop. */
float mu_safe_div(float a, float b) { return a / (b + select(b >= 0.0, MU_EPS, -MU_EPS)); }
float mu_safe_rcp(float x)          { return rcp(x + select(x >= 0.0, MU_EPS, -MU_EPS)); }
/* rsqrt is ONE quarter-rate op; 1/sqrt is rsqrt + a reciprocal (two). */
float mu_rsqrt(float x)             { return rsqrt(max(x, MU_EPS_SQ)); }

float3 mu_safe_normalize(float3 v)  { return v * mu_rsqrt(dot(v, v)); }
float2 mu_safe_normalize(float2 v)  { return v * rsqrt(max(dot(v, v), MU_EPS_SQ)); }

/* --------------------------------------------------------------- powers */
float mu_pow2(float x) { return x * x; }
float mu_pow3(float x) { return x * x * x; }
float mu_pow4(float x) { float x2 = x * x; return x2 * x2; }
float mu_pow5(float x) { float x2 = x * x; return x2 * x2 * x; }
float mu_pow6(float x) { float x2 = x * x; return x2 * x2 * x2; }
float mu_pow8(float x) { float x2 = x * x; float x4 = x2 * x2; return x4 * x4; }

/* Non-constant exponent, written explicitly so the two quarter-rate ops are
 * visible in the source instead of hiding inside an OpExtInst Pow. */
float mu_pow(float x, float y) { return exp2(y * log2(max(x, MU_EPS_SQ))); }
float mu_exp_neg(float x)      { return exp2(-x * MU_LOG2_E); }   /* 1 quarter-rate op */
float mu_fast_exp(float x)     { return exp2(x * MU_LOG2_E); }    /* 1 quarter-rate op */
float mu_fast_log(float x)     { return log2(max(x, MU_EPS_SQ)) * MU_LN2; } /* 1 quarter-rate op */

/* Gaussian without an exp: exp(-x*x/(2s*s)) is two quarter-rate ops. This
 * rational form is 1 reciprocal and is exact enough for splatting/soft masks. */
float mu_gauss_approx(float x, float inv_sigma)
{
    float t = x * inv_sigma;
    return 1.0 / (1.0 + t * t * 0.5 + t * t * t * t * 0.125);
}

/* --------------------------------------------------------------- trig */
/* sincos(a,s,c) lowers to OpExtInst Sin + OpExtInst Cos: two quarter-rate
 * ops. Calling sin(a) and cos(a) separately produces the same two. The win is
 * that the compiler keeps the argument reduction once; write it as one call so
 * the intent survives every backend. */
float2 mu_sincos(float a) { float s, c; sincos(a, s, c); return float2(s, c); }

/* Odd polynomial on |x| <= 1: x*(1 + x2*(-0.327622764 + x2*(0.15931422
 *      - 0.0464964749*x2))). At x=1 it gives 0.78518 (pi/4 = 0.78540).
 *      Fold |x|>1 to atan(1/|x|) first — the polynomial is not valid there. */
float mu_atan_fast(float x)
{
    float ax = abs(x);
    float t  = min(ax, 1.0) * mu_rcp(max(ax, 1.0) + MU_EPS);  /* 1 v_rcp_f32 */
    float t2 = t * t;                                     /* multiply form */
    float r  = mu_mad(mu_mad(mu_mad(-0.0464964749, t2, 0.15931422),
                             t2, -0.327622764) * t2, t, t);
    r = select(ax > 1.0, MU_HALF_PI - r, r);              /* OpSelect */
    return select(x < 0.0, -r, r);                        /* OpSelect */
}

/* Full-plane atan2: fold to the first octant with min/max (two selects, no
 * branch), restore the quadrants, 1 reciprocal. 19 SPIR-V arithmetic ops at
 * -O3 vs one OpExtInst Atan2 that the driver expands to ~109 instructions.
 * Measured max error 0.00021 rad (0.012 deg) over the full circle. */
float mu_atan2_fast(float y, float x)
{
    float ax = abs(x);
    float ay = abs(y);
    float t  = min(ax, ay) * mu_rcp(max(ax, ay) + MU_EPS);  /* 1 v_rcp_f32 */
    float t2 = t * t;
    float r  = mu_mad(mu_mad(mu_mad(-0.0464964749, t2, 0.15931422),
                             t2, -0.327622764) * t2, t, t);
    r = select(ay > ax, MU_HALF_PI - r, r);
    r = select(x < 0.0, MU_PI - r, r);
    r = select(y < 0.0, -r, r);
    return r;
}

/* Facing yaw in the XZ plane — the gameplay-angle case where 109 instructions
 * per instance per frame is easy to avoid. */
float mu_yaw(float3 dir) { return mu_atan2_fast(dir.x, dir.z); }

/* ---------------------------------------------------------------- colour */
float mu_luma709(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float mu_luma601(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

/* EXACT sRGB: 1 vector OpExtInst Pow (3 quarter-rate lane-ops, exp2+log2 each)
 * plus step/lerp = 7 net SPIR-V ops. Use it only when you are the transfer
 * function of record (swapchain write); the polynomial below is 6 ops. */
float3 mu_srgb_to_linear(float3 c)
{
    float3 lo = c * (1.0 / 12.92);
    float3 hi = pow((c + 0.055) * (1.0 / 1.055), 2.4);
    return lerp(hi, lo, step(c, 0.04045));   /* step+lerp: no bool3 conversion */
}

/* Polynomial fit to the exact curve: measured max abs error 0.00167 over
 * [0,1] (mu/tests/validate_math.py), three FMAs, zero quarter-rate ops.
 * This is the one to use on every texture read when a shader does not need
 * bit-exact transfer. */
float3 mu_srgb_to_linear_approx(float3 c)
{
    return mu_mad(c, mu_mad(c, mu_mad(c, float3(0.305306011),
                                       float3(0.682171111)),
                            float3(0.012522878)), float3(0.0));
}

/* 2.2-gamma forms: TWO quarter-rate ops (log2 + exp2), written out so the
 * cost is visible. `pow(c, 2.2)` is an OpExtInst Pow instead. */
float3 mu_linear_to_srgb_fast(float3 c) { return exp2(log2(max(mu_saturate(c), MU_EPS_SQ)) * (1.0 / 2.2)); }
float3 mu_srgb_to_linear_fast(float3 c) { return exp2(log2(max(mu_saturate(c), MU_EPS_SQ)) * 2.2); }

/* Lift / gamma / gain, branch-free. */
float3 mu_lift_gamma_gain(float3 c, float3 lift, float3 gain, float gamma)
{
    /* Do not hide a non-constant exponent in Pow: Log2 + Exp2 are the two
     * quarter-rate ops actually issued, written out so the cost is visible. */
    return exp2(log2(max(mu_saturate(lerp(lift, gain, mu_saturate(c))),
                         float3(MU_EPS_SQ))) * gamma);
}

/* ---------------------------------------------------------------- hashing */
/* PCG3D: three independent streams from one call. Use one component; calling
 * it three times for one value is three times the work. */
uint3 mu_pcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

float mu_hash01(uint3 v) { return float(mu_pcg3d(v).x) * (1.0 / 4294967296.0); }
float2 mu_hash02(uint3 v) { return float2(mu_pcg3d(v).xy) * (1.0 / 4294967296.0); }
float3 mu_hash03(uint3 v) { return float3(mu_pcg3d(v)) * (1.0 / 4294967296.0); }

/* Interleaved gradient noise: one fma chain, no fetch. Preferred over a hash
 * for SSAO rotation and dithering. */
float mu_ign(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));

}

/* 4x4 Bayer as arithmetic (no constant-array fetch).
 * M2(a,b) = 2*(a^b) + b gives [[0,2],[3,1]]; the recursive construction is
 *   M4(x,y) = 4*M2(x&1, y&1) + M2(x>>1, y>>1)
 * which is exactly {0,8,2,10 / 12,4,14,6 / 3,11,1,9 / 15,7,13,5}.
 * Verified against that table for all 16 pixels in mu/tests/probe.slang. */
float mu_bayer4(float2 pixel)
{
    uint2 p = uint2(pixel) & 3u;
    uint x = p.x, y = p.y;
    uint lo = 2u * ((x ^ y) & 1u) + (y & 1u);
    uint hi = 2u * (((x >> 1u) ^ (y >> 1u)) & 1u) + ((y >> 1u) & 1u);
    return float(4u * lo + hi) * (1.0 / 16.0);
}

/* Radical inverse / Hammersley for low-discrepancy sample sets. */
float mu_radical_inverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
float2 mu_hammersley(uint i, uint n) { return float2(float(i) / float(n), mu_radical_inverse(i)); }
/* Precompute 1/n once outside a sample loop; the inverse is uniform, so it is
 * one scalar reciprocal for the whole wave instead of one per sample. */
float2 mu_hammersley_inv(uint i, float inv_n)
{
    return float2(float(i) * inv_n, mu_radical_inverse(i));
}

#endif /* MU_COMMON_SLANG */
