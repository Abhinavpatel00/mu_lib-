/* test_time.c -- gate for mu/mu_time.h.
 *
 * The calendar is the part that has to be right for a hundred years, so it is
 * checked three independent ways:
 *
 *   1. EXHAUSTIVELY: every one of the 3 652 059 days from 0000-01-01 to
 *      9999-12-31 is converted to a day count and back, and the round trip must
 *      be the identity. A one-day drift anywhere in ten millennia fails here.
 *   2. AGAINST libc: 200 000 pseudo-random timestamps (both directions, both
 *      sides of the epoch) are compared field by field with gmtime_r/timegm.
 *      This is the check that catches "we are self-consistent but wrong".
 *   3. STRUCTURALLY: month lengths, leap years, the weekday advancing by one
 *      per day and two per leap day, and the ISO week-numbering edge cases that
 *      a day_of_year/7 implementation gets wrong.
 *
 * The timestamp parser is checked by what it REFUSES as much as by what it
 * accepts: 2026-02-30, 2100-02-29, hour 24, second 60, a 10-digit fraction and
 * year 10000 are all errors, not normalizations.
 *
 * _XOPEN_SOURCE is defined first so this file can use timegm and strptime as the
 * oracle; the library header itself never needs them.
 */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mu_time.h"

static int g_checks, g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL %s\n", what);
    }
}

/* Same, but for the millions of assertions inside the exhaustive loops: counted
 * like every other check, printed at most 25 times. */
static void check_n(int cond)
{
    ++g_checks;
    if (!cond) {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL in-loop assertion (see the loop's own summary)\n");
    }
}

static void check_i64(int64_t got, int64_t want, const char *what)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL %s (got %lld want %lld)\n", what, (long long)got,
                                  (long long)want);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    ++g_checks;
    if (!got || strcmp(got, want) != 0) {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL %s (got \"%s\" want \"%s\")\n", what, got ? got : "(null)",
                                  want);
    }
}

static uint32_t g_rs = 0x9e3779b9u;
static uint32_t rnd(void)
{
    g_rs ^= g_rs << 13u;
    g_rs ^= g_rs >> 17u;
    g_rs ^= g_rs << 5u;
    return g_rs;
}

/* ---------------------------------------------------------------- */
/* 1. exhaustive round trip over ten millennia                      */
/* ---------------------------------------------------------------- */

static void test_calendar_exhaustive(void)
{
    const int64_t first = mu_days_from_civil(0, 1, 1);
    const int64_t last  = mu_days_from_civil(9999, 12, 31);
    int64_t       d;
    int64_t       bad = 0;
    int64_t       n   = 0;

    /* years 0..9999 inclusive is exactly 25 * 400-year Gregorian cycles, and a
     * cycle is 146097 days: 3 652 425, not the 3 652 059 that 1..9999 gives. */
    check_i64(last - first + 1, 3652425, "calendar: ten millennia is 3 652 425 days");

    for (d = first; d <= last; ++d) {
        const mu_civil c   = mu_civil_from_days(d);
        const int64_t back = mu_days_from_civil(c.year, (int)c.month, (int)c.day);

        ++n;
        check_n(back == d && mu_civil_is_valid(c));
        if (back != d || !mu_civil_is_valid(c)) {
            if (bad < 3)
                printf("  first drift: day %lld -> %d-%u-%u -> %lld\n", (long long)d, c.year,
                       c.month, c.day, (long long)back);
            ++bad;
        }
    }
    check_i64(bad, 0, "calendar: every day in 0000..9999 round-trips");
    check_i64(n, 3652425, "calendar: the exhaustive loop covered every day");
}

/* ---------------------------------------------------------------- */
/* 2. cross-check against libc                                      */
/* ---------------------------------------------------------------- */

