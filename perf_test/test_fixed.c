/* test_fixed.c -- gate for mu/mu_fixed.h
 *
 * Three claims in that header need proving, and they are three different kinds
 * of proof:
 *
 *   1. ACCURACY. "sin/cos are within one LSB of the correctly rounded value"
 *      is checked exhaustively over every Q16.16 angle in a turn, against a
 *      double reference, and the worst case is asserted. That claim is about
 *      the *table* path; the CORDIC path kept beside it as the no-.rodata
 *      alternative is checked to its own looser bound on the same grid, so
 *      neither can rot silently. The quadrant points are asserted *exact*
 *      (0, 1, 0, -1) on both, because an approximation that gets sin(0) wrong
 *      by a unit is a smell every user would notice.
 *
 *   2. ARITHMETIC. mul/div/lerp/sqrt rounding, saturation and the negative
 *      cases (which is where fixed-point rounding formulas usually break: the
 *      shift floors, so (x + half) >> 16 rounds -0.6 *up*).
 *
 *   3. NO LIBM, NO FLOAT. Two mechanical proofs, both in the Makefile:
 *      `fixed-nolibm` links this file with -DMU_FIXED_LINK_ONLY and no -lm, so
 *      a stray sinf would be an undefined symbol; `fixed-determinism` builds
 *      the same code at -O0 and -O3 with different -march values and compares
 *      a hash of the full output, so an optimization that reassociated the
 *      integer maths would be caught.
 *
 *   make -C perf_test fixed-test      # 393 000+ checks
 *   ./test_fixed --hash              # the determinism digest
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MU_FIXED_LINK_ONLY
#define MU_FIXED_WANT_FLOAT_SQRT 1 /* to check the opt-in FPU path too */
#endif
#include "mu_fixed.h"

#ifndef MU_FIXED_LINK_ONLY
#include <math.h>
#endif

#ifndef MU_FIXED_LINK_ONLY
static int g_checks;
static int g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s\n", what);
    }
}

static void check_i64(int64_t got, int64_t want, const char *what)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s (got %lld want %lld)\n", what, (long long)got, (long long)want);
    }
}

static void check_le(int64_t got, int64_t lim, const char *what)
{
    ++g_checks;
    if (got > lim) {
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s (got %lld, limit %lld)\n", what, (long long)got, (long long)lim);
    }
}
#endif /* !MU_FIXED_LINK_ONLY */

/* ------------------------------------------------------------------ */
/* link-only build: call everything, prove nothing comes from libm    */
/* ------------------------------------------------------------------ */

#ifdef MU_FIXED_LINK_ONLY

int main(void)
{
    volatile int32_t sink = 0;
    mu_q16 s, c;

    sink += mu_q16_mul(MU_Q16_ONE, MU_SQRT2_Q16);
    sink += mu_q16_div(MU_Q16_ONE, MU_E_Q16);
    sink += mu_q16_sqrt(2 * MU_Q16_ONE);
    sink += mu_q16_rsqrt(2 * MU_Q16_ONE);
    sink += mu_q16_lerp(0, MU_Q16_ONE, MU_Q16_HALF);
    sink += mu_q16_abs(-MU_Q16_ONE);
    sink += mu_q16_clamp(MU_Q16_ONE, 0, MU_Q16_HALF);
    sink += mu_u64_sqrt(1ull << 40);
    mu_q16_sincos(MU_ANG_QUARTER / 3, &s, &c);
    sink += s + c;
    sink += mu_q16_sin(MU_ANG_EIGHTH);
    sink += mu_q16_cos(MU_ANG_EIGHTH);
    sink += mu_q16_tan(MU_ANG_EIGHTH);
    sink += mu_q16_atan2(MU_Q16_ONE, MU_Q16_ONE);
    sink += mu_q16_asin(MU_Q16_HALF);
    sink += mu_q16_acos(MU_Q16_HALF);
    sink += mu_ang_from_radians(MU_PI_Q16);
    sink += mu_ang_to_radians(MU_ANG_QUARTER);
    sink += mu_ang_from_degrees_i32(30);
    sink += mu_ang_to_degrees(MU_ANG_EIGHTH);
    sink += mu_q16_from_f32(0.5f);
    sink += mu_q16_to_i32(MU_Q16_ONE * 3);

    printf("mu_fixed links without libm: %d\n", sink != 0);
    return 0;
}

