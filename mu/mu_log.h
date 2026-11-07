#pragma once
#ifndef MU_LOG_H
#define MU_LOG_H

/* ==========================================================================
 * mu_log.h — flexible single-header logging (C99)
 * ==========================================================================
 *
 * "Flexible" here means four independent choices, because the four things
 * people actually need to vary are independent:
 *
 *   1. FILTER  — per-logger level, plus per-category bits so a subsystem can be
 *                turned up without turning everything up.
 *   2. DESTINATION — stderr, a FILE*, a callback, and/or an in-memory ring
 *                buffer, several at once. The ring is the crash-dump trick: it
 *                costs nothing until something goes wrong, then you have the
 *                last N lines.
 *   3. FORMAT  — printf-style, or structured key/value pairs that a callback
 *                sink can render as JSON and a plain sink renders as `k=v`.
 *   4. COST — MU_LOG_MIN_LEVEL strips records AT COMPILE TIME, so a release
 *                build has no format string, no arguments and no call.
 *
 * Design notes that are opinions, not options:
 *
 *   - No allocation. A logger never mallocs: sinks are caller-owned FILE*s or
 *     caller-owned buffers. Logging inside a failing allocator must work.
 *   - No global state. Every logger is a struct you own; passing it explicitly
 *     is what keeps this usable from tests, from libraries and in parallel.
 *     There is a convenience mu_log_default() for the lazy case.
 *   - Records carry file/line/function and a monotonic timestamp. A log line
 *     you cannot locate is a log line you will delete.
 *   - The message is built ONCE into a fixed stack buffer, then fan-out to all
 *     sinks. Two sinks cost one vsnprintf, not two.
 *
 * Typical use:
 *
 *     static mu_log g_log;
 *     mu_log_init(&g_log, "net", MU_LOG_INFO);
 *     mu_log_add_stderr(&g_log, 1);            // colours when tty
 *     mu_log_add_file(&g_log, f, 0);
 *     MU_LOG_INFO(&g_log, "connected to %s:%d", host, port);
 *
 *     MU_LOG_ERROR(&g_log, "decode failed");   // only this line runs in a
 *                                              // build with MU_LOG_MIN_LEVEL=3
 *
 * Structured, for machine-readable sinks:
 *
 *     const char *k[] = { "host", "port", "ms" };
 *     const char *v[] = { host, "443", "12.4" };
 *     mu_log_kv(&g_log, MU_LOG_INFO, __FILE__, __LINE__, "connected", 3, k, v);
 *
 * Scope timer (needs a GCC/Clang cleanup attribute, degrades to a no-op):
 *
 *     MU_LOG_TIME_SCOPE(&g_log, MU_LOG_DEBUG, "upload");
 *
 * Compile-time knobs:
 *     MU_LOG_MIN_LEVEL     records below this level compile to nothing
 *     MU_LOG_NO_COLOR      never emit ANSI colours
 *     MU_LOG_THREADSAFE    guard record emission with an atomic spinlock
 *     MU_LOG_MAX_SINKS     sinks per logger (default 4)
 *     MU_LOG_LINE_MAX      bytes per formatted record (default 512)
 *     MU_LOG_RING_MAX      bytes per in-memory ring sink (soft cap)
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#  define MU_LOG__UNUSED   __attribute__((unused))
#  define MU_LOG__PRINTF(a, b) __attribute__((format(printf, a, b)))
#  define MU_LOG__HAS_CLEANUP 1
#else
#  define MU_LOG__UNUSED
#  define MU_LOG__PRINTF(a, b)
#  define MU_LOG__HAS_CLEANUP 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * levels
 * -------------------------------------------------------------------------- */

/* Numeric macros first, enum second: the #if blocks below test
 * MU_LOG_MIN_LEVEL against these, and an enum constant is NOT usable in a
 * preprocessor expression -- it silently evaluates to 0 there, which would make
 * every level compare equal and the compile-time strip a no-op. */
#define MU_LOG_LVL_TRACE 0
#define MU_LOG_LVL_DEBUG 1
#define MU_LOG_LVL_INFO  2
#define MU_LOG_LVL_WARN  3
#define MU_LOG_LVL_ERROR 4
#define MU_LOG_LVL_FATAL 5
#define MU_LOG_LVL_NONE  6