static void test_calendar_vs_libc(void)
{
    int     i;
    int64_t mismatches_gm = 0, mismatches_tm = 0, compared = 0;

    /* 1970..2100 forward, then the same range backwards through timegm, then a
     * slab of pre-epoch timestamps (gmtime_r handles negatives; so must we). */
    for (i = 0; i < 200000; ++i) {
        const int64_t s = (i < 150000) ? (int64_t)(rnd() % 4102444800u)  /* 1970..2100 */
                                       : -(int64_t)(rnd() % 2000000000u); /* 1906..1970 */
        struct tm     tm_ref;
        time_t        tt = (time_t)s;
        mu_datetime   dt;

        if (!gmtime_r(&tt, &tm_ref)) continue;
        dt = mu_datetime_from_unix(s);
        ++compared;
        {
            const int ok = (dt.date.year == (int32_t)tm_ref.tm_year + 1900 &&
                            dt.date.month == (uint8_t)tm_ref.tm_mon + 1u &&
                            dt.date.day == (uint8_t)tm_ref.tm_mday && dt.hour == (uint8_t)tm_ref.tm_hour &&
                            dt.minute == (uint8_t)tm_ref.tm_min && dt.second == (uint8_t)tm_ref.tm_sec);

            check_n(ok); /* counted per stamp, not folded into one assertion */
            if (!ok) {
                if (mismatches_gm < 3)
                    printf("  gmtime_r mismatch at %lld: libc %04d-%02d-%02d %02d:%02d:%02d, ours "
                           "%04d-%02u-%02u %02u:%02u:%02u\n",
                           (long long)s, tm_ref.tm_year + 1900, tm_ref.tm_mon + 1, tm_ref.tm_mday,
                           tm_ref.tm_hour, tm_ref.tm_min, tm_ref.tm_sec, dt.date.year, dt.date.month,
                           dt.date.day, dt.hour, dt.minute, dt.second);
                ++mismatches_gm;
            }
        }

        /* and back: timegm on libc's own struct must equal our conversion */
        {
            const int64_t back = mu_datetime_to_unix(dt);

            check_n(back == s);
            if (back != s) {
                if (mismatches_tm < 3) printf("  to_unix mismatch at %lld -> %lld\n", (long long)s,
                                              (long long)back);
                ++mismatches_tm;
            }
            /* the oracle itself must agree with the stamp we started from, or we
             * are comparing two broken things */
            check_n(timegm(&tm_ref) == (time_t)s);
        }
    }
    check_i64(mismatches_gm, 0, "calendar: mu_datetime_from_unix agrees with gmtime_r");
    check_i64(mismatches_tm, 0, "calendar: mu_datetime_to_unix round-trips every sampled stamp");
    check(compared > 190000, "calendar: libc cross-check actually compared ~200k stamps");
}

/* ---------------------------------------------------------------- */
/* 3. structural invariants                                         */
/* ---------------------------------------------------------------- */

