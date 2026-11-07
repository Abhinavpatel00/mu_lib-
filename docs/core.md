# Core: `mu_common.h`, `mu_allocators.h`, `mu_perf.h`, `mu_pcg.h`

Included by `mu.h`. These are the foundation everything else builds on.

---

## mu_common.h — macros, linkage, bit intrinsics

### Linkage model (`MU_API`)

| Define | Effect |
|---|---|
| *(nothing)* | `MU_API` = `extern` (default) |
| `MU_STATIC` | `MU_API` = `static` — stash everything in one TU |
| `MU_DLL_EXPORT` (Win32) | `__declspec(dllexport)` |
| `MU_DLL_IMPORT` (Win32) | `__declspec(dllimport)` |

C++ consumers get automatic `extern "C"` via `MU_BEGIN_EXTERN_C` /
`MU_END_EXTERN_C`.

### Allocation hooks

```c
#define mu_malloc(size) malloc(size)   /* override before including mu.h */
#define mu_calloc(count, size) calloc((count), (size))
#define mu_free(ptr)           free(ptr)
```

All core containers route through these. Redirect them to an arena, a tracked
allocator (`mu_mmgr.h`), or a debug heap without touching library code.

### Attribute / compiler portability macros

| Macro | MSVC | GCC/Clang | Fallback |
|---|---|---|---|
| `MU_INLINE` | `__forceinline` | `inline __attribute__((always_inline))` | `inline` |
| `MU_NOINLINE` | `__declspec(noinline)` | `__attribute__((noinline))` | — |
| `MU_RESTRICT` | `__restrict` | `restrict` (`__restrict__` in C++) | — |
| `MU_ALIGN(N)` | `__declspec(align(N))` | `__attribute__((aligned(N)))` | — |
| `MU_DEBUG_BREAK()` | `__debugbreak` | native `int3`/`brk`/`ebreak`/etc. | null deref |
| `MU_LIKELY(x)` / `MU_UNLIKELY(x)` | passthrough | `__builtin_expect` | passthrough |
| `MU_PREFETCH(addr)` | no-op | `__builtin_prefetch` | no-op |
| `MU_DEPRECATED(msg)` | `__declspec(deprecated)` | `__attribute__((deprecated))` | no-op |

`MU_STATIC_ASSERT(cond, msg)` maps to `_Static_assert` (C11), `static_assert`
(C++), or a typedef trick on ancient compilers.

### Useful macro toolbox

```c
MU_MIN(a, b)  MU_MAX(a, b)  MU_CLAMP(x, lo, hi)
MU_ARRAY_COUNT(a)            /* sizeof(a)/sizeof((a)[0]) — arrays only   */
MU_IS_POW2(x)  MU_BIT(n)
MU_KB(x) MU_MB(x) MU_GB(x)   /* unsigned long long byte literals         */
MU_CEIL(x, y) MU_FLOOR(x, y) /* integer div with rounding                */
MU_ALIGN_UP(x, a) MU_ALIGN_DOWN(x, a)
MU_HAS_FLAG / MU_SET_FLAG / MU_CLEAR_FLAG
MU_SWAP(TYPE, a, b)
MU_OFFSET_OF(type, member)   MU_CONTAINER_OF(ptr, type, member)
MU_ASSERT(x)                 /* debug-break on failure                   */
MU_PANIC()                   /* debug-break then forced crash            */
```

`MU_CONTAINER_OF` is the basis of the intrusive `mu_pool_link` lists: given a
pointer to an embedded link field, recover the enclosing struct.

`MU_DEBUG_BREAK()` emits a native breakpoint instruction (not a generic trap),
so debuggers stop with `SIGTRAP` and can continue normally. Prefer the
`mu_debug_break()` function from `mu_debug_break.h`; see
[debug-break.md](debug-break.md) for target behavior and caveats.

### Bit intrinsics (all `MU_INLINE`, branchless where possible)

```c
int      mu_trailing_zeroes_u64(uint64_t x);  /* CTZ; x==0 -> 64 */
uint32_t mu_leading_zeroes_u64(uint64_t x);   /* CLZ; x==0 -> 64 */
uint32_t mu_popcount_u64(uint64_t x);         /* POPCNT          */
```

