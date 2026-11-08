# `mu_time.h` — clocks, the calendar, and ISO 8601

`#include "mu_time.h"` — one header, C99, no libc date code, no timezone
database, no allocation, no global state.

```c
#include "mu_time.h"
```

Measured against glibc in [perf_test/PERF_TIME.md](../perf_test/PERF_TIME.md):
5.7× faster than `strftime` on a second-precision timestamp and 7.7–9.3× with
milliseconds, 4.2× faster than `strptime` + `timegm` on parsing, 3.3–4.1× on
calendar conversion, 18× on duration humanization. The clock wrapper costs
exactly nothing over the raw `clock_gettime` (46.12 vs 46.03 ns).

---

## Why this header exists at all

`mu_log.h` had a field documented as

```c
uint64_t time_ns;   /* monotonic, from the process start */
```

and filled it from `timespec_get(&ts, TIME_UTC)`, which is a **wall** clock, with
a C99 fallback of `clock()`, which is **CPU** time. So a log timestamp meant
three different things depending on the compiler, and on a laptop that resumed
from sleep or a VM that got NTP-corrected, `MU_LOG_TIME_SCOPE` could report a
**negative** duration. The comment explained that `clock_gettime` could not be
used because it needs `_POSIX_C_SOURCE` defined before the first system header —
which stopped being true when `mu_macros.h` took ownership of that define.

That is the whole argument for separating the two jobs. A duration and a
timestamp are different questions with different right answers, and libc uses the
same calls for both.

---

## §1 clocks

| function | what it is |
| --- | --- |
| `mu_clock_ns()` | steady: `CLOCK_MONOTONIC` on POSIX, `QueryPerformanceCounter` on Windows. Never goes backwards, arbitrary origin |
| `mu_time_is_monotonic()` | 1 if the line above is a real steady clock on this build, 0 if it fell back to the wall clock. Check it instead of hoping |
| `mu_wall_unix_s()`, `mu_wall_nanos()` | UTC seconds and sub-second from `CLOCK_REALTIME`, separately |
| `mu_wall_now(&s, &ns)` | **both from one read** — 2.0× faster than calling the two above, and it cannot return a torn pair across a second boundary |
| `mu_clock_start_ns()`, `mu_clock_elapsed_ns()` | elapsed-since-program-start as a subtraction. The origin is per translation unit by design: no init call to forget, no shared mutable state |
| `mu_sleep_ns(ns)`, `mu_sleep_ms(ms)` | best effort, `nanosleep`/`Sleep`, retries on `EINTR`. For tools, never for pacing a frame |

Use `mu_clock_ns` for anything that measures, `mu_wall_*` for anything that
labels. `mu_wall_now` exists rather than being your problem because the two-read
version is what this header first shipped, and the paired benchmark said
91.2 ns → 45.5 ns (ratio 0.504 in both of two independent runs — the tightest
number in the report).

## §2 the calendar

The representation is `int64_t days since 1970-01-01`: fits in a register, sorts
like a date, exact for ten millennia in both directions.

```c
mu_civil    c  = mu_civil_from_days(-719528);      /* {0, 1, 1} */
int64_t     n  = mu_days_from_civil(2026, 11, 7);
mu_civil    x  = mu_civil_add_days(c, 45);
mu_civil    m  = mu_civil_add_months(c, 1);        /* Jan 31 -> Feb 28/29, clamped */
int         leap = mu_is_leap_year(2100);          /* 0: the century rule */
int         dim  = mu_days_in_month(2024, 2);      /* 29 */
int         ok   = mu_civil_is_valid((mu_civil){2026, 2, 30});   /* 0, refused */
int         wd   = mu_weekday_iso(n);              /* 1=Mon .. 7=Sun */
const char *name = mu_weekday_name(wd);            /* "Sat" */
int         doy  = mu_civil_day_of_year(c);

int32_t iy; uint32_t wk, wd2;
mu_civil_iso_week((mu_civil){2021, 1, 1}, &iy, &wk, &wd2);  /* 2020, 53, 5 */

mu_datetime dt = mu_datetime_from_unix(1794009600);
int64_t     s  = mu_datetime_to_unix(dt);
```

`mu_civil_iso_week` is the one worth having from someone else: a week belongs to
the year containing its **Thursday**, which is why 2021-01-01 is week 53 of 2020,
2016-01-01 is week 53 of 2015, and 2000-01-01 is week 52 of 1999. Any
`day_of_year/7` implementation is wrong on exactly those dates, and the gate
asserts them plus 140 years of the structural rule.

## §3 timestamps