static void test_calendar_invariants(void)
{
    /* leap years, including the negative and century cases */
    check(mu_is_leap_year(2024) == 1, "leap: 2024");
    check(mu_is_leap_year(2023) == 0, "leap: 2023");
    check(mu_is_leap_year(1900) == 0, "leap: 1900 is not (century)");
    check(mu_is_leap_year(2000) == 1, "leap: 2000 is (400)");
    check(mu_is_leap_year(2100) == 0, "leap: 2100 is not");
    check(mu_is_leap_year(0) == 1, "leap: year 0");
    check(mu_is_leap_year(-4) == 1, "leap: -4");
    check(mu_is_leap_year(-1) == 0, "leap: -1");

    check(mu_days_in_month(2024, 2) == 29, "month: Feb 2024");
    check(mu_days_in_month(2023, 2) == 28, "month: Feb 2023");
    check(mu_days_in_month(2024, 13) == 0, "month: 13 is rejected");
    check(mu_days_in_month(2024, 0) == 0, "month: 0 is rejected");
    check(mu_days_in_month(2024, 7) == 31, "month: July");

    /* validity, including the classic impossible dates */
    {
        const mu_civil good = { 2024, 2, 29 };
        const mu_civil bad1 = { 2026, 2, 30 };
        const mu_civil bad2 = { 2100, 2, 29 };
        const mu_civil bad3 = { 2024, 4, 31 };
        const mu_civil bad4 = { -1, 1, 1 };

        check(mu_civil_is_valid(good), "valid: 2024-02-29");
        check(!mu_civil_is_valid(bad1), "valid: 2026-02-30 refused");
        check(!mu_civil_is_valid(bad2), "valid: 2100-02-29 refused");
        check(!mu_civil_is_valid(bad3), "valid: 2024-04-31 refused");
        check(!mu_civil_is_valid(bad4), "valid: negative year outside the supported range");
    }

    /* day of year */
    {
        const mu_civil jan1  = { 2024, 1, 1 };
        const mu_civil dec31 = { 2024, 12, 31 };
        const mu_civil feb29 = { 2024, 2, 29 };
        const mu_civil mar1  = { 2023, 3, 1 };

        check(mu_civil_day_of_year(jan1) == 1, "doy: Jan 1 is 1");
        check(mu_civil_day_of_year(dec31) == 366, "doy: Dec 31 2024 is 366");
        check(mu_civil_day_of_year(feb29) == 60, "doy: Feb 29 is 60");
        check(mu_civil_day_of_year(mar1) == 60, "doy: Mar 1 2023 is 60 (non-leap)");
    }

    /* add_days across boundaries */
    {
        const mu_civil dec31 = { 2023, 12, 31 };

        check_str(mu_weekday_name(mu_weekday_iso(mu_days_from_civil(1970, 1, 1))), "Thu",
                  "weekday: 1970-01-01 was a Thursday");
        check_str(mu_weekday_name(mu_weekday_iso(mu_days_from_civil(2000, 1, 1))), "Sat",
                  "weekday: 2000-01-01 was a Saturday");
        check_str(mu_weekday_name(mu_weekday_iso(mu_days_from_civil(2026, 11, 7))), "Sat",
                  "weekday: 2026-11-07 is a Saturday");

        {
            const mu_civil n = mu_civil_add_days(dec31, 1);

            check(n.year == 2024 && n.month == 1 && n.day == 1, "add_days: 2023-12-31 + 1");
        }
        {
            const mu_civil n = mu_civil_add_days((mu_civil){ 2024, 2, 28 }, 1);

            check(n.year == 2024 && n.month == 2 && n.day == 29, "add_days: into a leap day");
        }
        {
            const mu_civil n = mu_civil_add_days((mu_civil){ 2024, 1, 1 }, -1);

            check(n.year == 2023 && n.month == 12 && n.day == 31, "add_days: negative delta");
        }
        {
            const int64_t span = mu_days_from_civil(9999, 12, 31) - mu_days_from_civil(1970, 1, 1);
            const mu_civil n = mu_civil_add_days((mu_civil){ 1970, 1, 1 }, span);

            check(n.year == 9999 && n.month == 12 && n.day == 31, "add_days: to the last supported day");
            check_i64(span, 2932896, "add_days: that span is 2 932 896 days");
        }
    }

    /* add_months clamps instead of rolling over */
    {
        const mu_civil a = mu_civil_add_months((mu_civil){ 2024, 1, 31 }, 1);
        const mu_civil b = mu_civil_add_months((mu_civil){ 2023, 1, 31 }, 1);
        const mu_civil c = mu_civil_add_months((mu_civil){ 2024, 3, 31 }, -1);
        const mu_civil d = mu_civil_add_months((mu_civil){ 2024, 12, 15 }, 13);
        const mu_civil e = mu_civil_add_months((mu_civil){ 2024, 1, 15 }, -13);

        check(a.year == 2024 && a.month == 2 && a.day == 29, "add_months: Jan 31 + 1 -> Feb 29 (leap)");
        check(b.year == 2023 && b.month == 2 && b.day == 28, "add_months: Jan 31 + 1 -> Feb 28");
        check(c.year == 2024 && c.month == 2 && c.day == 29, "add_months: Mar 31 - 1 -> Feb 29");
        check(d.year == 2026 && d.month == 1 && d.day == 15, "add_months: +13 crosses a year and a month");
        check(e.year == 2022 && e.month == 12 && e.day == 15, "add_months: -13 crosses one year back");
    }

    /* the weekday advances by one per day, and by two across a leap day */
    {
        int64_t d;
        int64_t bad = 0;

        for (d = 0; d < 20000; ++d) {
            const int w0 = mu_weekday_iso(d);
            const int w1 = mu_weekday_iso(d + 1);

            if (w0 < 1 || w0 > 7) ++bad;
            if (w1 != (w0 % 7) + 1) ++bad;
        }
        check_i64(bad, 0, "weekday: advances by exactly one per day, 1..7 wrap");
        check(mu_weekday_iso(mu_days_from_civil(2024, 3, 1)) == mu_weekday_iso(mu_days_from_civil(2024, 2, 28) + 2),
              "weekday: a leap day inserts exactly one day");
    }
}

/* ---------------------------------------------------------------- */
/* 4. ISO week numbering                                            */
/* ---------------------------------------------------------------- */

