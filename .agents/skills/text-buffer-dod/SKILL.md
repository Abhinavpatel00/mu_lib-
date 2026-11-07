---
name: text-buffer-dod
description: >
  Design, optimize and debug chunked text buffers — ropes, piece tables, gap
  buffers, segment arrays — for editors and any structure serving the six
  operations insert, erase, read_line, line_range, offset<->cursor and search.
  Use when the task involves a text editor buffer, rope, piece table, gap
  buffer, line indexing, newline summaries, undo journals, viewport reads,
  minified/long-line documents, or making keystroke-latency editing fast —
  even if the user does not name a data structure. Also use when an existing
  buffer is "fast on big files but slow on typing", or when line counts
  disagree with byte counts after an edit.
---

# Skill: chunked text buffers

Load `dod-performance` first — this skill is a specialization of it, not a
replacement. `AGENTS.md` remains the style contract.

## Routing

| Situation | Read |
|---|---|
| Choosing a structure, sizing chunks, or a buffer is slow on one workload but fast on another | `references/chunked-text-buffers.md` §1–§3 |
| Line counts wrong, bytes right, off-by-one after an edit | `references/chunked-text-buffers.md` §4 (the boundary invariant) |
| Building a differential test or a benchmark for a buffer | `references/chunked-text-buffers.md` §5–§6 |
| About to propose a "faster" layout | `references/chunked-text-buffers.md` §7 (measured dead ends) |

## The one thing to get right first

**A chunked buffer's edit cost is dominated by the bytes it copies inside a
chunk, not by the index it walks to find the chunk.** Size the chunk to the
document before tuning anything else.

Measured on real editing traces: a 16 KiB segment moved **1344.6 MB** to produce
a **102 KB** file — 5176 bytes per single-byte insert. Scaling the fill target
with document size cut that 7.5× and made editing **1.49× faster** with the
index completely untouched. No amount of index tuning recovers a 64× copy.

This is `AGENTS.md` §2.5 (bandwidth first) applied to a domain where it is
routinely forgotten, because the literature emphasizes the tree.

## Non-negotiables for this domain

1. **Measure bytes moved, not just nanoseconds.** Override the copy macro and
   count. It separates "the index is slow" from "the chunk is too big" in one
   run, and those have opposite fixes.
2. **State the boundary invariant explicitly and assert it in debug builds.**
   Every serious bug we found in `mu_ted` was a line start owned by the wrong
   segment. See §4.
3. **Benchmark the workload you actually have.** A typing trace and a
   bulk-load trace want different chunk sizes; a viewport walk and a
   whole-file scan want different summaries. One op mix hid a bug for three
   rounds.
4. **Test long lines separately.** Documents where one line spans many chunks
   are a distinct code path, and prose tests never reach it.
5. **Never claim a win against a crashing baseline.** Fix correctness first,
   then A/B.
