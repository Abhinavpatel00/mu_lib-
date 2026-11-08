/* bench_time.c -- cost of mu_time.h against the libc it replaces.
 *
 * The pairing rule: both sides of every pair are compiled into this one binary,
 * so they see the same -O3 -march=native, the same input table, and the same
 * clock. The "libc" rows are not strawmen -- they are the best realistic code a
 * caller writes with strftime/gmtime_r/timegm/strptime/snprintf, with the
 * fraction appended by hand because libc has no portable milliseconds field.
 *
 * What each pair is actually comparing:
 *   fmt_1s      mu_iso8601_format(.., 0, ..)  vs  time() + gmtime_r + strftime
 *   fmt_ms      mu_iso8601_format(.., 3, ..)  vs  the above + snprintf of ".%03uZ"
 *   parse_1s    mu_iso8601_parse              vs  strptime + timegm
 *   civil       mu_datetime_from_unix         vs  gmtime_r (both read the fields)
 *   to_unix     mu_datetime_to_unix           vs  timegm
 *   clock_ns    mu_clock_ns                   vs  clock_gettime(CLOCK_MONOTONIC)
 *                                                     (the delta IS the header's cost)
 *   dur_human   mu_dur_format_human           vs  double scaling + snprintf
 *   dur_hms     mu_dur_format_hms             vs  four divisions + snprintf
 *
 *   make -C perf_test time-bench
 */

#define _GNU_SOURCE 1

#define MU_BENCH_IMPLEMENTATION
#include <stdlib.h>
#include "mu_bench.h"

#include "mu_time.h"

#include <stdio.h>
#include <time.h>

/* A spread of instants, so nothing constant-folds and the year arithmetic sees
 * both sides of the epoch, a leap day and a century. */
static const int64_t k_s[16] = {
    0, 1794009600ll, 2147483647ll, -2147483648ll, 951782400ll, 1709164800ll,
    253402300799ll, -62167219200ll, 1000000000ll, 1234567890ll, 1893456000ll,
    946684800ll, 1104537600ll, 1609459200ll, 2051222400ll, 1234567890ll
};
static const uint32_t k_ns[16] = {
    0u, 678732032u, 1u, 999999999u, 500000000u, 123456789u, 999000000u, 0u,
    1000u, 1000000u, 987654321u, 4u, 42u, 999999998u, 7u, 314159265u
};

#define N_STAMPS ((int)(sizeof k_s / sizeof k_s[0]))

