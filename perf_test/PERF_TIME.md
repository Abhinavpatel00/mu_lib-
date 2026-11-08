# PERF_TIME.md — `mu/mu_time.h`

Evidence for a header that did not exist before this round: a steady clock, a
proleptic Gregorian calendar, ISO 8601 / RFC 3339 timestamps and duration
formatting. The baseline is not another library — it is **glibc**, because the
alternative for every function here is `strftime` / `gmtime_r` / `timegm` /
`strptime` / `snprintf`, and those are already linked into the caller.

All seven fields first, then the numbers, then what they do not show.

---

## 1. The seven fields

| # | field | value |
| --- | --- |
| 1 | CPU / µarch | `Intel(R) Xeon(R) Processor @ 2.60GHz` (family 6 server part, AVX-512 present), one vCPU of a guest |
| 2 | Cores / affinity | 2 vCPU total, **1 thread**, every timed run wrapped in `taskset -c 0` |
| 3 | Measured frequency | `/proc/cpuinfo` says `2600.042` MHz (nominal). A 20 000 000-long **dependent `add` chain** — 1 µop, 1 cycle, the calibration AGENTS.md §2.6(3) prescribes instead of invariant `rdtsc` — measured **0.642, 0.798, 0.848, 0.875, 0.909, 0.983, 1.071 GHz** over seven consecutive windows: the core runs at ≈40 % of nominal **and is still ramping upward during a run**. Consequence: `cycles/op ≈ ns × 1.07` on this box, and a host sustained at 2.6 GHz would print ≈ 41 % of every nanosecond figure below. That ramp is also why medians disagree between runs, which is why §3 exists. |
| 4 | Compiler / flags | `gcc (Debian 14.2.0-19) 14.2.0`, `-O3 -march=native -std=c11 -Wall -Wextra -Werror -lm` for the bench and the gate; the gate also runs at `-O1 -fsanitize=address,undefined -fno-sanitize-recover=all` → **8 179 268 checks, 0 failures**. Kernel `6.1.189+ x86_64`, `glibc 2.41` |
| 5 | Workload / working set | 16 instants spanning 0000 AD … 9999 AD, both sides of the epoch, a leap day and a century boundary (`k_s`/`k_ns`, 192 B), plus 40-byte output buffers. Total < 1 KiB against **48 KiB L1d / 1.3 MiB L2 / 54 MiB L3**, so every row is L1-resident and none of them is a memory-bound row. All inputs non-zero (no zero-store elimination) |
| 6 | Baseline | two, both in the same binary: (a) glibc `strftime`/`gmtime_r`/`timegm`/`strptime`/`snprintf`, written the way a competent caller writes them — the fraction appended by hand, because libc has no portable milliseconds conversion; (b) `mu_time.h` **before the pair-table optimization**, kept in the bench as a local `naive_format`, so an optimization is measured against the code it replaced rather than asserted |
| 7 | Method | `mu_bench`: `CLOCK_MONOTONIC`, `MU_BENCH_DO_NOT_OPTIMIZE` barriers, auto-sized batches, warmup, **61 samples/row** (3× the ≥21 floor), reporting min (latency floor), median (throughput), CV, CI99. Plus a second independent method (§3) with 101 interleaved paired rounds, because on this box that is the only way to make a ratio mean something |

---

## 2. The mu_bench table (61 samples, min | median | CV | CI99)

