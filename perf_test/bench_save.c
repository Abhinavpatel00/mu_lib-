/* bench_save.c -- what the save format costs, in ns and in bytes
 *
 * Four questions, in the order they matter to a game:
 *
 *   1. WHAT DOES A REALISTIC SAVE COST?  A 200-entity world (position, health,
 *      inventory count, a name) written and read back, against the obvious
 *      baseline: `fwrite`-per-field with no framing, in the same process.
 *
 *   2. WHAT DOES INTEGRITY COST?  CRC32C over the payload, slicing-by-8 table
 *      vs SSE4.2, at 64 B / 4 KiB / 1 MiB. The 64 B row is the one that matters
 *      for a save (tens of small chunks), the 1 MiB row is the one that shows
 *      whether the table is even in cache any more.
 *
 *   3. WHAT DOES A VARINT BUY?  Bytes, not ns: the same 200-entity payload with
 *      fixed-width fields against varint fields. A save file's cost to the
 *      player is bytes on disk and bytes on the wire, and this is where that
 *      number comes from.
 *
 *   4. WHAT DOES THE UP-FRONT VALIDATION COST?  mu_save_open with and without
 *      the whole-file CRC pass, at 4 KiB and 1 MiB.
 *
 *   make -C perf_test save-bench
 *   ./bench_save --filter=save --samples=31
 *   ./bench_save --bytes           (the byte-count table, no clock involved)
 */

#define MU_BENCH_IMPLEMENTATION 1
#include "mu_bench.h" /* first: it sets _POSIX_C_SOURCE */

#define MU_SAVE_IMPLEMENTATION 1
#include "mu_save.h"

#include <stdio.h>
#include <string.h>

static volatile uint32_t g_sink;

#define ENTITIES 200u
#define NAME_LEN 16u

/* The payload a game actually saves. Kept as parallel arrays because that is
 * how an ECS holds it; the writer walks them the same way. */
static float    g_pos_x[ENTITIES], g_pos_y[ENTITIES];
static int32_t  g_hp[ENTITIES];
static uint8_t  g_inv_count[ENTITIES];
static char     g_name[ENTITIES][NAME_LEN];
static uint32_t g_id[ENTITIES];

static uint8_t  g_file[64 * 1024];   /* the save buffer */
static uint8_t  g_copy[64 * 1024];
static uint8_t  g_big[(1u << 20) + 65536u]; /* 1 MiB + headroom for chunk headers */
static uint8_t  g_crc_small[64];

static uint32_t g_file_size;

static void fill_world(void)
{
    uint32_t s = 12345u, i;
    for (i = 0; i < ENTITIES; ++i) {
        s = s * 1664525u + 1013904223u;
        g_pos_x[i] = (float)(int32_t)(s >> 8) * 0.001f;
        s = s * 1664525u + 1013904223u;
        g_pos_y[i] = (float)(int32_t)(s >> 8) * 0.001f;
        s = s * 1664525u + 1013904223u;
        g_hp[i] = (int32_t)(s % 1000u);
        s = s * 1664525u + 1013904223u;
        g_inv_count[i] = (uint8_t)(s % 20u);
        s = s * 1664525u + 1013904223u;
        g_id[i] = s % 3000u; /* small ids: the case a varint is for */
        for (uint32_t k = 0; k < NAME_LEN; ++k) g_name[i][k] = (char)('a' + (s >> (k % 24)) % 26u);
        g_name[i][NAME_LEN - 1] = '\0';
    }
}

/* ---- the payload, written the way a caller would ------------------------- */

