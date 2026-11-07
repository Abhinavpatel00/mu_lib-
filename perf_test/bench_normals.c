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
                (&g_m4[i].col[j].x)[k] = (j == 3) ? (float)(k == 3) : v;
            }
        /* non-uniform scale in the diagonal band, plus a reflection every 3rd */
        g_m4[i].col[0].x += 1.5f;
        g_m4[i].col[1].y += 0.75f;
        g_m4[i].col[2].z += (i % 3 == 0) ? -2.0f : 2.0f;

        g_m3[i].col[0] = mu_vec3_make(g_m4[i].col[0].x, g_m4[i].col[0].y, g_m4[i].col[0].z);
        g_m3[i].col[1] = mu_vec3_make(g_m4[i].col[1].x, g_m4[i].col[1].y, g_m4[i].col[1].z);
        g_m3[i].col[2] = mu_vec3_make(g_m4[i].col[2].x, g_m4[i].col[2].y, g_m4[i].col[2].z);

        x = x * 1664525u + 1013904223u;
        g_n[i] = mu_vec3_normalize(mu_vec3_make(
            (float)(x >> 8) * (1.0f / 16777216.0f) - 0.5f,
            (float)((x >> 16) & 0xffffu) * (1.0f / 65536.0f) - 0.5f,
            0.25f));
    }
}

/* ---- baselines ---------------------------------------------------------- */

MU_BENCH(normals, mat4_inverse_transpose)
{
    uint32_t it;
    float    acc = 0.0f;

    MU_BENCH_ITERATE(it)
    {
        const mu_mat4 *m = &g_m4[it & (N - 1)];
        mu_mat4        inv;
        mu_vec3        n = g_n[it & (N - 1)];
        mu_vec3        r;

        mu_mat4_inverse(m, &inv);
        /* (M^-1)^T * n */
        r = mu_vec3_make(inv.col[0].x * n.x + inv.col[1].x * n.y + inv.col[2].x * n.z,
                         inv.col[0].y * n.x + inv.col[1].y * n.y + inv.col[2].y * n.z,
                         inv.col[0].z * n.x + inv.col[1].z * n.y + inv.col[2].z * n.z);
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
        const mu_mat3 *m = &g_m3[it & (N - 1)];
        mu_mat3        invt = mu_mat3_inverse_transpose(m);
        mu_vec3        r = mu_mat3_mulv(&invt, g_n[it & (N - 1)]);

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
        mu_vec3 r = mu_mat4_normal(&g_m4[it & (N - 1)], g_n[it & (N - 1)]);

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
        mu_mat3 c = mu_mat3_cofactor(&g_m3[it & (N - 1)]);
        mu_vec3 r = mu_mat3_mulv(&c, g_n[it & (N - 1)]);

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