- MSVC: `_BitScanForward64`/`_BitScanReverse64`/`__popcnt64` with 32-bit
  fallback paths for Win32.
- GCC/Clang: `__builtin_ctzll` / `__builtin_clzll` / `__builtin_popcountll`
  wrapped so `x == 0` returns 64 instead of UB.

> Compile with `-march=native -O3` and GCC/Clang will emit real `TZCNT`/`LZCNT`
> instead of the older `BSF`/`BSR` encodings.

Also provides legacy uppercase `KB(x) / MB(x) / GB(x)` and a `PAD(name, size)`
struct-padding macro.

---

## mu_allocators.h — tiny buffer allocators

Two stack/frame-friendly allocators over user-provided memory. No globals, no
system calls, no threadsafety guarantees (they're for per-frame or per-thread
use).

### `mu_buffer`

```c
typedef struct { uint8_t* memory; uint32_t size; } mu_buffer;
```

Plain (pointer, size) pair used by both allocators below.

### `mu_linear_allocator` — bump allocator

```c
mu_linear_init(a, memory, size);                  // bind to memory
void* p = mu_linear_alloc(a, size, align);        // NULL when exhausted
mu_linear_reset(a);                               // rewind to start
```

- Allocation is `head = align_up(head); head += size`.
- **No individual free.** Reclaim everything with `mu_linear_reset`.
- Ideal for per-frame scratch, load-time staging, one-shot serialization.

### `mu_ring_allocator` — ring allocator with partial free

```c
mu_ring_init(r, memory, size);
uint32_t offset;
void* p = mu_ring_alloc(r, size, align, &out_offset); // NULL when full
mu_ring_free_to(r, offset);                           // release up to offset
uint32_t used = mu_ring_used(r);                      // bytes in flight
```

- Two cursor model: `head` (bump point) and `tail` (oldest live byte).
- `mu_ring_alloc` fills forward when `head >= tail`; if it hits the end it
  wraps to 0 and fills only if it cannot catch `tail`.
- `mu_ring_free_to(r, some_old_offset)` invalidates everything from `tail` up
  to that offset — free allocations **in the order you made them** and this is
  a general-purpose FIFO frame allocator.
- Returns offsets (not just pointers) because offsets stay meaningful across
  wrap-around, and can be stored/serialized like handles.

---

## mu_perf.h — timing

```c
uint64_t now = mu_time_now();     /* platform ticks                     */
double   freq = mu_time_freq();   /* ticks per second (ns on POSIX)     */

MU_SCOPE_TIMER("gen_meshes");     /* prints "gen_meshes: 1.234 ms"      */
{ /* ... scoped work ... */ }
```

- Windows: `QueryPerformanceCounter` with a cached frequency.
- POSIX: `CLOCK_MONOTONIC`, frequency fixed at 1e9 (nanoseconds).
- `MU_SCOPE_TIMER` is a one-iteration `for` loop so it scopes cleanly without
  extra braces; prints to stdout on exit.

> Strict C99 on glibc needs `-D_POSIX_C_SOURCE=199309L` for
> `clock_gettime`/`CLOCK_MONOTONIC`.

---

## mu_pcg.h — PCG32 random

`rand()`-replacement scalar PCG (O'Neill variant, 64-bit state → 32-bit
output).

```c
mu_pcg32 rng;
mu_pcg32_init(&rng, /*seed*/ 0x853c49e6748fea9b, /*seq*/ 0xda3e39cb94b95bdb);
uint32_t r   = mu_pcg32_next_u32(&rng);
float    f01 = r * (1.0f / 4294967296.0f);   /* roll your own float maps */
```

- `seed`: starting state; `seq`: stream id — different `seq` values give
  independent sequences from the same seed.
- Two-step init: LCG advance, add seed, LCG advance (as recommended by the
  PCG paper so any seed/seq pair is a valid stream start).
- Output: xorshift-high + rotate, the standard PCG-XSH-RR output function.
- ~5 ns/number, passes TestU01 SmallCrush, tiny state (16 bytes).
- An AVX2 4-lane batch variant (`mu_pcg32x4`) is included but commented out —
  enable and adapt if your target guarantees AVX2.

For seeded procedural pipelines (noise, world gen) prefer this over
`mu_rog_rng` (same family but specialized to the roguelike header) and over
`rand()` (unspecified algorithm, global state).
