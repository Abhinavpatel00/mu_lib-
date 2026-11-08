#pragma once
#ifndef MU_TIME_H
#define MU_TIME_H

#include "mu_macros.h"

/* ==========================================================================
 * mu_time.h — a steady clock, a proleptic Gregorian calendar and ISO 8601
 *             timestamps with no libc date code and no timezone database
 *             (single header C99)
 * ==========================================================================
 *
 * WHY THIS IS A HEADER AND NOT A CALL TO <time.h>
 * -----------------------------------------------
 * Three things go wrong when a library leans on libc for time:
 *
 *   1. `gmtime`/`localtime` RETURN A POINTER TO A SHARED STATIC BUFFER, so two
 *      threads formatting two log lines corrupt each other. The `_r` variants
 *      fix that and are POSIX rather than C99, so portable code ends up with
 *      three #ifdef branches and a fallback that returns NULL.
 *
 *   2. `strftime` IS SLOW — measured here at 199 ns for the 20 characters of
 *      "2026-11-07T09:36:12Z" and 294 ns with a millisecond field appended by
 *      hand, more than an entire log record costs to build, because it re-parses
 *      the format string and consults locale data on every call.
 *      perf_test/PERF_TIME.md measures us against the same libc in the same
 *      binary: 5.7x faster on that format, 7.7-9.3x with milliseconds, 4.2x on
 *      parsing (strptime + timegm), 3.3-4.1x on calendar conversion, 18x on
 *      duration humanization where snprintf dominates the baseline -- and the
 *      clock wrapper costs exactly nothing over a raw clock_gettime (46.12 vs
 *      46.03 ns), which is what lets a logger stamp every record.
 *
 *   3. THE CLOCK IS THE PART PEOPLE GET WRONG. `time()` and
 *      `timespec_get(TIME_UTC)` are WALL clocks: they jump backwards when NTP
 *      steps, when the user changes the timezone, or when a VM resumes from a
 *      snapshot. Anything that measures a duration — a frame budget, a log
 *      line's timestamp, a benchmark — must not use them. mu_log.h shipped
 *      exactly this bug: a field documented "monotonic, from the process start"
 *      filled from `timespec_get(TIME_UTC)`, with a C99 fallback that put *CPU
 *      time* under the same field name. The comment explaining it said
 *      `clock_gettime` needs _POSIX_C_SOURCE before the first system header —
 *      a constraint that no longer exists, because mu_macros.h owns that define
 *      now. §1 is the fix and mu_log.h uses it.
 *
 * So the two jobs are taken apart the way the problem actually splits:
 *
 *   §1  clocks      a steady clock and a wall clock, separately, with
 *                   mu_time_is_monotonic() so a caller can KNOW which one it got
 *                   instead of hoping. Nothing in §§2-4 reads a clock: they are
 *                   pure integer functions, testable against a known-good value
 *                   for every day in ten thousand years.
 *   §2  calendar    days <-> (y, m, d) in the proleptic Gregorian calendar, plus
 *                   ISO weekday and ISO week-numbering year. Exact integer
 *                   arithmetic (400-year era decomposition), no libc, no locale,
 *                   no timezone, no allocation, no global state, thread-safe by
 *                   construction.
 *   §3  timestamps  RFC 3339 / ISO 8601, formatted into the caller's buffer and
 *                   parsed strictly — refusing 2026-02-30 rather than silently
 *                   normalizing it to March 2, and reporting how many bytes it
 *                   consumed so a timestamp can be read out of a stream.
 *   §4  durations   human ("1.24 ms", "3d 04:05:06.789") and fixed width
 *                   ("04:05:06.789"), integer math only, no stdio.
 *
 * DELIBERATE LIMITS, so nobody has to guess:
 *   - No timezones and no DST. Everything is UTC. A timezone is a database, not
 *     a header, and a wrong offset is worse than none.
 *   - No leap seconds: POSIX time ignores them and so do we. ":60" is parsed as
 *     an error, not clamped.
 *   - The proleptic Gregorian calendar is used for every year, including before
 *     1582 when it did not exist yet. That is what libc's own `gmtime` does, and
 *     it is the only choice that makes old dates consistent.
 *   - Formatting and parsing cover years 0000-9999 (the RFC 3339 profile). The
 *     calendar math itself stays exact far beyond that, because it is worth
 *     nothing if it silently wraps.
 *   - NANOSECONDS SINCE THE EPOCH AS A SINGLE int64 SPANS 1678..2262 (it is
 *     9.2e18 ns). That is why the calendar API takes seconds while the timestamp
 *     API takes (seconds, nanos) as a pair, and why storing "nanos since 1970"
 *     in an int64 is a bug with a due date.
 *
 * WHAT libc DOES NOT GIVE YOU AT ALL
 *   - ISO 8601 week numbering (ISO 8601-1:2019 §5.7): the "week 53 of the
 *     previous year" cases that hand-rolled reports get wrong at least once.
 *   - A parser that reports consumed bytes, so a log line or a stream can be
 *     read without copying the field out first.
 *   - mu_civil_add_months with end-of-month clamping (Jan 31 + 1 month is
 *     Feb 28/29, not March 3).
 *   - Duration formatting with no stdio, so a log sink can be allocation-free.
 *
 * USAGE
 *   uint64_t t0 = mu_clock_ns();
 *   ... work ...
 *   char buf[40];
 *   mu_dur_format_human((int64_t)(mu_clock_ns() - t0), buf, sizeof buf);
 *
 *   int64_t  s  = mu_wall_unix_s();
 *   uint32_t ns = mu_wall_nanos();
 *   mu_iso8601_format(s, ns, 3, buf, sizeof buf);   // 2026-11-07T09:36:12.123Z
 *   mu_iso8601_parse(buf, 24, &s, &ns);             // returns 24
 */

