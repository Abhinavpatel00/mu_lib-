/* bench_easing.c -- what an easing curve actually costs, per call and per batch
 *
 * The unit here is not nanoseconds: it is nanoseconds per eased value. Every row
 * evaluates a fixed payload of 64 values (or of 8 vectors, for the batch rows) so
 * the payload is in the row name and a number cannot drift away from the work it
 * measured. A single call to in_quad is 2 ns and the clock resolution is worse
 * than that.
 *
 * The point of the table is to answer "which of the eleven sections should I
 * use", which is a question about a factor of ten:
 *
 *   polynomial families   3 multiplies                    -> call the function
 *   sine / expo / circ    one transcendental / a sqrt     -> call the function
 *   elastic / bounce      exp2 + sin, four-way branch     -> bake a table
 *   dispatch by enum      one indirect-free switch        -> free, use it
 *   bezier / spline       an inversion / a search         -> bake a table
 *
 * and the honest losses:
 *
 *   - A LUT is SLOWER than a quadratic. Look at lut_eval vs out_quad: if the
 *     curve is cheap, the table costs more than it saves, and the docs say so.
 *   - Everything mu_easing does clamps, dispatches and guards its endpoints. The
 *     par comparison (bench_easing_vs_par.c) is where that overhead shows up.
 *
 *   make -C perf_test easing-bench
 *   ./bench_easing --filter=mu.lut --samples=21
 *   ./bench_easing --list
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"          /* first: sets _POSIX_C_SOURCE */

#include "mu_easing.h"

#include <math.h>
#include <string.h>

static volatile uint32_t g_sink;

/* ---- the payload ---------------------------------------------------------
 * 1024 values spread over [0,1], deterministic, and read with a stride so the
 * inner loop is not a constant the compiler can fold. */

#define PAYLOAD 1024
static float g_t[PAYLOAD];

static void payload_init(void)
{
    /* a cheap deterministic scramble: three xorshifts, no library */
    uint32_t s = 0x9e3779b9u ^ 0x85ebca6bu;
    int i;
    if (g_t[1] != 0.0f) return;
    for (i = 0; i < PAYLOAD; ++i)
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        g_t[i] = (float)(s & 0xffffff) / 16777215.0f;
    }
    g_t[0] = 0.0f;
    g_t[1] = 1.0f;
}

/* 64 evaluations starting at a rotating offset, so successive iterations differ */
#define X64(body)                                                              \
    do {                                                                       \
        uint32_t off = (i * 64u) & (PAYLOAD - 1);                              \
        int j;                                                                 \
        for (j = 0; j < 64; ++j)                                               \
        {                                                                      \
            float t = g_t[(off + (uint32_t)j * 7u) & (PAYLOAD - 1)];            \
            body                                                               \
        }                                                                      \
    } while (0)

/* ---------- one curve at a time ---------- */

MU_BENCH(mu, direct_out_quad_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_out_quad(t) * 1024.0f); });
    }
}

MU_BENCH(mu, direct_in_out_cubic_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_in_out_cubic(t) * 1024.0f); });
    }
}

MU_BENCH(mu, dispatch_in_out_cubic_x64)
{
    uint32_t i;
    mu_ease_fn fn = mu_ease_get(MU_EASE_IN_OUT_CUBIC);

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(fn(t) * 1024.0f); });
    }
}

MU_BENCH(mu, apply_in_out_cubic_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_apply(MU_EASE_IN_OUT_CUBIC, t) * 1024.0f); });
    }
}

MU_BENCH(mu, smoothstep_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_smoothstep(t) * 1024.0f); });
    }
}

MU_BENCH(mu, direct_out_sine_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_out_sine(t) * 1024.0f); });
    }
}

MU_BENCH(mu, direct_out_circ_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_out_circ(t) * 1024.0f); });
    }
}

MU_BENCH(mu, direct_out_expo_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_out_expo(t) * 1024.0f); });
    }
}

MU_BENCH(mu, direct_out_back_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_out_back(t) * 1024.0f); });
    }
}

/* the expensive one: exp2f and sinf in the same expression */
MU_BENCH(mu, direct_out_elastic_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_out_elastic(t) * 1024.0f); });
    }
}

/* the branchy one: four arcs, and which one you take is data dependent */
MU_BENCH(mu, direct_out_bounce_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_out_bounce(t) * 1024.0f); });
    }
}

/* ---------- velocity ---------- */

MU_BENCH(mu, velocity_out_cubic_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_velocity(MU_EASE_OUT_CUBIC, t) * 1024.0f); });
    }
}

MU_BENCH(mu, velocity_out_elastic_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_velocity(MU_EASE_OUT_ELASTIC, t) * 1024.0f); });
    }
}

