#ifndef MU_SAVE_H
#define MU_SAVE_H

/* fsync/fileno in the optional file layer are POSIX and hidden under a strict
 * -std=c99 unless asked for. Same pattern as mu_vm.h and mu_bench.h: ask only
 * if the caller has not chosen already. */
#if defined(MU_SAVE_WANT_STDIO) && !defined(_POSIX_C_SOURCE) && !defined(_DEFAULT_SOURCE) && \
	!defined(__cplusplus) && !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

/* mu_save.h -- a save file format, not just a bit writer.
 *
 * mu_bitserial.h already writes bits. What it does not do is answer the four
 * questions a shipped game asks the first time a player has a 40-hour save and
 * the studio patches the schema:
 *
 *   1. HOW DO I SKIP WHAT I DO NOT UNDERSTAND?  Chunks carry an id, a version
 *      and a length, so a reader walks past a chunk it has never heard of and
 *      keeps going. A save written by patch 1.4 loads in a 1.2 binary because
 *      1.2 skips the two new chunks instead of failing. This is the single
 *      property that makes old saves work at all.
 *
 *   2. HOW DO I KNOW THE FILE IS INTACT?  CRC32C per chunk (so corruption is
 *      *localised*, and reported against the chunk id that failed) plus a
 *      whole-file CRC in the footer, so mu_save_open can reject a damaged or
 *      truncated file before a single game field is decoded.
 *
 *   3. HOW DO I UPGRADE IT?  mu_save_upgrade_chain runs v1 -> v2 -> v3 from a
 *      table of one-step functions, ping-ponging through two caller-owned
 *      scratch buffers. No allocation, and every step is gated separately.
 *
 *   4. HOW DO I NOT LOSE IT?  mu_save_write_file_atomic writes a temp file,
 *      fsyncs it, then renames over the old save -- so a crash or a power cut
 *      mid-save leaves the *previous* save intact instead of a torn one.
 *      Optional (MU_SAVE_WANT_STDIO); the core does no IO.
 *
 * WHAT THIS HEADER IS NOT: a serializer for your structs. There is no
 * reflection, no field registration, no zero-copy struct dump. A struct dump is
 * exactly the thing that breaks between patches -- padding, endianness, float
 * bit patterns, a member added in 1.3 -- and every one of those bugs lands on
 * the player. You write the fields; this writes the file.
 *
 * DESIGN RULES, all deliberate:
 *
 *   - no allocation, ever. Every buffer is caller-owned, including the
 *     migration scratch. The writer refuses to overflow and *stays* refused.
 *   - little-endian on the wire, byte by byte, so the file is identical on
 *     every machine and a big-endian port reads the same bytes.
 *   - floats are serialised as their exact bits (memcpy through uint32/64), so
 *     a NaN payload survives instead of being silently canonicalised -- which
 *     would break replay equality.
 *   - varints for what is usually small (ids, counts, deltas), fixed widths
 *     where the byte length must not depend on the value.
 *   - every read is bounds-checked against the chunk payload; a short chunk
 *     sets MU_SAVE_ERR_TRUNCATED and returns 0 rather than reading past it.
 *   - mu_ prefix, single header, C99 and C++14 clean. */

#include "mu_macros.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------- 1. status ----------- */

typedef enum mu_save_status {
	MU_SAVE_OK = 0,
	MU_SAVE_END = 1,            /* no more chunks: not an error               */
	MU_SAVE_ERR_FULL = -1,      /* the writer's buffer is too small           */
	MU_SAVE_ERR_MAGIC = -2,     /* not a mu_save file (header or end magic)   */
	MU_SAVE_ERR_FORMAT = -3,    /* format_id does not match                   */
	MU_SAVE_ERR_TRUNCATED = -4, /* file or chunk ends mid-value               */
	MU_SAVE_ERR_CORRUPT = -5,   /* checksum, padding or a non-canonical value */
	MU_SAVE_ERR_VERSION = -6,   /* a version newer than we know               */
	MU_SAVE_ERR_STATE = -7,     /* API used out of order                      */
	MU_SAVE_ERR_IO = -8         /* MU_SAVE_WANT_STDIO only                    */
} mu_save_status;

MU_INLINE const char* mu_save_status_str(mu_save_status s) {
	switch (s) {
	case MU_SAVE_OK: return "ok";
	case MU_SAVE_END: return "end";
	case MU_SAVE_ERR_FULL: return "buffer full";
	case MU_SAVE_ERR_MAGIC: return "not a mu_save file";
	case MU_SAVE_ERR_FORMAT: return "format id mismatch";
	case MU_SAVE_ERR_TRUNCATED: return "truncated";
	case MU_SAVE_ERR_CORRUPT: return "corrupt";
	case MU_SAVE_ERR_VERSION: return "unsupported version";
	case MU_SAVE_ERR_STATE: return "bad call order";
	case MU_SAVE_ERR_IO: return "io error";
	}
	return "?";
}

/* ----------- 2. crc32c (Castagnoli) ----------- */

/* CRC-32C: polynomial 0x1EDC6F41 reflected (0x82F63B78), init 0xFFFFFFFF,
 * reflected in and out, no final xor. That is exactly what SSE4.2's `crc32`
 * instruction, iSCSI, Btrfs and ext4 compute, so the hardware path is a handful
 * of instructions instead of a table.
 *
 * mu_save_crc32c_sw is slicing-by-8; mu_save_crc32c_hw is the SSE4.2 one when
 * the compiler and CPU have it. Both are exposed so the gate can demand they
 * agree on every length and bit pattern, and so the bench can A/B them.
 * mu_save_crc32c dispatches at runtime.
 *
 * The slicing-by-8 tables are 8 KiB, built once at first use (mu_save_crc_tables_init
 * is optional -- the first CRC call does it). 8 KiB of writable .data buys ~3x
 * the throughput of slicing-by-1; PERF_SAVE.md measures it. */
void mu_save_crc_tables_init(void);
uint32_t mu_save_crc32c_sw(const void* data, size_t len);
uint32_t mu_save_crc32c_sw_extend(uint32_t crc, const void* data, size_t len);

#ifndef MU_SAVE_CRC_HW
#define MU_SAVE_CRC_HW 1
#endif

#if MU_SAVE_CRC_HW && (defined(__x86_64__) || defined(__i386__)) && \
	(defined(__GNUC__) || defined(__clang__))
#define MU_SAVE_HAS_CRC_HW 1
uint32_t mu_save_crc32c_hw(const void* data, size_t len);
uint32_t mu_save_crc32c_hw_extend(uint32_t crc, const void* data, size_t len);
#else
#define MU_SAVE_HAS_CRC_HW 0
#endif

uint32_t mu_save_crc32c(const void* data, size_t len);

