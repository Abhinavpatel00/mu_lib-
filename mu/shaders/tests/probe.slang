/* mu/tests/probe.slang — regression target for the whole mu shader library.
 * ============================================================================
 *  Compile this one file to prove:
 *    (a) every module can be included together with no duplicate symbols,
 *    (b) every public function type-checks and lowers to SPIR-V,
 *    (c) no helper introduces control flow when it promises not to.
 *
 *  Verified with:
 *    cd shaders && slangc mu/tests/probe.slang -target spirv -entry cs \
 *        -stage compute -O3 -I . -o /tmp/mu_probe.spv
 *    spirv-dis /tmp/mu_probe.spv | grep -E 'OpBranch|OpPhi|OpSelectionMerge'
 *
 *  The generic SDF marchers DO contain loops by design (their iteration count
 *  is a runtime uniform), so this probe checks them separately in the README.
 *  The leaf helpers (everything else) must produce zero branch/phi/merge ops.
 *
 *  `out_buf[0].x` is a runtime value on purpose: it stops the optimizer from
 *  constant-folding the entire probe away, which would make the op counts in
 *  README.md meaningless.
 * ==========================================================================*/
#include "mu/common.slang"
#include "mu/packing.slang"
#include "mu/quantize.slang"
#include "mu/brdf_util.slang"
#include "mu/lighting.slang"
#include "mu/toon.slang"
#include "mu/shadows.slang"
#include "mu/noise.slang"
#include "mu/sdf.slang"
#include "mu/post.slang"
#include "mu/dispatch.slang"
#include "mu/wave.slang"
#include "mu/fp16.slang"
#include "mu/format.slang"

Texture2D<float>          g_shadow_depth;
SamplerComparisonState    g_shadow_cmp;
SamplerState              g_shadow_samp;
ByteAddressBuffer         g_packed_bytes;

struct MuProbeField : IMuSdf3
{
    float dist(float3 p)
    {
        return mu_sd_smin(mu_sd_sphere(p, 1.0),
                          mu_sd_box(p, float3(0.8)), 0.25);
    }
};

#define MU_A3(e) acc  += float3(e)
#define MU_A1(e) s    += float(e)

