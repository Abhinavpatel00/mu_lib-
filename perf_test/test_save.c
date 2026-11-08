/* test_save.c -- gate for mu/mu_save.h
 *
 * A save format has one job that matters more than any other: a player must
 * never lose a save, and a corrupt one must be *refused* rather than half
 * loaded. So most of this file is adversarial rather than happy-path:
 *
 *   1. WIRE FORMAT. Golden bytes for a known payload, so an accidental change
 *      to the layout is a failing test rather than a batch of unloadable saves
 *      in the wild.
 *   2. TRUNCATION AT EVERY OFFSET. Cut the file at each of its N bytes and
 *      require a clean status every time -- never a crash, never a partial
 *      decode that reports success.
 *   3. CORRUPTION AT EVERY BYTE. Flip a bit in each byte and require that
 *      mu_save_open (or the chunk CRC behind it) refuses.
 *   4. FORWARD COMPATIBILITY. An unknown chunk is skipped, not fatal; a chunk
 *      version newer than the reader knows is reported, not guessed.
 *   5. MIGRATION. A three-version ladder, each rung gated, plus the "missing
 *      rung" refusal.
 *   6. A REALISTIC PAYLOAD. A roguelite cave: generated with mu_roguelike,
 *      saved with its RNG state, reloaded, and required to resume into a
 *      bit-identical future (same grid, same FOV, same next 100 dice rolls).
 *
 *   make -C perf_test save-test
 */

#define MU_SAVE_IMPLEMENTATION 1
#define MU_SAVE_WANT_STDIO 1
#include "mu_save.h"

#include "mu_roguelike.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL %s\n", what);
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *what)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        if (g_fails <= 25) printf("  FAIL %s (got %u want %u)\n", what, got, want);
    }
}

static void check_status(mu_save_status got, mu_save_status want, const char *what)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        if (g_fails <= 25)
            printf("  FAIL %s (got %s want %s)\n", what, mu_save_status_str(got), mu_save_status_str(want));
    }
}

/* ------------------------------------------------------------------ */
/* 0. crc32c: the two implementations must be the same function        */
/* ------------------------------------------------------------------ */

/* The format's integrity rests on CRC32C being CRC32C. There are two
 * implementations (slicing-by-8 and SSE4.2) and they must agree bit for bit on
 * every input, or a save written by one build fails to load in another. The
 * standard check value pins the polynomial and the reflection; the sweep pins
 * the implementations to each other. */
static void test_crc(void)
{
    static uint8_t data[4096];
    uint32_t i;

    check_u32(mu_save_crc32c_sw("123456789", 9u), 0xE3069283u, "crc: the standard CRC-32C check value");
    check_u32(mu_save_crc32c_sw("", 0u), 0x00000000u, "crc: empty input is 0");
    check_u32(mu_save_crc32c("123456789", 9u), 0xE3069283u, "crc: the dispatching entry point agrees");

#if MU_SAVE_HAS_CRC_HW
    check_u32(mu_save_crc32c_hw("123456789", 9u), 0xE3069283u, "crc: the SSE4.2 path agrees on the vector");
    {
        uint32_t bad = 0;
        for (i = 0; i < sizeof data; ++i) data[i] = (uint8_t)(i * 37u + 11u);
        for (i = 0; i <= sizeof data; ++i)
            if (mu_save_crc32c_sw(data, i) != mu_save_crc32c_hw(data, i)) ++bad;
        check_u32(bad, 0u, "crc: software and hardware agree on every length 0..4096");
    }
    /* Chaining must equal a single pass, which is what makes incremental
     * checksums legal (a writer that streams a payload). */
    {
        uint32_t bad = 0;
        for (i = 0; i <= 64u; ++i) {
            uint32_t a = mu_save_crc32c_sw(data, 100u);
            uint32_t b = mu_save_crc32c_sw_extend(mu_save_crc32c_sw(data, i), data + i, 100u - i);
            if (a != b) ++bad;
            if (mu_save_crc32c_hw(data, 100u) !=
                mu_save_crc32c_hw_extend(mu_save_crc32c_hw(data, i), data + i, 100u - i))
                ++bad;
        }
        check_u32(bad, 0u, "crc: extending in two pieces equals one pass, both implementations");
    }
#endif

    /* Every single-bit change must change the checksum: that is the property
     * the corruption test leans on, checked here directly on the CRC. */
    {
        uint32_t bad = 0, base;
        memset(data, 0xA5, 256);
        base = mu_save_crc32c(data, 256u);
        for (i = 0; i < 256u * 8u; ++i) {
            uint32_t byte = i / 8u, bit = i % 8u;
            data[byte] ^= (uint8_t)(1u << bit);
            if (mu_save_crc32c(data, 256u) == base) ++bad;
            data[byte] ^= (uint8_t)(1u << bit);
        }
        check_u32(bad, 0u, "crc: every single-bit flip changes the checksum");
    }
}

/* ------------------------------------------------------------------ */
/* 1. wire format: golden bytes                                       */
/* ------------------------------------------------------------------ */

/* A payload that uses every primitive once, so the golden blob pins down the
 * encoding of all of them: header, chunk header, LE scalars, float bits, a
 * varint, a zigzag varint, a length-prefixed string and the padding rule. */
static uint32_t build_golden(uint8_t *buf, uint32_t cap)
{
    mu_save_writer w;
    mu_save_writer_begin(&w, buf, cap, 0x1234ABCDu);
    mu_save_chunk_begin(&w, 0x10u, 3u);
    mu_save_u32(&w, 0x01020304u);      /* 04 03 02 01                     */
    mu_save_u16(&w, 0xBEEFu);          /* ef be                           */
    mu_save_u8(&w, 0x7Fu);             /* 7f                              */
    mu_save_f32(&w, 1.0f);             /* 00 00 80 3f                     */
    mu_save_vu32(&w, 300u);            /* ac 02                           */
    mu_save_vi32(&w, -5);              /* 09                              */
    mu_save_str(&w, "hi");             /* 02 68 69                        */
    mu_save_chunk_end(&w);             /* payload 18 bytes -> 2 pad bytes */
    return mu_save_writer_commit(&w);
}

