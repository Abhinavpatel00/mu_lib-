#pragma once
#ifndef MU_EASING_H
#define MU_EASING_H

/* ==========================================================================
 * mu_easing.h — easing curves as data, with derivatives, design-tool curves
 *               and a baked lookup path (single header C99)
 * ==========================================================================
 *
 * WHAT THIS IS
 * ------------
 * par_easings.h (prideout/par) is the reference: Robert Penner's easing
 * functions, 16 of them, each a closed form in t. This header keeps that idea
 * and fixes what makes it hard to actually ship:
 *
 *   1. IT IS INCOMPLETE. par_easings.h *implements* quart, quint, sine, expo and
 *      circ — `par_easings__in_circ` and friends — but never declares them, so
 *      they are unreachable dead code. That is 15 curves you cannot call. Every
 *      one of them is public here: 31 curves, in / out / in_out for ten families
 *      plus linear.
 *
 *   2. TWO OF ITS CURVES ARE WRONG, and the wrongness is not subtle:
 *          par_easings_in_out_back(0.5)   == 1.500000     (should be 0.5)
 *          par_easings_in_out_back(1.0)   == 3.978619     (should be 1.0)
 *          par_easings_in_out_elastic(1.0) == 0.500000    (should be 1.0)
 *      `in_out_back` multiplies `s` by 1.525 twice and drops a t^2, and
 *      `in_out_elastic` forgets to halve the time axis, so its two halves are
 *      only ever visited one way. Both are asserted against here (see the gate)
 *      by the invariant that actually matters: for every in_out curve
 *      f(0) = 0, f(1) = 1, f(0.5) = 0.5 and f(t) + f(1-t) = 1.
 *      The construction that makes those structural rather than lucky:
 *          out(t)    = 1 - in(1-t)
 *          in_out(t) = t < 0.5 ? 0.5*in(2t) : 1 - 0.5*in(2-2t)
 *      Every out_* and in_out_* below is defined exactly that way, so it cannot
 *      drift from its in_* half. (The cost: our in_out_back overshoots like the
 *      in half, not 1.525x more. Pass a bigger `s` to
 *      mu_ease_in_out_back_amt if you want Penner's classic look.)
 *
 *   3. IT DOES NOT LAND. Penner's expo and elastic formulas do not reach their
 *      endpoints: par_easings_in_elastic(0) is -0.000488, and
 *      par_easings_out_elastic(1) is 1.000488, because the exponential decay has
 *      not actually arrived. A tween that is supposed to end on a target value
 *      ends 0.05% short of it. Those two families are guarded at the endpoints
 *      here, so f(0) == 0 and f(1) == 1 exactly, for all 31 curves.
 *
 *   4. IT HAS NO SAFETY. par_easings_in_quad(NaN) is NaN,
 *      par_easings_in_bounce(1.5) is -0.89, and `in_circ` would be a sqrt of a
 *      negative number if it were reachable. Every public function here clamps t
 *      into [0,1] and maps NaN to 0, so a clock that spikes cannot put NaN into
 *      a transform. If you already know t is in range, the clamp is two
 *      compares on a predicted branch.
 *
 *   5. IT IS FLOAT CODE THAT RUNS IN DOUBLE. PAR_EASINGS_FLOAT is float, but the
 *      bodies call pow/cos/sin/sqrt, which promote to double and convert back on
 *      every call. Every transcendental here is the *f suffix (sinf, cosf,
 *      exp2f, sqrtf). Measured: see perf_test/PERF_EASING.md.
 *
 * WHAT IS ACTUALLY NEW (nothing in par_easings.h does any of this)
 * ----------------------------------------------------------------
 *   §4  Curves as data: a `mu_ease_kind` enum, `mu_ease_get` to fetch the
 *       function, and name -> kind / kind -> name for saving a curve in a file,
 *       binding it to a UI dropdown, or comparing two in a test.
 *   §5  Analytic velocities. `mu_ease_velocity(kind, t)` is the exact derivative,
 *       not a finite difference, for all 31 curves, and the gate cross-checks
 *       every one against a central difference. A spring, a trail, or a
 *       motion-blur shutter angle needs f'(t), and finite-differencing a UI
 *       curve is where the jitter comes from.
 *   §6  Cubic Bezier easing, the cubic-bezier(x1,y1,x2,y2) that a designer hands
 *       you from a web tool, solved with Newton plus a bisection fallback so it
 *       cannot hang on a flat segment.
 *   §7  Monotone cubic (Fritsch-Carlson) spline easing through control points,
 *       with the tangents precomputed once. This is the "here is my curve"
 *       workflow: no overshoot, C1 continuous, interpolates the keys exactly.
 *   §8  CSS-style discrete steps.
 *   §9  Baked lookup tables: sample any curve (or a Bezier, or a spline) into a
 *       buffer you own, then evaluate with a multiply and a lerp. out_elastic is
 *       11.4 ns a call and 3.1 ns out of a 257-entry table (measured,
 *       perf_test/PERF_EASING.md §3). The crossover is measured and it is *not*
 *       universal: out_quad is 1.8 ns, so baking it makes it slower.
 *   §10 Time helpers: ping-pong, loop, remap, and applying a curve to floats and
 *       mu_vec3 (the only thing most call sites actually want).
 *   §11 Eight-wide batch evaluation of a baked curve, for particles. Measured at
 *       1.09x the scalar call, not 8x: a lookup does not vectorize without a
 *       gather, and the gather is no faster than eight loads.
 *
 * MEMORY MODEL
 * ------------
 * Nothing here allocates. The spline and the LUT take caller-owned scratch:
 *
 *     float slopes[8];
 *     mu_ease_spline sp;
 *     mu_ease_spline_build(&sp, keys, 8, slopes);       -- no allocation
 *
 *     float table[257];
 *     mu_ease_lut lut;
 *     mu_ease_lut_bake(&lut, table, 257, mu_ease_out_elastic);
 *
 * A `mu_ease_lut` is a pointer + size, so it is POD, copyable, and safe to keep
 * in a struct that outlives the function that filled it (as long as `table`
 * does too - it is the caller's buffer).
 *
 * NAMING
 * ------
 *     par_easings_in_out_cubic(t)   ->   mu_ease_in_out_cubic(t)
 *     par_easings_out_bounce(t)     ->   mu_ease_out_bounce(t)
 *     par_easings_linear(t)         ->   mu_ease_linear(t)
 * i.e. the family tails are unchanged, so the port is a prefix rename.
 */

