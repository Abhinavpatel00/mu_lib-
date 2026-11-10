/* bench_normals.c — is the cofactor normal transform worth having?
 *
 * The comparison is deliberately IN-LIBRARY: the baseline is our own
 * transpose(inverse(M)) path, not some strawman. If the cofactor version is not
 * faster and more accurate than the best inverse we already ship, it does not
 * belong in the header.
 *
 * Two levels, because the two paths are not the same amount of work:
 *   mat4  inverse(M) then transpose then multiply  vs  mu_mat4_normal
 *   mat3  inverse(A) then transpose then multiply  vs  mu_mat3_cofactor + mulv
 *
 * Written with mu_bench.h -- this file is also the dogfood test for the
 * harness: if the framework measures nothing, these two rows come out equal.
 */

#include "mu_bench.h"          /* first: sets _POSIX_C_SOURCE */
#include "mu_macros.h"
#include "mu_math.h"

#include <math.h>

#define N 64

static mu_mat4 g_m4[N];
static mu_mat3 g_m3[N];
static mu_vec3 g_n[N];
static float   g_acc;

/* A deterministic non-uniform, skewed, sometimes reflecting set: the matrices
 * that make this problem interesting. Fill is runtime (a static initializer
 * would be constant-folded and we would measure the optimiser). */
static void setup(void)
{
    unsigned x = 0x12345678u;
    int      i, j, k;

    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < 4; ++j)
            for (k = 0; k < 4; ++k)
            {
                float v;

                x = x * 1664525u + 1013904223u;
                v = ((float)(x >> 8) * (1.0f / 16777216.0f) - 0.5f) * 2.0f;
                g_m4[i][4 * j + 0 + k] = (j == 3) ? (float)(k == 3) : v;
            }
        /* non-uniform scale in the diagonal band, plus a reflection every 3rd */
        g_m4[i][4 * 0 + 0] += 1.5f;
        g_m4[i][4 * 1 + 1] += 0.75f;
        g_m4[i][4 * 2 + 2] += (i % 3 == 0) ? -2.0f : 2.0f;

        mu_vec3 v0;
        mu_vec3_make(v0, g_m4[i][4 * 0 + 0], g_m4[i][4 * 0 + 1], g_m4[i][4 * 0 + 2]);
        memcpy(g_m3[i] + 3 * 0, v0, sizeof(mu_vec3));
        mu_vec3 v1;
        mu_vec3_make(v1, g_m4[i][4 * 1 + 0], g_m4[i][4 * 1 + 1], g_m4[i][4 * 1 + 2]);
        memcpy(g_m3[i] + 3 * 1, v1, sizeof(mu_vec3));
        mu_vec3 v2;
        mu_vec3_make(v2, g_m4[i][4 * 2 + 0], g_m4[i][4 * 2 + 1], g_m4[i][4 * 2 + 2]);
        memcpy(g_m3[i] + 3 * 2, v2, sizeof(mu_vec3));

        x = x * 1664525u + 1013904223u;
        mu_vec3 v3;
        mu_vec3_make(v3, (float)(x >> 8) * (1.0f / 16777216.0f) - 0.5f, (float)((x >> 16) & 0xffffu) * (1.0f / 65536.0f) - 0.5f, 0.25f);
        mu_vec3 normalized;
        mu_vec3_normalize(normalized, v3);
        memcpy(g_n[i], normalized, sizeof(mu_vec3));
    }
}

/* ---- baselines ---------------------------------------------------------- */

MU_BENCH(normals, mat4_inverse_transpose)
{
    uint32_t it;
    float    acc = 0.0f;

    MU_BENCH_ITERATE(it)
    {
        const float *m = g_m4[it & (N - 1)];
        mu_mat4      inv;
        mu_vec3      n;
        mu_vec3      r;

        memcpy(n, g_n[it & (N - 1)], sizeof(mu_vec3));
        mu_mat4_inverse(m, inv);
        /* (M^-1)^T * n */
        mu_vec3_make(r, inv[4 * 0 + 0] * n[0] + inv[4 * 1 + 0] * n[1] + inv[4 * 2 + 0] * n[2], inv[4 * 0 + 1] * n[0] + inv[4 * 1 + 1] * n[1] + inv[4 * 2 + 1] * n[2], inv[4 * 0 + 2] * n[0] + inv[4 * 1 + 2] * n[1] + inv[4 * 2 + 2] * n[2]);
        acc += mu_vec3_length(r);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
    g_acc = acc;
}

MU_BENCH(normals, mat3_inverse_transpose)
{
    uint32_t it;
    float    acc = 0.0f;

    MU_BENCH_ITERATE(it)
    {
        const float *m = g_m3[it & (N - 1)];
        mu_mat3      inv_t;
        mu_vec3      r;

        mu_mat3_inverse_transpose(inv_t, m);
        mu_mat3_mulv(r, inv_t, g_n[it & (N - 1)]);
        acc += mu_vec3_length(r);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
    g_acc = acc;
}

/* ---- cofactor ----------------------------------------------------------- */

MU_BENCH(normals, mat4_cofactor)
{
    uint32_t it;
    float    acc = 0.0f;

    MU_BENCH_ITERATE(it)
    {
        mu_vec3 r;
        mu_mat4_normal(r, g_m4[it & (N - 1)], g_n[it & (N - 1)]);

        acc += mu_vec3_length(r);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
    g_acc = acc;
}

MU_BENCH(normals, mat3_cofactor)
{
    uint32_t it;
    float    acc = 0.0f;

    MU_BENCH_ITERATE(it)
    {
        mu_mat3 c;
        mu_vec3 r;

        mu_mat3_cofactor(c, g_m3[it & (N - 1)]);
        mu_mat3_mulv(r, c, g_n[it & (N - 1)]);

        acc += mu_vec3_length(r);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
    g_acc = acc;
}

int main(int argc, char **argv)
{
    setup();
    (void)argc;
    (void)argv;
    return mu_bench_main(argc, argv);
}