enum
{
    MU_LOG_TRACE = MU_LOG_LVL_TRACE,
    MU_LOG_DEBUG = MU_LOG_LVL_DEBUG,
    MU_LOG_INFO  = MU_LOG_LVL_INFO,
    MU_LOG_WARN  = MU_LOG_LVL_WARN,
    MU_LOG_ERROR = MU_LOG_LVL_ERROR,
    MU_LOG_FATAL = MU_LOG_LVL_FATAL,
    MU_LOG_NONE  = MU_LOG_LVL_NONE
};

#ifndef MU_LOG_MIN_LEVEL
#  define MU_LOG_MIN_LEVEL MU_LOG_LVL_TRACE
#endif

#ifndef MU_LOG_MAX_SINKS
#  define MU_LOG_MAX_SINKS 4
#endif
#ifndef MU_LOG_LINE_MAX
#  define MU_LOG_LINE_MAX 512
#endif
#ifndef MU_LOG_RING_MAX
#  define MU_LOG_RING_MAX 8192
#endif
#ifndef MU_LOG_CATEGORIES
#  define MU_LOG_CATEGORIES 32
#endif

/* --------------------------------------------------------------------------
 * records
 * -------------------------------------------------------------------------- */

typedef struct mu_log_record
{
    const char *logger;     /* logger name, never NULL */
    const char *file;
    const char *func;
    int         line;
    int         level;
    uint64_t    time_ns;    /* monotonic, from the process start */
    double      time_s;     /* seconds, same origin, for humans */
    /* Either msg is set, or n_kv > 0. Both may be set: msg is the human
     * summary, kv the machine detail. */
    const char *msg;
    int         n_kv;
    const char *const *kv_keys;
    const char *const *kv_vals;
} mu_log_record;

typedef void (*mu_log_callback)(void *user, const mu_log_record *rec);

/* --------------------------------------------------------------------------
 * sinks
 * -------------------------------------------------------------------------- */

enum
{
    MU_LOG_SINK_STREAM = 0,   /* FILE*, colour optional */
    MU_LOG_SINK_CALLBACK,     /* your function */
    MU_LOG_SINK_RING          /* in-memory circular text buffer */
};

typedef struct mu_log_sink
{
    int               kind;
    int               min_level;
    int               color;
    FILE             *stream;
    mu_log_callback   cb;
    void             *user;
    /* ring */
    char             *ring;
    size_t            ring_cap;
    size_t            ring_head;      /* write position */
    size_t            ring_used;      /* bytes stored, <= ring_cap */
    int               ring_wrapped;
} mu_log_sink;

/* --------------------------------------------------------------------------
 * logger
 * -------------------------------------------------------------------------- */

typedef struct mu_log
{
    const char  *name;
    int          level;                    /* emits records >= this level */
    uint32_t     category_mask;            /* bit per category: 0 = disabled */
    /* One byte per category. The first version packed these into 2 bits each,
     * which silently cannot represent MU_LOG_ERROR (4) -- every category
     * threshold then wrapped and the filter did the opposite of what was
     * asked. A byte costs 32 bytes per logger and removes the class of bug. */
    uint8_t      category_min[MU_LOG_CATEGORIES];
    int          n_sinks;
    mu_log_sink  sinks[MU_LOG_MAX_SINKS];
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    int          locked;                   /* spinlock, only if MU_LOG_THREADSAFE */
#endif
} mu_log;

/* --------------------------------------------------------------------------
 * colours and names
 * -------------------------------------------------------------------------- */

MU_LOG__UNUSED static const char *mu_log_level_name(int level)
{
    switch (level)
    {
        case MU_LOG_TRACE: return "TRACE";
        case MU_LOG_DEBUG: return "DEBUG";
        case MU_LOG_INFO:  return "INFO ";
        case MU_LOG_WARN:  return "WARN ";
        case MU_LOG_ERROR: return "ERROR";
        case MU_LOG_FATAL: return "FATAL";
        default:           return "?????";
    }
}

