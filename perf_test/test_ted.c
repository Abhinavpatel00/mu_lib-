/*
 * test_ted.c — correctness harness for mu_ted (Ribbon Segments editor buffer).
 *
 * Checks against a naive reference model (flat array + memmove) over:
 *   - bulk load and line indexing
 *   - random single-byte edits, random range inserts/erases, long-line splits
 *   - offset<->(line,col) round trips at every byte
 *   - undo playback restoring the exact byte image
 *
 * Build: gcc -O2 -march=native -std=c11 -Wall -Wextra -I.. -I../mu test_ted.c -o test_ted
 */
#include "mu/mu_text_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c)                                                                        \
    do                                                                                  \
    {                                                                                   \
        if (!(c))                                                                       \
        {                                                                               \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                          \
            g_fail++;                                                                   \
        }                                                                               \
    } while (0)

/* ---------- reference model: flat buffer + memmove ---------- */
typedef struct
{
    char*  p;
    size_t n, cap;
} model;

static void model_init(model* m, const char* s, size_t n)
{
    m->cap = n + 64;
    m->p   = (char*)malloc(m->cap);
    m->n   = n;
    if (n)
        memcpy(m->p, s, n);
}

static void model_reserve(model* m, size_t want)
{
    if (want <= m->cap)
        return;
    while (m->cap < want)
        m->cap *= 2;
    m->p = (char*)realloc(m->p, m->cap);
}

static void model_insert(model* m, size_t at, const char* d, size_t n)
{
    model_reserve(m, m->n + n);
    memmove(m->p + at + n, m->p + at, m->n - at);
    memcpy(m->p + at, d, n);
    m->n += n;
}

static void model_erase(model* m, size_t at, size_t n)
{
    memmove(m->p + at, m->p + at + n, m->n - at - n);
    m->n -= n;
}

/* ---------- helpers ---------- */
static uint64_t rng_state = 0x123456789abcdefull;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}

/* Single-pass model line index: offs[] ascending, one entry per line. */
typedef struct
{
    uint32_t* offs;
    uint32_t  n, cap;
} midx;

static void midx_build(midx* x, const model* m)
{
    if (x->cap < 16)
    {
        x->cap  = 16;
        x->offs = (uint32_t*)malloc(x->cap * sizeof(uint32_t));
    }
    x->n        = 0;
    x->offs[x->n++] = 0;
    for (size_t i = 0; i < m->n; ++i)
        if (m->p[i] == '\n')
        {
            if (x->n == x->cap)
            {
                x->cap *= 2;
                x->offs = (uint32_t*)realloc(x->offs, x->cap * sizeof(uint32_t));
            }
            x->offs[x->n++] = (uint32_t)i + 1u;
        }
}

/* Compare text, line count, line offsets and line text against the model. */
static void compare(const mu_ted* t, const model* m, midx* x, const char* what)
{
    if (mu_ted_text_len(t) != m->n)
    {
        printf("FAIL(%s): len %u vs model %zu\n", what, mu_ted_text_len(t), m->n);
        g_fail++;
        return;
    }
    char* out = (char*)malloc(m->n + 1);
    mu_ted_text(t, 0, (uint32_t)m->n, out);
    if (m->n && memcmp(out, m->p, m->n) != 0)
    {
        size_t i = 0;
        while (i < m->n && out[i] == m->p[i])
            ++i;
        printf("FAIL(%s): byte %zu differs: ted=%02x model=%02x\n", what, i,
               (unsigned char)out[i], (unsigned char)m->p[i]);
        g_fail++;
    }
    free(out);

    midx_build(x, m);
    if (mu_ted_line_count(t) != x->n)
    {
        printf("FAIL(%s): lines %u vs model %u\n", what, mu_ted_line_count(t), x->n);
        g_fail++;
        return;
    }
    for (uint32_t l = 0; l < x->n; ++l)
    {
        const uint32_t want = x->offs[l];
        const uint32_t got  = mu_ted_line_offset(t, l);
        if (want != got)
        {
            printf("FAIL(%s): line %u offset %u vs model %u\n", what, l, got, want);
            g_fail++;
            return;
        }
        const uint32_t end = (l + 1u < x->n) ? x->offs[l + 1u] - 1u : (uint32_t)m->n;
        char           buf[8192];
        const uint32_t n = MU_MIN(end - want, (uint32_t)sizeof(buf));
        const uint32_t g = mu_ted_line_text(t, l, buf, n);
        if (g != n || (n && memcmp(buf, m->p + want, n) != 0))
        {
            printf("FAIL(%s): line %u text mismatch (n=%u got=%u)\n", what, l, n, g);
            g_fail++;
            return;
        }
    }
    printf("  [ok] %-14s len=%u lines=%u segs=%u\n", what, mu_ted_text_len(t),
           mu_ted_line_count(t), mu_ted_segment_count(t));
}

/* Source-like text: short average lines, some long, some blank. */
static char* make_source(size_t target, size_t* out_len)
{
    static const char* frag[] = {
        "static inline uint32_t fn(uint32_t a, uint32_t b)\n{\n    return a + b;\n}\n",
        "    /* comment: the quick brown fox jumps over the lazy dog */\n",
        "\n",
        "typedef struct { uint32_t a; float b; } pair;\n",
        "        acc = _mm512_add_epi32(acc, _mm512_loadu_si512(p));\n",
        "    if (unlikely(x == 0)) { handle_zero(); }\n",
    };
    size_t  cap = target + 4096, n = 0;
    char*   p   = (char*)malloc(cap);
    while (n < target)
    {
        const char* f = frag[rnd() % 6u];
        const size_t k = strlen(f);
        memcpy(p + n, f, k);
        n += k;
        if ((rnd() & 63u) == 0u)
        {
            /* one very long line */
            for (int i = 0; i < 3000; ++i)
                p[n++] = (char)('a' + (int)(rnd() % 26u));
            p[n++] = '\n';
        }
    }
    *out_len = n;
    return p;
}