[numthreads(MU_WG_64, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID,
        uniform RWStructuredBuffer<float4> out_buf)
{
    uint  idx = tid.x;
    float x   = out_buf[0].x;                    /* runtime, defeats folding */
    float2 px = float2(x * 100.0, float(idx & 63u));
    float2 p2 = float2(x, 0.37);
    float3 p3 = float3(x, 0.37, 1.2);
    float3 N  = mu_safe_normalize(float3(x, 0.4, 0.5));
    float3 V  = float3(0.0, 0.0, 1.0);
    float3 L  = mu_safe_normalize(float3(0.3, 0.4, 0.7));
    float3 c  = float3(x, 0.4, 0.2) * 2.0;

    float3 acc = float3(0.0);
    float  s   = 0.0;

    /* ---------------------------------------------------------- common.slang */
    MU_A1(mu_saturate(x)); MU_A1(mu_max3(p3)); MU_A1(mu_min3(p3));
    MU_A1(mu_length2(p3)); MU_A1(mu_remap(x, 0.2, 0.8));
    MU_A1(mu_remap_premul(x, 0.2, 2.0)); MU_A1(mu_remap01(x, 0.2, 2.0));
    MU_A1(mu_smooth01(x)); MU_A1(mu_linearstep(0.2, 0.8, x));
    MU_A1(mu_safe_div(x, 0.5)); MU_A1(mu_safe_rcp(x)); MU_A1(mu_rsqrt(x + 0.1));
    MU_A3(mu_safe_normalize(p3)); MU_A3(mu_safe_normalize(p2).xyx);
    MU_A1(mu_pow2(x)); MU_A1(mu_pow3(x)); MU_A1(mu_pow4(x)); MU_A1(mu_pow5(x));
    MU_A1(mu_pow6(x)); MU_A1(mu_pow8(x));
    MU_A1(mu_pow(x, 2.3)); MU_A1(mu_exp_neg(x)); MU_A1(mu_fast_exp(x));
    MU_A1(mu_fast_log(x + 1.0)); MU_A1(mu_gauss_approx(x, 2.0));
    MU_A3(mu_sincos(x).xyx);
    MU_A1(mu_atan_fast(x)); MU_A1(mu_atan2_fast(x, -0.7)); MU_A1(mu_yaw(p3));
    MU_A1(mu_mad(x, 2.0, 0.5)); MU_A3(mu_mad(p3, c, N)); MU_A3(mu_mads(p3, x, N));
    MU_A1(mu_rcp(x + 1.0)); MU_A1(mu_round_bias(x, 255.0));
    MU_A1(mu_sign_nz(x)); MU_A1(mu_sign_exact(x)); MU_A1(mu_sign_bit(x));
    MU_A1(mu_flip_sign(x, 0x80000000u)); MU_A1(mu_hammersley_inv(idx, 1.0/64.0).x);
    MU_A1(mu_luma709(c)); MU_A1(mu_luma601(c));
    MU_A3(mu_srgb_to_linear(float3(x))); MU_A3(mu_srgb_to_linear_approx(float3(x)));
    MU_A3(mu_srgb_to_linear_fast(float3(x))); MU_A3(mu_linear_to_srgb_fast(float3(x)));
    MU_A3(mu_lift_gamma_gain(float3(x), float3(0.0), float3(1.0), 1.0));
    MU_A3(mu_pcg3d(uint3(idx, 1, 2)));
    MU_A1(mu_hash01(uint3(idx, 1, 2))); MU_A1(mu_hash02(uint3(idx, 1, 2)).x);
    MU_A1(mu_hash03(uint3(idx, 1, 2)).x);
    MU_A1(mu_ign(px)); MU_A1(mu_bayer4(px)); MU_A1(mu_radical_inverse(idx));
    MU_A1(mu_hammersley(idx & 63u, 64u).x);

    /* --------------------------------------------------------- packing.slang */
    MU_A1(mu_bits_insert(0x12345678u, 0xABu, 8u, 8u));
    MU_A1(mu_bits_extract(0x12345678u, 8u, 8u));
    MU_A1(mu_bits_extract_signed(0xFFF0u, 0u, 16u));
    MU_A1(mu_pack_4x8(uint4(1, 2, 3, 4)));
    MU_A3(mu_unpack_4x8(0x04030201u).xyz);
    MU_A1(mu_pack_4x8_unorm(float4(x, 0.2, 0.3, 0.4)));
    MU_A3(mu_unpack_4x8_unorm(0x04030201u).xyz);
    MU_A1(mu_pack_2x8_2flags(p2, true, false));
    MU_A1(mu_pack_u16x2(uint2(300, 700)));
    MU_A1(mu_unpack_u16x2(0x00020001u).x);
    MU_A3(mu_unpack_half2(mu_pack_half2(p2)).xyx);
    MU_A1(mu_pack_snorm16(-0.3)); MU_A1(mu_unpack_snorm16(0xD000u));
    MU_A1(mu_pack_float_split(x).x);
    MU_A1(mu_unpack_float_split(mu_pack_float_split(x)));
    MU_A1(mu_byte_swap(0x12345678u));
    MU_A1(mu_load_u32_le(g_packed_bytes, idx * 4u));

    /* -------------------------------------------------------- quantize.slang */
    MU_A1(mu_quantize_unorm(x, 255u)); MU_A1(mu_dequantize_unorm(200u, 255u));
    MU_A1(mu_unorm8_encode(x)); MU_A1(mu_unorm8_decode(200u));
    MU_A1(mu_unorm16_encode(x)); MU_A1(mu_unorm16_decode(40000u));
    MU_A1(mu_snorm8_encode(x)); MU_A1(mu_snorm8_decode(200u));
    MU_A1(mu_pack_rgba8(float4(x))); MU_A3(mu_unpack_rgba8(0x04030201u).xyz);
    MU_A1(mu_pack_rgb10a2(float3(x), 0.5)); MU_A3(mu_unpack_rgb10a2(0x3FFFFFFFu).xyz);
    MU_A1(mu_pack_unorm16x2(p2)); MU_A3(mu_unpack_unorm16x2(0x00020001u).xyx);
    MU_A1(mu_sign_nz(x)); MU_A3(mu_sign_nz(p2).xyx);
    MU_A3(mu_oct_encode(N).xyx); MU_A3(mu_oct_decode(p2));
    MU_A1(mu_pack_normal_oct16(N)); MU_A3(mu_unpack_normal_oct16(0x80008000u));
    MU_A1(mu_pack_normal_oct8(N)); MU_A3(mu_unpack_normal_oct8(0x8080u));
    MU_A3(mu_oct_encode_ts(N).xyx); MU_A3(mu_oct_decode_ts(p2));
    MU_A3(mu_encode_rgbm(c, 8.0).xyz); MU_A3(mu_decode_rgbm(uint4(1,2,3,4), 8.0));
    MU_A3(mu_encode_rgbd(c, 8.0).xyz); MU_A3(mu_decode_rgbd(uint4(1,2,3,4), 8.0));
    MU_A3(mu_encode_rgbe(c).xyz); MU_A3(mu_decode_rgbe(uint4(1,2,3,130)));
    MU_A3(mu_encode_logluv(c).xyz); MU_A3(mu_decode_logluv(uint4(1,2,3,4)));
    MU_A1(mu_pack_depth16(x)); MU_A1(mu_unpack_depth16(40000u));
    MU_A1(mu_pack_depth24(x).x); MU_A1(mu_unpack_depth24(mu_pack_depth24(x)));
    MU_A1(mu_pack_velocity(p2, 1.0).x); MU_A3(mu_unpack_velocity(mu_pack_velocity(p2, 1.0), 1.0).xyx);
    MU_A1(mu_pack_yaw8(x)); MU_A1(mu_unpack_yaw8(200u));
    MU_A1(mu_pack_dir_xz(p3)); MU_A3(mu_unpack_dir_xz(0x80008000u));
    MU_A1(mu_quantize_steps(x, 4.0)); MU_A1(mu_quantize_steps_soft(x, 4.0, 0.2));
    MU_A3(mu_posterize(float3(x), 5.0, true));
    MuPackedVertex vert = mu_pack_vertex(p3, float3(1.0), N, p2);
    MU_A3(mu_unpack_vertex_pos(vert, float3(1.0)));
    MU_A1(mu_pack_nrm_rough_metal(N, 0.4, 0.6));
    MU_A3(mu_unpack_nrm_rough_metal(mu_pack_nrm_rough_metal(N, 0.4, 0.6)));
    MU_A3(mu_unpack_orm(mu_pack_nrm_rough_metal(N, 0.4, 0.6)).xyx);
    MuGBufferFat fat; fat.albedo = mu_pack_rgba8(float4(x));
    fat.nrm_orm = mu_pack_nrm_rough_metal(N, 0.4, 0.6); fat.velocity = 0u;
    MuGBufferThin thin; thin.albedo_mat = mu_pack_rgb10a2(float3(x), 0.5);
    thin.nrm_orm = fat.nrm_orm;
    MU_A1(fat.albedo + thin.albedo_mat);

    /* ------------------------------------------------------- brdf_util.slang */
    MU_A1(mu_roughness_to_alpha(x)); MU_A1(mu_alpha_to_roughness(0.25));
    MU_A1(mu_perceptual_roughness(x, 0.5));
    float3 t, bt; mu_onb(N, t, bt); MU_A3(t + bt);
    MU_A3(mu_tangent_to_world(N, mu_sample_ggx(p2, 0.25)));
    MU_A3(mu_world_to_tangent(N, V));
    MU_A3(mu_tbn(N, float4(t, 1.0))[0]);
    MU_A3(mu_tbn_unit(N, float4(t, 1.0))[0]);
    MU_A3(mu_sample_cosine_hemisphere(p2)); MU_A3(mu_sample_sphere(p2));
    MU_A3(mu_sample_ggx(p2, 0.25));
    MU_A3(mu_sample_vndf_ggx(p2, 0.25, V));
    MU_A1(mu_pdf_ggx(0.7, 0.5, 0.25));
    MU_A3(mu_golden_hemisphere(3u, 16u, x));
    MU_A1(mu_mip_from_solid_angle(0.1 + x, 256u, 32u));

    /* --------------------------------------------------------- lighting.slang */
    MU_A3(mu_f0_from_reflectance(0.5)); MU_A1(mu_ior_to_f0(1.5));
    MuSurface surf = mu_surface(float3(0.8, 0.2, 0.1), N, V, 0.4, 0.6);
    surf.clearcoat = 0.5; surf.sheen_color = float3(0.2, 0.1, 0.3);
    MU_A3(surf.f0 + surf.diffuse_color);
    MU_A3(mu_f_schlick(float3(0.04), 0.7));
    MU_A3(mu_f_schlick_f90(float3(0.04), float3(1.0), 0.7));
    MU_A3(mu_f_schlick_roughness(float3(0.04), 0.4, 0.7));
    MU_A1(mu_d_ggx(0.8, 0.25)); MU_A1(mu_d_ggx_aniso(0.8, 0.2, 0.1, 0.3, 0.2));
    MU_A1(mu_d_charlie(0.8, 0.3));
    MU_A1(mu_v_smith_ggx(0.7, 0.6, 0.25)); MU_A1(mu_v_smith_ggx_fast(0.7, 0.6, 0.25));
    MU_A1(mu_v_kelemen(0.8));
    MU_A3(mu_lambert_diffuse(float3(0.5)));
    MU_A1(mu_burley_diffuse(0.7, 0.6, 0.8, 0.4));
    MU_A1(mu_diffuse_frostbite(0.7, 0.6, 0.8, 0.4));
    MU_A3(mu_specular_brdf(float3(0.04), 0.7, 0.6, 0.8, 0.9, 0.25));
    MU_A3(mu_pbr_direct(surf, L, float3(3.0)));
    float3 pbr_d, pbr_s; mu_pbr_direct_split(surf, L, float3(3.0), pbr_d, pbr_s);
    MU_A3(pbr_d + pbr_s);
    MU_A1(mu_point_attenuation(4.0, 10.0)); MU_A1(mu_spot_attenuation(0.8, 0.7, 0.9));
    MU_A3(mu_punctual_light(float3(1,2,3), 10.0, float3(0,0,-1), 0.7, 0.9, float3(2.0)));
    MU_A3(mu_env_brdf_approx(0.7, 0.4).xyx);
    MU_A3(mu_multi_scatter_fresnel(float3(0.04), mu_env_brdf_approx(0.7, 0.4)));
    MU_A3(mu_pbr_indirect(surf, float3(0.4), float3(1.2), 0.8, 0.9));
    MU_A3(mu_clearcoat_brdf(surf, L)); MU_A3(mu_sheen_brdf(surf, L));
    MU_A3(mu_translucency(N, L, V, float3(0.6,0.1,0.1), 0.5, 4.0));
    MU_A1(mu_specular_occlusion(0.7, 0.8, 0.4));

    /* ------------------------------------------------------------- toon.slang */
    MU_A1(mu_half_lambert(0.3)); MU_A1(mu_wrap_lambert(0.3, 0.3));
    MU_A1(mu_wrap_half_lambert(0.3, 0.3));
    MU_A1(mu_cel_band(x, 4.0, 0.2)); MU_A1(mu_cel_diffuse(0.4, 3.0, 0.2, 0.05));
    MU_A3(mu_ramp3(x, float3(0.1), float3(0.4), float3(0.9)));
    MU_A3(mu_ramp4(x, float3(0.1), float3(0.3), float3(0.6), float3(1.0)));
    MU_A1(mu_rim2(N, V)); MU_A1(mu_rim4(N, V)); MU_A1(mu_rim8(N, V));
    MU_A1(mu_rim(N, V, 3.0)); MU_A1(mu_rim_light(N, V, L, 2.0, 0.2));
    MU_A1(mu_stylized_fresnel(N, V, 0.1, 3.0, 0.2));
    MU_A1(mu_blinn8(N, L, V)); MU_A1(mu_blinn16(N, L, V)); MU_A1(mu_blinn(N, L, V, 12.0));
    MU_A1(mu_cel_specular(N, L, V, 16.0, 2.0, 0.2));
    MU_A1(mu_hair_highlight(0.7, x, 3.0, 0.2, 8.0));
    MU_A1(mu_hatch_line(px, float2(0.7, 0.7), 8.0, 0.5));
    MU_A1(mu_hatch4(px, 0.4, 10.0)); MU_A1(mu_paper_grain(px, 0.1));
    MU_A1(mu_edge_depth(0.4, 0.5, 0.05)); MU_A1(mu_edge_normal(N, V, 0.2));
    MU_A1(mu_edge_id(1u, 2u)); MU_A1(mu_edge_depth_scale(0.5, 0.01, 0.1));
    MU_A1(mu_edge_combine(1.0, 0.0, 0.0));
    MuToonParams tp;
    tp.albedo = float3(0.8,0.3,0.2); tp.shadow_tint = float3(0.2,0.25,0.4);
    tp.steps = 3.0; tp.softness = 0.15; tp.rim_strength = 0.6;
    tp.rim_color = float3(0.9,0.7,1.0); tp.spec_steps = 2.0; tp.spec_strength = 0.5;
    MU_A3(mu_toon_shade(tp, N, L, V, float3(1.0)));

    /* ---------------------------------------------------------- shadows.slang */
    float4x4 lvp; lvp[0]=float4(1,0,0,0); lvp[1]=float4(0,1,0,0);
    lvp[2]=float4(0,0,1,0); lvp[3]=float4(0,0,0,1);
    float2 suv = mu_shadow_uv(lvp, p3, MU_NDC_TO_UV);
    float4 clip = mul(lvp, float4(p3, 1.0));
    MU_A3(mu_clip_to_ndc(clip));
    MU_A3(mu_ndc_to_uv(float3(x, 0.2, 0.3), MU_NDC_TO_UV).xyx);
    MU_A3(suv.xyx);
    MU_A1(mu_shadow_in_bounds(suv, 0.5));
    MU_A1(mu_depth_bias(dot(N, L), 0.001, 0.01));
    MU_A3(mu_normal_offset(p3, N, L, 0.05, 2.0));
    float2 sinv = float2(1.0 / 2048.0);
    MU_A1(mu_pcf3x3(g_shadow_depth, g_shadow_cmp, suv, 0.5, sinv));
    MU_A1(mu_pcf_bilinear(g_shadow_depth, g_shadow_cmp, suv, 0.5, sinv, float2(2048.0)));
    MU_A1(mu_shadow_compare4(float4(0.4,0.5,0.6,0.7), 0.5, 1.0).x);
    MU_A1(mu_pcf_gather_box(g_shadow_depth, g_shadow_samp, suv, 0.5, sinv, 1.0));
    MU_A1(mu_pcf_poisson(g_shadow_depth, g_shadow_samp, suv, 0.5, sinv, 2.0, px, 1.0));
    float blocker = mu_pcss_blocker(g_shadow_depth, g_shadow_samp, suv, 0.5, sinv, 3.0, 1.0);
    MU_A1(blocker); MU_A1(mu_pcss_penumbra(0.5, 0.3, 8.0));
    MU_A1(mu_vsm(float2(0.5, 0.26), 0.52, 0.0001, 0.2));
    MU_A1(mu_esm(0.7, 0.5, 40.0));
    MU_A3(mu_moment_blur(float2(0.3,0.1), float2(0.4,0.2), float2(0.5,0.3), float2(0.6,0.4)).xyx);
    MU_A1(mu_cascade_index(20.0, float4(10,30,70,150)));
    MU_A1(mu_cascade_blend_weight(29.0, 30.0, 4.0));
    MU_A1(mu_shadow_fade(140.0, 150.0, 10.0));

    /* ------------------------------------------------------------ noise.slang */
    MU_A1(mu_hash_u2(uint2(3,7))); MU_A1(mu_hash2_u2(uint2(3,7)).x);
    MU_A1(mu_hash_u3(uint3(1,2,3))); MU_A1(mu_hash2_u3(uint3(1,2,3)).x);
    MU_A1(mu_hash3_u3(uint3(1,2,3)).x);
    MU_A1(mu_cell_hash(int2(-3,4))); MU_A1(mu_cell_hash2(int2(-3,4)).x);
    MU_A1(mu_cell_hash3(int3(-1,2,3)));
    MU_A3(mu_grad2(int2(1,2)).xyx); MU_A3(mu_grad3(int3(1,2,3)));
    MU_A1(mu_value_noise2(p2)); MU_A1(mu_value_noise3(p3));
    MU_A1(mu_gradient_noise2(p2)); MU_A1(mu_gradient_noise3(p3));
    MU_A1(mu_simplex2(p2));
    float2 f1f2 = mu_worley2_f1f2(p2, 1.0);
    MU_A1(mu_worley2(p2, 1.0)); MU_A1(mu_worley_edge(f1f2));
    MU_A3(mu_curl_noise2(p2, 0.01).xyx); MU_A3(mu_curl_noise3(p3, 0.01));
    MU_A1(mu_fbm4(p2, 2.0, 0.5)); MU_A1(mu_ridged4(p2, 2.0, 0.5));
    MU_A1(mu_turbulence4(p2, 2.0, 0.5));
    MU_A3(mu_domain_warp2(p2, 0.3).xyx); MU_A1(mu_marble(p2, 8.0, 0.4));

    /* -------------------------------------------------------------- sdf.slang */
    MU_A1(mu_sd_union(0.3,0.5)); MU_A1(mu_sd_subtract(0.3,0.5));
    MU_A1(mu_sd_intersect(0.3,0.5)); MU_A1(mu_sd_smin(0.3,0.5,0.2));
    MU_A1(mu_sd_smin_cubic(0.3,0.5,0.2)); MU_A1(mu_sd_smax(0.3,0.5,0.2));
    MU_A1(mu_sd_onion(0.3,0.1)); MU_A1(mu_sd_round(0.3,0.1));
    MU_A3(mu_sd_rotate2_cs(p2, float2(0.8,0.6)).xyx);
    MU_A3(mu_sd_repeat3(p3, float3(2.0)));
    MU_A3(mu_sd_repeat2(p2, float2(2.0)).xyx);
    MU_A3(mu_sd_repeat_limited3(p3, float3(2.0), float3(3.0)));
    MU_A3(mu_sd_polar(p2, 6.0).xyx);
    MU_A3(mu_sd_repeat3_inv(p3, float3(0.5), float3(2.0)));
    MU_A3(mu_sd_repeat2_inv(p2, float2(0.5), float2(2.0)).xyx);
    MU_A3(mu_sd_repeat3_c(p3, 2.0)); MU_A3(mu_sd_repeat2_c(p2, 2.0).xyx);
    MU_A1(mu_sdf_cone_eps(20.0, 0.001)); MU_A1(mu_sdf_cone_mip(20.0, 0.001, 0.1));
    MU_A1(mu_sdf_last_step(0.02, 0.08)); MU_A1(mu_sdf_gradient_eps(0.1));
    MU_A1(mu_sd_circle2(p2,0.5)); MU_A1(mu_sd_box2(p2,float2(0.5)));
    MU_A1(mu_sd_round_box2(p2,float2(0.5),0.1));
    MU_A1(mu_sd_segment2(p2,float2(-1,0),float2(1,0)));
    MU_A1(mu_sd_triangle2(p2,float2(-1,0),float2(1,0),float2(0,1)));
    MU_A1(mu_sd_hexagon2(p2,0.5));
    MU_A1(mu_sd_capsule2(p2,float2(-1,0),float2(1,0),0.2));
    MU_A1(mu_sd_arc2(p2, float2(0.5,0.866), 0.6, 0.05));
    MU_A1(mu_sdf_fill2(0.1,0.01)); MU_A1(mu_sdf_stroke2(0.1,0.05,0.01));
    MU_A1(mu_sd_plane(p3,N,0.5)); MU_A1(mu_sd_sphere(p3,1.0));
    MU_A1(mu_sd_box(p3,float3(0.5))); MU_A1(mu_sd_round_box(p3,float3(0.5),0.1));
    MU_A1(mu_sd_torus(p3,float2(1.0,0.2)));
    MU_A1(mu_sd_capsule(p3,float3(0,-1,0),float3(0,1,0),0.2));
    MU_A1(mu_sd_cylinder(p3,0.5,1.0)); MU_A1(mu_sd_capped_cone(p3,1.0,0.5,0.1));
    MU_A1(mu_sd_octahedron(p3,1.0));
    MuProbeField field;
    MuSdfSphere sph; sph.center = float3(0.0); sph.radius = 1.0;
    MuSdfBox box; box.center = float3(0.0); box.half_extent = float3(0.8);
    MuSdfTorus tor; tor.center = float3(0.0); tor.radii = float2(1.0,0.2);
    MuSdfCapsule cap; cap.a = float3(0,-1,0); cap.b = float3(0,1,0); cap.radius = 0.2;
    MU_A3(mu_sdf_normal(field, p3, 0.001) + mu_sdf_normal(sph, p3, 0.001)
        + mu_sdf_normal(box, p3, 0.001) + mu_sdf_normal(tor, p3, 0.001)
        + mu_sdf_normal(cap, p3, 0.001));
    /* Loop-bearing helpers are exercised with a small step budget. */
    MU_A1(mu_sdf_raymarch(field, p3, float3(0,0,1), 0.0, 20.0, 8u, 0.001));
    MU_A1(mu_sdf_raymarch_stats(sph, p3, float3(0,0,1), 0.0, 20.0, 8u, 0.001).x);
    MU_A1(mu_sdf_soft_shadow(field, p3, L, 0.1, 10.0, 16.0, 8u));
    MU_A1(mu_sdf_ao(field, p3, N, 0.15));

    /* ------------------------------------------------------------- post.slang */
    MU_A3(mu_exposure(c, 1.0));
    MU_A3(mu_tonemap_reinhard(c)); MU_A3(mu_tonemap_reinhard_white(c, 4.0));
    MU_A3(mu_tonemap_aces_fitted(c)); MU_A3(mu_tonemap_aces(c));
    MU_A3(mu_aces_input_transform(c)); MU_A3(mu_aces_output_transform(c));
    MU_A3(mu_tonemap_neutral(c)); MU_A3(mu_tonemap_uchimura(c));
    MU_A3(mu_tonemap_uchimura(c, 1.0, 1.7, 0.1, 0.4, 1.33, 0.0));
    MU_A3(mu_hable_curve(c)); MU_A3(mu_tonemap_hable(c));
    MU_A3(mu_agx_sigmoid(float3(0.5))); MU_A3(mu_tonemap_agx(c));
    MU_A3(mu_dither_ign(c, px, 0.01)); MU_A3(mu_dither_bayer(c, px, 0.01));
    MU_A3(mu_dither_quantize(c, px, 8.0));
    MU_A3(mu_saturation(c, 1.2)); MU_A3(mu_contrast(c, 1.1, 0.18));
    MU_A3(mu_temperature(c, 0.1)); MU_A3(mu_film_grain(c, px, 0.02, 1.0));
    MU_A3(mu_vignette(c, float2(0.5), 0.4, 0.3));
    MU_A3(mu_chromatic_aberration(float2(0.5), float2(0.5), 0.01).xyx);
    MU_A3(mu_cas_sharpen(c, c*0.9, c*1.1, c*0.8, c*1.2, 0.7));
    MU_A3(mu_unsharp(c, c*0.9, 0.5));
    MU_A3(mu_karis_weight(c)); MU_A3(mu_bloom_threshold(c, 1.0, 0.5));
    MU_A3(mu_bloom_downsample13(c,c,c,c,c,c,c,c,c,c,c,c,c));
    MU_A3(mu_bloom_upsample9(c,c,c,c,c,c,c,c,c));
    MU_A3(mu_uv_from_pixel(px, float2(1.0/1920.0)).xyx);
    MU_A3(mu_pixel_from_uv(float2(0.5), float2(1920.0)).xyx);
    MU_A3(mu_uv_to_ndc(float2(0.5), MU_NDC_TO_UV).xyx);
    MU_A1(mu_linear_depth(0.5, 0.1, 100.0, false));
    MU_A1(mu_linear_depth(0.5, 0.1, 100.0, true));
    float4x4 inv_vp = lvp;
    MU_A3(mu_reconstruct_position(float2(0.5), 0.5, inv_vp, MU_NDC_TO_UV));
    MU_A1(mu_luma_edge(c, c*0.5));
    MU_A3(mu_ssao_offset(3u, 16u, x, 0.5));
    MU_A1(mu_ssao_sample_weight(float3(0.1,0.2,0.3), V, 1.0));

    /* -------------------------------------------------------- dispatch.slang */
    MU_A1(mu_sector_count(1000u)); MU_A1(mu_sector_bytes(4u));
    MU_A1(mu_sector_efficiency(4u, 1u));
    MU_A1(mu_bytes_in_flight(644.6f, 600.0f, 64u));
    MU_A1(mu_pass_bandwidth_us(2560u, 1440u, 8u, 2u));
    MU_A1(mu_halo_overhead(uint2(8u, 8u), 1u));
    MU_A1(mu_halo_border_ratio(uint2(16u, 16u), 1u));
    MU_A3(mu_dispatch_pixel(uint2(idx & 15u, 1u), uint2(3u, 4u), uint2(8u, 8u)).xyx);
    MU_A3(mu_fullscreen_triangle_uv(idx & 2u).xyx);
    MU_A3(mu_fullscreen_triangle_pos(idx & 2u).xy.xyx);
    MU_A3(mu_ndc_to_uv(p2).xyx); MU_A3(mu_uv_to_ndc_flipped(p2).xyx);
    uint2 tgid = uint2(idx & 63u, 1u), tgrid = uint2(240u, 135u);
    MU_A3(mu_tiled_group_id(tgid, tgrid, 16u).xyx);
    MU_A3(mu_tiled_group_id_pow2(tgid, tgrid, 4u).xyx);

    /* ------------------------------------------------------------ wave.slang */
    MU_A1(mu_wave_size()); MU_A1(mu_wave_lane());
    MU_A1(mu_wave_any(x > 0.0)); MU_A1(mu_wave_all(x > 0.0));
    MU_A1(mu_wave_sum(x)); MU_A1(mu_wave_max(x)); MU_A1(mu_wave_min(x));
    MU_A1(mu_wave_sum(idx & 63u)); MU_A1(mu_wave_max(idx & 63u));
    MU_A1(mu_wave_compact_offset(x > 0.0)); MU_A1(mu_wave_compact_count(x > 0.0));
    MU_A1(mu_wave_first(x > 0.0));
    MU_A1(mu_wave_prefix_sum_expensive(idx & 63u));
    MU_A1(mu_wave_broadcast_first(x)); MU_A1(mu_wave_broadcast_first(idx & 63u));
    MU_A1(mu_wave_shuffle(x, idx & 31u)); MU_A1(mu_wave_shuffle(idx & 63u, 0u));
    MU_A1(mu_wave_shuffle_xor(x, 1u)); MU_A1(mu_wave_loop_trip_count(idx & 15u));
    MU_A1(mu_antipodal_phase(x)); MU_A1(mu_reflected_pair(x));
    MU_A1(mu_noise_vs_samples(8u));

    /* ------------------------------------------------------------ fp16.slang */
    half2 h2 = mu_half2(mu_saturate(p2));
    half4 h4 = mu_half4(float4(c, 1.0));
    MU_A3(mu_float2(mu_half2_mad(h2, h2, h2)).xyx);
    MU_A3(mu_float2(mu_half2_saturate(h2)).xyx);
    MU_A3(mu_float4(mu_half4_saturate(h4)).xyz);
    MU_A3(mu_float2(mu_half2_lerp(h2, h2, h2)).xyx);
    MU_A3(mu_float4(mu_half4_lerp(h4, h4, h4)).xyz);
    MU_A1(mu_half3_luma(half3(h2.x, h2.y, half(0.5))));
    MU_A3(mu_float2(mu_half2_encode_colour(p2)).xyx);
    MU_A3(mu_half2_decode_colour(h2).xyx);
    MU_A1(mu_half2_pack_unorm(p2)); MU_A3(mu_half2_unpack_unorm(0x80008000u).xyx);
    MU_A1(mu_half2_pack_bits(h2)); MU_A3(mu_float2(mu_half2_unpack_bits(0x3C003C00u)).xyx);

    /* ---------------------------------------------------------- format.slang */
    MU_A1(mu_target_bytes(2560u, 1440u, MU_BPP_RGBA16F));
    MU_A1(mu_pass_bytes(2560u, 1440u, MU_BPP_RGBA8_UNORM));
    MU_A3(mu_linear_to_srgb(c)); MU_A3(mu_srgb_view_equivalent(c));
    MU_A3(mu_normal_for_snorm_view(N)); MU_A3(mu_normal_from_unorm8(float3(x)));
    MU_A1(mu_r11g11b10_max()); MU_A1(mu_r11g11b10_min());
    MU_A3(mu_gamma22_to_linear(c)); MU_A3(mu_linear_to_gamma22(c));
    MU_A1(mu_reverse_z_ndc(x));

    out_buf[idx] = float4(acc, mu_saturate(s * 0.0001));
}
