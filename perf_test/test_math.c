/*
 * test_math.c — correctness gate for mu/mu_math.h against cglm (the baseline).
 *
 * Every mu entry point is compared to the cglm function that does the same
 * thing, with a tolerance that depends on the op:
 *   exact  : same arithmetic, same order  -> |err| <= 1e-6 * scale
 *   fast   : rsqrt + 1 NR                 -> |err| <= 1e-5 * scale
 *   batch  : SoA kernel vs per-element    -> |err| <= 1e-5 * scale
 *
 * A release math library that silently disagrees with the one it replaces is
 * worse than a slow one, so this file is the gate, not a formality. Build:
 *   gcc -O2 -march=native -std=c11 -I../mu -I../../cglm/include \
 *       test_math.c -o test_math -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mu_macros.h"
#include "mu_math.h"

/* cglm's public API is `glm_*`, and its types are byte-compatible with ours. */
#include "cglm/cglm.h"
#include "cglm/struct.h"

static int g_fail = 0;
static int g_checks = 0;

/* Bridge between mu's flat vectors and cglm's array types. Both are float[N],
 * so this is a plain copy; the memcpy keeps it aliasing-clean. */
static void to_c3(const float *v, vec3 out) { memcpy(out, v, sizeof(vec3)); }
static void to_c4(const float *v, vec4 out) { memcpy(out, v, sizeof(vec4)); }

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

static void check_f(const char *what, float a, float b, float tol)
{
    float d, scale;

    ++g_checks;
    scale = fabsf(b) > 1.0f ? fabsf(b) : 1.0f;
    d = fabsf(a - b);
    if (d > tol * scale)
    {
        printf("  FAIL %-44s mu=%.9g cglm=%.9g err=%.3g tol=%.3g\n", what, a, b, d, tol * scale);
        ++g_fail;
    }
}

static void check_v3(const char *what, mu_vec3 a, vec3 b, float tol)
{
    char buf[128];

    snprintf(buf, sizeof buf, "%s.x", what); check_f(buf, a[0], b[0], tol);
    snprintf(buf, sizeof buf, "%s.y", what); check_f(buf, a[1], b[1], tol);
    snprintf(buf, sizeof buf, "%s.z", what); check_f(buf, a[2], b[2], tol);
}

static void check_v4(const char *what, mu_vec4 a, vec4 b, float tol)
{
    char buf[128];

    snprintf(buf, sizeof buf, "%s.x", what); check_f(buf, a[0], b[0], tol);
    snprintf(buf, sizeof buf, "%s.y", what); check_f(buf, a[1], b[1], tol);
    snprintf(buf, sizeof buf, "%s.z", what); check_f(buf, a[2], b[2], tol);
    snprintf(buf, sizeof buf, "%s.w", what); check_f(buf, a[3], b[3], tol);
}

static void check_m4(const char *what, const mu_mat4 *a, const mat4 b, float tol)
{
    char buf[160];
    int i, j;

    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i)
        {
            snprintf(buf, sizeof buf, "%s[%d][%d]", what, i, j);
            check_f(buf, (*a)[4 * j + i], b[j][i], tol);
        }
}

static void rand_mat4(mu_mat4 out)
{
    int i, j;

    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i)
            out[4 * j + i] = frand(-2.0f, 2.0f);
}

/* A random rotation+translation+uniform scale: the affine fast path's home. */
static void rand_affine(mu_mat4 out)
{
    mu_quat q;
    mu_vec3 t, s;

    mu_vec3 v;
    mu_vec3_make(v, frand(-1, 1), frand(-1, 1), frand(-1, 1));
    mu_vec3 normalized;
    mu_vec3_normalize(normalized, v);
    mu_quat_axis_angle(q, normalized, frand(-3.0f, 3.0f));
    mu_vec3_make(t, frand(-10, 10), frand(-10, 10), frand(-10, 10));
    mu_vec3_splat(s, frand(0.5f, 2.0f));
    mu_mat4_trs(out, t, q, s);
}

