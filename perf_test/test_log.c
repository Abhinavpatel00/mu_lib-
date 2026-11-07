/* test_log.c -- gate for mu_log.h.
 *
 * A logger's failure modes are quiet: a filtered record that should have
 * appeared, a ring that returns the newest bytes instead of the oldest, a
 * fan-out that formats twice. Each of those gets an assertion here.
 */

#include <stdio.h>
#include <string.h>

#include "mu_log.h"

static int g_checks, g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) { ++g_fails; if (g_fails <= 20) printf("  FAIL %s\n", what); }
}

/* ---- capture sink ---- */

static char  g_cap[4096];
static size_t g_cap_len;
static int   g_cap_n;

static void capture(void *user, const mu_log_record *r)
{
    (void)user;
    ++g_cap_n;
    g_cap_len += (size_t)snprintf(g_cap + g_cap_len, sizeof g_cap - g_cap_len,
                                  "[%s]%s;", mu_log_level_name(r->level), r->msg ? r->msg : "");
}

static void test_levels(void)
{
    mu_log lg;

    mu_log_init(&lg, "t", MU_LOG_WARN);
    mu_log_add_callback(&lg, capture, NULL, MU_LOG_TRACE);
    g_cap_n = 0; g_cap_len = 0; g_cap[0] = '\0';

    mu_log_write(&lg, MU_LOG_DEBUG, 0xffffffffu, __FILE__, __LINE__, "f", "debug line");
    mu_log_write(&lg, MU_LOG_INFO,  0xffffffffu, __FILE__, __LINE__, "f", "info line");
    check(g_cap_n == 0, "levels: records below the logger level are dropped");
    check(mu_log_enabled(&lg, MU_LOG_DEBUG, 0xffffffffu) == 0, "levels: enabled(DEBUG) false");
    check(mu_log_enabled(&lg, MU_LOG_ERROR, 0xffffffffu) == 1, "levels: enabled(ERROR) true");

    MU_LOG_ERROR_(&lg, "error %d", 7);
    check(g_cap_n == 1, "levels: the first record at the level arrives");
    check(strstr(g_cap, "error 7") != NULL, "levels: formatted with arguments");

    mu_log_set_level(&lg, MU_LOG_TRACE);
    MU_LOG_DEBUG_(&lg, "now visible");
    check(g_cap_n == 2, "levels: raising the level admits the record");
}

static void test_categories(void)
{
    mu_log lg;

    mu_log_init(&lg, "t", MU_LOG_TRACE);
    mu_log_add_callback(&lg, capture, NULL, MU_LOG_TRACE);
    g_cap_n = 0;

    mu_log_set_category(&lg, 3, MU_LOG_ERROR);
    mu_log_write(&lg, MU_LOG_INFO, 3u, __FILE__, __LINE__, "f", "cat3 info");
    check(g_cap_n == 0, "categories: a raised category filters its own level");
    mu_log_write(&lg, MU_LOG_INFO, 4u, __FILE__, __LINE__, "f", "cat4 info");
    check(g_cap_n == 1, "categories: other categories unaffected");
    mu_log_write(&lg, MU_LOG_ERROR, 3u, __FILE__, __LINE__, "f", "cat3 error");
    check(g_cap_n == 2, "categories: the error still passes in that category");
}

static void test_ring(void)
{
    mu_log lg;
    char   ring[128];
    char   out[256];

    mu_log_init(&lg, "t", MU_LOG_TRACE);
    check(mu_log_add_ring(&lg, ring, sizeof ring, MU_LOG_TRACE) == 1, "ring: attached");

    MU_LOG_INFO_(&lg, "alpha");
    MU_LOG_INFO_(&lg, "beta");
    mu_log_ring_read(&lg, 0, out, sizeof out);
    check(strstr(out, "alpha") && strstr(out, "beta"), "ring: both records present");
    check(strstr(out, "alpha") < strstr(out, "beta"), "ring: oldest first");
    check(out[strlen(out) - 1] == '\n', "ring: newline terminated");

    /* overflow: the ring keeps the NEWEST bytes and stays a C string */
    {
        int i;

        for (i = 0; i < 40; ++i) MU_LOG_INFO_(&lg, "filler record %03d", i);
        mu_log_ring_read(&lg, 0, out, sizeof out);
        check(strstr(out, "filler record 039") != NULL, "ring: newest record survives the wrap");
        check(strstr(out, "alpha") == NULL, "ring: oldest records were overwritten");
        check(memchr(out, '\0', sizeof out) != NULL, "ring: still NUL-terminated");
        check(strlen(out) < sizeof out, "ring: read respects the caller's buffer");
    }
}

static void test_kv_and_hexdump(void)
{
    mu_log lg;
    char   ring[512];
    char   out[512];

    mu_log_init(&lg, "t", MU_LOG_TRACE);
    mu_log_add_ring(&lg, ring, sizeof ring, MU_LOG_TRACE);
    mu_log_add_callback(&lg, capture, NULL, MU_LOG_TRACE);
    g_cap_n = 0;

    {
        const char *k[] = { "host", "port" };
        const char *v[] = { "example", "443" };

        mu_log_kv(&lg, MU_LOG_INFO, 0xffffffffu, __FILE__, __LINE__, "f",
                  "connected", 2, k, v);
    }
    mu_log_ring_read(&lg, 0, out, sizeof out);
    check(strstr(out, "host=example") != NULL, "kv: key/value rendered");
    check(strstr(out, "port=443") != NULL, "kv: second pair rendered");
    check(g_cap_n == 1, "kv: one record, one fan-out");

    mu_log_init(&lg, "t", MU_LOG_TRACE);
    mu_log_add_ring(&lg, ring, sizeof ring, MU_LOG_TRACE);
    mu_log_hexdump(&lg, MU_LOG_INFO, "ABCD", 4, "buf");
    mu_log_ring_read(&lg, 0, out, sizeof out);
    check(strstr(out, "41 42 43 44") != NULL, "hexdump: bytes rendered");
    check(strstr(out, "ABCD") != NULL, "hexdump: ascii column rendered");
}

static void test_default_and_timer(void)
{
    mu_log *def = mu_log_default();

    check(def != NULL, "default: logger exists");
    check(mu_log_enabled(def, MU_LOG_INFO, 0xffffffffu) == 1, "default: INFO enabled");

#if MU_LOG_HAS_CLEANUP
    {
        int before = 0;
        (void)before;
        {
            mu_log lg;
            char   ring[256];
            char   out[256];

            mu_log_init(&lg, "t", MU_LOG_TRACE);
            mu_log_add_ring(&lg, ring, sizeof ring, MU_LOG_TRACE);
            {
                MU_LOG_TIME_SCOPE(&lg, MU_LOG_DEBUG, "unit-of-work");
            }
            mu_log_ring_read(&lg, 0, out, sizeof out);
            check(strstr(out, "unit-of-work") != NULL, "timer: fires at scope exit");
            check(strstr(out, "ms") != NULL, "timer: reports a duration");
        }
    }
#else
    check(1, "timer: cleanup attribute unavailable on this compiler");
#endif
}

int main(void)
{
    printf("mu_log correctness gate\n");
    printf("----------------------------------------------------------\n");
    test_levels();
    test_categories();
    test_ring();
    test_kv_and_hexdump();
    test_default_and_timer();
    printf("----------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
