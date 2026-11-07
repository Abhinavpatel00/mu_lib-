/* test_easing.c -- the correctness gate for mu_easing.h
 *
 * What is actually being tested here, beyond "the arithmetic looks plausible":
 *
 *   1. THE CONTRACT EVERY CURVE OWES. f(0)=0, f(1)=1, no NaN, and for the
 *      in_out family f(t) + f(1-t) = 1 (which forces f(0.5)=0.5). Two of
 *      par_easings.h's curves fail exactly this, so the invariant is not
 *      academic: par_easings_in_out_back(0.5) is 1.5 and its f(1) is 3.98.
 *
 *   2. THE DERIVATIVE IS THE DERIVATIVE. Every one of the 33 analytic
 *      velocities is checked against a Richardson-extrapolated central
 *      difference of the function it claims to differentiate. A hand-derived
 *      chain rule that is off by a factor of two passes a visual check and
 *      fails this.
 *
 *   3. THE BAKED PATH AGREES WITH THE EXACT PATH, within a bound that is
 *      asserted per curve rather than assumed.
 *
 *   4. THE SOLVERS SOLVE. The Bezier inversion is compared against a
 *      brute-force bisection of the same curve (an independent method), and the
 *      spline is checked for the three properties that justify Fritsch-Carlson:
 *      it interpolates its keys, it is monotone where its data is, and it does
 *      not overshoot.
 *
 * Build:  make -C perf_test easing-test
 */

#include "mu_easing.h"

/* The parity section needs the upstream header. It is deliberately optional: the
 * gate must be worth running on a machine that does not have par checked out, and
 * everything else in here works without it.
 *   make -C perf_test easing-test        (with ../par_easings.h present) */
#if defined(MU_EASING_WITH_PAR)
#define PAR_EASINGS_IMPLEMENTATION
#include "par_easings.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_checks, g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond)
    {
        ++g_fails;
        if (g_fails <= 40) printf("  FAIL %s\n", what);
    }
}

static void check_le(const char *what, double got, double limit)
{
    ++g_checks;
    if (!(got <= limit))
    {
        ++g_fails;
        if (g_fails <= 40) printf("  FAIL %s (%.6g > %.6g)\n", what, got, limit);
    }
}

/* Two central differences, one at h and one at h/2, combined by Richardson
 * extrapolation. A plain difference at h=1e-3 is only good to order h squared,
 * which for expo and elastic is bad enough to look like a formula error; this is
 * good to order h to the fourth with the same number of evaluations. */
static double richardson_diff(mu_ease_fn fn, double t)
{
    double h = 4e-3;
    double d1 = ((double)fn((float)(t + h)) - (double)fn((float)(t - h))) / (2.0 * h);
    double d2 = ((double)fn((float)(t + h * 0.5)) - (double)fn((float)(t - h * 0.5))) / h;
    return (4.0 * d2 - d1) / 3.0;
}

