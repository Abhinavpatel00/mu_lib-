/*
 * test_ga.c — regression gate for mu/mu_geometric_algebra.h.
 *
 * The header used to be written against the struct-based mu_math_legacy.h and
 * now runs on the array-based mu_math.h. A geometric algebra is easy to "port"
 * into something that still compiles and is quietly wrong: a transposed motor
 * matrix or a flipped homogeneous coordinate looks perfectly plausible.
 *
 * So this file pins the *behaviour*, not the source: it calls every one of the
 * 177 public functions with deterministic inputs and compares a
 * rolling FNV-1a digest of the results against the values the legacy
 * implementation produced. Functions are checked in call order, so the first
 * failing row names the function that broke.
 *
 * Build and run:
 *   gcc -O2 -std=c99 -I../mu test_ga.c -o test_ga -lm && ./test_ga
 */
#include "mu_geometric_algebra.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t H = 1469598103934665603ull;
static int      g_fail;
static int      g_checks;

static void mix(const float *f, int n)
{
    for (int i = 0; i < n; ++i)
    {
        unsigned char b[4];
        memcpy(b, &f[i], 4);
        for (int k = 0; k < 4; ++k) { H ^= b[k]; H *= 1099511628211ull; }
    }
}

/* Digests produced by the struct-based implementation this header was ported
 * from. Any change in arithmetic order, argument mapping or layout shows up
 * here as a mismatch. */
typedef struct { const char *name; uint64_t digest; } ga_row;
static const ga_row g_expected[] = {
    {"mu_ga_sqrt", 0x2ec843a08478a6daull},
    {"mu_ga_inv_sqrt", 0xbb02342895464fa4ull},
    {"mu_ga_abs", 0x99fdd617200e6d0bull},
    {"mu_ga_clamp_eps", 0x7e54a2ead8704970ull},
    {"mu_ga_vec2", 0xbbb5cf035515b412ull},
    {"mu_ga_vec3", 0xf436b3fe13eba6feull},
    {"mu_ga_vec4", 0xfc925f54ea3c9c16ull},
    {"mu_ga_wedge_v3_v3", 0x1a6e9b629d34846bull},
    {"mu_ga_flat_point2_make", 0x968ebd0fecf6d4a3ull},
    {"mu_ga_flat_point2_from_point", 0xa36435127b07b4edull},
    {"mu_ga_flat_point2_from_dir", 0x8a872318b218719bull},
    {"mu_ga_line2_make", 0x8e3ad9cbf3b6123cull},
    {"mu_ga_line2_from_points", 0x3fd342ef78956b7dull},
    {"mu_ga_line2_from_point_dir", 0xe11cd4632caf0ab4ull},
    {"mu_ga_flat_point2_unitize", 0x61390e641a760e6aull},
    {"mu_ga_line2_unitize", 0x1e5a132f302f254full},
    {"mu_ga_line2_complement", 0xa1e9eef71b22269full},
    {"mu_ga_flat_point2_complement", 0x7c122fd7ff27e315ull},
    {"mu_ga_line2_bulk_dual", 0x6f62f99acfc672efull},
    {"mu_ga_flat_point2_bulk_dual", 0xda85e608c4531d8cull},
    {"mu_ga_line2_weight_dual", 0xd564a198554fbb0bull},
    {"mu_ga_flat_point2_weight_dual", 0xd290061e03e883ecull},
    {"mu_ga_flat_point2_support", 0xe3319f5768839c59ull},
    {"mu_ga_line2_antisupport", 0x3d32f7d29ebc4bb2ull},
    {"mu_ga_flat_point2_attitude", 0x6ad7c45b284d714aull},
    {"mu_ga_line2_attitude", 0xc060ccbc001a88b1ull},
    {"mu_ga_flat_point2_squared_bulk_norm", 0x12716b0a5990e0b3ull},
    {"mu_ga_line2_squared_bulk_norm", 0xb23939d3134b031bull},
    {"mu_ga_flat_point2_squared_weight_norm", 0xad4579751aa846a8ull},
    {"mu_ga_line2_squared_weight_norm", 0x5c15f04a3e5e4b77ull},
    {"mu_ga_flat_point2_dot", 0xa7db74f5be97c65full},
    {"mu_ga_line2_dot", 0xfa54b4995b369e72ull},
    {"mu_ga_flat_point2_antidot", 0x9b92d1cb92b9ec36ull},
    {"mu_ga_line2_antidot", 0xfd549546e939c399ull},
    {"mu_ga_flat_point2_translate", 0x4655762acaad436bull},
    {"mu_ga_line2_translate", 0x73cfda0a8ef867b4ull},
    {"mu_ga_wedge_flat_point2_flat_point2", 0x25ca9a71f2be6eb6ull},
    {"mu_ga_wedge_point2_point2", 0xcdcd4f25e9a4e148ull},
    {"mu_ga_wedge_point2_vec2", 0x36a432de963f85c9ull},
    {"mu_ga_antiwedge_line2_line2", 0x0c33b33c7e930d66ull},
    {"mu_ga_antiwedge_point2_line2", 0xec7fbcc1c5f9fa0dull},
    {"mu_ga_project_point2_line2", 0x229ef6f13bd2134cull},
    {"mu_ga_antiproject_line2_point2", 0x1f0b6ae3b8dfed30ull},
    {"mu_ga_flat_point3_make", 0x8afa3583db5cd695ull},
    {"mu_ga_flat_point3_from_point", 0xc48c92ab9900960eull},
    {"mu_ga_line3_make", 0x86503e794870be92ull},
    {"mu_ga_plane3_make", 0xd0294dbd9f92f2dbull},
    {"mu_ga_wedge_flat_point3_flat_point3", 0x6b9dd67c4ba645e3ull},
    {"mu_ga_wedge_point3_point3", 0xdc6ade732223f37dull},
    {"mu_ga_wedge_point3_vec3", 0xd83886642c457134ull},
    {"mu_ga_wedge_line3_flat_point3", 0x8350363e3b8ed038ull},
    {"mu_ga_wedge_line3_point3", 0xf9be3537b70737e4ull},
    {"mu_ga_wedge_line3_vec3", 0x87c766be51e723d7ull},
    {"mu_ga_antiwedge_plane3_plane3", 0xa9d018b873d56eacull},
    {"mu_ga_antiwedge_plane3_line3", 0x4783b05debcc2a68ull},
    {"mu_ga_antiwedge_line3_line3", 0x26de2f0eb97f2892ull},
    {"mu_ga_antiwedge_flat_point3_plane3", 0x92a9f12bd7d0e5d6ull},
    {"mu_ga_antiwedge_point3_plane3", 0x1a4eef67d1b4823eull},
    {"mu_ga_antiwedge_vec3_plane3", 0x40cd78920734dee2ull},
    {"mu_ga_flat_point3_translate", 0x07b0baa2e5535168ull},
    {"mu_ga_line3_translate", 0x52d5cfb8934df333ull},
    {"mu_ga_plane3_translate", 0x3563e982b65e5496ull},
    {"mu_ga_project_point3_line3", 0x3dc90bc9749c9e99ull},
    {"mu_ga_project_point3_plane3", 0x75b1d81d1e2e02a3ull},
    {"mu_ga_project_line3_plane3", 0x099bfaeac2845b54ull},
    {"mu_ga_antiproject_line3_point3", 0x2ab06bbc532006d8ull},
    {"mu_ga_antiproject_plane3_point3", 0x375e2f8855ef228aull},
    {"mu_ga_motor2_identity", 0xf7d016aa4ed55c03ull},
    {"mu_ga_motor2_make", 0x808d9a3f43ab8d04ull},
    {"mu_ga_motor2_unitize", 0xe1ec50bd294678d9ull},
    {"mu_ga_motor2_make_rotation", 0x9464cd50d165b536ull},
    {"mu_ga_motor2_make_translation", 0xf024614dea487df3ull},
    {"mu_ga_motor2_mul", 0xb8eed7a9c384bc97ull},
    {"mu_ga_transform_vec2_motor", 0x35ce2634e008124bull},
    {"mu_ga_transform_point2_motor", 0x22d0928ce7733979ull},
    {"mu_ga_transform_flat_point2_motor", 0x63dc80dc65409df9ull},
    {"mu_ga_transform_line2_motor", 0x81287d9317dd5acbull},
    {"mu_ga_flector2_make", 0x7f250e950a204973ull},
    {"mu_ga_flector2_unitize", 0xf2f0272a465338c5ull},
    {"mu_ga_flector2_make_transflection", 0xba65a061a3254fb4ull},
    {"mu_ga_flector2_mul_flector2", 0x33a0d6dddb7a60e0ull},
    {"mu_ga_flector2_mul_motor2", 0x6e238974813dc940ull},
    {"mu_ga_motor2_mul_flector2", 0xff1ebe6c6702d0cfull},
    {"mu_ga_transform_vec2_flector", 0xa4700c5dc65a0fe8ull},
    {"mu_ga_transform_point2_flector", 0x917a468f98d1cd49ull},
    {"mu_ga_transform_flat_point2_flector", 0xce20f92b3f3ff80aull},
    {"mu_ga_transform_line2_flector", 0xf1d29ea79d16a0ceull},
    {"mu_ga_motor3_identity", 0xe22eb16877888d7full},
    {"mu_ga_motor3_make", 0x52d8f24ac3a53ffeull},
    {"mu_ga_motor3_unitize", 0x9a2a37f2652320dcull},
    {"mu_ga_motor3_make_rotation", 0x3e21d17389ee1400ull},
    {"mu_ga_motor3_make_translation", 0x5d9558821ccddbbaull},
    {"mu_ga_motor3_make_screw", 0xe14a5846bc982bb0ull},
    {"mu_ga_motor3_mul", 0x59fa14a8fd81320cull},
    {"mu_ga_motor3_sqrt", 0xb5f10d14c9c5013aull},
    {"mu_ga_transform_vec3_motor", 0x40210eac7bc15cc5ull},
    {"mu_ga_transform_point3_motor", 0xd9f24000e036340bull},
    {"mu_ga_transform_flat_point3_motor", 0xddbb15c19a532050ull},
    {"mu_ga_transform_line3_motor", 0x46dc1916cc850c7full},
    {"mu_ga_transform_plane3_motor", 0x89ceae0938334c1eull},
    {"mu_ga_flector3_make", 0x3ba7d8381067dc2full},
    {"mu_ga_flector3_unitize", 0x10f58e2d4dbd9afdull},
    {"mu_ga_flector3_make_transflection", 0xc2f9102a1494f8a7ull},
    {"mu_ga_flector3_make_rotoreflection_line", 0x29e7a0aeae69bd6bull},
    {"mu_ga_flector3_mul_flector3", 0x8b7ac80b66ee861cull},
    {"mu_ga_transform_vec3_flector", 0x30e734bacbf95d99ull},
    {"mu_ga_transform_point3_flector", 0x95fe964f2d22e4cfull},
    {"mu_ga_transform_flat_point3_flector", 0xa4706864495b6becull},
    {"mu_ga_transform_line3_flector", 0x2236022b96738c96ull},
    {"mu_ga_transform_plane3_flector", 0x2ff8f608f6b07c3full},
    {"mu_ga_round_point2_make", 0xb0263ec99299dc20ull},
    {"mu_ga_round_point2_from_point", 0x8e476bdbacea0fbdull},
    {"mu_ga_dipole2_make", 0xe571f28ee4bda1d9ull},
    {"mu_ga_circle2_make", 0x6fdda4b43d92bff9ull},
    {"mu_ga_dual_round_point2", 0x491b9f58bc209cb3ull},
    {"mu_ga_dual_dipole2", 0x80bbd04cbda0e8f2ull},
    {"mu_ga_dual_circle2", 0xb65757c073e3fb6cull},
    {"mu_ga_round_point2_squared_radius_norm", 0x5356daed01184be1ull},
    {"mu_ga_dipole2_squared_radius_norm", 0x0e4060cc563dbd65ull},
    {"mu_ga_circle2_squared_radius_norm", 0xe6b79aa2b1fc10c6ull},
    {"mu_ga_center_round_point2", 0xbc5564608bb2bdb2ull},
    {"mu_ga_center_dipole2", 0x9f8b6de80f5f2108ull},
    {"mu_ga_center_circle2", 0xef174cc5f0a06a4cull},
    {"mu_ga_flat_center_dipole2", 0xa352b1558d92c7e1ull},
    {"mu_ga_container_round_point2", 0x98ec5438e5b64167ull},
    {"mu_ga_container_dipole2", 0xb66fff7046afc105ull},
    {"mu_ga_partner_dipole2", 0xf21019e9fd95837dull},
    {"mu_ga_wedge_round_point2_round_point2", 0xd0533f82adf1c99dull},
    {"mu_ga_wedge_flat_point2_round_point2", 0x46bd17b82e7cf580ull},
    {"mu_ga_wedge_point2_round_point2", 0x9c2d538b08bc3e9cull},
    {"mu_ga_wedge_dipole2_round_point2", 0x6b49f8814d8d9a9eull},
    {"mu_ga_antiwedge_circle2_circle2", 0x1e21acc56fe284f7ull},
    {"mu_ga_antiwedge_circle2_line2", 0xdb14bea9820c4aa2ull},
    {"mu_ga_antiwedge_circle2_dipole2", 0x9c47e0e8eaa322d8ull},
    {"mu_ga_antiwedge_line2_dipole2", 0xd906f61236146493ull},
    {"mu_ga_antiwedge_circle2_flat_point2", 0x403fa2fe68d17097ull},
    {"mu_ga_antiwedge_circle2_point2", 0x465a4670b9f05fd4ull},
    {"mu_ga_round_point3_make", 0x02459c2055a3425aull},
    {"mu_ga_round_point3_from_point", 0xe0a3fdaa8c65d872ull},
    {"mu_ga_dipole3_make", 0xdb7bec7ce57ecc4eull},
    {"mu_ga_circle3_make", 0x83c5e98d9bc258f0ull},
    {"mu_ga_sphere3_make", 0x00cc89cb233caf53ull},
    {"mu_ga_dual_round_point3", 0xc49b7b7632c1a1bdull},
    {"mu_ga_dual_dipole3", 0x4b2d4032fd13947dull},
    {"mu_ga_dual_circle3", 0xf10a54199af743d7ull},
    {"mu_ga_dual_sphere3", 0x83677ff3713ec9b5ull},
    {"mu_ga_center_dipole3", 0x480710bc171e3101ull},
    {"mu_ga_center_circle3", 0xe0bfe5f79ad5934full},
    {"mu_ga_center_sphere3", 0x30f0da3edd70884cull},
    {"mu_ga_flat_center_dipole3", 0x02c31bfb4e713822ull},
    {"mu_ga_flat_center_circle3", 0x757e859381cd58bdull},
    {"mu_ga_container_dipole3", 0x03c1f2c5d8a976daull},
    {"mu_ga_container_circle3", 0x92ce6efbdf85b24eull},
    {"mu_ga_round_point3_squared_radius_norm", 0x32ef3ece95292614ull},
    {"mu_ga_dipole3_squared_radius_norm", 0x31a2aaf445c87bb5ull},
    {"mu_ga_circle3_squared_radius_norm", 0xf77e16e24fa11fc4ull},
    {"mu_ga_sphere3_squared_radius_norm", 0x7ac024e2322a1573ull},
    {"mu_ga_wedge_round_point3_round_point3", 0x1e614c9912c2283aull},
    {"mu_ga_wedge_flat_point3_round_point3", 0xf5fbe87236331ae8ull},
    {"mu_ga_wedge_point3_round_point3", 0x6ef86bbcf6ffb005ull},
    {"mu_ga_wedge_dipole3_round_point3", 0xfeb5b4a30726a2eeull},
    {"mu_ga_wedge_line3_round_point3", 0xed09aee08c254508ull},
    {"mu_ga_wedge_dipole3_flat_point3", 0x3f3d997197946dbeull},
    {"mu_ga_wedge_dipole3_point3", 0x8804bae026b49d01ull},
    {"mu_ga_wedge_circle3_round_point3", 0x091612b6aea87832ull},
    {"mu_ga_wedge_dipole3_dipole3", 0x9e59b7286531c617ull},
    {"mu_ga_antiwedge_sphere3_sphere3", 0xe22c7710f59d5bdeull},
    {"mu_ga_antiwedge_sphere3_plane3", 0xae73b0f30120774cull},
    {"mu_ga_antiwedge_sphere3_circle3", 0xd50128fe1376de8full},
    {"mu_ga_antiwedge_plane3_circle3", 0x3351b9ec89cd6e9eull},
    {"mu_ga_antiwedge_sphere3_line3", 0xd22728e54b8d5e6dull},
    {"mu_ga_antiwedge_circle3_circle3", 0xa9059ff29376c269ull},
    {"mu_ga_antiwedge_circle3_line3", 0x9dca2c6c489c9a83ull},
    {"mu_ga_antiwedge_sphere3_dipole3", 0x1a55f7ff2a005389ull},
    {"mu_ga_antiwedge_plane3_dipole3", 0x49d3d775ba854b02ull},
    {"mu_ga_antiwedge_sphere3_flat_point3", 0xe8fb1a0fc4f9955full},
    {"mu_ga_antiwedge_sphere3_point3", 0xf58e6852812b9203ull},
};