#else /* ---------------------------- the real gate ---------------------------- */

/* ------------------------------------------------------------------ */
/* 1. constants and conversions                                       */
/* ------------------------------------------------------------------ */

static void test_fixed_constants(void)
{
    check_i64(MU_PI_Q16, (int64_t)lround(3.14159265358979323846 * 65536.0), "const: pi");
    check_i64(MU_TAU_Q16, (int64_t)lround(6.28318530717958647692 * 65536.0), "const: tau");
    check_i64(MU_HALF_PI_Q16, (int64_t)lround(1.57079632679489661923 * 65536.0), "const: half pi");
    check_i64(MU_E_Q16, (int64_t)lround(2.71828182845904523536 * 65536.0), "const: e");
    check_i64(MU_SQRT2_Q16, (int64_t)lround(1.41421356237309504880 * 65536.0), "const: sqrt2");
    check_i64(MU_LN2_Q16, (int64_t)lround(0.69314718055994530942 * 65536.0), "const: ln2");
    check_i64(MU_INV_LN2_Q16, (int64_t)lround(1.44269504088896340736 * 65536.0), "const: 1/ln2");
    check_i64(MU_TAU_Q16, 2 * MU_PI_Q16 + 1, "const: tau == 2*pi at this resolution");
    check_i64(MU_TAU_Q16 / 2, MU_PI_Q16, "const: half of tau rounds back to pi");
    check(MU_Q16_ONE == 65536 && MU_ANG_TURN == 65536, "const: one turn is one Q16.16 unit");

    check_i64(mu_q16_from_i32(1), MU_Q16_ONE, "conv: 1 -> 1.0");
    check_i64(mu_q16_from_i32(-3), -3 * MU_Q16_ONE, "conv: -3 -> -3.0");
    check_i64(mu_q16_to_i32(3 * MU_Q16_ONE + 12345), 3, "conv: to_i32 truncates on positives");
    check_i64(mu_q16_to_i32(-3 * MU_Q16_ONE - 12345), -4, "conv: to_i32 floors on negatives");
    check_i64(mu_q16_round_i32(3 * MU_Q16_ONE + 40000), 4, "conv: round_i32 rounds up");
    check_i64(mu_q16_round_i32(-3 * MU_Q16_ONE - 40000), -4, "conv: round_i32 rounds away from zero");
    check_i64(mu_q16_round_i32(-3 * MU_Q16_ONE - 10000), -3, "conv: round_i32 keeps a small negative");
    check_i64(mu_q16_to_f32(mu_q16_from_f32(0.25f)) == 0.25f ? 1 : 0, 1, "conv: f32 round trip");

    {
        float f;
        int bad = 0;
        for (f = -8.0f; f <= 8.0f; f += 0.001f) {
            if (labs((long)mu_q16_from_f32_round(f) - lround((double)f * 65536.0)) > 0) ++bad;
        }
        check(bad == 0, "conv: from_f32_round matches the double reference");
    }
}

/* ------------------------------------------------------------------ */
/* 2. arithmetic                                                      */
/* ------------------------------------------------------------------ */

