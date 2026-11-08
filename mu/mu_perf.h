#pragma once

/* POSIX feature test: clock_gettime / mkstemp / mmap are hidden behind
 * -std=c99 unless asked for, and the define has to come before any system
 * header. Ask only if the user has not chosen already. */
#if !defined(_POSIX_C_SOURCE) && !defined(_DEFAULT_SOURCE) && !defined(__cplusplus) && !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "mu_common.h"
#if defined(_WIN32)
#include <windows.h>

MU_INLINE uint64_t mu_time_now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

MU_INLINE double mu_time_freq()
{
    static double freq = 0;
    if(!freq)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (double)f.QuadPart;
    }
    return freq;
}

#else
#include <time.h>

MU_INLINE uint64_t mu_time_now()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

MU_INLINE double mu_time_freq()
{
    return 1e9;  // nanoseconds
}
#endif

#define MU_SCOPE_TIMER(name)                                                                                           \
    for(uint64_t __start = mu_time_now(), __once                                                   = 1; __once;        \
        printf("%s: %.3f ms\n", name, (mu_time_now() - __start) * 1000.0 / mu_time_freq()), __once = 0)


