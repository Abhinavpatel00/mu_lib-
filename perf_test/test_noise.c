/* test_noise.c -- gate for mu_noise_math.h.
 *
 * The assertions that a procedural noise library actually needs:
 *
 *   1. range and finiteness. Every sampler is sampled on a grid; a NaN here is
 *      a division by zero or an out-of-bounds hash, and an out-of-range value is
 *      a normalisation constant that drifted.
 *   2. determinism, and seed sensitivity: same seed => same float, different
 *      seed => different field.
 *   3. continuity: |n(p+h) - n(p)| must be small for small h. This is what
 *      catches a hash that lands on the wrong lattice cell (a seam).
 *   4. analytic derivatives agree with central differences. This is the one
 *      check that proves the derivative code is the derivative of *this*
 *      function and not of some other noise.
 *   5. identity cases: flow noise at angle 0 must reduce to plain simplex, and
 *      cellular noise with a huge falloff must converge to F1.
 *   6. the vector types are mu_math's types (interop), not private structs.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "mu_noise_math.h"

static int g_checks, g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) { ++g_fails; if (g_fails <= 25) printf("  FAIL %s\n", what); }
}

static void check_near(float got, float want, float tol, const char *what)
{
    ++g_checks;
    if (!(fabsf(got - want) <= tol))
    {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL %s (got %.6f want %.6f)\n", what, got, want);
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

#define GRID 37

static int finite_f(float v) { return (v == v) && (v > -1e30f) && (v < 1e30f); }

/* Sample a 1D/2D/3D/4D family over the grid, requiring finite results and a
 * range. Returns min/max for the caller's own assertions. */
static void scan3(float (*fn)(float, float, float, uint32_t),
                  float limit, const char *name, float *out_min, float *out_max)