```
time.fmt_1s_ours              34.000 |  35.115 ns | cv  6.6% | ±2.51%
time.fmt_1s_libc             192.914 | 199.249 ns | cv  5.7% | ±2.15%
time.fmt_ms_ours              31.657 |  37.452 ns | cv  4.3% | ±1.63%
time.fmt_ms_naive_digits      53.057 |  78.134 ns | cv 11.1% | ±4.20%
time.fmt_1s_naive_digits      47.957 |  67.649 ns | cv 10.4% | ±3.92%
time.fmt_ms_libc             275.716 | 293.574 ns | cv  8.1% | ±3.04%
time.fmt_ns_ours              33.628 |  44.527 ns | cv 13.1% | ±4.95%
time.parse_1s_ours            36.219 |  53.043 ns | cv  9.1% | ±3.42%
time.parse_1s_libc           163.723 | 213.043 ns | cv  9.1% | ±3.43%
time.parse_ns_ours            57.901 |  69.984 ns | cv 10.3% | ±3.88%
time.civil_add_days_day_space 10.270 |  15.282 ns | cv  7.9% | ±2.99%
time.civil_ours               15.696 |  20.810 ns | cv 12.1% | ±4.57%
time.civil_libc               56.244 |  62.120 ns | cv  3.1% | ±1.18%
time.to_unix_ours             20.053 |  20.656 ns | cv  6.9% | ±2.60%
time.to_unix_libc             99.883 | 103.517 ns | cv 12.3% | ±4.65%
time.clock_ours               32.612 |  46.124 ns | cv 10.1% | ±3.81%
time.clock_syscall            44.416 |  46.033 ns | cv  0.9% | ±0.35%
time.wall_ours                43.503 |  45.541 ns | cv  1.7% | ±0.64%
time.wall_ours_two_reads      82.316 |  91.174 ns | cv  9.2% | ±3.48%
time.wall_libc                44.656 |  47.861 ns | cv  6.5% | ±2.45%
time.dur_human_ours           14.462 |  15.037 ns | cv  6.5% | ±2.45%
time.dur_human_libc          197.832 | 200.598 ns | cv  6.6% | ±2.48%
time.dur_hms_ours             15.780 |  16.605 ns | cv  7.3% | ±2.76%
time.dur_hms_libc            240.010 | 250.171 ns | cv  4.5% | ±1.69%
time.iso_week_ours            52.193 |  57.487 ns | cv  4.0% | ±1.51%
time.iso_week_libc           134.259 | 153.717 ns | cv 10.0% | ±3.78%
time.civil_add_days_ours      31.288 |  42.263 ns | cv 14.5% | ±5.48%
time.civil_add_months_ours    13.854 |  15.774 ns | cv 16.0% | ±6.05%
```

`mu_bench` marks 18 of the 28 rows `UNSTABLE`; on this box that verdict is field
3, not a property of the code (the CV is 6–16 % on rows whose *min* moved by 3 %).
One pair is worth reading as a caution: `fmt_ms_ours` came out 22.4 ns median in
an earlier run and 37.5 ns in this one — a 67 % swing on identical code — while
`fmt_ms_naive_digits` sat at 78.0 and 78.1. So the table's shape is trustworthy
(what is cheap, what is not, and the ordering within a family) and its ratios
are not. Every claim below is made from §3.

---

## 3. The interleaved paired A/B — two runs of the numbers behind every claim

`make -C perf_test time-pairs`. Each pair is measured **101 times**, both sides
back to back inside the same frequency window, order alternating on odd rounds so
any warm-up asymmetry cancels, 20 000 ops per batch. Two adjacent batches see the
same core state, so their *ratio* survives the DVFS ramp that destroys the
absolute numbers. `ratio < 1` means the first column is cheaper.

```
pair                                  run 1                          run 2
                            ratio     p10     p90   med A   med B   ratio   p10    p90
format ms   table vs div    0.465   0.437   0.519  36.2 ns  77.7 ns  0.480  0.399  0.560
format 1s   table vs div    0.505   0.471   0.548  34.4 ns  67.6 ns  0.517  0.462  0.681
format ms   ours vs libc    0.124   0.118   0.132  36.6 ns 292.9 ns  0.107  0.088  0.130
format 1s   ours vs libc    0.175   0.164   0.190  34.5 ns 197.5 ns  0.171  0.155  0.185
format ms   div   vs libc   0.266   0.246   0.279  77.4 ns 291.4 ns  0.267  0.202  0.291
format 1s   div   vs libc   0.346   0.277   0.369  68.3 ns 197.3 ns  0.346  0.303  0.366
parse 1s    ours vs libc    0.241   0.231   0.256  51.3 ns 212.1 ns  0.240  0.208  0.294
civil       ours vs libc    0.272   0.243   0.304  16.4 ns  60.5 ns  0.244  0.213  0.311
to unix     ours vs libc    0.214   0.198   0.228  27.8 ns 129.5 ns  0.211  0.194  0.227
duration    ours vs libc    0.056   0.051   0.075  15.9 ns 269.6 ns  0.054  0.051  0.068
iso week    ours vs libc    0.370   0.318   0.479  54.1 ns 148.0 ns  0.342  0.325  0.388
wall clock  1 read vs 2     0.504   0.486   0.529  44.9 ns  88.7 ns  0.504  0.462  0.568
```

