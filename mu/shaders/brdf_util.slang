/* mu/brdf_util.slang — frames, sampling and the small pieces BRDF code shares.
 * ============================================================================
 *  Nothing here allocates or binds anything. Every function is a leaf: it
 *  takes values, returns values, and the compiler can inline it into the
 *  caller's register budget. Keep it that way — the moment a helper takes a
 *  Texture2D it belongs in lighting.slang or post.slang instead.
 * ==========================================================================*/
#ifndef MU_BRDF_UTIL_SLANG
#define MU_BRDF_UTIL_SLANG

#include "common.slang"

/* ------------------------------------------------------------ roughness */
/* Perceptual roughness -> GGX alpha. Squaring is what makes the parameter
 * perceptually linear; clamping at 1e-4 keeps D_GGX finite at mirror values. */
float mu_roughness_to_alpha(float r) { return max(r * r, 1e-4); }
float mu_alpha_to_roughness(float a) { return sqrt(max(a, 1e-4)); }  /* 1 quarter-rate sqrt */
/* Disney's "hot" remap for layered materials (clearcoat over a rough base). */
float mu_perceptual_roughness(float r, float k) { return mu_saturate(r * (1.0 + k)); }

/* ------------------------------------------------------- tangent frames */
/* Duff et al. 2017, "Building an Orthonormal Basis, Revisited": branchless,
 * no normalize (so no rsqrt), 1 reciprocal, ~12 scalar ops. The sign trick
 * keeps it stable at n.z = -1 where the naive cross-with-up basis blows up. */
void mu_onb(float3 n, out float3 t, out float3 b)
{
    float s  = select(n.z >= 0.0, 1.0, -1.0);        /* OpSelect */
    float a  = -1.0 / (s + n.z);                      /* 1 rcp */
    float bb = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
    b = float3(bb, s + n.y * n.y * a, -n.y);
}

float3 mu_tangent_to_world(float3 n, float3 v)
{
    float3 t, b; mu_onb(n, t, b);
    return mu_mads(n, v.z, mu_mads(b, v.y, t * v.x));   /* 2 Fma, 1 mul */
}

float3 mu_world_to_tangent(float3 n, float3 v)
{
    float3 t, b; mu_onb(n, t, b);
    return float3(dot(v, t), dot(v, b), dot(v, n));
}

/* TBN from a vertex tangent (the MikkTSpace case). The sign is carried in
 * tangent.w as +1/-1; rebuilding b from cross(n,t)*w avoids a normalize.
 * skill 02 §3: MikkTSpace tangents are ALREADY unit — calling normalize()
 * here spends a quarter-rate rsqrt + 3 muls for nothing. mu_tbn_unit() is
 * the hot path; mu_tbn() keeps the defensive form for untrusted data. */
float3x3 mu_tbn(float3 n, float4 tangent)
{
    float3 t = normalize(tangent.xyz);              /* 1 rsqrt */
    float3 b = cross(n, t) * tangent.w;
    return float3x3(t, b, n);
}
float3x3 mu_tbn_unit(float3 n, float4 tangent)
{
    float3 t = tangent.xyz;                      /* no normalize: already unit */
    float3 b = cross(n, t) * tangent.w;
    return float3x3(t, b, n);
}

/* ------------------------------------------------------ sample generators */
/* Cosine-weighted hemisphere, tangent space. sqrt is one quarter-rate op,
 * mu_sincos is two (Sin + Cos). Use mu_sincos rather than sin()+cos(). */
float3 mu_sample_cosine_hemisphere(float2 u)
{
    float r     = sqrt(u.x);                        /* 1 quarter-rate sqrt */
    float phi   = MU_2PI * u.y;
    float2 sinc = mu_sincos(phi);                   /* 2 quarter-rate ops */
    return float3(r * sinc.y, r * sinc.x, sqrt(max(1.0 - u.x, 0.0)));
}

/* Uniform sphere, no trig: Archimedes' hat-box theorem. */
float3 mu_sample_sphere(float2 u)
{
    float z   = 1.0 - 2.0 * u.x;
    float r   = sqrt(max(1.0 - z * z, 0.0));       /* 1 quarter-rate sqrt */
    float phi = MU_2PI * u.y;
    float2 sc = mu_sincos(phi);                     /* 2 quarter-rate ops */
    return float3(r * sc.y, r * sc.x, z);
}