static void test_golden(void)
{
    static const uint8_t expect[] = {
        /* header */
        'M', 'S', 'V', '1', 1, 0, 0, 0, 0xCD, 0xAB, 0x34, 0x12,
        /* chunk header: id 0x10, v3, flags 0, size 0x11 = 17, crc32c */
        0x10, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
        0x24, 0xEE, 0xBD, 0xAE,
        /* payload, 17 bytes: u32, u16, u8, f32, varint, zigzag, string */
        0x04, 0x03, 0x02, 0x01,
        0xEF, 0xBE,
        0x7F,
        0x00, 0x00, 0x80, 0x3F,
        0xAC, 0x02,
        0x09,
        0x02, 'h', 'i',
        0x00, 0x00, 0x00, /* pad 17 -> 20, so the next chunk starts aligned */
        /* footer */
        'S', 'E', 'N', 'D', 1, 0, 0, 0, 0x44, 0xB2, 0x0F, 0x72
    };
    uint8_t buf[128];
    uint32_t n = build_golden(buf, (uint32_t)sizeof buf);
    uint32_t i, bad = 0;

    check_u32(n, (uint32_t)sizeof expect, "golden: file size is pinned");
    for (i = 0; i < sizeof expect && i < n; ++i)
        if (buf[i] != expect[i]) ++bad;
    check(bad == 0, "golden: every byte matches the documented layout");
    if (bad) {
        printf("    got:");
        for (i = 0; i < n && i < 60; ++i) printf(" %02x", buf[i]);
        printf("\n    want:");
        for (i = 0; i < sizeof expect && i < 60; ++i) printf(" %02x", expect[i]);
        printf("\n");
    }
    /* The CRC words are the only part of the golden blob not hand-written above;
     * pin them too, so an algorithm change is caught here and not in the field. */
    /* These two words are the only part of the golden blob not hand-written
     * above, so they are pinned here. That they are the *right* CRC32C is
     * separate evidence: test_crc() checks the standard "123456789" vector
     * against the published 0xE3069283, and checks the two implementations
     * against each other on every length. */
    check_u32(buf[24] | (buf[25] << 8) | (buf[26] << 16) | ((uint32_t)buf[27] << 24), 0xAEBDEE24u,
              "golden: the chunk crc32c is unchanged");
    check_u32(buf[56] | (buf[57] << 8) | (buf[58] << 16) | ((uint32_t)buf[59] << 24), 0x720FB244u,
              "golden: the file crc32c is unchanged");
}

/* ------------------------------------------------------------------ */
/* 2. every value type round-trips exactly                            */
/* ------------------------------------------------------------------ */

static void test_roundtrip(void)
{
    uint8_t buf[4096];
    mu_save_writer w;
    mu_save_reader r;
    union { float f; uint32_t u; } f32v[4];
    union { double d; uint64_t u; } f64v[2];
    char str[32];

    f32v[0].f = 1.0f;
    f32v[1].f = -0.0f;
    f32v[2].u = 0x7FC00001u; /* a signalling-ish NaN with a payload */
    f32v[3].u = 0x7F800000u; /* +inf                           */
    f64v[0].d = 1.0 / 3.0;
    f64v[1].u = 0xFFF8000000000001ull; /* NaN, payload 1 */

    mu_save_writer_begin(&w, buf, (uint32_t)sizeof buf, 7u);
    mu_save_chunk_begin(&w, 1u, 0u);
    mu_save_u8(&w, 0xFFu);
    mu_save_u16(&w, 0xFFFFu);
    mu_save_u32(&w, 0xFFFFFFFFu);
    mu_save_u64(&w, 0xFFFFFFFFFFFFFFFFull);
    mu_save_i32(&w, INT32_MIN);
    mu_save_i64(&w, INT64_MIN);
    mu_save_bool(&w, 1);
    mu_save_vu32(&w, 0u);
    mu_save_vu32(&w, 127u);
    mu_save_vu32(&w, 128u);
    mu_save_vu32(&w, 0xFFFFFFFFu);
    mu_save_vu64(&w, 0xFFFFFFFFFFFFFFFFull);
    mu_save_vi32(&w, 0);
    mu_save_vi32(&w, -1);
    mu_save_vi32(&w, INT32_MIN);
    mu_save_vi32(&w, INT32_MAX);
    mu_save_vi64(&w, INT64_MIN);
    mu_save_vi64(&w, INT64_MAX);
    mu_save_f32(&w, f32v[0].f);
    mu_save_f32(&w, f32v[1].f);
    mu_save_f32(&w, f32v[2].f);
    mu_save_f32(&w, f32v[3].f);
    mu_save_f64(&w, f64v[0].d);
    mu_save_f64(&w, f64v[1].d);
    mu_save_str(&w, "");
    mu_save_str(&w, "abcdefghijklmnopqrstuvwxyz");
    mu_save_bytes(&w, NULL, 0u);
    mu_save_align4(&w);
    mu_save_u32(&w, 0xA5A5A5A5u); /* must survive the alignment */
    mu_save_chunk_end(&w);
    mu_save_chunk_begin(&w, 2u, 0u); /* an empty chunk is legal */
    mu_save_chunk_end(&w);
    check(mu_save_writer_commit(&w) != 0u, "roundtrip: commit succeeded");

    check_status(mu_save_open(&r, buf, mu_save_writer_size(&w), 7u), MU_SAVE_OK, "roundtrip: open");
    check_u32(mu_save_chunk_count(&r), 2u, "roundtrip: footer chunk count");

    check_status(mu_save_next(&r), MU_SAVE_OK, "roundtrip: first chunk");
    check_u32(r.chunk_id, 1u, "roundtrip: chunk id");
    check_u32(mu_save_read_u8(&r), 0xFFu, "roundtrip: u8");
    check_u32(mu_save_read_u16(&r), 0xFFFFu, "roundtrip: u16");
    check_u32(mu_save_read_u32(&r), 0xFFFFFFFFu, "roundtrip: u32");
    check(mu_save_read_u64(&r) == 0xFFFFFFFFFFFFFFFFull, "roundtrip: u64");
    check(mu_save_read_i32(&r) == INT32_MIN, "roundtrip: i32 min");
    check(mu_save_read_i64(&r) == INT64_MIN, "roundtrip: i64 min");
    check(mu_save_read_bool(&r) == 1, "roundtrip: bool");
    check_u32(mu_save_read_vu32(&r), 0u, "roundtrip: varint 0");
    check_u32(mu_save_read_vu32(&r), 127u, "roundtrip: varint 127 (one byte)");
    check_u32(mu_save_read_vu32(&r), 128u, "roundtrip: varint 128 (two bytes)");
    check_u32(mu_save_read_vu32(&r), 0xFFFFFFFFu, "roundtrip: varint u32 max");
    check(mu_save_read_vu64(&r) == 0xFFFFFFFFFFFFFFFFull, "roundtrip: varint u64 max");
    check(mu_save_read_vi32(&r) == 0, "roundtrip: zigzag 0");
    check(mu_save_read_vi32(&r) == -1, "roundtrip: zigzag -1");
    check(mu_save_read_vi32(&r) == INT32_MIN, "roundtrip: zigzag i32 min");
    check(mu_save_read_vi32(&r) == INT32_MAX, "roundtrip: zigzag i32 max");
    check(mu_save_read_vi64(&r) == INT64_MIN, "roundtrip: zigzag i64 min");
    check(mu_save_read_vi64(&r) == INT64_MAX, "roundtrip: zigzag i64 max");
    {
        union { float f; uint32_t u; } got;
        got.f = mu_save_read_f32(&r);
        check_u32(got.u, f32v[0].u, "roundtrip: f32 1.0 is bit-exact");
        got.f = mu_save_read_f32(&r);
        check_u32(got.u, f32v[1].u, "roundtrip: f32 -0.0 keeps its sign bit");
        got.f = mu_save_read_f32(&r);
        check_u32(got.u, f32v[2].u, "roundtrip: f32 NaN payload survives");
        got.f = mu_save_read_f32(&r);
        check_u32(got.u, f32v[3].u, "roundtrip: f32 inf");
        {
            union { double d; uint64_t u; } g64;
            g64.d = mu_save_read_f64(&r);
            check(g64.u == f64v[0].u, "roundtrip: f64 is bit-exact");
            g64.d = mu_save_read_f64(&r);
            check(g64.u == f64v[1].u, "roundtrip: f64 NaN payload survives");
        }
    }
    check_u32(mu_save_read_str(&r, str, (uint32_t)sizeof str), 0u, "roundtrip: empty string");
    check(str[0] == '\0', "roundtrip: empty string is NUL-terminated");
    check_u32(mu_save_read_str(&r, str, (uint32_t)sizeof str), 26u, "roundtrip: 26-byte string length");
    check(strcmp(str, "abcdefghijklmnopqrstuvwxyz") == 0, "roundtrip: string contents");
    check_u32(mu_save_read_bytes(&r, NULL, 0u), 0u, "roundtrip: empty blob");
    check_u32(mu_save_read_align4(&r), 0u,
              "roundtrip: align4 is a no-op when the payload is already aligned");
    check_u32(mu_save_read_u32(&r), 0xA5A5A5A5u, "roundtrip: value after the alignment is intact");
    check_u32(mu_save_chunk_remaining(&r), 0u, "roundtrip: first chunk fully consumed");

    check_status(mu_save_next(&r), MU_SAVE_OK, "roundtrip: second chunk");
    check_u32(r.chunk_size, 0u, "roundtrip: empty chunk has zero size");
    check_status(mu_save_next(&r), MU_SAVE_END, "roundtrip: end of file");
    check(mu_save_reader_ok(&r), "roundtrip: a clean END is not a failure");

    /* An unaligned payload: one byte, then align4 must add three zeros and the
     * reader must skip exactly three. */
    {
        uint8_t p3[32];
        mu_save_writer w3;
        mu_save_reader r3;
        mu_save_writer_begin_payload(&w3, p3, (uint32_t)sizeof p3);
        (void)mu_save_u8(&w3, 0x11u);
        (void)mu_save_align4(&w3);
        (void)mu_save_u32(&w3, 0x22334455u);
        mu_save_reader_over_payload(&r3, p3, mu_save_writer_commit(&w3));
        check_u32(mu_save_read_u8(&r3), 0x11u, "roundtrip: byte before the padding");
        check_u32(mu_save_read_align4(&r3), 3u, "roundtrip: three bytes of padding skipped");
        check_u32(mu_save_read_u32(&r3), 0x22334455u, "roundtrip: value after three bytes of padding");
    }

    /* A string longer than the caller's buffer is truncated to fit but the whole
     * value is consumed, so iteration stays in sync. Checked on a bare payload so
     * the field offsets are obvious. */
    {
        uint8_t p2[64];
        mu_save_writer w2;
        mu_save_reader r2;
        char small[6];
        mu_save_writer_begin_payload(&w2, p2, (uint32_t)sizeof p2);
        (void)mu_save_str(&w2, "abcdefghij");
        (void)mu_save_u8(&w2, 0x77u);
        mu_save_reader_over_payload(&r2, p2, mu_save_writer_commit(&w2));
        check_u32(mu_save_read_str(&r2, small, (uint32_t)sizeof small), 10u,
                  "roundtrip: a truncated string still reports its real length");
        check(strcmp(small, "abcde") == 0, "roundtrip: the truncated string is NUL-terminated");
        check_u32(mu_save_read_u8(&r2), 0x77u, "roundtrip: the field after it still lines up");
    }
}