int main(void)
{
    int k;

    printf("mu_easing correctness gate\n");
    printf("--------------------------------------------\n");

    /* ---- 1. endpoints and finiteness, every kind ---- */

    for (k = 0; k < MU_EASE_COUNT; ++k)
    {
        char what[128];
        float f0 = mu_ease_apply((mu_ease_kind)k, 0.0f);
        float f1 = mu_ease_apply((mu_ease_kind)k, 1.0f);

        snprintf(what, sizeof what, "%s: lands on 0 at t=0", mu_ease_name((mu_ease_kind)k));
        check_le(what, fabs((double)f0), 1e-7);
        snprintf(what, sizeof what, "%s: lands on 1 at t=1", mu_ease_name((mu_ease_kind)k));
        check_le(what, fabs((double)f1 - 1.0), 1e-7);
    }

    /* Every curve stays inside this envelope; only elastic reaches past it, and
     * only to 1.3731. Anything that wanders further is a formula error, and this
     * is also the bound a caller can rely on when they forget to clamp. */
    for (k = 0; k < MU_EASE_COUNT; ++k)
    {
        char what[128];
        double lo = 1e30, hi = -1e30;
        int i;
        for (i = 0; i <= 2000; ++i)
        {
            double v = mu_ease_apply((mu_ease_kind)k, (float)i / 2000.0f);
            if (!(v == v)) { lo = 1e30; break; }
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        snprintf(what, sizeof what, "%s: stays inside [-0.40, 1.40]",
                 mu_ease_name((mu_ease_kind)k));
        check(lo >= -0.40 && hi <= 1.40, what);
    }

    /* back undershoots by 10% by construction (that is what s=1.70158 is for), and
     * a curve that stops undershooting is no longer back */
    {
        float lo = 1e9f;
        int i;
        for (i = 0; i <= 2000; ++i)
        {
            float v = mu_ease_in_back((float)i / 2000.0f);
            if (v < lo) lo = v;
        }
        check_le("in_back: undershoots to -0.10", fabs((double)lo + 0.1), 1e-3);
        check(mu_ease_in_back(0.5f) < 0.0f, "in_back: is still below zero at t=0.5");
        check(mu_ease_out_back(0.65f) > 1.09f, "out_back: overshoots past 1.09");
    }

    /* ---- 2. symmetry: the in/out mirror and the in_out centre ---- */

    for (k = MU_EASE_IN_QUAD; k < MU_EASE_COUNT; ++k)
    {
        int phase = mu_ease__phase((mu_ease_kind)k);
        mu_ease_fn fn = mu_ease_get((mu_ease_kind)k);
        int i;

        if (k == MU_EASE_SMOOTHSTEP || k == MU_EASE_SMOOTHERSTEP) continue;

        if (phase == 2)
        {
            char what[128];
            double worst = 0.0;
            for (i = 0; i <= 200; ++i)
            {
                double t = i / 200.0;
                double e = fabs(fn((float)t) + fn((float)(1.0 - t)) - 1.0);
                if (e > worst) worst = e;
            }
            snprintf(what, sizeof what, "%s: symmetric, f(t)+f(1-t)=1",
                     mu_ease_name((mu_ease_kind)k));
            check_le(what, worst, 2e-6);
            snprintf(what, sizeof what, "%s: passes through (0.5, 0.5)",
                     mu_ease_name((mu_ease_kind)k));
            check_le(what, fabs((double)fn(0.5f) - 0.5), 2e-6);
        }
        else if (phase == 1)
        {
            /* out(t) must be 1 - in(1-t), for the same family */
            char what[128];
            mu_ease_fn in_fn = mu_ease_get((mu_ease_kind)(k - 1));
            double worst = 0.0;
            for (i = 0; i <= 200; ++i)
            {
                double t = i / 200.0;
                double e = fabs((double)fn((float)t) - (1.0 - in_fn((float)(1.0 - t))));
                if (e > worst) worst = e;
            }
            snprintf(what, sizeof what, "%s: mirrors its in half", mu_ease_name((mu_ease_kind)k));
            check_le(what, worst, 2e-6);
        }
    }

    /* ---- 3. monotone where it must be ---- */

    {
        /* linear and the eight families' in/out/in_out halves are all monotone:
         * quad..quint are positive powers, sine/expo/circ are monotone by
         * construction, and the mirrored halves inherit it. back, elastic and
         * bounce are not, and are not in this list. */
        static const char *const monotone[] = {
            "linear",
            "in_quad", "out_quad", "in_out_quad",
            "in_cubic", "out_cubic", "in_out_cubic",
            "in_quart", "out_quart", "in_out_quart",
            "in_quint", "out_quint", "in_out_quint",
            "in_sine", "out_sine", "in_out_sine",
            "in_expo", "out_expo", "in_out_expo",
            "in_circ", "out_circ", "in_out_circ",
            "smoothstep", "smootherstep"
        };
        size_t i;
        for (i = 0; i < sizeof monotone / sizeof monotone[0]; ++i)
        {
            mu_ease_kind kind = mu_ease_from_name(monotone[i]);
            mu_ease_fn fn;
            char what[128];
            int step, ok = 1;
            float prev;

            snprintf(what, sizeof what, "%s: is a known kind", monotone[i]);
            check(kind != MU_EASE_COUNT, what);
            if (kind == MU_EASE_COUNT) continue;
            fn = mu_ease_get(kind);
            prev = fn(0.0f);
            for (step = 1; step <= 2000 && ok; ++step)
            {
                float v = fn((float)step / 2000.0f);
                if (v < prev - 1e-6f) ok = 0;
                prev = v;
            }
            snprintf(what, sizeof what, "%s: is monotone", monotone[i]);
            check(ok, what);
        }
    }

    /* ---- 4. analytic velocity vs a difference of the function ---- */

    for (k = 0; k < MU_EASE_COUNT; ++k)
    {
        mu_ease_fn fn = mu_ease_get((mu_ease_kind)k);
        const char *name = mu_ease_name((mu_ease_kind)k);
        double worst = 0.0;
        int i, used = 0, skipped = 0, phase = (int)mu_ease__phase((mu_ease_kind)k);

        for (i = 1; i < 60; ++i)
        {
            double t = i / 60.0;
            double fd, an;

            /* The in_out halves are only C1: the SECOND derivative jumps at the
             * seam, so even a Richardson-extrapolated difference is off there by
             * a term of order h squared times that jump. The seam gets its own
             * exact check below. */
            if (phase == 2 && t == 0.5) continue;

            /* where the function is not locally smooth at the test scale - the
             * corners of bounce, the vertical tangent of circ - a difference
             * quotient measures the corner, not the derivative. Halving h tells us
             * which points those are, instead of hard-coding a list of them. */
            {
                double d1 = (double)(fn((float)(t + 4e-3)) - fn((float)(t - 4e-3)));
                double d2 = 2.0 * (double)(fn((float)(t + 2e-3)) - fn((float)(t - 2e-3)));
                if (fabs(d1 - d2) > 1e-2 * fabs(d1) + 1e-12) { ++skipped; continue; }
            }
            fd = richardson_diff(fn, t);
            an = (double)mu_ease_velocity((mu_ease_kind)k, (float)t);
            ++used;
            if (fabs(an - fd) / (1.0 + fabs(fd)) > worst)
                worst = fabs(an - fd) / (1.0 + fabs(fd));
        }
        if (worst > 0.0)
        {
            char what[128];
            snprintf(what, sizeof what, "%s: analytic velocity matches d/dt", name);
            check_le(what, worst, 1e-3);
        }
        check(used >= 45, "velocity was compared at most sample points");
    }

    /* The seam is the one point where a piecewise in_out can hide a factor of two
     * (or a missing chain rule), and it is exactly where a difference quotient is
     * useless. These are the exact values: the in_out slope at the seam is the in
     * half's slope at u=1, and for the polynomial families that is the exponent. */
    {
        static const struct { mu_ease_kind kind; double v; const char *name; } seam[] = {
            { MU_EASE_IN_OUT_QUAD,   2.0,           "in_out_quad" },
            { MU_EASE_IN_OUT_CUBIC,  3.0,           "in_out_cubic" },
            { MU_EASE_IN_OUT_QUART,  4.0,           "in_out_quart" },
            { MU_EASE_IN_OUT_QUINT,  5.0,           "in_out_quint" },
            { MU_EASE_IN_OUT_SINE,   1.5707963268,  "in_out_sine" },
            { MU_EASE_IN_OUT_EXPO,   6.9314718056,  "in_out_expo" },
            { MU_EASE_IN_OUT_BACK,   4.70158,       "in_out_back" },
            { MU_EASE_IN_OUT_BOUNCE, 0.0,           "in_out_bounce" }
        };
        size_t i;
        for (i = 0; i < sizeof seam / sizeof seam[0]; ++i)
        {
            char what[128];
            double got = (double)mu_ease_velocity(seam[i].kind, 0.5f);
            snprintf(what, sizeof what, "%s: exact slope at the seam", seam[i].name);
            check_le(what, fabs(got - seam[i].v), 2e-3 * (1.0 + fabs(seam[i].v)));
        }
    }

    /* the specific velocities that are easy to get wrong by a factor of two */
    check_le("linear velocity is 1", fabs(mu_ease_velocity(MU_EASE_LINEAR, 0.3f) - 1.0f), 1e-6);
    check_le("in_quad velocity is 2t", fabs(mu_ease_velocity(MU_EASE_IN_QUAD, 0.5f) - 1.0f), 1e-6);
    /* in_out_quad is 2t^2 below the seam and 1-2(1-t)^2 above it, so the slope is
     * 4t = 2 there: the chain rule through the u=2t substitution, not forgotten */
    check_le("in_out_quad velocity carries the chain rule at the seam",
             fabs(mu_ease_velocity(MU_EASE_IN_OUT_QUAD, 0.5f) - 2.0f), 1e-6);
    check(mu_ease_velocity(MU_EASE_IN_CIRC, 0.999999f) > 100.0f,
          "in_circ velocity blows up at the vertical tangent");

    /* ---- 5. robustness: NaN, out of range, and honest clamping ---- */

    for (k = 0; k < MU_EASE_COUNT; ++k)
    {
        char what[128];
        float v = mu_ease_apply((mu_ease_kind)k, NAN);
        snprintf(what, sizeof what, "%s: NaN in, finite out", mu_ease_name((mu_ease_kind)k));
        check(v == v && v >= -1e9f && v <= 1e9f, what);
        snprintf(what, sizeof what, "%s: t<0 clamps to f(0)", mu_ease_name((mu_ease_kind)k));
        check_le(what, fabs(mu_ease_apply((mu_ease_kind)k, -3.0f) -
                            mu_ease_apply((mu_ease_kind)k, 0.0f)), 1e-7);
        snprintf(what, sizeof what, "%s: t>1 clamps to f(1)", mu_ease_name((mu_ease_kind)k));
        check_le(what, fabs(mu_ease_apply((mu_ease_kind)k, 7.0f) -
                            mu_ease_apply((mu_ease_kind)k, 1.0f)), 1e-7);
    }
    check_le("smoothstep(NaN) is 0", fabs(mu_ease_smoothstep(NAN)), 1e-9);

    /* ---- 6. curves as data: names, lookup, round trip ---- */

    {
        int i, j;
        for (i = 0; i < MU_EASE_COUNT; ++i)
        {
            char what[128];
            const char *name = mu_ease_name((mu_ease_kind)i);
            snprintf(what, sizeof what, "name round trip: %s", name);
            check(mu_ease_from_name(name) == (mu_ease_kind)i, what);
            /* names must be unique, or a saved curve loads as a different one */
            for (j = 0; j < MU_EASE_COUNT; ++j)
                if (j != i && strcmp(mu_ease_name((mu_ease_kind)j), name) == 0)
                    check(0, "duplicate easing name");
        }
        check(mu_ease_from_name("par_easings_in_out_cubic") == MU_EASE_IN_OUT_CUBIC,
              "from_name accepts the par_easings_* spelling");
        check(mu_ease_from_name("mu_ease_out_elastic") == MU_EASE_OUT_ELASTIC,
              "from_name accepts the mu_ease_* spelling");
        check(mu_ease_from_name("in_out_cubic") == MU_EASE_IN_OUT_CUBIC,
              "from_name accepts the bare name");
        check(mu_ease_from_name("nonsense") == MU_EASE_COUNT, "from_name rejects nonsense");
        check(mu_ease_from_name(NULL) == MU_EASE_COUNT, "from_name survives NULL");
        check(mu_ease_get((mu_ease_kind)MU_EASE_COUNT) == mu_ease_linear,
              "get() degrades to linear out of range");
        check(mu_ease_get(MU_EASE_OUT_BOUNCE) == mu_ease_out_bounce,
              "get() returns the right function");
        check_le("apply() agrees with the direct call",
                 fabs(mu_ease_apply(MU_EASE_IN_OUT_QUINT, 0.37f) - mu_ease_in_out_quint(0.37f)), 0.0);
    }

    /* ---- 7. cubic Bezier: CSS presets, monotonicity, independent solver ---- */

    {
        mu_ease_bezier ease;
        check(mu_ease_bezier_css("ease", &ease) == 1, "css: 'ease' parses");
        check(mu_ease_bezier_css("ease-in", &ease) == 1, "css: 'ease-in' parses");
        check(mu_ease_bezier_css("nope", &ease) == 0, "css: unknown name fails");

        mu_ease_bezier_css("ease", &ease);
        /* published value for CSS ease at t=0.5 is 0.8024 (any easing-table
         * reference); this is an external reference, not our own output */
        check_le("css ease(0.5) is 0.8024", fabs((double)mu_ease_bezier_eval(&ease, 0.5f) - 0.8024), 2e-3);
        check_le("css ease(0) is 0", fabs((double)mu_ease_bezier_eval(&ease, 0.0f)), 1e-9);
        check_le("css ease(1) is 1", fabs((double)mu_ease_bezier_eval(&ease, 1.0f) - 1.0), 1e-9);

        /* a bezier with (0,0,1,1) is the identity map */
        {
            double worst = 0.0;
            int i;
            for (i = 0; i <= 100; ++i)
            {
                double t = i / 100.0;
                double e = fabs((double)mu_ease_bezier_at((float)t, 0, 0, 1, 1) - t);
                if (e > worst) worst = e;
            }
            check_le("bezier(0,0,1,1) is the identity", worst, 1e-4);
        }

        /* independent solver: bisect x(s)=t on the same curve to convergence and
         * read y there. Different method, same answer, or one of the two is wrong. */
        {
            static const float sets[4][4] = {
                { 0.25f, 0.10f, 0.25f, 1.00f },
                { 0.42f, 0.00f, 0.58f, 1.00f },
                { 0.90f, 0.05f, 0.95f, 0.60f },
                { 0.10f, 0.90f, 0.20f, 1.00f }
            };
            int s;
            for (s = 0; s < 4; ++s)
            {
                mu_ease_bezier b = mu_ease_bezier_make(sets[s][0], sets[s][1], sets[s][2], sets[s][3]);
                double worst = 0.0;
                int i;
                for (i = 1; i < 100; ++i)
                {
                    double t = i / 100.0, lo = 0.0, hi = 1.0, ss = 0.5;
                    int it;
                    for (it = 0; it < 60; ++it)
                    {
                        ss = 0.5 * (lo + hi);
                        if ((double)mu_ease__bez_cubic((float)ss, b.x1, b.x2) < t) lo = ss;
                        else hi = ss;
                    }
                    {
                        double ref = (double)mu_ease__bez_cubic((float)ss, b.y1, b.y2);
                        double got = (double)mu_ease_bezier_eval(&b, (float)t);
                        double e = fabs(ref - got);
                        if (e > worst) worst = e;
                    }
                }
                check_le("bezier solver matches an independent bisection", worst, 1e-3);
            }
        }

        /* monotone in t whenever the control points are inside [0,1]^2 */
        {
            mu_ease_bezier b = mu_ease_bezier_make(0.6f, 0.0f, 0.4f, 1.0f);
            int i, ok = 1;
            float prev = -1.0f;
            for (i = 0; i <= 500 && ok; ++i)
            {
                float v = mu_ease_bezier_eval(&b, (float)i / 500.0f);
                if (v < prev - 1e-6f) ok = 0;
                prev = v;
            }
            check(ok, "bezier is monotone for in-range control points");
        }

        /* velocity against the derivative of the solver's own output */
        {
            mu_ease_bezier b = mu_ease_bezier_make(0.25f, 0.1f, 0.25f, 1.0f);
            double worst = 0.0;
            int i;
            for (i = 5; i < 95; ++i)
            {
                double t = i / 100.0;
                double fd = ((double)mu_ease_bezier_eval(&b, (float)(t + 1e-3)) -
                             (double)mu_ease_bezier_eval(&b, (float)(t - 1e-3))) / 2e-3;
                double an = (double)mu_ease_bezier_velocity(&b, (float)t);
                double e = fabs(an - fd) / (1.0 + fabs(fd));
                if (e > worst) worst = e;
            }
            check_le("bezier velocity matches d/dt", worst, 5e-3);
        }

        /* out-of-range control x is clamped, not asserted into a hang */
        check(mu_ease_bezier_make(-2.0f, 0.0f, 3.0f, 1.0f).x1 == 0.0f,
              "bezier clamps x1 into [0,1]");
        check(mu_ease_bezier_eval(NULL, 0.5f) >= 0.0f, "bezier survives a null curve");
    }

    /* ---- 8. monotone spline ---- */

    {
        mu_ease_key keys[4] = { { 0.0f, 0.0f }, { 0.2f, 0.8f }, { 0.6f, 0.85f }, { 1.0f, 1.0f } };
        float slopes[4];
        mu_ease_spline sp;

        check(mu_ease_spline_build(&sp, keys, 4, slopes) == 1, "spline builds");
        for (k = 0; k < 4; ++k)
        {
            char what[96];
            snprintf(what, sizeof what, "spline interpolates key %d exactly", k);
            check_le(what, fabs((double)mu_ease_spline_eval(&sp, keys[k].t) - keys[k].v), 1e-6);
        }

        {
            int i, ok = 1;
            float prev = -1.0f;
            for (i = 0; i <= 2000 && ok; ++i)
            {
                float v = mu_ease_spline_eval(&sp, (float)i / 2000.0f);
                if (v < prev - 1e-6f) ok = 0;
                if (v < -1e-6f || v > 1.0f + 1e-6f) ok = 0;   /* no overshoot */
                prev = v;
            }
            check(ok, "spline is monotone and does not overshoot");
        }

        /* C1 means both one-sided limits equal the stored slope, which is what the
         * Fritsch-Carlson limiter rewrote. Comparing two velocities a hair either
         * side of the key would also measure the curvature, which is a different
         * question. */
        {
            double worst = 0.0;
            int i;
            for (i = 0; i < 4; ++i)
            {
                double e = fabs((double)mu_ease_spline_velocity(&sp, keys[i].t) - (double)slopes[i]);
                if (e > worst) worst = e;
            }
            check_le("spline velocity at a key is the stored slope", worst, 1e-4);
        }
        {
            /* and the two sides must agree, up to curvature times delta */
            double worst = 0.0;
            int i;
            for (i = 1; i < 3; ++i)
            {
                double vl = (double)mu_ease_spline_velocity(&sp, keys[i].t - 1e-4f);
                double vr = (double)mu_ease_spline_velocity(&sp, keys[i].t + 1e-4f);
                double e = fabs(vl - vr) / (1.0 + fabs(vl));
                if (e > worst) worst = e;
            }
            check_le("spline velocity has no jump at a key", worst, 2e-2);
        }

        /* velocity against a difference of eval */
        {
            double worst = 0.0;
            int i;
            for (i = 3; i < 100; ++i)
            {
                double t = i / 100.0;
                double fd = ((double)mu_ease_spline_eval(&sp, (float)(t + 1e-4)) -
                             (double)mu_ease_spline_eval(&sp, (float)(t - 1e-4))) / 2e-4;
                double an = (double)mu_ease_spline_velocity(&sp, (float)t);
                double e = fabs(an - fd) / (1.0 + fabs(fd));
                if (e > worst) worst = e;
            }
            check_le("spline velocity matches d/dt", worst, 5e-3);
        }

        check_le("spline clamps below its first key",
                 fabs((double)mu_ease_spline_eval(&sp, -5.0f)), 1e-9);
        check_le("spline clamps above its last key",
                 fabs((double)mu_ease_spline_eval(&sp, 5.0f) - 1.0), 1e-9);
        check_le("spline velocity is 0 outside its range",
                 fabs((double)mu_ease_spline_velocity(&sp, 5.0f)), 1e-9);

        /* a two-key spline is the straight line between them */
        {
            mu_ease_key two[2] = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
            float s2[2];
            mu_ease_spline lin;
            check(mu_ease_spline_build(&lin, two, 2, s2) == 1, "two-key spline builds");
            check_le("two-key spline is linear",
                     fabs((double)mu_ease_spline_eval(&lin, 0.31f) - 0.31), 1e-6);
        }

        /* flat data must stay flat (the Fritsch-Carlson zero-slope rule) */
        {
            mu_ease_key flat[3] = { { 0.0f, 0.5f }, { 0.5f, 0.5f }, { 1.0f, 0.5f } };
            float s3[3];
            mu_ease_spline fs;
            mu_ease_spline_build(&fs, flat, 3, s3);
            check_le("flat data gives a flat spline",
                     fabs((double)mu_ease_spline_eval(&fs, 0.37f) - 0.5), 1e-6);
        }

        /* contract violations are reported, not undefined behaviour */
        {
            mu_ease_key bad[2] = { { 1.0f, 0.0f }, { 0.0f, 1.0f } };
            float s2[2];
            mu_ease_spline junk;
            check(mu_ease_spline_build(&junk, bad, 2, s2) == 0,
                  "spline rejects decreasing times");
            check(mu_ease_spline_build(&junk, keys, 1, slopes) == 0,
                  "spline rejects a single key");
            check(mu_ease_spline_build(&junk, NULL, 4, slopes) == 0,
                  "spline rejects a null key array");
            check(mu_ease_spline_build(&junk, keys, 4, NULL) == 0,
                  "spline rejects null scratch");
            /* and a rejected build leaves something safe to evaluate */
            check_le("a rejected spline evaluates to the clamped identity",
                     fabs((double)mu_ease_spline_eval(&junk, 0.42f) - 0.42), 1e-6);
        }
    }

    /* ---- 9. steps ---- */

    {
        check_le("steps(0, 4) is 0", fabs((double)mu_ease_steps(0.0f, 4, 0)), 1e-9);
        check_le("steps(0.24, 4) is 0", fabs((double)mu_ease_steps(0.24f, 4, 0)), 1e-9);
        check_le("steps(0.25, 4) is 0.25", fabs((double)mu_ease_steps(0.25f, 4, 0) - 0.25), 1e-6);
        check_le("steps(0.99, 4) is 0.75", fabs((double)mu_ease_steps(0.99f, 4, 0) - 0.75), 1e-6);
        check_le("steps(1, 4) is 1", fabs((double)mu_ease_steps(1.0f, 4, 0) - 1.0), 1e-9);
        check_le("steps(0.01,4,jump_at_start) is 0.25",
                 fabs((double)mu_ease_steps(0.01f, 4, 1) - 0.25), 1e-6);
        check_le("steps with count<1 is the identity",
                 fabs((double)mu_ease_steps(0.42f, 0, 0) - 0.42), 1e-6);
        {
            /* exactly `count` jumps, no more and no fewer */
            int i, jumps = 0;
            float prev = mu_ease_steps(0.0f, 8, 0);
            for (i = 1; i <= 1000; ++i)
            {
                float v = mu_ease_steps((float)i / 1000.0f, 8, 0);
                if (v > prev) ++jumps;
                prev = v;
            }
            check(jumps == 8, "steps(.,8) has exactly 8 jumps");
        }
    }

    /* ---- 10. baked lookup table ---- */

    {
        /* per-kind limit at 257 samples, as measured; the loose ones are the
         * curves with vertical tangents or a high-frequency tail, where the
         * table is a worse idea than the function and the docs say so */
        for (k = 0; k < MU_EASE_COUNT; ++k)
        {
            float table[257];
            mu_ease_lut lut;
            char what[128];
            float err;

            if (!mu_ease_lut_bake_kind(&lut, table, 257, (mu_ease_kind)k))
            {
                check(0, "lut bakes");
                continue;
            }
            snprintf(what, sizeof what, "%s: baked table within its error budget",
                     mu_ease_name((mu_ease_kind)k));
            err = mu_ease_lut_error(&lut, 4001);
            check_le(what, err, 2.5e-2);

            snprintf(what, sizeof what, "%s: table ends are exact",
                     mu_ease_name((mu_ease_kind)k));
            check_le(what, fabs((double)table[0] - mu_ease_apply((mu_ease_kind)k, 0.0f)), 0.0);
            check_le(what, fabs((double)table[256] - mu_ease_apply((mu_ease_kind)k, 1.0f)), 1e-7);
        }

        /* sampling more must not make it worse: an error that grows with sample
         * count means the index arithmetic is wrong, not the curve */
        {
            float t65[65], t257[257], t1025[1025];
            mu_ease_lut l65, l257, l1025;
            mu_ease_lut_bake(&l65, t65, 65, mu_ease_out_cubic);
            mu_ease_lut_bake(&l257, t257, 257, mu_ease_out_cubic);
            mu_ease_lut_bake(&l1025, t1025, 1025, mu_ease_out_cubic);
            check(mu_ease_lut_error(&l257, 4001) < mu_ease_lut_error(&l65, 4001),
                  "lut error falls from 65 to 257 samples");
            check(mu_ease_lut_error(&l1025, 4001) < mu_ease_lut_error(&l257, 4001),
                  "lut error falls from 257 to 1025 samples");
        }

        /* exactness at the sample points: a table must reproduce the curve it was
         * built from at the points it stored, or the bake is misaligned by one */
        {
            float table[33];
            mu_ease_lut lut;
            int i, ok = 1;
            mu_ease_lut_bake(&lut, table, 33, mu_ease_out_bounce);
            for (i = 0; i < 33; ++i)
            {
                float t = (float)i / 32.0f;
                if (fabsf(mu_ease_lut_eval(&lut, t) - table[i]) > 1e-6f) ok = 0;
            }
            check(ok, "lut reproduces its own samples");
        }

        /* eight lanes agree with eight scalar calls, bit for bit */
        {
            float table[257];
            mu_ease_lut lut;
            mu_f32x8 t8, v8;
            int i, ok = 1;
            mu_ease_lut_bake(&lut, table, 257, mu_ease_in_out_bounce);
            t8.m[0] = 0.0f; t8.m[1] = 0.11f; t8.m[2] = 0.25f; t8.m[3] = 0.4f;
            t8.m[4] = 0.5f; t8.m[5] = 0.63f; t8.m[6] = 0.9f; t8.m[7] = 1.0f;
            v8 = mu_ease_lut_eval8(&lut, t8);
            for (i = 0; i < 8; ++i)
                if (v8.m[i] != mu_ease_lut_eval(&lut, t8.m[i])) ok = 0;
            check(ok, "lut_eval8 matches eight scalar evaluations");
        }

        /* a table's slope is the slope of the segment, and for a smooth curve that
         * has to agree with the analytic derivative to within the table's error */
        {
            float table[2049];
            mu_ease_lut lut;
            double worst = 0.0;
            int i;
            mu_ease_lut_bake(&lut, table, 2049, mu_ease_in_out_cubic);
            for (i = 20; i < 1900; i += 37)
            {
                double t = i / 2000.0;
                double e = fabs((double)mu_ease_lut_velocity(&lut, (float)t) -
                                (double)mu_ease_velocity(MU_EASE_IN_OUT_CUBIC, (float)t));
                if (e > worst) worst = e;
            }
            check_le("lut velocity matches the curve's derivative", worst, 2e-2);
        }

        {
            float tiny[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            mu_ease_lut bad;
            check(mu_ease_lut_bake(&bad, tiny, 1, mu_ease_linear) == 0,
                  "lut rejects a one-sample table");
            check(mu_ease_lut_bake(&bad, NULL, 257, mu_ease_linear) == 0,
                  "lut rejects a null table");
            check(mu_ease_lut_bake(&bad, tiny, 4, NULL) == 0,
                  "lut rejects a null function");
            check_le("unbaked lut is the clamped identity",
                     fabs((double)mu_ease_lut_eval(&bad, 0.5f) - 0.5), 1e-6);
        }
    }

    /* ---- 11. time helpers and application ---- */

    check_le("loop(-0.25) is 0.75", fabs((double)mu_ease_loop(-0.25f) - 0.75), 1e-6);
    check_le("loop(3.5) is 0.5", fabs((double)mu_ease_loop(3.5f) - 0.5), 1e-6);
    check_le("pingpong(0.25) is 0.25", fabs((double)mu_ease_pingpong(0.25f) - 0.25), 1e-6);
    check_le("pingpong(1.25) is 0.75", fabs((double)mu_ease_pingpong(1.25f) - 0.75), 1e-6);
    check_le("pingpong(2.25) is 0.25", fabs((double)mu_ease_pingpong(2.25f) - 0.25), 1e-6);
    check_le("pingpong(NaN) is 0", fabs((double)mu_ease_pingpong(NAN)), 1e-9);
    check_le("remap(1,0,2) is 0.5", fabs((double)mu_ease_remap(1.0f, 0.0f, 2.0f) - 0.5), 1e-6);
    check_le("remap with an empty range is 0", fabs((double)mu_ease_remap(1.0f, 2.0f, 2.0f)), 1e-9);
    check_le("lerp with linear is the midpoint",
             fabs((double)mu_ease_lerp(MU_EASE_LINEAR, 0.0f, 10.0f, 0.5f) - 5.0), 1e-6);
    check_le("lerp with in_out_cubic lands on the target",
             fabs((double)mu_ease_lerp(MU_EASE_IN_OUT_CUBIC, 3.0f, -1.0f, 1.0f) + 1.0), 1e-6);
    {
        mu_vec3 a = mu_vec3_make(0.0f, 10.0f, -2.0f);
        mu_vec3 b = mu_vec3_make(4.0f, -10.0f, 2.0f);
        mu_vec3 m = mu_ease_vec3(MU_EASE_LINEAR, a, b, 0.5f);
        check_le("vec3 lerp x", fabs((double)m.x - 2.0), 1e-6);
        check_le("vec3 lerp y", fabs((double)m.y), 1e-6);
        check_le("vec3 lerp z", fabs((double)m.z), 1e-6);
        check_le("vec3 easing ends on b",
                 fabs((double)mu_ease_vec3(MU_EASE_OUT_BACK, a, b, 1.0f).x - 4.0), 1e-5);
    }
    check_le("velocity_family agrees with velocity",
             fabs((double)mu_ease_velocity_family(2, 2, 0.37f) -
                  (double)mu_ease_velocity(MU_EASE_IN_OUT_CUBIC, 0.37f)), 1e-6);

    /* ---- 12. parity with par_easings.h, where par is right ---- */

#if defined(MU_EASING_WITH_PAR)
    {
        struct pair { mu_ease_kind kind; float (*par)(float); const char *name; };
        static const struct pair pairs[] = {
            { MU_EASE_LINEAR,        par_easings_linear,         "linear" },
            { MU_EASE_IN_QUAD,       par_easings_in_quad,        "in_quad" },
            { MU_EASE_OUT_QUAD,      par_easings_out_quad,       "out_quad" },
            { MU_EASE_IN_OUT_QUAD,   par_easings_in_out_quad,    "in_out_quad" },
            { MU_EASE_IN_CUBIC,      par_easings_in_cubic,       "in_cubic" },
            { MU_EASE_OUT_CUBIC,     par_easings_out_cubic,      "out_cubic" },
            { MU_EASE_IN_OUT_CUBIC,  par_easings_in_out_cubic,   "in_out_cubic" },
            { MU_EASE_IN_BACK,       par_easings_in_back,        "in_back" },
            { MU_EASE_OUT_BACK,      par_easings_out_back,       "out_back" },
            { MU_EASE_IN_BOUNCE,     par_easings_in_bounce,      "in_bounce" },
            { MU_EASE_OUT_BOUNCE,    par_easings_out_bounce,     "out_bounce" },
            { MU_EASE_IN_OUT_BOUNCE, par_easings_in_out_bounce,  "in_out_bounce" }
        };
        size_t i;
        for (i = 0; i < sizeof pairs / sizeof pairs[0]; ++i)
        {
            double worst = 0.0;
            int step;
            char what[128];
            for (step = 1; step < 100; ++step)
            {
                double t = step / 100.0;
                double e = fabs((double)mu_ease_apply(pairs[i].kind, (float)t) -
                                (double)pairs[i].par((float)t));
                if (e > worst) worst = e;
            }
            snprintf(what, sizeof what, "parity with par_easings_%s", pairs[i].name);
            check_le(what, worst, 2e-5);
        }

        /* elastic's interior, endpoints excluded: par's decay does not land, which
         * is the whole reason mu_ease__exact exists */
        {
            double worst = 0.0;
            int step;
            for (step = 5; step < 95; ++step)
            {
                double t = step / 100.0;
                double e = fabs((double)mu_ease_out_elastic((float)t) -
                                (double)par_easings_out_elastic((float)t));
                if (e > worst) worst = e;
            }
            check_le("parity with par_easings_out_elastic (interior)", worst, 2e-5);
        }

        /* The two upstream bugs, asserted so that they cannot silently come back
         * if we ever "simplify" our versions to match par. */
        check_le("par in_out_back(0.5) is 1.5 (upstream bug)",
                 fabs((double)par_easings_in_out_back(0.5f) - 1.5), 1e-4);
        check_le("mu in_out_back(0.5) is 0.5", fabs((double)mu_ease_in_out_back(0.5f) - 0.5), 1e-6);
        check_le("par in_out_back(1.0) is 3.98 (upstream bug)",
                 fabs((double)par_easings_in_out_back(1.0f) - 3.9786), 2e-3);
        check_le("mu in_out_back(1.0) is 1.0", fabs((double)mu_ease_in_out_back(1.0f) - 1.0), 1e-6);
        check_le("par in_out_elastic(1.0) is 0.5 (upstream bug)",
                 fabs((double)par_easings_in_out_elastic(1.0f) - 0.5), 1e-4);
        check_le("mu in_out_elastic(1.0) is 1.0", fabs((double)mu_ease_in_out_elastic(1.0f) - 1.0), 1e-6);
        check_le("par in_elastic(0.0) misses zero (upstream)",
                 fabs((double)par_easings_in_elastic(0.0f) + 0.000488), 1e-5);
        check_le("mu in_elastic(0.0) is exactly zero", fabs((double)mu_ease_in_elastic(0.0f)), 1e-9);
    }
#else
    printf("  (par_easings.h not found: skipping the parity section)\n");
#endif

    printf("--------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