/* ----------- 3. the file layout ----------- */
/*
 *   header (12 bytes)
 *     0  u32  magic          "MSV1" = 0x3156534D
 *     4  u16  file_version   1
 *     6  u16  flags          0 (reserved; a reader refuses unknown bits)
 *     8  u32  format_id      caller's, so two products cannot swap saves
 *
 *   then zero or more chunks, each:
 *     +0  u32  chunk_id      caller-defined; 0 is reserved as "invalid"
 *     +4  u16  chunk_version caller-defined, so schemas version independently
 *     +6  u16  flags         0 (reserved)
 *     +8  u32  size          payload bytes
 *     +12 u32  crc32c        of the payload
 *     +16 payload, then zero padding to a 4-byte boundary
 *
 *   footer (12 bytes)
 *     0  u32  magic          "SEND" = 0x444E4553
 *     4  u32  chunk_count
 *     8  u32  crc32c         of every byte before the footer
 *
 * NO CHUNK DIRECTORY, and that is a change from the plan in docs/roadmap.md,
 * which called for one. A directory exists to answer two questions: can a
 * reader skip data it does not understand, and can it validate the file before
 * touching it. Inline id+length records answer both -- skipping is
 * `pos += 16 + size + pad`, and the footer CRC validates the whole file in one
 * pass before any chunk is decoded -- for 16 bytes per chunk instead of a
 * second table to keep in sync with the first. What a directory would add is
 * random access by id, and this format does not do random access:
 * mu_save_find scans, because a save has tens of chunks, not millions.
 */

#define MU_SAVE_MAGIC 0x3156534Du    /* "MSV1" little-endian */
#define MU_SAVE_END_MAGIC 0x444E4553u /* "SEND" little-endian */
#define MU_SAVE_FILE_VERSION 1u
#define MU_SAVE_HEADER_SIZE 12u
#define MU_SAVE_FOOTER_SIZE 12u
#define MU_SAVE_CHUNK_HEADER_SIZE 16u
#define MU_SAVE_MAX_CHUNKS 1048576u /* a corrupt count cannot make us loop forever */
#define MU_SAVE_NO_ID 0u            /* "accept any format_id" on read */

/* ----------- 4. writer ----------- */

typedef struct mu_save_writer {
	uint8_t* buf;
	uint32_t cap;
	uint32_t pos;
	uint32_t format_id;

	uint32_t payload_base; /* where the current payload starts (align4 is relative to this) */
	uint32_t chunk_start;  /* offset of the current chunk's header */
	uint32_t chunk_count;
	uint32_t chunk_id;
	uint16_t chunk_version;

	int raw;      /* a bare payload writer: no header, no chunks, no footer */
	int in_chunk;
	int failed;   /* sticky: once set, every write is a no-op */
	mu_save_status status;
} mu_save_writer;

/* Starts a file. Writes the header immediately. */
mu_save_status mu_save_writer_begin(mu_save_writer* w, void* buf, uint32_t cap, uint32_t format_id);

/* Starts a bare payload in `buf`: no file header, no chunks, no footer. This is
 * what a migration step writes into (and what a caller uses when it wants the
 * fields without the container). mu_save_writer_commit returns the size. */
mu_save_status mu_save_writer_begin_payload(mu_save_writer* w, void* buf, uint32_t cap);

/* Opens a chunk. Writes are only legal between begin and end. A zero-size chunk
 * is legal and still counts. */
mu_save_status mu_save_chunk_begin(mu_save_writer* w, uint32_t chunk_id, uint16_t version);
mu_save_status mu_save_chunk_end(mu_save_writer* w);

/* Fills in the footer (or, for a payload writer, nothing) and returns the byte
 * count, or 0 on failure -- check mu_save_writer_status. */
uint32_t mu_save_writer_commit(mu_save_writer* w);

MU_INLINE uint32_t mu_save_writer_size(const mu_save_writer* w) { return w->pos; }
MU_INLINE mu_save_status mu_save_writer_status(const mu_save_writer* w) { return w->status; }
MU_INLINE int mu_save_writer_ok(const mu_save_writer* w) { return w->status == MU_SAVE_OK; }

/* Scalar writers. All little-endian on the wire. */
mu_save_status mu_save_u8(mu_save_writer* w, uint8_t v);
mu_save_status mu_save_u16(mu_save_writer* w, uint16_t v);
mu_save_status mu_save_u32(mu_save_writer* w, uint32_t v);
mu_save_status mu_save_u64(mu_save_writer* w, uint64_t v);
mu_save_status mu_save_i32(mu_save_writer* w, int32_t v);
mu_save_status mu_save_i64(mu_save_writer* w, int64_t v);
mu_save_status mu_save_bool(mu_save_writer* w, int v);

/* Floats go out as their exact bit pattern, NaN payloads included. */
mu_save_status mu_save_f32(mu_save_writer* w, float v);
mu_save_status mu_save_f64(mu_save_writer* w, double v);

/* Varints: LEB128 for unsigned, zigzag+LEB128 for signed. */
mu_save_status mu_save_vu32(mu_save_writer* w, uint32_t v);
mu_save_status mu_save_vu64(mu_save_writer* w, uint64_t v);
mu_save_status mu_save_vi32(mu_save_writer* w, int32_t v);
mu_save_status mu_save_vi64(mu_save_writer* w, int64_t v);

/* A varint length, then the bytes. NULL with len 0 writes just the length. */
mu_save_status mu_save_bytes(mu_save_writer* w, const void* data, uint32_t len);
/* NUL-terminated string, terminator not stored. NULL writes an empty string. */
mu_save_status mu_save_str(mu_save_writer* w, const char* s);

/* Pads with zero bytes to a 4-byte boundary, relative to the payload start.
 * Reads verify the padding is zero, which catches a chunk that parses but is
 * short. */
mu_save_status mu_save_align4(mu_save_writer* w);

/* ----------- 5. reader ----------- */

typedef struct mu_save_reader {
	const uint8_t* buf;
	uint32_t size; /* whole file (or whole payload, for the bare reader) */
	uint32_t pos;  /* cursor, in the file */

	uint32_t chunk_start; /* offset of the current chunk header */
	uint32_t chunk_id;
	uint32_t chunk_size;
	uint32_t chunk_crc;
	uint16_t chunk_version;
	uint16_t chunk_flags;
	const uint8_t* chunk_payload;
	uint32_t payload_read;

	uint32_t chunk_count;
	uint32_t chunks_seen;
	uint32_t format_id;
	uint16_t file_version;

	int payload_only; /* built by mu_save_reader_over_payload */
	int opened;
	int failed;
	mu_save_status status;
} mu_save_reader;

/* Validates magic, flags, file version, footer and the whole-file CRC, then
 * positions at the first chunk. `expect_format_id` may be MU_SAVE_NO_ID.
 * Nothing is decoded until this returns MU_SAVE_OK. */
mu_save_status mu_save_open(mu_save_reader* r, const void* buf, uint32_t size, uint32_t expect_format_id);

