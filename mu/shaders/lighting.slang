/* mu/lighting.slang — physically based direct/indirect lighting, plus the
 * ============================================================================
 *  layered extras a stylized PBR renderer actually ships.
 *
 *  Design rules:
 *   - Every BRDF term is a leaf function with an explicit denominator. Nothing
 *     hides a division inside a helper you cannot see.
 *   - Roughness in the public API is PERCEPTUAL roughness (the value an artist
 *     sets). Convert once with mu_roughness_to_alpha(); never square it twice.
 *   - Visibility already includes the 1/(4 NoL NoV) factor. mu_specular_brdf
 *     is F*V*D, so the caller multiplies by radiance*NoL and nothing else.
 *   - No loops over lights here. Callers build their own light lists; the
 *     library supplies the per-light math so the loop can be unrolled,
 *     clustered, or tiled without being rewritten.
 *   - Quarter-rate ops are counted in comments (rsqrt/rcp, exp2/log2, pow).
 * ==========================================================================*/
#ifndef MU_LIGHTING_SLANG
#define MU_LIGHTING_SLANG

#include "common.slang"
#include "brdf_util.slang"

#define MU_F0_DIELECTRIC 0.04

struct MuSurface
{
    float3 albedo;              /* linear, [0,1]                              */
    float3 N;                   /* normalized shading normal                  */
    float3 V;                   /* normalized surface -> eye                  */
    float  roughness;           /* perceptual                                */
    float  metalness;
    float3 f0;                  /* spectral base reflectance                 */
    float  alpha;               /* GGX alpha = roughness^2                    */
    float3 diffuse_color;       /* albedo*(1-metalness)                       */
    float  clearcoat;           /* 0..1 weight                               */
    float  clearcoat_roughness;
    float3 sheen_color;
    float  sheen_roughness;
};

/* Reflectance -> F0 for non-metals; 0.5 (F0=0.04) is the glTF default. */
float3 mu_f0_from_reflectance(float reflectance)
{
    return float3(0.16 * reflectance * reflectance);
}
float mu_ior_to_f0(float ior) { float f = (ior - 1.0) / (ior + 1.0); return f * f; }

MuSurface mu_surface(float3 albedo, float3 N, float3 V, float roughness, float metalness)
{
    MuSurface s;
    s.albedo              = albedo;
    s.N                   = N;
    s.V                   = V;
    s.roughness           = mu_saturate(roughness);
    s.metalness           = mu_saturate(metalness);
    s.f0                  = lerp(float3(MU_F0_DIELECTRIC), albedo, s.metalness);
    s.alpha               = mu_roughness_to_alpha(s.roughness);
    s.diffuse_color       = albedo * (1.0 - s.metalness);
    s.clearcoat           = 0.0;
    s.clearcoat_roughness = 0.03;
    s.sheen_color         = float3(0.0);
    s.sheen_roughness     = 0.3;
    return s;
}

/* -------------------------------------------------------------- Fresnel */
/* Schlick. mu_pow5 is x*x*x*x*x — five FMuls, NOT an OpExtInst Pow. */
float3 mu_f_schlick(float3 f0, float VoH)
{
    float f = mu_pow5(1.0 - mu_saturate(VoH));
    return f0 + (1.0 - f0) * f;
}
float3 mu_f_schlick_f90(float3 f0, float3 f90, float VoH)
{
    return f0 + (f90 - f0) * mu_pow5(1.0 - mu_saturate(VoH));
}
/* Spherical Gaussian fit for the roughness-dependent grazing reflectance used
 * on the environment term. max() uses the roughness to clamp f90. */
float3 mu_f_schlick_roughness(float3 f0, float roughness, float NoV)
{
    float f = mu_pow5(1.0 - mu_saturate(NoV));
    return f0 + (max(float3(1.0 - roughness), f0) - f0) * f;
}

/* ---------------------------------------------------------- distribution */
/* GGX / Trowbridge-Reitz NDF. `alpha` is squared roughness.
 * Form: a2/(pi * (NoH^2*(a2-1)+1)^2) — 1 reciprocal, no pow, no sqrt. */
float mu_d_ggx(float NoH, float alpha)
{
    float a2 = alpha * alpha;
    float d  = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 * MU_INV_PI / max(d * d, MU_EPS_SQ);
}
/* Anisotropic GGX. at / ab are the tangent/bitangent alphas; Ht/Hb are the
 * half vector in the tangent frame (mu_world_to_tangent, brdf_util.slang). */