static uint32_t write_world(uint8_t* buf, uint32_t cap, int use_varint)
{
    mu_save_writer w;
    uint32_t i;

    mu_save_writer_begin(&w, buf, cap, 0x574F524Cu /* "WORL" */);
    mu_save_chunk_begin(&w, 1u, 1u);
    mu_save_vu32(&w, ENTITIES);
    for (i = 0; i < ENTITIES; ++i) {
        mu_save_u32(&w, use_varint ? 0u : g_id[i]); /* id: varint or fixed */
        if (use_varint) {
            /* re-write as a varint, which is the whole point of the flag */
            w.pos -= 4u;
            mu_save_vu32(&w, g_id[i]);
        }
        mu_save_f32(&w, g_pos_x[i]);
        mu_save_f32(&w, g_pos_y[i]);
        if (use_varint) {
            mu_save_vi32(&w, g_hp[i]);
            mu_save_vu32(&w, g_inv_count[i]);
        } else {
            mu_save_i32(&w, g_hp[i]);
            mu_save_u32(&w, g_inv_count[i]);
        }
        mu_save_str(&w, g_name[i]);
    }
    mu_save_chunk_end(&w);
    return mu_save_writer_commit(&w);
}

static uint32_t read_world(const uint8_t* buf, uint32_t size, int use_varint)
{
    mu_save_reader r;
    uint32_t i, acc = 0u;

    if (mu_save_open(&r, buf, size, 0x574F524Cu) != MU_SAVE_OK) return 0u;
    while (mu_save_next(&r) == MU_SAVE_OK) {
        (void)mu_save_read_vu32(&r);
        for (i = 0; i < ENTITIES; ++i) {
            char name[NAME_LEN];
            acc += use_varint ? mu_save_read_vu32(&r) : mu_save_read_u32(&r);
            acc += (uint32_t)mu_save_read_f32(&r);
            acc += (uint32_t)mu_save_read_f32(&r);
            acc += use_varint ? (uint32_t)mu_save_read_vi32(&r) : (uint32_t)mu_save_read_i32(&r);
            acc += use_varint ? mu_save_read_vu32(&r) : mu_save_read_u32(&r);
            (void)mu_save_read_str(&r, name, NAME_LEN);
        }
    }
    return acc;
}

/* Field emission alone: the same 2000 fields into a payload writer, so the
 * difference against write_world is the framing and the chunk CRC rather than
 * an estimate of them. */
static uint32_t emit_fields(uint8_t* buf, uint32_t cap)
{
    mu_save_writer w;
    uint32_t i;

    mu_save_writer_begin_payload(&w, buf, cap);
    (void)mu_save_vu32(&w, ENTITIES);
    for (i = 0; i < ENTITIES; ++i) {
        (void)mu_save_u32(&w, g_id[i]);
        (void)mu_save_f32(&w, g_pos_x[i]);
        (void)mu_save_f32(&w, g_pos_y[i]);
        (void)mu_save_i32(&w, g_hp[i]);
        (void)mu_save_u32(&w, g_inv_count[i]);
        (void)mu_save_str(&w, g_name[i]);
    }
    return mu_save_writer_commit(&w);
}

static uint32_t read_fields(const uint8_t* buf, uint32_t size)
{
    mu_save_reader r;
    uint32_t i, acc = 0u;

    mu_save_reader_over_payload(&r, buf, size);
    (void)mu_save_read_vu32(&r);
    for (i = 0; i < ENTITIES; ++i) {
        char name[NAME_LEN];
        acc += mu_save_read_u32(&r);
        acc += (uint32_t)mu_save_read_f32(&r);
        acc += (uint32_t)mu_save_read_f32(&r);
        acc += (uint32_t)mu_save_read_i32(&r);
        acc += mu_save_read_u32(&r);
        (void)mu_save_read_str(&r, name, NAME_LEN);
    }
    return acc;
}

/* ---- the baseline: a raw struct dump, no framing, no checksum ------------ */

static uint32_t raw_write(uint8_t* buf, uint32_t cap)
{
    uint32_t at = 0, i;
    for (i = 0; i < ENTITIES; ++i) {
        memcpy(buf + at, &g_id[i], 4); at += 4;
        memcpy(buf + at, &g_pos_x[i], 4); at += 4;
        memcpy(buf + at, &g_pos_y[i], 4); at += 4;
        memcpy(buf + at, &g_hp[i], 4); at += 4;
        buf[at++] = g_inv_count[i];
        memcpy(buf + at, g_name[i], NAME_LEN); at += NAME_LEN;
        if (at > cap) return 0;
    }
    return at;
}