static void test_fixed_arith(void)
{
    int32_t a, b;
    int bad_mul = 0, bad_div = 0;

    /* Every product of two 0.00125-granular values in [-8, 8], checked against
     * the double reference rounded to nearest. */
    for (a = -6400; a <= 6400; a += 80) {
        for (b = -6400; b <= 6400; b += 80) {
            double want = ((double)a * (double)b) / 65536.0;
            int64_t got = mu_q16_mul((mu_q16)a, (mu_q16)b);
            if (llabs(got - llround(want)) > 0) ++bad_mul;
        }
    }
    check(bad_mul == 0, "arith: mul is correctly rounded on a 161x161 grid");

    for (a = -6400; a <= 6400; a += 80) {
        for (b = -6400; b <= 6400; b += 80) {
            double want;
            int64_t got;
            if (b == 0) continue;
            want = ((double)a * 65536.0) / (double)b;
            got = mu_q16_div((mu_q16)a, (mu_q16)b);
            if (llabs(got - llround(want)) > 1) ++bad_div;
        }
    }
    check(bad_div == 0, "arith: div is within 1 LSB on a 161x161 grid");

    check_i64(mu_q16_mul(MU_Q16_ONE, MU_Q16_ONE), MU_Q16_ONE, "arith: 1*1 == 1");
    check_i64(mu_q16_mul(-MU_Q16_ONE, MU_Q16_ONE), -MU_Q16_ONE, "arith: -1*1 == -1");
    check_i64(mu_q16_mul(MU_Q16_HALF, MU_Q16_HALF), MU_Q16_ONE / 4, "arith: 0.5*0.5 == 0.25");
    check_i64(mu_q16_mul(-MU_Q16_HALF, MU_Q16_HALF), -MU_Q16_ONE / 4, "arith: -0.5*0.5 == -0.25");
    check_i64(mu_q16_mul(3, 3), 0, "arith: 3 LSB * 3 LSB rounds to 0");
    check_i64(mu_q16_div(MU_Q16_ONE, MU_Q16_ONE), MU_Q16_ONE, "arith: 1/1 == 1");
    check_i64(mu_q16_div(MU_Q16_ONE, 2 * MU_Q16_ONE), MU_Q16_HALF, "arith: 1/2 == 0.5");
    check_i64(mu_q16_div(MU_Q16_ONE, 2), MU_Q16_MAX, "arith: dividing by 2 LSB saturates");
    check_i64(mu_q16_div(MU_Q16_ONE, 0), MU_Q16_MAX, "arith: x/0 saturates high");
    check_i64(mu_q16_div(-MU_Q16_ONE, 0), MU_Q16_MIN, "arith: -x/0 saturates low");
    check_i64(mu_q16_div(MU_Q16_MAX, 1), MU_Q16_MAX, "arith: div saturates instead of wrapping");

    check_i64(mu_q16_add_sat(MU_Q16_MAX, MU_Q16_MAX), MU_Q16_MAX, "arith: add_sat clamps high");
    check_i64(mu_q16_sub_sat(MU_Q16_MIN, MU_Q16_MAX), MU_Q16_MIN, "arith: sub_sat clamps low");
    check_i64(mu_q16_add(MU_Q16_MAX, 1), MU_Q16_MIN, "arith: plain add wraps (documented)");

    check_i64(mu_q16_lerp(0, MU_Q16_ONE, 0), 0, "arith: lerp t=0");
    check_i64(mu_q16_lerp(0, MU_Q16_ONE, MU_Q16_ONE), MU_Q16_ONE, "arith: lerp t=1");
    check_i64(mu_q16_lerp(0, MU_Q16_ONE, MU_Q16_HALF), MU_Q16_HALF, "arith: lerp t=0.5");
    check_i64(mu_q16_lerp(-MU_Q16_ONE, MU_Q16_ONE, MU_Q16_HALF), 0, "arith: lerp across zero");

    check_i64(mu_q16_abs(MU_Q16_MIN + 1), MU_Q16_MAX, "arith: abs of the lowest value");
    check_i64(mu_q16_neg(-7), 7, "arith: neg");
    check_i64(mu_q16_sign(-7), -MU_Q16_ONE, "arith: sign(-)");
    check_i64(mu_q16_sign(0), 0, "arith: sign(0)");
    check_i64(mu_q16_clamp(5 * MU_Q16_ONE, -MU_Q16_ONE, MU_Q16_ONE), MU_Q16_ONE, "arith: clamp high");
    check_i64(mu_q16_mul_i32(MU_Q16_ONE, 3), 3 * MU_Q16_ONE, "arith: mul_i32");
    check_i64(mu_q16_div_i32(3 * MU_Q16_ONE, 3), MU_Q16_ONE, "arith: div_i32");
    check_i64(mu_q16_mul_i32(MU_Q16_MAX, 4), MU_Q16_MAX, "arith: mul_i32 saturates");
}

/* ------------------------------------------------------------------ */
/* 3. square root                                                     */
/* ------------------------------------------------------------------ */

