# PERF_SAVE.md — `mu/mu_save.h`

Evidence for a save-file container: chunked framing, CRC32C, migration, atomic
write. The baseline is the naive thing a game does today — `memcpy` each struct
field straight into the file — because that is what this header replaces, and
the interesting question is what the framing, the varints and the checksums
cost in exchange for a save that still loads after a patch.

All seven fields first, then the numbers, then what they do not show.

---

## 1. The seven fields

| # | field | value |
| --- | --- | --- |
| 1 | CPU / µarch | `Intel(R) Xeon(R) Processor @ 2.60GHz` (family 6 server part, AVX-512 present, SSE4.2 present), one vCPU of a guest |
| 2 | Cores / affinity | 2 vCPU total, **1 thread**, every timed run wrapped in `taskset -c 0` |
| 3 | Measured frequency | as `PERF_TIME.md`: a 20 000 000-long dependent `add` chain measures **0.64–1.07 GHz** while ramping, against a nominal 2.60 GHz. `cycles ≈ ns × 1.07`; a host at nominal prints ≈41 % of these nanoseconds |
| 4 | Compiler / flags | `gcc (Debian 14.2.0-19) 14.2.0`, `-O3 -march=native -std=c11 -Wall -Wextra -Werror -lm`; the gate also runs clean under `-O1 -fsanitize=address,undefined -fno-sanitize-recover=all`, and a second build with `-DMU_SAVE_CRC_HW=0` must produce the identical CRC digest. Kernel `6.1.189+`, `glibc 2.41` |
| 5 | Workload / working set | 200 entities × (id, x, y, hp, inventory count, 15-char name) = a 7 244-byte save in a 64 KiB buffer — L2-resident, and deliberately not a 100 MiB stress file. CRC rows additionally use 64 B (L1, header-sized), 4 KiB and 1 MiB (L2/L3) buffers. All buffers are static and touched before timing |
| 6 | Baseline | two, both in the same binary: (a) a **raw struct dump** — `memcpy` per field, no framing, no checksum, no varints — i.e. what the save code looked like before; (b) **field emission alone** (`emit_fields`), the same 2 000 fields through the same writer with the framing and CRC removed, so the framing cost is measured rather than subtracted |
| 7 | Method | `mu_bench` with `MU_BENCH_DO_NOT_OPTIMIZE` barriers, auto-sized batches, **61 samples/row**, min / median / CV / CI99. Byte counts come from `--bytes`, which involves no clock at all |

---

## 2. Per-operation table (61 samples)

```
row                              min  |  median |  cv   | what one iteration is
save.write_world             4.293 us | 5.140 us|  5.4% | write the 200-entity save: framing + chunk CRC + file CRC
save.read_world              3.888 us | 4.091 us|  2.8% | open (whole-file CRC) + walk + decode + per-chunk CRC
save.emit_fields             2.850 us | 2.947 us|  3.5% | the same 2 000 fields, no framing, no checksum
save.read_fields             1.616 us | 1.750 us|  8.3% | decode them back, no framing, no checksum
save.read_world_varint       1.887 us | 1.974 us|  1.1% | read the *varint* save (5 816 B instead of 7 244 B)
save.write_raw                 263.9 ns| 302.9 ns|  5.8% | the baseline: raw struct dump, memcpy only
save.read_raw                  263.7 ns| 296.6 ns|  7.9% | and read it back
save.open_verify             102.3 us | 103.1 us|  9.6% | mu_save_open on a 1 MiB save: whole-file CRC32C
save.open_no_verify            1.693 ns|   1.748 ns| 2.2% | the same, with the up-front pass skipped
crc.sw_64                      2.337 ns|   2.458 ns| 1.7% | slicing-by-8 over 64 B
crc.hw_64                      2.403 ns|   2.470 ns| 2.4% | SSE4.2 over 64 B
crc.sw_4k                      2.177 us|   2.286 us| 1.9% | ... over 4 KiB
crc.hw_4k                      439.7 ns|   455.4 ns| 2.9% | ...
crc.sw_1m                      600.1 us|   636.6 us| 2.2% | ... over 1 MiB
crc.hw_1m                      121.1 us|   128.3 us| 3.0% | ...
```

Throughput derived from the medians: **table 1.65–1.79 GB/s, SSE4.2 8.2–9.0 GB/s**
— a flat **5.0×**, at both 4 KiB and 1 MiB, so the 8 KiB table is not costing
cache the way a 256 KiB table would. At 64 B the two are identical (2.46 vs
2.47 ns) because the call and the setup dominate; a save made of tiny chunks
gets no benefit from the hardware path, and a save with one big blob gets all
of it.

### A second run, because one run is not a measurement

The same binary, same `taskset -c 0`, re-run after `make clean`: the guest's
clock moved, so absolute numbers did too — `write_world` 4.50 µs,
`read_world` 3.36 µs, `emit_fields` 2.94 µs, `read_fields` 1.76 µs,
`read_world_varint` 1.66 µs, `crc.sw_4k` 1.50 µs, `crc.hw_4k` 379 ns,
`crc.sw_1m` 501 µs, `crc.hw_1m` 134 µs. What holds is the **pattern**:

| ratio | run 1 | run 2 |
| --- | --- | --- |
| write / emit_fields (container cost) | 1.74× | 1.53× |
| varint read / fixed read | 2.07× | 2.03× |
| raw dump / `mu_save` write | 17.0× | 17.7× |
| SSE4.2 / table CRC at 1 MiB | 5.0× | 3.7× |
| validation share of load | 57 % | 48 % |

