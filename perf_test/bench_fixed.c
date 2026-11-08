/* bench_fixed.c -- what the integer trig path costs, next to libm
 *
 * The comparison the header exists for is not "faster than sinf" -- glibc's
 * sinf is a tuned polynomial and CORDIC is 20 shift-add steps -- it is "the
 * same answer everywhere, with no libm and no FPU". So both sides get the same
 * treatment and the numbers are read as what they are.
 *
 * Every per-call row carries a dependency on the input: the angle advances by a
 * constant it cannot fold (`a += 7919`) and the result is accumulated, so the
 * loop cannot be hoisted, and the same shape is used on both sides. The batch
 * rows walk a 1024-entry angle array instead, which is the throughput question:
 * no dependency, one sequential stream of inputs.
 *
 *   make -C perf_test fixed-bench
 *   ./bench_fixed --filter=fixed --samples=31
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h" /* first: it sets _POSIX_C_SOURCE */

#define MU_FIXED_WANT_FLOAT_SQRT 1
#include "mu_fixed.h"

#include <math.h>

#define BATCH 1024

static mu_ang g_ang[BATCH];
static float  g_rad[BATCH];
static mu_q16 g_q16[BATCH];
static float  g_f32[BATCH];

/* The per-call rows share this: a 32-bit LCG so the inputs are not a pattern the
 * compiler can reason about, and a single accumulator so nothing is dead. */
static volatile int32_t  g_sink_i;
static volatile float    g_sink_f;

static void seed_inputs(void)
{
	uint32_t s = 987654321u;
	int i;

	for (i = 0; i < BATCH; ++i) {
		s = s * 1664525u + 1013904223u;
		g_ang[i] = (mu_ang)(int32_t)s;
		g_rad[i] = (float)g_ang[i] * (6.28318530717958647692f / 65536.0f);
		s = s * 1664525u + 1013904223u;
		g_q16[i] = (mu_q16)((int32_t)(s >> 8) % (3 * MU_Q16_ONE)) - MU_Q16_ONE;
		g_f32[i] = mu_q16_to_f32(g_q16[i]);
	}
}

/* ---- per call, dependent chain ------------------------------------------ */

MU_BENCH(fixed, sincos)
{
	uint64_t i;
	mu_ang a = 0;
	MU_BENCH_ITERATE(i)
	{
		mu_q16 s, c;
		a += 7919;
		mu_q16_sincos(a, &s, &c);
		g_sink_i = s + c;
	}
}

/* The no-table alternative kept in the header, on the same dependency shape:
 * this row is the reason the table exists. */
MU_BENCH(fixed, sincos_cordic)
{
	uint64_t i;
	mu_ang a = 0;
	MU_BENCH_ITERATE(i)
	{
		mu_q16 s, c;
		a += 7919;
		mu_q16_sincos_cordic(a, &s, &c);
		g_sink_i = s + c;
	}
}

MU_BENCH(libm, sincos)
{
	uint64_t i;
	mu_ang a = 0;
	MU_BENCH_ITERATE(i)
	{
		float r;
		a += 7919;
		r = (float)a * (6.28318530717958647692f / 65536.0f);
		g_sink_f = sinf(r) + cosf(r);
	}
}

MU_BENCH(fixed, sin)
{
	uint64_t i;
	mu_ang a = 0;
	MU_BENCH_ITERATE(i)
	{
		a += 7919;
		g_sink_i = mu_q16_sin(a);
	}
}

MU_BENCH(libm, sin)
{
	uint64_t i;
	mu_ang a = 0;
	MU_BENCH_ITERATE(i)
	{
		a += 7919;
		g_sink_f = sinf((float)a * (6.28318530717958647692f / 65536.0f));
	}
}

MU_BENCH(fixed, atan2)
{
	uint64_t i;
	mu_q16 x = MU_Q16_ONE;
	MU_BENCH_ITERATE(i)
	{
		x += 977;
		if (x > 3 * MU_Q16_ONE) x = MU_Q16_ONE;
		g_sink_i = mu_q16_atan2(MU_Q16_ONE, x);
	}
}

MU_BENCH(libm, atan2)
{
	uint64_t i;
	mu_q16 x = MU_Q16_ONE;
	MU_BENCH_ITERATE(i)
	{
		x += 977;
		if (x > 3 * MU_Q16_ONE) x = MU_Q16_ONE;
		g_sink_f = atan2f(1.0f, mu_q16_to_f32(x));
	}
}

MU_BENCH(fixed, sqrt)
{
	uint64_t i;
	mu_q16 x = MU_Q16_ONE;
	MU_BENCH_ITERATE(i)
	{
		x += 1237;
		if (x > 16 * MU_Q16_ONE) x = MU_Q16_ONE;
		g_sink_i = mu_q16_sqrt(x);
	}
}

MU_BENCH(libm, sqrt)
{
	uint64_t i;
	mu_q16 x = MU_Q16_ONE;
	MU_BENCH_ITERATE(i)
	{
		x += 1237;
		if (x > 16 * MU_Q16_ONE) x = MU_Q16_ONE;
		g_sink_f = sqrtf(mu_q16_to_f32(x));
	}
}

MU_BENCH(fixed, sqrt_f32)
{
	uint64_t i;
	mu_q16 x = MU_Q16_ONE;
	MU_BENCH_ITERATE(i)
	{
		x += 1237;
		if (x > 16 * MU_Q16_ONE) x = MU_Q16_ONE;
		g_sink_i = mu_q16_sqrt_f32(x);
	}
}

/* ---- batch throughput --------------------------------------------------- */

MU_BENCH(batch, fixed_sincos_1024)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		int32_t acc = 0;
		int k;
		for (k = 0; k < BATCH; ++k) {
			mu_q16 s, c;
			mu_q16_sincos(g_ang[k], &s, &c);
			acc += s + c;
		}
		g_sink_i = acc;
	}
}

MU_BENCH(batch, fixed_sincos_cordic_1024)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		int32_t acc = 0;
		int k;
		for (k = 0; k < BATCH; ++k) {
			mu_q16 s, c;
			mu_q16_sincos_cordic(g_ang[k], &s, &c);
			acc += s + c;
		}
		g_sink_i = acc;
	}
}

MU_BENCH(batch, libm_sincos_1024)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		float acc = 0.0f;
		int k;
		for (k = 0; k < BATCH; ++k) acc += sinf(g_rad[k]) + cosf(g_rad[k]);
		g_sink_f = acc;
	}
}

MU_BENCH(batch, fixed_atan2_1024)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		int32_t acc = 0;
		int k;
		for (k = 0; k < BATCH; ++k) acc += mu_q16_atan2(g_q16[k], MU_Q16_ONE);
		g_sink_i = acc;
	}
}

MU_BENCH(batch, libm_atan2_1024)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		float acc = 0.0f;
		int k;
		for (k = 0; k < BATCH; ++k) acc += atan2f(g_f32[k], 1.0f);
		g_sink_f = acc;
	}
}

int main(int argc, char** argv)
{
	seed_inputs();
	return mu_bench_main(argc, argv);
}

MU_BENCH_SETUP(batch, fixed_sincos_1024) { (void)state; seed_inputs(); }
MU_BENCH_SETUP(batch, fixed_sincos_cordic_1024) { (void)state; seed_inputs(); }
MU_BENCH_SETUP(batch, libm_sincos_1024) { (void)state; seed_inputs(); }
MU_BENCH_SETUP(batch, fixed_atan2_1024) { (void)state; seed_inputs(); }
MU_BENCH_SETUP(batch, libm_atan2_1024) { (void)state; seed_inputs(); }