/* The best a caller can do with libc for the same 20-30 characters. */
static size_t libc_format(int64_t s, uint32_t ns, int with_frac, char *buf, size_t cap)
{
    time_t    tt = (time_t)s;
    struct tm tmv;
    size_t    n;

    if (!gmtime_r(&tt, &tmv)) return 0;
    n = strftime(buf, with_frac ? cap - 8u : cap, "%Y-%m-%dT%H:%M:%S", &tmv);
    if (n == 0u) return 0;
    if (with_frac) n += (size_t)snprintf(buf + n, cap - n, ".%03uZ", (unsigned)(ns / 1000000u));
    else buf[n++] = 'Z';
    buf[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ */
/* formatting                                                          */
/* ------------------------------------------------------------------ */
/* The same routine as mu_iso8601_format, one digit at a time through the
 * division loop -- i.e. this header before the two-digit-pair table. It lives in
 * the bench and not the library because an optimization that is not measured in
 * the same binary as its baseline is not measured at all: same inlining
 * decisions, same -O3, same input table, same instant. */
static int naive_format(int64_t unix_s, uint32_t nanos, int frac_digits, char *buf, size_t cap)
{
    char        tmp[40];
    char       *p = tmp;
    const mu_datetime dt = mu_datetime_from_unix(unix_s);
    int         i;

    p = mu_time__put_u64(p, (uint64_t)dt.date.year, 4);
    *p++ = '-';
    p = mu_time__put_u64(p, dt.date.month, 2);
    *p++ = '-';
    p = mu_time__put_u64(p, dt.date.day, 2);
    *p++ = 'T';
    p = mu_time__put_u64(p, dt.hour, 2);
    *p++ = ':';
    p = mu_time__put_u64(p, dt.minute, 2);
    *p++ = ':';
    p = mu_time__put_u64(p, dt.second, 2);
    if (frac_digits > 0) {
        *p++ = '.';
        for (i = 9; i > frac_digits; --i) nanos /= 10u;
        p = mu_time__put_u64(p, nanos, frac_digits);
    }
    *p++ = 'Z';
    {
        const size_t n = (size_t)(p - tmp);

        if (n + 1u > cap) return 0;
        memcpy(buf, tmp, n);
        buf[n] = '\0';
        return (int)n;
    }
}


MU_BENCH(time, fmt_1s_ours)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += (size_t)mu_iso8601_format(k_s[k], k_ns[k], 0, buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, fmt_1s_libc)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += libc_format(k_s[k], k_ns[k], 0, buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, fmt_ms_ours)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += (size_t)mu_iso8601_format(k_s[k], k_ns[k], 3, buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, fmt_ms_naive_digits)
{
    char     buf[40];
    uint64_t i;
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += naive_format(k_s[k], k_ns[k], 3, buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, fmt_1s_naive_digits)
{
    char     buf[40];
    uint64_t i;
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += naive_format(k_s[k], k_ns[k], 0, buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, fmt_ms_libc)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += libc_format(k_s[k], k_ns[k], 1, buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, fmt_ns_ours)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += (size_t)mu_iso8601_format(k_s[k], k_ns[k], 9, buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */
/* ------------------------------------------------------------------ */

/* Strings to parse, built once. Second precision so both sides can do the same
 * job: strptime has no portable fractional-seconds conversion. */
static char  g_stamp[16][40];
static int   g_stamp_ready = 0;

static void stamps_init(void)
{
    int i;

    if (g_stamp_ready) return;
    for (i = 0; i < N_STAMPS; ++i) {
        const int n = mu_iso8601_format(k_s[i], 0u, 0, g_stamp[i], sizeof g_stamp[i]);

        if (n <= 0) { printf("stamp %d failed\n", i); exit(1); }
    }
    g_stamp_ready = 1;
}

MU_BENCH(time, parse_1s_ours)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;
    uint32_t ns;

    stamps_init();
    MU_BENCH_ITERATE(i) {
        const int     k = (int)i & (N_STAMPS - 1);
        int64_t       s = 0;

        acc += mu_iso8601_parse(g_stamp[k], 20u, &s, &ns) + s;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, parse_1s_libc)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    stamps_init();
    MU_BENCH_ITERATE(i) {
        const int  k = (int)i & (N_STAMPS - 1);
        struct tm  tmv;

        memset(&tmv, 0, sizeof tmv);
        if (strptime(g_stamp[k], "%Y-%m-%dT%H:%M:%SZ", &tmv) == NULL) { acc += -1; continue; }
        acc += (int64_t)timegm(&tmv);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* The fraction is ours alone; the row exists so the fmt_ms pair is not read as
 * "we only win because we do less". */
MU_BENCH(time, parse_ns_ours)
{
    /* A static table of full-precision stamps, indexed per iteration: a body
     * that does not depend on i gets hoisted out of the loop and reports
     * 0.000 ns, which is a row that measures nothing. */
    static char stamp[16][40];
    static int  slen[16];
    uint64_t    i;
    int64_t     acc = 0;
    int         j;

    for (j = 0; j < N_STAMPS; ++j) {
        slen[j] = mu_iso8601_format(k_s[j], k_ns[j], 9, stamp[j], sizeof stamp[j]);
        if (slen[j] <= 0) { printf("init\n"); exit(1); }
    }
    MU_BENCH_ITERATE(i) {
        const int k = i & (N_STAMPS - 1);
        int64_t   v = 0;
        uint32_t  ns = 0;

        acc += mu_iso8601_parse(stamp[k], (size_t)slen[k], &v, &ns) + v + (int64_t)ns;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* The day-arithmetic alternative, which is what a caller should use when they
 * do not need month semantics: stay in epoch seconds and convert once.
 * mu_civil_add_days converts there and back (two calendar passes); this row is
 * the number that says what that costs. */
MU_BENCH(time, civil_add_days_day_space)
{
    uint64_t i;
    int64_t  acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += (int64_t)mu_civil_from_days(k_s[k] / 86400 + 45).year;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* ------------------------------------------------------------------ */
/* calendar                                                            */
/* ------------------------------------------------------------------ */

MU_BENCH(time, civil_ours)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int       k = (int)i & (N_STAMPS - 1);
        const mu_datetime dt = mu_datetime_from_unix(k_s[k]);

        acc += (int64_t)dt.date.year * 10000 + dt.date.month * 100 + dt.date.day + dt.hour;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, civil_libc)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int    k = (int)i & (N_STAMPS - 1);
        const time_t t = (time_t)k_s[k];
        struct tm    tmv;

        if (gmtime_r(&t, &tmv) == NULL) { acc += -1; continue; }
        acc += (int64_t)(tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday +
               tmv.tm_hour;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, to_unix_ours)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int      k = (int)i & (N_STAMPS - 1);
        const mu_civil c = mu_civil_from_days(k_s[k] / 86400); /* not constant: no folding */
        mu_datetime    dt;

        dt.date       = c;
        dt.hour       = (uint8_t)(k & 23);
        dt.minute     = (uint8_t)(k & 59);
        dt.second     = (uint8_t)(k & 59);
        dt.nanosecond = 0u;
        acc += mu_datetime_to_unix(dt);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, to_unix_libc)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int  k = (int)i & (N_STAMPS - 1);
        struct tm  tmv;

        memset(&tmv, 0, sizeof tmv);
        {
            const mu_civil c = mu_civil_from_days(k_s[k] / 86400);

            tmv.tm_year = c.year - 1900;
            tmv.tm_mon  = (int)c.month - 1;
            tmv.tm_mday = (int)c.day;
        }
        tmv.tm_hour = k & 23;
        tmv.tm_min  = k & 59;
        tmv.tm_sec  = k & 59;
        acc += (int64_t)timegm(&tmv);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* The header's clock call versus the syscall it wraps: the difference is what
 * mu_time.h itself costs, and it is the number that decides whether a logger can
 * stamp every record. */
MU_BENCH(time, clock_ours)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) { acc += mu_clock_ns(); }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, clock_syscall)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        struct timespec ts;

        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
            acc += (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, wall_ours)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        int64_t  s;
        uint32_t ns;

        mu_wall_now(&s, &ns); /* one clock read */
        acc += s + (int64_t)ns;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, wall_ours_two_reads)
{
    uint64_t i;
    int64_t  acc = 0;

    MU_BENCH_ITERATE(i) { acc += mu_wall_unix_s() + (int64_t)mu_wall_nanos(); }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, wall_libc)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        struct timespec ts;

        acc += (int64_t)time(NULL);
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) acc += (int64_t)ts.tv_nsec;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* ------------------------------------------------------------------ */
/* durations                                                           */
/* ------------------------------------------------------------------ */

MU_BENCH(time, dur_human_ours)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += (size_t)mu_dur_format_human(k_s[k] & 0x7fffffff, buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, dur_human_libc)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int64_t v = k_s[(int)i & (N_STAMPS - 1)] & 0x7fffffff;
        double        d = (double)v;
        const char   *u = "ns";

        if (d >= 1e9) { d *= 1e-9; u = "s"; }
        else if (d >= 1e6) { d *= 1e-6; u = "ms"; }
        else if (d >= 1e3) { d *= 1e-3; u = "us"; }
        acc += (size_t)snprintf(buf, sizeof buf, "%.2f %s", d, u);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, dur_hms_ours)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int k = (int)i & (N_STAMPS - 1);

        acc += (size_t)mu_dur_format_hms(k_s[k], buf, sizeof buf);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, dur_hms_libc)
{
    char    buf[40];
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        int64_t v = k_s[(int)i & (N_STAMPS - 1)];
        int64_t h, mi, s, ms;

        if (v < 0) v = -v;
        h  = v / 3600000000000ll;
        mi = (v % 3600000000000ll) / 60000000000ll;
        s  = (v % 60000000000ll) / 1000000000ll;
        ms = (v % 1000000000ll) / 1000000ll;
        acc += (size_t)snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld.%03lld", (long long)h,
                               (long long)mi, (long long)s, (long long)ms);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

/* ------------------------------------------------------------------ */
/* calendar extras, no libc equivalent (these are the "we have it and
 * you do not" rows: strptime/gmtime_r give no ISO week numbering at all)  */
/* ------------------------------------------------------------------ */

MU_BENCH(time, iso_week_ours)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int        k = (int)i & (N_STAMPS - 1);
        const mu_civil   c = mu_civil_from_days(k_s[k] / 86400);
        int32_t          iy = 0;
        uint32_t         wk = 0, wd = 0;

        mu_civil_iso_week(c, &iy, &wk, &wd);
        acc += (int64_t)iy * 100 + wk * 10 + wd;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, iso_week_libc)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        /* what "no support" costs: strftime %G/%V/%u is the only portable route,
         * and it still needs a struct tm, so the conversion is paid for first. */
        const int    k = (int)i & (N_STAMPS - 1);
        const time_t t = (time_t)k_s[k];
        struct tm    tmv;
        char         out[24];

        if (gmtime_r(&t, &tmv) == NULL) { acc += -1; continue; }
        acc += (int64_t)strftime(out, sizeof out, "%G-W%V-%u", &tmv);
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, civil_add_days_ours)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int      k = (int)i & (N_STAMPS - 1);
        const mu_civil c = mu_civil_from_days(k_s[k] / 86400);

        acc += (int64_t)mu_civil_add_days(c, 45).year * 10000;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}