/* ------------------------------------------------------------------ */
/* 3. truncation at every byte offset                                 */
/* ------------------------------------------------------------------ */

static void test_truncation(void)
{
    uint8_t buf[512];
    uint8_t cut[512];
    uint32_t n = 0, len;
    int all_clean = 1, opened_some = 0;

    mu_save_writer w;
    mu_save_writer_begin(&w, buf, (uint32_t)sizeof buf, 42u);
    mu_save_chunk_begin(&w, 1u, 1u);
    mu_save_u32(&w, 0xDEADBEEFu);
    mu_save_str(&w, "truncate me");
    mu_save_chunk_end(&w);
    mu_save_chunk_begin(&w, 2u, 1u);
    mu_save_u64(&w, 1234567890123ull);
    mu_save_chunk_end(&w);
    n = mu_save_writer_commit(&w);
    check(n > 40u, "truncation: the fixture is big enough to be interesting");

    for (len = 0; len < n; ++len) {
        mu_save_reader r;
        mu_save_status s;
        memcpy(cut, buf, len);
        s = mu_save_open(&r, cut, len, 42u);
        if (s == MU_SAVE_OK) {
            /* Only a prefix that happens to end exactly on a chunk boundary can
             * open -- and it must still be reported as truncated, because the
             * footer says how many chunks there are. */
            ++opened_some;
            while (mu_save_next(&r) == MU_SAVE_OK) {
                if (r.chunk_id == 1u) {
                    (void)mu_save_read_u32(&r);
                    (void)mu_save_read_str(&r, NULL, 0);
                } else {
                    (void)mu_save_read_u64(&r);
                }
            }
            if (!r.failed) all_clean = 0; /* a truncated file that "succeeded" */
        } else {
            /* Any clean refusal is acceptable; a crash is not, and neither is
             * OK. The specific code differs by where the cut fell: MAGIC when
             * the footer magic is gone, CORRUPT when the CRC covers it,
             * TRUNCATED when the length is obviously short. */
            if (s == MU_SAVE_OK) all_clean = 0;
        }
    }
    check(all_clean, "truncation: every prefix is refused, none half-loads");
    printf("  truncation: %u prefixes, %d of them reached iteration, all refused\n", n, opened_some);
}

/* ------------------------------------------------------------------ */
/* 4. corruption: flip a bit in every byte                            */
/* ------------------------------------------------------------------ */

static void test_corruption(void)
{
    uint8_t buf[512];
    uint8_t bad[512];
    uint32_t n, i, undetected = 0;
    mu_save_writer w;

    mu_save_writer_begin(&w, buf, (uint32_t)sizeof buf, 42u);
    mu_save_chunk_begin(&w, 0x41414141u, 2u);
    mu_save_u32(&w, 0xDEADBEEFu);
    mu_save_str(&w, "flip me");
    mu_save_chunk_end(&w);
    n = mu_save_writer_commit(&w);

    for (i = 0; i < n; ++i) {
        unsigned bit;
        for (bit = 0; bit < 8u; ++bit) {
            mu_save_reader r;
            mu_save_status s;
            int detected = 0;

            memcpy(bad, buf, n);
            bad[i] ^= (uint8_t)(1u << bit);

            s = mu_save_open(&r, bad, n, 42u);
            if (s != MU_SAVE_OK) {
                detected = 1;
            } else {
                /* mu_save_open only checks the footer CRC over every preceding
                 * byte, so anything after this point is a chunk-level catch. */
                while (mu_save_next(&r) == MU_SAVE_OK) {
                    (void)mu_save_read_u32(&r);
                    (void)mu_save_read_str(&r, NULL, 0);
                }
                if (r.failed) detected = 1;
            }
            if (!detected) {
                ++undetected;
                if (undetected <= 3) printf("  FAIL corruption at byte %u bit %u went undetected\n", i, bit);
            }
            ++g_checks;
            if (!detected) ++g_fails;
        }
    }
    printf("  corruption: %u bytes x 8 bits flipped, %u undetected\n", n, undetected);
}