The summary, in the form worth quoting — and note that the ranges are between
runs, not confidence intervals:

| operation | vs glibc | stability across the two runs |
| --- | --- | --- |
| format `…T…Z` at seconds | **5.7 – 5.8×** | 0.175 / 0.171 |
| format with milliseconds | **7.7 – 9.3×** | 0.124 / 0.107 |
| format with nanoseconds | no baseline exists (see §5) | — |
| parse `…T…Z` | **4.2×** | 0.241 / 0.240 |
| unix seconds → `(y,m,d,h,m,s)` | **3.3 – 4.1×** | 0.272 / 0.244 |
| `(y,m,d,h,m,s)` → unix seconds | **4.7×** | 0.214 / 0.211 |
| duration humanization | **18 – 19×** | 0.056 / 0.054 |
| ISO week numbering | **2.6 – 2.9×** | 0.370 / 0.342 |
| `mu_clock_ns` vs raw `clock_gettime` | **1.00×** (46.12 vs 46.03 ns, CI ±3.8 %/±0.4 %) | tie |

Where §2 and §3 can be compared, they agree: `fmt_1s` vs libc 0.176 (§2 medians)
vs 0.175/0.171 (§3); table-vs-loop 0.479 vs 0.465/0.480; wall 1-vs-2 reads 0.4995
vs 0.504/0.504; parse 0.249 vs 0.241/0.240; duration 0.075 vs 0.056/0.054. The
last one is the only real gap, and it is explained: §3's baseline body has a
serial `acc +=` dependency across `snprintf` calls, §2's does not, and `snprintf`
is the slow part in both, so a 13× vs 18× difference is a difference in how much
of the surrounding loop is visible, not in the thing being measured. Where two
methods that disagree about everything else agree to a few percent, the claim is
safe; where they differ by more, the claim is reported as a range.

---

## 4. Decomposed per optimization, not as an aggregate

**(a) Not calling `strftime` — 3.7× of the external win, and it is not ours.**
`format ms div loop vs libc = 0.266/0.267` — the *unoptimized* version of this
header was already 3.75× faster than glibc, and 2.9× at second precision
(0.346 both runs). The reason is structural and has nothing to do with our
arithmetic: `strftime` re-parses the format string and consults locale data on
**every call**, and the caller must first pay `gmtime_r` to obtain a `struct tm`
at all. We walk from an `int64` to characters in one pass with no locale, no
format string and no intermediate struct. Anyone crediting our digit handling for
the libc comparison is double counting; rows 5 and 6 of §3 exist precisely to
separate the two effects.

**(b) The two-digit pair table — 2.0× to 2.1× on format.**
`fmt_ms` 37.5 → 78.1 ns and `fmt_1s` 35.1 → 67.6 ns when the table is replaced by
a digit-at-a-time loop. **The obvious explanation for that win is wrong**: `gcc
-O3 -march=native` emits *zero* `div`/`idiv` instructions in either version,
because every divisor in both is the constant 10 or 100 and it has already
strength-reduced them into multiply-shift. The instruction mix says what actually
changed:

```
digit-at-a-time loop (inlined 7x)   891 instructions, 109 branches, 26 mul/imul
pair table                          227 instructions,   8 branches, 25 mul/imul
```

Same multiply count, a quarter of the instructions, an eighth of the branches.
What the old path cost was a **data-dependent trip count inlined seven times**:
discover how many digits there are, emit them backwards into scratch, copy them
forwards, then a third loop for the caller's zero padding. A table lookup with a
provably in-range index is one load and no branch, so the whole formatter becomes
straight-line code. If you re-derive this optimization from "one division per
digit" you will predict 2× *per field* and be wrong about the mechanism while
accidentally landing near the right magnitude (2.0× for the whole call).

