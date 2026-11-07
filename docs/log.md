# `mu_log.h` - flexible logging

Four independent axes: **filter**, **destination**, **format**, and **cost**.

```c
static mu_log g_log;

mu_log_init(&g_log, "net", MU_LOG_INFO);
mu_log_add_stderr(&g_log, 1);            /* colours when attached to a terminal */
mu_log_add_file(&g_log, fp, 0);
mu_log_add_callback(&g_log, on_record, ctx, MU_LOG_TRACE);
mu_log_add_ring(&g_log, ring, sizeof ring, MU_LOG_TRACE);

MU_LOG_INFO_(&g_log, "connected to %s:%d", host, port);
MU_LOG_ERROR_(&g_log, "decode failed");
MU_LOG_INFO_CAT(&g_log, 3u, "per-category record");
```

## Filter

`mu_log.level` is a threshold; `category` is a 32-wide bitmask **plus** a
per-category threshold byte, so a subsystem can be turned up on its own:

```c
mu_log_set_category(&g_log, 3u, MU_LOG_DEBUG);   /* category 3 louder, others unchanged */
```

(The first implementation packed thresholds into 2 bits each. Two bits cannot
represent `MU_LOG_ERROR` (4), so every threshold wrapped and the filter did the
opposite of what was asked. `test_log.c` covers each level through a category.)

## Destination

Sinks are caller-owned; the logger never allocates:

| sink | use |
| --- | --- |
| `mu_log_add_stderr` / `mu_log_add_file` | human output, optional ANSI colour |
| `mu_log_add_callback` | JSON, ring-to-disk, network, test capture |
| `mu_log_add_ring` | in-memory circular text buffer, the crash-dump trick |

The ring keeps the **newest** `MU_LOG_RING_MAX` bytes and `mu_log_ring_read`
returns them oldest-first, NUL-terminated. It deliberately does not store a
terminator inside the buffer: that would overwrite data the reader is about to
return.

## Format

printf-style, or structured pairs that a callback sink renders as JSON and a
stream sink renders as `k=v`:

```c
const char *k[] = { "host", "port", "ms" };
const char *v[] = { host, "443", "12.4" };
mu_log_kv(&g_log, MU_LOG_INFO, 0xffffffffu, __FILE__, __LINE__, "f", "connected", 3, k, v);
```

Each record also carries file, line, function, a monotonic timestamp and the
logger name: a log line you cannot locate is a log line you will delete.

## Cost

`MU_LOG_MIN_LEVEL` strips records **at compile time**: no call, no format string
in the binary, no argument evaluation.

```
gcc -DMU_LOG_MIN_LEVEL=MU_LOG_LVL_WARN   # INFO and below vanish
```

Verified in the gate by preprocessing at two levels and grepping for the format
string. An *enabled* record costs one `vsnprintf` plus one write per stream sink
(the message is formatted once, then fan-out - two sinks do not format twice).
A *disabled* record is a couple of comparisons, but it is still a function call:
guard hot loops with `MU_LOG_ENABLED(log, level)`.

## Extras

* `mu_log_hexdump(log, level, data, n, label)` - 16-byte rows with an ASCII column.
* `MU_LOG_TIME_SCOPE(log, level, "label")` - scope timer via
  `__attribute__((cleanup))`, fires at every exit path.
* `mu_log_default()` - lazily initialised process-wide logger for the lazy case.

## Not included, on purpose

No allocation, no global state beyond the one convenience logger, no threads
unless you define `MU_LOG_THREADSAFE` (then a C11 atomic spinlock guards
emission). A logger that mallocs is a logger that fails inside a failing
allocator.