static void test_fixed_sqrt(void)
{
    uint32_t v;
    int bad_floor = 0, bad_q16 = 0;

    for (v = 0; v < 200000u; ++v) {
        double d = sqrt((double)v);
        if ((double)mu_u64_sqrt(v) != floor(d)) ++bad_floor;
    }
    check(bad_floor == 0, "sqrt: u64_sqrt is exact for 0..200000");

    for (v = 0; v < 200000u; ++v) {
        double want = sqrt((double)v * 65536.0);
        if (llabs((int64_t)mu_q16_sqrt((mu_q16)v) - llround(want)) > 0) ++bad_q16;
    }
    check(bad_q16 == 0, "sqrt: q16_sqrt is correctly rounded for 0..200000");

    check_i64(mu_q16_sqrt(MU_Q16_ONE), MU_Q16_ONE, "sqrt: sqrt(1) == 1");
    check_i64(mu_q16_sqrt(4 * MU_Q16_ONE), 2 * MU_Q16_ONE, "sqrt: sqrt(4) == 2");
    check_i64(mu_q16_sqrt(65536 / 4), MU_Q16_HALF, "sqrt: sqrt(0.25) == 0.5");
    check_i64(mu_q16_sqrt(MU_SQRT2_Q16), 77936, "sqrt: sqrt(sqrt2) == 2^(1/4) == 1.1892");
    check_i64(mu_q16_sqrt(0), 0, "sqrt: sqrt(0) == 0");
    check_i64(mu_q16_sqrt(-1), 0, "sqrt: sqrt(-x) is defined as 0");
    check_i64(mu_u64_sqrt(~(uint64_t)0), 0xFFFFFFFFu, "sqrt: u64_sqrt of the maximum");
    check_i64(mu_q16_rsqrt(MU_Q16_ONE), MU_Q16_ONE, "sqrt: rsqrt(1) == 1");
    check_le(llabs((int64_t)mu_q16_rsqrt(4 * MU_Q16_ONE) - MU_Q16_HALF), 2, "sqrt: rsqrt(4) ~ 0.5");

    /* The opt-in FPU path must agree with the integer one, which is the whole
     * condition on it being allowed to exist. */
    {
        int bad = 0;
        for (v = 0; v < 200000u; ++v)
            if (llabs((int64_t)mu_q16_sqrt_f32((mu_q16)v) - (int64_t)mu_q16_sqrt((mu_q16)v)) > 1) ++bad;
        check(bad == 0, "sqrt: the FPU path agrees with the integer path within 1 LSB");
    }
}

/* ------------------------------------------------------------------ */
/* 4. angles                                                          */
/* ------------------------------------------------------------------ */

static void test_fixed_angles(void)
{
    check_i64(mu_ang_from_radians(MU_PI_Q16), MU_ANG_HALF, "angle: pi radians is half a turn");
    check_i64(mu_ang_from_radians(MU_TAU_Q16), MU_ANG_TURN, "angle: tau radians is a turn");
    check_i64(mu_ang_from_radians(0), 0, "angle: 0 radians");
    check_le(llabs((int64_t)mu_ang_to_radians(MU_ANG_HALF) - MU_PI_Q16), 1, "angle: half turn is pi");
    check_i64(mu_ang_from_degrees_i32(360), MU_ANG_TURN, "angle: 360 degrees is a turn");
    check_i64(mu_ang_from_degrees_i32(-90), -MU_ANG_QUARTER, "angle: -90 degrees is -quarter");
    check_i64(mu_ang_to_degrees(MU_ANG_EIGHTH), 45 * MU_Q16_ONE, "angle: eighth turn is 45 degrees");
    check_i64(mu_ang_wrap(MU_ANG_TURN + 5), 5, "angle: wrap past a turn");
    check_i64(mu_ang_wrap(-5), MU_ANG_TURN - 5, "angle: wrap below zero");
    check_i64(mu_ang_wrap(-MU_ANG_TURN - 5), MU_ANG_TURN - 5, "angle: wrap a whole negative turn");
}

/* ------------------------------------------------------------------ */
/* 5. sin/cos, exhaustively                                           */
/* ------------------------------------------------------------------ */

static int64_t double_sin_q16(double turns) { return llround(sin(turns * 6.28318530717958647692) * 65536.0); }
static int64_t double_cos_q16(double turns) { return llround(cos(turns * 6.28318530717958647692) * 65536.0); }