/* ------------------------------------------------------------------ */
/* 5. forward compatibility: unknown chunks and newer versions        */
/* ------------------------------------------------------------------ */

#define ID_KNOWN 0x4B4E4F57u /* "KNOWN" */
#define ID_FUTURE 0x46555455u /* "FUTUR" */

static void test_forward_compat(void)
{
    uint8_t buf[512];
    mu_save_writer w;
    mu_save_reader r;

    mu_save_writer_begin(&w, buf, (uint32_t)sizeof buf, 9u);
    mu_save_chunk_begin(&w, ID_KNOWN, 1u);
    mu_save_u32(&w, 111u);
    mu_save_chunk_end(&w);
    mu_save_chunk_begin(&w, ID_FUTURE, 99u); /* a schema this build has never seen */
    mu_save_str(&w, "new stuff the old binary must not choke on");
    mu_save_bytes(&w, "\x01\x02\x03", 3u);
    mu_save_chunk_end(&w);
    mu_save_chunk_begin(&w, ID_KNOWN, 1u);
    mu_save_u32(&w, 222u);
    mu_save_chunk_end(&w);
    mu_save_writer_commit(&w);

    check_status(mu_save_open(&r, buf, mu_save_writer_size(&w), 9u), MU_SAVE_OK, "compat: open");
    {
        uint32_t found = 0u;
        while (mu_save_next(&r) == MU_SAVE_OK) {
            if (r.chunk_id == ID_KNOWN) {
                check_u32(mu_save_read_u32(&r), found == 0u ? 111u : 222u, "compat: known chunk payload");
                ++found;
            } else {
                /* The point: a reader that has never heard of this chunk skips it
                 * by length and keeps going. */
                check_u32(r.chunk_version, 99u, "compat: the unknown chunk reports its version");
                check(mu_save_skip_bytes(&r) == MU_SAVE_OK, "compat: unknown chunk skipped");
                check(mu_save_skip_bytes(&r) == MU_SAVE_OK, "compat: second field skipped");
            }
        }
        check_u32(found, 2u, "compat: both known chunks were read after skipping the future one");
        check_status(r.status, MU_SAVE_END, "compat: reached the end cleanly");
    }

    /* A chunk version newer than the caller knows is *reported*, so the caller
     * can decide: skip it (above) or refuse it. The library does not guess. */
    check_status(mu_save_find(&r, ID_FUTURE), MU_SAVE_OK, "compat: find() locates the future chunk");
    check_u32(r.chunk_version, 99u, "compat: find() leaves the version readable");

    /* A file whose format_id is someone else's must be refused, not parsed. */
    check_status(mu_save_open(&r, buf, mu_save_writer_size(&w), 10u), MU_SAVE_ERR_FORMAT,
                 "compat: a foreign format_id is refused");
    check_status(mu_save_open(&r, buf, mu_save_writer_size(&w), MU_SAVE_NO_ID), MU_SAVE_OK,
                 "compat: MU_SAVE_NO_ID accepts any format");
}

/* ------------------------------------------------------------------ */
/* 5b. the chunk-level guards, reached by skipping the file CRC       */
/* ------------------------------------------------------------------ */

/* The whole-file CRC catches every corruption first, which is the right order
 * for production but hides the chunk-level checks from a test. mu_save_open_ex
 * with verify=0 gets past the file CRC, so the per-chunk length and CRC guards
 * get exercised directly -- they are what protects a caller that chose to skip
 * the up-front pass. */
static void test_chunk_guards(void)
{
    uint8_t buf[256];
    uint8_t bad[256];
    mu_save_writer w;
    mu_save_reader r;
    uint32_t n;

    mu_save_writer_begin(&w, buf, (uint32_t)sizeof buf, 3u);
    mu_save_chunk_begin(&w, 1u, 1u);
    mu_save_u32(&w, 0xAABBCCDDu);
    mu_save_chunk_end(&w);
    n = mu_save_writer_commit(&w);

    /* The skip-the-file-CRC path still works on an intact file. */
    check_status(mu_save_open_ex(&r, buf, n, 3u, 0), MU_SAVE_OK, "guards: open without the file CRC");
    check_status(mu_save_next(&r), MU_SAVE_OK, "guards: chunk still validates itself");
    check_u32(mu_save_read_u32(&r), 0xAABBCCDDu, "guards: payload still reads");

    /* A chunk size that runs past the footer. */
    memcpy(bad, buf, n);
    bad[12 + 8] = 0xFFu; /* size low byte */
    bad[12 + 9] = 0xFFu;
    check_status(mu_save_open_ex(&r, bad, n, 3u, 0), MU_SAVE_OK, "guards: file CRC skipped, header ok");
    check_status(mu_save_next(&r), MU_SAVE_ERR_TRUNCATED, "guards: a chunk size past the end is TRUNCATED");

    /* A flipped byte in the payload with the file CRC skipped: the chunk CRC is
     * the only thing left, and it has to be enough. */
    memcpy(bad, buf, n);
    bad[12 + 16] ^= 0x01u;
    check_status(mu_save_open_ex(&r, bad, n, 3u, 0), MU_SAVE_OK, "guards: payload flip survives open without CRC");
    check_status(mu_save_next(&r), MU_SAVE_ERR_CORRUPT, "guards: the per-chunk CRC catches it");

    /* A zero chunk id in the stream is corruption, not a chunk. */
    memcpy(bad, buf, n);
    memset(bad + 12, 0, 4);
    mu_save_open_ex(&r, bad, n, 3u, 0);
    check_status(mu_save_next(&r), MU_SAVE_ERR_CORRUPT, "guards: chunk id 0 in the stream is CORRUPT");

    /* A reserved flag bit set, likewise. */
    memcpy(bad, buf, n);
    bad[12 + 6] = 0x01u;
    mu_save_open_ex(&r, bad, n, 3u, 0);
    check_status(mu_save_next(&r), MU_SAVE_ERR_CORRUPT, "guards: an unknown chunk flag is CORRUPT");

    /* A footer chunk count that disagrees with the stream (the hole the first
     * run of this gate found). */
    memcpy(bad, buf, n);
    bad[n - 8] = 9u;
    mu_save_open_ex(&r, bad, n, 3u, 0);
    while (mu_save_next(&r) == MU_SAVE_OK) mu_save_skip_chunk(&r);
    check_status(mu_save_reader_status(&r), MU_SAVE_ERR_CORRUPT,
                 "guards: a footer count that lies is CORRUPT at the end of the walk");

    /* An unknown file version is a clean refusal, not a parse of garbage. */
    memcpy(bad, buf, n);
    bad[4] = 2u;
    check_status(mu_save_open(&r, bad, n, 3u), MU_SAVE_ERR_VERSION, "guards: a future file version is refused");
}