static void test_iso_week(void)
{
    /* fixtures computed independently (python datetime.isocalendar) */
    static const struct {
        int32_t y;
        uint8_t m, d;
        int32_t iso_year;
        uint32_t week, weekday;
    } k_fix[] = {
        { 2021, 1, 1, 2020, 53, 5 },  { 2015, 12, 31, 2015, 53, 4 }, { 2016, 1, 1, 2015, 53, 5 },
        { 2026, 12, 31, 2026, 53, 4 }, { 2027, 1, 1, 2026, 53, 5 },  { 2000, 1, 1, 1999, 52, 6 },
        { 1999, 12, 31, 1999, 52, 5 }, { 2024, 2, 29, 2024, 9, 4 },  { 1582, 10, 15, 1582, 41, 5 },
        { 1970, 1, 1, 1970, 1, 4 },    { 2026, 11, 7, 2026, 45, 6 },
    };
    size_t i;

    for (i = 0; i < sizeof k_fix / sizeof k_fix[0]; ++i) {
        int32_t  iy = 0;
        uint32_t wk = 0, wd = 0;
        mu_civil c;

        c.year  = k_fix[i].y;
        c.month = k_fix[i].m;
        c.day   = k_fix[i].d;
        mu_civil_iso_week(c, &iy, &wk, &wd);
        if (iy != k_fix[i].iso_year || wk != k_fix[i].week || wd != k_fix[i].weekday) {
            printf("  ISO mismatch %04d-%02u-%02u: got %d-W%u-%u want %d-W%u-%u\n", c.year, c.month,
                   c.day, iy, wk, wd, k_fix[i].iso_year, k_fix[i].week, k_fix[i].weekday);
        }
        check(iy == k_fix[i].iso_year && wk == k_fix[i].week && wd == k_fix[i].weekday,
              "iso week: fixture");
    }

    /* structural: week is 1..53, all seven days of a week agree, and the
     * ISO year is within one of the calendar year */
    {
        const int64_t start = mu_days_from_civil(1900, 1, 1);
        const int64_t end   = mu_days_from_civil(2100, 12, 31);
        int64_t       d;
        int64_t       bad = 0;

        for (d = start; d <= end; ++d) {
            const mu_civil c = mu_civil_from_days(d);
            int32_t        iy;
            uint32_t       wk, wd;

            mu_civil_iso_week(c, &iy, &wk, &wd);
            if (wk < 1u || wk > 53u) ++bad;
            if (iy < c.year - 1 || iy > c.year + 1) ++bad;
            if ((int32_t)wd != mu_weekday_iso(d)) ++bad;
            if (wd == 4u && (mu_days_from_civil(iy, 1, 1) > d)) ++bad; /* Thursday rule */
        }
        check_i64(bad, 0, "iso week: 1900..2100 is structurally consistent");
    }

    /* the Thursday rule directly, for a century: the week's Thursday is in the
     * ISO year, and the ISO week of that Thursday is the same week */
    {
        int64_t d;
        int64_t bad = 0;

        for (d = mu_days_from_civil(1990, 1, 1); d < mu_days_from_civil(2031, 1, 1); ++d) {
            const mu_civil c = mu_civil_from_days(d);
            const int      wd = mu_weekday_iso(d);
            const mu_civil th = mu_civil_from_days(d + (4 - wd));
            int32_t        iy, iy2;
            uint32_t       wk, wk2, wd2;

            mu_civil_iso_week(c, &iy, &wk, NULL);
            mu_civil_iso_week(th, &iy2, &wk2, &wd2);
            if (iy != iy2 || wk != wk2 || wd2 != 4u) ++bad;
            if (th.year != iy) ++bad;
        }
        check_i64(bad, 0, "iso week: every week's Thursday is in the ISO year it names");
    }
}

/* ---------------------------------------------------------------- */
/* 5. timestamps: format, parse, refuse                             */
/* ---------------------------------------------------------------- */

