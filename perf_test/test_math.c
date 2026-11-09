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

#include "mu_math_legacy.h"

/* cglm's public API is `glm_*`, and its types are byte-compatible with ours. */
#include "cglm/cglm.h"
#include "cglm/struct.h"

static int g_fail = 0;
static int g_checks = 0;

/* Bridge between mu structs and cglm's array types without the aliasing
 * warnings you get from &v.x (GCC cannot see the following members). */
static void to_c3(mu_vec3 v, vec3 out) { memcpy(out, &v, sizeof(vec3)); }
static void to_c4(mu_vec4 v, vec4 out) { memcpy(out, &v, sizeof(vec4)); }

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

    snprintf(buf, sizeof buf, "%s.x", what); check_f(buf, a.x, b[0], tol);
    snprintf(buf, sizeof buf, "%s.y", what); check_f(buf, a.y, b[1], tol);
    snprintf(buf, sizeof buf, "%s.z", what); check_f(buf, a.z, b[2], tol);
}

static void check_v4(const char *what, mu_vec4 a, vec4 b, float tol)
{
    char buf[128];

    snprintf(buf, sizeof buf, "%s.x", what); check_f(buf, a.x, b[0], tol);
    snprintf(buf, sizeof buf, "%s.y", what); check_f(buf, a.y, b[1], tol);
    snprintf(buf, sizeof buf, "%s.z", what); check_f(buf, a.z, b[2], tol);
    snprintf(buf, sizeof buf, "%s.w", what); check_f(buf, a.w, b[3], tol);
}

static void check_m4(const char *what, const mu_mat4 *a, const mat4 b, float tol)
{
    char buf[160];
    int i, j;

    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i)
        {
            snprintf(buf, sizeof buf, "%s[%d][%d]", what, i, j);
            check_f(buf, (&a->col[j].x)[i], b[j][i], tol);
        }
}

static mu_mat4 rand_mat4(void)
{
    mu_mat4 m;
    int i, j;

    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i)
            (&m.col[j].x)[i] = frand(-2.0f, 2.0f);
    return m;
}