#include "mu_macros.h"
#include "mu_math.h"
#include "mu_simd.h"

#include <math.h>

MU_BEGIN_EXTERN_C

/* ==========================================================================
 * 1. Helpers
 * ========================================================================== */

/* Clamp to [0,1] and turn NaN into 0.
 *
 * NaN is checked before the compares because `NaN < 0` and `NaN > 1` are both
 * false, so a NaN would otherwise sail through the clamp untouched. */
MU_INLINE float mu_ease_clamp01(float t)
{
    if (t != t) return 0.0f;        /* NaN */
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

/* Guarantee an exact landing at both ends: several Penner formulas miss by
 * design (see the header comment), and a tween that stops 0.05% short of its
 * target is a visible pop at the end of a long animation. */
MU_INLINE float mu_ease__exact(float t, float v)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return v;
}

/* 2*pi as a float. mu_macros.h has MU_TAU, but it is a DOUBLE literal, so
 * `MU_TAU / p` promotes the whole elastic kernel to double-precision arithmetic
 * and back: exactly the thing this header claims not to do. */
#define MU_EASE_TAU       6.28318530717958647692f

#define MU_EASE_BACK_S    1.70158f
#define MU_EASE_ELASTIC_P 0.3f
#define MU_EASE_LN2       0.6931471805599453f
#define MU_EASE_HALF_PI   1.5707963267948966f

/* Hermite/mirror construction used by every out_* and in_out_* below. */
#define MU_EASE__MIRROR(in_fn, t) (1.0f - in_fn(1.0f - (t)))
#define MU_EASE__IN_OUT(in_fn, t)                                              \
    ((t) < 0.5f ? 0.5f * in_fn(2.0f * (t)) : 1.0f - 0.5f * in_fn(2.0f - 2.0f * (t)))

/* ==========================================================================
 * 2. The 31 curves
 * ==========================================================================
 *
 * Every `in_*` takes t already clamped by the caller-facing wrapper. The out_*
 * and in_out_* are the mirror construction, so they inherit endpoints and
 * symmetry from the in_* half by construction.
 */

MU_INLINE float mu_ease_linear(float t) { return mu_ease_clamp01(t); }