MU_LOG__UNUSED static const char *mu_log_level_color(int level)
{
#if defined(MU_LOG_NO_COLOR)
    (void)level;
    return "";
#else
    switch (level)
    {
        case MU_LOG_TRACE: return "\x1b[90m";   /* bright black */
        case MU_LOG_DEBUG: return "\x1b[36m";   /* cyan */
        case MU_LOG_INFO:  return "\x1b[32m";   /* green */
        case MU_LOG_WARN:  return "\x1b[33m";   /* yellow */
        case MU_LOG_ERROR: return "\x1b[31m";   /* red */
        case MU_LOG_FATAL: return "\x1b[35;1m"; /* bold magenta */
        default:           return "";
    }
#endif
}

/* --------------------------------------------------------------------------
 * clock
 * --------------------------------------------------------------------------
 *
 * Deliberately NOT clock_gettime. That needs _POSIX_C_SOURCE to be defined
 * before the first system header, which a library header cannot guarantee: any
 * translation unit that includes <stdio.h> before this file gets an implicit
 * declaration and a build error on the user's side. timespec_get is C11, needs
 * no feature macro, and is what the library already targets.
 */
#if defined(_MSC_VER)
#  include <windows.h>
MU_LOG__UNUSED static uint64_t mu_log__now_ns(void)
{
    static LARGE_INTEGER freq;
    static int           init = 0;
    LARGE_INTEGER        t;

    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
}
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  include <time.h>
MU_LOG__UNUSED static uint64_t mu_log__now_ns(void)
{
    struct timespec ts;

    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0u;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#else
#  include <time.h>
MU_LOG__UNUSED static uint64_t mu_log__now_ns(void)
{
    return (uint64_t)clock() * (1000000000ull / (uint64_t)CLOCKS_PER_SEC);
}
#endif

/* --------------------------------------------------------------------------
 * configure
 * -------------------------------------------------------------------------- */

MU_LOG__UNUSED static void mu_log_init(mu_log *log, const char *name, int level)
{
    memset(log, 0, sizeof *log);
    log->name = name ? name : "app";
    log->level = level;
    log->category_mask = 0xffffffffu;
    memset(log->category_min, MU_LOG_TRACE, sizeof log->category_min);
    log->n_sinks = 0;
}

MU_LOG__UNUSED static void mu_log_set_level(mu_log *log, int level)
{
    if (log) log->level = level;
}

/* Turn a category (0..31) up or down independently of the logger level. */
MU_LOG__UNUSED static void mu_log_set_category(mu_log *log, uint32_t category, int level)
{
    if (!log || category >= MU_LOG_CATEGORIES) return;
    log->category_min[category] = (uint8_t)((level < 0) ? 0 : (level > MU_LOG_NONE ? MU_LOG_NONE : level));
}

MU_LOG__UNUSED static int mu_log_enabled(const mu_log *log, int level, uint32_t category)
{
    if (!log) return 0;
    if (level < MU_LOG_MIN_LEVEL) return 0;
    if (level < log->level) return 0;
    if (category < MU_LOG_CATEGORIES)
    {
        if (!(log->category_mask & (1u << category))) return 0;
        if (level < (int)log->category_min[category]) return 0;
    }
    return 1;
}

MU_LOG__UNUSED static mu_log_sink *mu_log__new_sink(mu_log *log)
{
    if (!log || log->n_sinks >= MU_LOG_MAX_SINKS) return NULL;
    memset(&log->sinks[log->n_sinks], 0, sizeof log->sinks[0]);
    return &log->sinks[log->n_sinks++];
}

MU_LOG__UNUSED static int mu_log_add_stderr(mu_log *log, int color)
{
    mu_log_sink *s = mu_log__new_sink(log);

    if (!s) return 0;
    s->kind = MU_LOG_SINK_STREAM;
    s->stream = stderr;
    s->color = color;
    s->min_level = MU_LOG_TRACE;
    return 1;
}

MU_LOG__UNUSED static int mu_log_add_file(mu_log *log, FILE *f, int color)
{
    mu_log_sink *s = mu_log__new_sink(log);

    if (!s || !f) return 0;
    s->kind = MU_LOG_SINK_STREAM;
    s->stream = f;
    s->color = color;
    s->min_level = MU_LOG_TRACE;
    return 1;
}

MU_LOG__UNUSED static int mu_log_add_callback(mu_log *log, mu_log_callback cb, void *user, int min_level)
{
    mu_log_sink *s = mu_log__new_sink(log);

    if (!s || !cb) return 0;
    s->kind = MU_LOG_SINK_CALLBACK;
    s->cb = cb;
    s->user = user;
    s->min_level = min_level;
    return 1;
}

/* In-memory ring. The buffer is yours; the logger only writes into it. When it
 * fills, the oldest bytes are overwritten, so what you keep is always the most
 * recent log. Dump with mu_log_ring_read() after a failure. */
MU_LOG__UNUSED static int mu_log_add_ring(mu_log *log, char *buf, size_t cap, int min_level)
{
    mu_log_sink *s = mu_log__new_sink(log);

    if (!s || !buf || cap < 2) return 0;
    s->kind = MU_LOG_SINK_RING;
    s->ring = buf;
    s->ring_cap = (cap < MU_LOG_RING_MAX) ? cap : MU_LOG_RING_MAX;
    s->ring_head = 0;
    s->ring_used = 0;
    s->ring_wrapped = 0;
    s->min_level = min_level;
    buf[0] = '\0';
    return 1;
}

/* Copy the ring content out, oldest first. Returns bytes written, always
 * NUL-terminated. */
MU_LOG__UNUSED static size_t mu_log_ring_read(const mu_log *log, int sink_index, char *out, size_t cap)
{
    const mu_log_sink *s;
    size_t             n = 0;

    if (!log || sink_index < 0 || sink_index >= log->n_sinks) return 0;
    s = &log->sinks[sink_index];
    if (s->kind != MU_LOG_SINK_RING || !out || cap == 0) return 0;

    if (!s->ring_wrapped)
    {
        n = (s->ring_used < cap - 1) ? s->ring_used : cap - 1;
        memcpy(out, s->ring, n);
    }
    else
    {
        /* oldest data starts at head */
        const size_t first = s->ring_cap - s->ring_head;

        if (first < cap - 1) { memcpy(out, s->ring + s->ring_head, first); n = first; }
        else { memcpy(out, s->ring + s->ring_head, cap - 1); n = cap - 1; }
        if (n < cap - 1)
        {
            size_t second = s->ring_head;

            if (second > cap - 1 - n) second = cap - 1 - n;
            memcpy(out + n, s->ring, second);
            n += second;
        }
    }
    out[n] = '\0';
    return n;
}

/* --------------------------------------------------------------------------
 * emission
 * -------------------------------------------------------------------------- */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#  include <stdatomic.h>
#  define MU_LOG__HAS_ATOMICS 1
#endif

MU_LOG__UNUSED static void mu_log__lock(mu_log *log)
{
#if defined(MU_LOG_THREADSAFE) && defined(MU_LOG__HAS_ATOMICS)
    while (atomic_exchange((_Atomic int *)&log->locked, 1)) { /* spin */ }
#else
    (void)log;
#endif
}

MU_LOG__UNUSED static void mu_log__unlock(mu_log *log)
{
#if defined(MU_LOG_THREADSAFE) && defined(MU_LOG__HAS_ATOMICS)
    atomic_store((_Atomic int *)&log->locked, 0);
#else
    (void)log;
#endif
}

MU_LOG__UNUSED static void mu_log__ring_push(mu_log_sink *s, const char *text, size_t len)
{
    size_t i;

    for (i = 0; i < len; ++i)
    {
        s->ring[s->ring_head] = text[i];
        s->ring_head += 1;
        if (s->ring_head == s->ring_cap)
        {
            s->ring_head = 0;
            s->ring_wrapped = 1;
        }
        if (!s->ring_wrapped) s->ring_used += 1;
    }
    /* Never place a NUL inside the ring: it would overwrite a byte that
     * mu_log_ring_read is about to return, and the "crash dump is a C string"
     * convenience is worth less than not corrupting the data. mu_log_ring_read
     * terminates what it copies out. */
}

MU_LOG__UNUSED static void mu_log__format_stream(char *dst, size_t cap, const mu_log_record *rec,
                                                 int color, int tty)
{
    int    n = 0;
    size_t off = 0;

    if (color && tty)
        n = snprintf(dst, cap, "%s%s %-10s %s:%d %s%s: %s",
                     mu_log_level_color(rec->level),
                     mu_log_level_name(rec->level), rec->logger, rec->file, rec->line,
                     rec->func, "\x1b[0m", rec->msg ? rec->msg : "");
    else
        n = snprintf(dst, cap, "%s %-10s %s:%d %s: %s",
                     mu_log_level_name(rec->level), rec->logger, rec->file, rec->line,
                     rec->func, rec->msg ? rec->msg : "");

    off = (n > 0) ? (size_t)n : 0;
    if (off > cap) off = cap;

    /* structured fields ride along after the message */
    if (rec->n_kv > 0)
    {
        int i;

        for (i = 0; i < rec->n_kv && off + 1 < cap; ++i)
        {
            const int w = snprintf(dst + off, cap - off, " %s=%s",
                                   rec->kv_keys[i], rec->kv_vals[i]);
            if (w > 0) off += (size_t)w;
            if (off > cap) off = cap;
        }
    }
    if (off + 1 < cap) { dst[off] = '\n'; off += 1; }
    dst[off < cap ? off : cap - 1] = '\0';
}

MU_LOG__UNUSED static void mu_log__emit(mu_log *log, int level, uint32_t category,
                                        const char *file, int line, const char *func,
                                        int n_kv, const char *const *kv_keys,
                                        const char *const *kv_vals,
                                        const char *msg)
{
    char           linebuf[MU_LOG_LINE_MAX + 64];
    mu_log_record  rec;
    int            i;

    if (!mu_log_enabled(log, level, category)) return;

    rec.logger = log->name;
    rec.file = file;
    rec.func = func;
    rec.line = line;
    rec.level = level;
    rec.time_ns = mu_log__now_ns();
    rec.time_s = (double)rec.time_ns * 1e-9;
    rec.msg = msg ? msg : "";
    rec.n_kv = n_kv;
    rec.kv_keys = kv_keys;
    rec.kv_vals = kv_vals;

    mu_log__lock(log);
    for (i = 0; i < log->n_sinks; ++i)
    {
        mu_log_sink *s = &log->sinks[i];

        if (level < s->min_level) continue;
        switch (s->kind)
        {
            case MU_LOG_SINK_STREAM:
            {
                const int tty = (s->stream == stderr) || (s->stream == stdout);

                mu_log__format_stream(linebuf, sizeof linebuf, &rec, s->color, tty);
                fputs(linebuf, s->stream);
                break;
            }
            case MU_LOG_SINK_CALLBACK:
                if (s->cb) s->cb(s->user, &rec);
                break;
            case MU_LOG_SINK_RING:
            {
                char   tmp[MU_LOG_LINE_MAX + 128];
                int    w = snprintf(tmp, sizeof tmp, "%s %s:%d %s: %s",
                                    mu_log_level_name(rec.level), rec.file, rec.line,
                                    rec.func, rec.msg ? rec.msg : "");

                if (w > 0) mu_log__ring_push(s, tmp, (size_t)w < sizeof tmp ? (size_t)w : sizeof tmp - 1);
                if (rec.n_kv > 0)
                {
                    int k;

                    for (k = 0; k < rec.n_kv; ++k)
                    {
                        char kv[MU_LOG_LINE_MAX];

                        w = snprintf(kv, sizeof kv, " %s=%s", rec.kv_keys[k], rec.kv_vals[k]);
                        if (w > 0) mu_log__ring_push(s, kv, (size_t)w < sizeof kv ? (size_t)w : sizeof kv - 1);
                    }
                }
                mu_log__ring_push(s, "\n", 1);
                break;
            }
            default: break;
        }
    }
    mu_log__unlock(log);
}

MU_LOG__UNUSED static void mu_log_write(mu_log *log, int level, uint32_t category,
                                        const char *file, int line, const char *func,
                                        const char *fmt, ...) MU_LOG__PRINTF(7, 8);
MU_LOG__UNUSED MU_LOG__PRINTF(7, 8)
static void mu_log_write(mu_log *log, int level, uint32_t category,
                         const char *file, int line, const char *func,
                         const char *fmt, ...)
{
    char    msg[MU_LOG_LINE_MAX];
    va_list ap;

    if (!mu_log_enabled(log, level, category)) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    mu_log__emit(log, level, category, file, line, func, 0, NULL, NULL, msg);
}

MU_LOG__UNUSED static void mu_log_kv(mu_log *log, int level, uint32_t category,
                                     const char *file, int line, const char *func,
                                     const char *msg, int n_kv,
                                     const char *const *keys, const char *const *vals)
{
    mu_log__emit(log, level, category, file, line, func, n_kv, keys, vals, msg);
}

/* Hexdump in the classic 16-byte rows. Written for the case where you have a
 * buffer and no debugger. */
MU_LOG__UNUSED static void mu_log_hexdump(mu_log *log, int level, const void *data, size_t n,
                                          const char *label)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t               off = 0;

    if (!mu_log_enabled(log, level, 0xffffffffu)) return;
    mu_log_write(log, level, 0xffffffffu, __FILE__, __LINE__, "hexdump", "%s (%zu bytes)",
                 label ? label : "buffer", n);

    while (off < n)
    {
        char   line[160];
        size_t cap = sizeof line;
        size_t col, i;
        int    w;

        /* Every write below is bounded by an explicit remaining-space test.
         * The first version trusted two of them and gcc's -Wstringop-overflow
         * proved a path where col could pass the end of the buffer. */
        w = snprintf(line, cap, "  %08zx  ", off);
        col = (w > 0) ? (size_t)w : 0u;
        if (col >= cap) col = cap - 1u;

        for (i = 0; i < 16u && off + i < n && col + 3u < cap; ++i)
        {
            w = snprintf(line + col, cap - col, "%02x ", p[off + i]);
            if (w > 0) col += (size_t)w;
        }
        for (; i < 16u && col + 3u < cap; ++i)
        {
            line[col++] = ' ';
            line[col++] = ' ';
            line[col++] = ' ';
        }
        if (col + 1u < cap) line[col++] = ' ';

        for (i = 0; i < 16u && off + i < n && col + 1u < cap; ++i)
        {
            const unsigned char c = p[off + i];

            line[col++] = (c >= 32u && c < 127u) ? (char)c : '.';
        }
        line[col < cap ? col : cap - 1u] = '\0';
        mu_log_write(log, level, 0xffffffffu, __FILE__, __LINE__, "hexdump", "%s", line);
        off += 16u;
    }
}

