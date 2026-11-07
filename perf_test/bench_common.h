#ifndef PERF_BENCH_COMMON_H
#define PERF_BENCH_COMMON_H

/*
 * bench_common.h — Scientific repetition-tester harness (Muratori / Fog / Algorithmica)
 *
 * Rules enforced:
 *   1. Minimum time & minimum TSC cycles across REPS runs (OS/VM noise is strictly additive).
 *   2. Explicit compiler barriers prevent dead-code elimination and loop hoisting across reps.
 *   3. Checksums verify mathematical equivalence between baseline and optimized kernels.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

#define BENCH_ALIGN(N) __attribute__((aligned(N)))
#define BENCH_NOINLINE __attribute__((noinline))
#define BENCH_RESTRICT __restrict__

#define DO_NOT_OPTIMIZE(val) __asm__ volatile("" : : "r,m"(val) : "memory")
#define FORCE_REG(val)       __asm__ volatile("" : "+r"(val) : : "memory")
#define CLOBBER_MEMORY()     __asm__ volatile("" : : : "memory")

static inline uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static inline uint64_t bench_rdtsc_start(void)
{
    _mm_lfence();
    return __rdtsc();
}

static inline uint64_t bench_rdtsc_end(void)
{
    unsigned int aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

static inline uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static inline void *bench_alloc_aligned(size_t alignment, size_t size)
{
    size_t padded = (size + alignment - 1u) & ~(alignment - 1u);
    void *p = aligned_alloc(alignment, padded);
    if (!p) {
        perror("aligned_alloc");
        exit(1);
    }
    return p;
}

static inline void bench_print_section(const char *title, const char *sources)
{
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("  %s\n", title);
    printf("  Sources: %s\n", sources);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("  %-38s %10s %10s %12s %9s  %s\n",
           "Variant", "ns/op", "cyc/op", "Throughput", "Speedup", "Checksum");
    printf("  ----------------------------------------------------------------------------------\n");
}

static inline void bench_print_row(FILE *csv, const char *suite, const char *case_name,
                                   const char *variant, uint64_t best_ns, uint64_t best_cyc,
                                   size_t ops, double base_ns, uint64_t checksum,
                                   const char *unit_label, double unit_scale)
{
    double ns_per_op  = (double)best_ns / (double)ops;
    double cyc_per_op = (double)best_cyc / (double)ops;
    double speedup    = (base_ns > 0.0) ? (base_ns / (double)best_ns) : 1.0;
    double tput       = ((double)ops * unit_scale) / ((double)best_ns * 1e-9) / 1e6;

    printf("  %-38s %10.3f %10.2f %8.1f %-3s %8.2fx  %llu\n",
           variant, ns_per_op, cyc_per_op, tput, unit_label, speedup,
           (unsigned long long)checksum);

    if (csv) {
        fprintf(csv, "%s,%s,\"%s\",%.4f,%.3f,%.3f,%llu\n",
                suite, case_name, variant, ns_per_op, cyc_per_op, speedup,
                (unsigned long long)checksum);
    }
}

#endif /* PERF_BENCH_COMMON_H */