static void test_timestamps(void)
{
    char    buf[64];
    int64_t s;
    uint32_t ns;

    /* known conversions */
    check(mu_iso8601_format(0, 0, 0, buf, sizeof buf) == 20, "iso: length of a second-precision stamp");
    check_str(buf, "1970-01-01T00:00:00Z", "iso: the epoch");
    mu_iso8601_format(1794009600ll, 0, 0, buf, sizeof buf); /* 2026-11-07T00:00:00Z */
    check_str(buf, "2026-11-07T00:00:00Z", "iso: 2026-11-07");
    mu_iso8601_format(253402300799ll, 999999999u, 9, buf, sizeof buf);
    check_str(buf, "9999-12-31T23:59:59.999999999Z", "iso: the last supported instant");
    mu_iso8601_format(-62167219200ll, 0, 0, buf, sizeof buf);
    check_str(buf, "0000-01-01T00:00:00Z", "iso: year 0 formats with four digits");
    mu_iso8601_format(-62135596800ll, 0, 0, buf, sizeof buf);
    check_str(buf, "0001-01-01T00:00:00Z", "iso: 0001-01-01");
    check(mu_iso8601_format(-62167219201ll, 0, 0, buf, sizeof buf) == 0,
          "iso: one second below year 0000 is refused");
    mu_iso8601_format(-1, 0, 0, buf, sizeof buf);
    check_str(buf, "1969-12-31T23:59:59Z", "iso: one second before the epoch");

    /* fraction truncation, not rounding */
    mu_iso8601_format(0, 999999999u, 3, buf, sizeof buf);
    check_str(buf, "1970-01-01T00:00:00.999Z", "iso: fraction truncates, never carries");
    mu_iso8601_format(0, 123456789u, 6, buf, sizeof buf);
    check_str(buf, "1970-01-01T00:00:00.123456Z", "iso: 6 digits");

    /* refusals of the formatter */
    check(mu_iso8601_format(0, 1000000000u, 3, buf, sizeof buf) == 0, "iso: nanos >= 1e9 refused");
    check(mu_iso8601_format(0, 0, 10, buf, sizeof buf) == 0, "iso: 10 fraction digits refused");
    check(mu_iso8601_format(253402300800ll, 0, 0, buf, sizeof buf) == 0, "iso: year 10000 refused");
    check(mu_iso8601_format(0, 0, 3, buf, 5) == 0, "iso: a tiny buffer refuses instead of truncating");
    check(buf[0] == '\0', "iso: a refused format leaves an empty string");

    /* parse round trip over a spread of instants and precisions */
    {
        static const int fracs[4] = { 0, 3, 6, 9 };
        int          i;
        int64_t      bad  = 0;
        int64_t      used_bad = 0;

        for (i = 0; i < 20000; ++i) {
            const int64_t  base = (int64_t)(rnd() % 4294967295u) - 1000000000ll;
            const uint32_t nano = rnd() % 1000000000u;
            const int      fd   = fracs[i & 3];
            int            used;
            int64_t        back_s;
            uint32_t       back_ns;
            uint32_t       want_ns = nano;

            {
                int k;

                for (k = 9; k > fd; --k) want_ns /= 10u;
                for (k = fd; k < 9; ++k) want_ns *= 10u;
            }
            used = mu_iso8601_format(base, nano, fd, buf, sizeof buf);
            if (used <= 0) { ++bad; continue; }
            if (mu_iso8601_parse(buf, (size_t)used, &back_s, &back_ns) != used) ++bad;
            else if (back_s != base || back_ns != want_ns) ++bad;
            if (mu_iso8601_parse_strict(buf, (size_t)used, &back_s, &back_ns) != used) ++used_bad;
        }
        check_i64(bad, 0, "iso: 20 000 format->parse round trips at 0/3/6/9 digits");
        check_i64(used_bad, 0, "iso: the formatted form always carries a zone (strict parse accepts it)");
    }

    /* offsets: the same instant in another zone */
    {
        int64_t  a = 0, b = 0;
        uint32_t na = 0, nb = 0;

        check(mu_iso8601_parse("2026-11-07T15:06:12+05:30", 25, &a, &na) == 25, "parse: +05:30 accepted");
        check(mu_iso8601_parse("2026-11-07T09:36:12Z", 20, &b, &nb) == 20, "parse: Z accepted");
        check_i64(a, b, "parse: +05:30 names the same instant as Z");
        check(mu_iso8601_parse("2026-11-07T09:36:12-08:00", 25, &a, &na) == 25, "parse: -08:00 accepted");
        check_i64(a, b + 8 * 3600, "parse: -08:00 is eight hours ahead of UTC");
        check(mu_iso8601_parse("2026-11-07T09:36:12+0530", 24, &a, &na) == 24, "parse: +0530 accepted");
        check_i64(a, b - 5 * 3600 - 1800, "parse: +0530 is five and a half hours behind UTC");
        check(mu_iso8601_parse("2026-11-07T09:36:12+05", 22, &a, &na) == 22, "parse: +05 accepted");
        check_i64(a, b - 5 * 3600, "parse: +05");
        check(mu_iso8601_parse("2026-11-07T09:36:12z", 20, &a, &na) == 20, "parse: lowercase z accepted");
        check(mu_iso8601_parse("2026-11-07 09:36:12Z", 20, &a, &na) == 20, "parse: space separator accepted");
        check(mu_iso8601_parse("2026-11-07t09:36:12Z", 20, &a, &na) == 20, "parse: lowercase t accepted");
        check(mu_iso8601_parse("2026-11-07T09:36:12,5Z", 22, &a, &na) == 22, "parse: comma decimal mark");
        check_i64(na, 500000000, "parse: comma fraction is a half second");
        check(mu_iso8601_parse("2026-11-07T09:36:12.123456789Z", 30, &a, &na) == 30, "parse: 9 digits");
        check_i64(na, 123456789, "parse: 9 digits exact");
        check(mu_iso8601_parse("2026-11-07T09:36:12.1Z", 22, &a, &na) == 22, "parse: 1 digit");
        check_i64(na, 100000000, "parse: 1 digit is a tenth, not a nanosecond");
        check(mu_iso8601_parse("2026-11-07", 10, &a, &na) == 10, "parse: date only");
        check(mu_iso8601_parse("2026-11-07X00:00:00Z", 21, &a, &na) == 10,
              "parse: stops at the first byte that cannot continue a timestamp");
        check(mu_iso8601_parse("2026-11-07T09:36:12Zjunk", 24, &a, &na) == 20,
              "parse: trailing junk is visible as consumed < len");
        check(mu_iso8601_parse("2026-11-07T09:36Z", 16, &a, &na) == 16, "parse: hours and minutes only");

        /* zone-less is UTC by default, refused by the strict parser */
        check(mu_iso8601_parse("2026-11-07T09:36:12", 19, &a, &na) == 19, "parse: no zone means UTC");
        check_i64(a, b, "parse: no zone == Z");
        check(mu_iso8601_parse_strict("2026-11-07T09:36:12", 19, &a, &na) == 0,
              "parse: strict refuses a missing zone");

        /* consumed-byte reporting: a timestamp inside a longer line */
        check(mu_iso8601_parse("[2026-11-07T09:36:12Z] level=info", 35, &a, &na) == 0,
              "parse: a leading bracket is not a timestamp");
        check(mu_iso8601_parse("2026-11-07T09:36:12Z level=info", 30, &a, &na) == 20,
              "parse: reports 20 consumed bytes inside a line");
    }

    /* refusals */
    {
        static const char *k_bad[] = {
            "",
            "2026",
            "2026-11-07T",
            "2026-13-01T00:00:00Z",
            "2026-00-01T00:00:00Z",
            "2026-02-30T00:00:00Z",
            "2024-02-30T00:00:00Z",
            "2100-02-29T00:00:00Z",
            "2024-04-31T00:00:00Z",
            "2026-11-07T24:00:00Z",
            "2026-11-07T23:60:00Z",
            "2026-11-07T23:59:60Z",
            "2026-11-07T23:59:59.1234567890Z",
            "2026-11-07T23:59:59.Z",
            "2026-11-07T23:59:59+24:00",
            "2026-11-07T23:59:59+05:60",
            "99999-01-01T00:00:00Z",
            "2026-1-1T00:00:00Z",
            "26-11-07T00:00:00Z",
            "garbage",
        };
        size_t i;

        for (i = 0; i < sizeof k_bad / sizeof k_bad[0]; ++i) {
            int64_t  x  = 0;
            uint32_t xn = 0;

            if (mu_iso8601_parse(k_bad[i], strlen(k_bad[i]), &x, &xn) != 0) {
                printf("  FAIL parse accepted \"%s\"\n", k_bad[i]);
                ++g_fails;
            }
            ++g_checks;
        }
        check(1, "parse: every malformed timestamp was refused");
    }

    /* NULL/degenerate arguments do not crash */
    check(mu_iso8601_format(0, 0, 0, NULL, 10) == 0, "iso: NULL buffer");
    check(mu_iso8601_format(0, 0, 0, buf, 0) == 0, "iso: zero capacity");
    check(mu_iso8601_parse(NULL, 10, &s, &ns) == 0, "parse: NULL string");
    check(mu_iso8601_parse("2026-11-07", 10, NULL, &ns) == 0, "parse: NULL out");

    /* cap-aware: the exact required size succeeds, one less fails */
    {
        const int n = mu_iso8601_format(0, 0, 0, buf, sizeof buf);

        check(mu_iso8601_format(0, 0, 0, buf, (size_t)n + 1u) == n, "iso: exactly-sized buffer works");
        check(mu_iso8601_format(0, 0, 0, buf, (size_t)n) == 0, "iso: one byte short is refused");
    }
}

