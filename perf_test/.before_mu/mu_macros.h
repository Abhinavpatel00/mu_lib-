#pragma once
#ifndef MU_MACROS_H
#define MU_MACROS_H

/*
 * mu_macros.h — the single macro layer for all of mylib.
 *
 * Every mylib header (the core mu/ family and the standalone modules)
 * includes this file instead of defining its own INLINE / ASSERT / MALLOC
 * family.
 *
 * Conventions:
 *   - Every macro is wrapped in `#ifndef MU_X`, so a user can override it by
 *     defining it before including any mylib header.
 *   - All public macros are prefixed `MU_`. There are no per-module variants
 *     (no MU_TS_INLINE, MUC_MALLOC, MU_ECS_FREE, ...).
 *   - `MU_INLINE` already means `static inline`; do not write `static MU_INLINE`.
 *   - `MU_FORCE_INLINE` is the always-inline variant for genuinely hot helpers.
 *
 * Requires C99 (inline, stdint, stdbool) or C++11.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* --------------------------------------------------------------------------
 * Linkage
 * -------------------------------------------------------------------------- */

#ifndef MU_API
#  if defined(MU_STATIC)
#    define MU_API static
#  elif defined(_WIN32) && defined(MU_DLL_EXPORT)
#    define MU_API __declspec(dllexport)
#  elif defined(_WIN32) && defined(MU_DLL_IMPORT)
#    define MU_API __declspec(dllimport)
#  else
#    define MU_API extern
#  endif
#endif

#ifdef __cplusplus
#  define MU_EXTERN_C       extern "C"
#  define MU_BEGIN_EXTERN_C extern "C" {
#  define MU_END_EXTERN_C   }
#else
#  define MU_EXTERN_C       extern
#  define MU_BEGIN_EXTERN_C
#  define MU_END_EXTERN_C
#endif

/* --------------------------------------------------------------------------
 * Compiler attributes
 * -------------------------------------------------------------------------- */

#ifndef MU_INLINE
#  define MU_INLINE static inline
#endif

#ifndef MU_FORCE_INLINE
#  if defined(_MSC_VER)
#    define MU_FORCE_INLINE static __forceinline
#  elif defined(__GNUC__) || defined(__clang__)
#    define MU_FORCE_INLINE static inline __attribute__((always_inline))
#  else
#    define MU_FORCE_INLINE static inline
#  endif
#endif

#ifndef MU_NOINLINE
#  if defined(_MSC_VER)
#    define MU_NOINLINE __declspec(noinline)
#  elif defined(__GNUC__) || defined(__clang__)
#    define MU_NOINLINE __attribute__((noinline))
#  else
#    define MU_NOINLINE
#  endif
#endif

#ifndef MU_RESTRICT
#  if defined(_MSC_VER)
#    define MU_RESTRICT __restrict
#  elif defined(__cplusplus)
#    define MU_RESTRICT __restrict__
#  elif defined(__GNUC__) || defined(__clang__)
#    define MU_RESTRICT restrict
#  else
#    define MU_RESTRICT
#  endif
#endif

#ifndef MU_ALIGN
#  if defined(_MSC_VER)
#    define MU_ALIGN(N) __declspec(align(N))
#  elif defined(__GNUC__) || defined(__clang__)
#    define MU_ALIGN(N) __attribute__((aligned(N)))
#  else
#    define MU_ALIGN(N)
#  endif
#endif

/*
 * MU_DEBUG_BREAK() emits a native breakpoint instruction instead of a
 * generic trap, so debuggers stop at the call site with SIGTRAP. It also acts
 * as a compiler barrier, so code after it is not optimized away.
 *
 * The instruction sequences are adapted from Scott Tsai's debugbreak.h
 * (BSD-2-Clause). See mu/mu_debug_break.h for the full notice.
 */