static uint32_t raw_read(const uint8_t* buf, uint32_t size)
{
    uint32_t at = 0, acc = 0, i;
    for (i = 0; i < ENTITIES && at + 21u + NAME_LEN <= size; ++i) {
        int32_t id, hp;
        float x, y;
        memcpy(&id, buf + at, 4); at += 4;
        memcpy(&x, buf + at, 4); at += 4;
        memcpy(&y, buf + at, 4); at += 4;
        memcpy(&hp, buf + at, 4); at += 4;
        acc += (uint32_t)id + (uint32_t)hp + (uint32_t)x + (uint32_t)y;
        at += 1 + NAME_LEN;
    }
    return acc;
}

/* ---- rows --------------------------------------------------------------- */

MU_BENCH(save, write_world)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_file_size = write_world(g_file, (uint32_t)sizeof g_file, 0);
		g_sink += g_file_size;
	}
}

MU_BENCH(save, read_world)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += read_world(g_file, g_file_size, 0);
	}
}

MU_BENCH_SETUP(save, write_world) { (void)state; fill_world(); }
MU_BENCH_SETUP(save, read_world)
{
	(void)state;
	fill_world();
	g_file_size = write_world(g_file, (uint32_t)sizeof g_file, 0);
}

MU_BENCH(save, write_raw)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += raw_write(g_copy, (uint32_t)sizeof g_copy);
	}
}

MU_BENCH(save, read_raw)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += raw_read(g_copy, sizeof g_copy);
	}
}

MU_BENCH_SETUP(save, emit_fields) { (void)state; fill_world(); }
MU_BENCH_SETUP(save, read_fields)
{
	(void)state;
	fill_world();
	(void)emit_fields(g_copy, (uint32_t)sizeof g_copy);
}
MU_BENCH_SETUP(save, read_world_varint)
{
	(void)state;
	fill_world();
	g_file_size = write_world(g_file, (uint32_t)sizeof g_file, 1);
}

MU_BENCH_SETUP(save, write_raw)
{
	(void)state;
	fill_world();
	(void)raw_write(g_copy, (uint32_t)sizeof g_copy);
}
MU_BENCH_SETUP(save, read_raw) { (void)state; fill_world(); }

MU_BENCH(save, emit_fields)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += emit_fields(g_copy, (uint32_t)sizeof g_copy);
	}
}

MU_BENCH(save, read_fields)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += read_fields(g_copy, g_file_size);
	}
}

MU_BENCH(save, read_world_varint)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += read_world(g_file, g_file_size, 1);
	}
}

/* open() with and without the whole-file CRC pass, on a 1 MiB file made of
 * 4 KiB chunks: the validation price a player pays when loading. */
static uint32_t g_big_size;

static void build_big(void)
{
    mu_save_writer w;
    uint32_t k;

    memset(g_big, 0, sizeof g_big);
    mu_save_writer_begin(&w, g_big, (uint32_t)sizeof g_big, 0x42494731u);
    for (k = 0; k < 255u; ++k) { /* 255 * (16 + 4096) + 24 == 1 048 584 ~ 1 MiB */
        mu_save_chunk_begin(&w, 0x1000u + k, 1u);
        (void)mu_save_bytes(&w, g_big, 4096u);
        mu_save_chunk_end(&w);
    }
    g_big_size = mu_save_writer_commit(&w);
}

MU_BENCH(save, open_verify)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		mu_save_reader r;
		g_sink += (uint32_t)mu_save_open(&r, g_big, g_big_size, 0x42494731u);
	}
}

MU_BENCH(save, open_no_verify)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		mu_save_reader r;
		g_sink += (uint32_t)mu_save_open_ex(&r, g_big, g_big_size, 0x42494731u, 0);
	}
}