/* ---------- baked tables ---------- */

static float g_table257[257];
static mu_ease_lut g_lut;

static mu_ease_lut *subject_lut(void)
{
    if (!g_lut.table) mu_ease_lut_bake(&g_lut, g_table257, 257, mu_ease_out_elastic);
    return &g_lut;
}

MU_BENCH(mu, lut_eval_x64)
{
    uint32_t i;
    mu_ease_lut *l = subject_lut();

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_lut_eval(l, t) * 1024.0f); });
    }
}

/* the same table, eight lanes at a time: 64 values as 8 vectors */
MU_BENCH(mu, lut_eval8_x512)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        uint32_t off = (i * 8u) & (PAYLOAD - 1);
        int j;
        for (j = 0; j < 64; j += 8)
        {
            mu_f32x8 v;
            int lane;
            for (lane = 0; lane < 8; ++lane) v.m[lane] = g_t[(off + (uint32_t)(j + lane) * 7u) & (PAYLOAD - 1)];
            v = mu_ease_lut_eval8(subject_lut(), v);
            for (lane = 0; lane < 8; ++lane) g_sink += (uint32_t)(v.m[lane] * 1024.0f);
        }
    }
}

/* the whole bake: 257 evaluations of the curve, once, at load time */
MU_BENCH(mu, lut_bake_257)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        g_sink += (uint32_t)(mu_ease_lut_bake(&g_lut, g_table257, 257, mu_ease_out_elastic) * 1u);
    }
}

/* ---------- bezier ---------- */

static mu_ease_bezier g_bez;

MU_BENCH(mu, bezier_css_ease_x64)
{
    uint32_t i;

    if (!(g_bez.x2 > 0.0f)) mu_ease_bezier_css("ease", &g_bez);

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_bezier_eval(&g_bez, t) * 1024.0f); });
    }
}

MU_BENCH(mu, bezier_velocity_x64)
{
    uint32_t i;

    if (!(g_bez.x2 > 0.0f)) mu_ease_bezier_css("ease", &g_bez);

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_bezier_velocity(&g_bez, t) * 1024.0f); });
    }
}

/* ---------- spline ---------- */

static mu_ease_key g_keys[8];
static float g_slopes[8];
static mu_ease_spline g_sp;

static mu_ease_spline *subject_spline(void)
{
    if (!g_sp.keys)
    {
        int k;
        static const float v[8] = { 0.0f, 0.42f, 0.66f, 0.78f, 0.86f, 0.93f, 0.97f, 1.0f };
        for (k = 0; k < 8; ++k) { g_keys[k].t = (float)k * (1.0f / 7.0f); g_keys[k].v = v[k]; }
        mu_ease_spline_build(&g_sp, g_keys, 8, g_slopes);
    }
    return &g_sp;
}

MU_BENCH(mu, spline_eval_x64)
{
    uint32_t i;
    mu_ease_spline *sp = subject_spline();

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_spline_eval(sp, t) * 1024.0f); });
    }
}

MU_BENCH(mu, spline_velocity_x64)
{
    uint32_t i;
    mu_ease_spline *sp = subject_spline();

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_spline_velocity(sp, t) * 1024.0f); });
    }
}

MU_BENCH(mu, spline_build_8keys)
{
    uint32_t i;

    subject_spline();

    MU_BENCH_ITERATE(i)
    {
        g_sink += (uint32_t)(mu_ease_spline_build(&g_sp, g_keys, 8, g_slopes) * 1u);
    }
}

/* ---------- steps, names, application ---------- */

MU_BENCH(mu, steps8_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_steps(t, 8, 0) * 1024.0f); });
    }
}

MU_BENCH(mu, from_name_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)mu_ease_from_name("in_out_bounce"); (void)t; });
    }
}

MU_BENCH(mu, lerp_out_cubic_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({ g_sink += (uint32_t)(mu_ease_lerp(MU_EASE_OUT_CUBIC, 3.0f, 9.0f, t) * 64.0f); });
    }
}

MU_BENCH(mu, vec3_vec3_x64)
{
    uint32_t i;
    mu_vec3 a = mu_vec3_make(0.0f, 1.0f, 2.0f);
    mu_vec3 b = mu_vec3_make(3.0f, 4.0f, 5.0f);

    MU_BENCH_ITERATE(i)
    {
        X64({
            mu_vec3 r = mu_ease_vec3(MU_EASE_OUT_CUBIC, a, b, t);
            g_sink += (uint32_t)((r.x + r.y + r.z) * 64.0f);
        });
    }
}

int main(int argc, char **argv)
{
    payload_init();
    return mu_bench_main(argc, argv);
}
