#ifndef MU_FIXED_H
#define MU_FIXED_H

/* mu_fixed.h -- Q16.16 fixed point, and trig that does not call libm.
 *
 * WHY THIS EXISTS. Everything else in this library is float, and float is
 * right almost everywhere. It is the wrong tool for three jobs:
 *
 *   1. determinism. sinf/cosf/atan2f are allowed to differ between libms, and
 *      between versions of the same libm, by an ulp or two. A replay, a
 *      lockstep session, or a server that must agree with a client cannot
 *      accept "an ulp or two" -- the divergence compounds. This header is
 *      integer arithmetic end to end, so the same inputs give the same bits on
 *      every machine with 32-bit two's complement ints: no libm, no FPU, and no
 *      FPU-mode dependency (no rounding mode, no FTZ/DAZ, no denormals).
 *
 *   2. platforms without an FPU, where every float op is a soft-float call.
 *
 *   3. size and comparability. A Q16.16 value is 4 bytes, can be memcpy'd,
 *      hashed, sent over the wire and compared for equality -- none of which is
 *      true of a float whose low bits you care about.
 *
 * THE COST, STATED UP FRONT. Q16.16 resolves 1/65536 = 1.5e-5 over a range of
 * +/-32768: about 4.5 decimal digits. Plenty for animation, audio gains, sensor
 * scaling, tile maths and game logic; not enough for the physics you would have
 * used a double for. sin/cos/atan2 here are CORDIC evaluations, accurate to
 * about one LSB (verified exhaustively over every Q16.16 angle in the turn --
 * perf_test/test_fixed.c), not correctly-rounded doubles.
 *
 * ANGLE REPRESENTATION. Angles are `mu_ang`: a Q16.16 count of *turns*, so a
 * full turn is exactly 65536 and wraps for free in two's complement. This is
 * the most useful decision in the file: range reduction is an AND, there is no
 * pi in the hot path, and a `mu_ang` is interchangeable with a 16.16 phase
 * accumulator.
 *
 *   mu_ang a = mu_ang_from_radians(MU_PI_Q16);   // half a turn
 *   mu_q16 s, c; mu_q16_sincos(a, &s, &c);       // (-1, 0) in Q16.16
 *
 * No allocation, no state, no mutable globals; everything is MU_INLINE.
 *
 * NOT HERE, DELIBERATELY: exp2/log2, matrix or vector fixed point, 64-bit
 * Q32.32, and saturation-checked trig (the trig saturates at the output
 * resolution instead, which is what Q16.16 gets you). */

#include "mu_macros.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This file relies on a sign-extending right shift of negative values, which
 * every compiler we build for does and C99 leaves implementation-defined. If
 * yours does not, the typedef below fails to compile instead of quietly
 * producing wrong sine waves. */
typedef char mu_fixed__arithmetic_shift_required[((-1 >> 4) == -1) ? 1 : -1];

/* ----------- 1. types, constants, conversions ----------- */

typedef int32_t mu_q16; /* signed Q16.16: value = mu_q16 / 65536 */
typedef int32_t mu_ang; /* angle in turns, Q16.16: 65536 == one full turn */

#define MU_Q16_ONE 65536
#define MU_Q16_HALF 32768
#define MU_Q16_MAX ((mu_q16)0x7FFFFFFF)
#define MU_Q16_MIN ((mu_q16)(-0x7FFFFFFF - 1))
#define MU_Q16_EPS 1 /* the smallest positive Q16.16 step */

#define MU_ANG_TURN 65536
#define MU_ANG_HALF 32768
#define MU_ANG_QUARTER 16384
#define MU_ANG_EIGHTH 8192
#define MU_ANG_MASK 0xFFFF /* one whole turn; also the wrap mask */

/* Nearest Q16.16 values, generated once rather than recomputed. */
#define MU_PI_Q16 205887
#define MU_TAU_Q16 411775
#define MU_HALF_PI_Q16 102944
#define MU_E_Q16 178145
#define MU_SQRT2_Q16 92682
#define MU_LN2_Q16 45426
#define MU_INV_LN2_Q16 94548 /* 1/ln(2) */

/* Float <-> fixed. Q16.16 -> float is exact (a 24-bit mantissa holds 16
 * fractional bits); float -> Q16.16 truncates toward zero, and the `_round`
 * variant rounds to nearest, ties away from zero. */