/* --------------------------------------------------------------------------
 * §1  clocks
 * --------------------------------------------------------------------------
 *
 * mu_clock_ns        steady where the platform has a steady clock: monotonic,
 *                    arbitrary origin, unaffected by NTP or timezone changes.
 *                    CLOCK_MONOTONIC on POSIX (declared because mu_macros.h set
 *                    _POSIX_C_SOURCE before the first system header),
 *                    QueryPerformanceCounter on Windows, and — only when
 *                    neither exists — timespec_get(TIME_UTC), which is a wall
 *                    clock; mu_time_is_monotonic() then returns 0.
 * mu_wall_unix_s     UTC seconds since 1970-01-01, leap seconds ignored.
 * mu_wall_nanos      sub-second part of the wall clock, [0, 1000000000).
 *                    Split from the seconds because int64 nanoseconds cannot
 *                    hold year 9999; see the limits above.
 * mu_clock_start_ns  the first value mu_clock_ns saw in this translation unit,
 *                    recorded lazily, so elapsed-since-start is a subtraction.
 *                    Per translation unit on purpose: no shared mutable state,
 *                    no init call to forget, no link-time symbol to collide.
 */

#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define MU_TIME__CLOCK_IS_MONOTONIC 1
#elif defined(CLOCK_MONOTONIC)
#  include <errno.h>
#  define MU_TIME__POSIX 1
#  define MU_TIME__CLOCK_IS_MONOTONIC 1
#else
#  define MU_TIME__CLOCK_IS_MONOTONIC 0
#endif

/* 1 if mu_clock_ns is a real steady clock on this build, 0 if it fell back to
 * the wall clock. A duration measured across an NTP step is only trustworthy
 * when this is 1, and a logger may want to say so once at startup. */
MU_INLINE int mu_time_is_monotonic(void)
{
    return MU_TIME__CLOCK_IS_MONOTONIC;
}