/* Same, with `verify` = 0 to skip the whole-file CRC pass. A deliberate escape
 * hatch for a huge save on a slow disk: per-chunk CRCs still run when each
 * chunk is entered, so corruption is still caught -- just later, and with a
 * worse message. */
mu_save_status mu_save_open_ex(mu_save_reader* r, const void* buf, uint32_t size,
							   uint32_t expect_format_id, int verify);

/* Builds a reader over a bare payload, as if it were the current chunk. Used by
 * migration steps and by anything that wants the field readers without the
 * container. */
mu_save_status mu_save_reader_over_payload(mu_save_reader* r, const void* payload, uint32_t size);

/* Advances to the next chunk: MU_SAVE_OK with chunk_id/version/size available,
 * or MU_SAVE_END. Reaching the footer with a chunk unconsumed is
 * MU_SAVE_ERR_STATE -- a caller that decoded the wrong layout is told. */
mu_save_status mu_save_next(mu_save_reader* r);

/* Back to the first chunk; also clears a failure, so a caller can retry. */
mu_save_status mu_save_rewind(mu_save_reader* r);

/* Scan for a chunk id from the first chunk: MU_SAVE_OK (positioned at it) or
 * MU_SAVE_ERR_STATE if absent. O(chunks). */
mu_save_status mu_save_find(mu_save_reader* r, uint32_t chunk_id);

/* Discards whatever is left of the current chunk so the next mu_save_next
 * works. This is the "I do not know this chunk" move, and it is what
 * mu_save_find uses internally. */
mu_save_status mu_save_skip_chunk(mu_save_reader* r);

MU_INLINE uint32_t mu_save_chunk_remaining(const mu_save_reader* r) {
	return r->chunk_size - r->payload_read;
}
MU_INLINE const uint8_t* mu_save_chunk_payload(const mu_save_reader* r) { return r->chunk_payload; }

/* Scalar readers. On underrun they set MU_SAVE_ERR_TRUNCATED, return 0, and
 * stay failed for every later read. */
uint8_t mu_save_read_u8(mu_save_reader* r);
uint16_t mu_save_read_u16(mu_save_reader* r);
uint32_t mu_save_read_u32(mu_save_reader* r);
uint64_t mu_save_read_u64(mu_save_reader* r);
int32_t mu_save_read_i32(mu_save_reader* r);
int64_t mu_save_read_i64(mu_save_reader* r);
int mu_save_read_bool(mu_save_reader* r);
float mu_save_read_f32(mu_save_reader* r);
double mu_save_read_f64(mu_save_reader* r);
uint32_t mu_save_read_vu32(mu_save_reader* r);
uint64_t mu_save_read_vu64(mu_save_reader* r);
int32_t mu_save_read_vi32(mu_save_reader* r);
int64_t mu_save_read_vi64(mu_save_reader* r);

/* Length-prefixed blob into `dst` (capacity `cap`): returns the byte count, or
 * 0 with status FULL if it does not fit -- the blob is consumed either way, so
 * iteration stays in sync with a caller that cannot hold it. */
uint32_t mu_save_read_bytes(mu_save_reader* r, void* dst, uint32_t cap);
mu_save_status mu_save_skip_bytes(mu_save_reader* r);

/* Length-prefixed string into `dst`, NUL-terminated, truncated to cap-1 bytes
 * if needed (the whole string is still consumed). Returns the stored length. */
uint32_t mu_save_read_str(mu_save_reader* r, char* dst, uint32_t cap);

/* Skips zero padding to a 4-byte boundary; non-zero padding is CORRUPT.
 * (The writer's counterpart is mu_save_align4; same idea, different subject, so
 * the names differ.) */
uint32_t mu_save_read_align4(mu_save_reader* r);

MU_INLINE int mu_save_reader_ok(const mu_save_reader* r) { return r->status == MU_SAVE_OK || r->status == MU_SAVE_END; }
MU_INLINE mu_save_status mu_save_reader_status(const mu_save_reader* r) { return r->status; }
MU_INLINE uint32_t mu_save_file_size(const mu_save_reader* r) { return r->size; }
MU_INLINE uint32_t mu_save_chunk_count(const mu_save_reader* r) { return r->chunk_count; }
MU_INLINE uint32_t mu_save_format_id(const mu_save_reader* r) { return r->format_id; }

/* ----------- 6. migration ----------- */

/* One rung: read a payload written by `from_version`, write the same logical
 * data as `from_version + 1`. Steps are small on purpose (add a defaulted
 * field, change a unit, split a struct) and each gets its own gate case.
 *
 * `in` is a reader over the old payload (the ordinary read functions work, and
 * `in->chunk_version` / `in->chunk_id` are set). `out` is a payload writer. */
typedef mu_save_status (*mu_save_upgrade_fn)(mu_save_reader* in, mu_save_writer* out, void* user);

typedef struct mu_save_upgrade {
	uint32_t chunk_id;
	uint16_t from_version;
	mu_save_upgrade_fn fn;
} mu_save_upgrade;

/* Runs the ladder from `from_version` to `to_version`, ping-ponging between
 * `scratch_a` and `scratch_b` (both `scratch_cap` bytes).
 *
 * MU_SAVE_ERR_VERSION if a rung is missing -- never a silent half-upgrade.
 * MU_SAVE_ERR_FULL if a scratch buffer is too small. When from_version ==
 * to_version the input is returned unchanged with no copy. */
mu_save_status mu_save_upgrade_chain(const mu_save_upgrade* steps, uint32_t step_count,
									 uint32_t chunk_id, const uint8_t* payload, uint32_t size,
									 uint16_t from_version, uint16_t to_version,
									 uint8_t* scratch_a, uint8_t* scratch_b, uint32_t scratch_cap,
									 const uint8_t** out_payload, uint32_t* out_size,
									 uint16_t* out_version, void* user);

/* ----------- 7. file IO (optional) ----------- */
#ifdef MU_SAVE_WANT_STDIO
/* Writes to `path` atomically: temp file in place, fflush + fsync, rename.
 * A crash leaves either the old file or the new one, never a torn one.
 *
 * Windows: the CRT's rename() refuses to overwrite, so this falls back to
 * remove+rename there, which is NOT atomic. A shipping Windows build should
 * swap the rename for MoveFileExW(..., MOVEFILE_REPLACE_EXISTING). Documented
 * rather than #ifdef'd into a lie. */
mu_save_status mu_save_write_file_atomic(const char* path, const void* data, uint32_t size);

/* Reads a whole file into `dst` (capacity `cap`). *out_size gets the real file
 * size even when it did not fit, so a caller can report "needs N bytes". */
mu_save_status mu_save_read_file(const char* path, void* dst, uint32_t cap, uint32_t* out_size);

/* File size, or 0xFFFFFFFF on failure. */
uint32_t mu_save_file_size_on_disk(const char* path);
#endif /* MU_SAVE_WANT_STDIO */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MU_SAVE_H */