MU_INLINE mu_q16 mu_q16_from_i32(int32_t v) { return (mu_q16)((int64_t)v * MU_Q16_ONE); }

/* Floor, not truncation: -0.5 becomes -1. Documented because it differs from
 * C's integer division and it is the useful direction for tiles and indices. */
MU_INLINE int32_t mu_q16_to_i32(mu_q16 q) { return q >> 16; }

/* Round half away from zero. The sign is handled explicitly because `>>` on a
 * negative int floors: (q + HALF) >> 16 would round -0.6 up to 0. */
MU_INLINE int32_t mu_q16_round_i32(mu_q16 q) {
	return q >= 0 ? (int32_t)((q + MU_Q16_HALF) >> 16) : (int32_t)(-((-q + MU_Q16_HALF) >> 16));
}

MU_INLINE mu_q16 mu_q16_from_f32(float f) { return (mu_q16)(f * 65536.0f); }

MU_INLINE mu_q16 mu_q16_from_f32_round(float f) {
	return (mu_q16)(f * 65536.0f + (f >= 0.0f ? 0.5f : -0.5f));
}

MU_INLINE float mu_q16_to_f32(mu_q16 q) { return (float)q * (1.0f / 65536.0f); }

/* ----------- 2. arithmetic ----------- */

/* Plain add/sub wrap on overflow, like every other int32 in C. The saturating
 * pair is here because a wrapped gain or angle shows up as a corrupted sprite
 * three frames later, which is a bad way to find a bug. */
MU_INLINE mu_q16 mu_q16_add(mu_q16 a, mu_q16 b) { return (mu_q16)((uint32_t)a + (uint32_t)b); }
MU_INLINE mu_q16 mu_q16_sub(mu_q16 a, mu_q16 b) { return (mu_q16)((uint32_t)a - (uint32_t)b); }

MU_INLINE mu_q16 mu_q16_add_sat(mu_q16 a, mu_q16 b) {
	int64_t r = (int64_t)a + (int64_t)b;
	return r > MU_Q16_MAX ? MU_Q16_MAX : (r < MU_Q16_MIN ? MU_Q16_MIN : (mu_q16)r);
}

MU_INLINE mu_q16 mu_q16_sub_sat(mu_q16 a, mu_q16 b) {
	int64_t r = (int64_t)a - (int64_t)b;
	return r > MU_Q16_MAX ? MU_Q16_MAX : (r < MU_Q16_MIN ? MU_Q16_MIN : (mu_q16)r);
}

/* a*b in Q16.16, rounded to nearest. The 64-bit product is exact; the divide by
 * a constant becomes a shift plus a sign correction, so this is one imul and a
 * handful of ALU ops. */
MU_INLINE mu_q16 mu_q16_mul(mu_q16 a, mu_q16 b) {
	int64_t p = (int64_t)a * (int64_t)b;
	return (mu_q16)((p + (p >= 0 ? MU_Q16_HALF : -MU_Q16_HALF)) / MU_Q16_ONE);
}

/* a/b, rounded. Division by zero saturates to +/-MU_Q16_MAX rather than
 * trapping or returning a silent 0: an obviously wrong answer beats a
 * plausible one. */
MU_INLINE mu_q16 mu_q16_div(mu_q16 a, mu_q16 b) {
	if (b == 0) return a >= 0 ? MU_Q16_MAX : MU_Q16_MIN;
	{
		int64_t n = (int64_t)a * MU_Q16_ONE; /* * not << : left shift of a negative is UB */
		int64_t r = (n + (b > 0 ? b / 2 : -(b / 2))) / b;
		return r > MU_Q16_MAX ? MU_Q16_MAX : (r < MU_Q16_MIN ? MU_Q16_MIN : (mu_q16)r);
	}
}

MU_INLINE mu_q16 mu_q16_mul_i32(mu_q16 a, int32_t b) {
	int64_t p = (int64_t)a * (int64_t)b;
	return p > MU_Q16_MAX ? MU_Q16_MAX : (p < MU_Q16_MIN ? MU_Q16_MIN : (mu_q16)p);
}

MU_INLINE mu_q16 mu_q16_div_i32(mu_q16 a, int32_t b) {
	if (b == 0) return a >= 0 ? MU_Q16_MAX : MU_Q16_MIN;
	return (mu_q16)(a / b);
}

