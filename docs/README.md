# `mu` Library Documentation

C99-first collection of data structures, allocators, math, ECS, and concurrency
primitives for engine and tools code. One umbrella header for the core:

```c
#include "mu.h"   /* core: containers, allocators, bitsets, spans, RNG, perf */
```

Subsystems ship as their own single headers:

```c
#include "mu/mu_ecs.h"               /* entities, pools, transforms, prefabs      */
#include "mu/mu_thread_system.h"     /* worker pool + atomics + primitives        */
#include "mu/mu_task_scheduler.h"    /* priorities, dependencies, parallel_for    */
#include "mu/mu_geometric_algebra.h" /* PGA / conformal math (requires cglm)      */
#include "mu/mu_noise_math.h"        /* 1D-4D noise, cellular, fBm, warp, curl    */
#include "mu/mu_roguelike.h"         /* grid gen, FOV, pathing, schedulers        */
#include "mu/allocators.h"           /* arena, pool, buddy                        */
#include "mu/offset_allocator.h"     /* two-level bin free-space allocator        */
#include "mu/mu_mmgr.h"              /* leak/corruption tracking malloc shim      */
#include "mu/mu_min_containers.h"    /* standalone stretchy buffer + hash64       */
#include "mu/mu_skiplist.h"          /* intrusive skip list with widths           */
#include "mu/mu_dlist.h"             /* intrusive doubly-linked list             */
#include "mu/mu_scope_stack.h"       /* scoped linear allocator + destructors     */
#include "mu/mu_log.h"               /* flexible logging, zero allocation         */
#include "mu/mu_bench.h"             /* benchmark harness (one TU per binary)     */
#include "mu/mu_shapes.h"            /* parametric shapes, SDF bake, L-systems    */
#include "mu/mu_easing.h"            /* 33 curves, derivatives, Bezier, LUT       */
```

## Documentation index