float mu_d_ggx_aniso(float NoH, float Ht, float Hb, float at, float ab)
{
    float3 v = float3(Ht / at, Hb / ab, NoH);
    float v2 = dot(v, v);
    return MU_INV_PI / max(at * ab * v2 * v2, MU_EPS_SQ);
}
/* Charlie sheen NDF (Estevez & Kulla): one pow = 2 quarter-rate ops. */
float mu_d_charlie(float NoH, float roughness)
{
    float a     = max(roughness, 0.07);
    float inv_a = 1.0 / a;                          /* 1 rcp */
    float sin2h = max(1.0 - NoH * NoH, 0.0078125);
    return (2.0 + inv_a) * mu_pow(sin2h, 0.5 * inv_a) * (0.5 * MU_INV_PI);
}

/* ------------------------------------------------------------ visibility */
/* Smith height-correlated GGX visibility, includes 1/(4 NoL NoV).
 * 2 sqrt (quarter-rate) + 1 rcp. Exact form used by Filament/Frostbite. */
float mu_v_smith_ggx(float NoV, float NoL, float alpha)
{
    float a2 = alpha * alpha;
    float gv = NoL * sqrt(max(NoV * NoV * (1.0 - a2) + a2, 0.0));
    float gl = NoV * sqrt(max(NoL * NoL * (1.0 - a2) + a2, 0.0));
    return 0.5 / max(gv + gl, MU_EPS);
}
/* Karis' mobile approximation: 1 rcp, no sqrt. ~2% error, use when the shader
 * is already bandwidth bound or on a low-tier device. */
float mu_v_smith_ggx_fast(float NoV, float NoL, float alpha)
{
    float k  = alpha * 0.5;
    float gv = NoV * (NoL * (1.0 - k) + k);
    float gl = NoL * (NoV * (1.0 - k) + k);
    return 0.5 / max(gv + gl, MU_EPS);
}
/* Kelemen/Szirmay-Kalos visibility for the clearcoat lobe: 1 rcp, no sqrt. */
float mu_v_kelemen(float VoH) { return 0.25 / max(VoH * VoH, MU_EPS); }

/* --------------------------------------------------------------- diffuse */
float3 mu_lambert_diffuse(float3 albedo) { return albedo * MU_INV_PI; }

/* Disney/Burley diffuse. Two mu_pow5 chains; no retroreflection division. */
float mu_burley_diffuse(float NoV, float NoL, float VoH, float roughness)
{
    float fd90 = 0.5 + 2.0 * VoH * VoH * roughness;
    float fv   = 1.0 + (fd90 - 1.0) * mu_pow5(1.0 - NoV);
    float fl   = 1.0 + (fd90 - 1.0) * mu_pow5(1.0 - NoL);
    return fv * fl * MU_INV_PI;
}
/* Frostbite's energy-normalized diffuse: cheaper than Burley, no VoH term. */
float mu_diffuse_frostbite(float NoV, float NoL, float VoH, float roughness)
{
    float energy_bias = lerp(0.0, 0.5, roughness);
    float energy_factor = lerp(1.0, 1.0 / 1.51, roughness);
    float fd90 = energy_bias + 2.0 * VoH * VoH * roughness;
    float f0 = 1.0;
    float light_scatter = f0 + (fd90 - f0) * mu_pow5(1.0 - NoL);
    float view_scatter  = f0 + (fd90 - f0) * mu_pow5(1.0 - NoV);
    return light_scatter * view_scatter * energy_factor * MU_INV_PI;
}

/* ----------------------------------------------------------- direct BRDF */
/* F*V*D for the specular lobe. Multiply by radiance*NoL at the call site. */
float3 mu_specular_brdf(float3 f0, float NoV, float NoL, float NoH, float VoH, float alpha)
{
    float3 F = mu_f_schlick(f0, VoH);
    float  V = mu_v_smith_ggx(NoV, NoL, alpha);
    float  D = mu_d_ggx(NoH, alpha);
    return F * (V * D);
}

/* One punctual light, no attenuation applied (the caller supplies
 * `radiance` already attenuated and shadowed). Returns linear radiance. */