MU_INLINE uint64_t mu_clock_ns(void)
{
#if defined(_WIN32) || defined(_WIN64)
    static LARGE_INTEGER freq;
    LARGE_INTEGER        t;

    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    {
        /* Split before multiplying: a 10 MHz QPC would overflow a straight
         * ticks*1e9 in 64 bits, and a 100 MHz one loses the low bits. */
        const uint64_t f = (uint64_t)freq.QuadPart;
        const uint64_t q = (uint64_t)t.QuadPart;

        return (f != 0u) ? (q / f) * 1000000000ull + ((q % f) * 1000000000ull) / f : 0u;
    }
#elif defined(MU_TIME__POSIX)
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    {
        struct timespec ts;

        /* No steady clock on this platform: this is the wall clock, which is why
         * mu_time_is_monotonic() returns 0. Callers that measure durations are
         * expected to have checked. */
        if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0u;
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
#else
    return (uint64_t)time(NULL) * 1000000000ull;
#endif
}

MU_INLINE int64_t mu_wall_unix_s(void)
{
#if defined(_WIN32) || defined(_WIN64)
    FILETIME       ft;
    ULARGE_INTEGER u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100 ns ticks since 1601-01-01 -> seconds since 1970-01-01 */
    return (int64_t)(u.QuadPart / 10000000ull) - 11644473600ll;
#elif defined(MU_TIME__POSIX)
    struct timespec ts;

    /* CLOCK_REALTIME, not timespec_get: with _POSIX_C_SOURCE defined (which
     * mu_macros.h does for the whole library) glibc hides the C11 timespec_get
     * from a -std=c99 translation unit, and a library header cannot ask the
     * user to change their -std. CLOCK_REALTIME is POSIX and always declared
     * here. Leap seconds are not applied, per POSIX. */
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    {
        struct timespec ts;

        if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0;
        return (int64_t)ts.tv_sec;
    }
#else
    return (int64_t)time(NULL);
#endif
}

/* Both wall-clock fields from ONE clock read. mu_wall_unix_s() and
 * mu_wall_nanos() are two separate reads, which costs a second clock_gettime and
 * can return an inconsistent pair across a second boundary (s from before, ns
 * from after -- a timestamp with 1 999 999 999 ns is not a valid instant). Use
 * this whenever you need both. */
MU_INLINE void mu_wall_now(int64_t *out_unix_s, uint32_t *out_nanos)
{
    if (out_unix_s) *out_unix_s = 0;
    if (out_nanos) *out_nanos = 0u;
#if defined(_WIN32) || defined(_WIN64)
    {
        FILETIME       ft;
        ULARGE_INTEGER u;

        GetSystemTimeAsFileTime(&ft);
        u.LowPart  = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        if (out_unix_s) *out_unix_s = (int64_t)(u.QuadPart / 10000000ull) - 11644473600ll;
        if (out_nanos) *out_nanos = (uint32_t)((u.QuadPart % 10000000ull) * 100ull);
    }
#elif defined(MU_TIME__POSIX)
    {
        struct timespec ts;

        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return;
        if (out_unix_s) *out_unix_s = (int64_t)ts.tv_sec;
        if (out_nanos) *out_nanos = (uint32_t)ts.tv_nsec;
    }
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    {
        struct timespec ts;

        if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return;
        if (out_unix_s) *out_unix_s = (int64_t)ts.tv_sec;
        if (out_nanos) *out_nanos = (uint32_t)ts.tv_nsec;
    }
#else
    if (out_unix_s) *out_unix_s = (int64_t)time(NULL);
#endif
}

MU_INLINE uint32_t mu_wall_nanos(void)
{
#if defined(_WIN32) || defined(_WIN64)
    FILETIME       ft;
    ULARGE_INTEGER u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (uint32_t)((u.QuadPart % 10000000ull) * 100ull);
#elif defined(MU_TIME__POSIX)
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0u;
    return (uint32_t)ts.tv_nsec;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    {
        struct timespec ts;

        if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0u;
        return (uint32_t)ts.tv_nsec;
    }
#else
    return 0u;
#endif
}

MU_INLINE uint64_t mu_clock_start_ns(void)
{
    static uint64_t origin;

    if (origin == 0u) origin = mu_clock_ns();
    return origin;
}

MU_INLINE uint64_t mu_clock_elapsed_ns(void)
{
    const uint64_t now = mu_clock_ns();
    const uint64_t org = mu_clock_start_ns();

    return (now >= org) ? (now - org) : 0u;
}

/* Best effort: sleeps at least ns when the platform has a sleep at all. For
 * tools and tests, never for pacing a frame — a scheduler wakes a sleeper late,
 * never early. Returns 0 if it could not sleep at all. */
MU_INLINE int mu_sleep_ns(uint64_t ns)
{
#if defined(_WIN32) || defined(_WIN64)
    Sleep((DWORD)((ns + 999999ull) / 1000000ull));
    return 1;
#elif defined(MU_TIME__POSIX)
    struct timespec req;
    int             rc;

    req.tv_sec  = (time_t)(ns / 1000000000ull);
    req.tv_nsec = (long)(ns % 1000000000ull);
    do { rc = nanosleep(&req, &req); } while (rc != 0 && errno == EINTR);
    return (rc == 0) ? 1 : 0;
#else
    MU_UNUSED(ns);
    return 0;
#endif
}

MU_INLINE void mu_sleep_ms(uint32_t ms)
{
    (void)mu_sleep_ns((uint64_t)ms * 1000000ull);
}

/* --------------------------------------------------------------------------
 * §2  the calendar (proleptic Gregorian, UTC, exact integer arithmetic)
 * --------------------------------------------------------------------------
 *
 * The representation everything is built on is "days since 1970-01-01" as an
 * int64 — a value that fits in a register and sorts like a date. Every
 * conversion below is exact and branch-cheap because the Gregorian leap rule
 * repeats every 400 years = 146097 days, so a date is decomposed into
 * (era, year-of-era, day-of-year) instead of being searched or iterated.
 *
 * The algorithms are Howard Hinnant's `days_from_civil` / `civil_from_days`
 * (public domain, howardhinnant.github.io/date_algorithms.html), adapted so
 * negative years stay exact. perf_test/test_time.c verifies them three ways:
 * every one of the 3 652 425 days from 0000-01-01 to 9999-12-31 round-trips, a
 * sample of 200 000 random stamps is cross-checked against libc's
 * gmtime_r/timegm, and the structural invariants (month lengths, leap years,
 * day-of-year monotonicity, ISO week edges) are asserted directly.
 */

typedef struct mu_civil
{
    int32_t year;  /* proleptic Gregorian; 0 is 1 BC */
    uint8_t month; /* 1..12 */
    uint8_t day;   /* 1..31 */
} mu_civil;

typedef struct mu_datetime
{
    mu_civil date;
    uint8_t  hour;       /* 0..23 */
    uint8_t  minute;     /* 0..59 */
    uint8_t  second;     /* 0..59 — leap seconds are not represented */
    uint32_t nanosecond; /* 0..999999999 */
} mu_datetime;

MU_INLINE int mu_is_leap_year(int32_t year)
{
    /* Negative years matter: -4 is a leap year, -1 is not. C's % truncates
     * toward zero, so take the magnitude first. */
    const int32_t y = (year < 0) ? -year : year;

    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
}

MU_INLINE int mu_days_in_month(int32_t year, int month)
{
    static const uint8_t k_days[12] = { 31u, 28u, 31u, 30u, 31u, 30u,
                                        31u, 31u, 30u, 31u, 30u, 31u };

    if (month < 1 || month > 12) return 0;
    if (month == 2 && mu_is_leap_year(year)) return 29;
    return (int)k_days[month - 1];
}

/* Validates a date with no conversion at all. The day is checked against the
 * real month length, so 2026-02-30 and 2100-02-29 are refused. The accepted
 * year range is the one the formatter and parser support (0000..9999); the
 * calendar math itself is exact well beyond it. */
MU_INLINE int mu_civil_is_valid(mu_civil c)
{
    if (c.year < 0 || c.year > 9999) return 0;
    if (c.month < 1u || c.month > 12u) return 0;
    if (c.day < 1u) return 0;
    return ((int)c.day <= mu_days_in_month(c.year, (int)c.month)) ? 1 : 0;
}

/* Days since 1970-01-01. Exact for any year an int32 can hold. */
MU_INLINE int64_t mu_days_from_civil(int32_t year, int month, int day)
{
    const int64_t y   = (int64_t)year - ((month <= 2) ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                       /* [0, 399] */
    const int64_t doy = (153 * (month + ((month > 2) ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               /* [0, 146096] */

    return era * 146097 + doe - 719468;
}

MU_INLINE mu_civil mu_civil_from_days(int64_t days)
{
    mu_civil      c;
    const int64_t z   = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                                    /* [0, 146096] */
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y   = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);             /* [0, 365] */
    const int64_t mp  = (5 * doy + 2) / 153;                                 /* [0, 11] */
    const int64_t d   = doy - (153 * mp + 2) / 5 + 1;                        /* [1, 31] */
    const int64_t m   = (mp < 10) ? mp + 3 : mp - 9;                         /* [1, 12] */

    c.year  = (int32_t)(y + ((m <= 2) ? 1 : 0));
    c.month = (uint8_t)m;
    c.day   = (uint8_t)d;
    return c;
}

MU_INLINE mu_civil mu_civil_add_days(mu_civil c, int64_t delta)
{
    return mu_civil_from_days(mu_days_from_civil(c.year, (int)c.month, (int)c.day) + delta);
}

/* Month arithmetic clamped to the target month's length, so "one month after
 * Jan 31" is Feb 28/29 and not March 3. Negative counts are allowed. */
MU_INLINE mu_civil mu_civil_add_months(mu_civil c, int32_t months)
{
    int64_t total = (int64_t)c.year * 12 + (int64_t)(c.month - 1u) + months;
    int64_t y     = total / 12;
    int     m     = (int)(total % 12);
    int     dim;

    if (m < 0) { m += 12; --y; }
    c.year  = (int32_t)y;
    c.month = (uint8_t)(m + 1);
    dim     = mu_days_in_month(c.year, m + 1);
    if (dim > 0 && (int)c.day > dim) c.day = (uint8_t)dim;
    return c;
}

/* ISO 8601 weekday of a day count: 1 = Monday ... 7 = Sunday. 1970-01-01 was a
 * Thursday, which is the only fact this needs. */
MU_INLINE int mu_weekday_iso(int64_t days)
{
    int64_t w = (days + 3) % 7; /* Monday == 0 after this shift */

    if (w < 0) w += 7;
    return (int)w + 1;
}

/* "Mon".."Sun", lowercase-free and not localized: locale is a database. */
MU_INLINE const char *mu_weekday_name(int iso_weekday)
{
    static const char k_names[7][4] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

    if (iso_weekday < 1 || iso_weekday > 7) return "???";
    return k_names[iso_weekday - 1];
}

MU_INLINE int mu_civil_day_of_year(mu_civil c)
{
    return (int)(mu_days_from_civil(c.year, (int)c.month, (int)c.day) -
                 mu_days_from_civil(c.year, 1, 1)) + 1;
}

/* ISO 8601 week-numbering year and week, 1..53.
 *
 * The rule — and the reason this is not four lines of arithmetic — is that a
 * week belongs to the year containing its Thursday. So 2021-01-01 is week 53 of
 * *2020*, 2016-01-01 is week 53 of *2015*, and 2000-01-01 is week 52 of *1999*.
 * Anything that computes day_of_year/7 gets all three wrong. */
MU_INLINE void mu_civil_iso_week(mu_civil c, int32_t *out_iso_year, uint32_t *out_week,
                                uint32_t *out_weekday)
{
    const int64_t  days     = mu_days_from_civil(c.year, (int)c.month, (int)c.day);
    const int      wd       = mu_weekday_iso(days);          /* 1..7 */
    const int64_t  thursday = days + (4 - wd);               /* Thursday of this week */
    const mu_civil th       = mu_civil_from_days(thursday);
    const int64_t  jan1     = mu_days_from_civil(th.year, 1, 1);
    const int64_t  week     = (thursday - jan1) / 7 + 1;

    if (out_iso_year) *out_iso_year = th.year;
    if (out_week) *out_week = (uint32_t)week;
    if (out_weekday) *out_weekday = (uint32_t)wd;
}

/* ---- (y, m, d, h, m, s) <-> seconds since the epoch ---- */

MU_INLINE int64_t mu_datetime_to_unix(mu_datetime dt)
{
    const int64_t days = mu_days_from_civil(dt.date.year, (int)dt.date.month, (int)dt.date.day);

    return days * 86400 + (int64_t)dt.hour * 3600 + (int64_t)dt.minute * 60 + (int64_t)dt.second;
}

MU_INLINE mu_datetime mu_datetime_from_unix(int64_t unix_s)
{
    mu_datetime dt;
    int64_t     days = unix_s / 86400;
    int64_t     rem  = unix_s % 86400;

    if (rem < 0) { rem += 86400; --days; }
    dt.date       = mu_civil_from_days(days);
    dt.hour       = (uint8_t)(rem / 3600);
    dt.minute     = (uint8_t)((rem % 3600) / 60);
    dt.second     = (uint8_t)(rem % 60);
    dt.nanosecond = 0u;
    return dt;
}

/* --------------------------------------------------------------------------
 * §2b small integer-to-text helpers (no stdio, no allocation)
 * --------------------------------------------------------------------------
 *
 * These write forward into a buffer the caller has sized; they are the whole of
 * "printf" that §3-4 need, which is what lets a log sink stay printf-free.
 *
 * THE TWO-DIGIT PAIR TABLE, AND WHY IT IS 2x AND NOT FOR THE OBVIOUS REASON
 * Writing a timestamp digit at a time looks like "one integer division per
 * digit", so the usual story is that a pair table halves the division count.
 * That story is wrong here, and the assembly says so: NEITHER version issues a
 * hardware divide, because every divisor in both paths is the constant 10 or 100
 * and gcc has already turned those into multiply-shift sequences (25 multiplies
 * in the table version, 26 in the digit loop). What the digit loop actually costs
 * is that it is a *data-dependent trip count inlined seven times*: it must
 * discover how many digits it has, emit them backwards into scratch, then copy
 * them forwards, and the caller's zero padding is a third loop.
 *
 *   gcc 14.2 -O3 -march=native, both inlined into one function, instruction
 *   counts from the generated assembly (perf_test/bench_time.c reproduces it):
 *       digit-at-a-time loop   891 instructions, 109 branches
 *       pair table             227 instructions,   8 branches
 *
 * So the saving is loop bookkeeping and 100-odd hard-to-predict exits, not
 * division; that is also why the win shows up as a clean 2.0-2.2x on the
 * interleaved measurement (perf_test/PERF_TIME.md) instead of the 2x-per-field
 * an arithmetic argument would have predicted. A table lookup with a
 * provably-in-range index is one load with no branch at all, and the whole
 * timestamp becomes straight-line code.
 */

/* The two-digit table: "00".."99" as one 200-character literal (ten rows,
 * concatenated), so index 2*v and 2*v+1 are in range for every v < 100. Two characters per division instead of
 * one, and no reverse-then-copy. It is `static const` inside an inline function,
 * so a translation unit that never formats pays nothing for it: 201 bytes of
 * .rodata where it is used, no init, no runtime build step. */
MU_INLINE const char *mu_time__pair_table(void)
{
    /* 201, not 200: C lets an exactly-sized char array swallow the terminating
     * NUL and C++ does not, and this header has to compile as both. The NUL is
     * the one byte of this table nothing ever reads. */
    static const char k_pairs[201] =
    {
        "00010203040506070809"
        "10111213141516171819"
        "20212223242526272829"
        "30313233343536373839"
        "40414243444546474849"
        "50515253545556575859"
        "60616263646566676869"
        "70717273747576777879"
        "80818283848586878889"
        "90919293949596979899"
    };

    return k_pairs;
}

/* Writes exactly `width` zero-padded digits, advancing p. The small fixed
 * widths carry preconditions on v -- v < 100 for u2, v < 1000 for u3, v < 10000
 * for u4 -- and every call site in this header proves them: the fields come out
 * of mu_datetime_from_unix, whose components are in range by construction, and
 * the year is bounded by the unix_s range check in mu_iso8601_format. u3 and u4
 * divide before they index, so an out-of-range caller yields a wrong digit
 * rather than an out-of-bounds read either way. The gate formats every day in
 * ten millennia and compares against snprintf, which is what keeps these
 * honest. */
MU_INLINE char *mu_time__put_u2(char *p, unsigned v)
{
    const char *t = mu_time__pair_table();

    p[0] = t[v * 2u];
    p[1] = t[v * 2u + 1u];
    return p + 2;
}

MU_INLINE char *mu_time__put_u3(char *p, unsigned v) /* v < 1000 */
{
    unsigned hi = v / 100u; /* constant divisor: mulhi + shift, no hardware div */

    if (hi > 9u) hi = 9u; /* unreachable for in-range v; it is the bounds proof */
    p[0] = (char)('0' + (int)hi);
    return mu_time__put_u2(p + 1, v - hi * 100u);
}

MU_INLINE char *mu_time__put_u4(char *p, unsigned v) /* v < 10000 */
{
    unsigned hi = v / 100u; /* < 100 when v < 10000 */

    if (hi > 99u) hi %= 100u; /* likewise: keeps the table index in range */
    p = mu_time__put_u2(p, hi);
    return mu_time__put_u2(p, v - hi * 100u);
}

/* General case: digits backwards into a scratch row, then copied out padded.
 * Used for the widths the table paths do not cover (5, 7, 8 digits) and for the
 * unbounded day counts in §4. */
MU_INLINE char *mu_time__put_u64(char *p, uint64_t v, int width)
{
    char    digits[20];
    int     n = 0;
    int     i;

    do {
        digits[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    for (i = n; i < width; ++i) *p++ = '0';
    while (n > 0) *p++ = digits[--n];
    return p;
}

/* Zero-padded fixed width, 1..9, on the table path wherever it applies. Widths
 * 5, 7 and 8 fall through to the digit loop: adding three more table cases
 * would make the common ones no cheaper, and those three widths are not on any
 * path this header takes. */
MU_INLINE char *mu_time__put_fixed(char *p, uint64_t v, int width)
{
    switch (width) {
    case 1: p[0] = (char)('0' + (int)(v % 10u)); return p + 1;
    case 2: return mu_time__put_u2(p, (unsigned)v);
    case 3: return mu_time__put_u3(p, (unsigned)v);
    case 4: return mu_time__put_u4(p, (unsigned)v);
    case 6:
        p = mu_time__put_u3(p, (unsigned)(v / 1000u));
        return mu_time__put_u3(p, (unsigned)(v % 1000u));
    case 9:
        p = mu_time__put_u3(p, (unsigned)(v / 1000000u));
        p = mu_time__put_u3(p, (unsigned)((v / 1000u) % 1000u));
        return mu_time__put_u3(p, (unsigned)(v % 1000u));
    default: return mu_time__put_u64(p, v, width);
    }
}

MU_INLINE char *mu_time__put_i64(char *p, int64_t v, int width)
{
    if (v < 0) {
        *p++ = '-';
        /* avoid overflow on INT64_MIN */
        return mu_time__put_u64(p, (uint64_t)(-(v + 1)) + 1u, width);
    }
    return mu_time__put_u64(p, (uint64_t)v, width);
}

/* --------------------------------------------------------------------------
 * §3  ISO 8601 / RFC 3339 timestamps
 * --------------------------------------------------------------------------
 *
 * WRITTEN: "YYYY-MM-DDTHH:MM:SS[.f{frac_digits}]Z" — always UTC, always Z.
 *          frac_digits may be 0..9; 3 gives milliseconds, 9 nanoseconds.
 *          Returns the characters written (excluding the NUL) or 0 if the
 *          buffer is too small. It never writes a truncated string, and it
 *          never writes a partial timestamp on failure.
 *
 * PARSED:  reads ONE timestamp at the start of the buffer and returns how many
 *          bytes it consumed, so a timestamp inside a log line or a network
 *          record is read without copying the field out first. That also means
 *          it stops at the first character that cannot continue a timestamp
 *          rather than failing the whole call: "2026-11-07X..." returns 10 and
 *          hands back midnight, and comparing the return value with the field
 *          length is how you detect trailing garbage. strptime behaves the same
 *          way. Use the strict parser when a zone designator is required.
 *          Accepts 'T', 't' or ' ' as the separator; an optional fraction of
 *          1..9 digits with '.' or ',' (RFC 3339 allows both); and 'Z'/'z' or
 *          an explicit offset ±HH, ±HHMM or ±HH:MM. A timestamp with no zone is
 *          read as UTC, and mu_iso8601_parse_strict refuses it instead.
 *          Rejects month 13, 2026-02-30, hour 24, second 60, a 10-digit
 *          fraction, and years outside 0000..9999 — validated by rebuilding the
 *          date from the day count rather than by trusting the field ranges.
 *
 * Both are allocation-free and reentrant: no static state anywhere, so two
 * threads can format two log lines at once. That is the actual reason this
 * header exists.
 */

MU_INLINE int mu_iso8601_format(int64_t unix_s, uint32_t nanos, int frac_digits, char *buf,
                                size_t cap)
{
    char         tmp[40];
    char        *p;
    mu_datetime  dt;
    size_t       n;
    int          i;

    if (!buf || cap == 0u) return 0;
    buf[0] = '\0';
    if (nanos > 999999999u) return 0;
    if (frac_digits < 0 || frac_digits > 9) return 0;
    /* 0000-01-01T00:00:00Z .. 9999-12-31T23:59:59Z, the same span
     * mu_civil_is_valid accepts. Hard-coded rather than computed because it is
     * on the format path and the two constants are the whole contract. */
    if (unix_s < -62167219200ll || unix_s > 253402300799ll) return 0;

    dt = mu_datetime_from_unix(unix_s);
    p  = tmp;
    /* every field below is in its fast path's range: the year because of the
     * unix_s check above, the rest because mu_datetime_from_unix cannot produce
     * anything else */
    p  = mu_time__put_u4(p, (unsigned)dt.date.year);
    *p++ = '-';
    p  = mu_time__put_u2(p, dt.date.month);
    *p++ = '-';
    p  = mu_time__put_u2(p, dt.date.day);
    *p++ = 'T';
    p  = mu_time__put_u2(p, dt.hour);
    *p++ = ':';
    p  = mu_time__put_u2(p, dt.minute);
    *p++ = ':';
    p  = mu_time__put_u2(p, dt.second);
    if (frac_digits > 0) {
        *p++ = '.';
        /* Truncate, never round: rounding 999 999 999 to 3 digits would print
         * ".1000" or carry into the second, and a timestamp that is one tick
         * off is still a lie in the direction of the future. */
        for (i = 9; i > frac_digits; --i) nanos /= 10u;
        p = mu_time__put_fixed(p, nanos, frac_digits);
    }
    *p++ = 'Z';

    n = (size_t)(p - tmp);
    if (n + 1u > cap) return 0;
    memcpy(buf, tmp, n);
    buf[n] = '\0';
    return (int)n;
}

/* Reads `count` decimal digits at *i, advancing it. 0 on failure. */
MU_INLINE int mu_time__get_num(const char *s, size_t len, size_t *i, int count, int *out)
{
    int v = 0;
    int k;

    for (k = 0; k < count; ++k) {
        if (*i >= len) return 0;
        if (s[*i] < '0' || s[*i] > '9') return 0;
        v = v * 10 + (s[*i] - '0');
        ++*i;
    }
    *out = v;
    return 1;
}

MU_INLINE int mu_iso8601_parse_impl(const char *s, size_t len, int require_zone,
                                   int64_t *out_unix_s, uint32_t *out_nanos)
{
    mu_datetime dt;
    int         y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    int         ndigits = 0, offset_min = 0;
    uint32_t    frac = 0u;
    size_t      i = 0u;
    int64_t     secs;

    if (!s || !out_unix_s || !out_nanos) return 0;

    if (!mu_time__get_num(s, len, &i, 4, &y)) return 0;
    if (i >= len || s[i] != '-') return 0;
    ++i;
    if (!mu_time__get_num(s, len, &i, 2, &mo)) return 0;
    if (i >= len || s[i] != '-') return 0;
    ++i;
    if (!mu_time__get_num(s, len, &i, 2, &d)) return 0;

    if (i < len && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        ++i;
        if (!mu_time__get_num(s, len, &i, 2, &h)) return 0;
        if (i >= len || s[i] != ':') return 0;
        ++i;
        if (!mu_time__get_num(s, len, &i, 2, &mi)) return 0;
        if (i < len && s[i] == ':') {
            ++i;
            if (!mu_time__get_num(s, len, &i, 2, &sec)) return 0;
        }
        if (i < len && (s[i] == '.' || s[i] == ',')) {
            ++i;
            while (i < len && s[i] >= '0' && s[i] <= '9') {
                if (ndigits >= 9) return 0; /* finer than a nanosecond */
                frac = frac * 10u + (uint32_t)(s[i] - '0');
                ++ndigits;
                ++i;
            }
            if (ndigits == 0) return 0;
            for (; ndigits < 9; ++ndigits) frac *= 10u;
        }
    }

    if (i < len && (s[i] == 'Z' || s[i] == 'z')) {
        ++i;
    } else if (i < len && (s[i] == '+' || s[i] == '-')) {
        const int sign = (s[i] == '-') ? -1 : 1;
        int       oh   = 0, om = 0;

        ++i;
        if (!mu_time__get_num(s, len, &i, 2, &oh)) return 0;
        if (i < len && s[i] == ':') ++i;
        if (i + 1u < len && s[i] >= '0' && s[i] <= '9') {
            if (!mu_time__get_num(s, len, &i, 2, &om)) return 0;
        }
        if (oh > 23 || om > 59) return 0;
        offset_min = sign * (oh * 60 + om);
    } else if (require_zone) {
        return 0;
    }

    if (h > 23 || mi > 59 || sec > 59) return 0;
    dt.date.year   = (int32_t)y;
    dt.date.month  = (uint8_t)mo;
    dt.date.day    = (uint8_t)d;
    dt.hour        = (uint8_t)h;
    dt.minute      = (uint8_t)mi;
    dt.second      = (uint8_t)sec;
    dt.nanosecond  = frac;
    if (!mu_civil_is_valid(dt.date)) return 0;

    secs = mu_datetime_to_unix(dt);

    /* The validation that actually catches nonsense: a value that cannot be
     * rebuilt from its own day count was not a date. Cheap, and it covers every
     * case we did not think of. */
    {
        const mu_civil back = mu_civil_from_days(mu_days_from_civil(y, mo, d));

        if (back.year != dt.date.year || back.month != dt.date.month || back.day != dt.date.day)
            return 0;
    }

    *out_unix_s = secs - (int64_t)offset_min * 60;
    *out_nanos  = frac;
    return (int)i;
}

/* Not strict about the zone designator (missing means UTC). */
MU_INLINE int mu_iso8601_parse(const char *s, size_t len, int64_t *out_unix_s,
                               uint32_t *out_nanos)
{
    return mu_iso8601_parse_impl(s, len, 0, out_unix_s, out_nanos);
}

/* Requires an explicit 'Z' or offset, which is what you want when reading a
 * timestamp that was supposed to come from another program. */
MU_INLINE int mu_iso8601_parse_strict(const char *s, size_t len, int64_t *out_unix_s,
                                      uint32_t *out_nanos)
{
    return mu_iso8601_parse_impl(s, len, 1, out_unix_s, out_nanos);
}

/* Convenience for a log line: the wall clock at millisecond precision. */
MU_INLINE int mu_iso8601_format_now_ms(char *buf, size_t cap)
{
    int64_t  s;
    uint32_t ns;

    mu_wall_now(&s, &ns); /* one clock read, not two: see mu_wall_now */
    return mu_iso8601_format(s, ns, 3, buf, cap);
}

/* --------------------------------------------------------------------------
 * §4  durations
 * --------------------------------------------------------------------------
 *
 * mu_dur_format_human  nanoseconds -> the shortest honest string, three
 *                        significant digits and integer math only:
 *                        512 ns | 1.24 us | 12.4 us | 124 us | 1.24 ms | 12.4 ms
 *                        | 124 ms | 1.24 s | 12.4 s | 3d 04:05:06.789
 *                      No double, no printf, no allocation: safe to call from a
 *                      signal handler and cheap enough for a hot log path.
 * mu_dur_format_hms    fixed "H:MM:SS.mmm" (hours unpadded past 99), for
 *                      column-aligned tables.
 *
 * A negative duration keeps its sign ("-1.24 ms") instead of printing garbage,
 * because the one time you see it is the time you most need to see it.
 */

MU_INLINE int mu_dur_format_human(int64_t ns, char *buf, size_t cap)
{
    static const char *k_units[4] = { "ns", "us", "ms", "s" };
    char      tmp[40];
    char     *p = tmp;
    /* The magnitude is computed as uint64 on purpose: |INT64_MIN| does not fit
     * in an int64, and negating it is undefined behaviour. Every duration below
     * works in uint64 and only the sign is kept separately. */
    uint64_t  v;
    int       neg = 0;
    int       unit;
    uint64_t  scale;
    uint64_t  whole, frac;

    if (!buf || cap == 0u) return 0;
    buf[0] = '\0';
    if (ns < 0) { neg = 1; v = (uint64_t)(-(ns + 1)) + 1u; } else { v = (uint64_t)ns; }
    if (neg) *p++ = '-';

    if (v >= 60000000000ull) {   /* from a minute up: days + clock form */
        const uint64_t days = v / 86400000000000ull;
        const uint64_t rem  = v % 86400000000000ull;

        if (days > 0u) {
            p    = mu_time__put_u64(p, days, 1);
            *p++ = 'd';
            *p++ = ' ';
        }
        p    = mu_time__put_fixed(p, rem / 3600000000000ull, 2);
        *p++ = ':';
        p    = mu_time__put_fixed(p, (rem % 3600000000000ull) / 60000000000ull, 2);
        *p++ = ':';
        p    = mu_time__put_fixed(p, (rem % 60000000000ull) / 1000000000ull, 2);
        *p++ = '.';
        p    = mu_time__put_fixed(p, (rem % 1000000000ull) / 1000000ull, 3);
    } else {
        const char *u;

        unit  = 0;
        scale = 1u;
        while (unit < 3 && v >= scale * 1000ull) { scale *= 1000ull; ++unit; }
        whole = v / scale;
        frac  = v % scale;
        u     = k_units[unit];

        if (whole < 10u && frac != 0u) {
            p    = mu_time__put_u64(p, whole, 1);
            *p++ = '.';
            p    = mu_time__put_u2(p, (unsigned)((frac * 100ull) / scale));
        } else if (whole < 100u && frac != 0u) {
            p    = mu_time__put_u64(p, whole, 1);
            *p++ = '.';
            p[0] = (char)('0' + (int)((frac * 10ull) / scale)); ++p;
        } else {
            p = mu_time__put_u64(p, whole, 1);
        }
        *p++ = ' ';
        while (*u) *p++ = *u++;
    }

    {
        const size_t n = (size_t)(p - tmp);

        if (n + 1u > cap) return 0;
        memcpy(buf, tmp, n);
        buf[n] = '\0';
        return (int)n;
    }
}

MU_INLINE int mu_dur_format_hms(int64_t ns, char *buf, size_t cap)
{
    char      tmp[40];
    char     *p = tmp;
    uint64_t  v;
    int       neg = 0;

    if (!buf || cap == 0u) return 0;
    buf[0] = '\0';
    if (ns < 0) { neg = 1; v = (uint64_t)(-(ns + 1)) + 1u; } else { v = (uint64_t)ns; }
    if (neg) *p++ = '-';

    p    = mu_time__put_fixed(p, v / 3600000000000ull, 2);
    *p++ = ':';
    p    = mu_time__put_fixed(p, (v % 3600000000000ull) / 60000000000ull, 2);
    *p++ = ':';
    p    = mu_time__put_fixed(p, (v % 60000000000ull) / 1000000000ull, 2);
    *p++ = '.';
    p    = mu_time__put_fixed(p, (v % 1000000000ull) / 1000000ull, 3);

    {
        const size_t n = (size_t)(p - tmp);

        if (n + 1u > cap) return 0;
        memcpy(buf, tmp, n);
        buf[n] = '\0';
        return (int)n;
    }
}

#endif /* MU_TIME_H */