MU_BENCH(time, civil_add_months_ours)
{
    uint64_t i;   /* MU_BENCH_ITERATE counts in uint64_t */
    int64_t acc = 0;

    MU_BENCH_ITERATE(i) {
        const int      k = (int)i & (N_STAMPS - 1);
        const mu_civil c = mu_civil_from_days(k_s[k] / 86400);

        acc += (int64_t)mu_civil_add_months(c, (int32_t)(k & 7)).month;
    }
    MU_BENCH_DO_NOT_OPTIMIZE(acc);
}


/* ------------------------------------------------------------------ */
/* interleaved paired A/B                                              */
/* ------------------------------------------------------------------ */
/*
 * Why this section sits next to the mu_bench rows instead of replacing them:
 * mu_bench reports nanoseconds, and this guest's clock drifts between 1.6 and
 * 3.1 GHz while 28 rows run, so a row measured in a slow window can look 2x
 * worse than the same row measured in a fast one. The run-to-run drift is bigger
 * than most of the effects worth reporting: "fmt_ms ours vs naive digits" came
 * out 1.16x on one run's medians and 1.47x on the same run's mins, which is not
 * evidence of anything.
 *
 * So every pair below is measured 101 times with both sides running back to back
 * inside the same frequency window, alternating which side goes first so any
 * warm-up asymmetry cancels. Two adjacent batches see the same core state, so
 * their RATIO is nearly immune to the drift even though each absolute number is
 * not, and the p10..p90 spread across rounds says how much to believe it.
 *
 * The mu_bench table is still the headline (it is the house format and it is
 * reproducible); this table is what decides whether a difference is real.
 *
 *   make -C perf_test time-pairs        # or ./bench_time --pairs
 */