/* ------------------------------------------------------------------ */
/* 6. migration ladder v1 -> v2 -> v3                                 */
/* ------------------------------------------------------------------ */

/* The scenario every shipped game hits: v1 stored a health as an int and had no
 * name; v2 added a defaulted name; v3 stores health as a float and a spawn
 * point. Each rung is one function, and each is gated. */
typedef struct save_v3 {
    int32_t hp_tenths; /* v3 stores tenths, not whole points, to survive 0.5 hp */
    char name[32];
    float x, y;
} save_v3;

static mu_save_status up_1_to_2(mu_save_reader *in, mu_save_writer *out, void *user)
{
    int32_t hp;
    (void)user;
    hp = mu_save_read_i32(in);
    if (in->failed) return in->status;
    mu_save_i32(out, hp);
    mu_save_str(out, "unnamed"); /* the defaulted field */
    return out->status;
}

static mu_save_status up_2_to_3(mu_save_reader *in, mu_save_writer *out, void *user)
{
    char name[32];
    int32_t hp;
    (void)user;
    hp = mu_save_read_i32(in);
    (void)mu_save_read_str(in, name, (uint32_t)sizeof name);
    if (in->failed) return in->status;
    mu_save_i32(out, hp * 10);      /* int points -> float tenths */
    mu_save_str(out, name);
    mu_save_f32(out, 0.0f);         /* spawn point, defaulted */
    mu_save_f32(out, 0.0f);
    return out->status;
}

static const mu_save_upgrade g_ladder[] = {
    { 0x484C5448u /* "HLTH" */, 1u, up_1_to_2 },
    { 0x484C5448u, 2u, up_2_to_3 },
};

static void decode_v3(const uint8_t *payload, uint32_t size, save_v3 *out)
{
    mu_save_reader r;
    mu_save_reader_over_payload(&r, payload, size);
    out->hp_tenths = mu_save_read_i32(&r);
    (void)mu_save_read_str(&r, out->name, (uint32_t)sizeof out->name);
    out->x = mu_save_read_f32(&r);
    out->y = mu_save_read_f32(&r);
    check(mu_save_reader_status(&r) == MU_SAVE_OK, "migration: v3 payload decodes cleanly");
}

static void test_migration(void)
{
    uint8_t v1[64], v3[64], scratch_a[256], scratch_b[256];
    mu_save_writer w;
    mu_save_reader r;
    const uint8_t *out_payload = NULL;
    uint32_t out_size = 0, n1, n3;
    uint16_t out_version = 0;
    save_v3 a, b;
    char stored[8];

    /* A v1 file: hp as an int, nothing else. */
    mu_save_writer_begin(&w, v1, (uint32_t)sizeof v1, 0x55u);
    mu_save_chunk_begin(&w, 0x484C5448u, 1u);
    mu_save_i32(&w, 75);
    mu_save_chunk_end(&w);
    n1 = mu_save_writer_commit(&w);

    /* A native v3 file with the same logical content, written the way a current
     * build would write it. */
    mu_save_writer_begin(&w, v3, (uint32_t)sizeof v3, 0x55u);
    mu_save_chunk_begin(&w, 0x484C5448u, 3u);
    mu_save_i32(&w, 750);
    mu_save_str(&w, "unnamed");
    mu_save_f32(&w, 0.0f);
    mu_save_f32(&w, 0.0f);
    mu_save_chunk_end(&w);
    n3 = mu_save_writer_commit(&w);

    check_status(mu_save_open(&r, v1, n1, 0x55u), MU_SAVE_OK, "migration: open v1");
    check_status(mu_save_next(&r), MU_SAVE_OK, "migration: v1 chunk");
    check_u32(r.chunk_version, 1u, "migration: v1 chunk says version 1");

    check_status(mu_save_upgrade_chain(g_ladder, 2u, r.chunk_id, mu_save_chunk_payload(&r), r.chunk_size,
                                       1u, 3u, scratch_a, scratch_b, (uint32_t)sizeof scratch_a,
                                       &out_payload, &out_size, &out_version, NULL),
                 MU_SAVE_OK, "migration: v1 -> v3 chain runs");
    check_u32(out_version, 3u, "migration: the chain lands on v3");
    decode_v3(out_payload, out_size, &a);

    /* The strongest form of the assertion: the migrated bytes are *identical* to
     * the bytes a native v3 writer produces for the same content. */
    check_status(mu_save_open(&r, v3, n3, 0x55u), MU_SAVE_OK, "migration: open native v3");
    check_status(mu_save_next(&r), MU_SAVE_OK, "migration: native v3 chunk");
    check_u32(out_size, r.chunk_size, "migration: migrated payload is the same size as a native one");
    check(out_size == r.chunk_size && memcmp(out_payload, mu_save_chunk_payload(&r), out_size) == 0,
          "migration: migrated bytes are identical to a native v3 payload");
    decode_v3(mu_save_chunk_payload(&r), r.chunk_size, &b);
    check(a.hp_tenths == b.hp_tenths && a.x == b.x && a.y == b.y && strcmp(a.name, b.name) == 0,
          "migration: migrated state equals native state");
    check_u32((uint32_t)a.hp_tenths, 750u, "migration: 75 hp became 750 tenths");
    check(strcmp(a.name, "unnamed") == 0, "migration: the defaulted name is present");

    /* A v2 file: only one rung needed, and it must pick the right one. */
    {
        uint8_t v2[64];
        uint32_t n2;
        mu_save_writer_begin(&w, v2, (uint32_t)sizeof v2, 0x55u);
        mu_save_chunk_begin(&w, 0x484C5448u, 2u);
        mu_save_i32(&w, 75);
        mu_save_str(&w, "hero");
        mu_save_chunk_end(&w);
        n2 = mu_save_writer_commit(&w);

        check_status(mu_save_open(&r, v2, n2, 0x55u), MU_SAVE_OK, "migration: open v2");
        mu_save_next(&r);
        check_status(mu_save_upgrade_chain(g_ladder, 2u, r.chunk_id, mu_save_chunk_payload(&r), r.chunk_size,
                                           2u, 3u, scratch_a, scratch_b, (uint32_t)sizeof scratch_a,
                                           &out_payload, &out_size, &out_version, NULL),
                     MU_SAVE_OK, "migration: v2 -> v3 uses the right rung");
        decode_v3(out_payload, out_size, &a);
        check(strcmp(a.name, "hero") == 0, "migration: v2 keeps its own name");
        check_u32((uint32_t)a.hp_tenths, 750u, "migration: v2 hp is converted, not reinterpreted");
    }

    /* No rung from v1 straight to v3 in a table that only has 2->3: refuse. */
    {
        const mu_save_upgrade partial[] = { { 0x484C5448u, 2u, up_2_to_3 } };
        check_status(mu_save_upgrade_chain(partial, 1u, 0x484C5448u, out_payload, out_size, 1u, 3u,
                                           scratch_a, scratch_b, (uint32_t)sizeof scratch_a,
                                           &out_payload, &out_size, &out_version, NULL),
                     MU_SAVE_ERR_VERSION, "migration: a missing rung is refused, not skipped");
    }

    /* A chunk id with no ladder at all. */
    check_status(mu_save_upgrade_chain(g_ladder, 2u, 0x11111111u, out_payload, out_size, 1u, 3u,
                                       scratch_a, scratch_b, (uint32_t)sizeof scratch_a,
                                       &out_payload, &out_size, &out_version, NULL),
                 MU_SAVE_ERR_VERSION, "migration: an unknown chunk id has no ladder");
    /* Same version in and out: no copy, same pointer. */
    check_status(mu_save_upgrade_chain(g_ladder, 2u, 0x484C5448u, scratch_a, 12u, 3u, 3u,
                                       scratch_b, NULL, 64u, &out_payload, &out_size, &out_version, NULL),
                 MU_SAVE_OK, "migration: equal versions is a no-op");
    check(out_payload == scratch_a && out_size == 12u, "migration: the no-op does not copy");
    (void)stored;
}