/* ======================================================================= */
/* implementation                                                          */
/* ======================================================================= */

#ifdef MU_SAVE_IMPLEMENTATION
#ifndef MU_SAVE_IMPL_ONCE
#define MU_SAVE_IMPL_ONCE

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------- crc32c ----------- */

/* Slicing-by-8: table 0 is the classic reflected CRC-32C table, tables 1..7 are
 * its successive 8-bit shifts, which lets the inner loop consume 8 bytes per
 * eight lookups instead of one. Built once, on first use. */
static uint32_t mu_save__crc_t[8][256];
static int mu_save__crc_ready = 0;

void mu_save_crc_tables_init(void) {
	uint32_t i, j, c;
	if (mu_save__crc_ready) return;
	for (i = 0; i < 256u; ++i) {
		c = i;
		for (j = 0; j < 8u; ++j) c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
		mu_save__crc_t[0][i] = c;
	}
	for (i = 0; i < 256u; ++i) {
		c = mu_save__crc_t[0][i];
		for (j = 1; j < 8u; ++j) {
			c = mu_save__crc_t[0][c & 0xFFu] ^ (c >> 8);
			mu_save__crc_t[j][i] = c;
		}
	}
	mu_save__crc_ready = 1;
}

uint32_t mu_save_crc32c_sw_extend(uint32_t crc, const void* data, size_t len) {
	const uint8_t* p = (const uint8_t*)data;
	mu_save_crc_tables_init();
	crc = ~crc;
	while (len >= 8u) {
		uint32_t lo = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
		lo ^= crc;
		crc = mu_save__crc_t[7][lo & 0xFFu] ^ mu_save__crc_t[6][(lo >> 8) & 0xFFu] ^
			  mu_save__crc_t[5][(lo >> 16) & 0xFFu] ^ mu_save__crc_t[4][(lo >> 24) & 0xFFu] ^
			  mu_save__crc_t[3][hi & 0xFFu] ^ mu_save__crc_t[2][(hi >> 8) & 0xFFu] ^
			  mu_save__crc_t[1][(hi >> 16) & 0xFFu] ^ mu_save__crc_t[0][(hi >> 24) & 0xFFu];
		p += 8;
		len -= 8u;
	}
	while (len--) crc = mu_save__crc_t[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
	return ~crc;
}

uint32_t mu_save_crc32c_sw(const void* data, size_t len) { return mu_save_crc32c_sw_extend(0u, data, len); }

#if MU_SAVE_HAS_CRC_HW
#include <nmmintrin.h>

/* SSE4.2 crc32 needs no table at all: 8 bytes per instruction. Compiled with a
 * target attribute so the rest of the header stays baseline-clean, and called
 * only after __builtin_cpu_supports("sse4.2"). */
__attribute__((target("sse4.2"))) uint32_t mu_save_crc32c_hw_extend(uint32_t crc, const void* data, size_t len) {
	const uint8_t* p = (const uint8_t*)data;
	/* The instruction does NOT complement for you: `crc32` in 64-bit mode
	 * computes the raw reflected CRC-32C with the given initial value, so the
	 * init/xorout convention lives here, exactly as it does in the software
	 * version. Forgetting the pair (which this header did on first compile) is a
	 * whole different function -- the gate caught it with the standard
	 * "123456789" vector. */
	unsigned long long c = (unsigned long long)~crc;

	while (len >= 8u) {
		unsigned long long v;
		memcpy(&v, p, 8);
		c = _mm_crc32_u64(c, v);
		p += 8;
		len -= 8u;
	}
	if (len >= 4u) {
		uint32_t v;
		memcpy(&v, p, 4);
		c = _mm_crc32_u32((unsigned int)c, v);
		p += 4;
		len -= 4u;
	}
	while (len--) c = _mm_crc32_u8((unsigned int)c, *p++);
	return ~(uint32_t)c;
}

__attribute__((target("sse4.2"))) uint32_t mu_save_crc32c_hw(const void* data, size_t len) {
	return mu_save_crc32c_hw_extend(0u, data, len);
}
#endif

uint32_t mu_save_crc32c(const void* data, size_t len) {
#if MU_SAVE_HAS_CRC_HW
	if (__builtin_cpu_supports("sse4.2")) return mu_save_crc32c_hw(data, len);
#endif
	return mu_save_crc32c_sw(data, len);
}

/* ----------- little-endian helpers ----------- */

static void mu_save__put_u16(uint8_t* p, uint16_t v) {
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}
static void mu_save__put_u32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static uint16_t mu_save__get_u16(const uint8_t* p) {
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t mu_save__get_u32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ----------- writer ----------- */

static void mu_save__fail(mu_save_writer* w, mu_save_status s) {
	if (!w->failed) {
		w->failed = 1;
		w->status = s;
	}
}

/* The only place the cursor moves forward. Returns NULL (and a sticky failure)
 * when the bytes do not fit. */
static uint8_t* mu_save__reserve(mu_save_writer* w, uint32_t n) {
	if (!w || w->failed) return NULL;
	if (n > w->cap || w->pos > w->cap - n) {
		mu_save__fail(w, MU_SAVE_ERR_FULL);
		return NULL;
	}
	{
		uint8_t* p = w->buf + w->pos;
		w->pos += n;
		return p;
	}
}

mu_save_status mu_save_writer_begin(mu_save_writer* w, void* buf, uint32_t cap, uint32_t format_id) {
	uint8_t* h;
	if (!w) return MU_SAVE_ERR_STATE;
	MU_MEMSET(w, 0, sizeof *w);
	w->buf = (uint8_t*)buf;
	w->cap = cap;
	w->format_id = format_id;
	w->status = MU_SAVE_OK;
	if (!buf || cap < MU_SAVE_HEADER_SIZE + MU_SAVE_FOOTER_SIZE) {
		mu_save__fail(w, MU_SAVE_ERR_FULL);
		return w->status;
	}
	h = mu_save__reserve(w, MU_SAVE_HEADER_SIZE);
	mu_save__put_u32(h, MU_SAVE_MAGIC);
	mu_save__put_u16(h + 4, MU_SAVE_FILE_VERSION);
	mu_save__put_u16(h + 6, 0u);
	mu_save__put_u32(h + 8, format_id);
	w->payload_base = MU_SAVE_HEADER_SIZE;
	return w->status;
}

mu_save_status mu_save_writer_begin_payload(mu_save_writer* w, void* buf, uint32_t cap) {
	if (!w) return MU_SAVE_ERR_STATE;
	MU_MEMSET(w, 0, sizeof *w);
	w->buf = (uint8_t*)buf;
	w->cap = cap;
	w->status = MU_SAVE_OK;
	w->raw = 1;
	w->payload_base = 0u;
	if (!buf) mu_save__fail(w, MU_SAVE_ERR_FULL);
	return w->status;
}

mu_save_status mu_save_chunk_begin(mu_save_writer* w, uint32_t chunk_id, uint16_t version) {
	uint8_t* p;
	if (!w) return MU_SAVE_ERR_STATE;
	if (w->failed) return w->status;
	if (w->raw || w->in_chunk || chunk_id == 0u) {
		/* 0 is reserved: it is what a zeroed buffer holds. */
		mu_save__fail(w, MU_SAVE_ERR_STATE);
		return w->status;
	}
	w->chunk_start = w->pos;
	w->chunk_id = chunk_id;
	w->chunk_version = version;
	p = mu_save__reserve(w, MU_SAVE_CHUNK_HEADER_SIZE);
	if (!p) return w->status;
	mu_save__put_u32(p, chunk_id);
	mu_save__put_u16(p + 4, version);
	mu_save__put_u16(p + 6, 0u);
	mu_save__put_u32(p + 8, 0u);  /* size: filled by chunk_end */
	mu_save__put_u32(p + 12, 0u); /* crc:  filled by chunk_end */
	w->payload_base = w->pos;
	w->in_chunk = 1;
	return w->status;
}

mu_save_status mu_save_chunk_end(mu_save_writer* w) {
	uint32_t payload_at, payload_size, pad, crc;
	uint8_t* p;

	if (!w) return MU_SAVE_ERR_STATE;
	if (w->failed) return w->status;
	if (!w->in_chunk) {
		mu_save__fail(w, MU_SAVE_ERR_STATE);
		return w->status;
	}
	payload_at = w->payload_base;
	payload_size = w->pos - payload_at;
	pad = (4u - (payload_size & 3u)) & 3u;
	if (pad) {
		p = mu_save__reserve(w, pad);
		if (!p) return w->status;
		MU_MEMSET(p, 0, pad);
	}
	crc = mu_save_crc32c(w->buf + payload_at, payload_size);
	p = w->buf + w->chunk_start;
	mu_save__put_u32(p + 8, payload_size);
	mu_save__put_u32(p + 12, crc);
	w->in_chunk = 0;
	++w->chunk_count;
	return w->status;
}

uint32_t mu_save_writer_commit(mu_save_writer* w) {
	uint8_t* f;
	if (!w || w->failed) return 0u;
	if (w->in_chunk) {
		mu_save__fail(w, MU_SAVE_ERR_STATE);
		return 0u;
	}
	if (w->raw) return w->pos;
	f = mu_save__reserve(w, MU_SAVE_FOOTER_SIZE);
	if (!f) return 0u;
	mu_save__put_u32(f, MU_SAVE_END_MAGIC);
	mu_save__put_u32(f + 4, w->chunk_count);
	mu_save__put_u32(f + 8, mu_save_crc32c(w->buf, w->pos - MU_SAVE_FOOTER_SIZE));
	return w->pos;
}

mu_save_status mu_save_u8(mu_save_writer* w, uint8_t v) {
	uint8_t* p = mu_save__reserve(w, 1u);
	if (!p) return w->status;
	p[0] = v;
	return MU_SAVE_OK;
}

mu_save_status mu_save_u16(mu_save_writer* w, uint16_t v) {
	uint8_t* p = mu_save__reserve(w, 2u);
	if (!p) return w->status;
	mu_save__put_u16(p, v);
	return MU_SAVE_OK;
}

mu_save_status mu_save_u32(mu_save_writer* w, uint32_t v) {
	uint8_t* p = mu_save__reserve(w, 4u);
	if (!p) return w->status;
	mu_save__put_u32(p, v);
	return MU_SAVE_OK;
}

mu_save_status mu_save_u64(mu_save_writer* w, uint64_t v) {
	uint8_t* p = mu_save__reserve(w, 8u);
	if (!p) return w->status;
	mu_save__put_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
	mu_save__put_u32(p + 4, (uint32_t)(v >> 32));
	return MU_SAVE_OK;
}

mu_save_status mu_save_i32(mu_save_writer* w, int32_t v) { return mu_save_u32(w, (uint32_t)v); }
mu_save_status mu_save_i64(mu_save_writer* w, int64_t v) { return mu_save_u64(w, (uint64_t)v); }

mu_save_status mu_save_bool(mu_save_writer* w, int v) { return mu_save_u8(w, v ? 1u : 0u); }

mu_save_status mu_save_f32(mu_save_writer* w, float v) {
	uint32_t bits;
	memcpy(&bits, &v, 4);
	return mu_save_u32(w, bits);
}

mu_save_status mu_save_f64(mu_save_writer* w, double v) {
	uint64_t bits;
	memcpy(&bits, &v, 8);
	return mu_save_u64(w, bits);
}

mu_save_status mu_save_vu64(mu_save_writer* w, uint64_t v) {
	uint8_t* p;
	uint32_t n = 1u, i;
	uint64_t t = v;
	while (t >= 0x80u) {
		t >>= 7;
		++n;
	}
	p = mu_save__reserve(w, n);
	if (!p) return w->status;
	for (i = 0; i < n; ++i) {
		uint8_t b = (uint8_t)(v & 0x7Fu);
		v >>= 7;
		if (i + 1u < n) b = (uint8_t)(b | 0x80u);
		p[i] = b;
	}
	return MU_SAVE_OK;
}

mu_save_status mu_save_vu32(mu_save_writer* w, uint32_t v) { return mu_save_vu64(w, (uint64_t)v); }

mu_save_status mu_save_vi64(mu_save_writer* w, int64_t v) {
	uint64_t z = ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); /* zigzag */
	return mu_save_vu64(w, z);
}

mu_save_status mu_save_vi32(mu_save_writer* w, int32_t v) {
	uint32_t z = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
	return mu_save_vu32(w, z);
}

mu_save_status mu_save_bytes(mu_save_writer* w, const void* data, uint32_t len) {
	uint8_t* p;
	mu_save_status s = mu_save_vu32(w, len);
	if (s != MU_SAVE_OK) return s;
	if (len == 0u) return MU_SAVE_OK;
	if (!data) {
		mu_save__fail(w, MU_SAVE_ERR_STATE);
		return w->status;
	}
	p = mu_save__reserve(w, len);
	if (!p) return w->status;
	MU_MEMCPY(p, data, len);
	return MU_SAVE_OK;
}

mu_save_status mu_save_str(mu_save_writer* w, const char* s) {
	uint32_t n = 0u;
	if (s)
		while (s[n]) ++n;
	return mu_save_bytes(w, s, n);
}

mu_save_status mu_save_align4(mu_save_writer* w) {
	uint32_t at, pad;
	uint8_t* p;
	if (!w) return MU_SAVE_ERR_STATE;
	if (w->failed) return w->status;
	at = w->pos - w->payload_base;
	pad = (4u - (at & 3u)) & 3u;
	if (!pad) return MU_SAVE_OK;
	p = mu_save__reserve(w, pad);
	if (!p) return w->status;
	MU_MEMSET(p, 0, pad);
	return MU_SAVE_OK;
}

/* ----------- reader ----------- */

static void mu_save__rfail(mu_save_reader* r, mu_save_status s) {
	if (!r->failed) {
		r->failed = 1;
		r->status = s;
	}
}

mu_save_status mu_save_open_ex(mu_save_reader* r, const void* buf, uint32_t size,
							   uint32_t expect_format_id, int verify) {
	const uint8_t* b = (const uint8_t*)buf;
	uint16_t file_version, flags;
	uint32_t count;

	if (!r) return MU_SAVE_ERR_STATE;
	MU_MEMSET(r, 0, sizeof *r);
	r->status = MU_SAVE_OK;

	if (!b || size < MU_SAVE_HEADER_SIZE + MU_SAVE_FOOTER_SIZE) {
		mu_save__rfail(r, MU_SAVE_ERR_TRUNCATED);
		return r->status;
	}
	if (mu_save__get_u32(b) != MU_SAVE_MAGIC) {
		mu_save__rfail(r, MU_SAVE_ERR_MAGIC);
		return r->status;
	}
	file_version = mu_save__get_u16(b + 4);
	flags = mu_save__get_u16(b + 6);
	if (file_version != MU_SAVE_FILE_VERSION || flags != 0u) {
		/* A future file version is a clean refusal, not a parse of garbage. */
		mu_save__rfail(r, MU_SAVE_ERR_VERSION);
		return r->status;
	}
	if (expect_format_id != MU_SAVE_NO_ID && mu_save__get_u32(b + 8) != expect_format_id) {
		mu_save__rfail(r, MU_SAVE_ERR_FORMAT);
		return r->status;
	}
	if (mu_save__get_u32(b + size - MU_SAVE_FOOTER_SIZE) != MU_SAVE_END_MAGIC) {
		mu_save__rfail(r, MU_SAVE_ERR_MAGIC);
		return r->status;
	}
	count = mu_save__get_u32(b + size - MU_SAVE_FOOTER_SIZE + 4);
	if (count > MU_SAVE_MAX_CHUNKS) {
		mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
		return r->status;
	}
	if (verify) {
		uint32_t want = mu_save__get_u32(b + size - MU_SAVE_FOOTER_SIZE + 8);
		if (mu_save_crc32c(b, size - MU_SAVE_FOOTER_SIZE) != want) {
			mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
			return r->status;
		}
	}

	r->buf = b;
	r->size = size;
	r->pos = MU_SAVE_HEADER_SIZE;
	r->file_version = file_version;
	r->format_id = mu_save__get_u32(b + 8);
	r->chunk_count = count;
	r->opened = 1;
	r->status = MU_SAVE_OK;
	return MU_SAVE_OK;
}

mu_save_status mu_save_open(mu_save_reader* r, const void* buf, uint32_t size, uint32_t expect_format_id) {
	return mu_save_open_ex(r, buf, size, expect_format_id, 1);
}

mu_save_status mu_save_reader_over_payload(mu_save_reader* r, const void* payload, uint32_t size) {
	if (!r) return MU_SAVE_ERR_STATE;
	MU_MEMSET(r, 0, sizeof *r);
	r->buf = (const uint8_t*)payload;
	r->chunk_payload = (const uint8_t*)payload; /* grab() reads from here */
	r->size = size;
	r->chunk_size = size;
	r->chunk_count = 1u;
	r->chunks_seen = 1u;
	r->payload_only = 1;
	r->opened = 1;
	r->status = MU_SAVE_OK;
	return MU_SAVE_OK;
}

static uint32_t mu_save__file_end(const mu_save_reader* r) { return r->size - MU_SAVE_FOOTER_SIZE; }

mu_save_status mu_save_next(mu_save_reader* r) {
	const uint8_t* b;
	uint32_t payload_at, padded_end;
	uint16_t version, flags;

	if (!r || !r->opened) return MU_SAVE_ERR_STATE;
	if (r->payload_only) {
		/* A bare payload has exactly one "chunk": the payload itself. */
		r->status = MU_SAVE_END;
		return MU_SAVE_END;
	}
	if (r->failed) return r->status;
	b = r->buf;

	/* Leaving a chunk partially consumed means the caller decoded the wrong
	 * layout; everything after it is noise, so say so instead of guessing. */
	if (r->chunk_payload && r->payload_read != r->chunk_size) {
		mu_save__rfail(r, MU_SAVE_ERR_STATE);
		return r->status;
	}

	if (r->pos == mu_save__file_end(r)) {
		/* The footer's chunk count has to agree with what we walked through.
		 * Without this, flipping a bit in the count field is invisible: the CRC
		 * covers everything *before* the footer, and nothing else compares the
		 * two numbers. The gate found exactly this hole. */
		if (r->chunks_seen != r->chunk_count) {
			mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
			return r->status;
		}
		r->status = MU_SAVE_END;
		return MU_SAVE_END;
	}
	if (r->pos > mu_save__file_end(r) || mu_save__file_end(r) - r->pos < MU_SAVE_CHUNK_HEADER_SIZE) {
		mu_save__rfail(r, MU_SAVE_ERR_TRUNCATED);
		return r->status;
	}

	r->chunk_start = r->pos;
	r->chunk_id = mu_save__get_u32(b + r->pos);
	version = mu_save__get_u16(b + r->pos + 4);
	flags = mu_save__get_u16(b + r->pos + 6);
	r->chunk_size = mu_save__get_u32(b + r->pos + 8);
	r->chunk_crc = mu_save__get_u32(b + r->pos + 12);
	r->chunk_version = version;
	r->chunk_flags = flags;
	r->payload_read = 0u;

	if (r->chunk_id == 0u || flags != 0u) {
		mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
		return r->status;
	}

	payload_at = r->pos + MU_SAVE_CHUNK_HEADER_SIZE;
	if (r->chunk_size > mu_save__file_end(r) - payload_at) {
		mu_save__rfail(r, MU_SAVE_ERR_TRUNCATED);
		return r->status;
	}
	padded_end = payload_at + r->chunk_size + ((4u - (r->chunk_size & 3u)) & 3u);
	if (padded_end > mu_save__file_end(r)) {
		mu_save__rfail(r, MU_SAVE_ERR_TRUNCATED);
		return r->status;
	}

	/* Per-chunk CRC: localises corruption to a chunk id ("chunk 0x4d4150 is
	 * bad") instead of "the file is bad somewhere", and it is cheap next to the
	 * decode that follows. */
	if (mu_save_crc32c(b + payload_at, r->chunk_size) != r->chunk_crc) {
		mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
		return r->status;
	}

	r->chunk_payload = b + payload_at;
	r->pos = padded_end;
	++r->chunks_seen;
	return MU_SAVE_OK;
}

mu_save_status mu_save_rewind(mu_save_reader* r) {
	if (!r || !r->opened) return MU_SAVE_ERR_STATE;
	r->pos = MU_SAVE_HEADER_SIZE;
	r->chunk_start = 0u;
	r->chunk_id = 0u;
	r->chunk_size = 0u;
	r->chunk_payload = r->payload_only ? r->buf : NULL;
	r->payload_read = 0u;
	r->chunks_seen = 0u;
	r->failed = 0;
	r->status = MU_SAVE_OK;
	return MU_SAVE_OK;
}

mu_save_status mu_save_skip_chunk(mu_save_reader* r) {
	if (!r || !r->opened) return MU_SAVE_ERR_STATE;
	if (r->failed) return r->status;
	if (!r->chunk_payload) return MU_SAVE_ERR_STATE;
	r->payload_read = r->chunk_size;
	return MU_SAVE_OK;
}

mu_save_status mu_save_find(mu_save_reader* r, uint32_t chunk_id) {
	mu_save_status s;
	if (!r || !r->opened) return MU_SAVE_ERR_STATE;
	s = mu_save_rewind(r);
	if (s != MU_SAVE_OK) return s;
	while ((s = mu_save_next(r)) == MU_SAVE_OK) {
		if (r->chunk_id == chunk_id) return MU_SAVE_OK;
		/* find() is a scan, not a decode: the chunk it just looked at is
		 * discarded rather than left half-read, which mu_save_next would (very
		 * correctly) refuse to skip past. */
		s = mu_save_skip_chunk(r);
		if (s != MU_SAVE_OK) return s;
	}
	return (s == MU_SAVE_END) ? MU_SAVE_ERR_STATE : s;
}

/* The only place payload_read moves forward, so a short chunk fails once. */
static int mu_save__take(mu_save_reader* r, uint32_t n) {
	if (!r || !r->opened) return 0;
	if (r->failed) return 0;
	if (n > r->chunk_size - r->payload_read) {
		mu_save__rfail(r, MU_SAVE_ERR_TRUNCATED);
		return 0;
	}
	return 1;
}

static const uint8_t* mu_save__grab(mu_save_reader* r, uint32_t n) {
	const uint8_t* p;
	if (!mu_save__take(r, n)) return NULL;
	p = r->chunk_payload + r->payload_read;
	r->payload_read += n;
	return p;
}

uint8_t mu_save_read_u8(mu_save_reader* r) {
	const uint8_t* p = mu_save__grab(r, 1u);
	return p ? p[0] : 0u;
}

uint16_t mu_save_read_u16(mu_save_reader* r) {
	const uint8_t* p = mu_save__grab(r, 2u);
	return p ? mu_save__get_u16(p) : 0u;
}

uint32_t mu_save_read_u32(mu_save_reader* r) {
	const uint8_t* p = mu_save__grab(r, 4u);
	return p ? mu_save__get_u32(p) : 0u;
}

uint64_t mu_save_read_u64(mu_save_reader* r) {
	const uint8_t* p = mu_save__grab(r, 8u);
	if (!p) return 0u;
	return (uint64_t)mu_save__get_u32(p) | ((uint64_t)mu_save__get_u32(p + 4) << 32);
}

int32_t mu_save_read_i32(mu_save_reader* r) { return (int32_t)mu_save_read_u32(r); }
int64_t mu_save_read_i64(mu_save_reader* r) { return (int64_t)mu_save_read_u64(r); }

int mu_save_read_bool(mu_save_reader* r) {
	uint8_t v = mu_save_read_u8(r);
	if (r->failed) return 0;
	if (v > 1u) { /* a bool that is 7 is corruption, not "true" */
		mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
		return 0;
	}
	return (int)v;
}

float mu_save_read_f32(mu_save_reader* r) {
	uint32_t bits = mu_save_read_u32(r);
	float v;
	memcpy(&v, &bits, 4);
	return v;
}

double mu_save_read_f64(mu_save_reader* r) {
	uint64_t bits = mu_save_read_u64(r);
	double v;
	memcpy(&v, &bits, 8);
	return v;
}

uint64_t mu_save_read_vu64(mu_save_reader* r) {
	uint64_t v = 0u;
	uint32_t shift = 0u, i;
	for (i = 0; i < 10u; ++i) {
		const uint8_t* p = mu_save__grab(r, 1u);
		uint8_t b;
		if (!p) return 0u;
		b = p[0];
		if (shift < 64u) v |= (uint64_t)(b & 0x7Fu) << shift;
		shift += 7u;
		if ((b & 0x80u) == 0u) {
			/* Non-canonical encodings are rejected: a longer-than-needed varint
			 * means two different byte strings decode to the same value, which
			 * breaks both "same state -> same bytes" and any delta scheme. */
			if (i > 0u && b == 0u) {
				mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
				return 0u;
			}
			return v;
		}
	}
	mu_save__rfail(r, MU_SAVE_ERR_CORRUPT); /* more than 10 groups */
	return 0u;
}

uint32_t mu_save_read_vu32(mu_save_reader* r) {
	uint64_t v = mu_save_read_vu64(r);
	if (r->failed) return 0u;
	if (v > 0xFFFFFFFFu) {
		mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
		return 0u;
	}
	return (uint32_t)v;
}

int64_t mu_save_read_vi64(mu_save_reader* r) {
	uint64_t z = mu_save_read_vu64(r);
	return (int64_t)((z >> 1) ^ (uint64_t)(-(int64_t)(z & 1u)));
}

int32_t mu_save_read_vi32(mu_save_reader* r) {
	uint32_t z = mu_save_read_vu32(r);
	if (r->failed) return 0;
	return (int32_t)((z >> 1) ^ (uint32_t)(-(int32_t)(z & 1u)));
}

uint32_t mu_save_read_bytes(mu_save_reader* r, void* dst, uint32_t cap) {
	uint32_t len = mu_save_read_vu32(r);
	const uint8_t* p;
	if (r->failed) return 0u;
	if (!mu_save__take(r, len)) return 0u;
	p = r->chunk_payload + r->payload_read;
	if (len > cap) {
		r->payload_read += len; /* consume anyway: iteration must stay in sync */
		mu_save__rfail(r, MU_SAVE_ERR_FULL);
		return 0u;
	}
	if (len && dst) MU_MEMCPY(dst, p, len);
	r->payload_read += len;
	return len;
}

mu_save_status mu_save_skip_bytes(mu_save_reader* r) {
	uint32_t len = mu_save_read_vu32(r);
	if (r->failed) return r->status;
	if (!mu_save__take(r, len)) return r->status;
	r->payload_read += len;
	return MU_SAVE_OK;
}

uint32_t mu_save_read_str(mu_save_reader* r, char* dst, uint32_t cap) {
	uint32_t len = mu_save_read_vu32(r);
	const uint8_t* p;
	uint32_t copy;
	if (r->failed) {
		if (cap) dst[0] = '\0';
		return 0u;
	}
	if (!mu_save__take(r, len)) {
		if (cap) dst[0] = '\0';
		return 0u;
	}
	p = r->chunk_payload + r->payload_read;
	copy = (cap > 0u) ? cap - 1u : 0u;
	if (copy > len) copy = len;
	if (copy) MU_MEMCPY(dst, p, copy);
	if (cap) dst[copy] = '\0';
	r->payload_read += len;
	return len;
}

uint32_t mu_save_read_align4(mu_save_reader* r) {
	uint32_t pad, i;
	const uint8_t* p;
	if (!r || !r->opened || r->failed) return 0u;
	pad = (4u - (r->payload_read & 3u)) & 3u;
	if (!pad) return 0u;
	if (!mu_save__take(r, pad)) return 0u;
	p = r->chunk_payload + r->payload_read;
	for (i = 0; i < pad; ++i) {
		if (p[i] != 0u) {
			mu_save__rfail(r, MU_SAVE_ERR_CORRUPT);
			return 0u;
		}
	}
	r->payload_read += pad;
	return pad;
}

/* ----------- migration ----------- */

mu_save_status mu_save_upgrade_chain(const mu_save_upgrade* steps, uint32_t step_count,
									 uint32_t chunk_id, const uint8_t* payload, uint32_t size,
									 uint16_t from_version, uint16_t to_version,
									 uint8_t* scratch_a, uint8_t* scratch_b, uint32_t scratch_cap,
									 const uint8_t** out_payload, uint32_t* out_size,
									 uint16_t* out_version, void* user) {
	const uint8_t* cur = payload;
	uint32_t cur_size = size;
	uint16_t cur_ver = from_version;
	uint8_t* dst = scratch_a;

	if (!out_payload || !out_size || !out_version) return MU_SAVE_ERR_STATE;
	if (from_version > to_version) return MU_SAVE_ERR_VERSION;
	if (from_version == to_version) {
		*out_payload = payload;
		*out_size = size;
		*out_version = from_version;
		return MU_SAVE_OK;
	}
	if (!cur && cur_size) return MU_SAVE_ERR_STATE;

	for (;;) {
		const mu_save_upgrade* step = NULL;
		mu_save_reader in;
		mu_save_writer out;
		mu_save_status s;
		uint32_t i;

		for (i = 0; i < step_count; ++i) {
			if (steps[i].chunk_id == chunk_id && steps[i].from_version == cur_ver) {
				step = &steps[i];
				break;
			}
		}
		if (!step) return MU_SAVE_ERR_VERSION; /* missing rung: refuse, don't guess */
		if (!dst) return MU_SAVE_ERR_STATE;

		mu_save_reader_over_payload(&in, cur, cur_size);
		in.chunk_id = chunk_id;
		in.chunk_version = cur_ver;
		s = mu_save_writer_begin_payload(&out, dst, scratch_cap);
		if (s != MU_SAVE_OK) return s;

		s = step->fn(&in, &out, user);
		if (s != MU_SAVE_OK) return s;
		if (out.failed) return out.status;
		if (in.failed) return in.status;
		if (in.payload_read != in.chunk_size) return MU_SAVE_ERR_STATE; /* step left bytes */

		cur = dst;
		cur_size = out.pos;
		++cur_ver;
		if (cur_ver == to_version) break;
		dst = (dst == scratch_a) ? scratch_b : scratch_a;
	}

	*out_payload = cur;
	*out_size = cur_size;
	*out_version = cur_ver;
	return MU_SAVE_OK;
}

/* ----------- file IO ----------- */
#ifdef MU_SAVE_WANT_STDIO
#include <stdio.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include <unistd.h> /* fsync, fileno: see mu_save_write_file_atomic */
#endif

uint32_t mu_save_file_size_on_disk(const char* path) {
	FILE* f;
	long n;
	if (!path) return 0xFFFFFFFFu;
	f = fopen(path, "rb");
	if (!f) return 0xFFFFFFFFu;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 0xFFFFFFFFu;
	}
	n = ftell(f);
	fclose(f);
	return (n < 0) ? 0xFFFFFFFFu : (uint32_t)n;
}

