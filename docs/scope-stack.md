# `mu_scope_stack.h` - scoped linear allocator with destructors

graphitemaster's [scope_stack_alloc](https://github.com/graphitemaster/scope_stack_alloc)
in C. The C++ original gets automatic rewind from RAII; C gets it from three
things instead:

1. **`__attribute__((cleanup(fn)))`** - a real scope-exit hook that runs on
   `return`, `break`, `goto` and fallthrough. This is what makes `MU_SCOPE_LIFETIME`
   automatic.
2. **Function pointers stored beside the allocation** - the destructor. A linear
   allocator cannot otherwise know whether the bytes it is about to reclaim held
   a `FILE*` or an `int`.
3. **Frame indices** - because `longjmp` skips every cleanup attribute, the
   stack must be purgeable by index after a longjmp.

```c
static char arena_mem[1 << 16];
mu_scope_stack arena;
mu_scope_init(&arena, arena_mem, sizeof arena_mem);

void update(void)
{
    MU_SCOPE_LIFETIME(&arena);                 /* rewinds on every exit path */

    float *scratch = mu_scope_array(&arena, float, 4096);
    char  *name    = mu_scope_strdup(&arena, "frame");
    FILE  *f       = mu_scope_own(&arena, fopen("x", "rb"), fclose_dtor);
    /* ... */
}                                              /* all of it released here */
```

## API

| function | cost | notes |
| --- | --- | --- |
| `mu_scope_init(ss, mem, bytes)` | O(1) | caller owns the memory; no allocation ever |
| `mu_scope_alloc(ss, bytes)` / `mu_scope_array` | pointer add + bounds check | 16 B aligned by default; **NULL when full** |
| `mu_scope_alloc_aligned(ss, bytes, align)` | same | power-of-two alignments only |
| `mu_scope_strdup(ss, s)` | O(len) | |
| `mu_scope_register(ss, ptr, dtor, user)` | O(1) | adopt a foreign allocation with a destructor |
| `mu_scope_enter(ss)` / `mu_scope_leave(ss, f)` | O(1) / O(#dtors) | frames, may nest |
| `MU_SCOPE_LIFETIME(ss)` | O(1) | automatic rewind via `cleanup` |
| `mu_scope_purge_to(ss, f)` | O(n) | the `longjmp` path |
| `mu_scope_peak` / `mu_scope_overflow_count` | O(1) | sizing and diagnostics |

## The rules (inherited from the original)

* Allocations are **LIFO**. Rewinding a frame invalidates every pointer acquired
  after it - that is the trade: one pointer store frees the whole frame.
* A pointer acquired here must not escape the frame that acquired it.
* Only the top frame can be allocated from.
* A refusal is a `NULL`, never a wrap: an arena that runs out is a fallback
  decision, not a crash. `mu_scope_overflow_count` tells you it happened.

## Destructors

Registered actions run **newest-first** when the frame is left, and an inner
frame never runs an outer frame's destructors. `test_scope_stack.c` pins both
properties. On the `longjmp` path nothing runs until `mu_scope_purge_to`, which
the gate also asserts - including that the skipped destructors *did* not run,
so the test documents the hazard rather than wishing it away.

## Where this pays

Purely local, throwaway data with a common lifetime: scratch buffers inside one
frame, per-frame parse trees, temporary string assembly, per-task state in a job
system. Allocation is a bump and release is `top = saved`, so a frame that runs
thousands of times a second never touches the allocator, and everything in it is
adjacent in memory.

## Gate

`perf_test/test_scope_stack.c` - 36 checks: LIFO reuse (same addresses each
iteration, peak of one frame not three), alignment, destructor order and frame
isolation, refusal without corrupting the bump pointer, `strdup`, and the
`longjmp` + purge sequence.