#define SCAN3(fn, lim, name) scan3(fn, lim, name, NULL, NULL)
{
    float mn = 1e30f, mx = -1e30f;
    int bad = 0;

    for (int j = 0; j < GRID; ++j)
    {
        for (int i = 0; i < GRID; ++i)
        {
            float v = fn(i * 0.37f + 0.11f, j * 0.41f - 0.07f, i * 0.13f - j * 0.19f, 17u);
            if (!finite_f(v)) { ++bad; continue; }
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }
    ++g_checks;
    if (bad) { ++g_fails; if (g_fails <= 25) printf("  FAIL %s: %d non-finite samples\n", name, bad); }
    ++g_checks;
    if (mn < -limit || mx > limit)
    {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL %s: range [%.3f, %.3f] outside +-%.3f\n", name, mn, mx, limit);
    }
    if (out_min) *out_min = mn;
    if (out_max) *out_max = mx;
}

static void scan2(float (*fn)(float, float, uint32_t),
                  float limit, const char *name)
{
    float mn = 1e30f, mx = -1e30f;
    int bad = 0;

    for (int j = 0; j < GRID; ++j)
    {
        for (int i = 0; i < GRID; ++i)
        {
            float v = fn(i * 0.37f + 0.11f, j * 0.41f - 0.07f, 17u);
            if (!finite_f(v)) { ++bad; continue; }
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }
    ++g_checks;
    if (bad) { ++g_fails; if (g_fails <= 25) printf("  FAIL %s: %d non-finite samples\n", name, bad); }
    ++g_checks;
    if (mn < -limit || mx > limit)
    {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL %s: range [%.3f, %.3f] outside +-%.3f\n", name, mn, mx, limit);
    }
}

/* ------------------------------------------------------------------ */
/* 1. range / finiteness of the families                              */
/* ------------------------------------------------------------------ */

static float w2_wrap(float x, float y, uint32_t s) { return mu_noise_worley2_f1(x, y, s, MU_NOISE_DIST_EUCLIDEAN); }
static float w2_soft(float x, float y, uint32_t s) { return mu_noise_worley2_falloff(x, y, s, 2.5f); }
static float iq2_wrap(float x, float y, uint32_t s) { return mu_noise_iq_fbm2(mu_noise_simplex2_deriv, x, y, s, 4); }
static float iqr2_wrap(float x, float y, uint32_t s)
{
    return mu_noise_iq_rot_fbm2(mu_noise_simplex2, x, y, s, 4, 2.0f, 0.6f, 0.5f);
}
static float flow2_wrap(float x, float y, uint32_t s) { return mu_noise_flow2(x, y, 0.7f, s); }
static float flow3_wrap(float x, float y, float z, uint32_t s) { return mu_noise_flow3(x, y, z, 0.7f, s); }
static float simplexfbm2_wrap(float x, float y, uint32_t s)
{
    return mu_noise_fbm2(mu_noise_simplex2, x, y, s, 5, 2.0f, 0.5f);
}

static void test_ranges(void)
{
    float mn, mx;

    scan2(mu_noise_simplex2, 1.0f, "simplex2");
    SCAN3(mu_noise_simplex3, 1.0f, "simplex3");
    scan2(mu_noise_perlin2, 1.1f, "perlin2");
    SCAN3(mu_noise_perlin3, 1.1f, "perlin3");
    scan2(simplexfbm2_wrap, 1.0f, "fbm2(simplex)");
    scan2(flow2_wrap, 1.0f, "flow2");
    SCAN3(flow3_wrap, 1.0f, "flow3");
    scan2(iq2_wrap, 1.0f, "iq_fbm2");
    scan2(iqr2_wrap, 1.0f, "iq_rot_fbm2");
    scan2(w2_wrap, 4.0f, "worley2_f1");
    scan2(w2_soft, 8.0f, "worley2_falloff");

    /* 1D simplex is the new family member: check it is finite and bounded, and
     * that it actually uses the domain (a constant would pass the bound). */
    {
        float lo = 1e30f, hi = -1e30f;
        for (int i = 0; i < 4096; ++i)
        {
            float x = i * 0.013f - 20.0f;
            float v = mu_noise_simplex1(x, 5u);
            check(finite_f(v), "simplex1: finite");
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        check(lo < -0.2f && hi > 0.2f, "simplex1: uses a useful part of [-1,1]");
        check(lo >= -1.0001f && hi <= 1.0001f, "simplex1: inside [-1,1]");
    }

    /* 4D family members */
    {
        int bad = 0;
        float lo = 1e30f, hi = -1e30f;
        for (int t = 0; t < 512; ++t)
        {
            float x = t * 0.17f, y = t * 0.29f, z = t * 0.07f, w = t * 0.11f;
            float v = mu_noise_fbm4(mu_noise_simplex4, x, y, z, w, 3u, 4, 2.0f, 0.5f);
            float u = mu_noise_turbulence4(mu_noise_simplex4, x, y, z, w, 3u, 4, 2.0f, 0.5f);
            float b = mu_noise_billow4(mu_noise_simplex4, x, y, z, w, 3u, 4, 2.0f, 0.5f);
            float r = mu_noise_ridged4(mu_noise_simplex4, x, y, z, w, 3u, 4, 2.0f, 0.5f, 1.0f);
            if (!finite_f(v) || !finite_f(u) || !finite_f(b) || !finite_f(r)) ++bad;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        check(bad == 0, "fbm4 family: all finite");
        check(lo >= -1.0001f && hi <= 1.0001f, "fbm4: inside [-1,1]");
        (void)mn; (void)mx;
    }
}

/* ------------------------------------------------------------------ */
/* 2. determinism and seed sensitivity                                */
/* ------------------------------------------------------------------ */

static void test_determinism(void)
{
    for (int i = 0; i < 64; ++i)
    {
        float x = i * 0.27f, y = i * 0.13f;
        check(mu_noise_simplex2(x, y, 9u) == mu_noise_simplex2(x, y, 9u), "simplex2 deterministic");
        check(mu_noise_simplex1(x, 9u) == mu_noise_simplex1(x, 9u), "simplex1 deterministic");
        check(mu_noise_flow2(x, y, 0.3f, 9u) == mu_noise_flow2(x, y, 0.3f, 9u), "flow2 deterministic");
    }

    /* different seed must change the field */
    {
        int differ = 0;
        for (int i = 0; i < 64; ++i)
            if (mu_noise_simplex2(i * 0.31f, i * 0.19f, 1u) != mu_noise_simplex2(i * 0.31f, i * 0.19f, 2u))
                ++differ;
        check(differ > 60, "simplex2: seed changes the field");
    }
}

/* ------------------------------------------------------------------ */
/* 3. continuity                                                      */
/* ------------------------------------------------------------------ */

static void test_continuity(void)
{
    const float h = 1e-4f;
    float worst = 0.0f;

    for (int i = 0; i < 4096; ++i)
    {
        float x = i * 0.07f - 30.0f;
        float y = i * 0.11f - 30.0f;

        float a = mu_noise_simplex1(x, 3u);
        float b = mu_noise_simplex1(x + h, 3u);
        if (fabsf(b - a) > worst) worst = fabsf(b - a);

        if (fabsf(mu_noise_flow2(x, y, 0.9f, 3u) - mu_noise_flow2(x + h, y, 0.9f, 3u)) > worst)
            worst = fabsf(mu_noise_flow2(x, y, 0.9f, 3u) - mu_noise_flow2(x + h, y, 0.9f, 3u));
    }
    /* A step of 1e-4 in a field whose slope is order 10 changes it by order
     * 1e-3; anything near 1 would be a lattice seam. */
    check(worst < 5e-3f, "simplex1/flow2: no seams (step of 1e-4 changes < 5e-3)");

    /* Across the 1D cell boundary explicitly, where a bad hash shows up. */
    {
        float e = 1e-3f;
        float lo = mu_noise_simplex1(3.0f - e, 77u);
        float at = mu_noise_simplex1(3.0f, 77u);
        float hi = mu_noise_simplex1(3.0f + e, 77u);
        check(fabsf(at - lo) < 5e-2f, "simplex1: continuous across the cell boundary (left)");
        check(fabsf(hi - at) < 5e-2f, "simplex1: continuous across the cell boundary (right)");
    }
}

/* ------------------------------------------------------------------ */
/* 4. derivatives vs central differences                              */
/* ------------------------------------------------------------------ */

static void test_derivatives(void)
{
    const float e = 1e-3f;

    /* 1D analytic derivative */
    for (int i = 0; i < 128; ++i)
    {
        float x = i * 0.31f - 10.0f;
        mu_noise_sample2 s = mu_noise_simplex1_deriv(x, 21u);
        float fd = (mu_noise_simplex1(x + e, 21u) - mu_noise_simplex1(x - e, 21u)) / (2.0f * e);
        if (fabsf(fd - s.gradient.x) > 2e-2f)
        {
            ++g_fails;
            if (g_fails <= 25) printf("  FAIL simplex1_deriv at x=%.3f (analytic %.4f, fd %.4f)\n",
                                      x, s.gradient.x, fd);
        }
        ++g_checks;
        check_near(s.gradient.y, 0.0f, 1e-9f, "simplex1_deriv: no y derivative");
    }

    /* 2D/3D simplex derivatives */
    {
        mu_noise_sample2 s = mu_noise_simplex2_deriv(1.7f, -2.3f, 4u);
        float fx = (mu_noise_simplex2(1.7f + e, -2.3f, 4u) - mu_noise_simplex2(1.7f - e, -2.3f, 4u)) / (2.0f * e);
        float fy = (mu_noise_simplex2(1.7f, -2.3f + e, 4u) - mu_noise_simplex2(1.7f, -2.3f - e, 4u)) / (2.0f * e);
        check_near(s.gradient.x, fx, 5e-2f, "simplex2_deriv: dx matches central difference");
        check_near(s.gradient.y, fy, 5e-2f, "simplex2_deriv: dy matches central difference");
    }

    /* dfbm2 must be the derivative of the fbm sum it returns */
    {
        float x = 0.31f, y = -1.42f;
        mu_noise_sample2 d = mu_noise_dfbm2(mu_noise_simplex2_deriv, x, y, 8u, 4, 2.0f, 0.5f);
        float v0 = mu_noise_fbm2(mu_noise_simplex2, x, y, 8u, 4, 2.0f, 0.5f);
        check_near(d.value, v0, 1e-4f, "dfbm2: value equals fbm2 of the same base");

        float fx = (mu_noise_fbm2(mu_noise_simplex2, x + e, y, 8u, 4, 2.0f, 0.5f) -
                    mu_noise_fbm2(mu_noise_simplex2, x - e, y, 8u, 4, 2.0f, 0.5f)) / (2.0f * e);
        check_near(d.gradient.x, fx, 0.2f, "dfbm2: dx matches central difference of fbm2");
    }

    /* dfbm3: value consistency only (the 3D chain rule is the same code) */
    {
        float x = 0.13f, y = 0.71f, z = -0.29f;
        mu_noise_sample3 d = mu_noise_dfbm3(mu_noise_simplex3_deriv, x, y, z, 8u, 3, 2.0f, 0.5f);
        float v0 = mu_noise_fbm3(mu_noise_simplex3, x, y, z, 8u, 3, 2.0f, 0.5f);
        check_near(d.value, v0, 1e-4f, "dfbm3: value equals fbm3 of the same base");
        check(finite_f(d.gradient.x) && finite_f(d.gradient.y) && finite_f(d.gradient.z),
              "dfbm3: gradient finite");
    }
}

/* ------------------------------------------------------------------ */
/* 5. identity cases                                                  */
/* ------------------------------------------------------------------ */

static void test_identities(void)
{
    /* flow noise at angle 0 is exactly plain simplex: the rotation is identity,
     * so this checks the ported code against the implementation it was derived
     * from, bit for bit. */
    {
        float worst = 0.0f;
        for (int i = 0; i < 512; ++i)
        {
            float x = i * 0.19f - 20.0f, y = i * 0.23f + 3.0f;
            float a = mu_noise_simplex2(x, y, 12u);
            float b = mu_noise_flow2(x, y, 0.0f, 12u);
            float d = fabsf(a - b);
            if (d > worst) worst = d;
        }
        check(worst == 0.0f, "flow2 at angle 0 is exactly simplex2");
    }

    {
        float worst = 0.0f;
        for (int i = 0; i < 512; ++i)
        {
            float x = i * 0.19f - 20.0f, y = i * 0.23f + 3.0f, z = i * 0.07f - 1.0f;
            float a = mu_noise_simplex3(x, y, z, 12u);
            float b = mu_noise_flow3(x, y, z, 0.0f, 12u);
            float d = fabsf(a - b);
            if (d > worst) worst = d;
        }
        check(worst < 1e-5f, "flow3 at angle 0 equals simplex3");
    }

    /* Rotating every gradient by pi negates them, so the field must negate
     * exactly. (Rotating by pi/2 is NOT the same as rotating the *input*
     * coordinate: the lattice does not rotate with the gradients, so any such
     * test would be asserting a false identity.) */
    {
        float x = 1.3f, y = -0.7f;
        float a = mu_noise_flow2(x, y, 3.14159265f, 30u);
        float b = mu_noise_simplex2(x, y, 30u);
        check_near(a, -b, 1e-5f, "flow2: rotating gradients by pi negates the field");
        check(fabsf(mu_noise_flow2(x, y, 1.5707963f, 30u) - b) > 1e-3f,
              "flow2: a quarter turn actually changes the field");
    }

    /* cellular falloff -> F1 as the falloff grows */
    {
        float x = 3.37f, y = -8.11f;
        float f1 = mu_noise_worley2_f1(x, y, 6u, MU_NOISE_DIST_EUCLIDEAN);
        float soft = mu_noise_worley2_falloff(x, y, 6u, 60.0f);
        check_near(soft, f1, 0.06f, "worley2_falloff converges to F1 at high falloff");
        check(mu_noise_worley2_falloff(x, y, 6u, 0.5f) > soft, "worley2_falloff: low falloff is larger (softer)");
    }

    /* iq_rot_fbm at angle 0, scale 1, one octave degenerates to the base */
    {
        float x = 0.4f, y = 0.9f;
        float a = mu_noise_iq_rot_fbm2(mu_noise_simplex2, x, y, 2u, 1, 1.0f, 0.0f, 0.5f);
        float b = mu_noise_simplex2(x, y, 2u);
        check_near(a, b, 1e-5f, "iq_rot_fbm2: one octave at scale 1/angle 0 is the base noise");
    }
}

/* ------------------------------------------------------------------ */
/* 6. interop with mu_math                                            */
/* ------------------------------------------------------------------ */

static void test_interop(void)
{
    /* The noise vector types must BE the mu_math types. If someone re-adds a
     * private struct this fails to compile, which is the point. */
    mu_vec2 v2 = mu_noise_v2(3.0f, 4.0f);
    mu_vec3 v3 = mu_noise_v3(1.0f, 2.0f, 2.0f);
    mu_vec4 v4 = mu_noise_v4(1.0f, 0.0f, 0.0f, 0.0f);

    check_near(mu_vec2_length(v2), 5.0f, 1e-6f, "interop: mu_vec2_length on a noise vec2");
    check_near(mu_vec3_length(v3), 3.0f, 1e-6f, "interop: mu_vec3_length on a noise vec3");
    check_near(mu_vec4_length(v4), 1.0f, 1e-6f, "interop: mu_vec4_length on a noise vec4");

    {
        mu_vec3 n = mu_vec3_normalize(mu_noise_grad3_from_hash(5u));
        check_near(mu_vec3_length(n), 1.0f, 1e-5f, "interop: normalising a noise gradient works");
    }

    check(sizeof(mu_noise_vec2) == sizeof(mu_vec2), "interop: vec2 sizes match");
    check(sizeof(mu_noise_vec3) == sizeof(mu_vec3), "interop: vec3 sizes match");
    check(sizeof(mu_noise_vec4) == sizeof(mu_vec4), "interop: vec4 sizes match");
}

int main(void)
{
    printf("mu_noise_math correctness gate\n");
    printf("----------------------------------------------------------\n");
    test_ranges();
    test_determinism();
    test_continuity();
    test_derivatives();
    test_identities();
    test_interop();
    printf("----------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