/* ---- quad ---- */
MU_INLINE float mu_ease__in_quad(float t) { return t * t; }
MU_INLINE float mu_ease_in_quad(float t) { return mu_ease__in_quad(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_quad(float t) { return MU_EASE__MIRROR(mu_ease__in_quad, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_quad(float t) { return MU_EASE__IN_OUT(mu_ease__in_quad, mu_ease_clamp01(t)); }

/* ---- cubic ---- */
MU_INLINE float mu_ease__in_cubic(float t) { return t * t * t; }
MU_INLINE float mu_ease_in_cubic(float t) { return mu_ease__in_cubic(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_cubic(float t) { return MU_EASE__MIRROR(mu_ease__in_cubic, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_cubic(float t) { return MU_EASE__IN_OUT(mu_ease__in_cubic, mu_ease_clamp01(t)); }

/* ---- quart ---- */
MU_INLINE float mu_ease__in_quart(float t) { float t2 = t * t; return t2 * t2; }
MU_INLINE float mu_ease_in_quart(float t) { return mu_ease__in_quart(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_quart(float t) { return MU_EASE__MIRROR(mu_ease__in_quart, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_quart(float t) { return MU_EASE__IN_OUT(mu_ease__in_quart, mu_ease_clamp01(t)); }

/* ---- quint ---- */
MU_INLINE float mu_ease__in_quint(float t) { float t2 = t * t; return t2 * t2 * t; }
MU_INLINE float mu_ease_in_quint(float t) { return mu_ease__in_quint(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_quint(float t) { return MU_EASE__MIRROR(mu_ease__in_quint, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_quint(float t) { return MU_EASE__IN_OUT(mu_ease__in_quint, mu_ease_clamp01(t)); }

/* ---- sine: 1 - cos(t*pi/2). cosf(pi/2) is -4.4e-8 in float, so the endpoint
 * guard is what makes in_sine(1) exactly 1 instead of 1.00000004. ---- */
MU_INLINE float mu_ease__in_sine(float t) { return mu_ease__exact(t, 1.0f - cosf(t * MU_EASE_HALF_PI)); }
MU_INLINE float mu_ease_in_sine(float t) { return mu_ease__in_sine(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_sine(float t) { return MU_EASE__MIRROR(mu_ease__in_sine, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_sine(float t) { return MU_EASE__IN_OUT(mu_ease__in_sine, mu_ease_clamp01(t)); }

/* ---- expo: 2^(10t-10). Does not reach 0 at t=0 without the guard (it is
 * 2^-10 = 0.000977). exp2f is a single instruction-class op, unlike powf. ---- */
MU_INLINE float mu_ease__in_expo(float t) { return mu_ease__exact(t, exp2f(10.0f * t - 10.0f)); }
MU_INLINE float mu_ease_in_expo(float t) { return mu_ease__in_expo(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_expo(float t) { return MU_EASE__MIRROR(mu_ease__in_expo, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_expo(float t) { return MU_EASE__IN_OUT(mu_ease__in_expo, mu_ease_clamp01(t)); }

/* ---- circ: 1 - sqrt(1 - t^2). The maxf keeps the sqrt argument non-negative
 * if a compiler contracts 1-t*t nastily at t==1. ---- */
MU_INLINE float mu_ease__in_circ(float t)
{
    float u = 1.0f - t * t;
    return 1.0f - sqrtf(u > 0.0f ? u : 0.0f);
}
MU_INLINE float mu_ease_in_circ(float t) { return mu_ease__in_circ(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_circ(float t) { return MU_EASE__MIRROR(mu_ease__in_circ, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_circ(float t) { return MU_EASE__IN_OUT(mu_ease__in_circ, mu_ease_clamp01(t)); }

/* ---- back: t^2((s+1)t - s), the "anticipate" curve ---- */
MU_INLINE float mu_ease__in_back_s(float t, float s) { return t * t * ((s + 1.0f) * t - s); }
MU_INLINE float mu_ease__in_back(float t) { return mu_ease__in_back_s(t, MU_EASE_BACK_S); }
MU_INLINE float mu_ease_in_back(float t) { return mu_ease__in_back(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_back(float t) { return MU_EASE__MIRROR(mu_ease__in_back, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_back(float t) { return MU_EASE__IN_OUT(mu_ease__in_back, mu_ease_clamp01(t)); }

/* ---- elastic: an exponentially damped sine.
 *
 * The phase offset s = p/4 is what makes the curve start at rest on the target
 * axis (sin(-pi/2) = -1 exactly, so the amplitude cancels at t=0 and the
 * derivative is the largest of any curve here). The decay has not reached zero
 * at t=1 - that is what mu_ease__exact is for. */
MU_INLINE float mu_ease__in_elastic_p(float t, float p)
{
    float w = MU_EASE_TAU / p;
    float s = p * 0.25f;
    float u = t - 1.0f;
    return mu_ease__exact(t, -exp2f(10.0f * u) * sinf(w * (u - s)));
}
MU_INLINE float mu_ease__in_elastic(float t) { return mu_ease__in_elastic_p(t, MU_EASE_ELASTIC_P); }
MU_INLINE float mu_ease_in_elastic(float t) { return mu_ease__in_elastic(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_elastic(float t) { return MU_EASE__MIRROR(mu_ease__in_elastic, mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_elastic(float t) { return MU_EASE__IN_OUT(mu_ease__in_elastic, mu_ease_clamp01(t)); }

/* ---- bounce: four decaying parabolic arcs.
 *
 * out_bounce is the primitive (the ball hits the floor and settles); in_bounce
 * is its mirror. The constants are Penner's: n1 = 7.5625 = (2.75/2)^2 * ... is
 * irrelevant, what matters is that each arc is tangent to the next. */
MU_INLINE float mu_ease__out_bounce(float t)
{
    const float n = 7.5625f, d = 1.0f / 2.75f;
    if (t < 1.0f * d) { return n * t * t; }
    if (t < 2.0f * d) { t -= 1.5f * d; return n * t * t + 0.75f; }
    if (t < 2.5f * d) { t -= 2.25f * d; return n * t * t + 0.9375f; }
    t -= 2.625f * d;
    return n * t * t + 0.984375f;
}
MU_INLINE float mu_ease__in_bounce(float t) { return 1.0f - mu_ease__out_bounce(1.0f - t); }
MU_INLINE float mu_ease_in_bounce(float t) { return mu_ease__in_bounce(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_out_bounce(float t) { return mu_ease__out_bounce(mu_ease_clamp01(t)); }
MU_INLINE float mu_ease_in_out_bounce(float t) { return MU_EASE__IN_OUT(mu_ease__in_bounce, mu_ease_clamp01(t)); }

/* ---- smoothstep family: Perlin's fade, the one every shader has ---- */
MU_INLINE float mu_ease_smoothstep(float t) { float u = mu_ease_clamp01(t); return u * u * (3.0f - 2.0f * u); }
MU_INLINE float mu_ease_smootherstep(float t)
{
    float u = mu_ease_clamp01(t);
    return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
}

/* ==========================================================================
 * 3. Parameterized families
 * ==========================================================================
 *
 * Penner hard-codes the two constants that matter: back's overshoot (1.70158)
 * and elastic's period (0.3). 1.70158 is the s that makes the curve peak at
 * exactly ~10% past its target; a UI usually wants to dial that. */

MU_INLINE float mu_ease_in_back_amt(float t, float s) { return mu_ease__in_back_s(mu_ease_clamp01(t), s); }
MU_INLINE float mu_ease_out_back_amt(float t, float s)
{
    float u = mu_ease_clamp01(t);
    return 1.0f - mu_ease__in_back_s(1.0f - u, s);
}
MU_INLINE float mu_ease_in_out_back_amt(float t, float s)
{
    float u = mu_ease_clamp01(t);
    return u < 0.5f ? 0.5f * mu_ease__in_back_s(2.0f * u, s)
                    : 1.0f - 0.5f * mu_ease__in_back_s(2.0f - 2.0f * u, s);
}

MU_INLINE float mu_ease_in_elastic_p(float t, float period) { return mu_ease__in_elastic_p(mu_ease_clamp01(t), period); }
MU_INLINE float mu_ease_out_elastic_p(float t, float period)
{
    float u = mu_ease_clamp01(t);
    return 1.0f - mu_ease__in_elastic_p(1.0f - u, period);
}
MU_INLINE float mu_ease_in_out_elastic_p(float t, float period)
{
    float u = mu_ease_clamp01(t);
    return u < 0.5f ? 0.5f * mu_ease__in_elastic_p(2.0f * u, period)
                    : 1.0f - 0.5f * mu_ease__in_elastic_p(2.0f - 2.0f * u, period);
}

/* ==========================================================================
 * 4. Curves as data
 * ==========================================================================
 *
 * Everything above is a fixed function, which is fine for code you write and
 * useless for code that has to *choose*: an editor dropdown, a saved tween
 * asset, a CLI that takes --ease=out_cubic, a test that loops over every curve.
 * The enum is the link. MU_EASE_COUNT is the number of kinds, so
 * `for (k = 0; k < MU_EASE_COUNT; ++k)` is the way to iterate. */

typedef enum mu_ease_kind
{
    MU_EASE_LINEAR = 0,
    MU_EASE_IN_QUAD, MU_EASE_OUT_QUAD, MU_EASE_IN_OUT_QUAD,
    MU_EASE_IN_CUBIC, MU_EASE_OUT_CUBIC, MU_EASE_IN_OUT_CUBIC,
    MU_EASE_IN_QUART, MU_EASE_OUT_QUART, MU_EASE_IN_OUT_QUART,
    MU_EASE_IN_QUINT, MU_EASE_OUT_QUINT, MU_EASE_IN_OUT_QUINT,
    MU_EASE_IN_SINE, MU_EASE_OUT_SINE, MU_EASE_IN_OUT_SINE,
    MU_EASE_IN_EXPO, MU_EASE_OUT_EXPO, MU_EASE_IN_OUT_EXPO,
    MU_EASE_IN_CIRC, MU_EASE_OUT_CIRC, MU_EASE_IN_OUT_CIRC,
    MU_EASE_IN_BACK, MU_EASE_OUT_BACK, MU_EASE_IN_OUT_BACK,
    MU_EASE_IN_ELASTIC, MU_EASE_OUT_ELASTIC, MU_EASE_IN_OUT_ELASTIC,
    MU_EASE_IN_BOUNCE, MU_EASE_OUT_BOUNCE, MU_EASE_IN_OUT_BOUNCE,
    MU_EASE_SMOOTHSTEP, MU_EASE_SMOOTHERSTEP,
    MU_EASE_COUNT
} mu_ease_kind;

typedef float (*mu_ease_fn)(float t);

/* Fetch the function for a kind. Returns mu_ease_linear for anything out of
 * range, so a corrupt enum value degrades to "no animation" instead of a null
 * call. */
MU_INLINE mu_ease_fn mu_ease_get(mu_ease_kind k)
{
    switch (k)
    {
    case MU_EASE_IN_QUAD:        return mu_ease_in_quad;
    case MU_EASE_OUT_QUAD:       return mu_ease_out_quad;
    case MU_EASE_IN_OUT_QUAD:    return mu_ease_in_out_quad;
    case MU_EASE_IN_CUBIC:       return mu_ease_in_cubic;
    case MU_EASE_OUT_CUBIC:      return mu_ease_out_cubic;
    case MU_EASE_IN_OUT_CUBIC:   return mu_ease_in_out_cubic;
    case MU_EASE_IN_QUART:       return mu_ease_in_quart;
    case MU_EASE_OUT_QUART:      return mu_ease_out_quart;
    case MU_EASE_IN_OUT_QUART:   return mu_ease_in_out_quart;
    case MU_EASE_IN_QUINT:       return mu_ease_in_quint;
    case MU_EASE_OUT_QUINT:      return mu_ease_out_quint;
    case MU_EASE_IN_OUT_QUINT:   return mu_ease_in_out_quint;
    case MU_EASE_IN_SINE:        return mu_ease_in_sine;
    case MU_EASE_OUT_SINE:       return mu_ease_out_sine;
    case MU_EASE_IN_OUT_SINE:    return mu_ease_in_out_sine;
    case MU_EASE_IN_EXPO:        return mu_ease_in_expo;
    case MU_EASE_OUT_EXPO:       return mu_ease_out_expo;
    case MU_EASE_IN_OUT_EXPO:    return mu_ease_in_out_expo;
    case MU_EASE_IN_CIRC:        return mu_ease_in_circ;
    case MU_EASE_OUT_CIRC:       return mu_ease_out_circ;
    case MU_EASE_IN_OUT_CIRC:    return mu_ease_in_out_circ;
    case MU_EASE_IN_BACK:        return mu_ease_in_back;
    case MU_EASE_OUT_BACK:       return mu_ease_out_back;
    case MU_EASE_IN_OUT_BACK:    return mu_ease_in_out_back;
    case MU_EASE_IN_ELASTIC:     return mu_ease_in_elastic;
    case MU_EASE_OUT_ELASTIC:    return mu_ease_out_elastic;
    case MU_EASE_IN_OUT_ELASTIC: return mu_ease_in_out_elastic;
    case MU_EASE_IN_BOUNCE:      return mu_ease_in_bounce;
    case MU_EASE_OUT_BOUNCE:     return mu_ease_out_bounce;
    case MU_EASE_IN_OUT_BOUNCE:  return mu_ease_in_out_bounce;
    case MU_EASE_SMOOTHSTEP:     return mu_ease_smoothstep;
    case MU_EASE_SMOOTHERSTEP:   return mu_ease_smootherstep;
    case MU_EASE_LINEAR:
    default:                     return mu_ease_linear;
    }
}

/* Stable, lowercase, no-prefix name ("in_out_cubic"). Stable because it is what
 * gets written into an asset file; see mu_ease_from_name. */
MU_INLINE const char *mu_ease_name(mu_ease_kind k)
{
    static const char *const names[MU_EASE_COUNT] = {
        "linear",
        "in_quad", "out_quad", "in_out_quad",
        "in_cubic", "out_cubic", "in_out_cubic",
        "in_quart", "out_quart", "in_out_quart",
        "in_quint", "out_quint", "in_out_quint",
        "in_sine", "out_sine", "in_out_sine",
        "in_expo", "out_expo", "in_out_expo",
        "in_circ", "out_circ", "in_out_circ",
        "in_back", "out_back", "in_out_back",
        "in_elastic", "out_elastic", "in_out_elastic",
        "in_bounce", "out_bounce", "in_out_bounce",
        "smoothstep", "smootherstep"
    };
    if ((int)k < 0 || k >= MU_EASE_COUNT) return "linear";
    return names[k];
}

MU_INLINE int mu_ease__streq(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* Parse a name back into a kind. Accepts the par_easings_* spelling too, so a
 * file written against the old library still loads. Returns MU_EASE_COUNT when
 * the name is unknown - a caller must check, it is not a crash. */
MU_INLINE mu_ease_kind mu_ease_from_name(const char *name)
{
    int k;
    if (!name) return MU_EASE_COUNT;
    for (k = 0; k < (int)MU_EASE_COUNT; ++k)
    {
        const char *n = mu_ease_name((mu_ease_kind)k);
        const char *p = name;
        if (p[0] == 'p' && p[1] == 'a' && p[2] == 'r' && p[3] == '_') p += 4;
        if (p[0] == 'e' && p[1] == 'a' && p[2] == 's' && p[3] == 'i' &&
            p[4] == 'n' && p[5] == 'g' && p[6] == 's' && p[7] == '_') p += 8;
        if (p[0] == 'm' && p[1] == 'u' && p[2] == '_' && p[3] == 'e' &&
            p[4] == 'a' && p[5] == 's' && p[6] == 'e' && p[7] == '_') p += 8;
        if (mu_ease__streq(p, n)) return (mu_ease_kind)k;
    }
    return MU_EASE_COUNT;
}

MU_INLINE float mu_ease_apply(mu_ease_kind k, float t)
{
    return mu_ease_get(k)(t);
}

/* ==========================================================================
 * 5. Velocity: the exact derivative
 * ==========================================================================
 *
 * Chain rules that make 33 curves cost 11 derivatives:
 *     out(t)    = 1 - in(1-t)              -> out'(t)    = +in'(1-t)
 *     in_out(t) = t<.5 ? .5*in(2t) : 1-.5*in(2-2t)
 *                                          -> in_out'(t) = in'(min(2t, 2-2t))
 * Note there is no factor of 2 in in_out': the time compression and the
 * amplitude halving cancel.
 *
 * in_circ's derivative is infinite at t=1 (a vertical tangent, by construction);
 * that is reported as a large finite value rather than inf so it can go into a
 * physics step without poisoning it. */

MU_INLINE float mu_ease__vin_quad(float t) { return 2.0f * t; }
MU_INLINE float mu_ease__vin_cubic(float t) { return 3.0f * t * t; }
MU_INLINE float mu_ease__vin_quart(float t) { return 4.0f * t * t * t; }
MU_INLINE float mu_ease__vin_quint(float t) { float t2 = t * t; return 5.0f * t2 * t2; }
MU_INLINE float mu_ease__vin_sine(float t) { return MU_EASE_HALF_PI * sinf(t * MU_EASE_HALF_PI); }
MU_INLINE float mu_ease__vin_expo(float t) { return 10.0f * MU_EASE_LN2 * exp2f(10.0f * t - 10.0f); }
MU_INLINE float mu_ease__vin_circ(float t)
{
    float u = 1.0f - t * t;
    if (u <= 1e-12f) return 1e12f;              /* vertical tangent at t=1 */
    return t / sqrtf(u);
}
MU_INLINE float mu_ease__vin_back(float t)
{
    const float s = MU_EASE_BACK_S;
    return 3.0f * (s + 1.0f) * t * t - 2.0f * s * t;
}
MU_INLINE float mu_ease__vin_elastic(float t)
{
    /* d/dt [ -2^(10(t-1)) sin(w(t-1-s)) ] */
    const float w = MU_EASE_TAU / MU_EASE_ELASTIC_P;
    const float s = MU_EASE_ELASTIC_P * 0.25f;
    float u = t - 1.0f;
    float e = exp2f(10.0f * u);
    return -(10.0f * MU_EASE_LN2 * e * sinf(w * (u - s)) +
             e * w * cosf(w * (u - s)));
}
MU_INLINE float mu_ease__vin_bounce(float t)
{
    /* in_bounce(t) = 1 - out_bounce(1-t), so in'(t) = +out'(1-t) and out' is the
     * derivative of each parabola: 2*n*(t - arc_origin). */
    const float n = 7.5625f, d = 1.0f / 2.75f;
    float u = 1.0f - t;
    /* the mirror inverts the sign twice (once from 1-f, once from the -t inside),
     * so in'(t) = +out'(1-t) and the arcs keep the sign of their own slope */
    if (u < 1.0f * d) return 2.0f * n * u;
    if (u < 2.0f * d) return 2.0f * n * (u - 1.5f * d);
    if (u < 2.5f * d) return 2.0f * n * (u - 2.25f * d);
    return 2.0f * n * (u - 2.625f * d);
}

/* in-family derivative, indexed by family. */
MU_INLINE float mu_ease__in_velocity(int family, float t)
{
    switch (family)
    {
    case 1:  return mu_ease__vin_quad(t);
    case 2:  return mu_ease__vin_cubic(t);
    case 3:  return mu_ease__vin_quart(t);
    case 4:  return mu_ease__vin_quint(t);
    case 5:  return mu_ease__vin_sine(t);
    case 6:  return mu_ease__vin_expo(t);
    case 7:  return mu_ease__vin_circ(t);
    case 8:  return mu_ease__vin_back(t);
    case 9:  return mu_ease__vin_elastic(t);
    case 10: return mu_ease__vin_bounce(t);
    case 11: return 6.0f * t * (1.0f - t);                      /* smoothstep  */
    case 12: { float u = t - 1.0f; return 30.0f * t * t * u * u; } /* smootherstep */
    default: return 1.0f;                                       /* linear      */
    }
}

/* family + phase for a kind, shared by velocity and lut_error. */
MU_INLINE int  mu_ease__family(mu_ease_kind k) { return ((int)k <= 0) ? 0 : (((int)k - 1) / 3 + 1); }
MU_INLINE int  mu_ease__phase(mu_ease_kind k)  { return ((int)k <= 0) ? 0 : (((int)k - 1) % 3); }

MU_INLINE float mu_ease_velocity(mu_ease_kind k, float t)
{
    int family = mu_ease__family(k);
    int phase = mu_ease__phase(k);            /* 0 = in, 1 = out, 2 = in_out */
    float u = mu_ease_clamp01(t);

    /* the two non-family kinds sit past the 3-per-family block */
    if (k == MU_EASE_SMOOTHSTEP)   return mu_ease__in_velocity(11, u);
    if (k == MU_EASE_SMOOTHERSTEP) return mu_ease__in_velocity(12, u);

    if (phase == 1) u = 1.0f - u;                            /* out mirror */
    else if (phase == 2) u = (u < 0.5f) ? 2.0f * u : 2.0f - 2.0f * u;
    return mu_ease__in_velocity(family, u);
}

/* ==========================================================================
 * 6. Cubic Bezier easing (the curve a designer hands you)
 * ==========================================================================
 *
 * CSS's cubic-bezier(x1,y1,x2,y2) is a cubic with control points (0,0), (x1,y1),
 * (x2,y2), (1,1). The catch is that the *parameter* is the x axis: the curve is
 * defined as y(s), x(s), and you are asked for the y at a given x. That means
 * inverting a cubic, which is where naive implementations either look wrong or
 * hang. Newton converges in 3-4 steps for the curves UI tools emit; the
 * bisection fallback is for the ones they should not (a flat segment through
 * y = const has a vanishing derivative and Newton walks off the interval).
 *
 * x1 and x2 are clamped into [0,1] on the way in: outside that range x(s) is not
 * monotone, there is no function to return, and every real tool enforces it. */

typedef struct mu_ease_bezier
{
    float x1, y1, x2, y2;
} mu_ease_bezier;

MU_INLINE mu_ease_bezier mu_ease_bezier_make(float x1, float y1, float x2, float y2)
{
    mu_ease_bezier b;
    b.x1 = mu_ease_clamp01(x1);
    b.y1 = y1;
    b.x2 = mu_ease_clamp01(x2);
    b.y2 = y2;
    return b;
}

MU_INLINE float mu_ease__bez_cubic(float s, float c1, float c2)
{
    float r = 1.0f - s;
    return 3.0f * r * r * s * c1 + 3.0f * r * s * s * c2 + s * s * s;
}

MU_INLINE float mu_ease__bez_cubic_d(float s, float c1, float c2)
{
    float r = 1.0f - s;
    return 3.0f * r * r * c1 + 6.0f * r * s * (c2 - c1) + 3.0f * s * s * (1.0f - c2);
}

/* s such that x(s) == t. Newton inside the bracket, bisection as the net. */
MU_INLINE float mu_ease__bez_solve(float t, float x1, float x2)
{
    float s = t;
    int i;

    for (i = 0; i < 8; ++i)
    {
        float e = mu_ease__bez_cubic(s, x1, x2) - t;
        float d;
        if (fabsf(e) < 1e-6f) return s;
        d = mu_ease__bez_cubic_d(s, x1, x2);
        if (fabsf(d) < 1e-6f) break;
        {
            float ns = s - e / d;
            if (!(ns >= 0.0f && ns <= 1.0f)) break;
            s = ns;
        }
    }

    {
        float lo = 0.0f, hi = 1.0f;
        for (i = 0; i < 24; ++i)
        {
            s = 0.5f * (lo + hi);
            if (mu_ease__bez_cubic(s, x1, x2) < t) lo = s;
            else hi = s;
        }
    }
    return s;
}

MU_INLINE float mu_ease_bezier_eval(const mu_ease_bezier *b, float t)
{
    float u = mu_ease_clamp01(t);
    if (!b) return u;
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    return mu_ease__bez_cubic(mu_ease__bez_solve(u, b->x1, b->x2), b->y1, b->y2);
}

/* One-shot form, for a call site that has the four numbers and nothing else. */
MU_INLINE float mu_ease_bezier_at(float t, float x1, float y1, float x2, float y2)
{
    mu_ease_bezier b = mu_ease_bezier_make(x1, y1, x2, y2);
    return mu_ease_bezier_eval(&b, t);
}

/* dy/dx = (dy/ds) / (dx/ds). Infinite where dx/ds is zero, i.e. on a vertical
 * segment, which is a real curve a designer can export; reported as a large
 * finite value. */
MU_INLINE float mu_ease_bezier_velocity(const mu_ease_bezier *b, float t)
{
    float u, s, dx, dy;
    if (!b) return 1.0f;
    u = mu_ease_clamp01(t);
    if (u <= 0.0f || u >= 1.0f) return 1.0f;   /* one-sided limit, see docs */
    s = mu_ease__bez_solve(u, b->x1, b->x2);
    dx = mu_ease__bez_cubic_d(s, b->x1, b->x2);
    dy = mu_ease__bez_cubic_d(s, b->y1, b->y2);
    if (fabsf(dx) < 1e-9f) return dy >= 0.0f ? 1e12f : -1e12f;
    return dy / dx;
}

/* The four CSS keywords, as data. Returns 0 for an unknown name. */
MU_INLINE int mu_ease_bezier_css(const char *name, mu_ease_bezier *out)
{
    if (!name || !out) return 0;
    if (mu_ease__streq(name, "ease"))         { *out = mu_ease_bezier_make(0.25f, 0.1f, 0.25f, 1.0f); return 1; }
    if (mu_ease__streq(name, "ease-in"))      { *out = mu_ease_bezier_make(0.42f, 0.0f, 1.0f, 1.0f);  return 1; }
    if (mu_ease__streq(name, "ease-out"))     { *out = mu_ease_bezier_make(0.0f, 0.0f, 0.58f, 1.0f);  return 1; }
    if (mu_ease__streq(name, "ease-in-out"))  { *out = mu_ease_bezier_make(0.42f, 0.0f, 0.58f, 1.0f); return 1; }
    if (mu_ease__streq(name, "linear"))       { *out = mu_ease_bezier_make(0.0f, 0.0f, 1.0f, 1.0f);   return 1; }
    return 0;
}

/* ==========================================================================
 * 7. Monotone cubic spline through control points
 * ==========================================================================
 *
 * This is the "here is my curve" workflow: a designer places keys, you evaluate
 * them. A Catmull-Rom or a plain cubic Hermite would be shorter, and it would
 * overshoot - a curve through (0,0.9) and (0.9,1) can dip below 0 with
 * Catmull-Rom tangents, which in a UI means a panel that flies off the edge of
 * the screen. Fritsch-Carlson fixes the tangents so that the interpolant is
 * monotone wherever the data is monotone: no overshoot, ever.
 *
 *     mu_ease_key keys[] = { {0.0f,0.0f}, {0.3f,0.8f}, {1.0f,1.0f} };
 *     float slopes[3];
 *     mu_ease_spline sp;
 *     mu_ease_spline_build(&sp, keys, 3, slopes);
 *     float y = mu_ease_spline_eval(&sp, 0.5f);
 *
 * `slopes` is caller-owned scratch, exactly `count` floats: the header does not
 * allocate. `keys` must outlive `sp` (the struct points at them). */

typedef struct mu_ease_key
{
    float t;
    float v;
} mu_ease_key;

typedef struct mu_ease_spline
{
    const mu_ease_key *keys;
    int                count;
    float             *slopes;
} mu_ease_spline;

MU_INLINE int mu_ease_spline_build(mu_ease_spline *sp, const mu_ease_key *keys, int count,
                                   float *slopes)
{
    int i;

    if (!sp) return 0;
    /* a failed build leaves an EMPTY spline, not a half-built one: every consumer
     * can call eval on it and get the clamped identity instead of reading stack */
    sp->keys = NULL;
    sp->count = 0;
    sp->slopes = NULL;

    if (!keys || !slopes || count < 2) return 0;
    for (i = 1; i < count; ++i)
        if (!(keys[i].t > keys[i - 1].t)) return 0;     /* must be strictly increasing */

    sp->keys = keys;
    sp->count = count;
    sp->slopes = slopes;

    /* secants as a first guess */
    if (count == 2)
    {
        float d = (keys[1].v - keys[0].v) / (keys[1].t - keys[0].t);
        slopes[0] = d;
        slopes[1] = d;
        return 1;
    }

    for (i = 0; i < count - 1; ++i)
        slopes[i] = (keys[i + 1].v - keys[i].v) / (keys[i + 1].t - keys[i].t);
    /* interior tangents: average of the neighbouring secants, in place */
    for (i = count - 1; i >= 1; --i)
        slopes[i] = (i == count - 1) ? slopes[count - 2]
                                     : 0.5f * (slopes[i - 1] + slopes[i]);

    /* Fritsch-Carlson limiter: keep (alpha, beta) inside the monotonicity circle */
    for (i = 0; i < count - 1; ++i)
    {
        float d = (keys[i + 1].v - keys[i].v) / (keys[i + 1].t - keys[i].t);

        if (d == 0.0f)
        {
            slopes[i] = 0.0f;
            slopes[i + 1] = 0.0f;
        }
        else
        {
            float a = slopes[i] / d;
            float b = slopes[i + 1] / d;
            float m = a * a + b * b;

            if (m > 9.0f)
            {
                float tau = 3.0f / sqrtf(m);
                slopes[i] = tau * a * d;
                slopes[i + 1] = tau * b * d;
            }
        }
    }
    return 1;
}

/* Segment index for a time inside the key range. Binary search above 8 keys,
 * linear below: a UI curve has a handful. */
MU_INLINE int mu_ease__spline_seg(const mu_ease_spline *sp, float t)
{
    int lo = 0, hi = sp->count - 1;
    if (sp->count <= 8)
    {
        int i;
        for (i = 0; i < sp->count - 2; ++i)
            if (t < sp->keys[i + 1].t) return i;
        return sp->count - 2;
    }
    while (hi - lo > 1)
    {
        int mid = (lo + hi) >> 1;
        if (sp->keys[mid].t <= t) lo = mid;
        else hi = mid;
    }
    return lo;
}

MU_INLINE float mu_ease_spline_eval(const mu_ease_spline *sp, float t)
{
    int i;
    float h, u, u2, u3, v0, v1, m0, m1;

    if (!sp || !sp->keys || sp->count < 2) return mu_ease_clamp01(t);
    if (t != t) return 0.0f;
    if (t <= sp->keys[0].t) return sp->keys[0].v;
    if (t >= sp->keys[sp->count - 1].t) return sp->keys[sp->count - 1].v;

    i = mu_ease__spline_seg(sp, t);
    h = sp->keys[i + 1].t - sp->keys[i].t;
    u = (t - sp->keys[i].t) / h;
    u2 = u * u;
    u3 = u2 * u;
    v0 = sp->keys[i].v;
    v1 = sp->keys[i + 1].v;
    m0 = sp->slopes[i] * h;
    m1 = sp->slopes[i + 1] * h;

    return (2.0f * u3 - 3.0f * u2 + 1.0f) * v0 + (u3 - 2.0f * u2 + u) * m0 +
           (-2.0f * u3 + 3.0f * u2) * v1 + (u3 - u2) * m1;
}

MU_INLINE float mu_ease_spline_velocity(const mu_ease_spline *sp, float t)
{
    int i;
    float h, u, v0, v1, m0, m1;

    if (!sp || !sp->keys || sp->count < 2) return 1.0f;
    if (t != t) return 0.0f;
    /* outside the keys: no extrapolation, so no slope. At the keys themselves the
     * one-sided limit is the stored tangent, and seg() below returns it. */
    if (t < sp->keys[0].t || t > sp->keys[sp->count - 1].t) return 0.0f;

    i = mu_ease__spline_seg(sp, t);
    h = sp->keys[i + 1].t - sp->keys[i].t;
    u = (t - sp->keys[i].t) / h;
    v0 = sp->keys[i].v;
    v1 = sp->keys[i + 1].v;
    m0 = sp->slopes[i] * h;
    m1 = sp->slopes[i + 1] * h;

    return ((6.0f * u * u - 6.0f * u) * v0 + (3.0f * u * u - 4.0f * u + 1.0f) * m0 +
            (-6.0f * u * u + 6.0f * u) * v1 + (3.0f * u * u - 2.0f * u) * m1) / h;
}

/* ==========================================================================
 * 8. Discrete steps
 * ==========================================================================
 *
 * steps(4) is what a health bar tick or a sprite-sheet flip wants, and no
 * continuous curve gives it. jump_at_start = 0 is CSS's default (jump-end). */

MU_INLINE float mu_ease_steps(float t, int count, int jump_at_start)
{
    float u = mu_ease_clamp01(t);
    float x;

    if (count < 1) return u;
    if (u >= 1.0f) return 1.0f;
    x = u * (float)count;
    x = jump_at_start ? ceilf(x) : floorf(x);
    if (x > (float)count) x = (float)count;
    return x / (float)count;
}

/* ==========================================================================
 * 9. Baked lookup tables
 * ==========================================================================
 *
 * An out_elastic call costs 11.4 ns on this box; the same curve in a 257-entry
 * table costs 3.11 ns (both measured, 64-value payloads, PERF_EASING.md). For a
 * particle system doing 100k eased interpolations per frame that is 1.14 ms
 * against 0.31 ms. A cubic bezier is 33 ns a call, so for it the table wins by
 * an order of magnitude.
 *
 * The table is CALLER-OWNED. `samples` is the number of entries, so the value at
 * t=1 is table[samples-1] and the internal span count is samples-1. 257 is the
 * usual choice: 256 spans of 1/256, and one extra entry so the top of the range
 * does not need a special case.
 *
 * The crossover is not universal, and saying so matters: out_quad is 1.97 ns a
 * call and the same table lookup is 3.11 ns, so baking a quadratic makes it
 * SLOWER. Bake what is expensive (elastic, bezier, spline, and sine/expo at a
 * stretch); call the function for the polynomial families and for bounce, which
 * is 1.88 ns - four unpredictable branches that modern predictors get right.
 * Measured table in perf_test/PERF_EASING.md. */

typedef struct mu_ease_lut
{
    float     *table;
    int        samples;
    mu_ease_fn fn;          /* what it was baked from; used by lut_error */
} mu_ease_lut;

MU_INLINE int mu_ease_lut_bake(mu_ease_lut *lut, float *table, int samples, mu_ease_fn fn)
{
    int i;

    if (!lut) return 0;
    lut->table = NULL;          /* a failed bake leaves an empty, safe lut */
    lut->samples = 0;
    lut->fn = NULL;
    if (!table || !fn || samples < 2) return 0;
    for (i = 0; i < samples; ++i)
        table[i] = fn((float)i / (float)(samples - 1));
    /* note what is NOT done here: the ends are not forced to 0 and 1. A table
     * baked from a spline through keys at 0.2..0.9 must reproduce the spline, not
     * a normalized curve, or lut_error would be measuring the wrong thing. */
    lut->table = table;
    lut->samples = samples;
    lut->fn = fn;
    return 1;
}

MU_INLINE int mu_ease_lut_bake_kind(mu_ease_lut *lut, float *table, int samples, mu_ease_kind k)
{
    return mu_ease_lut_bake(lut, table, samples, mu_ease_get(k));
}

MU_INLINE float mu_ease_lut_eval(const mu_ease_lut *lut, float t)
{
    float u, x, a, b;
    int i, last;

    if (!lut || !lut->table || lut->samples < 2) return mu_ease_clamp01(t);
    u = mu_ease_clamp01(t);
    last = lut->samples - 1;
    x = u * (float)last;
    i = (int)x;
    if (i >= last) i = last - 1;
    if (i < 0) i = 0;
    a = lut->table[i];
    b = lut->table[i + 1];
    return a + (b - a) * (x - (float)i);
}

/* Slope of the piecewise-linear table: what a physics step or a motion-blur
 * shutter should use when it samples a baked curve. */
MU_INLINE float mu_ease_lut_velocity(const mu_ease_lut *lut, float t)
{
    float u, x;
    int i, last;

    if (!lut || !lut->table || lut->samples < 2) return 1.0f;
    u = mu_ease_clamp01(t);
    last = lut->samples - 1;
    x = u * (float)last;
    i = (int)x;
    if (i >= last) i = last - 1;
    if (i < 0) i = 0;
    return (lut->table[i + 1] - lut->table[i]) * (float)last;
}

/* Worst-case absolute error of the table against the function it came from,
 * sampled at `probes` points. This is how you pick the sample count instead of
 * guessing: bake at 33, 65, 129, 257 and look. Returns -1 if there is nothing to
 * compare against. */
MU_INLINE float mu_ease_lut_error(const mu_ease_lut *lut, int probes)
{
    float worst = 0.0f;
    int i;

    if (!lut || !lut->fn || !lut->table || lut->samples < 2 || probes < 2) return -1.0f;
    for (i = 0; i < probes; ++i)
    {
        float t = (float)i / (float)(probes - 1);
        float e = fabsf(mu_ease_lut_eval(lut, t) - lut->fn(t));
        if (e > worst) worst = e;
    }
    return worst;
}

/* Eight lanes of the same table. This is the particle path: bake once, then
 * evaluate a whole batch.
 *
 * It is written as eight independent scalar lookups, and that is a measurement,
 * not an omission: an AVX2 gather (_mm256_i32gather_ps with a converted index
 * vector) was tried and came out at the same speed - a gather costs about what
 * eight L1 loads plus the float-to-int conversion cost, so there is nothing to
 * win. What the batch form actually buys is a 1.1x saving on call overhead
 * (2.84 vs 3.11 ns per value, PERF_EASING.md), and the ILP that lets eight
 * independent chains overlap. Anything more would need the table in a form that
 * can be indexed without a gather, i.e. a different data structure. */
MU_INLINE mu_f32x8 mu_ease_lut_eval8(const mu_ease_lut *lut, mu_f32x8 t)
{
    mu_f32x8 r;
    int lane;

    for (lane = 0; lane < 8; ++lane)
        r.m[lane] = mu_ease_lut_eval(lut, t.m[lane]);
    return r;
}

/* ==========================================================================
 * 10. Time helpers and application
 * ========================================================================== */

/* Wrap any real time into [0,1): a looping tween. */
MU_INLINE float mu_ease_loop(float t)
{
    if (t != t) return 0.0f;
    t = fmodf(t, 1.0f);
    return t < 0.0f ? t + 1.0f : t;
}

/* 0 -> 1 -> 0 over each period of 2: a ping-pong tween. */
MU_INLINE float mu_ease_pingpong(float t)
{
    float u;
    if (t != t) return 0.0f;
    u = fmodf(fabsf(t), 2.0f);
    return u <= 1.0f ? u : 2.0f - u;
}

MU_INLINE float mu_ease_remap(float t, float lo, float hi)
{
    if (!(hi > lo)) return 0.0f;
    return mu_ease_clamp01((t - lo) / (hi - lo));
}

/* The two calls 90% of real code makes. */
MU_INLINE float mu_ease_lerp(mu_ease_kind k, float a, float b, float t)
{
    return a + (b - a) * mu_ease_apply(k, t);
}

MU_INLINE mu_vec3 mu_ease_vec3(mu_ease_kind k, mu_vec3 a, mu_vec3 b, float t)
{
    float e = mu_ease_apply(k, t);
    return mu_vec3_make(a.x + (b.x - a.x) * e,
                        a.y + (b.y - a.y) * e,
                        a.z + (b.z - a.z) * e);
}

/* ==========================================================================
 * 11. The velocity table, for batching
 * ==========================================================================
 *
 * mu_ease_velocity is a switch over the family; when a caller wants velocity for
 * a whole batch it is worth resolving the family once. These two helpers expose
 * the split without exposing the internals. */

MU_INLINE float mu_ease_velocity_family(int family, int phase, float t)
{
    float u = mu_ease_clamp01(t);

    if (phase == 1) u = 1.0f - u;
    else if (phase == 2) u = (u < 0.5f) ? 2.0f * u : 2.0f - 2.0f * u;
    return mu_ease__in_velocity(family, u);
}

MU_END_EXTERN_C

#endif /* MU_EASING_H */
