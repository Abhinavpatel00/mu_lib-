
#pragma once

#include "mu_common.h"

#include <stdlib.h>
#include <math.h>

/*

    This function generates Gaussian (Normal) distributed random numbers
    using the Box-Muller Transform.

    WHY THIS EXISTS:
    ----------------
    Computers only give you uniform randomness (flat distribution).
    Real-world phenomena follow Gaussian distribution (bell curve).

    So we transform uniform → Gaussian.

    THEORY IN SHORT:
    ----------------
    If:
        u1, u2 ~ Uniform(0,1)

    Then:
        z0 = sqrt(-2 ln(u1)) * cos(2π u2)
        z1 = sqrt(-2 ln(u1)) * sin(2π u2)

    gives:
        z0, z1 ~ Normal(0,1)

    WHY THIS WORKS:
    ----------------
    - Think of random points inside a unit square.
    - Convert them into polar coordinates.
    - Warp the radius using logarithm.
    - Boom: probability density reshaped into a bell curve.

    Yeah, math basically bends space here.

    PERFORMANCE TRICK:
    ------------------
    Box-Muller gives TWO values (z0, z1),
    so we cache one instead of wasting it like a careless dev.
proof

We want:

z ~ N(0,1)

Which means probability density:

f(z) = (1 / √(2π)) * e^(-z² / 2)

But instead, we only have this sad little toy:

u ~ Uniform(0,1)

Flat. Emotionless. Like your productivity at 3 AM.

Think in 2D (this is where people mess up)

Instead of generating one Gaussian, we generate two at once:

z0, z1 ~ N(0,1)

Joint distribution:

f(z0, z1) = (1 / 2π) * e^(-(z0² + z1²)/2)

Notice something?

Notice something?

z0² + z1² = r²

This screams:

👉 “Switch to polar coordinates, you coward.”


z0 = r cos(θ)
z1 = r sin(θ)
f(r, θ) = (1 / 2π) * e^(-r²/2) * r
f(r, θ) = [ (1 / 2π) ] * [ r e^(-r²/2) ]
θ ~ Uniform(0, 2π)
r ~ distribution with PDF: r e^(-r²/2)
p(r) = r e^(-r²/2)
F(r) = ∫₀^r s e^(-s²/2) ds
t = -s²/2
dt = -s ds
F(r) = 1 - e^(-r²/2)
u1 = F(r)

u1 = 1 - e^(-r²/2)
-r²/2 = ln(1 - u1)
r² = -2 ln(1 - u1)
Since 1 - u1 is still uniform:
r = sqrt(-2 ln(u1))


	*/

/*
    Start: Uniform square

        u1, u2 ∈ (0,1)

        +------------------+
        |       •          |
        |   •         •    |
        |        •         |
        +------------------+

    Step 1: Interpret as polar

        u2 → angle (θ)
        u1 → radius (but warped via ln)

    Step 2: Warp radius

        r = sqrt(-2 ln(u1))

        WHY?

        Because Gaussian density decays like:
            e^(-r²/2)

        So we "stretch" inner points and
        compress outer ones.

    Step 3: Map to circle

            y
            ^
        *         *
      *             *
     *       •       *
      *             *
        *         *
            +-----> x

    Step 4: Project

        z0 = r cos(θ)
        z1 = r sin(θ)

    Result:

        Points cluster near center
        → Gaussian distribution
*/

typedef struct {
    int hasSpare;
    double spare;
} gaussian_state_t;

MU_INLINE double random_gaussian_state(gaussian_state_t *state, double mean, double stddev)
{
    if (state->hasSpare)
    {
        state->hasSpare = 0;
        return mean + stddev * state->spare;
    }

    double u1, u2;

    do {
        u1 = (double)rand() / (double)RAND_MAX;
    } while (u1 <= 1e-12);

    u2 = (double)rand() / (double)RAND_MAX;

    double r     = sqrt(-2.0 * log(u1));
    double theta = 2.0 * MU_PI * u2;

    double z0 = r * cos(theta);
    double z1 = r * sin(theta);

    state->spare = z1;
    state->hasSpare = 1;

    return mean + stddev * z0;
}