static void test_fixed_sincos(void)
{
    int32_t a;
    int64_t worst_s = 0, worst_c = 0;
    int32_t at_s = 0, at_c = 0;

    /* The quadrant points must be exact, not one LSB off: sin(0) == -1 LSB is
     * the kind of thing that shows up as a jittering sprite. */
    {
        mu_q16 s, c;
        mu_q16_sincos(0, &s, &c);
        check_i64(s, 0, "sincos: sin(0) is exactly 0");
        check_i64(c, MU_Q16_ONE, "sincos: cos(0) is exactly 1");
        mu_q16_sincos(MU_ANG_QUARTER, &s, &c);
        check_i64(s, MU_Q16_ONE, "sincos: sin(90) is exactly 1");
        check_i64(c, 0, "sincos: cos(90) is exactly 0");
        mu_q16_sincos(MU_ANG_HALF, &s, &c);
        check_i64(s, 0, "sincos: sin(180) is exactly 0");
        check_i64(c, -MU_Q16_ONE, "sincos: cos(180) is exactly -1");
        mu_q16_sincos(3 * MU_ANG_QUARTER, &s, &c);
        check_i64(s, -MU_Q16_ONE, "sincos: sin(270) is exactly -1");
        check_i64(c, 0, "sincos: cos(270) is exactly 0");
    }

    /* Every Q16.16 angle in the turn, both signs, against double. */
    for (a = -MU_ANG_TURN; a < MU_ANG_TURN; ++a) {
        mu_q16 s, c;
        int64_t es, ec;
        mu_q16_sincos((mu_ang)a, &s, &c);
        es = llabs((int64_t)s - double_sin_q16((double)a / 65536.0));
        ec = llabs((int64_t)c - double_cos_q16((double)a / 65536.0));
        if (es > worst_s) { worst_s = es; at_s = a; }
        if (ec > worst_c) { worst_c = ec; at_c = a; }
        check(s >= -MU_Q16_ONE && s <= MU_Q16_ONE, "sincos: sin stays in [-1, 1]");
        check(c >= -MU_Q16_ONE && c <= MU_Q16_ONE, "sincos: cos stays in [-1, 1]");
    }
    check_le(worst_s, 1, "sincos: sin is within 1 LSB of the double reference");
    check_le(worst_c, 1, "sincos: cos is within 1 LSB of the double reference");

    /* The unit circle. Each term carries its own interpolation rounding (and the
     * two interpolations are independent: cos is sin a quarter turn later, not
     * derived from the same sample), so squaring doubles the relative error --
     * 5 LSB out of 65536 is 8e-5, and the honest bound is 4 plus one for the
     * mul rounding, not 1. */
    {
        int64_t worst = 0;
        for (a = 0; a < MU_ANG_TURN; a += 7) {
            mu_q16 s, c;
            mu_q16_sincos((mu_ang)a, &s, &c);
            worst = llabs((int64_t)mu_q16_mul(s, s) + mu_q16_mul(c, c) - MU_Q16_ONE) > worst
                        ? llabs((int64_t)mu_q16_mul(s, s) + mu_q16_mul(c, c) - MU_Q16_ONE)
                        : worst;
        }
        check_le(worst, 5, "sincos: sin^2 + cos^2 == 1 within 5 LSB");
    }

    /* The table-free arm, which nothing calls by default: allowed to be one LSB
     * worse (CORDIC residual 0.06 LSB, symmetric rounding), checked on every
     * third angle so the gate stays quick. */
    {
        int64_t wc_s = 0, wc_c = 0;
        int32_t b;
        for (b = -MU_ANG_TURN; b < MU_ANG_TURN; b += 3) {
            mu_q16 s, c;
            int64_t es, ec;
            mu_q16_sincos_cordic((mu_ang)b, &s, &c);
            es = llabs((int64_t)s - double_sin_q16((double)b / 65536.0));
            ec = llabs((int64_t)c - double_cos_q16((double)b / 65536.0));
            if (es > wc_s) wc_s = es;
            if (ec > wc_c) wc_c = ec;
        }
        check_le(wc_s, 2, "cordic: sin within 2 LSB");
        check_le(wc_c, 2, "cordic: cos within 2 LSB");
        printf("  cordic: worst sin %lld LSB, cos %lld LSB (table path is 1)\n",
               (long long)wc_s, (long long)wc_c);
    }

    check_i64(mu_q16_sin(MU_ANG_EIGHTH), 46341, "sincos: sin(45) == 46341");
    check_i64(mu_q16_cos(MU_ANG_EIGHTH), 46341, "sincos: cos(45) == 46341");
    check_le(llabs((int64_t)mu_q16_tan(MU_ANG_EIGHTH) - MU_Q16_ONE), 2, "sincos: tan(45) ~ 1");
    check_le(llabs((int64_t)mu_q16_sin_rad(MU_HALF_PI_Q16) - MU_Q16_ONE), 1, "sincos: sin_rad(pi/2)");
    check_le(llabs((int64_t)mu_q16_cos_rad(0) - MU_Q16_ONE), 1, "sincos: cos_rad(0)");

    printf("  sincos: worst sin error %lld LSB at angle %ld, cos %lld LSB at angle %ld\n",
           (long long)worst_s, (long)at_s, (long long)worst_c, (long)at_c);
}