#define PAIRS_REPS   20000u
#define PAIRS_ROUNDS 101

static volatile int64_t g_sink;

#if defined(__GNUC__)
#  define PAIRS_NOINLINE __attribute__((noinline))
#else
#  define PAIRS_NOINLINE
#endif

/* Each body is one batch of PAIRS_REPS operations, and every one of them indexes
 * the stamp table by i so nothing is loop-invariant: a body that does not depend
 * on the counter is hoisted out and reports 0.000 ns, which is a row that
 * measures nothing. */

PAIRS_NOINLINE static void body_fmt_ms_ours(void)
{
    char     buf[40];
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += mu_iso8601_format(k_s[(int)i & (N_STAMPS - 1)], k_ns[(int)i & (N_STAMPS - 1)], 3,
                                 buf, sizeof buf);
    g_sink = acc;
}

PAIRS_NOINLINE static void body_fmt_ms_naive(void)
{
    char     buf[40];
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += naive_format(k_s[(int)i & (N_STAMPS - 1)], k_ns[(int)i & (N_STAMPS - 1)], 3, buf,
                            sizeof buf);
    g_sink = acc;
}

PAIRS_NOINLINE static void body_fmt_ms_libc(void)
{
    char     buf[40];
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += (int64_t)libc_format(k_s[(int)i & (N_STAMPS - 1)], k_ns[(int)i & (N_STAMPS - 1)], 1,
                                   buf, sizeof buf);
    g_sink = acc;
}

PAIRS_NOINLINE static void body_fmt_1s_ours(void)
{
    char     buf[40];
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += mu_iso8601_format(k_s[(int)i & (N_STAMPS - 1)], k_ns[(int)i & (N_STAMPS - 1)], 0,
                                 buf, sizeof buf);
    g_sink = acc;
}