/* ---------------------------------------------------------------- */
/* 6. durations                                                     */
/* ---------------------------------------------------------------- */

static void test_durations(void)
{
    char buf[64];

    mu_dur_format_human(0, buf, sizeof buf);
    check_str(buf, "0 ns", "dur: zero");
    mu_dur_format_human(512, buf, sizeof buf);
    check_str(buf, "512 ns", "dur: nanoseconds");
    mu_dur_format_human(1240, buf, sizeof buf);
    check_str(buf, "1.24 us", "dur: two decimals below ten");
    mu_dur_format_human(12400, buf, sizeof buf);
    check_str(buf, "12.4 us", "dur: one decimal below a hundred");
    mu_dur_format_human(124000, buf, sizeof buf);
    check_str(buf, "124 us", "dur: no decimals above a hundred");
    mu_dur_format_human(1240000, buf, sizeof buf);
    check_str(buf, "1.24 ms", "dur: milliseconds");
    mu_dur_format_human(1240000000ll, buf, sizeof buf);
    check_str(buf, "1.24 s", "dur: seconds");
    mu_dur_format_human(45900000000ll, buf, sizeof buf);
    check_str(buf, "45.9 s", "dur: below a minute stays in seconds");
    mu_dur_format_human(266706789000000ll, buf, sizeof buf);
    check_str(buf, "3d 02:05:06.789", "dur: days + clock");
    mu_dur_format_human(-1240000, buf, sizeof buf);
    check_str(buf, "-1.24 ms", "dur: negative keeps its sign");
    mu_dur_format_human(INT64_MIN, buf, sizeof buf);
    check_str(buf, "-106751d 23:47:16.854", "dur: INT64_MIN does not overflow");

    mu_dur_format_hms(0, buf, sizeof buf);
    check_str(buf, "00:00:00.000", "hms: zero");
    mu_dur_format_hms(3723456, buf, sizeof buf);
    check_str(buf, "00:00:00.003", "hms: milliseconds truncate");
    mu_dur_format_hms(266706789000000ll, buf, sizeof buf);
    check_str(buf, "74:05:06.789", "hms: hours are not clamped to a day");

    check(mu_dur_format_human(1000, buf, 3) == 0, "dur: a tiny buffer refuses");
    check(buf[0] == '\0', "dur: a refused format leaves an empty string");
    check(mu_dur_format_human(1000, NULL, 10) == 0, "dur: NULL buffer");
}