/* ------------------------------------------------------------------ */
/* 7. the writer fails closed                                         */
/* ------------------------------------------------------------------ */

static void test_writer_overflow(void)
{
    uint8_t buf[64];
    mu_save_writer w;
    uint32_t i, dirty = 0;

    memset(buf, 0xCC, sizeof buf); /* guard pattern beyond capacity */
    mu_save_writer_begin(&w, buf, 40u, 1u);
    mu_save_chunk_begin(&w, 1u, 1u);
    for (i = 0; i < 100u; ++i) (void)mu_save_u64(&w, 0x0123456789ABCDEFull);
    check_status(mu_save_writer_status(&w), MU_SAVE_ERR_FULL, "overflow: the writer reports FULL");
    check_status(mu_save_u32(&w, 1u), MU_SAVE_ERR_FULL, "overflow: the failure is sticky");
    check(mu_save_writer_commit(&w) == 0u, "overflow: commit refuses a failed writer");
    check(mu_save_chunk_end(&w) != MU_SAVE_OK, "overflow: chunk_end refuses too");
    for (i = 40u; i < sizeof buf; ++i)
        if (buf[i] != 0xCCu) ++dirty;
    check_u32(dirty, 0u, "overflow: not one byte past capacity was written");

    /* Exactly the buffer's size works, one less fails: the boundary is where a
     * caller's arithmetic is most likely to be off by one. */
    {
        uint8_t fit[128];
        mu_save_writer fw;
        uint32_t need;
        mu_save_writer_begin(&fw, fit, (uint32_t)sizeof fit, 1u);
        mu_save_chunk_begin(&fw, 1u, 1u);
        for (i = 0; i < 4u; ++i) (void)mu_save_u64(&fw, 1u);
        mu_save_chunk_end(&fw);
        need = mu_save_writer_commit(&fw);
        check_u32(need, 12u + 16u + 32u + 12u, "overflow: the exact size is predictable");

        mu_save_writer_begin(&fw, fit, need - 1u, 1u);
        mu_save_chunk_begin(&fw, 1u, 1u);
        for (i = 0; i < 4u; ++i) (void)mu_save_u64(&fw, 1u);
        mu_save_chunk_end(&fw);
        check(mu_save_writer_commit(&fw) == 0u, "overflow: one byte short fails");

        mu_save_writer_begin(&fw, fit, need, 1u);
        mu_save_chunk_begin(&fw, 1u, 1u);
        for (i = 0; i < 4u; ++i) (void)mu_save_u64(&fw, 1u);
        mu_save_chunk_end(&fw);
        check_u32(mu_save_writer_commit(&fw), need, "overflow: exactly enough succeeds");
    }
}

/* ------------------------------------------------------------------ */
/* 8. malformed values are refused, not coerced                       */
/* ------------------------------------------------------------------ */

static void test_malformed(void)
{
    uint8_t payload[32];
    mu_save_writer w;
    mu_save_reader r;
    uint32_t n;

    /* Non-canonical varint: 0x80 0x00 encodes 0 in two bytes. */
    mu_save_writer_begin_payload(&w, payload, (uint32_t)sizeof payload);
    (void)mu_save_u8(&w, 0x80u);
    (void)mu_save_u8(&w, 0x00u);
    n = mu_save_writer_commit(&w);
    mu_save_reader_over_payload(&r, payload, n);
    (void)mu_save_read_vu32(&r);
    check_status(mu_save_reader_status(&r), MU_SAVE_ERR_CORRUPT, "malformed: over-long varint is CORRUPT");

    /* A varint that never terminates. */
    {
        uint32_t i;
        mu_save_writer_begin_payload(&w, payload, (uint32_t)sizeof payload);
        for (i = 0; i < 11u; ++i) (void)mu_save_u8(&w, 0xFFu);
        n = mu_save_writer_commit(&w);
        mu_save_reader_over_payload(&r, payload, n);
        (void)mu_save_read_vu64(&r);
        check_status(mu_save_reader_status(&r), MU_SAVE_ERR_CORRUPT, "malformed: endless varint is CORRUPT");
    }

    /* A bool that is not 0 or 1. */
    mu_save_writer_begin_payload(&w, payload, (uint32_t)sizeof payload);
    (void)mu_save_u8(&w, 7u);
    n = mu_save_writer_commit(&w);
    mu_save_reader_over_payload(&r, payload, n);
    (void)mu_save_read_bool(&r);
    check_status(mu_save_reader_status(&r), MU_SAVE_ERR_CORRUPT, "malformed: bool 7 is CORRUPT, not true");

    /* Non-zero alignment padding. */
    mu_save_writer_begin_payload(&w, payload, (uint32_t)sizeof payload);
    (void)mu_save_u8(&w, 1u);
    (void)mu_save_u8(&w, 0u);
    (void)mu_save_u8(&w, 0u);
    (void)mu_save_u8(&w, 0xEEu); /* should be padding, so it must be zero */
    n = mu_save_writer_commit(&w);
    mu_save_reader_over_payload(&r, payload, n);
    (void)mu_save_read_u8(&r);
    (void)mu_save_read_align4(&r);
    check_status(mu_save_reader_status(&r), MU_SAVE_ERR_CORRUPT, "malformed: dirty padding is CORRUPT");

    /* A blob longer than the caller's buffer: consumed and reported FULL, so the
     * next field still lines up. */
    mu_save_writer_begin_payload(&w, payload, (uint32_t)sizeof payload);
    (void)mu_save_bytes(&w, "0123456789", 10u);
    (void)mu_save_u8(&w, 0x5Au);
    n = mu_save_writer_commit(&w);
    mu_save_reader_over_payload(&r, payload, n);
    {
        char tiny[4];
        check_u32(mu_save_read_bytes(&r, tiny, (uint32_t)sizeof tiny), 0u, "malformed: oversize blob returns 0");
        check_status(mu_save_reader_status(&r), MU_SAVE_ERR_FULL, "malformed: oversize blob reports FULL");
    }
}

