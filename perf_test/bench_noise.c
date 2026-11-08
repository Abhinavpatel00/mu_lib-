/* bench_noise.c -- what each noise family costs, ns per sample.
 *
 * No baselines here except one: the fractal compositions are measured against
 * the base sampler they are built from, so the octave overhead is visible
 * rather than implied. Every row is the same clock in the same binary.
 *
 *   make -C perf_test noise-bench
 *   ./bench_noise --filter=fbm --samples=21 --confidence=6
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"          /* first: sets _POSIX_C_SOURCE */

#include "mu_noise_math.h"

static volatile float g_sink;
static volatile float g_sink2;

#define STEP 0.0137f

MU_BENCH(simplex, s1)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_simplex1((float)i * STEP, 7u);
}

MU_BENCH(simplex, s2)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_simplex2((float)i * STEP, (float)i * STEP * 0.7f, 7u);
}

MU_BENCH(simplex, s3)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_simplex3((float)i * STEP, (float)i * STEP * 0.7f, (float)i * STEP * 0.3f, 7u);
}

MU_BENCH(simplex, s4)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_simplex4((float)i * STEP, (float)i * STEP * 0.7f,
                                                    (float)i * STEP * 0.3f, (float)i * STEP * 0.11f, 7u);
}

MU_BENCH(simplex, s1_deriv)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) {
        mu_noise_sample2 s = mu_noise_simplex1_deriv((float)i * STEP, 7u);
        g_sink += s.value + s.gradient.x;
    }
}

MU_BENCH(simplex, s2_deriv)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) {
        mu_noise_sample2 s = mu_noise_simplex2_deriv((float)i * STEP, (float)i * STEP * 0.7f, 7u);
        g_sink += s.value + s.gradient.x;
    }
}

MU_BENCH(flow, flow2)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_flow2((float)i * STEP, (float)i * STEP * 0.7f, 0.9f, 7u);
}

MU_BENCH(flow, flow2_precomputed_trig)
{
    uint32_t i;
    const float s = 0.7833f, c = 0.6216f;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_flow2_sc((float)i * STEP, (float)i * STEP * 0.7f, s, c, 7u);
}

MU_BENCH(flow, flow3)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_flow3((float)i * STEP, (float)i * STEP * 0.7f,
                                                 (float)i * STEP * 0.3f, 0.9f, 7u);
}

MU_BENCH(fbm, base_simplex2_1oct)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_fbm2(mu_noise_simplex2, (float)i * STEP, (float)i * STEP * 0.7f, 7u, 1, 2.0f, 0.5f);
}

MU_BENCH(fbm, fbm2_5oct)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_fbm2(mu_noise_simplex2, (float)i * STEP, (float)i * STEP * 0.7f, 7u, 5, 2.0f, 0.5f);
}

MU_BENCH(fbm, dfbm2_5oct)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) {
        mu_noise_sample2 s = mu_noise_dfbm2(mu_noise_simplex2_deriv, (float)i * STEP, (float)i * STEP * 0.7f, 7u, 5, 2.0f, 0.5f);
        g_sink += s.value + s.gradient.x;
    }
}

MU_BENCH(fbm, iq_fbm2_5oct)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_iq_fbm2(mu_noise_simplex2_deriv, (float)i * STEP, (float)i * STEP * 0.7f, 7u, 5);
}

MU_BENCH(fbm, iq_rot_fbm2_5oct)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_iq_rot_fbm2(mu_noise_simplex2, (float)i * STEP, (float)i * STEP * 0.7f, 7u, 5, 2.0f, 0.6f, 0.5f);
}

MU_BENCH(fbm, fbm4_5oct)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_fbm4(mu_noise_simplex4, (float)i * STEP, (float)i * STEP * 0.7f,
                                                (float)i * STEP * 0.3f, (float)i * STEP * 0.11f, 7u, 5, 2.0f, 0.5f);
}

MU_BENCH(cellular, worley2_f1)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_worley2_f1((float)i * STEP, (float)i * STEP * 0.7f, 7u, MU_NOISE_DIST_EUCLIDEAN);
}

MU_BENCH(cellular, worley2_falloff)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_worley2_falloff((float)i * STEP, (float)i * STEP * 0.7f, 7u, 3.0f);
}

MU_BENCH(cellular, worley3_falloff)
{
    uint32_t i;
    MU_BENCH_ITERATE(i) g_sink += mu_noise_worley3_falloff((float)i * STEP, (float)i * STEP * 0.7f,
                                                           (float)i * STEP * 0.3f, 7u, 3.0f);
}

int main(int argc, char** argv)
{
    (void)g_sink2;
    return mu_bench_main(argc, argv);
}