float3 mu_pbr_direct(MuSurface s, float3 L, float3 radiance)
{
    float3 H   = mu_safe_normalize(s.V + L);       /* 1 rsqrt */
    float  NoL = mu_saturate(dot(s.N, L));
    float  NoV = max(dot(s.N, s.V), MU_EPS);
    float  NoH = mu_saturate(dot(s.N, H));
    float  VoH = mu_saturate(dot(s.V, H));
    float active = step(MU_EPS, NoL);            /* no branch, no divergence */

    float3 F   = mu_f_schlick(s.f0, VoH);
    float3 Fr  = F * (mu_v_smith_ggx(NoV, NoL, s.alpha) * mu_d_ggx(NoH, s.alpha));
    float  kd  = (1.0 - F.x) * (1.0 - s.metalness);
    float3 Fd  = s.diffuse_color * mu_burley_diffuse(NoV, NoL, VoH, s.roughness) * kd;
    /* skill 02 §2: collapse the two uniform scalars BEFORE touching the
     * vector; one vector-times-scalar instead of two. */
    return (Fd + Fr) * (radiance * (NoL * active));
}

/* Split of diffuse/specular for clustered/tiled light accumulation that wants
 * to apply SSS or a separate specular occlusion afterwards. */
void mu_pbr_direct_split(MuSurface s, float3 L, float3 radiance,
                         out float3 diffuse, out float3 specular)
{
    float3 H   = mu_safe_normalize(s.V + L);
    float  NoL = mu_saturate(dot(s.N, L));
    float  NoV = max(dot(s.N, s.V), MU_EPS);
    float  NoH = mu_saturate(dot(s.N, H));
    float  VoH = mu_saturate(dot(s.V, H));
    float3 F   = mu_f_schlick(s.f0, VoH);
    float  vis = mu_v_smith_ggx(NoV, NoL, s.alpha) * mu_d_ggx(NoH, s.alpha);
    float  kd  = (1.0 - F.x) * (1.0 - s.metalness);
    diffuse  = s.diffuse_color * mu_burley_diffuse(NoV, NoL, VoH, s.roughness) * kd * radiance * NoL;
    specular = F * vis * radiance * NoL;
}

/* ---------------------------------------------------------- light falloff */
/* Inverse-square with a smooth window to zero at `radius`. No hard cutoff
 * means no discontinuity when a light crosses a cluster boundary. */
float mu_point_attenuation(float distance2, float radius)
{
    float d2 = max(distance2, MU_EPS_SQ);
    float w  = mu_saturate(1.0 - mu_pow2(d2 / (radius * radius)));  /* (d/r)^4 */
    return (w * w) / d2;
}
/* Physically correct spot: 1/r^2 plus an inner/outer cone smoothstep. */
float mu_spot_attenuation(float cos_theta, float cos_outer, float cos_inner)
{
    return mu_smooth01(mu_linearstep(cos_outer, cos_inner, cos_theta));
}
/* Distance + cone in one call, the common case. `to_light` is unnormalized. */
float3 mu_punctual_light(float3 to_light, float radius, float3 spot_dir,
                         float cos_outer, float cos_inner, float3 radiance)
{
    float d2 = dot(to_light, to_light);
    float3 L = to_light * rsqrt(max(d2, MU_EPS_SQ));   /* 1 quarter-rate rsqrt */
    float c  = dot(-L, spot_dir);
    return radiance * (mu_point_attenuation(d2, radius) * mu_spot_attenuation(c, cos_outer, cos_inner));
}

/* ------------------------------------------------------- indirect / env */
/* Karis' analytic split-sum DFG approximation (UE4 mobile). Returns
 * (scale, bias) for f0; replaces an env-BRDF LUT fetch with ~10 ALU ops and
 * zero texture reads. Accurate for GGX + Schlick; exp2 is 1 quarter-rate op. */
float2 mu_env_brdf_approx(float NoV, float roughness)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572,  0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.04,  -0.04);
    /* REVERTED mu_mads(): with a constant roughness slangc folds this whole
     * expression to a constant, but it does NOT fold OpExtInst Fma. The mad
     * form measured 8 -> 13 net ops (measure.py). No win, so revert. */
    float4 r    = roughness * c0 + c1;
    float  a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;  /* 1 exp2 */
    return float2(-1.04, 1.04) * a004 + r.zw;
}