/* ---------------------------------------------------------------- */
/* 7. clocks                                                        */
/* ---------------------------------------------------------------- */

static void test_clocks(void)
{
    int      i;
    int      backwards = 0;
    uint64_t prev      = mu_clock_ns();

    for (i = 0; i < 20000; ++i) {
        const uint64_t now = mu_clock_ns();

        if (now < prev) ++backwards;
        prev = now;
    }
    check(backwards == 0, "clock: mu_clock_ns never goes backwards over 20 000 reads");
    check(mu_clock_ns() > 0u, "clock: mu_clock_ns is non-zero");

    /* the wall clock agrees with libc's time() to within a couple of seconds
     * (the two reads are not atomic, and a scheduler tick is a scheduler tick) */
    {
        const int64_t wall = mu_wall_unix_s();
        const int64_t ref  = (int64_t)time(NULL);
        const int64_t d    = (wall > ref) ? (wall - ref) : (ref - wall);

        check(d <= 2, "clock: mu_wall_unix_s agrees with libc time() within 2 s");
        check(mu_wall_nanos() < 1000000000u, "clock: nanos are in range");
    }

    /* elapsed-since-start is small and non-negative */
    check(mu_clock_elapsed_ns() < 3600000000000ll, "clock: elapsed since start is sane");

    /* sleep: monotonic sleep must not return early on a steady clock */
    if (mu_time_is_monotonic()) {
        const uint64_t t0 = mu_clock_ns();
        int            ok = mu_sleep_ns(2000000u); /* 2 ms */
        const uint64_t dt = mu_clock_ns() - t0;

        check(ok == 1, "sleep: mu_sleep_ns reports success on this platform");
        check(dt >= 1500000u, "sleep: 2 ms of sleep measured at least 1.5 ms");
        check(dt < 2000000000u, "sleep: 2 ms of sleep did not take a second");
    } else {
        check(1, "sleep: skipped (no monotonic clock on this build)");
    }
}

/* ---------------------------------------------------------------- */
/* 8. the formatter, against snprintf, over every value it can emit */
/* ---------------------------------------------------------------- */

/* The pair table replaced a digit-at-a-time loop, so the optimization has to be
 * checked against an independent digit emitter rather than against itself.
 * snprintf is that oracle, and the loops below are exhaustive over the fields
 * that have a bounded range: every year/month/day in ten millennia, every second
 * of a leap and a non-leap year, and every fraction width 1..9 against a
 * independently computed truncation. */