PAIRS_NOINLINE static void body_fmt_1s_naive(void)
{
    char     buf[40];
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += naive_format(k_s[(int)i & (N_STAMPS - 1)], k_ns[(int)i & (N_STAMPS - 1)], 0, buf,
                            sizeof buf);
    g_sink = acc;
}

PAIRS_NOINLINE static void body_fmt_1s_libc(void)
{
    char     buf[40];
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += (int64_t)libc_format(k_s[(int)i & (N_STAMPS - 1)], k_ns[(int)i & (N_STAMPS - 1)], 0,
                                    buf, sizeof buf);
    g_sink = acc;
}

PAIRS_NOINLINE static void body_parse_1s_ours(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        int64_t  v  = 0;
        uint32_t ns = 0;

        acc += mu_iso8601_parse(g_stamp[(int)i & (N_STAMPS - 1)], 20u, &v, &ns) + v;
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_parse_1s_libc(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        struct tm tmv;

        memset(&tmv, 0, sizeof tmv);
        if (strptime(g_stamp[(int)i & (N_STAMPS - 1)], "%Y-%m-%dT%H:%M:%SZ", &tmv) != NULL)
            acc += (int64_t)timegm(&tmv);
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_civil_ours(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += (int64_t)mu_datetime_from_unix(k_s[(int)i & (N_STAMPS - 1)]).date.day;
    g_sink = acc;
}

PAIRS_NOINLINE static void body_civil_libc(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        const time_t t = (time_t)k_s[(int)i & (N_STAMPS - 1)];
        struct tm    tmv;

        if (gmtime_r(&t, &tmv) != NULL) acc += tmv.tm_mday;
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_tounix_ours(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        const int      k = (int)i & (N_STAMPS - 1);
        mu_datetime    dt;

        dt.date       = mu_civil_from_days(k_s[k] / 86400);
        dt.hour       = 0;
        dt.minute     = 0;
        dt.second     = (uint8_t)(k & 59);
        dt.nanosecond = 0u;
        acc += mu_datetime_to_unix(dt);
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_tounix_libc(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        const int      k = (int)i & (N_STAMPS - 1);
        const mu_civil c = mu_civil_from_days(k_s[k] / 86400);
        struct tm      tmv;

        memset(&tmv, 0, sizeof tmv);
        tmv.tm_year = c.year - 1900;
        tmv.tm_mon  = (int)c.month - 1;
        tmv.tm_mday = (int)c.day;
        acc += (int64_t)timegm(&tmv);
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_dur_ours(void)
{
    char     buf[40];
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += mu_dur_format_human(k_s[(int)i & (N_STAMPS - 1)] & 0x3fffffff, buf, sizeof buf);
    g_sink = acc;
}

PAIRS_NOINLINE static void body_dur_libc(void)
{
    char     buf[40];
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        const int64_t v = k_s[(int)i & (N_STAMPS - 1)] & 0x3fffffff;
        double        d = (double)v;
        const char   *u = "ns";

        if (d >= 1e9) { d *= 1e-9; u = "s"; }
        else if (d >= 1e6) { d *= 1e-6; u = "ms"; }
        else if (d >= 1e3) { d *= 1e-3; u = "us"; }
        acc += snprintf(buf, sizeof buf, "%.2f %s", d, u);
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_isoweek_ours(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        const int      k = (int)i & (N_STAMPS - 1);
        const mu_civil c = mu_civil_from_days(k_s[k] / 86400);
        int32_t        iy = 0;
        uint32_t       wk = 0, wd = 0;

        mu_civil_iso_week(c, &iy, &wk, &wd);
        acc += (int64_t)iy + wk + wd;
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_isoweek_libc(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        const time_t t = (time_t)k_s[(int)i & (N_STAMPS - 1)];
        struct tm    tmv;
        char         out[24];

        if (gmtime_r(&t, &tmv) != NULL)
            acc += (int64_t)strftime(out, sizeof out, "%G-W%V-%u", &tmv);
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_wall_1read(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i) {
        int64_t  v;
        uint32_t n;

        mu_wall_now(&v, &n);
        acc += v + (int64_t)n;
    }
    g_sink = acc;
}

PAIRS_NOINLINE static void body_wall_2read(void)
{
    uint64_t i;
    int64_t  acc = 0;

    for (i = 0; i < PAIRS_REPS; ++i)
        acc += mu_wall_unix_s() + (int64_t)mu_wall_nanos();
    g_sink = acc;
}

/* One batch, timed. The warm-up loop is not decoration: without it the first
 * round of every pair pays for a cold icache and a ramping core, which is a
 * 2-3x error on a 0.6 ms batch. */
static double pair_time(void (*fn)(void))
{
    struct timespec a;
    struct timespec b;
    uint64_t        i;

    for (i = 0; i < 8u; ++i) fn();
    clock_gettime(CLOCK_MONOTONIC, &a);
    fn();
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec);
}

static int cmp_double(const void *x, const void *y)
{
    const double a = *(const double *)x;
    const double b = *(const double *)y;

    return (a < b) ? -1 : (a > b ? 1 : 0);
}

/* sorts v in place, then picks the element at rank q */
static double quantile(double *v, int n, double q)
{
    int idx;

    qsort(v, (size_t)n, sizeof v[0], cmp_double);
    idx = (int)(q * (double)(n - 1) + 0.5);
    return v[idx];
}

static int run_pairs(void)
{
    static const struct {
        const char *what;
        void (*ours)(void);
        void (*base)(void);
    } k_pairs[] = {
        { "format ms   pair table vs div loop", body_fmt_ms_ours,   body_fmt_ms_naive   },
        { "format 1s   pair table vs div loop", body_fmt_1s_ours,   body_fmt_1s_naive   },
        { "format ms   ours vs libc",           body_fmt_ms_ours,   body_fmt_ms_libc    },
        { "format 1s   ours vs libc",           body_fmt_1s_ours,   body_fmt_1s_libc    },
        { "format ms   div loop vs libc",       body_fmt_ms_naive,  body_fmt_ms_libc    },
        { "format 1s   div loop vs libc",       body_fmt_1s_naive,  body_fmt_1s_libc    },
        { "parse 1s    ours vs libc",           body_parse_1s_ours, body_parse_1s_libc  },
        { "civil       ours vs libc",           body_civil_ours,    body_civil_libc     },
        { "to unix     ours vs libc",           body_tounix_ours,   body_tounix_libc    },
        { "duration    ours vs libc",           body_dur_ours,      body_dur_libc       },
        { "iso week    ours vs libc",           body_isoweek_ours,  body_isoweek_libc   },
        { "wall clock  one read vs two",        body_wall_1read,    body_wall_2read     },
    };
    double ratio[PAIRS_ROUNDS];
    double sa[PAIRS_ROUNDS];
    double sb[PAIRS_ROUNDS];
    size_t p;

    stamps_init();
    printf("\ninterleaved paired A/B: %d rounds of %u ops, alternating order\n", PAIRS_ROUNDS,
           PAIRS_REPS);
    printf("  a ratio below 1.000 means the first column is cheaper than the baseline\n");
    printf("  %-*s %8s %8s %8s %10s %10s\n", 33, "pair", "ratio", "p10", "p90", "med A", "med B");
    printf("  %s\n",
           "---------------------------------------------------------------------------");
    for (p = 0; p < sizeof k_pairs / sizeof k_pairs[0]; ++p) {
        int    r;
        double med_r, p10, p90;

        for (r = 0; r < PAIRS_ROUNDS; ++r) {
            double x, y;

            if ((r & 1) != 0) {
                y = pair_time(k_pairs[p].base);
                x = pair_time(k_pairs[p].ours);
            } else {
                x = pair_time(k_pairs[p].ours);
                y = pair_time(k_pairs[p].base);
            }
            ratio[r] = x / y;
            sa[r] = x / (double)PAIRS_REPS;
            sb[r] = y / (double)PAIRS_REPS;
        }
        /* quantile sorts in place: the ns/op medians are taken from the copies,
         * before ratio is sorted for its own percentiles. */
        {
            const double na = quantile(sa, PAIRS_ROUNDS, 0.5);
            const double nb = quantile(sb, PAIRS_ROUNDS, 0.5);

            p10 = quantile(ratio, PAIRS_ROUNDS, 0.10);
            p90 = quantile(ratio, PAIRS_ROUNDS, 0.90);
            med_r = quantile(ratio, PAIRS_ROUNDS, 0.5);
            printf("  %-*s %8.3f %8.3f %8.3f %8.1f ns %8.1f ns\n", 33, k_pairs[p].what, med_r, p10,
                   p90, na, nb);
        }
    }
    printf("\n");
    return 0;
}

static int mu_bench_main(int argc, char **argv);

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--pairs") == 0) return run_pairs();
    }
    return mu_bench_main(argc, argv);
}