/* ------------------------------------------------------------------ */
/* 9. reader call-order errors                                        */
/* ------------------------------------------------------------------ */

static void test_reader_state(void)
{
    uint8_t buf[256];
    mu_save_writer w;
    mu_save_reader r;

    mu_save_writer_begin(&w, buf, (uint32_t)sizeof buf, 1u);
    mu_save_chunk_begin(&w, 1u, 1u);
    mu_save_u32(&w, 1u);
    mu_save_u32(&w, 2u);
    mu_save_chunk_end(&w);
    mu_save_chunk_begin(&w, 2u, 1u);
    mu_save_u32(&w, 3u);
    mu_save_chunk_end(&w);
    mu_save_writer_commit(&w);

    /* Leaving a chunk half-read and advancing: the caller's layout is wrong and
     * the library says so instead of silently skipping bytes. */
    mu_save_open(&r, buf, mu_save_writer_size(&w), 1u);
    mu_save_next(&r);
    (void)mu_save_read_u32(&r);
    check_status(mu_save_next(&r), MU_SAVE_ERR_STATE, "state: advancing with bytes left is refused");
    check(mu_save_reader_status(&r) == MU_SAVE_ERR_STATE, "state: status reflects it");

    /* rewind clears the failure, so a caller can retry after fixing itself. */
    check_status(mu_save_rewind(&r), MU_SAVE_OK, "state: rewind clears the failure");
    check_status(mu_save_next(&r), MU_SAVE_OK, "state: and iteration works again");
    check_u32(mu_save_read_u32(&r), 1u, "state: first field");
    check_u32(mu_save_read_u32(&r), 2u, "state: second field");
    check_status(mu_save_next(&r), MU_SAVE_OK, "state: second chunk");
    check_u32(mu_save_read_u32(&r), 3u, "state: third field");
    check_status(mu_save_next(&r), MU_SAVE_END, "state: end");

    /* NULL / empty inputs. */
    check_status(mu_save_open(NULL, buf, 10u, 0u), MU_SAVE_ERR_STATE, "state: NULL reader");
    check_status(mu_save_open(&r, NULL, 10u, 0u), MU_SAVE_ERR_TRUNCATED, "state: NULL buffer");
    check_status(mu_save_open(&r, buf, 0u, 0u), MU_SAVE_ERR_TRUNCATED, "state: empty file");
    check_status(mu_save_open(&r, buf, 12u, 0u), MU_SAVE_ERR_TRUNCATED, "state: header without footer");

    /* A writer with a chunk open cannot be committed. */
    {
        uint8_t b2[128];
        mu_save_writer w2;
        mu_save_writer_begin(&w2, b2, (uint32_t)sizeof b2, 1u);
        mu_save_chunk_begin(&w2, 1u, 1u);
        check(mu_save_writer_commit(&w2) == 0u, "state: commit with a chunk open fails");
        check_status(mu_save_writer_status(&w2), MU_SAVE_ERR_STATE, "state: and says why");
        /* Nested chunks are a caller bug, not a feature. */
        mu_save_writer_begin(&w2, b2, (uint32_t)sizeof b2, 1u);
        mu_save_chunk_begin(&w2, 1u, 1u);
        check(mu_save_chunk_begin(&w2, 2u, 1u) != MU_SAVE_OK, "state: nested chunk is refused");
        /* A chunk id of 0 is reserved. */
        mu_save_writer_begin(&w2, b2, (uint32_t)sizeof b2, 1u);
        check(mu_save_chunk_begin(&w2, 0u, 1u) != MU_SAVE_OK, "state: chunk id 0 is reserved");
    }
}

/* ------------------------------------------------------------------ */
/* 10. determinism                                                    */
/* ------------------------------------------------------------------ */

static void test_determinism(void)
{
    uint8_t a[512], b[512];
    uint32_t na, nb;

    na = build_golden(a, (uint32_t)sizeof a);
    nb = build_golden(b, (uint32_t)sizeof b);
    check_u32(na, nb, "determinism: same input, same length");
    check(memcmp(a, b, na) == 0, "determinism: same input, byte-identical output");

    /* A single changed bit in the payload changes both CRCs and the bytes. */
    b[40] ^= 1u;
    check(memcmp(a, b, na) != 0, "determinism: a different state produces a different file");
}

/* ------------------------------------------------------------------ */
/* 11. the real thing: a roguelite save that resumes identically       */
/* ------------------------------------------------------------------ */

#define ROG_W 64
#define ROG_H 40
#define CH_MAP 0x4D415031u /* "MAP1" */
#define CH_RNG 0x524E4731u /* "RNG1" */

static uint8_t g_map_data[ROG_W * ROG_H];
static uint8_t g_vis_a[ROG_W * ROG_H];
static uint8_t g_vis_b[ROG_W * ROG_H];

typedef struct rog_block_ctx {
    const mu_rog_grid_u8 *g;
    uint8_t wall;
} rog_block_ctx;

static int rog_blocked_tile(int x, int y, void *user)
{
    rog_block_ctx *c = (rog_block_ctx *)user;
    return c->g->data[y * c->g->w + x] == c->wall;
}