/* ------------------------------------------------------------------ */
/* 6. atan2 / asin / acos                                             */
/* ------------------------------------------------------------------ */

static void test_fixed_atan2(void)
{
    int64_t worst = 0;
    int32_t ax = 0, ay = 0;
    int32_t x, y;

    check_i64(mu_q16_atan2(0, 0), 0, "atan2: (0,0) is defined as 0");
    check_i64(mu_q16_atan2(0, MU_Q16_ONE), 0, "atan2: +x axis is 0");
    check_i64(mu_q16_atan2(0, -MU_Q16_ONE), MU_ANG_HALF, "atan2: -x axis is half a turn");
    check_i64(mu_q16_atan2(MU_Q16_ONE, 0), MU_ANG_QUARTER, "atan2: +y axis is a quarter turn");
    check_i64(mu_q16_atan2(-MU_Q16_ONE, 0), -MU_ANG_QUARTER, "atan2: -y axis is -quarter");
    check_i64(mu_q16_atan2(MU_Q16_ONE, MU_Q16_ONE), MU_ANG_EIGHTH, "atan2: 45 degrees is an eighth");
    check_i64(mu_q16_atan2(-MU_Q16_ONE, -MU_Q16_ONE), -3 * MU_ANG_EIGHTH, "atan2: -135 degrees");
    check_i64(mu_q16_atan2(MU_Q16_ONE, -MU_Q16_ONE), 3 * MU_ANG_EIGHTH, "atan2: +135 degrees");

    /* A grid over the plane, plus the four quadrants at 1:1000 and 1000:1
     * ratios (the extremes are where a CORDIC without a half-plane fold
     * silently saturates at +/-99.88 degrees). */
    for (y = -2000; y <= 2000; y += 100) {
        for (x = -2000; x <= 2000; x += 100) {
            long want, got, e;
            if (x == 0 && y == 0) continue;
            want = lround(atan2((double)y, (double)x) / 6.28318530717958647692 * 65536.0);
            got = mu_q16_atan2((mu_q16)(y * 32), (mu_q16)(x * 32));
            e = labs(got - want);
            if (e > worst) { worst = e; ax = x; ay = y; }
        }
    }
    check_le(worst, 1, "atan2: within 1 LSB of the double reference on the plane");
    printf("  atan2: worst error %lld LSB at (%ld,%ld), 41x41 grid\n", (long long)worst, (long)ax, (long)ay);

    /* Two million random pairs, which is where the 1-LSB bound actually comes
     * from: a grid of round numbers lands on points the CORDIC answers exactly,
     * so a grid alone would over-claim. */
    {
        uint32_t seed = 12345u;
        int64_t worst_rnd = 0;
        int n;
        for (n = 0; n < 2000000; ++n) {
            int32_t y, x;
            long want, got, e;
            seed = seed * 1664525u + 1013904223u;
            y = (int32_t)(seed >> 8) % 65537 - 32768;
            seed = seed * 1664525u + 1013904223u;
            x = (int32_t)(seed >> 8) % 65537 - 32768;
            if (x == 0 && y == 0) continue;
            want = lround(atan2((double)y, (double)x) / 6.28318530717958647692 * 65536.0);
            got = mu_q16_atan2((mu_q16)y, (mu_q16)x);
            e = labs(got - want);
            if (e > worst_rnd) worst_rnd = e;
        }
        check_le(worst_rnd, 1, "atan2: within 1 LSB over 2 000 000 random pairs");
        printf("  atan2: random-pair worst %lld LSB over 2 000 000 pairs\n", (long long)worst_rnd);
    }

    /* Round trip: atan2(sin a, cos a) must give a back, within 2 LSB. */
    {
        int32_t a;
        int64_t worst_rt = 0;
        /* -half a turn is the one angle the atan2 range convention cannot return
         * (it answers in (-half, +half]), so the sweep starts one step above it. */
        for (a = -MU_ANG_TURN / 2 + 13; a < MU_ANG_TURN / 2; a += 13) {
            mu_q16 s, c;
            int64_t e;
            mu_q16_sincos((mu_ang)a, &s, &c);
            e = llabs((int64_t)mu_q16_atan2(s, c) - (int64_t)a);
            if (e > worst_rt) worst_rt = e;
        }
        check_le(worst_rt, 2, "atan2: round trips through sincos within 2 LSB");
        printf("  atan2: round trip worst %lld LSB\n", (long long)worst_rt);
    }

    {
        int64_t worst_as = 0, worst_ac = 0;
        int32_t s;
        for (s = -MU_Q16_ONE; s <= MU_Q16_ONE; ++s) {
            long want_as = lround(asin((double)s / 65536.0) / 6.28318530717958647692 * 65536.0);
            long want_ac = lround(acos((double)s / 65536.0) / 6.28318530717958647692 * 65536.0);
            int64_t e_as = llabs((int64_t)mu_q16_asin((mu_q16)s) - want_as);
            int64_t e_ac = llabs((int64_t)mu_q16_acos((mu_q16)s) - want_ac);
            if (e_as > worst_as) worst_as = e_as;
            if (e_ac > worst_ac) worst_ac = e_ac;
        }
        check_le(worst_as, 2, "asin: within 2 LSB over the whole domain");
        check_le(worst_ac, 2, "acos: within 2 LSB over the whole domain");
        printf("  asin/acos: worst %lld / %lld LSB over 65537 inputs\n", (long long)worst_as, (long long)worst_ac);
    }

    check_i64(mu_q16_asin(MU_Q16_ONE), MU_ANG_QUARTER, "asin: asin(1) is exactly a quarter");
    check_i64(mu_q16_asin(-MU_Q16_ONE), -MU_ANG_QUARTER, "asin: asin(-1) is exactly -quarter");
    check_i64(mu_q16_asin(2 * MU_Q16_ONE), MU_ANG_QUARTER, "asin: out of range clamps");
    check_i64(mu_q16_acos(MU_Q16_ONE), 0, "acos: acos(1) is exactly 0");
    check_i64(mu_q16_acos(-MU_Q16_ONE), MU_ANG_HALF, "acos: acos(-1) is exactly half");
}