#ifndef MU_DEBUG_BREAK
#  if defined(_MSC_VER)
#    define MU_DEBUG_BREAK() do { __debugbreak(); } while (0)
#  elif defined(__clang__) && defined(__aarch64__) && defined(__APPLE__)
#    define MU_DEBUG_BREAK() do { __builtin_debugtrap(); } while (0)
#  elif defined(__i386__) || defined(__x86_64__)
#    define MU_DEBUG_BREAK() do { __asm__ __volatile__("int3"); } while (0)
#  elif defined(__thumb__)
#    define MU_DEBUG_BREAK() do { __asm__ __volatile__(".inst 0xde01"); } while (0)
#  elif defined(__arm__)
#    define MU_DEBUG_BREAK() do { __asm__ __volatile__(".inst 0xe7f001f0"); } while (0)
#  elif defined(__aarch64__)
#    define MU_DEBUG_BREAK() do { __asm__ __volatile__(".inst 0xd4200000"); } while (0)
#  elif defined(__powerpc__) || defined(__powerpc64__)
#    define MU_DEBUG_BREAK() do { __asm__ __volatile__(".4byte 0x7d821008"); } while (0)
#  elif defined(__riscv)
#    define MU_DEBUG_BREAK() do { __asm__ __volatile__(".4byte 0x00100073"); } while (0)
#  elif defined(__GNUC__) || defined(__clang__)
#    define MU_DEBUG_BREAK() do { __builtin_trap(); } while (0)
#  else
#    define MU_DEBUG_BREAK() do { (*(volatile int*)0 = 0); } while (0)
#  endif
#endif

#ifndef MU_DEPRECATED
#  if defined(_MSC_VER)
#    define MU_DEPRECATED(msg) __declspec(deprecated(msg))
#  elif defined(__GNUC__) || defined(__clang__)
#    define MU_DEPRECATED(msg) __attribute__((deprecated(msg)))
#  else
#    define MU_DEPRECATED(msg)
#  endif
#endif

#ifndef MU_LIKELY
#  if defined(__GNUC__) || defined(__clang__)
#    define MU_LIKELY(x)   __builtin_expect(!!(x), 1)
#    define MU_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  else
#    define MU_LIKELY(x)   (x)
#    define MU_UNLIKELY(x) (x)
#  endif
#endif

#ifndef MU_PREFETCH
#  if defined(__GNUC__) || defined(__clang__)
#    define MU_PREFETCH(addr) __builtin_prefetch(addr)
#  else
#    define MU_PREFETCH(addr) ((void)(addr))
#  endif
#endif

/* --------------------------------------------------------------------------
 * Assertions
 *   MU_ASSERT follows the standard assert() convention: compiled out under
 *   NDEBUG. MU_PANIC always aborts.
 * -------------------------------------------------------------------------- */

#ifndef MU_ASSERT
#  define MU_ASSERT(x) assert(x)
#endif

#ifndef MU_PANIC
#  define MU_PANIC() do { MU_DEBUG_BREAK(); abort(); } while (0)
#endif

/* --------------------------------------------------------------------------
 * Allocation and memory.
 *   Route ALL mylib allocation through these so a single override redirects
 *   the whole library.
 * -------------------------------------------------------------------------- */

#ifndef MU_MALLOC
#  define MU_MALLOC(sz)          malloc(sz)
#endif

#ifndef MU_CALLOC
#  define MU_CALLOC(count, sz)   calloc((count), (sz))
#endif

#ifndef MU_REALLOC
#  define MU_REALLOC(ptr, sz)    realloc((ptr), (sz))
#endif

#ifndef MU_FREE
#  define MU_FREE(ptr)           free(ptr)
#endif

#ifndef MU_MEMSET
#  define MU_MEMSET(dst, val, sz)      memset((dst), (val), (sz))
#endif

#ifndef MU_MEMCPY
#  define MU_MEMCPY(dst, src, sz)      memcpy((dst), (src), (sz))
#endif

#ifndef MU_MEMMOVE
#  define MU_MEMMOVE(dst, src, sz)     memmove((dst), (src), (sz))
#endif

#ifndef MU_MEMCMP
#  define MU_MEMCMP(a, b, sz)          memcmp((a), (b), (sz))
#endif

/* --------------------------------------------------------------------------
 * Common constants
 * -------------------------------------------------------------------------- */

#ifndef MU_PI
#  define MU_PI 3.14159265358979323846
#endif

#ifndef MU_TAU
#  define MU_TAU 6.28318530717958647692
#endif

#ifndef MU_DEG2RAD
#  define MU_DEG2RAD (MU_PI / 180.0)
#endif

#ifndef MU_RAD2DEG
#  define MU_RAD2DEG (180.0 / MU_PI)
#endif

#ifndef MU_INVALID_INDEX
#  define MU_INVALID_INDEX UINT32_MAX
#endif

