#pragma once

#include "mu_common.h"
/*

We sometimes need the raw bit pattern of a float. Not its value. Its binary representation.

Example float:

1.0f

Binary inside memory:

0 01111111 00000000000000000000000
^ ^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^
s exponent mantissa

But in C, if you do this:

uint32_t x = (uint32_t)f;

you are converting the number, not reading its bits.

Example:

float 1.5 → integer 1

The bit pattern is lost.

We need the exact bits, because float compression works by manipulating the exponent and mantissa directly.
So we use a union.

union {
    float    f;
    uint32_t u;
};

Both variables occupy the same memory.

Visualization:

memory (4 bytes)

[ byte ][ byte ][ byte ][ byte ]
     ↑ same memory

float view
uint32 view

So this works:

mu_float_bits fb;
fb.f = 1.0f;

printf("%u\n", fb.u);

No conversion happens.
We simply reinterpret the bits.

 C also allows memcpy for this.

*/

/*
===============================================================================
Mu Quantization Utilities
-------------------------------------------------------------------------------
Tools for compressing float data for rendering.

Includes:
    float -> half conversion
    half  -> float conversion
    float precision reduction
    UNORM quantization
    SNORM quantization

Used for:
    vertex packing
    texture coordinates
    normals
    GPU bandwidth reduction
===============================================================================
*/

/*
    reinterpret float bits as integer bits

    float layout (IEEE 754)

    [ sign | exponent | mantissa ]
       1        8          23
*/
typedef union
{
    float    f;
    uint32_t u;
} mu_float_bits;


/*
===============================================================================
float -> half float
-------------------------------------------------------------------------------
    Convert 32-bit float → 16-bit half float

    float layout:
    [s][eeeeeeee][mmmmmmmmmmmmmmmmmmmmmmm]
     1    8                23

    half layout:
    [s][eeeee][mmmmmmmmmm]
     1   5        10
===============================================================================
*/
MU_INLINE uint16_t mu_quantize_half(float v)
{
    mu_float_bits fb = {.f = v};
    uint32_t      ui = fb.u;
    /* extract sign and move to half-float position */
    uint32_t sign = (ui >> 16) & 0x8000;
    /* remove sign so we can work on exponent/mantissa */
    uint32_t em = ui & 0x7fffffff;


    /*
        adjust exponent bias

        float bias = 127
        half  bias = 15

        difference = 112
    */
    uint32_t half = (em - (112u << 23) + (1u << 12)) >> 13;

    /* undermu → 0 */
    if(em < (113u << 23))
        half = 0;

    /* overmu → infinity */
    if(em >= (143u << 23))
        half = 0x7c00;

    /* NaN → quiet NaN */
    if(em > (255u << 23))
        half = 0x7e00;
    return (uint16_t)(sign | half);
}


/*
===============================================================================
half -> float
===============================================================================
*/
MU_INLINE float mu_dequantize_half(uint16_t h)
{
    /* extract sign */
    uint32_t sign = ((uint32_t)(h & 0x8000)) << 16;

    /* exponent + mantissa */
    uint32_t em = h & 0x7fff;

    /*
        restore exponent bias

        half bias = 15
        float bias = 127
        difference = 112
    */

    uint32_t r = (em + (112u << 10)) << 13;

    /* denormals → zero */
    if(em < (1u << 10))
        r = 0;

    /* infinity / NaN */
    if(em >= (31u << 10))
        r += (112u << 23);

    mu_float_bits fb;
    fb.u = sign | r;

    return fb.f;
}


/*
===============================================================================
reduce float precision
-------------------------------------------------------------------------------
keep N mantissa bits (max 23)
   Reduce float precision.

    float mantissa = 23 bits

    If N = 10
    keep 10 bits, remove 13 bits.
original mantissa

mmmmmmmmmmmmmmmmmmmmmmm  (23 bits)

↓

after quantization (N=10)

mmmmmmmmmm0000000000000
===============================================================================
*/
MU_INLINE float mu_quantize_float(float v, int n)
{
    assert(n >= 0 && n <= 23);

    mu_float_bits fb = {.f = v};
    uint32_t      ui = fb.u;
    /* mask of bits to remove */
    uint32_t mask = (1u << (23 - n)) - 1;

    /* rounding offset */
    uint32_t round = (1u << (23 - n)) >> 1;

    /* isolate exponent */
    uint32_t exponent = ui & 0x7f800000;

    /* round and clear lower bits */
    uint32_t rounded = (ui + round) & ~mask;

    /* avoid touching inf/nan */
    if(exponent != 0x7f800000)
        ui = rounded;

    /* flush denormals */
    if(exponent == 0)
        ui = 0;
    fb.u = ui;
    return fb.f;
}


/*
===============================================================================
UNORM quantization
-------------------------------------------------------------------------------
float [0,1] → integer with N bits
===============================================================================
*/
MU_INLINE int mu_quantize_unorm(float v, int n)
{
    float scale = (float)((1 << n) - 1);

    if(v < 0.0f)
        v = 0.0f;
    if(v > 1.0f)
        v = 1.0f;

    return (int)(v * scale + 0.5f);
}


/*
===============================================================================
SNORM quantization
-------------------------------------------------------------------------------
float [-1,1] → signed integer
===============================================================================
*/
MU_INLINE int mu_quantize_snorm(float v, int n)
{
    float scale = (float)((1 << (n - 1)) - 1);
    float round = (v >= 0.0f) ? 0.5f : -0.5f;

    if(v < -1.0f)
        v = -1.0f;
    if(v > 1.0f)
        v = 1.0f;

    return (int)(v * scale + round);
}
MU_INLINE float mu_dequantize_unorm(int v, int n)
{
    float scale = (float)((1 << n) - 1);
    return (float)v / scale;
}

MU_INLINE float mu_dequantize_snorm(int v, int n)
{
    float scale = (float)((1 << (n - 1)) - 1);
    return (float)v / scale;
}