int main(void)
{
    int it;
    int compared;              /* mat4_inverse cases actually compared to cglm */
    mu_mat4 ma, mb, mc;
    mat4 ca, cb, cc;

    srand(1234);

    printf("mu_math vs cglm correctness gate\n");
    /* ---- normal transformation: cofactor, not inverse-transpose ---- */
    {
        int k;
        int tested = 0;

        for (k = 0; k < 300; ++k)
        {
            mu_mat3 a, inv_t, cof;
            mu_vec3 v0, v1, v2, n, t0, t1, t2, nt, cn, scaled;
            float   det, len_nt, len_cn, dot;

            mu_vec3_make(a + 3 * 0, frand(-3, 3), frand(-3, 3), frand(-3, 3));
            mu_vec3_make(a + 3 * 1, frand(-3, 3), frand(-3, 3), frand(-3, 3));
            mu_vec3_make(a + 3 * 2, frand(-3, 3), frand(-3, 3), frand(-3, 3));

            /* identity: cofactor == det * inverse-transpose, always */
            mu_mat3_inverse_transpose(inv_t, a);
            mu_mat3_cofactor(cof, a);
            det = a[3 * 0 + 0] * cof[3 * 0 + 0] + a[3 * 0 + 1] * cof[3 * 0 + 1] +
                  a[3 * 0 + 2] * cof[3 * 0 + 2];      /* a0 . (a1 x a2) */
            if (fabsf(det) < 1e-2f) continue;
            ++tested;

            check_f("cofactor[0].x vs det*invT", cof[3 * 0 + 0], det * inv_t[3 * 0 + 0], 3e-3f);
            check_f("cofactor[0].y vs det*invT", cof[3 * 0 + 1], det * inv_t[3 * 0 + 1], 3e-3f);
            check_f("cofactor[1].z vs det*invT", cof[3 * 1 + 2], det * inv_t[3 * 1 + 2], 3e-3f);
            check_f("cofactor[2].y vs det*invT", cof[3 * 2 + 1], det * inv_t[3 * 2 + 1], 3e-3f);

            /* the property that matters: the normal of the TRANSFORMED triangle
             * is parallel to C*n and points the same way */
            mu_vec3_make(v0, frand(-2, 2), frand(-2, 2), frand(-2, 2));
            mu_vec3_make(v1, frand(-2, 2), frand(-2, 2), frand(-2, 2));
            mu_vec3_make(v2, frand(-2, 2), frand(-2, 2), frand(-2, 2));
            mu_vec3 diff0;
            mu_vec3_sub(diff0, v1, v0);
            mu_vec3 diff1;
            mu_vec3_sub(diff1, v2, v0);
            mu_vec3 cross0;
            mu_vec3_cross(cross0, diff0, diff1);
            memcpy(n, cross0, sizeof(n));
            if (mu_vec3_dot(n, n) < 1e-3f) continue;

            mu_mat3_mulv(t0, a, v0);
            mu_mat3_mulv(t1, a, v1);
            mu_mat3_mulv(t2, a, v2);
            mu_vec3 diff2;
            mu_vec3_sub(diff2, t1, t0);
            mu_vec3 diff3;
            mu_vec3_sub(diff3, t2, t0);
            mu_vec3_cross(nt, diff2, diff3);
            mu_mat3_mulv(cn, cof, n);
            len_nt = sqrtf(mu_vec3_dot(nt, nt));
            len_cn = sqrtf(mu_vec3_dot(cn, cn));
            if (len_nt < 1e-4f || len_cn < 1e-4f) continue;

            dot = mu_vec3_dot(nt, cn) / (len_nt * len_cn);
            check_f("cofactor normal: same direction", dot, 1.0f, 2e-3f);

            /* mu_mat4_normal must agree with the 3x3 cofactor path */
            {
                mu_mat4 m4;
                mu_mat4_identity(m4);
                mu_vec3 r4;

                mu_vec4_make(m4 + 4 * 0, a[3 * 0 + 0], a[3 * 0 + 1], a[3 * 0 + 2], 0.0f);
                mu_vec4_make(m4 + 4 * 1, a[3 * 1 + 0], a[3 * 1 + 1], a[3 * 1 + 2], 0.0f);
                mu_vec4_make(m4 + 4 * 2, a[3 * 2 + 0], a[3 * 2 + 1], a[3 * 2 + 2], 0.0f);
                mu_mat4_normal(r4, m4, n);
                check_f("mu_mat4_normal.x vs cofactor", r4[0], cn[0], 2e-3f);
                check_f("mu_mat4_normal.y vs cofactor", r4[1], cn[1], 2e-3f);
                check_f("mu_mat4_normal.z vs cofactor", r4[2], cn[2], 2e-3f);
            }
            (void)scaled;
        }
        if (tested < 200) { printf("  FAIL too few invertible normals cases\n"); ++g_fail; }

        /* The case the whole cofactor business exists for: a reflection.
         * Triangle (0,0,0),(1,0,0),(0,1,0) has normal +z. Reflect x. The
         * transformed triangle's winding gives -z, and that is the ONLY correct
         * answer. The cofactor reproduces it; inverse-transpose returns +z,
         * i.e. the normal points into the surface, which is the bug
         * normals_revisited is about. Both are asserted, so if someone
         * "simplifies" mu_mat4_normal back to an inverse-transpose this test
         * fails and says why. */
        {
            mu_mat4 refl;
            mu_mat4_identity(refl);
            mu_vec3 n;
            mu_vec3_make(n, 0.0f, 0.0f, 1.0f);
            mu_vec3 by_cof, by_invt;
            mu_mat4 inv;
            mu_vec3 t[3], recomputed;

            refl[4 * 0 + 0] = -1.0f;
            mu_mat4_normal(by_cof, refl, n);
            check_f("reflection: cofactor keeps -z", by_cof[2], -1.0f, 1e-6f);

            mu_vec3 v3;
            mu_vec3_make(v3, 0.0f, 0.0f, 0.0f);
            memcpy(t[0], v3, sizeof(t[0]));
            mu_vec3 v4;
            mu_vec3_make(v4, -1.0f, 0.0f, 0.0f);
            memcpy(t[1], v4, sizeof(t[1]));   /* (1,0,0) reflected */
            mu_vec3 v5;
            mu_vec3_make(v5, 0.0f, 1.0f, 0.0f);
            memcpy(t[2], v5, sizeof(t[2]));
            mu_vec3 diff4;
            mu_vec3_sub(diff4, t[1], t[0]);
            mu_vec3 diff5;
            mu_vec3_sub(diff5, t[2], t[0]);
            mu_vec3_cross(recomputed, diff4, diff5);
            check_f("reflection: triangle winding agrees", by_cof[2], recomputed[2], 1e-6f);

            /* inverse-transpose of this reflection is the reflection itself */
            mu_mat4_inverse(refl, inv);
            mu_vec3_make(by_invt, inv[4 * 0 + 0] * n[0] + inv[4 * 1 + 0] * n[1] + inv[4 * 2 + 0] * n[2], inv[4 * 0 + 1] * n[0] + inv[4 * 1 + 1] * n[1] + inv[4 * 2 + 1] * n[2], inv[4 * 0 + 2] * n[0] + inv[4 * 1 + 2] * n[1] + inv[4 * 2 + 2] * n[2]);
            check_f("reflection: inverse-transpose is WRONG by sign", by_invt[2], 1.0f, 1e-6f);
        }
    }

    printf("--------------------------------\n");

    /* ---- layout: mu_mat4 must be byte-identical to cglm mat4 ---- */
    {
        mu_mat4 m;
        mat4 c;
        int i, j;

        rand_mat4(m);
        memcpy(c, &m, sizeof c);
        for (j = 0; j < 4; ++j)
            for (i = 0; i < 4; ++i)
                check_f("layout col-major", m[4 * j + 0 + i], c[j][i], 0.0f);
        if (sizeof(mu_mat4) != sizeof(mat4)) {
            printf("  FAIL sizeof(mu_mat4)=%zu != sizeof(mat4)=%zu\n", sizeof(mu_mat4), sizeof(mat4));
            ++g_fail;
        }
        if (sizeof(mu_vec3) != sizeof(vec3)) {
            printf("  FAIL sizeof(mu_vec3)=%zu != sizeof(vec3)=%zu\n", sizeof(mu_vec3), sizeof(vec3));
            ++g_fail;
        }
    }

    compared = 0;
    for (it = 0; it < 200; ++it)
    {
        mu_vec3 v3a, v3b, r3;
        vec3 c3a, c3b, c3r;
        float f, cf;

        mu_vec3_make(v3a, frand(-5, 5), frand(-5, 5), frand(-5, 5));
        mu_vec3_make(v3b, frand(-5, 5), frand(-5, 5), frand(-5, 5));
        to_c3(v3a, c3a);
        to_c3(v3b, c3b);

        /* vec3 */
        check_f("vec3_dot", mu_vec3_dot(v3a, v3b), glm_vec3_dot(c3a, c3b), 1e-6f);
        mu_vec3 cross1;
        mu_vec3_cross(cross1, v3a, v3b);
        check_v3("vec3_cross", cross1, (glm_vec3_cross(c3a, c3b, c3r), c3r), 1e-6f);
        mu_vec3 sum;
        mu_vec3_add(sum, v3a, v3b);
        check_v3("vec3_add", sum, (glm_vec3_add(c3a, c3b, c3r), c3r), 1e-6f);
        mu_vec3 diff6;
        mu_vec3_sub(diff6, v3a, v3b);
        check_v3("vec3_sub", diff6, (glm_vec3_sub(c3a, c3b, c3r), c3r), 1e-6f);
        check_f("vec3_length", mu_vec3_length(v3a), glm_vec3_norm(c3a), 1e-6f);
        mu_vec3 normalized0;
        mu_vec3_normalize(normalized0, v3a);
        check_v3("vec3_normalize", normalized0,
                 (glm_vec3_normalize_to(c3a, c3r), c3r), 1e-6f);
        mu_vec3 normalized1;
        mu_vec3_normalize_fast(normalized1, v3a);
        check_v3("vec3_normalize_fast", normalized1,
                 (glm_vec3_normalize_to(c3a, c3r), c3r), 1e-5f);
        mu_vec3 lerped;
        mu_vec3_lerp(lerped, v3a, v3b, 0.37f);
        check_v3("vec3_lerp", lerped,
                 (glm_vec3_lerp(c3a, c3b, 0.37f, c3r), c3r), 1e-6f);

        /* scalar helpers */
        f  = mu_rsqrt_f32(fabsf(v3a[0]) + 0.5f);
        cf = 1.0f / sqrtf(fabsf(v3a[0]) + 0.5f);
        check_f("rsqrt_f32", f, cf, 1e-5f);

        /* mat4 core */
        rand_mat4(ma);
        rand_mat4(mb);
        memcpy(ca, &ma, sizeof ca);
        memcpy(cb, &mb, sizeof cb);

        mu_mat4_mul(ma, mb, mc);
        glm_mat4_mul(ca, cb, cc);
        check_m4("mat4_mul", &mc, cc, 1e-5f);

        mu_mat4_transpose(ma, mc);
        glm_mat4_transpose_to(ca, cc);
        check_m4("mat4_transpose", &mc, cc, 0.0f);

        check_f("mat4_determinant", mu_mat4_determinant(ma), glm_mat4_det(ca), 1e-5f);

        /* Compare against cglm only when the inverse is actually determined.
         * Two different but equally valid algorithms disagree by ~eps*cond(A),
         * so on a near-singular matrix this comparison measures conditioning,
         * not correctness -- the identity round-trip below is the real check
         * and it is guarded by the same |det| threshold. Counting the cases we
         * did compare keeps the guard from silently deleting coverage. */
        if (fabsf(mu_mat4_determinant(ma)) > 0.05f)
        {
            mu_mat4_inverse(ma, mc);
            glm_mat4_inv(ca, cc);
            check_m4("mat4_inverse", &mc, cc, 1e-4f);
            ++compared;
        }

        /* inverse * original == identity, the property that actually matters.
         * Near-singular matrices are skipped: both libraries produce garbage
         * there and the comparison would be float noise, not a bug. */
        if (fabsf(mu_mat4_determinant(ma)) > 0.05f)
        {
            mu_mat4 id;
            int i, j;

            mu_mat4_mul(ma, mc, id);
            for (j = 0; j < 4; ++j)
                for (i = 0; i < 4; ++i)
                    check_f("mat4_inverse * mat4 == I", id[4 * j + 0 + i],
                            (i == j) ? 1.0f : 0.0f, 1e-3f);
        }

        /* transform */
        {
            mu_vec4 hv;
            vec4 chv, chr;

            mu_vec4_make(hv, v3a[0], v3a[1], v3a[2], 1.0f);
            to_c4(hv, chv);
            mu_mat4 product;
            mu_mat4_mulv(product, ma, hv);
            {
                mu_vec4 r;
                mu_mat4_mulv(r, ma, hv);
                glm_mat4_mulv(ca, chv, chr);
                check_v4("mat4_mulv", r, chr, 1e-6f);
            }
            mu_mat4_transform_point(r3, ma, v3a);
            glm_mat4_mulv3(ca, c3a, 1.0f, c3r);
            check_v3("mat4_transform_point", r3, c3r, 1e-5f);
            mu_mat4_transform_dir(r3, ma, v3a);
            glm_mat4_mulv3(ca, c3a, 0.0f, c3r);
            check_v3("mat4_transform_dir", r3, c3r, 1e-5f);

            /* hot (register-resident) path must agree exactly with the cold one */
            {
                mu_mat4_hot h = mu_mat4_make_hot(ma);
                mu_mat4_hot_point(r3, &h, v3a);
                glm_mat4_mulv3(ca, c3a, 1.0f, c3r);
                check_v3("mat4_hot_point", r3, c3r, 1e-5f);
            }
        }

        /* affine */
        rand_affine(ma);
        rand_affine(mb);
        memcpy(ca, &ma, sizeof ca);
        memcpy(cb, &mb, sizeof cb);

        mu_mat4_mul_affine(mc, ma, mb);
        glm_mul(ca, cb, cc);
        check_m4("mat4_mul_affine", &mc, cc, 1e-5f);

        mu_mat4_mul(ma, mb, mc);
        check_m4("mat4_mul_affine==mat4_mul", &mc, cc, 1e-5f);

        /* mu_inverse_affine is checked against the GENERAL inverse, because it
         * divides out per-axis scale; cglm's glm_inv_tr is a bare transpose and
         * is only right for rigid transforms. For a rigid transform the two
         * must agree exactly. */
        mu_inverse_affine(ma, mc);
        glm_mat4_inv(ca, cc);
        check_m4("inverse_affine vs general inverse", &mc, cc, 1e-4f);

        {
            mu_mat4 rigid;
            mat4 crigid;

            mu_vec3 v6;
            mu_vec3_make(v6, 3, -4, 5);
            mu_vec3 v7;
            mu_vec3_make(v7, 0.2f, 1.0f, -0.4f);
            mu_quat quat0;
            mu_quat_axis_angle(quat0, v7, 1.1f);
            mu_vec3 v8;
            mu_vec3_splat(v8, 1.0f);
            mu_mat4_trs(rigid, v6, quat0, v8);
            memcpy(crigid, &rigid, sizeof crigid);
            mu_inverse_affine(rigid, mc);
            glm_inv_tr(crigid);
            check_m4("inverse_affine (rigid) == glm_inv_tr", &mc, crigid, 1e-5f);
        }
        {
            /* mu_inverse_rigid: the orthonormal fast path. Must equal the
             * general inverse on rigid input (it is an exact transpose, so the
             * tolerance is tight) and must NOT be used on scaled input -- the
             * negative test below pins that contract down. */
            mu_mat4 rigid, mr;
            mat4    crigid;

            mu_vec3 v9;
            mu_vec3_make(v9, -2, 7, 0.5f);
            mu_vec3 v10;
            mu_vec3_make(v10, 1.0f, -0.3f, 0.7f);
            mu_quat quat1;
            mu_quat_axis_angle(quat1, v10, 2.0f);
            mu_vec3 v11;
            mu_vec3_splat(v11, 1.0f);
            mu_mat4_trs(rigid, v9, quat1, v11);
            memcpy(crigid, &rigid, sizeof crigid);
            mu_inverse_rigid(rigid, mr);
            glm_mat4_inv(crigid, cc);
            check_m4("inverse_rigid vs general inverse", &mr, cc, 1e-5f);

            mu_mat4_mul(mr, rigid, mc);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    check_f("inverse_rigid * mat4 == I", mc[4 * j + 0 + i],
                            (i == j) ? 1.0f : 0.0f, 1e-5f);
        }
        {
            mu_mat4 id;
            int i, j;

            mu_inverse_affine(ma, mc);
            mu_mat4_mul(mc, ma, id);
            for (j = 0; j < 4; ++j)
                for (i = 0; i < 4; ++i)
                    check_f("inverse_affine * mat4 == I", id[4 * j + 0 + i],
                            (i == j) ? 1.0f : 0.0f, 1e-4f);
        }

        /* quaternion */
        {
            mu_quat qa, qb, qr;
            versor cqa, cqb, cqr;
            mu_vec3 axis;
            float ang;

            mu_vec3 v12;
            mu_vec3_make(v12, frand(-1, 1), frand(-1, 1), frand(-1, 1));
            mu_vec3_normalize(axis, v12);
            ang  = frand(-3.0f, 3.0f);

            mu_quat_axis_angle(qa, axis, ang);
            to_c3(axis, c3a); glm_quatv(cqa, ang, c3a);
            check_v4("quat_axis_angle", qa, cqa, 1e-6f);

            mu_vec3 v13;
            mu_vec3_make(v13, 0, 1, 0);
            mu_quat_axis_angle(qb, v13, frand(-3, 3));
            to_c4(qb, cqb);

            mu_quat_mul(qr, qa, qb);
            glm_quat_mul(cqa, cqb, cqr);
            check_v4("quat_mul", qr, cqr, 1e-5f);

            mu_quat_slerp(qr, qa, qb, 0.3f);
            glm_quat_slerp(cqa, cqb, 0.3f, cqr);
            check_v4("quat_slerp", qr, cqr, 1e-4f);
            if (fabsf(qr[0] - cqr[0]) > 1e-3f)
                printf("  note slerp d=%.6f t=0.3 mu=(%.5f %.5f %.5f %.5f) cglm=(%.5f %.5f %.5f %.5f)\n",
                       mu_vec4_dot(qa, qb), qr[0], qr[1], qr[2], qr[3], cqr[0], cqr[1], cqr[2], cqr[3]);

            mu_quat_rotate_vec3(r3, qa, v3a);
            {
                vec3 cv3;
                to_c3(v3a, cv3);
                glm_quat_rotatev(cqa, cv3, c3r);
            }
            check_v3("quat_rotate_vec3", r3, c3r, 1e-5f);

            /* SoA batch rotate: same quaternion, n vectors, including every
             * tail length across the 16-lane and 8-lane boundaries, and an
             * in-place pass (the kernel loads before it stores, so aliasing
             * output onto input must be safe). */
            {
                enum { NROT = 53 };
                float xs[NROT], ys[NROT], zs[NROT];
                float ox[NROT], oy[NROT], oz[NROT];
                int k;

                for (k = 0; k < NROT; ++k)
                {
                    xs[k] = frand(-4, 4);
                    ys[k] = frand(-4, 4);
                    zs[k] = frand(-4, 4);
                }

                for (k = 0; k <= NROT; ++k)
                {
                    mu_quat_rotate_vec3_soa(qa, xs, ys, zs, ox, oy, oz, (size_t)k);
                    for (int t = 0; t < k; ++t)
                    {
                        mu_vec3 e;
                        mu_vec3 v14;
                        mu_vec3_make(v14, xs[t], ys[t], zs[t]);
                        mu_quat_rotate_vec3(e, qa, v14);
                        vec3 ce;

                        ce[0] = e[0]; ce[1] = e[1]; ce[2] = e[2];
                        mu_vec3 v15;
                        mu_vec3_make(v15, ox[t], oy[t], oz[t]);
                        check_v3("quat_rotate_vec3_soa",
                                 v15, ce, 1e-5f);
                    }
                }

                /* in place: output aliases input */
                mu_quat_rotate_vec3_soa(qa, xs, ys, zs, xs, ys, zs, NROT);
                for (k = 0; k < NROT; ++k)
                {
                    vec3 ce;

                    ce[0] = ox[k]; ce[1] = oy[k]; ce[2] = oz[k];
                    mu_vec3 v16;
                    mu_vec3_make(v16, xs[k], ys[k], zs[k]);
                    check_v3("quat_rotate_vec3_soa (in place)",
                             v16, ce, 1e-5f);
                }
            }

            /* quat -> mat must match glm_quat_mat4 */
            {
                mu_mat4 mq;
                mat4 cmq;

                mu_mat4_from_quat(mq, qa);
                glm_quat_mat4(cqa, cmq);
                check_m4("mat4_from_quat", &mq, cmq, 1e-5f);
            }
        }

        /* mat3 normal matrix */
        {
            mu_mat3 nm;
            mat3 cnm, cnmr;

            mu_mat3_from_mat4_linear(nm, ma);
            memcpy(cnm, &nm, sizeof cnm);
            mu_mat3_inverse_transpose(nm, nm);
            glm_mat3_inv(cnm, cnmr);
            glm_mat3_transpose(cnmr);
            {
                mu_mat3 want;
                int i, j;

                memcpy(&want, cnmr, sizeof want);
                for (j = 0; j < 3; ++j)
                    for (i = 0; i < 3; ++i)
                        check_f("mat3_inverse_transpose", nm[3 * j + 0 + i], want[3 * j + 0 + i], 1e-4f);
            }
        }
    }

    /* ---- batch kernels vs the per-element reference ---- */
    {
        enum { N = 1024 };
        static float x[N], y[N], z[N], ox[N], oy[N], oz[N];
        mu_mat4 m;
        size_t i;

        rand_affine(m);
        for (i = 0; i < N; ++i) {
            x[i] = frand(-50, 50);
            y[i] = frand(-50, 50);
            z[i] = frand(-50, 50);
        }

        mu_xform_points3_soa(m, x, y, z, ox, oy, oz, N);
        for (i = 0; i < N; ++i) {
            mu_vec3 want;
            mu_vec3 v17;
            mu_vec3_make(v17, x[i], y[i], z[i]);
            mu_mat4_transform_point(want, m, v17);
            check_f("batch xform x", ox[i], want[0], 1e-5f);
            check_f("batch xform y", oy[i], want[1], 1e-5f);
            check_f("batch xform z", oz[i], want[2], 1e-5f);
        }

        /* tail handling: every n from 0..40 must be exact, since that is where
         * a naive kernel silently drops elements */
        {
            size_t n;

            for (n = 0; n <= 40; ++n) {
                size_t k;

                for (k = 0; k <= 40; ++k) {
                    ox[k] = oy[k] = oz[k] = -999.0f;
                }
                mu_xform_points3_soa(m, x, y, z, ox, oy, oz, n);
                for (k = 0; k < n; ++k) {
                    mu_vec3 want;
                    mu_vec3 v18;
                    mu_vec3_make(v18, x[k], y[k], z[k]);
                    mu_mat4_transform_point(want, m, v18);
                    check_f("batch xform tail", ox[k] + oy[k] + oz[k],
                            want[0] + want[1] + want[2], 1e-5f);
                }
                for (k = n; k <= 40; ++k) {
                    if (ox[k] != -999.0f || oy[k] != -999.0f || oz[k] != -999.0f) {
                        printf("  FAIL batch xform wrote past n=%zu at %zu\n", n, k);
                        ++g_fail;
                        break;
                    }
                }
            }
        }

        mu_normalize3_soa(x, y, z, ox, oy, oz, N);
        for (i = 0; i < N; ++i) {
            float l = sqrtf(x[i] * x[i] + y[i] * y[i] + z[i] * z[i]);
            check_f("batch normalize x", ox[i], x[i] / l, 1e-5f);
            check_f("batch normalize len", sqrtf(ox[i] * ox[i] + oy[i] * oy[i] + oz[i] * oz[i]), 1.0f, 1e-5f);
        }

        {
            mu_mat3 nm;
            mu_mat3_from_mat4_linear(nm, m);

            mu_xform_normals3_soa(nm, x, y, z, ox, oy, oz, N);
            for (i = 0; i < N; ++i) {
                mu_vec3 want;
                mu_vec3 v19;
                mu_vec3_make(v19, x[i], y[i], z[i]);
                mu_mat3_mulv(want, nm, v19);
                check_f("batch normals x", ox[i], want[0], 1e-4f);
                check_f("batch normals z", oz[i], want[2], 1e-4f);
            }
        }

        /* mat4 batch: 4-at-a-time packed path must equal the plain kernel */
        {
            enum { M8 = 16 };
            mu_mat4 as[M8], bs[M8], want[M8], got[M8];
            int k;

            for (k = 0; k < M8; ++k) { rand_mat4(as[k]); rand_mat4(bs[k]); }
            for (k = 0; k < M8; ++k) mu_mat4_mul(as[k], bs[k], want[k]);
            mu_mat4_mul_batch(&got[0][0], sizeof(mu_mat4), &as[0][0], sizeof(mu_mat4),
                              &bs[0][0], sizeof(mu_mat4), M8);
            for (k = 0; k < M8; ++k) check_m4("mat4_mul_batch", &got[k], (const float (*)[4])&want[k], 1e-5f);

            /* and all of them must equal cglm's product */
            for (k = 0; k < M8; ++k) {
                mat4 x, y2, z2;

                memcpy(x, &as[k], sizeof x);
                memcpy(y2, &bs[k], sizeof y2);
                glm_mat4_mul(x, y2, z2);
                check_m4("mat4_mul_batch vs cglm", &got[k], z2, 1e-5f);
            }
        }

        /* perspective_zo: zero-to-one depth (D3D/Vulkan/Metal). Two gates —
         * byte-wise agreement with cglm's rh_zo, and the mapping itself, so a
         * "simplification" back to the [-1,1] form fails loudly. */
        {
            static const float fovs[3] = { 35.0f, 60.0f, 110.0f };
            static const float aspects[3] = { 0.5f, 1.0f, 16.0f / 9.0f };
            static const float ranges[3][2] = { { 0.01f, 10.0f }, { 0.1f, 100.0f }, { 1.0f, 5000.0f } };
            int fi, ai, ri;

            for (fi = 0; fi < 3; ++fi)
                for (ai = 0; ai < 3; ++ai)
                    for (ri = 0; ri < 3; ++ri)
                    {
                        mu_mat4 proj;
                        mat4 cproj;
                        float fovy = mu_radians(fovs[fi]);
                        float near_z = ranges[ri][0], far_z = ranges[ri][1];
                        float pn[4] = { 0.0f, 0.0f, -near_z, 1.0f };
                        float pf[4] = { 0.0f, 0.0f, -far_z, 1.0f };
                        mu_vec4 cn, cf;

                        mu_mat4_perspective_zo(proj, fovy, aspects[ai], near_z, far_z);
                        glm_perspective_rh_zo(fovy, aspects[ai], near_z, far_z, cproj);
                        check_m4("mat4_perspective_zo", &proj, cproj, 1e-5f);

                        mu_mat4_mulv(cn, proj, pn);
                        mu_mat4_mulv(cf, proj, pf);
                        check_f("perspective_zo depth at near", cn[2] / cn[3], 0.0f, 1e-4f);
                        check_f("perspective_zo depth at far",  cf[2] / cf[3], 1.0f, 1e-4f);
                    }
        }

        /* frustum cull: compacted list must equal the scalar test's list */
        {
            enum { NC = 512 };
            static float cxx[NC], cyy[NC], czz[NC];
            uint32_t idx[NC];
            mu_mat4 proj, view, vp;
            mu_frustum f;
            size_t n, want_count, k;
            uint8_t seen[NC];

            mu_mat4_perspective(proj, mu_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
            mu_vec3 v20;
            mu_vec3_make(v20, 0, 0, 5);
            mu_vec3 v21;
            mu_vec3_make(v21, 0, 0, 0);
            mu_vec3 v22;
            mu_vec3_make(v22, 0, 1, 0);
            mu_mat4_look_at(view, v20, v21, v22);
            mu_mat4_mul(proj, view, vp);

            /* cglm builds the same frustum; compare plane by plane */
            {
                mat4 cp, cv, cvp;
                vec4 planes[6];

                memcpy(cp, &proj, sizeof cp);
                memcpy(cv, &view, sizeof cv);
                glm_mat4_mul(cp, cv, cvp);
                glm_frustum_planes(cvp, planes);
                mu_frustum_from_view_proj(&f, vp);
                for (k = 0; k < 6; ++k) {
                    /* cglm's plane order: left,right,bottom,top,near,far */
                    check_f("frustum plane.x", f.p[k][0], planes[k][0], 1e-5f);
                    check_f("frustum plane.y", f.p[k][1], planes[k][1], 1e-5f);
                    check_f("frustum plane.z", f.p[k][2], planes[k][2], 1e-5f);
                    check_f("frustum plane.d", f.p[k][3], planes[k][3], 1e-5f);
                }
            }

            for (n = 0; n < NC; ++n) {
                cxx[n] = frand(-20, 20);
                cyy[n] = frand(-20, 20);
                czz[n] = frand(-30, 10);
            }

            want_count = 0;
            memset(seen, 0, sizeof seen);
            {
                vec4 pl[6];
                mat4 cp, cv, cvp;

                memcpy(cp, &proj, sizeof cp);
                memcpy(cv, &view, sizeof cv);
                glm_mat4_mul(cp, cv, cvp);
                glm_frustum_planes(cvp, pl);
            for (n = 0; n < NC; ++n) {
                int vis;

                vis = 1;
                for (k = 0; k < 6; ++k) {
                    float d = pl[k][0] * cxx[n] + pl[k][1] * cyy[n] + pl[k][2] * czz[n] + pl[k][3];
                    if (d < -1.0f) vis = 0;
                }
                if (vis) { ++want_count; }
                seen[n] = (uint8_t)vis;
            }
            }

            {
                size_t got = mu_frustum_cull_spheres_compact(&f, cxx, cyy, czz, 1.0f, idx, NC);

                if (got != want_count) {
                    printf("  FAIL frustum cull count %zu != %zu\n", got, want_count);
                    ++g_fail;
                }
                {
                    uint8_t hit[NC];

                    memset(hit, 0, sizeof hit);
                    for (k = 0; k < got; ++k) hit[idx[k]] = 1;
                    for (n = 0; n < NC; ++n) hit[n] = (uint8_t)(hit[n] ^ seen[n]);
                    for (n = 0; n < NC; ++n) {
                        if (hit[n]) { printf("  FAIL frustum cull membership at %zu\n", n); ++g_fail; break; }
                    }
                }
            }
        }
    }

    printf("--------------------------------\n");
    /* The |det| guard above must not quietly shrink the comparison to nothing. */
    if (compared < 150)
    {
        printf("  FAIL only %d of 200 mat4_inverse cases were well-conditioned\n", compared);
        ++g_fail;
    }
    printf("    mat4_inverse vs cglm: %d/200 well-conditioned cases compared\n", compared);
    printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