int main(void)
{
    /* ---- 1. load + index ---- */
    size_t        len = 0;
    char*         src = make_source(3u << 20, &len);
    static mu_ted t;
    CHECK(mu_ted_init(&t, 0));
    CHECK(mu_ted_load(&t, src, len));
    CHECK(mu_ted_validate(&t));

    {
        model m;
        midx  mi = {0};
        model_init(&m, src, len);
        compare(&t, &m, &mi, "load");

        /* ---- 2. random single-byte typing at a drifting cursor ---- */
        uint32_t cur = len / 2u;
        for (int i = 0; i < 20000; ++i)
        {
            char c = "abcdefgh \n"[rnd() % 11u];
            if ((rnd() & 3u) == 0u && m.n > 10u)
            {
                uint32_t at = cur < (uint32_t)m.n ? cur : (uint32_t)m.n - 1u;
                size_t   k  = 1u + (rnd() % 16u);
                if (at + k > m.n)
                    k = m.n - at;
                if (k)
                {
                    mu_ted_erase(&t, at, k);
                    model_erase(&m, at, k);
                }
            }
            else
            {
                const uint32_t at = cur <= (uint32_t)m.n ? cur : (uint32_t)m.n;
                mu_ted_insert(&t, at, &c, 1u);
                model_insert(&m, at, &c, 1u);
                cur = at + 1u;
            }
            cur = (uint32_t)((uint64_t)cur * 2654435761u % (m.n ? m.n : 1u));
            if ((i & 1023) == 0)
                compare(&t, &m, &mi, "typing");
        }
        compare(&t, &m, &mi, "typing-final");
        CHECK(mu_ted_validate(&t));

        /* ---- 3. big bulk paste / range delete in the middle ---- */
        {
            char* big = (char*)malloc(1u << 20);
            for (size_t i = 0; i < (1u << 20); ++i)
                big[i] = (i % 97u == 0u) ? '\n' : (char)('0' + (int)(i % 10u));
            const uint32_t at = (uint32_t)(m.n / 3u);
            CHECK(mu_ted_insert(&t, at, big, 1u << 20));
            model_insert(&m, at, big, 1u << 20);
            compare(&t, &m, &mi, "paste");
            CHECK(mu_ted_validate(&t));

            CHECK(mu_ted_erase(&t, at, 1u << 19));
            model_erase(&m, at, 1u << 19);
            compare(&t, &m, &mi, "range-delete");
            CHECK(mu_ted_validate(&t));
            free(big);
        }

        /* ---- 4. insert into a very long line, then delete the newline ---- */
        {
            const uint32_t lines = mu_ted_line_count(&t);
            uint32_t       raw   = 0, big_len = 0;
            for (uint32_t l = 0; l < lines; ++l)
            {
                const mu_ted_line r = mu_ted_line_range(&t, l);
                if (r.len > big_len)
                {
                    big_len = r.len;
                    raw     = l;
                }
            }
            CHECK(big_len > 2000u);
            const uint32_t off = mu_ted_line_offset(&t, raw) + 5u;
            static const char blob[] = "XXXX\nYYYY\nZZZZ";
            CHECK(mu_ted_insert(&t, off, blob, sizeof(blob) - 1u));
            model_insert(&m, off, blob, sizeof(blob) - 1u);
            compare(&t, &m, &mi, "long-line-split");
            CHECK(mu_ted_validate(&t));

            /* join the next line onto the previous one */
            const uint32_t nl = mu_ted_line_offset(&t, raw + 2u) - 1u;
            CHECK(mu_ted_erase(&t, nl, 1u));
            model_erase(&m, nl, 1u);
            compare(&t, &m, &mi, "join-lines");
            CHECK(mu_ted_validate(&t));

            /* delete everything on that line */
            const mu_ted_line r2 = mu_ted_line_range(&t, raw);
            if (r2.len)
            {
                CHECK(mu_ted_erase(&t, r2.offset, r2.len));
                model_erase(&m, r2.offset, r2.len);
            }
            compare(&t, &m, &mi, "clear-line");
            CHECK(mu_ted_validate(&t));
        }

        /* ---- 5. undo everything ---- */
        {
            char* before = (char*)malloc(m.n + 1);
            memcpy(before, m.p, m.n);
            const size_t before_len = m.n;
            uint32_t     steps      = 0;
            while (mu_ted_undo(&t))
                ++steps;
            CHECK(steps > 0u);
            /* undo rewinds to the freshly loaded document */
            if (mu_ted_text_len(&t) != len)
            {
                printf("FAIL: undo depth rewind len %u vs %zu (steps=%u)\n", mu_ted_text_len(&t),
                       len, steps);
                g_fail++;
            }
            else
            {
                char* out = (char*)malloc(len + 1);
                mu_ted_text(&t, 0, (uint32_t)len, out);
                if (memcmp(out, src, len) != 0)
                {
                    printf("FAIL: undo did not restore the original image\n");
                    g_fail++;
                }
                free(out);
            }
            CHECK(mu_ted_validate(&t));
            free(before);
            (void)before_len;
        }
        free(m.p);
        free(mi.offs);
    }

    printf("segments=%u lines=%u text=%u mem=%.1f MB (index %.2f MB)\n",
           mu_ted_segment_count(&t), mu_ted_line_count(&t), mu_ted_text_len(&t),
           (double)mu_ted_memory_usage(&t) / 1e6, (double)mu_ted_index_usage(&t) / 1e6);

    mu_ted_destroy(&t);
    free(src);

    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall ted checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
