/* test_scope_stack.c -- gate for mu_scope_stack.h.
 *
 * The interesting assertions are the ones a linear allocator can get wrong:
 * that a rewind actually releases everything the frame took (and nothing more),
 * that destructors run newest-first, that a refused allocation leaves the
 * allocator untouched, and that longjmp can be recovered from.
 */

#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#include "mu_scope_stack.h"

static int g_checks, g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) { ++g_fails; if (g_fails <= 20) printf("  FAIL %s\n", what); }
}

static void check_sz(size_t got, size_t want, const char *what)
{
    ++g_checks;
    if (got != want)
    {
        ++g_fails;
        if (g_fails <= 20) printf("  FAIL %s (got %zu want %zu)\n", what, got, want);
    }
}

static char          g_mem[4096];
static mu_scope_stack g_ss;
static int           g_order[8], g_norder;

static void dtor_first(void *p, void *u)  { (void)p; (void)u; g_order[g_norder++] = 1; }
static void dtor_second(void *p, void *u) { (void)p; (void)u; g_order[g_norder++] = 2; }
static void dtor_third(void *p, void *u)  { (void)p; (void)u; g_order[g_norder++] = 3; }

static void test_lifo_reuse(void)
{
    size_t first_a = 0, first_b = 0;
    int    i;

    mu_scope_init(&g_ss, g_mem, sizeof g_mem);
    for (i = 0; i < 3; ++i)
    {
        /* the guard must live in an inner block: the cleanup attribute fires at
         * the end of ITS block, so asserting "clean" in the same block asserts
         * before the rewind and fails */
        {
            MU_SCOPE_LIFETIME(&g_ss);
            void *a = mu_scope_alloc(&g_ss, 100);
            void *b = mu_scope_array(&g_ss, float, 64);

            check(a != NULL && b != NULL, "lifo: allocations succeed");
            check(((uintptr_t)a & (MU_SCOPE_ALIGN - 1u)) == 0u, "lifo: 16-byte aligned a");
            check(((uintptr_t)b & (MU_SCOPE_ALIGN - 1u)) == 0u, "lifo: 16-byte aligned b");
            if (i == 0) { first_a = (size_t)(uintptr_t)a; first_b = (size_t)(uintptr_t)b; }
            else
            {
                check((size_t)(uintptr_t)a == first_a, "lifo: same address each iteration");
                check((size_t)(uintptr_t)b == first_b, "lifo: reused, no drift");
            }
        }
        check_sz(mu_scope_used(&g_ss), 0u, "lifo: clean after scope exit");
    }
    check(mu_scope_peak(&g_ss) > 100u, "lifo: peak recorded");
    check(mu_scope_peak(&g_ss) < 400u, "lifo: peak is one frame, not three");
}

static void test_destructors(void)
{
    mu_scope_frame f;

    mu_scope_init(&g_ss, g_mem, sizeof g_mem);
    g_norder = 0;

    f = mu_scope_enter(&g_ss);
    mu_scope_register(&g_ss, NULL, dtor_first, NULL);
    mu_scope_register(&g_ss, NULL, dtor_second, NULL);
    mu_scope_register(&g_ss, NULL, dtor_third, NULL);
    check(g_norder == 0, "dtor: nothing runs while the frame is live");

    mu_scope_leave(&g_ss, f);
    check_sz((size_t)g_norder, 3u, "dtor: all three ran");
    check(g_order[0] == 3 && g_order[1] == 2 && g_order[2] == 1,
          "dtor: newest first (reverse order of registration)");

    /* an inner frame must not run the outer frame's destructors */
    g_norder = 0;
    {
        mu_scope_frame outer = mu_scope_enter(&g_ss);

        mu_scope_register(&g_ss, NULL, dtor_first, NULL);
        {
            mu_scope_frame inner = mu_scope_enter(&g_ss);

            (void)mu_scope_alloc(&g_ss, 64);
            mu_scope_leave(&g_ss, inner);
        }
        check_sz((size_t)g_norder, 0u, "dtor: inner frame left outer's dtor alone");
        mu_scope_leave(&g_ss, outer);
        check_sz((size_t)g_norder, 1u, "dtor: outer frame released its own");
    }
}

static void test_overflow(void)
{
    mu_scope_init(&g_ss, g_mem, 256);
    check(mu_scope_alloc(&g_ss, 200) != NULL, "overflow: a fitting allocation works");
    check(mu_scope_alloc(&g_ss, 200) == NULL, "overflow: an oversized one is refused");
    check_sz(mu_scope_used(&g_ss), 200u, "overflow: refusal did not move the bump pointer");
    check(mu_scope_overflow_count(&g_ss) == 1u, "overflow: counted");

    mu_scope_reset(&g_ss);
    check_sz(mu_scope_used(&g_ss), 0u, "overflow: reset clears");
}

static void test_strdup_and_alignment(void)
{
    const char *src = "frame-local string";
    char       *copy;
    void       *p32;

    mu_scope_init(&g_ss, g_mem, sizeof g_mem);
    copy = mu_scope_strdup(&g_ss, src);
    check(copy != NULL && strcmp(copy, src) == 0, "strdup: content");
    check(copy != src, "strdup: distinct storage");

    p32 = mu_scope_alloc_aligned(&g_ss, 64, 32);
    check(p32 != NULL && ((uintptr_t)p32 & 31u) == 0u, "aligned: 32-byte request honoured");

    check(mu_scope_alloc_aligned(&g_ss, 64, 24) == NULL, "aligned: non-power-of-two refused");
}

static void test_longjmp(void)
{
    jmp_buf        jb;
    mu_scope_frame base;

    mu_scope_init(&g_ss, g_mem, sizeof g_mem);
    g_norder = 0;
    base = mu_scope_enter(&g_ss);
    mu_scope_register(&g_ss, NULL, dtor_first, NULL);

    if (setjmp(jb) == 0)
    {
        (void)mu_scope_enter(&g_ss);        /* frame that will be skipped */
        (void)mu_scope_alloc(&g_ss, 500);
        mu_scope_register(&g_ss, NULL, dtor_second, NULL);
        longjmp(jb, 1);                      /* skips every cleanup attribute */
    }

    /* the skipped frame's destructor has NOT run: that is what longjmp does */
    check(g_norder == 0, "longjmp: destructors were indeed skipped");
    check(mu_scope_used(&g_ss) > 0u, "longjmp: memory still held");

    mu_scope_purge_to(&g_ss, base);
    check(g_norder == 2, "longjmp: purge ran both destructors");
    check_sz(mu_scope_used(&g_ss), 0u, "longjmp: purge released everything");
}

int main(void)
{
    printf("mu_scope_stack correctness gate\n");
    printf("----------------------------------------------------------\n");
    test_lifo_reuse();
    test_destructors();
    test_overflow();
    test_strdup_and_alignment();
    test_longjmp();
    printf("----------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