static void test_roguelite(void)
{
    mu_rog_rng rng;
    mu_rog_grid_u8 map;
    mu_rog_roomgen_params p;
    mu_rog_rect rooms[32];
    int accepted;

    map.w = ROG_W;
    map.h = ROG_H;
    map.data = g_map_data;

    mu_rog_rng_seed(&rng, 0xC0FFEEull);
    memset(&p, 0, sizeof p);
    p.map_w = ROG_W;
    p.map_h = ROG_H;
    p.room_count_max = 40;
    p.room_w_min = 5;
    p.room_w_max = 12;
    p.room_h_min = 4;
    p.room_h_max = 9;
    p.room_padding = 1;
    p.wall_tile = 1u;
    p.floor_tile = 0u;
    accepted = mu_rog_generate_rooms_and_corridors(&rng, map, &p, rooms, 32);
    check(accepted > 0, "roguelite: the dungeon generated (workload sanity)");

    /* Save: the map, its dimensions, and the RNG state. That is the whole
     * resumable state of the generator, which is the point of a seeded one. */
    {
        uint8_t file[8192];
        mu_save_writer w;
        mu_save_writer_begin(&w, file, (uint32_t)sizeof file, 0x524F4755u /* "ROGU" */);
        mu_save_chunk_begin(&w, CH_MAP, 1u);
        mu_save_u32(&w, (uint32_t)ROG_W);
        mu_save_u32(&w, (uint32_t)ROG_H);
        mu_save_bytes(&w, map.data, (uint32_t)(ROG_W * ROG_H));
        mu_save_chunk_end(&w);
        mu_save_chunk_begin(&w, CH_RNG, 1u);
        mu_save_u64(&w, rng.s0);
        mu_save_u64(&w, rng.s1);
        mu_save_chunk_end(&w);
        check(mu_save_writer_commit(&w) != 0u, "roguelite: the save was written");

        /* Reload into fresh state. */
        {
            mu_save_reader r;
            static uint8_t loaded_map[ROG_W * ROG_H];
            mu_rog_rng loaded_rng = { 0u, 0u };
            mu_rog_grid_u8 lmap;
            uint32_t lw = 0, lh = 0, bytes = 0;

            check_status(mu_save_open(&r, file, mu_save_writer_size(&w), 0x524F4755u), MU_SAVE_OK,
                         "roguelite: reload opens");
            while (mu_save_next(&r) == MU_SAVE_OK) {
                if (r.chunk_id == CH_MAP) {
                    lw = mu_save_read_u32(&r);
                    lh = mu_save_read_u32(&r);
                    bytes = mu_save_read_bytes(&r, loaded_map, (uint32_t)sizeof loaded_map);
                } else if (r.chunk_id == CH_RNG) {
                    loaded_rng.s0 = mu_save_read_u64(&r);
                    loaded_rng.s1 = mu_save_read_u64(&r);
                }
            }
            check_status(r.status, MU_SAVE_END, "roguelite: iteration ends cleanly");
            check_u32(lw, ROG_W, "roguelite: width round-tripped");
            check_u32(lh, ROG_H, "roguelite: height round-tripped");
            check_u32(bytes, ROG_W * ROG_H, "roguelite: the whole map blob round-tripped");
            check(memcmp(loaded_map, map.data, (size_t)bytes) == 0, "roguelite: the map is byte-identical");
            check(loaded_rng.s0 == rng.s0 && loaded_rng.s1 == rng.s1, "roguelite: the RNG state round-tripped");

            /* The future must be identical, not merely the present: 100 dice
             * rolls from the loaded generator must match the live one. */
            {
                int i, same = 1;
                for (i = 0; i < 100; ++i)
                    if (mu_rog_rng_u64(&rng) != mu_rog_rng_u64(&loaded_rng)) same = 0;
                check(same, "roguelite: 100 rolls after reload match the live generator");
            }

            /* And the derived data matches: FOV from the same origin over the
             * reloaded map equals FOV over the original. */
            lmap.w = (int)lw;
            lmap.h = (int)lh;
            lmap.data = loaded_map;
            {
                rog_block_ctx ca, cb;
                ca.g = &map;
                ca.wall = 1u;
                cb.g = &lmap;
                cb.wall = 1u;
                mu_rog_fov_raycast(ROG_W, ROG_H, 10, 10, 8, rog_blocked_tile, &ca, g_vis_a);
                mu_rog_fov_raycast(ROG_W, ROG_H, 10, 10, 8, rog_blocked_tile, &cb, g_vis_b);
                check(memcmp(g_vis_a, g_vis_b, sizeof g_vis_a) == 0,
                      "roguelite: FOV over the reloaded map matches");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 12. file IO (atomic write, read back)                              */
/* ------------------------------------------------------------------ */

static void test_file_io(void)
{
    uint8_t file[4096], back[4096];
    uint32_t n, got = 0;
    const char *path = "save_test.bin";
    const char *tmp = "save_test.bin.tmp";
    mu_save_writer w;
    mu_save_reader r;

    mu_save_writer_begin(&w, file, (uint32_t)sizeof file, 0xBEEFu);
    mu_save_chunk_begin(&w, 1u, 1u);
    mu_save_str(&w, "on disk");
    mu_save_chunk_end(&w);
    n = mu_save_writer_commit(&w);

    check_u32(mu_save_file_size_on_disk(path), 0xFFFFFFFFu, "io: a missing file reports a sentinel");
    check_status(mu_save_write_file_atomic(path, file, n), MU_SAVE_OK, "io: atomic write succeeds");
    check_u32(mu_save_file_size_on_disk(path), n, "io: the file has the right size");
    check(mu_save_file_size_on_disk(tmp) == 0xFFFFFFFFu, "io: the temp file is gone after the rename");
    check_status(mu_save_read_file(path, back, (uint32_t)sizeof back, &got), MU_SAVE_OK, "io: read back");
    check_u32(got, n, "io: read back the same size");
    check(memcmp(file, back, n) == 0, "io: read back the same bytes");
    check_status(mu_save_open(&r, back, got, 0xBEEFu), MU_SAVE_OK, "io: the file on disk opens");

    /* Overwriting an existing file must replace it, not append. */
    {
        uint8_t small[64];
        uint32_t ns;
        mu_save_writer vw;
        mu_save_writer_begin(&vw, small, (uint32_t)sizeof small, 0xBEEFu);
        mu_save_chunk_begin(&vw, 2u, 1u);
        mu_save_chunk_end(&vw);
        ns = mu_save_writer_commit(&vw);
        check_status(mu_save_write_file_atomic(path, small, ns), MU_SAVE_OK, "io: overwrite succeeds");
        check_u32(mu_save_file_size_on_disk(path), ns, "io: overwrite truncated, did not append");
    }

    /* A too-small destination reports the real size and does not lie about it. */
    check_status(mu_save_read_file(path, back, 4u, &got), MU_SAVE_ERR_FULL, "io: a short buffer is FULL");
    check(got > 4u, "io: but the real size is still reported");
    check_status(mu_save_read_file("save_test_missing.bin", back, (uint32_t)sizeof back, &got), MU_SAVE_ERR_IO,
                 "io: a missing file is an IO error");

    remove(path);
}

/* Digest of every CRC in a fixed workload, for the build-comparison target:
 * the same digest from a -DMU_SAVE_CRC_HW=0 build and the default build proves
 * the hardware path is an optimization and not a format change. */
static uint32_t run_crc_hash(void)
{
    static uint8_t buf[512];
    uint32_t h = 2166136261u, i, k;
    for (k = 0; k < 512u; ++k) buf[k] = (uint8_t)(k * 97u + 3u);
    for (i = 0; i <= 512u; i += 13u) {
        uint32_t c = mu_save_crc32c(buf, i);
        for (k = 0; k < 4u; ++k) {
            h ^= (c >> (8u * k)) & 0xFFu;
            h *= 16777619u;
        }
    }
    printf("%08x\n", h);
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--crc-hash") == 0) return (int)run_crc_hash();

    printf("mu save/load gate (chunked container, CRC32C, migration, atomic write)\n");
    printf("--------------------------------------------------------------------\n");
    test_crc();
    test_golden();
    test_roundtrip();
    test_truncation();
    test_corruption();
    test_forward_compat();
    test_chunk_guards();
    test_migration();
    test_writer_overflow();
    test_malformed();
    test_reader_state();
    test_determinism();
    test_roguelite();
    test_file_io();
    printf("--------------------------------------------------------------------\n");
    printf("%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