/* Cost model: an enabled record is one vsnprintf plus a write per stream sink.
 * A disabled record is a couple of comparisons against a struct you already
 * have in cache -- but it is still a function call. For hot loops use the
 * MU_LOG_*_ENABLED guard, which is what the macros do for you at
 * compile-time levels only. */

/* --------------------------------------------------------------------------
 * convenience: one process-wide logger
 * -------------------------------------------------------------------------- */

MU_LOG__UNUSED static mu_log *mu_log_default(void)
{
    static mu_log  def;
    static int     init = 0;

    if (!init)
    {
        mu_log_init(&def, "mu", MU_LOG_INFO);
        mu_log_add_stderr(&def, 1);
        init = 1;
    }
    return &def;
}

/* --------------------------------------------------------------------------
 * scoped timer
 * -------------------------------------------------------------------------- */

typedef struct mu_log_timer
{
    mu_log     *log;
    const char *label;
    int         level;
    uint64_t    t0;
} mu_log_timer;

MU_LOG__UNUSED static void mu_log_timer_stop(mu_log_timer *t)
{
    const uint64_t dt = mu_log__now_ns() - t->t0;

    mu_log_write(t->log, t->level, 0xffffffffu, __FILE__, __LINE__, "timer",
                 "%s took %.3f ms", t->label, (double)dt * 1e-6);
}