/* Fdez-Agüera multi-scatter energy compensation. `dfg` comes from
 * mu_env_brdf_approx (or a LUT if you have one). Returns a Fresnel-like
 * multiplier to use INSTEAD of a single F_Schlick on the indirect specular.
 * Without this, rough metals lose up to ~40% of their energy. */
float3 mu_multi_scatter_fresnel(float3 f0, float2 dfg)
{
    float  ess   = dfg.x + dfg.y;
    float  ems   = max(1.0 - ess, 0.0);
    float3 f_avg = f0 + (1.0 - f0) * (1.0 / 21.0);
    float3 f_ss  = dfg.x * f0 + float3(dfg.y);
    float3 f_ms  = f_ss * f_avg / max(float3(1.0) - (1.0 - ems) * f_avg, float3(MU_EPS));
    return f_ss + f_ms * ems;
}

/* Indirect: irradiance (already convolved diffuse) + prefiltered radiance.
 * `ao` is a scalar occlusion; `specular_ao` may be a separate visibility. */
float3 mu_pbr_indirect(MuSurface s, float3 irradiance, float3 prefiltered,
                       float ao, float specular_ao)
{
    float  NoV = max(dot(s.N, s.V), MU_EPS);
    float2 dfg = mu_env_brdf_approx(NoV, s.roughness);
    float3 Fd  = s.diffuse_color * irradiance * MU_INV_PI * ao;
    float3 Fr  = prefiltered * mu_multi_scatter_fresnel(s.f0, dfg) * specular_ao;
    return Fd + Fr;
}

/* -------------------------------------------------------- layered extras */
/* Clearcoat as a second GGX lobe with Kelemen visibility and F0=0.04. One
 * extra D + one rcp + one pow5; call only on surfaces with s.clearcoat>0. */
float3 mu_clearcoat_brdf(MuSurface s, float3 L)
{
    float3 H   = mu_safe_normalize(s.V + L);        /* 1 rsqrt */
    float  NoL = mu_saturate(dot(s.N, L));
    float  NoV = max(dot(s.N, s.V), MU_EPS);
    float  NoH = mu_saturate(dot(s.N, H));
    float  VoH = mu_saturate(dot(s.V, H));
    float  a   = mu_roughness_to_alpha(s.clearcoat_roughness);
    float  D   = mu_d_ggx(NoH, a);
    float  V   = mu_v_kelemen(VoH);
    float  F   = MU_F0_DIELECTRIC + (1.0 - MU_F0_DIELECTRIC) * mu_pow5(1.0 - VoH);
    return float3(s.clearcoat * F * D * V * NoL);
}

/* Sheen lobe (cloth/fabric), Charlie NDF + a simple Neubelt visibility.
 * mu_d_charlie contains 1 pow (2 quarter-rate ops). */
float3 mu_sheen_brdf(MuSurface s, float3 L)
{
    float3 H   = mu_safe_normalize(s.V + L);        /* 1 rsqrt */
    float  NoL = mu_saturate(dot(s.N, L));
    float  NoV = max(dot(s.N, s.V), MU_EPS);
    float  NoH = mu_saturate(dot(s.N, H));
    float  D   = mu_d_charlie(NoH, s.sheen_roughness);
    float  V   = 1.0 / max(4.0 * (NoL + NoV - NoL * NoV), MU_EPS);  /* 1 rcp */
    return s.sheen_color * D * V * NoL;
}

/* Cheap wrapped subsurface for foliage/thin materials: a back-side
 * translucency term with no extra depth or thickness texture. `power` should
 * be small (4..16); use mu_pow4/mu_pow8 for fixed exponents. */
float3 mu_translucency(float3 N, float3 L, float3 V, float3 thickness_color,
                       float scale, float power)
{
    float3 H    = mu_safe_normalize(-L + N * 0.3);   /* 1 rsqrt */
    float  back = mu_saturate(dot(V, -H));
    float  t    = mu_pow(back, power);               /* 2 quarter-rate ops */
    return thickness_color * t * scale;
}

/* ------------------------------------------------------------- occlusion */
/* Horizon-based GTAO-style specular occlusion. Cheap, branch-free, keeps
 * rough surfaces from going unnaturally bright in cavities. */
float mu_specular_occlusion(float NoV, float ao, float roughness)
{
    float f = mu_saturate(NoV + ao);
    return mu_saturate(f * f * (1.0 - roughness) + roughness * ao);
}

#endif /* MU_LIGHTING_SLANG */