**(c) One clock read instead of two — 1.98×, the tightest number in this file.**
0.504 in both runs (p10 0.462–0.486, p90 0.529–0.568), and §2 agrees to within
1 %: 45.54 vs 91.17 ns = 0.4995. `mu_wall_unix_s()` + `mu_wall_nanos()` were two
independent `clock_gettime` calls — 88.7 ns → 44.9 ns — and across a second
boundary they can return a pair that is not a valid instant (`ns` sampled after
`s` stepped). `mu_wall_now()` is the single-read API and
`mu_iso8601_format_now_ms()` uses it. This row exists because the two-read
version is what the header shipped until the bench caught it.

**(d) `mu_clock_ns()` over the raw syscall — exactly nothing, which is the point.**
46.124 vs 46.033 ns. That is what makes "stamp every log record" affordable, and
it is the number `mu_log.h` needed: a per-record wrapper is only acceptable if it
is free, and it is. It is also the row that must not be sold as a win.

**(e) The header as a whole, in cycles.** At the min-window frequency (1.07 GHz):
`fmt_ms_ours` 37.5 ns ≈ **40 cycles**, `parse_1s_ours` 53.0 ≈ 57 cycles,
`civil_ours` 20.8 ≈ 22 cycles, `dur_human_ours` 15.0 ≈ 16 cycles. Against the
146097-day era decomposition being 5 divisions-by-constant and ~10 multiplies, 22
cycles for `int64 → (y,m,d,h,m,s)` is roughly the floor for straight-line scalar
code; there is no SIMD story here worth chasing (one element per call, and the
output is bytes).

---

## 5. Losses, and the rows that do not favor us

- **`mu_civil_add_days` is 2.8× slower than not calling it:** 42.3 ns vs
  `civil_add_days_day_space` at 15.3 ns. Adding days to a civil date converts
  `civil → days → civil`, two calendar passes for an answer available with one.
  Stay in day counts unless you need month semantics. The counter-intuitive part:
  `mu_civil_add_months` — the operation with the awkward end-of-month clamping
  rule — is **cheaper** at 15.8 ns, because it never touches a day count. Months
  are arithmetic; days are a conversion.
- **No baseline for `parse_ns_ours` (70.0 ns).** `strptime` has no portable
  fractional-seconds conversion, so the comparison for "parse a timestamp with
  nanoseconds" is "write your own". 32 % more work than the second-precision
  parse for nine digits of precision is a reasonable cost, and the absence of a
  baseline is a capability we have and libc does not — not a speed win to claim.
- **`iso_week_ours` 57.5 ns is 2.8× `civil_ours`.** The Thursday rule costs a
  second `civil_from_days` plus a `days_from_civil`. It is also the widest band in
  §3 (p10 0.318 / p90 0.479 in run 1), so the honest statement is "2.6–2.9×
  faster than `gmtime_r` + `strftime("%G-W%V-%u")`", not "2.7×".
- **Nothing beats `time()` at being `time()`.** `wall_ours` 45.5 ns vs
  `wall_libc` 47.9 ns is inside CI — a tie, not a win. This header is not "faster
  clocks"; it is the same clock with the fields in a usable order, a real
  monotonic source, and no two-read tearing.
- **Absolute numbers are ~2.4× pessimistic** (field 3): a host sustained at
  2.6 GHz would show `fmt_ms_ours` around 15 ns and `fmt_ms_libc` around 113 ns.
  The ratios transfer; the nanoseconds do not.
- **`fmt_ms_libc` is not a strawman but it is not minimal either:** the glibc row
  appends the fraction with `snprintf(".%03uZ")` because there is no portable
  `strftime` field for it. A caller who hand-rolls the whole string with
  `gmtime_r` + `snprintf("%04d-%02d-…")` would land between our number and glibc's
  — faster than `strftime`, slower than us, and it would still be a locale-free
  reimplementation of this header. That is the honest reading of (a): the win over
  libc is largely "do not call `strftime`", and the win of the pair table over the
  digit loop is ours.

---

## 6. Correctness gate (why the speed is worth anything)

`make -C perf_test time-test` → **8 179 268 checks, 0 failures**, ~1 s, and clean
under `-fsanitize=address,undefined -fno-sanitize-recover=all`.