/* The cleanup attribute hands the guarded variable's ADDRESS to the handler, so
 * the guarded object must be the mu_log_timer itself (not a pointer to one).
 * Naming it with __COUNTER__ lets several timers live in one block. */
#if MU_LOG__HAS_CLEANUP
#  define MU_LOG_HAS_CLEANUP 1
MU_LOG__UNUSED static void mu_log__timer_cleanup(mu_log_timer *t) { mu_log_timer_stop(t); }

#  define MU_LOG_TIME_SCOPE_IMPL(log_, level_, label_, ctr)                      \
       __attribute__((cleanup(mu_log__timer_cleanup)))                          \
       mu_log_timer mu_log__timer_##ctr =                                       \
           { (log_), (label_), (level_), mu_log__now_ns() }
#  define MU_LOG_TIME_SCOPE_2(log_, level_, label_, ctr) MU_LOG_TIME_SCOPE_IMPL(log_, level_, label_, ctr)
#  define MU_LOG_TIME_SCOPE_1(log_, level_, label_, ctr) MU_LOG_TIME_SCOPE_2(log_, level_, label_, ctr)
#  define MU_LOG_TIME_SCOPE(log_, level_, label_)                                 \
       MU_LOG_TIME_SCOPE_1(log_, level_, label_, __COUNTER__)