MU_BENCH_SETUP(save, open_verify) { (void)state; build_big(); }
MU_BENCH_SETUP(save, open_no_verify) { (void)state; build_big(); }

/* ---- crc, three sizes, two implementations ------------------------------ */

MU_BENCH(crc, sw_64)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += mu_save_crc32c_sw(g_crc_small, sizeof g_crc_small);
	}
}

MU_BENCH(crc, sw_4k)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += mu_save_crc32c_sw(g_big, 4096u);
	}
}

MU_BENCH(crc, sw_1m)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += mu_save_crc32c_sw(g_big, (uint32_t)sizeof g_big);
	}
}

#if MU_SAVE_HAS_CRC_HW
MU_BENCH(crc, hw_64)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += mu_save_crc32c_hw(g_crc_small, sizeof g_crc_small);
	}
}

MU_BENCH(crc, hw_4k)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += mu_save_crc32c_hw(g_big, 4096u);
	}
}

MU_BENCH(crc, hw_1m)
{
	uint64_t i;
	MU_BENCH_ITERATE(i)
	{
		g_sink += mu_save_crc32c_hw(g_big, (uint32_t)sizeof g_big);
	}
}
#endif

/* ---- the byte table (no clock) ------------------------------------------ */

static int run_bytes(void)
{
    uint32_t fixed_size, var_size, raw_size;

    fill_world();
    fixed_size = write_world(g_file, (uint32_t)sizeof g_file, 0);
    var_size = write_world(g_file, (uint32_t)sizeof g_file, 1);
    raw_size = raw_write(g_copy, (uint32_t)sizeof g_copy);

    printf("mu_save payload sizes, %u entities with a %u-byte name\n", ENTITIES, NAME_LEN - 1u);
    printf("--------------------------------------------------------------\n");
    printf("  raw struct dump (no framing)      %6u bytes\n", raw_size);
    printf("  mu_save, fixed-width fields       %6u bytes  (+%.1f%% vs raw: header,\n"
           "                                                  chunk header, footer, varint prefixes)\n",
           fixed_size, 100.0 * ((double)fixed_size - (double)raw_size) / (double)raw_size);
    printf("  mu_save, varints for id/hp/inv    %6u bytes  (%.1f%% smaller than fixed)\n", var_size,
           100.0 * ((double)fixed_size - (double)var_size) / (double)fixed_size);
    printf("  per entity: fixed %.1f B, varint %.1f B, raw %.1f B\n",
           (double)fixed_size / ENTITIES, (double)var_size / ENTITIES, (double)raw_size / ENTITIES);
    /* Framing is exactly header + one chunk header + footer. The rest of the gap
     * against the raw dump is per-value metadata: a 1-byte length prefix on every
     * string and the inventory count stored as u32 in this payload. */
    printf("  framing (header + chunk header + footer): %u bytes\n", 12u + 16u + 12u);
    printf("  rest of the gap vs raw: %d bytes, all per-field prefixes and\n"
           "                          the inventory count as u32 (36 B/entity vs 33 raw)\n",
           (int)fixed_size - (int)raw_size - 40);
    return 0;
}

int main(int argc, char** argv)
{
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--bytes") == 0) return run_bytes();
	}
	fill_world();
	g_file_size = write_world(g_file, (uint32_t)sizeof g_file, 0);
	memset(g_crc_small, 0xA5, sizeof g_crc_small);
	mu_save_crc_tables_init();

	/* A fixture that silently failed to build turns its rows into measurements
	 * of the failure path (the first version of this file timed mu_save_open on
	 * a zero-byte file at 2.1 ns and called it validation). Refuse to run. */
	build_big();
	if (g_big_size == 0u) {
		fprintf(stderr, "bench_save: the 1 MiB fixture did not build\n");
		return 1;
	}
	return mu_bench_main(argc, argv);
}