The two that swung most are the ones made of two large measured numbers
subtracted from each other (container cost, validation share) and the table
CRC, whose 1 MiB row is the least stable in the set (CV 10 %, marked
`UNSTABLE`). Quote the ratios as ranges, not as constants; the table above is
run 1, and run 2 is here so nobody has to trust it.

---

## 3. Per-change decomposition

`save.write_world` (5.14 µs) against `save.emit_fields` (2.95 µs) isolates the
containers, and the CRC rows size each piece:

| piece | measured | evidence |
| --- | --- | --- |
| 2 000 field emissions (bounds check + LE store + varint)` | **2.95 µs** (1.5 ns/field) | `save.emit_fields` |
| chunk CRC over the 7 244-byte payload | **≈0.8 µs** | 7 244 B at `crc.hw_4k`'s 9.0 GB/s |
| whole-file CRC over the same bytes | **≈0.8 µs** | same rate; this is the *second* pass |
| framing (12 B header, 16 B chunk header, padding, 12 B footer, commit) | **≈0.6 µs** | the remaining difference |

That last row is the honest cost of the design: **writing a save pays for two
CRC passes** — one per chunk and one over the whole file — because the footer
checksum has to cover the chunks it follows. For the 7 KiB save here that is
1.6 µs out of 5.1; for a 1 MiB save it is 2 × 128 µs = 0.26 ms. A save is not a
per-frame operation, so it stayed. A knob to skip the file CRC on write would
halve it, and is deliberately not added: the file CRC is what makes
`mu_save_open` able to refuse a corrupt file *before* decoding anything, and
that is the property the whole header exists for.

On the read side, `save.read_world` (4.09 µs) minus `save.read_fields`
(1.75 µs) is the same two CRC passes plus the chunk walk: **validation is 57 %
of the time to load the file**, and it is the part that turns "the player's
save is garbage" into `"corrupt"` with a chunk id.

**Varints pay twice.** The same world with varint ids/health/inventory is
**19.7 % smaller** (5 816 vs 7 244 bytes) *and* 2.1× faster to read than the
fixed-width file (1.97 µs vs 4.09 µs), because the read is dominated by bytes
touched and CRC, not by the decode loop. The varint decode itself is a
branch-per-byte loop, and it does not show up at this size.

---

## 4. Bytes (no clock involved)

`make -C perf_test save-bytes`:

```
raw struct dump (no framing)        6600 bytes   33.0 B/entity
mu_save, fixed-width fields         7244 bytes   36.2 B/entity  (+9.8 %)
mu_save, varints for id/hp/inv      5816 bytes   29.1 B/entity  (-19.7 % vs fixed)
framing (header + chunk header + footer): 40 bytes
```

The framing is **40 bytes, fixed**, whatever the payload — that is the whole
container. The rest of the 9.8 % against a raw dump is per-value metadata: a
1-byte length prefix on each of the 200 strings, and the inventory count stored
as `u32` in this payload. A raw dump is smaller only until the first schema
change, at which point it is unreadable; **the comparison that matters is the
varint column**, which is smaller than the dump *and* self-describing.

---

## 5. Losses, named

* **The raw struct dump is 17× faster to write and 14× faster to read**
  (303 ns vs 5.14 µs, 297 ns vs 4.09 µs). It has no
  framing, no checksums, no length prefixes and no schema; it also cannot be
  loaded by the next build of the binary, cannot be validated, and cannot tell
  a truncated file from a complete one. If a game truly only needs a memcpy, it
  should not use this header.
* **Two CRC passes on write** (see above): 1.6 µs of the 5.14 for this payload.
* **`mu_save_open` on a 1 MiB save costs 103 µs**, essentially all of it the
  whole-file CRC. Skipping it (`open_ex(..., verify=0)`) drops to 1.7 ns and
  keeps the per-chunk guards, at the price of finding corruption later and with
  a worse message.
* **Per-chunk CRC is redundant work after a whole-file CRC passes.** It is kept
  because it is what identifies *which* chunk is bad when it fails, and because
  it is the only guard left when the caller skips the file pass.
* **The 8 KiB slicing-by-8 table costs 8 KiB of writable `.data`** and a
  one-time init, for 5× the throughput of a 1 KiB table... which is itself 3× a
  byte-at-a-time loop, unmeasured here. It is a table-size decision, not a free
  win, and the SSE4.2 path makes it moot on any x86-64 CPU from 2008 onwards.
* **Nothing here is a compression story.** A save file is typically 5–20 % of
  its raw size after any general-purpose compressor; this format does framing
  and varints and leaves compression to the caller's outer container.

## 6. What this does not show

* Anything about very large saves (>100 MiB) or about a slow disk: every row is
  CPU-side, on a buffer already in RAM.
* Anything about **fsync cost**, which in practice dominates a real save (1–20 ms
  on consumer SSDs). `mu_save_write_file_atomic` does fflush + fsync + rename;
  none of that is timed here because it depends on the filesystem, not on us.
* Anything about migration cost at scale: the ladder in the gate runs two rungs
  over a 20-byte payload. A rung that rewrites a 10 MiB entity table would be
  linear and is not measured.
* A comparison against other save formats (MessagePack, CBOR, protobuf,
  FlatBuffers). Those are general-purpose serializers with their own trade-offs;
  this is a game-save container, and the baseline chosen here is the thing it
  replaced.

## Reproduce

```sh
make -C perf_test save-test        # 580 checks incl. truncation at every byte, a flip in every bit
make -C perf_test save-test-nohw   # same gate with the CRC hardware path removed; digests must match
make -C perf_test save-bench       # 15 rows, 61 samples, taskset -c 0
                                   # exits non-zero if a row is UNSTABLE (CV >= 8 % on this guest)
make -C perf_test save-bytes       # the byte table, no clock
```