static void test_format_exhaustive(void)
{
    char    ours[40];
    char    ref[64];
    int64_t d;
    int64_t bad = 0;
    const int64_t first = mu_days_from_civil(0, 1, 1);
    const int64_t last  = mu_days_from_civil(9999, 12, 31);

    /* every day, at a fixed time: covers year 0..9999, all 12 months, all 31
     * days, and the 4/2/2/2/2-digit table paths */
    for (d = first; d <= last; ++d) {
        const mu_civil c = mu_civil_from_days(d);

        if (mu_iso8601_format(d * 86400 + 12 * 3600 + 34 * 60 + 56, 0u, 0, ours, sizeof ours) <= 0) {
            ++bad;
            continue;
        }
        sprintf(ref, "%04d-%02d-%02dT12:34:56Z", c.year, c.month, c.day);
        check_n(strcmp(ours, ref) == 0);
        if (bad < 3 && strcmp(ours, ref) != 0) {
            printf("  format mismatch day %lld: ours \"%s\" snprintf \"%s\"\n", (long long)d, ours,
                   ref);
            ++bad;
        }
    }
    check_i64(bad, 0, "format: every day in 0000..9999 matches snprintf byte for byte");

    /* every second of two days: covers hour, minute and second exhaustively,
     * one of them a leap day, with the date part hard-coded so the comparison is
     * whole-string and not an offset trick */
    {
        static const char *k_day[2] = { "2024-02-29", "2023-12-31" };
        const int64_t      d0       = mu_days_from_civil(2024, 2, 29);
        const int64_t      d1       = mu_days_from_civil(2023, 12, 31);
        int64_t            sec;

        bad = 0;
        for (sec = 0; sec < 86400; ++sec) {
            int v;

            for (v = 0; v < 2; ++v) {
                const int64_t base = (int64_t)(v == 0 ? d0 : d1) * 86400 + sec;

                if (mu_iso8601_format(base, 0u, 0, ours, sizeof ours) <= 0) { ++bad; continue; }
                sprintf(ref, "%sT%02d:%02d:%02dZ", k_day[v], (int)(sec / 3600), (int)((sec / 60) % 60),
                        (int)(sec % 60));
                check_n(strcmp(ours, ref) == 0);
                if (strcmp(ours, ref) != 0) {
                    if (bad < 3) printf("  time mismatch at %lld: ours \"%s\" ref \"%s\"\n",
                                        (long long)sec, ours, ref);
                    ++bad;
                }
            }
        }
        check_i64(bad, 0, "format: all 86 400 seconds of a day match snprintf (2 days sampled)");
    }

    /* every fraction width, against an independently computed truncation */
    {
        static const uint32_t k_pow10[10] = { 1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u,
                                              10000000u, 100000000u, 1000000000u };
        int       fd;
        uint32_t  nano;

        bad = 0;
        for (fd = 0; fd <= 9; ++fd) {
            for (nano = 0; nano < 10000u; ++nano) {
                const uint32_t v   = nano * 100000u + (uint32_t)fd; /* spread over 0..999999999 */
                const uint32_t want = (fd > 0) ? (v / k_pow10[9 - fd]) % k_pow10[fd] : 0u;

                if (mu_iso8601_format(0, v, fd, ours, sizeof ours) <= 0) { ++bad; continue; }
                if (fd == 0) {
                    check_n(strcmp(ours, "1970-01-01T00:00:00Z") == 0);
                    if (strcmp(ours, "1970-01-01T00:00:00Z") != 0) ++bad;
                } else {
                    sprintf(ref, "1970-01-01T00:00:00.%0*uZ", fd, (unsigned)want);
                    check_n(strcmp(ours, ref) == 0);
                    if (strcmp(ours, ref) != 0) {
                        if (bad < 3) printf("  frac %d of %u: ours \"%s\" ref \"%s\"\n", fd, v, ours,
                                           ref);
                        ++bad;
                    }
                }
            }
        }
        check_i64(bad, 0, "format: every width 0..9 matches snprintf's digit emission");
    }

    /* the parser's own digit path, same idea: every month/day combination of a
     * leap year and a non-leap year, formatted then parsed back */
    {
        int      y;
        uint32_t nano;

        bad = 0;
        for (y = 2023; y <= 2024; ++y) {
            int m;

            for (m = 1; m <= 12; ++m) {
                int day;
                const int dim = mu_days_in_month(y, m);

                for (day = 1; day <= dim; ++day) {
                    int64_t s;

                    s = mu_days_from_civil(y, m, day) * 86400;
                    if (mu_iso8601_format(s, 0u, 0, ours, sizeof ours) <= 0) { ++bad; continue; }
                    check_n(mu_iso8601_parse(ours, 20u, &d, &nano) == 20 && d == s);
                    if (!(mu_iso8601_parse(ours, 20u, &d, &nano) == 20 && d == s)) ++bad;
                    /* and the impossible day just past the end must not parse */
                    {
                        char bad_stamp[24];

                        sprintf(bad_stamp, "%04d-%02d-%02dT00:00:00Z", y, m, dim + 1);
                        check_n(mu_iso8601_parse(bad_stamp, 20u, &d, &nano) == 0);
                        if (mu_iso8601_parse(bad_stamp, 20u, &d, &nano) != 0) ++bad;
                    }
                }
            }
        }
        check_i64(bad, 0, "parse: every real day of 2023/2024 round-trips, every fake one is refused");
    }
}

/* ---------------------------------------------------------------- */

int main(void)
{
    char stamp[40];

    mu_iso8601_format_now_ms(stamp, sizeof stamp);
    printf("mu_time correctness gate\n");
    printf("----------------------------------------------------------\n");
    printf("  clock: monotonic=%d  wall=%s\n", mu_time_is_monotonic(), stamp);

    test_calendar_exhaustive();
    test_calendar_vs_libc();
    test_calendar_invariants();
    test_iso_week();
    test_timestamps();
    test_durations();
    test_clocks();
    test_format_exhaustive();

    printf("----------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
