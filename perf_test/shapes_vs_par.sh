#!/bin/sh
# shapes_vs_par.sh — paired mu_shapes.h vs par_shapes.h table.
#
# Runs the two suites ABBA (par, mu, mu, par, ...) so a machine that drifts
# between runs does not systematically favour one side, takes the median of the
# per-run medians, and prints par/mu so a number above 1.00 means mu is faster.
# The geometric mean at the bottom is over the per-case ratios, because a mean of
# ratios is not a ratio of means and these rows span 30 ns to 2 ms.
#
#   sh shapes_vs_par.sh                  # 3 reps x 21 samples, pinned to core 0
#   SAMPLES=7 REPS=1 sh shapes_vs_par.sh # quick look
set -e

BIN=${BIN:-./bench_shapes_vs_par}
SAMPLES=${SAMPLES:-21}
REPS=${REPS:-3}
PIN=${PIN:-taskset -c 0}
# The harness exits non-zero when a sample is flagged UNSTABLE. That is a warning
# about the machine, not a failure of the comparison, so it must not abort a
# paired run - otherwise a shared 2-vCPU box produces no table at all.
CONF=${CONF:-6}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

r=0
while [ "$r" -lt "$REPS" ]; do
    $PIN "$BIN" --filter=par. --samples=$SAMPLES --confidence=$CONF --csv="$TMP/par.a$r.csv" >/dev/null || true
    $PIN "$BIN" --filter=mu.  --samples=$SAMPLES --confidence=$CONF --csv="$TMP/mu.a$r.csv"  >/dev/null || true
    $PIN "$BIN" --filter=mu.  --samples=$SAMPLES --confidence=$CONF --csv="$TMP/mu.b$r.csv"  >/dev/null || true
    $PIN "$BIN" --filter=par. --samples=$SAMPLES --confidence=$CONF --csv="$TMP/par.b$r.csv" >/dev/null || true
    r=$((r + 1))
done

awk -F, '
function median(a, n,   i, j, t) {
    for (i = 1; i <= n; ++i)
        for (j = i + 1; j <= n; ++j)
            if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t; }
    return (n % 2) ? a[(n + 1) / 2] : 0.5 * (a[n / 2] + a[n / 2 + 1]);
}
FNR == 1 { next }                      # header
$1 == "par" { par[$2] = par[$2] " " $3; seen[$2] = 1; next }
$1 == "mu"  { mu[$2]  = mu[$2]  " " $3; seen[$2] = 1; next }
END {
    for (k in seen) {
        np = split(par[k], ps, " "); nm = split(mu[k], ms, " ")
        if (np < 1 || nm < 1) continue
        p = median(ps, np); m = median(ms, nm)
        big = (p > m) ? p : m
        if (big >= 1e6)      { p /= 1e6; m /= 1e6; unit = "ms" }
        else if (big >= 1000){ p /= 1000; m /= 1000; unit = "us" }
        else                 { unit = "ns" }
        printf "%-22s %10.3f %10.3f %8.2fx  %s\n", k, p, m, p / m, unit
    }
}' "$TMP"/par.*.csv "$TMP"/mu.*.csv > "$TMP/rows.txt"

printf "%-22s %10s %10s %9s\n" "case" "par" "mu" "par/mu"
printf "%-22s %10s %10s %9s\n" "----------------------" "----------" \
       "----------" "---------"
sort "$TMP/rows.txt"

awk '{ lg += log($4); ++n } END {
    printf "\ngeometric mean of par/mu over %d cases: %.2fx  (>1 means mu is faster)\n",
           n, exp(lg / n) }' "$TMP/rows.txt"