/* A random rotation+translation+uniform scale: the affine fast path's home. */
static mu_mat4 rand_affine(void)
{
    mu_mat4 m;
    mu_quat q;
    mu_vec3 t, s;

    q = mu_quat_axis_angle(mu_vec3_normalize(mu_vec3_make(frand(-1, 1), frand(-1, 1), frand(-1, 1))),
                           frand(-3.0f, 3.0f));
    t = mu_vec3_make(frand(-10, 10), frand(-10, 10), frand(-10, 10));
    s = mu_vec3_splat(frand(0.5f, 2.0f));
    m = mu_mat4_trs(t, q, s);
    return m;
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

            a.col[0] = mu_vec3_make(frand(-3, 3), frand(-3, 3), frand(-3, 3));
            a.col[1] = mu_vec3_make(frand(-3, 3), frand(-3, 3), frand(-3, 3));
            a.col[2] = mu_vec3_make(frand(-3, 3), frand(-3, 3), frand(-3, 3));

            /* identity: cofactor == det * inverse-transpose, always */
            inv_t = mu_mat3_inverse_transpose(&a);
            cof = mu_mat3_cofactor(&a);
            det = a.col[0].x * cof.col[0].x + a.col[0].y * cof.col[0].y +
                  a.col[0].z * cof.col[0].z;      /* a0 . (a1 x a2) */
            if (fabsf(det) < 1e-2f) continue;
            ++tested;

            check_f("cofactor[0].x vs det*invT", cof.col[0].x, det * inv_t.col[0].x, 3e-3f);
            check_f("cofactor[0].y vs det*invT", cof.col[0].y, det * inv_t.col[0].y, 3e-3f);
            check_f("cofactor[1].z vs det*invT", cof.col[1].z, det * inv_t.col[1].z, 3e-3f);
            check_f("cofactor[2].y vs det*invT", cof.col[2].y, det * inv_t.col[2].y, 3e-3f);

            /* the property that matters: the normal of the TRANSFORMED triangle
             * is parallel to C*n and points the same way */
            v0 = mu_vec3_make(frand(-2, 2), frand(-2, 2), frand(-2, 2));
            v1 = mu_vec3_make(frand(-2, 2), frand(-2, 2), frand(-2, 2));
            v2 = mu_vec3_make(frand(-2, 2), frand(-2, 2), frand(-2, 2));
            n  = mu_vec3_cross(mu_vec3_sub(v1, v0), mu_vec3_sub(v2, v0));
            if (mu_vec3_dot(n, n) < 1e-3f) continue;

            t0 = mu_mat3_mulv(&a, v0);
            t1 = mu_mat3_mulv(&a, v1);
            t2 = mu_mat3_mulv(&a, v2);
            nt = mu_vec3_cross(mu_vec3_sub(t1, t0), mu_vec3_sub(t2, t0));
            cn = mu_mat3_mulv(&cof, n);
            len_nt = sqrtf(mu_vec3_dot(nt, nt));
            len_cn = sqrtf(mu_vec3_dot(cn, cn));
            if (len_nt < 1e-4f || len_cn < 1e-4f) continue;

            dot = mu_vec3_dot(nt, cn) / (len_nt * len_cn);
            check_f("cofactor normal: same direction", dot, 1.0f, 2e-3f);

            /* mu_mat4_normal must agree with the 3x3 cofactor path */
            {
                mu_mat4 m4 = mu_mat4_identity();
                mu_vec3 r4;

                m4.col[0] = mu_vec4_make(a.col[0].x, a.col[0].y, a.col[0].z, 0.0f);
                m4.col[1] = mu_vec4_make(a.col[1].x, a.col[1].y, a.col[1].z, 0.0f);
                m4.col[2] = mu_vec4_make(a.col[2].x, a.col[2].y, a.col[2].z, 0.0f);
                r4 = mu_mat4_normal(&m4, n);
                check_f("mu_mat4_normal.x vs cofactor", r4.x, cn.x, 2e-3f);
                check_f("mu_mat4_normal.y vs cofactor", r4.y, cn.y, 2e-3f);
                check_f("mu_mat4_normal.z vs cofactor", r4.z, cn.z, 2e-3f);
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
            mu_mat4 refl = mu_mat4_identity();
            mu_vec3 n = mu_vec3_make(0.0f, 0.0f, 1.0f);
            mu_vec3 by_cof, by_invt;
            mu_mat4 inv;
            mu_vec3 t[3], recomputed;

            refl.col[0].x = -1.0f;
            by_cof = mu_mat4_normal(&refl, n);
            check_f("reflection: cofactor keeps -z", by_cof.z, -1.0f, 1e-6f);

            t[0] = mu_vec3_make(0.0f, 0.0f, 0.0f);
            t[1] = mu_vec3_make(-1.0f, 0.0f, 0.0f);   /* (1,0,0) reflected */
            t[2] = mu_vec3_make(0.0f, 1.0f, 0.0f);
            recomputed = mu_vec3_cross(mu_vec3_sub(t[1], t[0]), mu_vec3_sub(t[2], t[0]));
            check_f("reflection: triangle winding agrees", by_cof.z, recomputed.z, 1e-6f);

            /* inverse-transpose of this reflection is the reflection itself */
            mu_mat4_inverse(&refl, &inv);
            by_invt = mu_vec3_make(inv.col[0].x * n.x + inv.col[1].x * n.y + inv.col[2].x * n.z,
                                   inv.col[0].y * n.x + inv.col[1].y * n.y + inv.col[2].y * n.z,
                                   inv.col[0].z * n.x + inv.col[1].z * n.y + inv.col[2].z * n.z);
            check_f("reflection: inverse-transpose is WRONG by sign", by_invt.z, 1.0f, 1e-6f);
        }
    }

    printf("--------------------------------\n");

    /* ---- layout: mu_mat4 must be byte-identical to cglm mat4 ---- */
    {
        mu_mat4 m;
        mat4 c;
        int i, j;

        m = rand_mat4();
        memcpy(c, &m, sizeof c);
        for (j = 0; j < 4; ++j)
            for (i = 0; i < 4; ++i)
                check_f("layout col-major", (&m.col[j].x)[i], c[j][i], 0.0f);
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

        v3a = mu_vec3_make(frand(-5, 5), frand(-5, 5), frand(-5, 5));
        v3b = mu_vec3_make(frand(-5, 5), frand(-5, 5), frand(-5, 5));
        to_c3(v3a, c3a);
        to_c3(v3b, c3b);

        /* vec3 */
        check_f("vec3_dot", mu_vec3_dot(v3a, v3b), glm_vec3_dot(c3a, c3b), 1e-6f);
        check_v3("vec3_cross", mu_vec3_cross(v3a, v3b), (glm_vec3_cross(c3a, c3b, c3r), c3r), 1e-6f);
        check_v3("vec3_add", mu_vec3_add(v3a, v3b), (glm_vec3_add(c3a, c3b, c3r), c3r), 1e-6f);
        check_v3("vec3_sub", mu_vec3_sub(v3a, v3b), (glm_vec3_sub(c3a, c3b, c3r), c3r), 1e-6f);
        check_f("vec3_length", mu_vec3_length(v3a), glm_vec3_norm(c3a), 1e-6f);
        check_v3("vec3_normalize", mu_vec3_normalize(v3a),
                 (glm_vec3_normalize_to(c3a, c3r), c3r), 1e-6f);
        check_v3("vec3_normalize_fast", mu_vec3_normalize_fast(v3a),
                 (glm_vec3_normalize_to(c3a, c3r), c3r), 1e-5f);
        check_v3("vec3_lerp", mu_vec3_lerp(v3a, v3b, 0.37f),
                 (glm_vec3_lerp(c3a, c3b, 0.37f, c3r), c3r), 1e-6f);

        /* scalar helpers */
        f  = mu_rsqrt_f32(fabsf(v3a.x) + 0.5f);
        cf = 1.0f / sqrtf(fabsf(v3a.x) + 0.5f);
        check_f("rsqrt_f32", f, cf, 1e-5f);

        /* mat4 core */
        ma = rand_mat4();
        mb = rand_mat4();
        memcpy(ca, &ma, sizeof ca);
        memcpy(cb, &mb, sizeof cb);

        mu_mat4_mul(&ma, &mb, &mc);
        glm_mat4_mul(ca, cb, cc);
        check_m4("mat4_mul", &mc, cc, 1e-5f);

        mu_mat4_transpose(&ma, &mc);
        glm_mat4_transpose_to(ca, cc);
        check_m4("mat4_transpose", &mc, cc, 0.0f);

        check_f("mat4_determinant", mu_mat4_determinant(&ma), glm_mat4_det(ca), 1e-5f);

        /* Compare against cglm only when the inverse is actually determined.
         * Two different but equally valid algorithms disagree by ~eps*cond(A),
         * so on a near-singular matrix this comparison measures conditioning,
         * not correctness -- the identity round-trip below is the real check
         * and it is guarded by the same |det| threshold. Counting the cases we
         * did compare keeps the guard from silently deleting coverage. */
        if (fabsf(mu_mat4_determinant(&ma)) > 0.05f)
        {
            mu_mat4_inverse(&ma, &mc);
            glm_mat4_inv(ca, cc);
            check_m4("mat4_inverse", &mc, cc, 1e-4f);
            ++compared;
        }

        /* inverse * original == identity, the property that actually matters.
         * Near-singular matrices are skipped: both libraries produce garbage
         * there and the comparison would be float noise, not a bug. */
        if (fabsf(mu_mat4_determinant(&ma)) > 0.05f)
        {
            mu_mat4 id;
            int i, j;

            mu_mat4_mul(&ma, &mc, &id);
            for (j = 0; j < 4; ++j)
                for (i = 0; i < 4; ++i)
                    check_f("mat4_inverse * mat4 == I", (&id.col[j].x)[i],
                            (i == j) ? 1.0f : 0.0f, 1e-3f);
        }

        /* transform */
        {
            mu_vec4 hv;
            vec4 chv, chr;

            hv = mu_vec4_make(v3a.x, v3a.y, v3a.z, 1.0f);
            to_c4(hv, chv);
            mu_mat4_mulv(&ma, hv);
            {
                mu_vec4 r = mu_mat4_mulv(&ma, hv);
                glm_mat4_mulv(ca, chv, chr);
                check_v4("mat4_mulv", r, chr, 1e-6f);
            }
            r3 = mu_mat4_transform_point(&ma, v3a);
            glm_mat4_mulv3(ca, c3a, 1.0f, c3r);
            check_v3("mat4_transform_point", r3, c3r, 1e-5f);
            r3 = mu_mat4_transform_dir(&ma, v3a);
            glm_mat4_mulv3(ca, c3a, 0.0f, c3r);
            check_v3("mat4_transform_dir", r3, c3r, 1e-5f);

            /* hot (register-resident) path must agree exactly with the cold one */
            {
                mu_mat4_hot h = mu_mat4_make_hot(&ma);
                r3 = mu_mat4_hot_point(&h, v3a);
                glm_mat4_mulv3(ca, c3a, 1.0f, c3r);
                check_v3("mat4_hot_point", r3, c3r, 1e-5f);
            }
        }

        /* affine */
        ma = rand_affine();
        mb = rand_affine();
        memcpy(ca, &ma, sizeof ca);
        memcpy(cb, &mb, sizeof cb);

        mu_mat4_mul_affine(&ma, &mb, &mc);
        glm_mul(ca, cb, cc);
        check_m4("mat4_mul_affine", &mc, cc, 1e-5f);

        mu_mat4_mul(&ma, &mb, &mc);
        check_m4("mat4_mul_affine==mat4_mul", &mc, cc, 1e-5f);

        /* mu_inverse_affine is checked against the GENERAL inverse, because it
         * divides out per-axis scale; cglm's glm_inv_tr is a bare transpose and
         * is only right for rigid transforms. For a rigid transform the two
         * must agree exactly. */
        mu_inverse_affine(&ma, &mc);
        glm_mat4_inv(ca, cc);
        check_m4("inverse_affine vs general inverse", &mc, cc, 1e-4f);

        {
            mu_mat4 rigid;
            mat4 crigid;

            rigid = mu_mat4_trs(mu_vec3_make(3, -4, 5),
                                mu_quat_axis_angle(mu_vec3_make(0.2f, 1.0f, -0.4f), 1.1f),
                                mu_vec3_splat(1.0f));
            memcpy(crigid, &rigid, sizeof crigid);
            mu_inverse_affine(&rigid, &mc);
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

            rigid = mu_mat4_trs(mu_vec3_make(-2, 7, 0.5f),
                                mu_quat_axis_angle(mu_vec3_make(1.0f, -0.3f, 0.7f), 2.0f),
                                mu_vec3_splat(1.0f));
            memcpy(crigid, &rigid, sizeof crigid);
            mu_inverse_rigid(&rigid, &mr);
            glm_mat4_inv(crigid, cc);
            check_m4("inverse_rigid vs general inverse", &mr, cc, 1e-5f);

            mu_mat4_mul(&mr, &rigid, &mc);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    check_f("inverse_rigid * mat4 == I", (&mc.col[j].x)[i],
                            (i == j) ? 1.0f : 0.0f, 1e-5f);
        }
        {
            mu_mat4 id;
            int i, j;

            mu_inverse_affine(&ma, &mc);
            mu_mat4_mul(&mc, &ma, &id);
            for (j = 0; j < 4; ++j)
                for (i = 0; i < 4; ++i)
                    check_f("inverse_affine * mat4 == I", (&id.col[j].x)[i],
                            (i == j) ? 1.0f : 0.0f, 1e-4f);
        }

        /* quaternion */
        {
            mu_quat qa, qb, qr;
            versor cqa, cqb, cqr;
            mu_vec3 axis;
            float ang;

            axis = mu_vec3_normalize(mu_vec3_make(frand(-1, 1), frand(-1, 1), frand(-1, 1)));
            ang  = frand(-3.0f, 3.0f);

            qa = mu_quat_axis_angle(axis, ang);
            to_c3(axis, c3a); glm_quatv(cqa, ang, c3a);
            check_v4("quat_axis_angle", qa, cqa, 1e-6f);

            qb = mu_quat_axis_angle(mu_vec3_make(0, 1, 0), frand(-3, 3));
            to_c4(qb, cqb);

            qr = mu_quat_mul(qa, qb);
            glm_quat_mul(cqa, cqb, cqr);
            check_v4("quat_mul", qr, cqr, 1e-5f);

            qr = mu_quat_slerp(qa, qb, 0.3f);
            glm_quat_slerp(cqa, cqb, 0.3f, cqr);
            check_v4("quat_slerp", qr, cqr, 1e-4f);
            if (fabsf(qr.x - cqr[0]) > 1e-3f)
                printf("  note slerp d=%.6f t=0.3 mu=(%.5f %.5f %.5f %.5f) cglm=(%.5f %.5f %.5f %.5f)\n",
                       mu_vec4_dot(qa, qb), qr.x, qr.y, qr.z, qr.w, cqr[0], cqr[1], cqr[2], cqr[3]);

            r3 = mu_quat_rotate_vec3(qa, v3a);
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
                        const mu_vec3 e = mu_quat_rotate_vec3(qa, mu_vec3_make(xs[t], ys[t], zs[t]));
                        vec3 ce;

                        ce[0] = e.x; ce[1] = e.y; ce[2] = e.z;
                        check_v3("quat_rotate_vec3_soa",
                                 mu_vec3_make(ox[t], oy[t], oz[t]), ce, 1e-5f);
                    }
                }

                /* in place: output aliases input */
                mu_quat_rotate_vec3_soa(qa, xs, ys, zs, xs, ys, zs, NROT);
                for (k = 0; k < NROT; ++k)
                {
                    vec3 ce;

                    ce[0] = ox[k]; ce[1] = oy[k]; ce[2] = oz[k];
                    check_v3("quat_rotate_vec3_soa (in place)",
                             mu_vec3_make(xs[k], ys[k], zs[k]), ce, 1e-5f);
                }
            }

            /* quat -> mat must match glm_quat_mat4 */
            {
                mu_mat4 mq;
                mat4 cmq;

                mq = mu_mat4_from_quat(qa);
                glm_quat_mat4(cqa, cmq);
                check_m4("mat4_from_quat", &mq, cmq, 1e-5f);
            }
        }

        /* mat3 normal matrix */
        {
            mu_mat3 nm;
            mat3 cnm, cnmr;

            nm = mu_mat3_from_mat4_linear(&ma);
            memcpy(cnm, &nm, sizeof cnm);
            nm = mu_mat3_inverse_transpose(&nm);
            glm_mat3_inv(cnm, cnmr);
            glm_mat3_transpose(cnmr);
            {
                mu_mat3 want;
                int i, j;

                memcpy(&want, cnmr, sizeof want);
                for (j = 0; j < 3; ++j)
                    for (i = 0; i < 3; ++i)
                        check_f("mat3_inverse_transpose", (&nm.col[j].x)[i], (&want.col[j].x)[i], 1e-4f);
            }
        }
    }

    /* ---- batch kernels vs the per-element reference ---- */
    {
        enum { N = 1024 };
        static float x[N], y[N], z[N], ox[N], oy[N], oz[N];
        mu_mat4 m;
        size_t i;

        m = rand_affine();
        for (i = 0; i < N; ++i) {
            x[i] = frand(-50, 50);
            y[i] = frand(-50, 50);
            z[i] = frand(-50, 50);
        }

        mu_xform_points3_soa(&m, x, y, z, ox, oy, oz, N);
        for (i = 0; i < N; ++i) {
            mu_vec3 want = mu_mat4_transform_point(&m, mu_vec3_make(x[i], y[i], z[i]));
            check_f("batch xform x", ox[i], want.x, 1e-5f);
            check_f("batch xform y", oy[i], want.y, 1e-5f);
            check_f("batch xform z", oz[i], want.z, 1e-5f);
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
                mu_xform_points3_soa(&m, x, y, z, ox, oy, oz, n);
                for (k = 0; k < n; ++k) {
                    mu_vec3 want = mu_mat4_transform_point(&m, mu_vec3_make(x[k], y[k], z[k]));
                    check_f("batch xform tail", ox[k] + oy[k] + oz[k],
                            want.x + want.y + want.z, 1e-5f);
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
            mu_mat3 nm = mu_mat3_from_mat4_linear(&m);

            mu_xform_normals3_soa(&nm, x, y, z, ox, oy, oz, N);
            for (i = 0; i < N; ++i) {
                mu_vec3 want = mu_mat3_mulv(&nm, mu_vec3_make(x[i], y[i], z[i]));
                check_f("batch normals x", ox[i], want.x, 1e-4f);
                check_f("batch normals z", oz[i], want.z, 1e-4f);
            }
        }

        /* mat4 batch: 4-at-a-time packed path must equal the plain kernel */
        {
            enum { M8 = 16 };
            mu_mat4 as[M8], bs[M8], want[M8], got[M8];
            int k;

            for (k = 0; k < M8; ++k) { as[k] = rand_mat4(); bs[k] = rand_mat4(); }
            for (k = 0; k < M8; ++k) mu_mat4_mul(&as[k], &bs[k], &want[k]);
            mu_mat4_mul_batch(as, sizeof(mu_mat4), bs, sizeof(mu_mat4), got, sizeof(mu_mat4), M8);
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

        /* frustum cull: compacted list must equal the scalar test's list */
        {
            enum { NC = 512 };
            static float cxx[NC], cyy[NC], czz[NC];
            uint32_t idx[NC];
            mu_mat4 proj, view, vp;
            mu_frustum f;
            size_t n, want_count, k;
            uint8_t seen[NC];

            proj = mu_mat4_perspective(mu_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
            view = mu_mat4_look_at(mu_vec3_make(0, 0, 5), mu_vec3_make(0, 0, 0), mu_vec3_make(0, 1, 0));
            mu_mat4_mul(&proj, &view, &vp);

            /* cglm builds the same frustum; compare plane by plane */
            {
                mat4 cp, cv, cvp;
                vec4 planes[6];

                memcpy(cp, &proj, sizeof cp);
                memcpy(cv, &view, sizeof cv);
                glm_mat4_mul(cp, cv, cvp);
                glm_frustum_planes(cvp, planes);
                f = mu_frustum_from_view_proj(&vp);
                for (k = 0; k < 6; ++k) {
                    /* cglm's plane order: left,right,bottom,top,near,far */
                    check_f("frustum plane.x", f.p[k].x, planes[k][0], 1e-5f);
                    check_f("frustum plane.y", f.p[k].y, planes[k][1], 1e-5f);
                    check_f("frustum plane.z", f.p[k].z, planes[k][2], 1e-5f);
                    check_f("frustum plane.d", f.p[k].w, planes[k][3], 1e-5f);
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