/* t = 0 gives a, t = MU_Q16_ONE gives b. The difference is taken in 64 bits so
 * a = -32768 and b = 32768 do not overflow. */
MU_INLINE mu_q16 mu_q16_lerp(mu_q16 a, mu_q16 b, mu_q16 t) {
	int64_t d = (int64_t)b - (int64_t)a;
	int64_t p = d * t;
	return (mu_q16)((int64_t)a + (p + (p >= 0 ? MU_Q16_HALF : -MU_Q16_HALF)) / MU_Q16_ONE);
}

MU_INLINE mu_q16 mu_q16_abs(mu_q16 a) { return a < 0 ? (mu_q16)(0u - (uint32_t)a) : a; }
MU_INLINE mu_q16 mu_q16_neg(mu_q16 a) { return (mu_q16)(0u - (uint32_t)a); }
MU_INLINE mu_q16 mu_q16_min(mu_q16 a, mu_q16 b) { return a < b ? a : b; }
MU_INLINE mu_q16 mu_q16_max(mu_q16 a, mu_q16 b) { return a > b ? a : b; }

MU_INLINE mu_q16 mu_q16_clamp(mu_q16 v, mu_q16 lo, mu_q16 hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

MU_INLINE mu_q16 mu_q16_sign(mu_q16 a) { return a < 0 ? -MU_Q16_ONE : (a > 0 ? MU_Q16_ONE : 0); }

/* ----------- 3. square root ----------- */

/* Floor of the square root of a 64-bit value, digit by digit: no division, no
 * libm, at most 32 iterations (one per pair of bits). */
MU_INLINE uint32_t mu_u64_sqrt(uint64_t v) {
	uint64_t rem = v;
	uint64_t root = 0;
	uint64_t bit = (uint64_t)1 << 62;

	while (bit > rem) bit >>= 2;
	while (bit != 0) {
		if (rem >= root + bit) {
			rem -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)root;
}

/* sqrt of a Q16.16 value, rounded to nearest; sqrt of a negative is 0 (there is
 * no NaN here, and a negative input is a caller bug the header refuses to
 * propagate). */
MU_INLINE mu_q16 mu_q16_sqrt(mu_q16 x) {
	uint64_t v;
	uint32_t r;

	if (x <= 0) return 0;
	v = (uint64_t)x << 16;
	r = mu_u64_sqrt(v);
	/* round(sqrt(v)) == r+1 exactly when v - r^2 >= r + 1 (integers). */
	if (v - (uint64_t)r * (uint64_t)r >= (uint64_t)r + 1u) ++r;
	return (mu_q16)r;
}

/* 1/sqrt(x). Two integer ops plus a divide; the point is that it needs no
 * reciprocal-sqrt instruction and gives the same bits everywhere. */
MU_INLINE mu_q16 mu_q16_rsqrt(mu_q16 x) {
	if (x <= 0) return 0;
	return mu_q16_div(MU_Q16_ONE, mu_q16_sqrt(x));
}

/* The FPU version, for callers that have an FPU and want the speed: define
 * MU_FIXED_WANT_FLOAT_SQRT before including this header. IEEE-754 requires
 * sqrt to be correctly rounded, so this is *still* bit-deterministic across
 * platforms (unlike sin/cos, which is why those are integer end to end) -- it
 * just needs hardware. 1.1 ns against the 84 ns digit-by-digit loop, and the
 * gate checks the two agree within 1 LSB. Opt-in so that a no-libm,
 * no-float target never sees <math.h> or a sqrtf symbol. */
#ifdef MU_FIXED_WANT_FLOAT_SQRT
#include <math.h>
MU_INLINE mu_q16 mu_q16_sqrt_f32(mu_q16 x) {
	if (x <= 0) return 0;
	return (mu_q16)(sqrtf((float)x * (1.0f / 65536.0f)) * 65536.0f);
}
#endif

/* ----------- 4. angles ----------- */

MU_INLINE mu_ang mu_ang_wrap(mu_ang a) { return (mu_ang)((uint32_t)a & MU_ANG_MASK); }

/* Both directions round to nearest: truncation here costs a whole LSB at exact
 * quarter points (pi in Q16.16 is itself rounded, so pi -> turn -> pi is a
 * two-step division that lands 0.9999 short and then floors). */
MU_INLINE mu_ang mu_ang_from_radians(mu_q16 rad) {
	int64_t n = (int64_t)rad * 65536;
	int64_t h = MU_TAU_Q16 / 2;
	return (mu_ang)((n + (n >= 0 ? h : -h)) / MU_TAU_Q16);
}

MU_INLINE mu_q16 mu_ang_to_radians(mu_ang a) {
	int64_t n = (int64_t)a * MU_TAU_Q16;
	return (mu_q16)((n + (n >= 0 ? MU_Q16_HALF : -MU_Q16_HALF)) / MU_Q16_ONE);
}

MU_INLINE mu_ang mu_ang_from_degrees_i32(int32_t deg) {
	int64_t n = (int64_t)deg * 65536;
	return (mu_ang)((n + (n >= 0 ? 180 : -180)) / 360);
}

/* turns -> degrees. In Q16.16 that is literally `a * 360` (see the derivation
 * in the comment below), saturating because 91 turns is where int32 ends. */
MU_INLINE mu_q16 mu_ang_to_degrees(mu_ang a) {
	int64_t d = (int64_t)a * 360;
	return d > MU_Q16_MAX ? MU_Q16_MAX : (d < MU_Q16_MIN ? MU_Q16_MIN : (mu_q16)d);
}

/* ----------- 5. trig ----------- */

/* Q1.30 -> Q16.16, rounded half away from zero, clamped to [-1, +1]. */
#define MU_FIXED__Q30_TO_Q16(v) (((v) >= 0) ? (int32_t)(((v) + 8192) >> 14) : (int32_t)(-((-(v) + 8192) >> 14)))
#define MU_FIXED__CLAMP1(v)                                                                        \
	(MU_FIXED__Q30_TO_Q16(v) > MU_Q16_ONE ? MU_Q16_ONE                                                \
										  : (MU_FIXED__Q30_TO_Q16(v) < -MU_Q16_ONE ? -MU_Q16_ONE     \
																					: MU_FIXED__Q30_TO_Q16(v)))

/* ---- 5a. sin/cos: quarter-wave table with linear interpolation ----
 *
 * sin(i/512 * pi/2) in Q16.16, i = 0..512, plus one guard entry so the
 * interpolation at the quarter point reads tab[512] instead of wrapping.
 * 513 * 4 bytes = 2052 bytes of read-only data, and it is the single biggest
 * decision in this file.
 *
 * WHY A TABLE AND NOT CORDIC. CORDIC is the textbook answer for fixed-point
 * trig, so it was written first (it is still here, as mu_q16_sincos_cordic)
 * and it measured 133 ns per sincos against 15.8 ns for glibc's sinf+cosf --
 * 8x slower, because 20 dependent shift-add steps with a data-dependent
 * branch each is 20 chances to mispredict. Q16.16 wants 16 bits of accuracy,
 * a quarter wave sampled at 512 points with linear interpolation is accurate
 * to 1 LSB (measured exhaustively, test_fixed.c), and the interpolation is
 * one multiply. Same accuracy, 10x the speed; see PERF_FIXED.md section 3.
 *
 * Interior arithmetic: `frac` is the position inside a segment scaled to
 * 0..65536, `d` the difference between neighbouring entries, so the result is
 * tab[i] + (d * frac) >> 16. The table itself is round-to-nearest, the
 * interpolation truncates, and the sign comes from the quadrant -- which is
 * why sin(-x) is exactly -sin(x). */
#define MU_FIXED__SIN_SEGMENTS 512
#define MU_FIXED__SIN_SHIFT 5 /* 16384 quarter-units / 512 segments == 32 */

MU_INLINE int32_t mu_fixed__sinq(int i)
{
	static const int32_t tab[MU_FIXED__SIN_SEGMENTS + 1] = {
	     0,    201,    402,    603,    804,   1005,   1206,   1407,   1608,   1809,
	  2010,   2211,   2412,   2613,   2814,   3015,   3216,   3417,   3617,   3818,
	  4019,   4219,   4420,   4621,   4821,   5022,   5222,   5422,   5623,   5823,
	  6023,   6224,   6424,   6624,   6824,   7024,   7224,   7423,   7623,   7823,
	  8022,   8222,   8421,   8621,   8820,   9019,   9218,   9417,   9616,   9815,
	 10014,  10212,  10411,  10609,  10808,  11006,  11204,  11402,  11600,  11798,
	 11996,  12193,  12391,  12588,  12785,  12983,  13180,  13376,  13573,  13770,
	 13966,  14163,  14359,  14555,  14751,  14947,  15143,  15338,  15534,  15729,
	 15924,  16119,  16314,  16508,  16703,  16897,  17091,  17285,  17479,  17673,
	 17867,  18060,  18253,  18446,  18639,  18832,  19024,  19216,  19409,  19600,
	 19792,  19984,  20175,  20366,  20557,  20748,  20939,  21129,  21320,  21510,
	 21699,  21889,  22078,  22268,  22457,  22645,  22834,  23022,  23210,  23398,
	 23586,  23774,  23961,  24148,  24335,  24521,  24708,  24894,  25080,  25265,
	 25451,  25636,  25821,  26005,  26190,  26374,  26558,  26742,  26925,  27108,
	 27291,  27474,  27656,  27838,  28020,  28202,  28383,  28564,  28745,  28926,
	 29106,  29286,  29466,  29645,  29824,  30003,  30182,  30360,  30538,  30716,
	 30893,  31071,  31248,  31424,  31600,  31776,  31952,  32127,  32303,  32477,
	 32652,  32826,  33000,  33173,  33347,  33520,  33692,  33865,  34037,  34208,
	 34380,  34551,  34721,  34892,  35062,  35231,  35401,  35570,  35738,  35907,
	 36075,  36243,  36410,  36577,  36744,  36910,  37076,  37241,  37407,  37572,
	 37736,  37900,  38064,  38228,  38391,  38554,  38716,  38878,  39040,  39201,
	 39362,  39523,  39683,  39843,  40002,  40161,  40320,  40478,  40636,  40794,
	 40951,  41108,  41264,  41420,  41576,  41731,  41886,  42040,  42194,  42348,
	 42501,  42654,  42806,  42958,  43110,  43261,  43412,  43562,  43713,  43862,
	 44011,  44160,  44308,  44456,  44604,  44751,  44898,  45044,  45190,  45335,
	 45480,  45625,  45769,  45912,  46056,  46199,  46341,  46483,  46624,  46765,
	 46906,  47046,  47186,  47325,  47464,  47603,  47741,  47878,  48015,  48152,
	 48288,  48424,  48559,  48694,  48828,  48962,  49095,  49228,  49361,  49493,
	 49624,  49756,  49886,  50016,  50146,  50275,  50404,  50532,  50660,  50787,
	 50914,  51041,  51166,  51292,  51417,  51541,  51665,  51789,  51911,  52034,
	 52156,  52277,  52398,  52519,  52639,  52759,  52878,  52996,  53114,  53232,
	 53349,  53465,  53581,  53697,  53812,  53926,  54040,  54154,  54267,  54379,
	 54491,  54603,  54714,  54824,  54934,  55043,  55152,  55260,  55368,  55476,
	 55582,  55689,  55794,  55900,  56004,  56108,  56212,  56315,  56418,  56520,
	 56621,  56722,  56823,  56923,  57022,  57121,  57219,  57317,  57414,  57511,
	 57607,  57703,  57798,  57892,  57986,  58079,  58172,  58265,  58356,  58448,
	 58538,  58628,  58718,  58807,  58896,  58983,  59071,  59158,  59244,  59330,
	 59415,  59499,  59583,  59667,  59750,  59832,  59914,  59995,  60075,  60156,
	 60235,  60314,  60392,  60470,  60547,  60624,  60700,  60776,  60851,  60925,
	 60999,  61072,  61145,  61217,  61288,  61359,  61429,  61499,  61568,  61637,
	 61705,  61772,  61839,  61906,  61971,  62036,  62101,  62165,  62228,  62291,
	 62353,  62415,  62476,  62536,  62596,  62655,  62714,  62772,  62830,  62886,
	 62943,  62998,  63054,  63108,  63162,  63215,  63268,  63320,  63372,  63423,
	 63473,  63523,  63572,  63621,  63668,  63716,  63763,  63809,  63854,  63899,
	 63944,  63987,  64031,  64073,  64115,  64156,  64197,  64237,  64277,  64316,
	 64354,  64392,  64429,  64465,  64501,  64536,  64571,  64605,  64639,  64672,
	 64704,  64735,  64766,  64797,  64827,  64856,  64884,  64912,  64940,  64967,
	 64993,  65018,  65043,  65067,  65091,  65114,  65137,  65159,  65180,  65200,
	 65220,  65240,  65259,  65277,  65294,  65311,  65328,  65343,  65358,  65373,
	 65387,  65400,  65413,  65425,  65436,  65447,  65457,  65467,  65476,  65484,
	 65492,  65499,  65505,  65511,  65516,  65521,  65525,  65528,  65531,  65533,
	 65535,  65536,  65536,
	};
	return tab[i];
}

/* sin of an angle in turns, one interpolation. */
MU_INLINE mu_q16 mu_q16_sin(mu_ang a)
{
	uint32_t u = (uint32_t)a;
	uint32_t quad = (u >> 14) & 3u;
	uint32_t f = u & 0x3FFFu;                       /* position in the quadrant */
	uint32_t ff = (quad & 1u) ? (16384u - f) : f;   /* fold the mirror half      */
	uint32_t idx = ff >> MU_FIXED__SIN_SHIFT;
	int32_t d, v;

	if (idx > MU_FIXED__SIN_SEGMENTS - 1u) idx = MU_FIXED__SIN_SEGMENTS - 1u; /* the quarter point */
	d = mu_fixed__sinq((int)idx + 1) - mu_fixed__sinq((int)idx);
	v = mu_fixed__sinq((int)idx) +
		(int32_t)(((int64_t)((ff - (idx << MU_FIXED__SIN_SHIFT)) << (16 - MU_FIXED__SIN_SHIFT)) * d) >> 16);
	return (quad & 2u) ? -v : v;
}

/* cos is sin a quarter turn later: no second table, no second case analysis. */
MU_INLINE mu_q16 mu_q16_cos(mu_ang a) { return mu_q16_sin((mu_ang)((uint32_t)a + MU_ANG_QUARTER)); }

MU_INLINE void mu_q16_sincos(mu_ang a, mu_q16* out_sin, mu_q16* out_cos)
{
	*out_sin = mu_q16_sin(a);
	*out_cos = mu_q16_cos(a);
}

MU_INLINE mu_q16 mu_q16_tan(mu_ang a) {
	mu_q16 s, c;
	mu_q16_sincos(a, &s, &c);
	return mu_q16_div(s, c); /* saturates at the 90-degree points */
}

/* ---- 5b. the CORDIC pair, kept: no table, ~4x the code path cost ----
 *
 * Nothing in the library calls this by default. It is here because (a) the
 * accuracy claim for the table is only meaningful next to the thing it
 * replaced, and (b) a build that cannot afford 2 KB of .rodata (a boot ROM, a
 * 4 KB parts bin) still gets exact-to-1.5-LSB sin/cos from 80 bytes of atan
 * table. It declines a dependency on any table larger than that at the cost of
 * speed: 133 ns vs 15 ns for the table, on the machine in PERF_FIXED.md. */
MU_INLINE void mu_q16_sincos_cordic(mu_ang a, mu_q16* out_sin, mu_q16* out_cos)
{
	static const int32_t atan_q30[20] = {
		134217728, 79233351, 41864727, 21251189, 10666833,
		5338616,   2669960,  1335061,  667541,   333772,
		166886,    83443,    41722,    20861,    10430,
		5215,      2608,     1304,     652,      326,
	};
	uint32_t u = (uint32_t)a;
	int32_t quad = (int32_t)((u >> 14) & 3u);
	int32_t z = (int32_t)((u & 0x3FFFu) << 14); /* [0, quarter turn), Q1.30 */
	int32_t x = 652032874;                      /* 1/K in Q1.30, K = 1.64676 */
	int32_t y = 0;
	int32_t i;
	int32_t cs, sn;

	for (i = 0; i < 20; ++i) {
		int32_t t = y >> i; /* old y * 2^-i */
		int32_t e = x >> i; /* old x * 2^-i */
		if (z >= 0) {
			x -= t;
			y += e;
			z -= atan_q30[i];
		} else {
			x += t;
			y -= e;
			z += atan_q30[i];
		}
	}

	sn = y;
	cs = x;
	switch (quad) {
	case 0: break;                                          /* 0..90    */
	case 1: { int32_t t = cs; cs = -sn; sn = t; break; }     /* 90..180  */
	case 2: cs = -cs; sn = -sn; break;                       /* 180..270 */
	default: { int32_t t = cs; cs = sn; sn = -t; break; }    /* 270..360 */
	}
	*out_sin = MU_FIXED__CLAMP1(sn);
	*out_cos = MU_FIXED__CLAMP1(cs);
}

/* ---- 5c. atan2 and its inverse friends: still CORDIC ----
 *
 * atan2 is not periodic, so the table trick does not apply directly and the
 * vectoring loop is 37 ns against libm's 23 -- a genuine loss, kept because it
 * is the only version that is bit-identical across platforms. A table for
 * atan2 is possible (ratio -> angle) and is not written. */
MU_INLINE mu_ang mu_q16_atan2(mu_q16 y, mu_q16 x)
{
	static const int32_t atan_q30[20] = {
		134217728, 79233351, 41864727, 21251189, 10666833,
		5338616,   2669960,  1335061,  667541,   333772,
		166886,    83443,    41722,    20861,    10430,
		5215,      2608,     1304,     652,      326,
	};
	int64_t vx, vy;
	int32_t z = 0;
	int32_t i;

	/* The axes are exact answers, not approximations: return them instead of
	 * looping toward them from a rounding-level start. */
	if (x == 0) {
		if (y == 0) return 0;
		return y > 0 ? MU_ANG_QUARTER : -MU_ANG_QUARTER;
	}
	if (y == 0) return x > 0 ? 0 : MU_ANG_HALF;

	vx = (int64_t)x * 16384; /* * not << : left shift of a negative is UB */
	vy = (int64_t)y * 16384;

	/* CORDIC converges only for |angle| < sum(atan(2^-i)) = 99.88 degrees, so
	 * fold the left half plane onto the right one and remember the half turn.
	 * Without this, atan2(-1,-1) comes back as -99.88 degrees. */
	if (vx < 0) {
		z = (vy >= 0) ? (int32_t)(1 << 29) : -(int32_t)(1 << 29); /* half a turn, Q1.30 */
		vx = -vx;
		vy = -vy;
	}

	for (i = 0; i < 20; ++i) {
		int64_t t = vy >> i;
		int64_t e = vx >> i;
		if (vy > 0) { /* rotate clockwise: the angle is positive */
			vx += t;
			vy -= e;
			z += atan_q30[i];
		} else {
			vx -= t;
			vy += e;
			z -= atan_q30[i];
		}
	}
	return MU_FIXED__Q30_TO_Q16(z);
}

MU_INLINE mu_ang mu_q16_asin(mu_q16 s) {
	mu_q16 c2;
	if (s >= MU_Q16_ONE) return MU_ANG_QUARTER;
	if (s <= -MU_Q16_ONE) return -MU_ANG_QUARTER;
	c2 = MU_Q16_ONE - mu_q16_mul(s, s);
	return mu_q16_atan2(s, mu_q16_sqrt(c2));
}

MU_INLINE mu_ang mu_q16_acos(mu_q16 c) { return MU_ANG_QUARTER - mu_q16_asin(c); }

MU_INLINE mu_q16 mu_q16_sin_rad(mu_q16 rad) { return mu_q16_sin(mu_ang_from_radians(rad)); }
MU_INLINE mu_q16 mu_q16_cos_rad(mu_q16 rad) { return mu_q16_cos(mu_ang_from_radians(rad)); }

/* atan2 with radians in and out, for code that lives in radians and only
 * borrows the integer path for speed or determinism. */
MU_INLINE mu_q16 mu_q16_atan2_rad(mu_q16 y, mu_q16 x) {
	return mu_ang_to_radians(mu_q16_atan2(y, x));
}

/* ----------- 6. what Q16.16 sin actually costs in accuracy ----------- */
/* One LSB of Q16.16 is 1.52588e-05. The CORDIC residual after 20 iterations is
 * 1.5e-07 turns = 9.5e-07 rad, i.e. 0.06 LSB, so the error you can observe is
 * the output rounding: at most 1 LSB, which is what the exhaustive gate
 * measures and asserts. Do not read that as "as accurate as sinf" -- it is
 * about 1e-5 absolute, where sinf is 1e-7 relative. */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MU_FIXED_H */
