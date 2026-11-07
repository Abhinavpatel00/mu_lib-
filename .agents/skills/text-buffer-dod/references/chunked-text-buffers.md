# Chunked text buffers — design, sizing, invariants, testing

Evidence base: `mu_ted` (two-level segment array) and `mu_loom` (B+tree,
fanout 16) in `mylib/mu/`, seven optimization rounds, and an external
head-to-head against Ropey, JumpRope, Crop and rune's gap buffer on
[rope-benches](https://github.com/josephg/rope-benches). Numbers below are
measured on this project, not quoted.

---

## 1. The six operations, and why frequency order matters

An editor buffer serves, in rough frequency order:

| Op | What it really costs |
|---|---|
| `insert` / `erase` | copy inside one chunk + index fixup + undo record |
| `read_line` / `line_range` | descend to a line, then copy out — **the hottest op in real integration** |
| `offset ↔ cursor` | two aggregate lookups (bytes and line count) |
| viewport walk | 20–60 consecutive `read_line`s |
| `search` | scan, usually with a line-break summary to skip |

VS Code's 2018 piece-tree write-up reports `getLineContent` was the **hottest
method in real integration** — not insert, delete or search. Design the read
path first; it is the one that runs 40× per frame.

Corollary: a structure can win every edit benchmark and still feel slow. Our
internal head-to-head had `mu_loom` winning viewport walks 1.5× while losing
7 of 9 ops overall.

## 2. Chunk size is the dominant term

Copy volume per single-byte insert ≈ half the chunk size. With chunk `C` and
`P` patches, total bytes moved ≈ `P·C/2` — **independent of the final document
size**. For a typing trace that is catastrophic:

| trace | patches | final size | bytes moved, C = 16 KiB | B/patch |
|---|---|---|---|---|
| automerge-paper | 259 778 | 102 KB | 1344.6 MB | 5176 |
| seph-blog1 | 137 993 | 48 KB | 730.9 MB | 5297 |
| rustcode | 40 173 | 64 KB | 265.7 MB | 6615 |
| sveltecomponent | 19 749 | 18 KB | 69.3 MB | 3509 |

Crop and JumpRope use ~256 B chunks and move ~128 B/patch. **That, not the
B-tree, was the entire gap.**

### Soft target, not hard chunk size

Keep the allocation and the offset integer width fixed; make only the *fill
target* adaptive:

```c
target = clamp(text_len / MIN_SEGS, SEG_MIN, SEG_TEXT)
```

`mu_ted` uses `clamp(text_len/32, 2048, 16384)`. Properties worth copying:

* **Above `MIN_SEGS × SEG_TEXT` the target saturates**, so large documents take
  provably identical decisions. Ours saturates at 512 KB. This turns "we
  measured no regression on big files" into "big files cannot regress".
* Only insert's split trigger and erase's merge condition consult it. The
  index, the u16 in-segment line offsets and the allocation are untouched.
* A compile-time switch back to the fixed target stays in the test suite as a
  control, so the old path never rots.

Measured: **1.492× geomean** over five traces, 21 paired reps, no trace
regressing; bytes moved down 4.7–8.9×. Large-doc control at 8 MiB: queries
0.99–1.01×, `type_undo` 1.85×, `range_edit` 1.00×.

### Do not overshoot

Swept `MIN_SEGS ∈ {32,64,128,256}` × `SEG_MIN ∈ {256,512,1024,2048}`.
`SEG_MIN ≤ 512` **regressed** the smallest trace to 0.90×: below ~2 KB the
per-chunk boundary and index work costs more than the copy it saves. There is
an optimum, and it is not "as small as possible". Pick a plateau, not a knife
edge — short traces run at ±20 % CV.

### Instrument it

Override the copy macro (`MU_MEMMOVE` is `#ifndef`-guarded) and count bytes.
One run separates "index too slow" from "chunk too big". Those have opposite
fixes and identical wall-clock symptoms.

## 3. Summaries: what to cache per node

Cache **subtree-relative** aggregates, never absolute keys — absolute keys
force O(N) fixup on every edit.

Per node, the useful set is: byte length, line-break count, and (for the read
path) enough line-start information to answer `read_line` without scanning.
Zed's SumTree caches UTF-8 boundaries, UTF-16 length, newlines and tabs per
chunk; VS Code's piece tree caches `lineStarts[]` plus left-subtree byte and LF
counts.

Two measured lessons:

* Holding **references** into a buffer's line-break array instead of copying it
  made VS Code's ops 3× faster. Copy the summary, not the text.
* A separate SoA "hot field" mirror beside an AoS chunk header **lost** here
  (0.84–0.90×, five variants). The header already shares a cache line with the
  line-offset array the descent reads. See §7.

## 4. The boundary invariant — where every real bug lived

Seven of the eight `mu_ted` bugs we fixed were line-accounting bugs, and the
bytes were always correct. State the invariant explicitly:

> Every `\n` at local offset `q` owns a line start at `q+1` **in the same
> chunk**, unless `q+1 == nbytes` and a live successor owns it as `offs[0]==0`.

Every operation that moves a chunk boundary must preserve it. The failure
modes we actually hit:

| Bug | Shape |
|---|---|
| erase at a chunk endpoint | whole-chunk erase drops the endpoint-owned line start |
| insert at `p == nbytes`, chunk ends with `\n` | the start was handed to the successor; inserting makes it interior again, and `fix_boundary` then deletes the successor's copy — gone from both |
| split cut lands exactly on the insert point | split gives the start at `cut` to the right piece, but the insert loop only hops right when `p > cut`, so it keeps writing left |
| erase removes the last live chunk | the predecessor's phantom start was trimmed earlier and nothing restores it |
| continuation accounting | chunk smaller than one line ⇒ `cont` flag and leading-start accounting must agree |

**Diagnostic signature:** `text_len` correct, `line_count` off by one, and
every later line mis-numbered. When you see that, do not look at the copy
code — look at who owns the boundary start.

**Method that found them:** dump the offending chunk's `offs[]` next to the
flat model's starts over the same byte range. The missing entry is visible
immediately. Guessing from the symptom cost two wrong hypotheses.

**A latent bug becomes visible when you change chunk size.** Bug #8 existed
with fixed 16 KiB chunks — a 5120 B document with 64 B lines and a 20000 B
paste at 2624 reproduces it — but a 16 KiB slot swallowed almost every paste
whole, so it almost never fired. Smaller chunks made it routine. **When a
sizing change "causes" a correctness failure, first check whether it merely
exposed one.**

## 5. Testing

* **Differential against a flat model**, checking `line_count`, `text_len`,
  every line offset, and line text — not just the final string. Bytes can be
  perfect while the line index is wrong.
* **Five document flavours minimum**: short lines, very short/empty lines,
  **long spanning lines** (period 5–25 KB, so one line crosses many chunks),
  mixed, and CRLF if supported. Flavour 2 caught what the others missed.
* **Sweep ≥30 seeds.** An 8-seed sanitizer run missed two latent boundary bugs
  that a 30-seed sweep found immediately.
* **Long-line capacity probe.** `mu_loom` still refuses inserts into a single
  line spanning ≥64 leaves (995328 B) — invisible to prose tests, fatal for
  minified JSON/JS. Fails cleanly, which is why it survived.
* **Whole-chunk erase.** Random insert/erase produces almost no tombstones;
  you must erase whole chunks to exercise compaction.
* **Exhaustive small cases.** All insert/erase sequences up to n=11 is 229 374
  cases and runs in seconds.
* **ASan + UBSan**, plus a no-SIMD build, plus the fixed-chunk-size control.

## 6. Benchmarking

* **Replay real traces**, not synthetic op mixes. Export the trace, verify the
  result against the recorded `endContent` byte-for-byte, then time it.
* **Report ns/patch** for traces and ns/op for micro-ops; geomean for ratios;
  ≥21 paired randomized round-robin reps; min for latency floors; CV alongside
  (`AGENTS.md` §2.6.3).
* **A/B against a fixed baseline include tree**, never against a rebuild of
  the same header. Keep the pre-change header in a separate include root.
* **Profile with the real op mix.** A harness at 55 % undo versus a benchmark
  at 2–5 % undo ranks structures differently.
* **Check the viewport walk reaches the last line.** Ours started in
  `[0, nlines-40)` and never touched the final line, which is why a last-line
  bug survived three rounds.
* **Exclude degenerate rows** when ranking against other implementations:
  `from_string` where the competitor just clones a `String` is a memcpy
  baseline; a "search" benchmark whose entry point is insert+delete is not a
  search.

## 7. Measured dead ends — do not retry

| Tried | Result |
|---|---|
| SoA hot-field mirror beside AoS chunks (5 variants, incl. maintain-but-never-read control) | 0.84–0.90×. Control still lost ⇒ write-side rebuild is the cost. |
| Fanout 32 index | Lost |
| Whole-region balanced chunking | 8955 vs 4272 ns greedy. Balance only the 2-leaf split case. |
| Branchless leaf search | No effect |
| Chunk target ≤512 B | Regresses small documents to 0.90× |

## 8. Known limits of the structures here

* `mu_ted` needs ~3–4× document reserve or `insert` returns false.
* `mu_ted` undo is an unbounded journal; `mu_ted_redo` does not exist.
* `mu_loom` refuses single-line inserts spanning ≥64 leaves (open).
* Neither indexes codepoints, so neither is comparable on Unicode
  cursor benchmarks without adding that summary.
* `search_linewise` is 3.6× behind JumpRope — a missing line-break summary,
  not a chunk-size problem.