```c
char buf[40];
int64_t  s  = 1794009600;
uint32_t ns = 678732032;

mu_iso8601_format(s, ns, 3, buf, sizeof buf);   /* "2026-11-07T09:36:12.678Z" */
mu_iso8601_format(s, ns, 0, buf, sizeof buf);   /* "2026-11-07T09:36:12Z"     */
mu_iso8601_format_now_ms(buf, sizeof buf);      /* the wall clock, one read    */

int64_t  s2; uint32_t ns2;
int used = mu_iso8601_parse("2026-11-07T15:06:12+05:30", 25, &s2, &ns2);
/* used == 25, and s2 is the same instant as the Z form */
```

- `frac_digits` is 0..9, **truncated** not rounded — rounding `999 999 999` to 3
  digits would print `".1000"` and put the timestamp in the future.
- The formatter returns the bytes written and **refuses to truncate**: if the
  buffer is one byte short you get `0` and an empty string, never a
  half-written date.
- The parser reads one timestamp and returns **how many bytes it consumed**, so
  you can point it into a log line or a network record instead of copying the
  field out. It stops at the first byte that cannot continue a timestamp rather
  than failing the whole call: `used < len` is how you detect trailing garbage.
  (`"2026-11-07X…"` yields 10 and midnight — same behaviour as `strptime`.)
- `mu_iso8601_parse_strict` additionally **requires** a `Z` or an offset, which
  is what you want for data from another program; the lenient one treats a
  missing zone as UTC.
- Accepted: `T`/`t`/space separator, `.` or `,` decimal mark, 1..9 fraction
  digits, `±HH`, `±HHMM`, `±HH:MM`, `Z`/`z`. Refused: month 13, `2026-02-30`,
  hour 24, second 60 (leap seconds are not represented), 10-digit fractions,
  years outside 0000..9999.

## §4 durations

```c
char b[40];
mu_dur_format_human(512, b, 40);           /* "512 ns"        */
mu_dur_format_human(1240, b, 40);          /* "1.24 us"       */
mu_dur_format_human(1240000, b, 40);       /* "1.24 ms"       */
mu_dur_format_human(45900000000ll, b, 40); /* "45.9 s"        */
mu_dur_format_human(266706789000000ll, b, 40); /* "3d 02:05:06.789" */
mu_dur_format_human(-1240000, b, 40);      /* "-1.24 ms" — a clock that went
                                              backwards says so */
mu_dur_format_hms(266706789000000ll, b, 40); /* "74:05:06.789", aligned */
```

Three significant digits, integer arithmetic only — no `double`, no `printf`, no
allocation — so it is usable in a signal handler and cheap enough to run once per
log record. `mu_dur_format_hms` deliberately does not wrap at 24 h, because a
profile table wants the total.

---

## Deliberate limits

| not supported | why |
| --- | --- |
| timezones, DST | a timezone is a database, not a header, and a wrong offset is worse than none. Everything here is UTC; the *parser* accepts an explicit offset and normalizes to UTC, which is not the same thing |
| leap seconds | POSIX time ignores them, so `:60` is an error, not a clamp |
| years outside 0000..9999 in text form | that is the RFC 3339 profile. The calendar math is exact far beyond it; only the formatter/parser refuse |
| nanoseconds-since-epoch in one `int64` | it spans 1678..2262. That is why the API is `(seconds, nanos)` as a pair and the calendar takes seconds — the convenience version is a bug with a due date |
| pre-1582 history | the proleptic Gregorian calendar is used for every year, the same convention as `gmtime`. Old dates are not "wrong", they are consistent |

## Gate and measurements

- `make -C perf_test time-test` — **8 179 268 checks, 0 failures**, ~1 s, and
  clean under ASan+UBSan. Every day from 0000-01-01 to 9999-12-31 (3 652 425 of
  them) round-trips *and* is compared byte-for-byte against `snprintf`; 200 000
  random stamps cross-check against `gmtime_r`/`timegm`; 20 malformed strings must
  be refused.
- `make -C perf_test time-bench` — the 28-row cost table.
- `make -C perf_test time-pairs` — 12 interleaved A/B ratios. On this guest the
  core runs at 0.64–1.07 GHz while a benchmark set runs, so the ratios, not the
  absolute nanoseconds, are the transferable numbers. Both methods agree to a few
  percent on every pair except one, which is called out in
  [PERF_TIME.md](../perf_test/PERF_TIME.md) §3.

## Not faster than, and why we kept it anyway

`mu_civil_add_days` costs 41.7 ns; `mu_civil_from_days(days + 45)` costs 15.1
ns. Adding days to a civil date is two calendar passes (to the day count, add,
back) for an answer you can get with one, so if you do not need month semantics,
stay in day counts. The counter-intuitive part is that `mu_civil_add_months` —
the operation with the awkward end-of-month clamping rule — is *cheaper* at
15.8 ns, because it never touches a day count: it folds year and month into a
month index, adds, and clamps the day against a twelve-byte table. Months are
arithmetic; days are a conversion.