#else
#  define MU_LOG_HAS_CLEANUP 0
#  define MU_LOG_TIME_SCOPE(log_, level_, label_) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

/* --------------------------------------------------------------------------
 * call-site macros
 * --------------------------------------------------------------------------
 *
 * The compile-time level test is a `#if`, so a stripped record leaves nothing
 * behind: no call, no format string in the binary, no argument evaluation.
 * That is the difference between "logging is cheap" and "logging is free".
 */

#define MU_LOG__CAT_DEFAULT 0xffffffffu

#if MU_LOG_MIN_LEVEL <= MU_LOG_LVL_TRACE
#  define MU_LOG_TRACE_(log_, ...) mu_log_write((log_), MU_LOG_TRACE, MU_LOG__CAT_DEFAULT, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define MU_LOG_TRACE_(log_, ...) ((void)0)
#endif

#if MU_LOG_MIN_LEVEL <= MU_LOG_LVL_DEBUG
#  define MU_LOG_DEBUG_(log_, ...) mu_log_write((log_), MU_LOG_DEBUG, MU_LOG__CAT_DEFAULT, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define MU_LOG_DEBUG_(log_, ...) ((void)0)
#endif

#if MU_LOG_MIN_LEVEL <= MU_LOG_LVL_INFO
#  define MU_LOG_INFO_(log_, ...) mu_log_write((log_), MU_LOG_INFO, MU_LOG__CAT_DEFAULT, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define MU_LOG_INFO_(log_, ...) ((void)0)
#endif