static void check(const char *name, uint64_t got)
{
    static int row;
    uint64_t want = g_expected[row].digest;

    ++g_checks;
    if (want != got)
    {
        ++g_fail;
        if (g_fail <= 20)
            printf("  FAIL %-40s want %016llx got %016llx\n",
                   name, (unsigned long long)want, (unsigned long long)got);
    }
    ++row;
}

int main(void)
{
    { /* mu_ga_sqrt */
    float x = ((float)((((0) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_sqrt(x);
        mix(&r, 1);
        check("mu_ga_sqrt", H);
    }
    { /* mu_ga_inv_sqrt */
    float x = ((float)((((1) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_inv_sqrt(x);
        mix(&r, 1);
        check("mu_ga_inv_sqrt", H);
    }
    { /* mu_ga_abs */
    float x = ((float)((((2) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_abs(x);
        mix(&r, 1);
        check("mu_ga_abs", H);
    }
    { /* mu_ga_clamp_eps */
    float x = ((float)((((3) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_clamp_eps(x);
        mix(&r, 1);
        check("mu_ga_clamp_eps", H);
    }
    { /* mu_ga_vec2 */
    float x = ((float)((((4) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((4) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec2 r;
        mu_ga_vec2(r, x, y);
        mix((const float*)&r, 2);
        check("mu_ga_vec2", H);
    }
    { /* mu_ga_vec3 */
    float x = ((float)((((5) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((5) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((5) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec3 r;
        mu_ga_vec3(r, x, y, z);
        mix((const float*)&r, 3);
        check("mu_ga_vec3", H);
    }
    { /* mu_ga_vec4 */
    float x = ((float)((((6) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((6) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((6) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float w = ((float)((((6) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec4 r;
        mu_ga_vec4(r, x, y, z, w);
        mix((const float*)&r, 4);
        check("mu_ga_vec4", H);
    }
    { /* mu_ga_wedge_v3_v3 */
    mu_vec3 a = {((float)((((10) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((10) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((10) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_vec3 b = {((float)((((10) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((10) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((10) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_vec3 r;
        mu_ga_wedge_v3_v3(r, a, b);
        mix((const float*)&r, 3);
        check("mu_ga_wedge_v3_v3", H);
    }
    { /* mu_ga_flat_point2_make */
    float x = ((float)((((11) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((11) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((11) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_flat_point2_make(x, y, z);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_make", H);
    }
    { /* mu_ga_flat_point2_from_point */
    mu_vec2 p = {((float)((((12) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((12) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_flat_point2 r = mu_ga_flat_point2_from_point(p);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_from_point", H);
    }
    { /* mu_ga_flat_point2_from_dir */
    mu_vec2 v = {((float)((((13) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((13) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_flat_point2 r = mu_ga_flat_point2_from_dir(v);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_from_dir", H);
    }
    { /* mu_ga_line2_make */
    float x = ((float)((((14) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((14) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((14) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_line2_make(x, y, z);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_make", H);
    }
    { /* mu_ga_line2_from_points */
    mu_vec2 p = {((float)((((15) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((15) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_vec2 q = {((float)((((15) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((15) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line2 r = mu_ga_line2_from_points(p, q);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_from_points", H);
    }
    { /* mu_ga_line2_from_point_dir */
    mu_vec2 p = {((float)((((16) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((16) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_vec2 v = {((float)((((16) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((16) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line2 r = mu_ga_line2_from_point_dir(p, v);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_from_point_dir", H);
    }
    { /* mu_ga_flat_point2_unitize */
    mu_ga_flat_point2 p;
    p.x = ((float)((((17) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((17) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((17) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_flat_point2_unitize(p);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_unitize", H);
    }
    { /* mu_ga_line2_unitize */
    mu_ga_line2 g;
    g.x = ((float)((((18) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((18) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((18) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_line2_unitize(g);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_unitize", H);
    }
    { /* mu_ga_line2_complement */
    mu_ga_flat_point2 p;
    p.x = ((float)((((19) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((19) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((19) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_line2_complement(p);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_complement", H);
    }
    { /* mu_ga_flat_point2_complement */
    mu_ga_line2 g;
    g.x = ((float)((((20) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((20) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((20) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_flat_point2_complement(g);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_complement", H);
    }
    { /* mu_ga_line2_bulk_dual */
    mu_ga_flat_point2 p;
    p.x = ((float)((((21) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((21) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((21) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_line2_bulk_dual(p);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_bulk_dual", H);
    }
    { /* mu_ga_flat_point2_bulk_dual */
    mu_ga_line2 g;
    g.x = ((float)((((22) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((22) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((22) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_flat_point2_bulk_dual(g);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_bulk_dual", H);
    }
    { /* mu_ga_line2_weight_dual */
    mu_ga_flat_point2 p;
    p.x = ((float)((((23) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((23) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((23) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_line2_weight_dual(p);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_weight_dual", H);
    }
    { /* mu_ga_flat_point2_weight_dual */
    mu_ga_line2 g;
    g.x = ((float)((((24) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((24) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((24) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_flat_point2_weight_dual(g);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_weight_dual", H);
    }
    { /* mu_ga_flat_point2_support */
    mu_ga_line2 g;
    g.x = ((float)((((25) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((25) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((25) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_flat_point2_support(g);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_support", H);
    }
    { /* mu_ga_line2_antisupport */
    mu_ga_flat_point2 p;
    p.x = ((float)((((26) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((26) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((26) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_line2_antisupport(p);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_antisupport", H);
    }
    { /* mu_ga_flat_point2_attitude */
    mu_ga_flat_point2 p;
    p.x = ((float)((((27) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((27) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((27) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_flat_point2_attitude(p);
        mix(&r, 1);
        check("mu_ga_flat_point2_attitude", H);
    }
    { /* mu_ga_line2_attitude */
    mu_ga_line2 g;
    g.x = ((float)((((28) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((28) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((28) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec2 r;
        mu_ga_line2_attitude(r, g);
        mix((const float*)&r, 2);
        check("mu_ga_line2_attitude", H);
    }
    { /* mu_ga_flat_point2_squared_bulk_norm */
    mu_ga_flat_point2 p;
    p.x = ((float)((((29) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((29) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((29) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_flat_point2_squared_bulk_norm(p);
        mix(&r, 1);
        check("mu_ga_flat_point2_squared_bulk_norm", H);
    }
    { /* mu_ga_line2_squared_bulk_norm */
    mu_ga_line2 g;
    g.x = ((float)((((30) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((30) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((30) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_line2_squared_bulk_norm(g);
        mix(&r, 1);
        check("mu_ga_line2_squared_bulk_norm", H);
    }
    { /* mu_ga_flat_point2_squared_weight_norm */
    mu_ga_flat_point2 p;
    p.x = ((float)((((31) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((31) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((31) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_flat_point2_squared_weight_norm(p);
        mix(&r, 1);
        check("mu_ga_flat_point2_squared_weight_norm", H);
    }
    { /* mu_ga_line2_squared_weight_norm */
    mu_ga_line2 g;
    g.x = ((float)((((32) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((32) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((32) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_line2_squared_weight_norm(g);
        mix(&r, 1);
        check("mu_ga_line2_squared_weight_norm", H);
    }
    { /* mu_ga_flat_point2_dot */
    mu_ga_flat_point2 a;
    a.x = ((float)((((33) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((33) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((33) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flat_point2 b;
    b.x = ((float)((((33) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((33) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((33) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_flat_point2_dot(a, b);
        mix(&r, 1);
        check("mu_ga_flat_point2_dot", H);
    }
    { /* mu_ga_line2_dot */
    mu_ga_line2 a;
    a.x = ((float)((((34) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((34) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((34) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line2 b;
    b.x = ((float)((((34) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((34) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((34) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_line2_dot(a, b);
        mix(&r, 1);
        check("mu_ga_line2_dot", H);
    }
    { /* mu_ga_flat_point2_antidot */
    mu_ga_flat_point2 a;
    a.x = ((float)((((35) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((35) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((35) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flat_point2 b;
    b.x = ((float)((((35) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((35) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((35) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_flat_point2_antidot(a, b);
        mix(&r, 1);
        check("mu_ga_flat_point2_antidot", H);
    }
    { /* mu_ga_line2_antidot */
    mu_ga_line2 a;
    a.x = ((float)((((36) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((36) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((36) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line2 b;
    b.x = ((float)((((36) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((36) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((36) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_line2_antidot(a, b);
        mix(&r, 1);
        check("mu_ga_line2_antidot", H);
    }
    { /* mu_ga_flat_point2_translate */
    mu_ga_flat_point2 p;
    p.x = ((float)((((37) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((37) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((37) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec2 t = {((float)((((37) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((37) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_flat_point2 r = mu_ga_flat_point2_translate(p, t);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_point2_translate", H);
    }
    { /* mu_ga_line2_translate */
    mu_ga_line2 g;
    g.x = ((float)((((38) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((38) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((38) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec2 t = {((float)((((38) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((38) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line2 r = mu_ga_line2_translate(g, t);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_line2_translate", H);
    }
    { /* mu_ga_wedge_flat_point2_flat_point2 */
    mu_ga_flat_point2 p;
    p.x = ((float)((((39) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((39) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((39) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flat_point2 q;
    q.x = ((float)((((39) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((39) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((39) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_wedge_flat_point2_flat_point2(p, q);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_wedge_flat_point2_flat_point2", H);
    }
    { /* mu_ga_wedge_point2_point2 */
    mu_vec2 p = {((float)((((40) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((40) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_vec2 q = {((float)((((40) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((40) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line2 r = mu_ga_wedge_point2_point2(p, q);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_wedge_point2_point2", H);
    }
    { /* mu_ga_wedge_point2_vec2 */
    mu_vec2 p = {((float)((((41) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((41) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_vec2 v = {((float)((((41) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((41) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line2 r = mu_ga_wedge_point2_vec2(p, v);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_wedge_point2_vec2", H);
    }
    { /* mu_ga_antiwedge_line2_line2 */
    mu_ga_line2 g;
    g.x = ((float)((((42) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((42) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((42) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line2 h;
    h.x = ((float)((((42) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    h.y = ((float)((((42) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    h.z = ((float)((((42) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_antiwedge_line2_line2(g, h);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_antiwedge_line2_line2", H);
    }
    { /* mu_ga_antiwedge_point2_line2 */
    mu_vec2 p = {((float)((((43) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((43) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_line2 g;
    g.x = ((float)((((43) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((43) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((43) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_antiwedge_point2_line2(p, g);
        mix(&r, 1);
        check("mu_ga_antiwedge_point2_line2", H);
    }
    { /* mu_ga_project_point2_line2 */
    mu_vec2 p = {((float)((((44) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((44) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_line2 g;
    g.x = ((float)((((44) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((44) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((44) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec2 r;
        mu_ga_project_point2_line2(r, p, g);
        mix((const float*)&r, 2);
        check("mu_ga_project_point2_line2", H);
    }
    { /* mu_ga_antiproject_line2_point2 */
    mu_ga_line2 g;
    g.x = ((float)((((45) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((45) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((45) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec2 p = {((float)((((45) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((45) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line2 r = mu_ga_antiproject_line2_point2(g, p);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_antiproject_line2_point2", H);
    }
    { /* mu_ga_flat_point3_make */
    float x = ((float)((((46) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((46) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((46) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float w = ((float)((((46) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point3 r = mu_ga_flat_point3_make(x, y, z, w);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flat_point3_make", H);
    }
    { /* mu_ga_flat_point3_from_point */
    mu_vec3 p = {((float)((((47) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((47) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((47) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_flat_point3 r = mu_ga_flat_point3_from_point(p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flat_point3_from_point", H);
    }
    { /* mu_ga_line3_make */
    float vx = ((float)((((48) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vy = ((float)((((48) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vz = ((float)((((48) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mx = ((float)((((48) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float my = ((float)((((48) * 37 + (4) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mz = ((float)((((48) * 37 + (5) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line3 r = mu_ga_line3_make(vx, vy, vz, mx, my, mz);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_line3_make", H);
    }
    { /* mu_ga_plane3_make */
    float x = ((float)((((49) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((49) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((49) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float w = ((float)((((49) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_plane3 r = mu_ga_plane3_make(x, y, z, w);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_plane3_make", H);
    }
    { /* mu_ga_wedge_flat_point3_flat_point3 */
    mu_ga_flat_point3 p;
    p.x = ((float)((((50) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((50) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((50) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((50) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flat_point3 q;
    q.x = ((float)((((50) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((50) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((50) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.w = ((float)((((50) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line3 r = mu_ga_wedge_flat_point3_flat_point3(p, q);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_wedge_flat_point3_flat_point3", H);
    }
    { /* mu_ga_wedge_point3_point3 */
    mu_vec3 p = {((float)((((51) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((51) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((51) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_vec3 q = {((float)((((51) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((51) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((51) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line3 r = mu_ga_wedge_point3_point3(p, q);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_wedge_point3_point3", H);
    }
    { /* mu_ga_wedge_point3_vec3 */
    mu_vec3 p = {((float)((((52) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((52) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((52) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_vec3 v = {((float)((((52) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((52) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((52) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line3 r = mu_ga_wedge_point3_vec3(p, v);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_wedge_point3_vec3", H);
    }
    { /* mu_ga_wedge_line3_flat_point3 */
    mu_ga_line3 l;
    l.vx = ((float)((((53) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((53) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((53) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((53) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((53) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((53) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flat_point3 p;
    p.x = ((float)((((53) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((53) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((53) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((53) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_plane3 r = mu_ga_wedge_line3_flat_point3(l, p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_wedge_line3_flat_point3", H);
    }
    { /* mu_ga_wedge_line3_point3 */
    mu_ga_line3 l;
    l.vx = ((float)((((54) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((54) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((54) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((54) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((54) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((54) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 p = {((float)((((54) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((54) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((54) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_plane3 r = mu_ga_wedge_line3_point3(l, p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_wedge_line3_point3", H);
    }
    { /* mu_ga_wedge_line3_vec3 */
    mu_ga_line3 l;
    l.vx = ((float)((((55) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((55) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((55) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((55) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((55) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((55) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 v = {((float)((((55) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((55) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((55) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_plane3 r = mu_ga_wedge_line3_vec3(l, v);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_wedge_line3_vec3", H);
    }
    { /* mu_ga_antiwedge_plane3_plane3 */
    mu_ga_plane3 g;
    g.x = ((float)((((56) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((56) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((56) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((56) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_plane3 h;
    h.x = ((float)((((56) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    h.y = ((float)((((56) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    h.z = ((float)((((56) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    h.w = ((float)((((56) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line3 r = mu_ga_antiwedge_plane3_plane3(g, h);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_antiwedge_plane3_plane3", H);
    }
    { /* mu_ga_antiwedge_plane3_line3 */
    mu_ga_plane3 g;
    g.x = ((float)((((57) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((57) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((57) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((57) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line3 l;
    l.vx = ((float)((((57) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((57) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((57) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((57) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((57) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((57) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point3 r = mu_ga_antiwedge_plane3_line3(g, l);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_antiwedge_plane3_line3", H);
    }
    { /* mu_ga_antiwedge_line3_line3 */
    mu_ga_line3 k;
    k.vx = ((float)((((58) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    k.vy = ((float)((((58) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    k.vz = ((float)((((58) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    k.mx = ((float)((((58) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    k.my = ((float)((((58) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    k.mz = ((float)((((58) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line3 l;
    l.vx = ((float)((((58) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((58) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((58) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((58) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((58) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((58) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_antiwedge_line3_line3(k, l);
        mix(&r, 1);
        check("mu_ga_antiwedge_line3_line3", H);
    }
    { /* mu_ga_antiwedge_flat_point3_plane3 */
    mu_ga_flat_point3 p;
    p.x = ((float)((((59) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((59) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((59) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((59) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_plane3 g;
    g.x = ((float)((((59) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((59) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((59) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((59) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_antiwedge_flat_point3_plane3(p, g);
        mix(&r, 1);
        check("mu_ga_antiwedge_flat_point3_plane3", H);
    }
    { /* mu_ga_antiwedge_point3_plane3 */
    mu_vec3 p = {((float)((((60) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((60) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((60) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_plane3 g;
    g.x = ((float)((((60) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((60) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((60) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((60) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_antiwedge_point3_plane3(p, g);
        mix(&r, 1);
        check("mu_ga_antiwedge_point3_plane3", H);
    }
    { /* mu_ga_antiwedge_vec3_plane3 */
    mu_vec3 v = {((float)((((61) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((61) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((61) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_plane3 g;
    g.x = ((float)((((61) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((61) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((61) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((61) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_antiwedge_vec3_plane3(v, g);
        mix(&r, 1);
        check("mu_ga_antiwedge_vec3_plane3", H);
    }
    { /* mu_ga_flat_point3_translate */
    mu_ga_flat_point3 p;
    p.x = ((float)((((62) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((62) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((62) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((62) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 t = {((float)((((62) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((62) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((62) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_flat_point3 r = mu_ga_flat_point3_translate(p, t);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flat_point3_translate", H);
    }
    { /* mu_ga_line3_translate */
    mu_ga_line3 l;
    l.vx = ((float)((((63) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((63) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((63) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((63) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((63) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((63) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 t = {((float)((((63) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((63) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((63) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line3 r = mu_ga_line3_translate(l, t);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_line3_translate", H);
    }
    { /* mu_ga_plane3_translate */
    mu_ga_plane3 g;
    g.x = ((float)((((64) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((64) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((64) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((64) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 t = {((float)((((64) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((64) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((64) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_plane3 r = mu_ga_plane3_translate(g, t);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_plane3_translate", H);
    }
    { /* mu_ga_project_point3_line3 */
    mu_vec3 p = {((float)((((65) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((65) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((65) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_line3 l;
    l.vx = ((float)((((65) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((65) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((65) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((65) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((65) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((65) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec3 r;
        mu_ga_project_point3_line3(r, p, l);
        mix((const float*)&r, 3);
        check("mu_ga_project_point3_line3", H);
    }
    { /* mu_ga_project_point3_plane3 */
    mu_vec3 p = {((float)((((66) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((66) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((66) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_plane3 g;
    g.x = ((float)((((66) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((66) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((66) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((66) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec3 r;
        mu_ga_project_point3_plane3(r, p, g);
        mix((const float*)&r, 3);
        check("mu_ga_project_point3_plane3", H);
    }
    { /* mu_ga_project_line3_plane3 */
    mu_ga_line3 l;
    l.vx = ((float)((((67) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((67) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((67) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((67) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((67) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((67) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_plane3 g;
    g.x = ((float)((((67) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((67) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((67) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((67) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line3 r = mu_ga_project_line3_plane3(l, g);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_project_line3_plane3", H);
    }
    { /* mu_ga_antiproject_line3_point3 */
    mu_ga_line3 l;
    l.vx = ((float)((((68) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((68) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((68) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((68) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((68) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((68) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 p = {((float)((((68) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((68) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((68) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_line3 r = mu_ga_antiproject_line3_point3(l, p);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_antiproject_line3_point3", H);
    }
    { /* mu_ga_antiproject_plane3_point3 */
    mu_ga_plane3 g;
    g.x = ((float)((((69) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((69) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((69) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((69) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 p = {((float)((((69) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((69) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((69) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_plane3 r = mu_ga_antiproject_plane3_point3(g, p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_antiproject_plane3_point3", H);
    }
    { /* mu_ga_motor2_identity */
        mu_ga_motor2 r = mu_ga_motor2_identity();
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_motor2_identity", H);
    }
    { /* mu_ga_motor2_make */
    float x = ((float)((((71) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((71) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((71) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float w = ((float)((((71) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor2 r = mu_ga_motor2_make(x, y, z, w);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_motor2_make", H);
    }
    { /* mu_ga_motor2_unitize */
    mu_ga_motor2 q;
    q.x = ((float)((((72) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((72) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((72) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.w = ((float)((((72) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor2 r = mu_ga_motor2_unitize(q);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_motor2_unitize", H);
    }
    { /* mu_ga_motor2_make_rotation */
    float angle = ((float)((((73) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec2 center = {((float)((((73) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((73) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_motor2 r = mu_ga_motor2_make_rotation(angle, center);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_motor2_make_rotation", H);
    }
    { /* mu_ga_motor2_make_translation */
    mu_vec2 offset = {((float)((((74) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((74) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_motor2 r = mu_ga_motor2_make_translation(offset);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_motor2_make_translation", H);
    }
    { /* mu_ga_motor2_mul */
    mu_ga_motor2 a;
    a.x = ((float)((((75) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((75) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((75) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((75) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_motor2 b;
    b.x = ((float)((((75) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((75) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((75) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.w = ((float)((((75) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor2 r = mu_ga_motor2_mul(a, b);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_motor2_mul", H);
    }
    { /* mu_ga_transform_vec2_motor */
    mu_vec2 v = {((float)((((78) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((78) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_motor2 q;
    q.x = ((float)((((78) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((78) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((78) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.w = ((float)((((78) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec2 r;
        mu_ga_transform_vec2_motor(r, v, q);
        mix((const float*)&r, 2);
        check("mu_ga_transform_vec2_motor", H);
    }
    { /* mu_ga_transform_point2_motor */
    mu_vec2 p = {((float)((((79) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((79) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_motor2 q;
    q.x = ((float)((((79) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((79) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((79) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.w = ((float)((((79) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec2 r;
        mu_ga_transform_point2_motor(r, p, q);
        mix((const float*)&r, 2);
        check("mu_ga_transform_point2_motor", H);
    }
    { /* mu_ga_transform_flat_point2_motor */
    mu_ga_flat_point2 p;
    p.x = ((float)((((80) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((80) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((80) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_motor2 q;
    q.x = ((float)((((80) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((80) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((80) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.w = ((float)((((80) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_transform_flat_point2_motor(p, q);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_transform_flat_point2_motor", H);
    }
    { /* mu_ga_transform_line2_motor */
    mu_ga_line2 g;
    g.x = ((float)((((81) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((81) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((81) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_motor2 q;
    q.x = ((float)((((81) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((81) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((81) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.w = ((float)((((81) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_transform_line2_motor(g, q);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_transform_line2_motor", H);
    }
    { /* mu_ga_flector2_make */
    float x = ((float)((((83) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((83) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((83) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float w = ((float)((((83) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector2 r = mu_ga_flector2_make(x, y, z, w);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flector2_make", H);
    }
    { /* mu_ga_flector2_unitize */
    mu_ga_flector2 f;
    f.x = ((float)((((84) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.y = ((float)((((84) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.z = ((float)((((84) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.w = ((float)((((84) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector2 r = mu_ga_flector2_unitize(f);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flector2_unitize", H);
    }
    { /* mu_ga_flector2_make_transflection */
    mu_vec2 offset = {((float)((((85) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((85) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_line2 line_unit;
    line_unit.x = ((float)((((85) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    line_unit.y = ((float)((((85) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    line_unit.z = ((float)((((85) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector2 r = mu_ga_flector2_make_transflection(offset, line_unit);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flector2_make_transflection", H);
    }
    { /* mu_ga_flector2_mul_flector2 */
    mu_ga_flector2 a;
    a.x = ((float)((((88) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((88) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((88) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((88) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flector2 b;
    b.x = ((float)((((88) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((88) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((88) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.w = ((float)((((88) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor2 r = mu_ga_flector2_mul_flector2(a, b);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flector2_mul_flector2", H);
    }
    { /* mu_ga_flector2_mul_motor2 */
    mu_ga_flector2 a;
    a.x = ((float)((((89) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((89) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((89) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((89) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_motor2 b;
    b.x = ((float)((((89) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((89) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((89) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.w = ((float)((((89) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector2 r = mu_ga_flector2_mul_motor2(a, b);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flector2_mul_motor2", H);
    }
    { /* mu_ga_motor2_mul_flector2 */
    mu_ga_motor2 a;
    a.x = ((float)((((90) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((90) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((90) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((90) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flector2 b;
    b.x = ((float)((((90) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((90) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((90) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.w = ((float)((((90) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector2 r = mu_ga_motor2_mul_flector2(a, b);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_motor2_mul_flector2", H);
    }
    { /* mu_ga_transform_vec2_flector */
    mu_vec2 v = {((float)((((91) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((91) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_flector2 q;
    q.x = ((float)((((91) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((91) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((91) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.w = ((float)((((91) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec2 r;
        mu_ga_transform_vec2_flector(r, v, q);
        mix((const float*)&r, 2);
        check("mu_ga_transform_vec2_flector", H);
    }
    { /* mu_ga_transform_point2_flector */
    mu_vec2 p = {((float)((((92) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((92) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_flector2 q;
    q.x = ((float)((((92) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.y = ((float)((((92) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.z = ((float)((((92) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.w = ((float)((((92) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec2 r;
        mu_ga_transform_point2_flector(r, p, q);
        mix((const float*)&r, 2);
        check("mu_ga_transform_point2_flector", H);
    }
    { /* mu_ga_transform_flat_point2_flector */
    mu_ga_flat_point2 p;
    p.x = ((float)((((93) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((93) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((93) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flector2 f;
    f.x = ((float)((((93) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.y = ((float)((((93) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.z = ((float)((((93) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.w = ((float)((((93) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_transform_flat_point2_flector(p, f);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_transform_flat_point2_flector", H);
    }
    { /* mu_ga_transform_line2_flector */
    mu_ga_line2 g;
    g.x = ((float)((((94) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((94) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((94) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flector2 f;
    f.x = ((float)((((94) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.y = ((float)((((94) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.z = ((float)((((94) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.w = ((float)((((94) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_transform_line2_flector(g, f);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_transform_line2_flector", H);
    }
    { /* mu_ga_motor3_identity */
        mu_ga_motor3 r = mu_ga_motor3_identity();
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_motor3_identity", H);
    }
    { /* mu_ga_motor3_make */
    float vx = ((float)((((96) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vy = ((float)((((96) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vz = ((float)((((96) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vw = ((float)((((96) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mx = ((float)((((96) * 37 + (4) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float my = ((float)((((96) * 37 + (5) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mz = ((float)((((96) * 37 + (6) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mw = ((float)((((96) * 37 + (7) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor3 r = mu_ga_motor3_make(vx, vy, vz, vw, mx, my, mz, mw);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_motor3_make", H);
    }
    { /* mu_ga_motor3_unitize */
    mu_ga_motor3 q;
    q.vx = ((float)((((97) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vy = ((float)((((97) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vz = ((float)((((97) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vw = ((float)((((97) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mx = ((float)((((97) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.my = ((float)((((97) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mz = ((float)((((97) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mw = ((float)((((97) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor3 r = mu_ga_motor3_unitize(q);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_motor3_unitize", H);
    }
    { /* mu_ga_motor3_make_rotation */
    float angle = ((float)((((98) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 axis_unit = {((float)((((98) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((98) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((98) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_motor3 r = mu_ga_motor3_make_rotation(angle, axis_unit);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_motor3_make_rotation", H);
    }
    { /* mu_ga_motor3_make_translation */
    mu_vec3 offset = {((float)((((99) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((99) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((99) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_motor3 r = mu_ga_motor3_make_translation(offset);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_motor3_make_translation", H);
    }
    { /* mu_ga_motor3_make_screw */
    float angle = ((float)((((100) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line3 axis_unit;
    axis_unit.vx = ((float)((((100) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.vy = ((float)((((100) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.vz = ((float)((((100) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.mx = ((float)((((100) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.my = ((float)((((100) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.mz = ((float)((((100) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    float disp = ((float)((((100) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor3 r = mu_ga_motor3_make_screw(angle, axis_unit, disp);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_motor3_make_screw", H);
    }
    { /* mu_ga_motor3_mul */
    mu_ga_motor3 a;
    a.vx = ((float)((((101) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.vy = ((float)((((101) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.vz = ((float)((((101) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.vw = ((float)((((101) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.mx = ((float)((((101) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.my = ((float)((((101) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.mz = ((float)((((101) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.mw = ((float)((((101) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_motor3 b;
    b.vx = ((float)((((101) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.vy = ((float)((((101) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.vz = ((float)((((101) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.vw = ((float)((((101) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.mx = ((float)((((101) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.my = ((float)((((101) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.mz = ((float)((((101) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.mw = ((float)((((101) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor3 r = mu_ga_motor3_mul(a, b);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_motor3_mul", H);
    }
    { /* mu_ga_motor3_sqrt */
    mu_ga_motor3 q;
    q.vx = ((float)((((102) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vy = ((float)((((102) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vz = ((float)((((102) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vw = ((float)((((102) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mx = ((float)((((102) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.my = ((float)((((102) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mz = ((float)((((102) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mw = ((float)((((102) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor3 r = mu_ga_motor3_sqrt(q);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_motor3_sqrt", H);
    }
    { /* mu_ga_transform_vec3_motor */
    mu_vec3 v = {((float)((((107) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((107) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((107) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_motor3 q;
    q.vx = ((float)((((107) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vy = ((float)((((107) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vz = ((float)((((107) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vw = ((float)((((107) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mx = ((float)((((107) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.my = ((float)((((107) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mz = ((float)((((107) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mw = ((float)((((107) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec3 r;
        mu_ga_transform_vec3_motor(r, v, q);
        mix((const float*)&r, 3);
        check("mu_ga_transform_vec3_motor", H);
    }
    { /* mu_ga_transform_point3_motor */
    mu_vec3 p = {((float)((((108) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((108) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((108) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_motor3 q;
    q.vx = ((float)((((108) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vy = ((float)((((108) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vz = ((float)((((108) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vw = ((float)((((108) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mx = ((float)((((108) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.my = ((float)((((108) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mz = ((float)((((108) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mw = ((float)((((108) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec3 r;
        mu_ga_transform_point3_motor(r, p, q);
        mix((const float*)&r, 3);
        check("mu_ga_transform_point3_motor", H);
    }
    { /* mu_ga_transform_flat_point3_motor */
    mu_ga_flat_point3 p;
    p.x = ((float)((((109) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((109) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((109) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((109) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_motor3 q;
    q.vx = ((float)((((109) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vy = ((float)((((109) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vz = ((float)((((109) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vw = ((float)((((109) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mx = ((float)((((109) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.my = ((float)((((109) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mz = ((float)((((109) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mw = ((float)((((109) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point3 r = mu_ga_transform_flat_point3_motor(p, q);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_transform_flat_point3_motor", H);
    }
    { /* mu_ga_transform_line3_motor */
    mu_ga_line3 l;
    l.vx = ((float)((((110) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((110) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((110) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((110) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((110) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((110) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_motor3 q;
    q.vx = ((float)((((110) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vy = ((float)((((110) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vz = ((float)((((110) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vw = ((float)((((110) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mx = ((float)((((110) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.my = ((float)((((110) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mz = ((float)((((110) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mw = ((float)((((110) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line3 r = mu_ga_transform_line3_motor(l, q);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_transform_line3_motor", H);
    }
    { /* mu_ga_transform_plane3_motor */
    mu_ga_plane3 g;
    g.x = ((float)((((111) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((111) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((111) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((111) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_motor3 q;
    q.vx = ((float)((((111) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vy = ((float)((((111) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vz = ((float)((((111) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.vw = ((float)((((111) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mx = ((float)((((111) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.my = ((float)((((111) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mz = ((float)((((111) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.mw = ((float)((((111) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_plane3 r = mu_ga_transform_plane3_motor(g, q);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_transform_plane3_motor", H);
    }
    { /* mu_ga_flector3_make */
    float px = ((float)((((112) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float py = ((float)((((112) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float pz = ((float)((((112) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float pw = ((float)((((112) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gx = ((float)((((112) * 37 + (4) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gy = ((float)((((112) * 37 + (5) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gz = ((float)((((112) * 37 + (6) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gw = ((float)((((112) * 37 + (7) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector3 r = mu_ga_flector3_make(px, py, pz, pw, gx, gy, gz, gw);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_flector3_make", H);
    }
    { /* mu_ga_flector3_unitize */
    mu_ga_flector3 f;
    f.px = ((float)((((113) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.py = ((float)((((113) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pz = ((float)((((113) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pw = ((float)((((113) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gx = ((float)((((113) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gy = ((float)((((113) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gz = ((float)((((113) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gw = ((float)((((113) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector3 r = mu_ga_flector3_unitize(f);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_flector3_unitize", H);
    }
    { /* mu_ga_flector3_make_transflection */
    mu_vec3 offset = {((float)((((114) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((114) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((114) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_plane3 plane_unit;
    plane_unit.x = ((float)((((114) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    plane_unit.y = ((float)((((114) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    plane_unit.z = ((float)((((114) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    plane_unit.w = ((float)((((114) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector3 r = mu_ga_flector3_make_transflection(offset, plane_unit);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_flector3_make_transflection", H);
    }
    { /* mu_ga_flector3_make_rotoreflection_line */
    float angle = ((float)((((115) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line3 axis_unit;
    axis_unit.vx = ((float)((((115) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.vy = ((float)((((115) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.vz = ((float)((((115) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.mx = ((float)((((115) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.my = ((float)((((115) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    axis_unit.mz = ((float)((((115) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_plane3 plane_unit;
    plane_unit.x = ((float)((((115) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    plane_unit.y = ((float)((((115) * 37 + (2) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    plane_unit.z = ((float)((((115) * 37 + (2) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    plane_unit.w = ((float)((((115) * 37 + (2) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flector3 r = mu_ga_flector3_make_rotoreflection_line(angle, axis_unit, plane_unit);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_flector3_make_rotoreflection_line", H);
    }
    { /* mu_ga_flector3_mul_flector3 */
    mu_ga_flector3 a;
    a.px = ((float)((((118) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.py = ((float)((((118) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.pz = ((float)((((118) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.pw = ((float)((((118) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.gx = ((float)((((118) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.gy = ((float)((((118) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.gz = ((float)((((118) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.gw = ((float)((((118) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flector3 b;
    b.px = ((float)((((118) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.py = ((float)((((118) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.pz = ((float)((((118) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.pw = ((float)((((118) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.gx = ((float)((((118) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.gy = ((float)((((118) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.gz = ((float)((((118) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.gw = ((float)((((118) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_motor3 r = mu_ga_flector3_mul_flector3(a, b);
        { float flat[8]; memcpy(flat, &r, sizeof flat); mix(flat, 8); }
        check("mu_ga_flector3_mul_flector3", H);
    }
    { /* mu_ga_transform_vec3_flector */
    mu_vec3 v = {((float)((((119) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((119) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((119) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_flector3 q;
    q.px = ((float)((((119) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.py = ((float)((((119) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.pz = ((float)((((119) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.pw = ((float)((((119) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.gx = ((float)((((119) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.gy = ((float)((((119) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.gz = ((float)((((119) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.gw = ((float)((((119) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec3 r;
        mu_ga_transform_vec3_flector(r, v, q);
        mix((const float*)&r, 3);
        check("mu_ga_transform_vec3_flector", H);
    }
    { /* mu_ga_transform_point3_flector */
    mu_vec3 p = {((float)((((120) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((120) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((120) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_flector3 q;
    q.px = ((float)((((120) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.py = ((float)((((120) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.pz = ((float)((((120) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.pw = ((float)((((120) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.gx = ((float)((((120) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.gy = ((float)((((120) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.gz = ((float)((((120) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    q.gw = ((float)((((120) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_vec3 r;
        mu_ga_transform_point3_flector(r, p, q);
        mix((const float*)&r, 3);
        check("mu_ga_transform_point3_flector", H);
    }
    { /* mu_ga_transform_flat_point3_flector */
    mu_ga_flat_point3 p;
    p.x = ((float)((((121) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((121) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((121) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((121) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flector3 f;
    f.px = ((float)((((121) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.py = ((float)((((121) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pz = ((float)((((121) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pw = ((float)((((121) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gx = ((float)((((121) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gy = ((float)((((121) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gz = ((float)((((121) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gw = ((float)((((121) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point3 r = mu_ga_transform_flat_point3_flector(p, f);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_transform_flat_point3_flector", H);
    }
    { /* mu_ga_transform_line3_flector */
    mu_ga_line3 l;
    l.vx = ((float)((((122) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((122) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((122) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((122) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((122) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((122) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flector3 f;
    f.px = ((float)((((122) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.py = ((float)((((122) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pz = ((float)((((122) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pw = ((float)((((122) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gx = ((float)((((122) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gy = ((float)((((122) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gz = ((float)((((122) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gw = ((float)((((122) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line3 r = mu_ga_transform_line3_flector(l, f);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_transform_line3_flector", H);
    }
    { /* mu_ga_transform_plane3_flector */
    mu_ga_plane3 g;
    g.x = ((float)((((123) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((123) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((123) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((123) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flector3 f;
    f.px = ((float)((((123) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.py = ((float)((((123) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pz = ((float)((((123) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pw = ((float)((((123) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gx = ((float)((((123) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gy = ((float)((((123) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gz = ((float)((((123) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.gw = ((float)((((123) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_plane3 r = mu_ga_transform_plane3_flector(g, f);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_transform_plane3_flector", H);
    }
    { /* mu_ga_round_point2_make */
    float x = ((float)((((124) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((124) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((124) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float w = ((float)((((124) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point2 r = mu_ga_round_point2_make(x, y, z, w);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_round_point2_make", H);
    }
    { /* mu_ga_round_point2_from_point */
    mu_vec2 p = {((float)((((125) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((125) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_round_point2 r = mu_ga_round_point2_from_point(p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_round_point2_from_point", H);
    }
    { /* mu_ga_dipole2_make */
    float gx = ((float)((((126) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gy = ((float)((((126) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gz = ((float)((((126) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float px = ((float)((((126) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float py = ((float)((((126) * 37 + (4) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float pz = ((float)((((126) * 37 + (5) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole2 r = mu_ga_dipole2_make(gx, gy, gz, px, py, pz);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_dipole2_make", H);
    }
    { /* mu_ga_circle2_make */
    float w = ((float)((((127) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float x = ((float)((((127) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((127) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((127) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle2 r = mu_ga_circle2_make(w, x, y, z);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_circle2_make", H);
    }
    { /* mu_ga_dual_round_point2 */
    mu_ga_round_point2 a;
    a.x = ((float)((((128) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((128) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((128) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((128) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle2 r = mu_ga_dual_round_point2(a);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_dual_round_point2", H);
    }
    { /* mu_ga_dual_dipole2 */
    mu_ga_dipole2 d;
    d.gx = ((float)((((129) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((129) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((129) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((129) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((129) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((129) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole2 r = mu_ga_dual_dipole2(d);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_dual_dipole2", H);
    }
    { /* mu_ga_dual_circle2 */
    mu_ga_circle2 c;
    c.w = ((float)((((130) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.x = ((float)((((130) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.y = ((float)((((130) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.z = ((float)((((130) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point2 r = mu_ga_dual_circle2(c);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_dual_circle2", H);
    }
    { /* mu_ga_round_point2_squared_radius_norm */
    mu_ga_round_point2 a;
    a.x = ((float)((((131) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((131) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((131) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((131) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_round_point2_squared_radius_norm(a);
        mix(&r, 1);
        check("mu_ga_round_point2_squared_radius_norm", H);
    }
    { /* mu_ga_dipole2_squared_radius_norm */
    mu_ga_dipole2 d;
    d.gx = ((float)((((132) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((132) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((132) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((132) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((132) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((132) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_dipole2_squared_radius_norm(d);
        mix(&r, 1);
        check("mu_ga_dipole2_squared_radius_norm", H);
    }
    { /* mu_ga_circle2_squared_radius_norm */
    mu_ga_circle2 c;
    c.w = ((float)((((133) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.x = ((float)((((133) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.y = ((float)((((133) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.z = ((float)((((133) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_circle2_squared_radius_norm(c);
        mix(&r, 1);
        check("mu_ga_circle2_squared_radius_norm", H);
    }
    { /* mu_ga_center_round_point2 */
    mu_ga_round_point2 a;
    a.x = ((float)((((134) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((134) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((134) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((134) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point2 r = mu_ga_center_round_point2(a);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_center_round_point2", H);
    }
    { /* mu_ga_center_dipole2 */
    mu_ga_dipole2 d;
    d.gx = ((float)((((135) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((135) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((135) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((135) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((135) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((135) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point2 r = mu_ga_center_dipole2(d);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_center_dipole2", H);
    }
    { /* mu_ga_center_circle2 */
    mu_ga_circle2 c;
    c.w = ((float)((((136) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.x = ((float)((((136) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.y = ((float)((((136) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.z = ((float)((((136) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point2 r = mu_ga_center_circle2(c);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_center_circle2", H);
    }
    { /* mu_ga_flat_center_dipole2 */
    mu_ga_dipole2 d;
    d.gx = ((float)((((137) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((137) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((137) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((137) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((137) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((137) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point2 r = mu_ga_flat_center_dipole2(d);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_flat_center_dipole2", H);
    }
    { /* mu_ga_container_round_point2 */
    mu_ga_round_point2 a;
    a.x = ((float)((((138) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((138) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((138) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((138) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle2 r = mu_ga_container_round_point2(a);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_container_round_point2", H);
    }
    { /* mu_ga_container_dipole2 */
    mu_ga_dipole2 d;
    d.gx = ((float)((((139) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((139) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((139) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((139) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((139) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((139) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle2 r = mu_ga_container_dipole2(d);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_container_dipole2", H);
    }
    { /* mu_ga_partner_dipole2 */
    mu_ga_dipole2 d;
    d.gx = ((float)((((140) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((140) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((140) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((140) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((140) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((140) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole2 r = mu_ga_partner_dipole2(d);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_partner_dipole2", H);
    }
    { /* mu_ga_wedge_round_point2_round_point2 */
    mu_ga_round_point2 a;
    a.x = ((float)((((141) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((141) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((141) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((141) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_round_point2 b;
    b.x = ((float)((((141) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((141) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((141) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.w = ((float)((((141) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole2 r = mu_ga_wedge_round_point2_round_point2(a, b);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_wedge_round_point2_round_point2", H);
    }
    { /* mu_ga_wedge_flat_point2_round_point2 */
    mu_ga_flat_point2 p;
    p.x = ((float)((((142) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((142) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((142) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_round_point2 a;
    a.x = ((float)((((142) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((142) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((142) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((142) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_wedge_flat_point2_round_point2(p, a);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_wedge_flat_point2_round_point2", H);
    }
    { /* mu_ga_wedge_point2_round_point2 */
    mu_vec2 p = {((float)((((143) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((143) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_round_point2 a;
    a.x = ((float)((((143) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((143) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((143) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((143) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line2 r = mu_ga_wedge_point2_round_point2(p, a);
        { float flat[3]; memcpy(flat, &r, sizeof flat); mix(flat, 3); }
        check("mu_ga_wedge_point2_round_point2", H);
    }
    { /* mu_ga_wedge_dipole2_round_point2 */
    mu_ga_dipole2 d;
    d.gx = ((float)((((144) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((144) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((144) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((144) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((144) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((144) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_round_point2 a;
    a.x = ((float)((((144) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((144) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((144) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((144) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle2 r = mu_ga_wedge_dipole2_round_point2(d, a);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_wedge_dipole2_round_point2", H);
    }
    { /* mu_ga_antiwedge_circle2_circle2 */
    mu_ga_circle2 c;
    c.w = ((float)((((145) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.x = ((float)((((145) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.y = ((float)((((145) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.z = ((float)((((145) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_circle2 o;
    o.w = ((float)((((145) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.x = ((float)((((145) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.y = ((float)((((145) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.z = ((float)((((145) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole2 r = mu_ga_antiwedge_circle2_circle2(c, o);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_antiwedge_circle2_circle2", H);
    }
    { /* mu_ga_antiwedge_circle2_line2 */
    mu_ga_circle2 c;
    c.w = ((float)((((146) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.x = ((float)((((146) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.y = ((float)((((146) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.z = ((float)((((146) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line2 g;
    g.x = ((float)((((146) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((146) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((146) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole2 r = mu_ga_antiwedge_circle2_line2(c, g);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_antiwedge_circle2_line2", H);
    }
    { /* mu_ga_antiwedge_circle2_dipole2 */
    mu_ga_circle2 c;
    c.w = ((float)((((147) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.x = ((float)((((147) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.y = ((float)((((147) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.z = ((float)((((147) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_dipole2 d;
    d.gx = ((float)((((147) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((147) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((147) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((147) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((147) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((147) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point2 r = mu_ga_antiwedge_circle2_dipole2(c, d);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_antiwedge_circle2_dipole2", H);
    }
    { /* mu_ga_antiwedge_line2_dipole2 */
    mu_ga_line2 g;
    g.x = ((float)((((148) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((148) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((148) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_dipole2 d;
    d.gx = ((float)((((148) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gy = ((float)((((148) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.gz = ((float)((((148) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((148) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((148) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((148) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point2 r = mu_ga_antiwedge_line2_dipole2(g, d);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_antiwedge_line2_dipole2", H);
    }
    { /* mu_ga_antiwedge_circle2_flat_point2 */
    mu_ga_circle2 c;
    c.w = ((float)((((149) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.x = ((float)((((149) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.y = ((float)((((149) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.z = ((float)((((149) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flat_point2 p;
    p.x = ((float)((((149) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((149) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((149) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point2 r = mu_ga_antiwedge_circle2_flat_point2(c, p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_antiwedge_circle2_flat_point2", H);
    }
    { /* mu_ga_antiwedge_circle2_point2 */
    mu_ga_circle2 c;
    c.w = ((float)((((150) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.x = ((float)((((150) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.y = ((float)((((150) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.z = ((float)((((150) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec2 p = {((float)((((150) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((150) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_round_point2 r = mu_ga_antiwedge_circle2_point2(c, p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_antiwedge_circle2_point2", H);
    }
    { /* mu_ga_round_point3_make */
    float x = ((float)((((151) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((151) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((151) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float w = ((float)((((151) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float u = ((float)((((151) * 37 + (4) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_round_point3_make(x, y, z, w, u);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_round_point3_make", H);
    }
    { /* mu_ga_round_point3_from_point */
    mu_vec3 p = {((float)((((152) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((152) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((152) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_round_point3 r = mu_ga_round_point3_from_point(p);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_round_point3_from_point", H);
    }
    { /* mu_ga_dipole3_make */
    float vx = ((float)((((153) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vy = ((float)((((153) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vz = ((float)((((153) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mx = ((float)((((153) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float my = ((float)((((153) * 37 + (4) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mz = ((float)((((153) * 37 + (5) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float px = ((float)((((153) * 37 + (6) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float py = ((float)((((153) * 37 + (7) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float pz = ((float)((((153) * 37 + (8) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float pw = ((float)((((153) * 37 + (9) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole3 r = mu_ga_dipole3_make(vx, vy, vz, mx, my, mz, px, py, pz, pw);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_dipole3_make", H);
    }
    { /* mu_ga_circle3_make */
    float gx = ((float)((((154) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gy = ((float)((((154) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gz = ((float)((((154) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float gw = ((float)((((154) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vx = ((float)((((154) * 37 + (4) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vy = ((float)((((154) * 37 + (5) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float vz = ((float)((((154) * 37 + (6) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mx = ((float)((((154) * 37 + (7) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float my = ((float)((((154) * 37 + (8) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float mz = ((float)((((154) * 37 + (9) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle3 r = mu_ga_circle3_make(gx, gy, gz, gw, vx, vy, vz, mx, my, mz);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_circle3_make", H);
    }
    { /* mu_ga_sphere3_make */
    float u = ((float)((((155) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float x = ((float)((((155) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float y = ((float)((((155) * 37 + (2) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float z = ((float)((((155) * 37 + (3) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    float w = ((float)((((155) * 37 + (4) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_sphere3 r = mu_ga_sphere3_make(u, x, y, z, w);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_sphere3_make", H);
    }
    { /* mu_ga_dual_round_point3 */
    mu_ga_round_point3 a;
    a.x = ((float)((((156) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((156) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((156) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((156) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.u = ((float)((((156) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_sphere3 r = mu_ga_dual_round_point3(a);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_dual_round_point3", H);
    }
    { /* mu_ga_dual_dipole3 */
    mu_ga_dipole3 d;
    d.vx = ((float)((((157) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((157) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((157) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((157) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((157) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((157) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((157) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((157) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((157) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((157) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle3 r = mu_ga_dual_dipole3(d);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_dual_dipole3", H);
    }
    { /* mu_ga_dual_circle3 */
    mu_ga_circle3 c;
    c.gx = ((float)((((158) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((158) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((158) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((158) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((158) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((158) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((158) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((158) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((158) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((158) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole3 r = mu_ga_dual_circle3(c);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_dual_circle3", H);
    }
    { /* mu_ga_dual_sphere3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((159) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((159) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((159) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((159) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((159) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_dual_sphere3(s);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_dual_sphere3", H);
    }
    { /* mu_ga_center_dipole3 */
    mu_ga_dipole3 d;
    d.vx = ((float)((((160) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((160) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((160) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((160) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((160) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((160) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((160) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((160) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((160) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((160) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_center_dipole3(d);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_center_dipole3", H);
    }
    { /* mu_ga_center_circle3 */
    mu_ga_circle3 c;
    c.gx = ((float)((((161) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((161) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((161) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((161) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((161) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((161) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((161) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((161) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((161) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((161) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_center_circle3(c);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_center_circle3", H);
    }
    { /* mu_ga_center_sphere3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((162) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((162) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((162) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((162) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((162) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_center_sphere3(s);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_center_sphere3", H);
    }
    { /* mu_ga_flat_center_dipole3 */
    mu_ga_dipole3 d;
    d.vx = ((float)((((163) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((163) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((163) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((163) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((163) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((163) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((163) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((163) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((163) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((163) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point3 r = mu_ga_flat_center_dipole3(d);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flat_center_dipole3", H);
    }
    { /* mu_ga_flat_center_circle3 */
    mu_ga_circle3 c;
    c.gx = ((float)((((164) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((164) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((164) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((164) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((164) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((164) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((164) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((164) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((164) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((164) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_flat_point3 r = mu_ga_flat_center_circle3(c);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_flat_center_circle3", H);
    }
    { /* mu_ga_container_dipole3 */
    mu_ga_dipole3 d;
    d.vx = ((float)((((165) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((165) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((165) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((165) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((165) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((165) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((165) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((165) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((165) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((165) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_sphere3 r = mu_ga_container_dipole3(d);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_container_dipole3", H);
    }
    { /* mu_ga_container_circle3 */
    mu_ga_circle3 c;
    c.gx = ((float)((((166) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((166) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((166) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((166) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((166) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((166) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((166) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((166) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((166) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((166) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_sphere3 r = mu_ga_container_circle3(c);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_container_circle3", H);
    }
    { /* mu_ga_round_point3_squared_radius_norm */
    mu_ga_round_point3 a;
    a.x = ((float)((((167) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((167) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((167) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((167) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.u = ((float)((((167) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_round_point3_squared_radius_norm(a);
        mix(&r, 1);
        check("mu_ga_round_point3_squared_radius_norm", H);
    }
    { /* mu_ga_dipole3_squared_radius_norm */
    mu_ga_dipole3 d;
    d.vx = ((float)((((168) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((168) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((168) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((168) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((168) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((168) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((168) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((168) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((168) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((168) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_dipole3_squared_radius_norm(d);
        mix(&r, 1);
        check("mu_ga_dipole3_squared_radius_norm", H);
    }
    { /* mu_ga_circle3_squared_radius_norm */
    mu_ga_circle3 c;
    c.gx = ((float)((((169) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((169) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((169) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((169) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((169) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((169) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((169) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((169) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((169) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((169) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_circle3_squared_radius_norm(c);
        mix(&r, 1);
        check("mu_ga_circle3_squared_radius_norm", H);
    }
    { /* mu_ga_sphere3_squared_radius_norm */
    mu_ga_sphere3 s;
    s.u = ((float)((((170) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((170) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((170) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((170) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((170) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        float r = mu_ga_sphere3_squared_radius_norm(s);
        mix(&r, 1);
        check("mu_ga_sphere3_squared_radius_norm", H);
    }
    { /* mu_ga_wedge_round_point3_round_point3 */
    mu_ga_round_point3 a;
    a.x = ((float)((((171) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((171) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((171) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((171) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.u = ((float)((((171) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_round_point3 b;
    b.x = ((float)((((171) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.y = ((float)((((171) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.z = ((float)((((171) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.w = ((float)((((171) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    b.u = ((float)((((171) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole3 r = mu_ga_wedge_round_point3_round_point3(a, b);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_wedge_round_point3_round_point3", H);
    }
    { /* mu_ga_wedge_flat_point3_round_point3 */
    mu_ga_flat_point3 p;
    p.x = ((float)((((172) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((172) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((172) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((172) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_round_point3 a;
    a.x = ((float)((((172) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((172) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((172) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((172) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.u = ((float)((((172) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line3 r = mu_ga_wedge_flat_point3_round_point3(p, a);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_wedge_flat_point3_round_point3", H);
    }
    { /* mu_ga_wedge_point3_round_point3 */
    mu_vec3 p = {((float)((((173) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((173) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((173) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
    mu_ga_round_point3 a;
    a.x = ((float)((((173) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((173) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((173) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((173) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.u = ((float)((((173) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_line3 r = mu_ga_wedge_point3_round_point3(p, a);
        { float flat[6]; memcpy(flat, &r, sizeof flat); mix(flat, 6); }
        check("mu_ga_wedge_point3_round_point3", H);
    }
    { /* mu_ga_wedge_dipole3_round_point3 */
    mu_ga_dipole3 d;
    d.vx = ((float)((((174) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((174) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((174) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((174) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((174) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((174) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((174) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((174) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((174) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((174) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_round_point3 a;
    a.x = ((float)((((174) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((174) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((174) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((174) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.u = ((float)((((174) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle3 r = mu_ga_wedge_dipole3_round_point3(d, a);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_wedge_dipole3_round_point3", H);
    }
    { /* mu_ga_wedge_line3_round_point3 */
    mu_ga_line3 l;
    l.vx = ((float)((((175) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((175) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((175) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((175) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((175) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((175) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_round_point3 a;
    a.x = ((float)((((175) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((175) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((175) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((175) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.u = ((float)((((175) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_plane3 r = mu_ga_wedge_line3_round_point3(l, a);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_wedge_line3_round_point3", H);
    }
    { /* mu_ga_wedge_dipole3_flat_point3 */
    mu_ga_dipole3 d;
    d.vx = ((float)((((176) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((176) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((176) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((176) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((176) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((176) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((176) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((176) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((176) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((176) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flat_point3 p;
    p.x = ((float)((((176) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((176) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((176) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((176) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_plane3 r = mu_ga_wedge_dipole3_flat_point3(d, p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_wedge_dipole3_flat_point3", H);
    }
    { /* mu_ga_wedge_dipole3_point3 */
    mu_ga_dipole3 d;
    d.vx = ((float)((((177) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((177) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((177) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((177) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((177) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((177) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((177) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((177) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((177) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((177) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 p = {((float)((((177) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((177) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((177) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_plane3 r = mu_ga_wedge_dipole3_point3(d, p);
        { float flat[4]; memcpy(flat, &r, sizeof flat); mix(flat, 4); }
        check("mu_ga_wedge_dipole3_point3", H);
    }
    { /* mu_ga_wedge_circle3_round_point3 */
    mu_ga_circle3 c;
    c.gx = ((float)((((178) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((178) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((178) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((178) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((178) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((178) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((178) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((178) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((178) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((178) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_round_point3 a;
    a.x = ((float)((((178) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.y = ((float)((((178) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.z = ((float)((((178) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.w = ((float)((((178) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    a.u = ((float)((((178) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_sphere3 r = mu_ga_wedge_circle3_round_point3(c, a);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_wedge_circle3_round_point3", H);
    }
    { /* mu_ga_wedge_dipole3_dipole3 */
    mu_ga_dipole3 d;
    d.vx = ((float)((((179) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((179) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((179) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((179) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((179) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((179) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((179) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((179) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((179) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((179) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_dipole3 f;
    f.vx = ((float)((((179) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.vy = ((float)((((179) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.vz = ((float)((((179) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.mx = ((float)((((179) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.my = ((float)((((179) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.mz = ((float)((((179) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.px = ((float)((((179) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.py = ((float)((((179) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pz = ((float)((((179) * 37 + (1) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    f.pw = ((float)((((179) * 37 + (1) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_sphere3 r = mu_ga_wedge_dipole3_dipole3(d, f);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_wedge_dipole3_dipole3", H);
    }
    { /* mu_ga_antiwedge_sphere3_sphere3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((180) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((180) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((180) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((180) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((180) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_sphere3 t;
    t.u = ((float)((((180) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    t.x = ((float)((((180) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    t.y = ((float)((((180) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    t.z = ((float)((((180) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    t.w = ((float)((((180) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle3 r = mu_ga_antiwedge_sphere3_sphere3(s, t);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_antiwedge_sphere3_sphere3", H);
    }
    { /* mu_ga_antiwedge_sphere3_plane3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((181) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((181) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((181) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((181) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((181) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_plane3 g;
    g.x = ((float)((((181) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((181) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((181) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((181) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_circle3 r = mu_ga_antiwedge_sphere3_plane3(s, g);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_antiwedge_sphere3_plane3", H);
    }
    { /* mu_ga_antiwedge_sphere3_circle3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((182) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((182) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((182) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((182) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((182) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_circle3 c;
    c.gx = ((float)((((182) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((182) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((182) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((182) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((182) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((182) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((182) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((182) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((182) * 37 + (1) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((182) * 37 + (1) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole3 r = mu_ga_antiwedge_sphere3_circle3(s, c);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_antiwedge_sphere3_circle3", H);
    }
    { /* mu_ga_antiwedge_plane3_circle3 */
    mu_ga_plane3 g;
    g.x = ((float)((((183) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((183) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((183) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((183) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_circle3 c;
    c.gx = ((float)((((183) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((183) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((183) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((183) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((183) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((183) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((183) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((183) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((183) * 37 + (1) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((183) * 37 + (1) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole3 r = mu_ga_antiwedge_plane3_circle3(g, c);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_antiwedge_plane3_circle3", H);
    }
    { /* mu_ga_antiwedge_sphere3_line3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((184) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((184) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((184) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((184) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((184) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line3 l;
    l.vx = ((float)((((184) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((184) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((184) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((184) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((184) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((184) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_dipole3 r = mu_ga_antiwedge_sphere3_line3(s, l);
        { float flat[10]; memcpy(flat, &r, sizeof flat); mix(flat, 10); }
        check("mu_ga_antiwedge_sphere3_line3", H);
    }
    { /* mu_ga_antiwedge_circle3_circle3 */
    mu_ga_circle3 c;
    c.gx = ((float)((((185) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((185) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((185) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((185) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((185) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((185) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((185) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((185) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((185) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((185) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_circle3 o;
    o.gx = ((float)((((185) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.gy = ((float)((((185) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.gz = ((float)((((185) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.gw = ((float)((((185) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.vx = ((float)((((185) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.vy = ((float)((((185) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.vz = ((float)((((185) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.mx = ((float)((((185) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.my = ((float)((((185) * 37 + (1) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    o.mz = ((float)((((185) * 37 + (1) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_antiwedge_circle3_circle3(c, o);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_antiwedge_circle3_circle3", H);
    }
    { /* mu_ga_antiwedge_circle3_line3 */
    mu_ga_circle3 c;
    c.gx = ((float)((((186) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gy = ((float)((((186) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gz = ((float)((((186) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.gw = ((float)((((186) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vx = ((float)((((186) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vy = ((float)((((186) * 37 + (0) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.vz = ((float)((((186) * 37 + (0) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mx = ((float)((((186) * 37 + (0) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.my = ((float)((((186) * 37 + (0) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    c.mz = ((float)((((186) * 37 + (0) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_line3 l;
    l.vx = ((float)((((186) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vy = ((float)((((186) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.vz = ((float)((((186) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mx = ((float)((((186) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.my = ((float)((((186) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    l.mz = ((float)((((186) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_antiwedge_circle3_line3(c, l);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_antiwedge_circle3_line3", H);
    }
    { /* mu_ga_antiwedge_sphere3_dipole3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((187) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((187) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((187) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((187) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((187) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_dipole3 d;
    d.vx = ((float)((((187) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((187) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((187) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((187) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((187) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((187) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((187) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((187) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((187) * 37 + (1) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((187) * 37 + (1) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_antiwedge_sphere3_dipole3(s, d);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_antiwedge_sphere3_dipole3", H);
    }
    { /* mu_ga_antiwedge_plane3_dipole3 */
    mu_ga_plane3 g;
    g.x = ((float)((((188) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.y = ((float)((((188) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.z = ((float)((((188) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    g.w = ((float)((((188) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_dipole3 d;
    d.vx = ((float)((((188) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vy = ((float)((((188) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.vz = ((float)((((188) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mx = ((float)((((188) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.my = ((float)((((188) * 37 + (1) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.mz = ((float)((((188) * 37 + (1) * 11 + (5) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.px = ((float)((((188) * 37 + (1) * 11 + (6) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.py = ((float)((((188) * 37 + (1) * 11 + (7) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pz = ((float)((((188) * 37 + (1) * 11 + (8) * 5) % 23) - 11) * 0.13f + 0.05f);
    d.pw = ((float)((((188) * 37 + (1) * 11 + (9) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_antiwedge_plane3_dipole3(g, d);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_antiwedge_plane3_dipole3", H);
    }
    { /* mu_ga_antiwedge_sphere3_flat_point3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((189) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((189) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((189) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((189) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((189) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_ga_flat_point3 p;
    p.x = ((float)((((189) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.y = ((float)((((189) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.z = ((float)((((189) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    p.w = ((float)((((189) * 37 + (1) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
        mu_ga_round_point3 r = mu_ga_antiwedge_sphere3_flat_point3(s, p);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_antiwedge_sphere3_flat_point3", H);
    }
    { /* mu_ga_antiwedge_sphere3_point3 */
    mu_ga_sphere3 s;
    s.u = ((float)((((190) * 37 + (0) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.x = ((float)((((190) * 37 + (0) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.y = ((float)((((190) * 37 + (0) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.z = ((float)((((190) * 37 + (0) * 11 + (3) * 5) % 23) - 11) * 0.13f + 0.05f);
    s.w = ((float)((((190) * 37 + (0) * 11 + (4) * 5) % 23) - 11) * 0.13f + 0.05f);
    mu_vec3 p = {((float)((((190) * 37 + (1) * 11 + (0) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((190) * 37 + (1) * 11 + (1) * 5) % 23) - 11) * 0.13f + 0.05f), ((float)((((190) * 37 + (1) * 11 + (2) * 5) % 23) - 11) * 0.13f + 0.05f)};
        mu_ga_round_point3 r = mu_ga_antiwedge_sphere3_point3(s, p);
        { float flat[5]; memcpy(flat, &r, sizeof flat); mix(flat, 5); }
        check("mu_ga_antiwedge_sphere3_point3", H);
    }
    printf("----------------------------------------------------------\n");
    if (g_fail == 0)
        printf("PASSED: %d checks, 0 failures\n", g_checks);
    else
        printf("FAILED: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