/* GGX NDF importance sample (tangent space, +Z is the normal). `alpha` is
 * squared perceptual roughness, not roughness. 2 sqrt + sincos. */
float3 mu_sample_ggx(float2 u, float alpha)
{
    float a2      = alpha * alpha;
    float cosT2   = (1.0 - u.y) / max(1.0 + (a2 - 1.0) * u.y, MU_EPS);
    float cosT    = sqrt(cosT2);                    /* 1 quarter-rate sqrt */
    float sinT    = sqrt(max(1.0 - cosT2, 0.0));   /* 1 quarter-rate sqrt */
    float2 sc     = mu_sincos(MU_2PI * u.x);        /* 2 quarter-rate ops */
    return float3(sinT * sc.y, sinT * sc.x, cosT);
}

/* Visible-Normal Distribution Function (Heitz 2018) sample in tangent space.
 * Costs more than mu_sample_ggx but removes the wasted samples below the
 * horizon — worth it for a 1-4 sample real-time reflection probe convolution.
 * `V` must be in the same tangent space. */
float3 mu_sample_vndf_ggx(float2 u, float alpha, float3 V)
{
    float3 Vh  = normalize(float3(alpha * V.x, alpha * V.y, V.z));  /* 1 rsqrt */
    float  len2 = mu_mad(Vh.x, Vh.x, Vh.y * Vh.y);      /* 1 Fma, 1 mul */
    float3 T1  = select(len2 > 0.0,
                        float3(-Vh.y, Vh.x, 0.0) * rsqrt(max(len2, MU_EPS_SQ)),
                        float3(1.0, 0.0, 0.0));
    float3 T2  = cross(Vh, T1);

    float r     = sqrt(u.x);                       /* 1 quarter-rate sqrt */
    float phi   = MU_2PI * u.y;
    float2 sc   = mu_sincos(phi);                  /* 2 quarter-rate ops */
    float  t1   = r * sc.y;
    float  t2   = r * sc.x;
    float  s    = 0.5 * (1.0 + Vh.z);
    t2          = lerp(sqrt(max(1.0 - t1 * t1, 0.0)), t2, s);  /* 1 sqrt */
    float3 Nh   = mu_mads(T1, t1, mu_mads(T2, t2,
                       sqrt(max(1.0 - t1 * t1 - t2 * t2, 0.0)) * Vh));
    float3 Ne   = normalize(float3(alpha * Nh.x, alpha * Nh.y, max(Nh.z, 0.0))); /* 1 rsqrt */
    return Ne;
}

/* PDF of mu_sample_ggx (reflection). D*NoH/(4*VoH) simplified to D*NoH/.. */
float mu_pdf_ggx(float NoH, float VoH, float alpha)
{
    float a2 = alpha * alpha;
    float d  = NoH * NoH * (a2 - 1.0) + 1.0;
    float D  = a2 * MU_INV_PI / max(d * d, MU_EPS_SQ);
    return D * NoH / max(4.0 * VoH, MU_EPS);
}

/* Concentrate a sample set around a lobe without an extra random stream:
 * the golden-angle spiral. Used by mu_ssao_kernel in post.slang. */
float3 mu_golden_hemisphere(uint i, uint count, float angle_offset)
{
    float f  = (float(i) + 0.5) / float(count);
    float r  = sqrt(f);                            /* 1 quarter-rate sqrt */
    float a  = float(i) * MU_GOLDEN_ANG + angle_offset;
    float2 sc = mu_sincos(a);                      /* 2 quarter-rate ops */
    return float3(r * sc.y, r * sc.x, sqrt(max(1.0 - f, 0.0)));
}

/* ------------------------------------------------------------ convolution */
/* solid-angle -> mip level for a cubemap prefilter, given its face size. */
float mu_mip_from_solid_angle(float omega, uint cubemap_size, uint num_samples)
{
    float pdf = max(omega * MU_INV_PI, MU_EPS);
    float sa_texel = 4.0 * MU_PI / (6.0 * float(cubemap_size * cubemap_size));
    float sa_sample = 1.0 / (float(num_samples) * pdf + MU_EPS);
    return 0.5 * log2(sa_sample / sa_texel);       /* 1 quarter-rate log2 */
}

#endif /* MU_BRDF_UTIL_SLANG */
