#pragma once
#ifndef MU_H
#define MU_H

/*
 * mu.h — umbrella header for the core `mu` library.
 *
 * All macros are provided by mu_macros.h. Override any MU_* macro before
 * including this header to redirect allocation, assertions, etc.
 */

#include "mu/mu_macros.h"

/* Legacy lowercase override names, kept as aliases of the unified macros. */
#ifndef mu_malloc
#  define mu_malloc MU_MALLOC
#endif
#ifndef mu_calloc
#  define mu_calloc MU_CALLOC
#endif
#ifndef mu_realloc
#  define mu_realloc MU_REALLOC
#endif
#ifndef mu_free
#  define mu_free MU_FREE
#endif

#include "mu/mu_common.h"
#include "mu/mu_debug_break.h"
#include "mu/mu_allocators.h"
#include "mu/mu_array.h"
#include "mu/mu_bitpacking.h"
#include "mu/mu_bitset.h"
#include "mu/mu_hier_bitset.h"
#include "mu/mu_chunked_bitset.h"
#include "mu/mu_bloom.h"
#include "mu/mu_bulk_storage.h"
#include "mu/mu_chunked_array.h"
#include "mu/mu_hash_table.h"
#include "mu/mu_perfect_hash.h"
#include "mu/mu_id_pool.h"
#include "mu/mu_length_index.h"
#include "mu/mu_multi_index.h"
#include "mu/mu_pcg.h"
#include "mu/mu_perf.h"
#include "mu/mu_spatial_array.h"
#include "mu/mu_span.h"
#include "mu/mu_sparse_set.h"
#include "mu/mu_sparse_array.h"
#include "mu/mu_worklist.h"
#include "mu/mu_slab.h"
#include "mu/mu_deferred_free.h"
#include "mu/mu_rb_tree.h"
#include "mu/mu_slab_manager.h"
#include "mu/mu_id_alloc.h"
#include "mu/mu_sparse_bitset.h"
#include "mu/mu_dag.h"
#include "mu/mu_range_remap.h"
#include "mu/mu_string.h"
#include "mu/mu_sort.h"
#include "mu/mu_simd.h"
#include "mu/mu_swiss_map.h"
#include "mu/mu_eytzinger.h"
#include "mu/mu_xor_filter.h"
#include "mu/mu_four_heap.h"
#include "mu/mu_timer_wheel.h"
#include "mu/mu_fast_codec.h"
#include "mu/mu_math.h"
#include "mu/mu_dlist.h"
#include "mu/mu_skiplist.h"
#include "mu/mu_shapes.h"
#include "mu/mu_scope_stack.h"
#include "mu/mu_log.h"

/* mu/mu_bench.h is deliberately NOT in the umbrella: it defines a benchmark
 * registry and a main(), so it belongs in exactly one translation unit of a
 * benchmark binary, not in every file that includes mu.h. */

/* mu/mu_rand.h is intentionally not part of the umbrella; include it directly
   if you want its competing RNG alongside mu_pcg32. */

#endif /* MU_H */