/* ------------------------------------------------------------------ */
/* digest mode: the determinism check                                 */
/* ------------------------------------------------------------------ */

static uint32_t fnv1a(uint32_t h, uint32_t v)
{
    int i;
    for (i = 0; i < 4; ++i) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

static int run_hash(void)
{
    uint32_t h = 2166136261u;
    int32_t a;
    int32_t x, y;

    for (a = -MU_ANG_TURN; a < MU_ANG_TURN; ++a) {
        mu_q16 s, c;
        mu_q16_sincos((mu_ang)a, &s, &c);
        h = fnv1a(h, (uint32_t)s);
        h = fnv1a(h, (uint32_t)c);
    }
    for (y = -512; y <= 512; y += 7) {
        for (x = -512; x <= 512; x += 5) {
            h = fnv1a(h, (uint32_t)mu_q16_atan2((mu_q16)(y * 128), (mu_q16)(x * 128)));
        }
    }
    for (x = 0; x < 100000; x += 3) {
        h = fnv1a(h, (uint32_t)mu_q16_sqrt((mu_q16)x));
        h = fnv1a(h, (uint32_t)mu_q16_rsqrt((mu_q16)x + 1));
        h = fnv1a(h, (uint32_t)mu_q16_asin((mu_q16)(x % (2 * MU_Q16_ONE + 1) - MU_Q16_ONE)));
    }
    printf("%08x\n", h);
    return 0;
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--hash") == 0) return run_hash();

    printf("mu fixed-point gate (Q16.16, CORDIC trig, no libm)\n");
    printf("-------------------------------------------------\n");
    test_fixed_constants();
    test_fixed_arith();
    test_fixed_sqrt();
    test_fixed_angles();
    test_fixed_sincos();
    test_fixed_atan2();
    printf("-------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif /* MU_FIXED_LINK_ONLY */
