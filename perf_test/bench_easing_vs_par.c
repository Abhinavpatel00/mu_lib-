/* bench_easing_vs_par.c -- mu_easing.h against par_easings.h, same 64-value payload
 *
 * par_easings.h is the header this one extends, so it is the only honest baseline.
 * Both are single-file and compiled into this one translation unit, and the two
 * suites use identical row names apart from the prefix so easing_vs_par.sh can
 * join them and report par/mu.
 *
 * What the comparison is and is not:
 *
 *   - It IS an apples-to-apples comparison of the 16 curves par implements. Same
 *     payload, same loop shape, same binary, interleaved runs.
 *   - It is NOT parity of behaviour, and where it differs par is wrong: par's
 *     in_out_back(0.5) is 1.5 and its in_out_elastic(1.0) is 0.5, and par clamps
 *     nothing, so par's out_elastic(2.0) is 1.000488 and the curve evaluates
 *     outside its own range. mu_easing does the clamping and forces the
 *     endpoints, and that work is in these numbers. The gate asserts all of it.
 *   - It is NOT the whole header. Bezier, spline, baked tables, steps, velocity
 *     and the batch path have no par equivalent; they are in bench_easing.c.
 *
 *   make -C perf_test easing-par
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h"

#include "mu_easing.h"

/* par_easings.h does not include <math.h> or <string.h> itself. */
#define PAR_EASINGS_IMPLEMENTATION 1
#include "par_easings.h"

#include <math.h>
#include <string.h>

static volatile uint32_t g_sink;

#define PAYLOAD 1024
static float g_t[PAYLOAD];

static void payload_init(void)
{
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

/* One macro pair per shared curve. Writing it out 16 times would be 300 lines of
 * copy-paste; the two definitions below are identical except for the call. */
#define ROW_PAIR(name, par_fn, mu_fn)                                          \
    MU_BENCH(par, name)                                                        \
    {                                                                          \
        uint32_t i;                                                            \
        MU_BENCH_ITERATE(i)                                                    \
        {                                                                      \
            X64({ g_sink += (uint32_t)(par_fn(t) * 1024.0f); });                \
        }                                                                      \
    }                                                                          \
    MU_BENCH(mu, name)                                                         \
    {                                                                          \
        uint32_t i;                                                            \
        MU_BENCH_ITERATE(i)                                                    \
        {                                                                      \
            X64({ g_sink += (uint32_t)(mu_fn(t) * 1024.0f); });                 \
        }                                                                      \
    }

ROW_PAIR(linear_x64,      par_easings_linear,      mu_ease_linear)
ROW_PAIR(in_quad_x64,     par_easings_in_quad,     mu_ease_in_quad)
ROW_PAIR(out_quad_x64,    par_easings_out_quad,    mu_ease_out_quad)
ROW_PAIR(in_out_quad_x64, par_easings_in_out_quad, mu_ease_in_out_quad)
ROW_PAIR(in_cubic_x64,    par_easings_in_cubic,    mu_ease_in_cubic)
ROW_PAIR(out_cubic_x64,   par_easings_out_cubic,   mu_ease_out_cubic)
ROW_PAIR(in_out_cubic_x64, par_easings_in_out_cubic, mu_ease_in_out_cubic)
ROW_PAIR(in_back_x64,     par_easings_in_back,     mu_ease_in_back)
ROW_PAIR(out_back_x64,    par_easings_out_back,    mu_ease_out_back)
ROW_PAIR(in_out_back_x64, par_easings_in_out_back, mu_ease_in_out_back)
ROW_PAIR(in_elastic_x64,  par_easings_in_elastic,  mu_ease_in_elastic)
ROW_PAIR(out_elastic_x64, par_easings_out_elastic, mu_ease_out_elastic)
ROW_PAIR(in_out_elastic_x64, par_easings_in_out_elastic, mu_ease_in_out_elastic)
ROW_PAIR(in_bounce_x64,   par_easings_in_bounce,   mu_ease_in_bounce)
ROW_PAIR(out_bounce_x64,  par_easings_out_bounce,  mu_ease_out_bounce)
ROW_PAIR(in_out_bounce_x64, par_easings_in_out_bounce, mu_ease_in_out_bounce)

/* The one extra row worth measuring: what the clamp and the endpoint guard cost on
 * a curve whose body is a single multiply. If the answer were "a lot", the guard
 * would be worth removing and the out-of-range behaviour worth losing. */
MU_BENCH(mu, clamp_overhead_out_quad_x64)
{
    uint32_t i;

    MU_BENCH_ITERATE(i)
    {
        X64({
            float u = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            g_sink += (uint32_t)((u * (2.0f - u)) * 1024.0f);
        });
    }
}

int main(int argc, char **argv)
{
    payload_init();
    return mu_bench_main(argc, argv);
}