/* --------------------------------------------------------------------------
 * Generic helpers
 * -------------------------------------------------------------------------- */

#ifndef MU_UNUSED
#  define MU_UNUSED(x) (void)(x)
#endif

#ifndef MU_NOOP
#  define MU_NOOP() do { } while (0)
#endif

#ifndef MU_ARRAY_COUNT
#  define MU_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef MU_STRINGIFY
#  define MU_STRINGIFY_(x) #x
#  define MU_STRINGIFY(x)  MU_STRINGIFY_(x)
#  define MU_TOSTRING(x)   MU_STRINGIFY(x)
#endif

#ifndef MU_CONCAT
#  define MU_CONCAT_(a, b) a##b
#  define MU_CONCAT(a, b)  MU_CONCAT_(a, b)
#endif

#ifndef MU_IS_POW2
#  define MU_IS_POW2(x) (((x) != 0) && (((x) & ((x) - 1)) == 0))
#endif

#ifndef MU_KB
#  define MU_KB(x) ((unsigned long long)(x) * 1024ull)
#  define MU_MB(x) ((unsigned long long)(x) * 1024ull * 1024ull)
#  define MU_GB(x) ((unsigned long long)(x) * 1024ull * 1024ull * 1024ull)
#endif

#ifndef MU_CEIL
#  define MU_CEIL(x, y)  (((x) + (y) - 1) / (y))
#endif

#ifndef MU_FLOOR
#  define MU_FLOOR(x, y) ((x) / (y))
#endif

#ifndef MU_ALIGN_UP
#  define MU_ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#endif

#ifndef MU_ALIGN_DOWN
#  define MU_ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#endif

#ifndef MU_MIN
#  if defined(__GNUC__) || defined(__clang__)
#    define MU_MIN(a, b) ({ __typeof__(a) _mu_a = (a); __typeof__(b) _mu_b = (b); _mu_a < _mu_b ? _mu_a : _mu_b; })
#    define MU_MAX(a, b) ({ __typeof__(a) _mu_a = (a); __typeof__(b) _mu_b = (b); _mu_a > _mu_b ? _mu_a : _mu_b; })
#  else
#    define MU_MIN(a, b) ((a) < (b) ? (a) : (b))
#    define MU_MAX(a, b) ((a) > (b) ? (a) : (b))
#  endif
#endif

#ifndef MU_CLAMP
#  define MU_CLAMP(x, lo, hi) (MU_MIN(MU_MAX((x), (lo)), (hi)))
#endif

#ifndef MU_SWAP
#  define MU_SWAP(type, a, b) do { type _mu_t = (a); (a) = (b); (b) = _mu_t; } while (0)
#endif

#ifndef MU_OFFSET_OF
#  define MU_OFFSET_OF(type, member) ((size_t)&(((type*)0)->member))
#endif

#ifndef MU_CONTAINER_OF
#  define MU_CONTAINER_OF(ptr, type, member) ((type*)((char*)(ptr) - MU_OFFSET_OF(type, member)))
#endif

#ifndef MU_BIT
#  define MU_BIT(n) (1ull << (n))
#endif

#ifndef MU_HAS_FLAG
#  define MU_HAS_FLAG(x, flag)   (((x) & (flag)) != 0)
#  define MU_SET_FLAG(x, flag)   ((x) |= (flag))
#  define MU_CLEAR_FLAG(x, flag) ((x) &= ~(flag))
#endif

#ifndef MU_STATIC_ASSERT
#  if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#    define MU_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#  elif defined(__cplusplus)
#    define MU_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#  else
#    define MU_STATIC_ASSERT_GLUE_(a, b) a##b
#    define MU_STATIC_ASSERT_GLUE(a, b)  MU_STATIC_ASSERT_GLUE_(a, b)
#    if defined(__GNUC__) || defined(__clang__)
#      define MU_STATIC_ASSERT(cond, msg) \
           typedef char MU_STATIC_ASSERT_GLUE(mu_static_assertion_, __LINE__)[(cond) ? 1 : -1] \
               __attribute__((unused))
#    else
#      define MU_STATIC_ASSERT(cond, msg) \
           typedef char MU_STATIC_ASSERT_GLUE(mu_static_assertion_, __LINE__)[(cond) ? 1 : -1]
#    endif
#  endif
#endif

#endif /* MU_MACROS_H */
