# `mu/mu_save.h` — a save file that survives patches

```c
#define MU_SAVE_IMPLEMENTATION          /* one TU */
#define MU_SAVE_WANT_STDIO              /* optional: atomic file write */
#include "mu/mu_save.h"

/* ---- write ---- */
mu_save_writer w;
mu_save_writer_begin(&w, buf, buflen, 0x524F4755u /* your format id */);
mu_save_chunk_begin(&w, CH_MAP, 1 /* chunk schema version */);
    mu_save_vu32(&w, map_w);
    mu_save_vu32(&w, map_h);
    mu_save_bytes(&w, tiles, map_w * map_h);
mu_save_chunk_end(&w);
mu_save_chunk_begin(&w, CH_PLAYER, 3);
    mu_save_f32(&w, x);
    mu_save_f32(&w, y);
    mu_save_i32(&w, hp);
    mu_save_str(&w, name);
mu_save_chunk_end(&w);
mu_save_writer_commit(&w);              /* footer + CRCs filled in */

/* ---- read ---- */
mu_save_reader r;
if (mu_save_open(&r, buf, len, 0x524F4755u) != MU_SAVE_OK) {
    /* the file is not ours, is truncated, or is corrupt -- before you decoded
     * a single field, and without trusting a length */
}
while (mu_save_next(&r) == MU_SAVE_OK) {
    switch (r.chunk_id) {
    case CH_MAP:     ... break;
    case CH_PLAYER:  ... break;
    default:         mu_save_skip_chunk(&r); break;   /* newer than this build */
    }
}
```

## The four problems it solves

1. **Old saves load in new binaries.** Every chunk carries an id, a version and
   a length, so a reader skips what it does not recognise instead of dying on
   it. Adding a chunk in a patch is safe; removing one is safe for old files.
2. **Corruption is refused, not decoded.** CRC32C per chunk (which localises the
   failure to a chunk id) plus a whole-file CRC in the footer, checked by
   `mu_save_open` before anything is parsed.
3. **Schemas evolve one chunk at a time.** A chunk's version is independent of
   the file's, and `mu_save_upgrade_chain` runs v1→v2→v3 from a table of
   one-step functions through two caller-owned scratch buffers. A missing rung
   is an error, never a silent half-upgrade.
4. **A crash mid-save cannot eat the previous save.** `mu_save_write_file_atomic`
   writes a temp file, flushes and fsyncs it, then renames over the old one.

## The format

```
header   12 B   "MSV1", u16 file_version, u16 flags, u32 format_id
chunk*   16 B   u32 id, u16 version, u16 flags, u32 size, u32 crc32c + payload (4-aligned)
footer   12 B   "SEND", u32 chunk_count, u32 crc32c of everything before it
```

Little-endian, byte by byte, so the file is the same on every machine. There is
**no directory**: a chunk is skipped by `pos += 16 + size + pad`, and the footer
CRC validates the whole file up front — the two things a directory would buy,
for 16 bytes per chunk instead of a second table to keep in sync. (The roadmap
proposed a directory; this is the measured simplification. See
`PERF_SAVE.md` §3.)

## Value types

| call | wire form |
| --- | --- |
| `mu_save_u8/16/32/64`, `i32/64` | fixed width, little-endian |
| `mu_save_f32/f64` | exact bit pattern — NaN payloads survive |
| `mu_save_bool` | one byte, 0 or 1 (anything else is CORRUPT on read) |
| `mu_save_vu32/64`, `vi32/64` | LEB128 / zigzag+LEB128; rejection of non-canonical encodings |
| `mu_save_bytes` | varint length + bytes |
| `mu_save_str` | varint length + bytes, no terminator |
| `mu_save_align4` | zero padding to a 4-byte payload offset (verified on read) |

`mu_save_str`/`mu_save_bytes` are length-prefixed, not NUL-terminated: a save
that contains a string with an embedded NUL is legal here and is a truncated
file everywhere else.

## Failure behaviour, which is the point

* The writer **fails closed**: once a request does not fit, every later write is
  a no-op and `commit` returns 0. Not one byte past `cap` is touched.
* The reader **never reads past a chunk**. A short chunk sets
  `MU_SAVE_ERR_TRUNCATED` and every later read returns 0.
* Advancing past a chunk with bytes left is `MU_SAVE_ERR_STATE` — the caller
  decoded the wrong layout, and pretending otherwise corrupts the rest of the
  file silently.
* `mu_save_open` refuses: a wrong magic, a wrong `format_id`, a future
  `file_version`, a footer that does not match, a whole-file CRC mismatch, a
  chunk count larger than any file could hold.
* `mu_save_rewind` clears a failure, so a caller can retry after a bug fix.

## Migrating a chunk

```c
static mu_save_status v1_to_v2(mu_save_reader* in, mu_save_writer* out, void* user)
{
    int32_t hp = mu_save_read_i32(in);
    if (in->failed) return in->status;
    mu_save_i32(out, hp);
    mu_save_str(out, "unnamed");   /* the field v2 added, with a default */
    return out->status;
}

static const mu_save_upgrade ladder[] = {
    { CH_PLAYER, 1u, v1_to_v2 },
    { CH_PLAYER, 2u, v2_to_v3 },
};
mu_save_upgrade_chain(ladder, 2, chunk_id, payload, size, chunk_version, 3,
                      scratch_a, scratch_b, sizeof scratch_a,
                      &out_payload, &out_size, &out_version, NULL);
```

The gate proves the strong form: the migrated bytes are *identical* to what a
native v3 writer emits for the same content, not merely equivalent.

## Limits, stated

* **Not a struct serializer.** No reflection, no field registration, no
  zero-copy dump. You write the fields; that is what keeps padding, endianness
  and `float` bit patterns out of the format.
* **Two CRC passes on write.** A chunk CRC and a whole-file CRC. 1.6 µs of a
  5.1 µs save at 7 KiB; 0.26 ms on 1 MiB. Deliberate — see `PERF_SAVE.md` §3.
* **`mu_save_open` costs ~100 µs/MiB** (the whole-file CRC). `mu_save_open_ex(…,
  verify=0)` skips it and keeps the per-chunk guards.
* **Windows rename is not atomic** through this header (the CRT refuses to
  overwrite, so it falls back to remove+rename). Swap in `MoveFileExW` in a
  shipping build; the comment in the header says so.
* **No compression, no encryption, no signing.** Wrap the file in whatever
  container you like; the chunk CRCs and footer keep working inside it.
* **Single-threaded writer per buffer.** Two threads writing one `mu_save_writer`
  is a data race; two writers on two buffers is fine.

## Evidence

* `perf_test/PERF_SAVE.md` — seven fields, the per-piece decomposition (fields
  vs framing vs the two CRC passes), CRC table-vs-SSE4.2 at three sizes, the
  byte table, named losses.
* `make -C perf_test save-test` — 580 checks: golden wire bytes, every value
  type, **truncation at every byte offset**, **a bit flipped in every byte**,
  forward compatibility, the migration ladder, writer overflow with guard bytes,
  malformed values, reader call-order errors, and a roguelite cave that must
  resume into an identical future (same map, same FOV, same next 100 rolls).
* `make -C perf_test save-test-nohw` — the same gate with the SSE4.2 CRC
  removed; the digests must match, so the hardware path is an optimization and
  not a format change.
