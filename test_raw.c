/*
 * test_raw_correctness.c — verify mu_math_raw.h produces correct results
 * by comparing against known-good values.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "mu/mu_math.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char* msg) {
    if (cond) { g_pass++; }
    else { g_fail++; printf("  FAIL: %s\n", msg); }
}

static int feq(float a, float b, float eps) {
    return fabsf(a-b) < eps;
}

int main(void) {
    printf("== mu_math_raw.h correctness tests ==\n\n");

    /* vec3 basic */
    {
        float a[3] = {1,2,3}, b[3] = {4,5,6}, r[3];
        mu_vec3_add(r, a, b);
        check(feq(r[0],5,1e-6) && feq(r[1],7,1e-6) && feq(r[2],9,1e-6), "vec3_add");

        float d = mu_vec3_dot(a, b);
        check(feq(d, 32.0f, 1e-6), "vec3_dot = 1*4+2*5+3*6 = 32");

        mu_vec3_cross(r, a, b);
        check(feq(r[0],-3,1e-6) && feq(r[1],6,1e-6) && feq(r[2],-3,1e-6), "vec3_cross");

        mu_vec3_normalize(r, a);
        float len = mu_vec3_length(r);
        check(feq(len, 1.0f, 1e-5), "normalize produces unit length");
    }

    /* vec4 basic */
    {
        float a[4] = {1,2,3,4}, b[4] = {5,6,7,8}, r[4];
        mu_vec4_add(r, a, b);
        check(feq(r[0],6,1e-6) && feq(r[1],8,1e-6) && feq(r[2],10,1e-6) && feq(r[3],12,1e-6), "vec4_add");

        float d = mu_vec4_dot(a, b);
        check(feq(d, 70.0f, 1e-6), "vec4_dot = 1*5+2*6+3*7+4*8 = 70");
    }

    /* mat4 identity * vec = vec */
    {
        float I[16], v[4] = {1,2,3,1}, r[4];
        mu_mat4_identity(I);
        mu_mat4_mulv(r, I, v);
        check(feq(r[0],1,1e-6) && feq(r[1],2,1e-6) && feq(r[2],3,1e-6) && feq(r[3],1,1e-6),
              "identity * v = v");
    }

    /* mat4 translate */
    {
        float T[16], v[4] = {1,2,3,1}, r[4], t[3] = {10,20,30};
        mu_mat4_translate(T, t);
        mu_mat4_mulv(r, T, v);
        check(feq(r[0],11,1e-6) && feq(r[1],22,1e-6) && feq(r[2],33,1e-6), "translate");
    }

    /* mat4 mul: I * A = A */
    {
        float I[16], A[16], R[16];
        mu_mat4_identity(I);
        mu_mat4_translate(A, (float[3]){5,10,15});
        mu_mat4_mul(I, A, R);
        check(feq(R[12],5,1e-6) && feq(R[13],10,1e-6) && feq(R[14],15,1e-6), "I * A = A");
    }

    /* mat4 inverse: A * A^-1 = I */
    {
        float A[16], Ainv[16], R[16];
        mu_mat4_trs(A, (float[3]){1,2,3}, (float[4]){0,0,0,1}, (float[3]){2,3,4});
        /* use a simple rotation around Z */
        float q[4];
        mu_quat_axis_angle(q, (float[3]){0,0,1}, 0.5f);
        float s[3] = {2,3,4};
        mu_mat4_trs(A, (float[3]){1,2,3}, q, s);
        mu_mat4_inverse(A, Ainv);
        mu_mat4_mul(A, Ainv, R);
        /* check diagonal ~= 1, off-diagonal ~= 0 */
        int ok = 1;
        for (int i = 0; i < 4 && ok; i++)
            for (int j = 0; j < 4 && ok; j++)
                if (i == j) { if (!feq(R[i*4+j], 1.0f, 1e-4)) ok = 0; }
                else { if (!feq(R[i*4+j], 0.0f, 1e-4)) ok = 0; }
        check(ok, "A * A^-1 = I");
    }

    /* quat: identity rotation doesn't change vector */
    {
        float q[4], v[3] = {1,2,3}, r[3];
        mu_quat_identity(q);
        mu_quat_rotate_vec3(r, q, v);
        check(feq(r[0],1,1e-6) && feq(r[1],2,1e-6) && feq(r[2],3,1e-6),
              "identity quat doesn't change vector");
    }

    /* quat mul: q * identity = q */
    {
        float a[4] = {0.5f, 0.5f, 0.5f, 0.5f}, id[4], r[4];
        mu_quat_identity(id);
        mu_quat_mul(r, a, id);
        check(feq(r[0],0.5f,1e-6) && feq(r[1],0.5f,1e-6) && feq(r[2],0.5f,1e-6) && feq(r[3],0.5f,1e-6),
              "q * identity = q");
    }

    /* mat3 inverse */
    {
        float A[9] = {1,0,0, 0,2,0, 0,0,3};
        float Ainv[9], R[9];
        mu_mat3_inverse(Ainv, A);
        mu_mat3_mul(R, A, Ainv);
        int ok = feq(R[0],1,1e-5) && feq(R[4],1,1e-5) && feq(R[8],1,1e-5)
              && feq(R[1],0,1e-5) && feq(R[3],0,1e-5) && feq(R[6],0,1e-5);
        check(ok, "mat3: A * A^-1 = I (diagonal)");
    }

    /* mat4 determinant */
    {
        float A[16];
        mu_mat4_identity(A);
        float det = mu_mat4_determinant(A);
        check(feq(det, 1.0f, 1e-6), "det(I) = 1");

        mu_mat4_scale(A, (float[3]){2,3,4});
        det = mu_mat4_determinant(A);
        check(feq(det, 24.0f, 1e-4), "det(scale(2,3,4)) = 24");
    }

    /* mat4 transpose */
    {
        float A[16] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
        float T[16];
        mu_mat4_transpose(A, T);
        check(feq(T[0],1,1e-6) && feq(T[1],5,1e-6) && feq(T[4],2,1e-6) && feq(T[5],6,1e-6),
              "transpose swaps indices");
    }

    /* rigid inverse */
    {
        float q[4], A[16], Ainv[16], R[16];
        mu_quat_axis_angle(q, (float[3]){0,1,0}, 1.0f);
        mu_mat4_trs(A, (float[3]){5,10,15}, q, (float[3]){1,1,1});
        mu_inverse_rigid(A, Ainv);
        mu_mat4_mul(A, Ainv, R);
        int ok = 1;
        for (int i = 0; i < 4 && ok; i++)
            for (int j = 0; j < 4 && ok; j++)
                if (i==j) { if (!feq(R[i*4+j],1,1e-4)) ok=0; }
                else { if (!feq(R[i*4+j],0,1e-4)) ok=0; }
        check(ok, "rigid inverse: A * A^-1 = I");
    }

    /* perspective matrix: not all zeros */
    {
        float P[16];
        mu_mat4_perspective(P, mu_radians(60.0f), 16.0f/9.0f, 0.1f, 100.0f);
        check(P[0] != 0 && P[5] != 0 && P[11] != 0, "perspective has non-zero entries");
    }

    /* look_at: eye at (0,0,5) looking at origin */
    {
        float V[16], p[4] = {0,0,0,1}, r[4];
        mu_mat4_look_at(V, (float[3]){0,0,5}, (float[3]){0,0,0}, (float[3]){0,1,0});
        mu_mat4_mulv(r, V, p);
        /* point at origin should be at (0, 0, -5) in view space */
        check(feq(r[0],0,1e-4) && feq(r[1],0,1e-4) && feq(r[2],-5,1e-4),
              "look_at: origin maps to (0,0,-5)");
    }

    /* SoA batch transform: identity matrix doesn't move points */
    {
        float I[16];
        mu_mat4_identity(I);
        float xin[4]={1,2,3,4}, yin[4]={5,6,7,8}, zin[4]={9,10,11,12};
        float xout[4], yout[4], zout[4];
        mu_xform_points3_soa(I, xin, yin, zin, xout, yout, zout, 4);
        check(feq(xout[0],1,1e-6) && feq(yout[0],5,1e-6) && feq(zout[0],9,1e-6),
              "SoA batch: identity preserves points");
    }

    /* frustum cull: point inside frustum is visible */
    {
        float VP[16], P[16], V[16];
        mu_mat4_perspective(P, mu_radians(60.0f), 1.0f, 0.1f, 100.0f);
        mu_mat4_look_at(V, (float[3]){0,0,10}, (float[3]){0,0,0}, (float[3]){0,1,0});
        mu_mat4_mul(P, V, VP);
        mu_frustum f;
        mu_frustum_from_view_proj(&f, VP);
        float center[3] = {0,0,0};
        int vis = mu_frustum_test_sphere(&f, center, 1.0f);
        check(vis == 1, "frustum: origin visible from z=10");

        float far_center[3] = {0,0,-200};
        vis = mu_frustum_test_sphere(&f, far_center, 1.0f);
        check(vis == 0, "frustum: far away point is culled");
    }

    /* sizes: verify layout matches shader expectations */
    {
        check(sizeof(mu_vec2) == 8,   "sizeof(mu_vec2) == 8");
        check(sizeof(mu_vec3) == 12,  "sizeof(mu_vec3) == 12");
        check(sizeof(mu_vec4) == 16,  "sizeof(mu_vec4) == 16");
        check(sizeof(mu_quat) == 16,  "sizeof(mu_quat) == 16");
        check(sizeof(mu_mat3) == 36,  "sizeof(mu_mat3) == 36");
        check(sizeof(mu_mat4) == 64,  "sizeof(mu_mat4) == 64");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