mu_save_status mu_save_read_file(const char* path, void* dst, uint32_t cap, uint32_t* out_size) {
	FILE* f;
	long n;
	uint32_t want, got;

	if (!path || (!dst && cap)) return MU_SAVE_ERR_STATE;
	f = fopen(path, "rb");
	if (!f) return MU_SAVE_ERR_IO;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return MU_SAVE_ERR_IO;
	}
	n = ftell(f);
	if (n < 0) {
		fclose(f);
		return MU_SAVE_ERR_IO;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return MU_SAVE_ERR_IO;
	}
	want = (uint32_t)n;
	if (out_size) *out_size = want;
	got = (want < cap) ? want : cap;
	if (got && fread(dst, 1, got, f) != got) {
		fclose(f);
		return MU_SAVE_ERR_IO;
	}
	fclose(f);
	return (want > got) ? MU_SAVE_ERR_FULL : MU_SAVE_OK;
}

mu_save_status mu_save_write_file_atomic(const char* path, const void* data, uint32_t size) {
	char tmp[1024];
	FILE* f;
	size_t plen;
	mu_save_status rc = MU_SAVE_OK;

	if (!path || (!data && size)) return MU_SAVE_ERR_STATE;
	plen = strlen(path);
	if (plen + 5u >= sizeof tmp) return MU_SAVE_ERR_IO;
	memcpy(tmp, path, plen);
	memcpy(tmp + plen, ".tmp", 5);

	f = fopen(tmp, "wb");
	if (!f) return MU_SAVE_ERR_IO;
	if (size && fwrite(data, 1, size, f) != size) rc = MU_SAVE_ERR_IO;
	if (fflush(f) != 0) rc = MU_SAVE_ERR_IO;
#ifndef _WIN32
	if (rc == MU_SAVE_OK) {
		/* fsync before the rename: without it a power cut can leave a file with
		 * the right name and no contents, which is worse than no file. */
		int fd = fileno(f);
		if (fd >= 0 && fsync(fd) != 0) rc = MU_SAVE_ERR_IO;
	}
#endif
	if (fclose(f) != 0) rc = MU_SAVE_ERR_IO;
	if (rc != MU_SAVE_OK) {
		remove(tmp);
		return rc;
	}
#ifdef _WIN32
	remove(path); /* rename() cannot replace on Windows: see the header comment */
#endif
	if (rename(tmp, path) != 0) {
		remove(tmp);
		return MU_SAVE_ERR_IO;
	}
	return MU_SAVE_OK;
}
#endif /* MU_SAVE_WANT_STDIO */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MU_SAVE_IMPL_ONCE */
#endif /* MU_SAVE_IMPLEMENTATION */
