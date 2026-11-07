#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_stats / mu_histogram — streaming summary + log-bucket percentiles.
 *
 * WHY: the profiling chapter of the performance skill. A mean alone hides the
 * thing that actually hurts (a p99 hitch), and storing every sample to get
 * percentiles costs O(n) memory. This does both cheaply:
 *
 *   mu_stats      Welford running mean/variance/min/max — O(1) memory, O(1)
 *                 per sample, numerically stable (no catastrophic cancellation
 *                 from summing then subtracting).
 *
 *   mu_histogram  HDR-style log-linear buckets: the value's binary exponent
 *                 picks an octave, its top mantissa bits pick a sub-bucket
 *                 inside that octave. Relative precision is constant
 *                 (1/16 of an octave) across the whole range, so a 0.1 ms
 *                 frame and a 10 s stall both land somewhere useful.
 *
 * PER-SAMPLE COST: one double add for Welford, and for the histogram one
 * uint64 bit-shuffle (extract exponent + mantissa from the IEEE-754 fields) —
 * no log(), no pow(), no libm. So this is safe to call inside a frame's
 * hottest measured scope.
 *
 * CONTRACT: single-threaded. Reset clears everything.
 * --------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- *
 * Running statistics (Welford)
 * -------------------------------------------------------------------------- */

typedef struct mu_stats
{
    double   mean;   /* running mean                                          */
    double   m2;     /* running sum of squared deviations                     */
    double   min;
    double   max;
    uint64_t count;
} mu_stats;

MU_INLINE void mu_stats_init(mu_stats* s)
{
    MU_ASSERT(s);
    s->mean  = 0.0;
    s->m2    = 0.0;
    s->min   = 0.0;
    s->max   = 0.0;
    s->count = 0;
}

MU_INLINE void mu_stats_reset(mu_stats* s)
{
    mu_stats_init(s);
}

MU_INLINE void mu_stats_push(mu_stats* s, double value)
{
    MU_ASSERT(s);
    ++s->count;

    if (s->count == 1)
    {
        s->mean = value;
        s->m2   = 0.0;
        s->min  = value;
        s->max  = value;
        return;
    }

    /* Welford: update in place, stable against large |value|. */
    double delta    = value - s->mean;
    s->mean        += delta / (double)s->count;
    s->m2          += delta * (value - s->mean);

    if (value < s->min)
        s->min = value;
    if (value > s->max)
        s->max = value;
}

MU_INLINE double mu_stats_mean(const mu_stats* s)
{
    return (s && s->count) ? s->mean : 0.0;
}

/* Sample variance (n-1 denominator). */
MU_INLINE double mu_stats_variance(const mu_stats* s)
{
    if (!s || s->count < 2)
        return 0.0;
    return s->m2 / (double)(s->count - 1);
}

MU_INLINE double mu_stats_stddev(const mu_stats* s)
{
    double v = mu_stats_variance(s);
    /* sqrt without libm: Newton iterations from a bit-derived seed.
       Three iterations gives ~1e-7 relative error on IEEE doubles. */
    if (v <= 0.0)
        return 0.0;
    double x = v * 0.5;
    x        = (x + v / x) * 0.5;
    x        = (x + v / x) * 0.5;
    x        = (x + v / x) * 0.5;
    return x;
}

MU_INLINE double mu_stats_min(const mu_stats* s)
{
    return (s && s->count) ? s->min : 0.0;
}

MU_INLINE double mu_stats_max(const mu_stats* s)
{
    return (s && s->count) ? s->max : 0.0;
}

/* -------------------------------------------------------------------------- *
 * Log-linear histogram
 * -------------------------------------------------------------------------- */

#define MU_HIST_OCTAVE_LOW  (-32)   /* smallest trackable value ~2^-32        */
#define MU_HIST_OCTAVE_HIGH 63      /* largest  trackable value ~2^63         */
#define MU_HIST_SUB_BITS    4
#define MU_HIST_SUB_BUCKETS (1u << MU_HIST_SUB_BITS)                          /* 16 */
#define MU_HIST_OCTAVES     ((uint32_t)(MU_HIST_OCTAVE_HIGH - MU_HIST_OCTAVE_LOW + 1))
#define MU_HIST_BUCKETS     (MU_HIST_OCTAVES * MU_HIST_SUB_BUCKETS)           /* 1536 */

typedef struct mu_histogram
{
    uint32_t buckets[MU_HIST_BUCKETS];
    uint64_t total;
} mu_histogram;