| Doc | Contents |
|-----|----------|
| [core.md](core.md) | `mu/mu_common.h` macros, bit intrinsics, `mu/mu_allocators.h` (linear/ring), `mu/mu_perf.h`, `mu/mu_pcg.h` |
| [intrusive.md](intrusive.md) | `mu/mu_dlist.h`: intrusive doubly-linked list, the AGENTS.md 3.15 rule, and the measured intrusive-vs-node-owning numbers |
| [skiplist.md](skiplist.md) | `mu/mu_skiplist.h`: intrusive skip list with widths (order statistics), pooled recycled level blocks, embedded nodes |
| [bench.md](bench.md) | `mu/mu_bench.h`: ubench-style micro-benchmark harness, the input-operand barrier, auto-sized batches, 99% CI, CSV |
| [log.md](log.md) | `mu/mu_log.h`: levels, per-category thresholds, 4 sink kinds, ring buffer, structured kv, compile-time strip |
| [scope-stack.md](scope-stack.md) | `mu/mu_scope_stack.h`: scoped linear allocator with destructors, `cleanup`-based auto rewind, longjmp purge |
| [easing.md](easing.md) | `mu/mu_easing.h`: 33 Penner curves (par\_easings.h extended and corrected), curves as data, analytic velocities, parametric back/elastic, CSS cubic-bezier, Fritsch-Carlson monotone spline, CSS steps, baked LUTs; measured vs `par_easings.h` (geomean 1.27x) |
| [shapes.md](shapes.md) | `mu/mu_shapes.h`: supershape, seashell, Möbius, Klein, RMF tube, lathe, twist extrusion, SDF→mesh (marching tets), L-systems, weld/subdivide/stats/validate; measured vs `par_shapes.h` |
| [normals.md](normals.md) | `mu_mat3_cofactor` / `mu_mat4_normal`: cofactor instead of inverse-transpose (2.9x, and correct under reflection) |
| [debug-break.md](debug-break.md) | `mu/mu_debug_break.h`: portable software breakpoint / `MU_DEBUG_BREAK()` |
| [allocators.md](allocators.md) | `mu/allocators.h` arena/pool/buddy, `mu/offset_allocator.h`, `mu/mu_mmgr.h` |
| [containers.md](containers.md) | `array`, `mu_span`, `mu_string_arena`, `mu_pool_link`/lists/freelists, `mu/mu_min_containers.h` |
| [sets-and-ids.md](sets-and-ids.md) | `mu_bitset`, `mu_sparse_set`, `mu_bulk_storage`, `mu_id_pool`, hash tables (`mu_hash32`, `mu_hash64`, `mu_phf`), `mu_multi_index`, `mu_chunked_u32_array` |
| [worklist.md](worklist.md) | `mu_worklist`: unique double-ended work queue |
| [sparse-array.md](sparse-array.md) | `mu_sparse_array`: paged 64-bit sparse array with stable addresses |
| [slab.md](slab.md) | `mu_slab`: fixed-size block allocator with per-thread caches |
| [deferred-free.md](deferred-free.md) | `mu_deferred_free`: frame-delayed retirement queue |
| [rb-tree.md](rb-tree.md) | `mu_rb_tree`: intrusive red-black tree, augmented updates, interval tree |
| [slab-manager.md](slab-manager.md) | `mu_slab_manager`: multi-heap, size-classed, reclaimable slab allocator |
| [id-alloc.md](id-alloc.md) | `mu_id_alloc`, `mu_id_alloc_mt`, `mu_id_alloc_sparse`: bitmap ID allocators |
| [sparse-bitset.md](sparse-bitset.md) | `mu_sparse_bitset`: 1024-bit-node sparse bitset |
| [dag.md](dag.md) | `mu_dag`: dynamic dependency graph with bottom-up traversal |
| [range-remap.md](range-remap.md) | `mu_range_remap`: sorted range-to-pointer map |
| [math.md](math.md) | `mu/mu_noise_math.h`, `mu/mu_geometric_algebra.h`, `mu_bitpacking.h` quantization |
| [mu_math.md](mu_math.md) | `mu/mu_math.h` — vec/quat/mat core, three inverse paths, SoA batch kernels; measured vs cglm |
| [ecs.md](ecs.md) | `mu/mu_ecs.h`: entity manager, pools, transforms, prefabs, resource registry |
| [concurrency.md](concurrency.md) | `mu/mu_thread_system.h`, `mu/mu_task_scheduler.h` |
| [roguelike.md](roguelike.md) | `mu/mu_roguelike.h`: RNG, grids, generation, FOV, pathfinding, tick scheduler |

## Design rules (apply to everything)

- **C99, single header, no dependencies** except where noted (cglm for GA,
  pthreads/Win32 for threading).
- **Customization via macros, not vtables.** Override `mu_malloc` / `mu_free`,
  `MU_TS_MALLOC`, `MUC_MALLOC`, etc. before including to redirect allocation.
- **`MU_API` / `MU_STATIC` linkage model.** Define `MU_STATIC` to make all
  `mu_*` symbols `static` (single-TU stashing), or `MU_DLL_EXPORT` /
  `MU_DLL_IMPORT` on Windows for shared libraries.
- **Assert on contract violations** (`MU_ASSERT`), never on recoverable
  runtime conditions. Out-of-memory returns `NULL`/`false`; capacity bugs
  assert loudly.
- **Offsets over pointers** wherever identity must survive serialization or
  realloc (indices, generations, handles).

## Build & test

```sh
# core C tests (mu.c includes every mu/*.c implementation)
cc -std=c99 -O2 -I. -D_POSIX_C_SOURCE=199309L test.c mu.c -o test

# full C++ test suite (bitsets + spans + all containers + scheduler)
c++ -std=c++17 -O2 -I. -D_POSIX_C_SOURCE=199309L -pthread test.cpp mu.c -o test_cpp
./test_cpp     # Summary: N/N checks passed
```

Note: `mu/mu_perf.h` uses `clock_gettime`; strict `-std=c99` needs
`-D_POSIX_C_SOURCE=199309L` on glibc.