| section | what it demands |
| --- | --- |
| exhaustive calendar | all **3 652 425** days from 0000-01-01 to 9999-12-31 round-trip through `days ↔ (y,m,d)`, and every one is accepted by `mu_civil_is_valid`. (3 652 425, not the 3 652 059 that years 1…9999 gives — year 0 is included, and it is a leap year. The fixture was wrong before the code was checked, which is the value of the exhaustive loop.) |
| format oracle | **every one of those 3 652 425 days** formatted and compared byte-for-byte against `snprintf("%04d-%02d-%02dT12:34:56Z", …)`, plus all 86 400 seconds of a leap day and of a non-leap day (whole-string compare, no offset trick), plus every fraction width 0…9 against an independently computed truncation. This is the row that would catch a bad table index |
| libc cross-check | 200 000 pseudo-random stamps, both sides of the epoch, compared field-by-field with `gmtime_r`; `timegm` on libc's own `struct tm` must return the stamp we started from (so a shared breakage cannot hide) |
| structural | leap years incl. `1900/2000/2100` and negative years, month lengths, `2026-02-30` / `2100-02-29` / `2024-04-31` refused, day-of-year, weekday advancing exactly one per day and inserting exactly one across a leap day, `add_months` end-of-month clamping in both directions |
| ISO week | 11 fixtures where `day_of_year/7` is wrong (2021-01-01 = 2020-W53-5, 2000-01-01 = 1999-W52-6, 2016-01-01 = 2015-W53-5, …), week in 1…53 and ISO year within one of the calendar year across 1900–2100, then the Thursday rule itself for 1990–2031: every week's Thursday lies inside the ISO year that week names |
| parse | 20 000 format→parse round trips at 0/3/6/9 digits; every real day of 2023 and 2024 round-trips and the day *just past* each month's end is refused; offsets `+05:30` / `-08:00` / `+0530` / `+05`, `Z`/`z`, `T`/`t`/space, `.`/`,`, 1- and 9-digit fractions, strict-vs-lenient zone handling, and consumed-byte counts inside a longer line |
| refusals | 20 malformed strings (month 13, hour 24, second 60, 10-digit fraction, `99999-01-01`, 1-digit fields, `26-11-07`, trailing `T`, …) must each return 0; NULL/zero-cap/exact-size/off-by-one buffers must refuse without writing a truncated string; `nanos ≥ 1e9` and 10 fraction digits are errors |
| durations | the printed ladder, `-1.24 ms` for a negative input, and `INT64_MIN → -106751d 23:47:16.854` — the magnitude is a `uint64` on purpose, because negating `INT64_MIN` is UB and this row is what pins that decision |
| clocks | 20 000 reads never go backwards, `mu_wall_unix_s` within 2 s of `time()`, nanos in range, a 2 ms sleep measured ≥1.5 ms and <2 s |

Two gaps, in the gate's own words: the oracle is glibc, so a *shared*
misunderstanding would survive it — mitigated by the independent 3.65M-day round
trip and the `timegm` self-consistency check, not eliminated; and there is no
timezone or DST coverage, because there is no timezone support.

---

## 7. Reproduce

```sh
cd /home/user/mu_lib-

make -C perf_test time-test       # gate, ~1 s: 8 179 268 checks, 0 failures
make -C perf_test time-bench      # §2, 28 rows x 61 samples, ~55 s
make -C perf_test time-pairs      # §3, 12 interleaved ratios, ~45 s per run

# sanitized gate
gcc -O1 -std=c11 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I mu -o /tmp/ttsan perf_test/test_time.c && taskset -c 0 /tmp/ttsan

# field 3: the frequency calibration (20M dependent adds per window, 7 windows)
gcc -O2 -o /tmp/freq /tmp/freq.c && taskset -c 0 /tmp/freq

# §4(b): the instruction counts behind "branches, not division"
gcc -O3 -march=native -std=c11 -I mu -S -o /tmp/divs.s /tmp/divs2.c
```

Expected: §2 in 55 s with 18 of 28 rows flagged `UNSTABLE`; §3 within a few
percent of the two runs above; the frequency probe under 1.1 GHz on this guest
and near 2.6 on a non-throttled host. `time-bench` is prefixed with `-` in the
Makefile because a row over `mu_bench`'s CV cutoff is a property of this shared
guest, not of `mu_time.h` — the CV column is the result, the exit code is not.