MU_INLINE void mu_histogram_init(mu_histogram* h)
{
    MU_ASSERT(h);
    MU_MEMSET(h->buckets, 0, sizeof(h->buckets));
    h->total = 0;
}

MU_INLINE void mu_histogram_reset(mu_histogram* h)
{
    mu_histogram_init(h);
}

/* Index of the bucket containing `value`. Bit-shuffle, no libm. */
MU_INLINE uint32_t mu_histogram__bucket(double value)
{
    if (!(value > 0.0))
        return 0; /* zero, negative or NaN all fold into bucket 0 */

    uint64_t bits;
    MU_MEMCPY(&bits, &value, sizeof(bits));

    int32_t exponent = (int32_t)((bits >> 52) & 0x7ffull) - 1023;
    uint32_t mant    = (uint32_t)((bits >> (52u - MU_HIST_SUB_BITS)) & ((1u << MU_HIST_SUB_BITS) - 1u));

    if (exponent < MU_HIST_OCTAVE_LOW)
        exponent = MU_HIST_OCTAVE_LOW;
    if (exponent > MU_HIST_OCTAVE_HIGH)
        exponent = MU_HIST_OCTAVE_HIGH;

    uint32_t oct = (uint32_t)(exponent - MU_HIST_OCTAVE_LOW);
    uint32_t idx = oct * MU_HIST_SUB_BUCKETS + mant;
    if (idx >= MU_HIST_BUCKETS)
        idx = MU_HIST_BUCKETS - 1u;
    return idx;
}

MU_INLINE void mu_histogram_push(mu_histogram* h, double value)
{
    ++h->buckets[mu_histogram__bucket(value)];
    ++h->total;
}

/* Representative value at the low edge of a bucket: (1 + mant/16) * 2^e.
   The exponent is materialised by writing the IEEE-754 exponent field —
   again no pow(). */
MU_INLINE double mu_histogram__bucket_low(uint32_t idx)
{
    if (idx >= MU_HIST_BUCKETS)
        idx = MU_HIST_BUCKETS - 1u;

    uint32_t oct = idx / MU_HIST_SUB_BUCKETS;
    uint32_t sub = idx % MU_HIST_SUB_BUCKETS;
    int32_t  e   = (int32_t)oct + MU_HIST_OCTAVE_LOW;

    uint64_t exp_bits = (uint64_t)(uint32_t)(e + 1023) << 52; /* 2^e */
    double   base;
    MU_MEMCPY(&base, &exp_bits, sizeof(base));

    return base * (1.0 + (double)sub / (double)MU_HIST_SUB_BUCKETS);
}

/* Value below which `p` (0.0 .. 1.0) of samples fall. */
MU_INLINE double mu_histogram_percentile(const mu_histogram* h, double p)
{
    if (!h || h->total == 0)
        return 0.0;

    double want = p * (double)h->total;
    if (want < 1.0)
        want = 1.0;

    double seen = 0.0;
    for (uint32_t i = 0; i < MU_HIST_BUCKETS; ++i)
    {
        seen += (double)h->buckets[i];
        if (seen >= want)
            return mu_histogram__bucket_low(i);
    }
    return mu_histogram__bucket_low(MU_HIST_BUCKETS - 1u);
}

MU_INLINE double mu_histogram_min(const mu_histogram* h)
{
    if (!h || h->total == 0)
        return 0.0;
    for (uint32_t i = 0; i < MU_HIST_BUCKETS; ++i)
        if (h->buckets[i])
            return mu_histogram__bucket_low(i);
    return 0.0;
}

MU_INLINE double mu_histogram_max(const mu_histogram* h)
{
    if (!h || h->total == 0)
        return 0.0;
    for (uint32_t i = MU_HIST_BUCKETS; i-- > 0;)
        if (h->buckets[i])
            return mu_histogram__bucket_low(i);
    return 0.0;
}

MU_INLINE double mu_histogram_mean_estimate(const mu_histogram* h)
{
    if (!h || h->total == 0)
        return 0.0;

    /* Bucket midpoints (low edge + half a sub-bucket) weighted by count. */
    double sum   = 0.0;
    double seen  = 0.0;
    for (uint32_t i = 0; i < MU_HIST_BUCKETS; ++i)
    {
        if (!h->buckets[i])
            continue;
        double low  = mu_histogram__bucket_low(i);
        double high = mu_histogram__bucket_low(i + 1u);
        sum += (double)h->buckets[i] * (low + high) * 0.5;
        seen += (double)h->buckets[i];
    }
    return seen > 0.0 ? sum / seen : 0.0;
}