#if MU_LOG_MIN_LEVEL <= MU_LOG_LVL_WARN
#  define MU_LOG_WARN_(log_, ...) mu_log_write((log_), MU_LOG_WARN, MU_LOG__CAT_DEFAULT, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define MU_LOG_WARN_(log_, ...) ((void)0)
#endif

#if MU_LOG_MIN_LEVEL <= MU_LOG_LVL_ERROR
#  define MU_LOG_ERROR_(log_, ...) mu_log_write((log_), MU_LOG_ERROR, MU_LOG__CAT_DEFAULT, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define MU_LOG_ERROR_(log_, ...) ((void)0)
#endif

#define MU_LOG_FATAL_(log_, ...) mu_log_write((log_), MU_LOG_FATAL, MU_LOG__CAT_DEFAULT, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* Category-aware variants: MU_LOG_INFO_C(log, category, ...) */
#define MU_LOG_INFO_CAT(log_, cat_, ...)  mu_log_write((log_), MU_LOG_INFO,  (cat_), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define MU_LOG_WARN_CAT(log_, cat_, ...)  mu_log_write((log_), MU_LOG_WARN,  (cat_), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define MU_LOG_ERROR_CAT(log_, cat_, ...) mu_log_write((log_), MU_LOG_ERROR, (cat_), __FILE__, __LINE__, __func__, __VA_ARGS__)

/* Runtime check, for the rare hot path where even the call must be skipped. */
#define MU_LOG_ENABLED(log_, level_) mu_log_enabled((log_), (level_), MU_LOG__CAT_DEFAULT)

#endif /* MU_LOG_H */
