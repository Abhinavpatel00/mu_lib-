#pragma once

/*
===============================================================================
mu_span.h
===============================================================================

A span is a non-owning view over a contiguous sequence of elements.

It is a "fat pointer": (pointer, length). It owns no memory, performs no
allocation, and never frees anything. Use it as a function parameter and
return type anywhere you would otherwise pass (ptr, count) pairs.

    "this function reads N floats from somewhere"

not:

    "this function owns and manages a float array"

------------------------------------------------------------------------------
Why this exists
------------------------------------------------------------------------------
The library already ships owning containers:

    array (mu_array.h)       owning dynamic array, header-prefixed pointer
    mu_string_arena          owning packed string storage
    mu_chunked_u32_array     owning chunked u32 storage
    mu_bulk_storage          owning slot storage with stable ids

None of those answers: "how do I pass a view of contiguous data without
copying it, and without coupling the callee to a specific container type?"

A span answers exactly that. A function taking `mu_span_f32 s` accepts:

    - a C array                      float a[16]
    - the mu array type              float* a (built with array_push)
    - min_containers stretchy buffers
    - a slice of any of the above    mu_span_f32_sub(s, 4, 8)
    - arena, stack, or mmap memory

...without the callee knowing or caring which.

------------------------------------------------------------------------------
Mental model
------------------------------------------------------------------------------

    data:   [ e0 ][ e1 ][ e2 ][ e3 ][ e4 ][ e5 ][ e6 ]
             ^                                       ^
             |                                       |
           span.data                    span.data + span.count - 1

    span = { data, count }

    empty span: { NULL, 0 } -- safe to create, safe to query, nothing to read.

A span is valid only while the underlying storage is alive and unmodified in
layout. If the backing container grows (realloc), spans into it dangle. Same
rule as raw pointers: a span is a borrow, not a container.

------------------------------------------------------------------------------
Subspans
------------------------------------------------------------------------------

    index:      0    1    2    3    4    5
    data:     [ a ][ b ][ c ][ d ][ e ][ f ]

    mu_span_f32_sub(s, 1, 3)   ->  [ b ][ c ][ d ]
    mu_span_f32_sub(s, 4, 99)  ->  [ e ][ f ]    (count clamped to parent)
    mu_span_f32_sub(s, 0, 0)   ->  empty
    mu_span_f32_sub(s, 99, 1)  ->  empty         (offset past end)

Out-of-range requests clamp instead of asserting, so callers can pass counts
derived from untrusted data without a wall of checks. Every empty result is a
valid span.

    mu_span_f32_first(s, 2)    ->  [ a ][ b ]
    mu_span_f32_last(s, 2)     ->  [ e ][ f ]

------------------------------------------------------------------------------
Byte view
------------------------------------------------------------------------------

mu_span itself is the byte view: data is void*, count is in BYTES.

    mu_span bytes = { ptr, byte_count }
    mu_span_from_array(payload)              // whole array, in bytes
    mu_span_slice(bytes, 16, 32)             // bytes [16, 48)

Typed aliases count ELEMENTS. Mixing them up is the classic span bug, so the
rule is: `mu_span` = bytes, `mu_span_<t>` = elements of <t>.

------------------------------------------------------------------------------
Copy semantics
------------------------------------------------------------------------------

    mu_span_<t>_copy_to(dst, dst_count)

copies into caller-owned storage, clamped to min(src, dst) count, overlap-safe
(memmove), returns the number of elements written. A span cannot grow, own, or
free; if you need ownership, push elements into an owning container and span it.

------------------------------------------------------------------------------
Performance
------------------------------------------------------------------------------

- trivially copyable two-field struct; pass by value
- every operation compiles to a loop / memmove / memcmp
- no allocation, no indirection, no error paths
===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/*
    Byte view.

    data   : pointer to first byte (NULL for empty spans)
    count  : number of BYTES visible
*/
typedef struct mu_span
{
    void*    data;
    uint32_t count;
} mu_span;

/*
    Declares a typed span alias `name` over element type `type`.

    Provided aliases: mu_span_u8 .. mu_span_u64, mu_span_i8 .. mu_span_i64,
    mu_span_f32, mu_span_f64, mu_span_size, mu_span_ptr (void* elements).
    Declare your own with MU_SPAN_OF(T, my_span_t) plus MU_SPAN_IMPL.
*/
#define MU_SPAN_OF(type, name) \
    typedef struct name        \
    {                          \
        type*    data;         \
        uint32_t count;        \
    } name

/*
    Emits the inline helper family for a span type.

    make / sub / first / last / find / contains / copy_to

    from_array is deliberately a macro (see below): the array element count can
    only be computed where the real array type is visible, never through a
    pointer parameter.
*/
#define MU_SPAN_IMPL(type, name)                                                                     \
    MU_INLINE name name##_make(type* ptr, uint32_t count)                                     \
    {                                                                                                \
        name s;                                                                                      \
        s.data  = ptr;                                                                               \
        s.count = count;                                                                             \
        return s;                                                                                    \
    }                                                                                                \
    MU_INLINE uint32_t name##_count(name s)                                                   \
    {                                                                                                \
        return s.count;                                                                              \
    }                                                                                                \
    MU_INLINE bool name##_empty(name s)                                                       \
    {                                                                                                \
        return s.count == 0;                                                                         \
    }                                                                                                \
    MU_INLINE name name##_sub(name s, uint32_t offset, uint32_t count)                        \
    {                                                                                                \
        name out;                                                                                    \
        if(s.data == NULL || offset >= s.count)                                                      \
        {                                                                                            \
            out.data  = NULL;                                                                        \
            out.count = 0;                                                                           \
            return out;                                                                              \
        }                                                                                            \
        uint64_t remaining = (uint64_t)s.count - (uint64_t)offset;                                   \
        out.data           = s.data + offset;                                                        \
        out.count          = (uint32_t)MU_MIN((uint64_t)count, remaining);                           \
        return out;                                                                                  \
    }                                                                                                \
    MU_INLINE name name##_first(name s, uint32_t count)                                       \
    {                                                                                                \
        return name##_sub(s, 0, count);                                                              \
    }                                                                                                \
    MU_INLINE name name##_last(name s, uint32_t count)                                        \
    {                                                                                                \
        if(count >= s.count)                                                                         \
            return s;                                                                                \
        return name##_sub(s, s.count - count, count);                                                \
    }                                                                                                \
    MU_INLINE int32_t name##_find(name s, type value)                                         \
    {                                                                                                \
        for(uint32_t i = 0; i < s.count; ++i)                                                        \
        {                                                                                            \
            if(s.data[i] == value)                                                                   \
                return (int32_t)i;                                                                   \
        }                                                                                            \
        return -1;                                                                                   \
    }                                                                                                \
    MU_INLINE bool name##_contains(name s, type value)                                        \
    {                                                                                                \
        return name##_find(s, value) >= 0;                                                           \
    }                                                                                                \
    MU_INLINE uint32_t name##_copy_to(name s, type* dst, uint32_t dst_count)                  \
    {                                                                                                \
        if(!s.data || !dst || dst_count == 0)                                                        \
            return 0;                                                                                \
        uint32_t n = MU_MIN(s.count, dst_count);                                                     \
        memmove(dst, s.data, (size_t)n * sizeof(type));                                              \
        return n;                                                                                    \
    }

MU_SPAN_OF(uint8_t, mu_span_u8);
MU_SPAN_OF(uint16_t, mu_span_u16);
MU_SPAN_OF(uint32_t, mu_span_u32);
MU_SPAN_OF(uint64_t, mu_span_u64);
MU_SPAN_OF(int8_t, mu_span_i8);
MU_SPAN_OF(int16_t, mu_span_i16);
MU_SPAN_OF(int32_t, mu_span_i32);
MU_SPAN_OF(int64_t, mu_span_i64);
MU_SPAN_OF(float, mu_span_f32);
MU_SPAN_OF(double, mu_span_f64);
MU_SPAN_OF(size_t, mu_span_size);
MU_SPAN_OF(void*, mu_span_ptr);

MU_SPAN_IMPL(uint8_t, mu_span_u8)
MU_SPAN_IMPL(uint16_t, mu_span_u16)
MU_SPAN_IMPL(uint32_t, mu_span_u32)
MU_SPAN_IMPL(uint64_t, mu_span_u64)
MU_SPAN_IMPL(int8_t, mu_span_i8)
MU_SPAN_IMPL(int16_t, mu_span_i16)
MU_SPAN_IMPL(int32_t, mu_span_i32)
MU_SPAN_IMPL(int64_t, mu_span_i64)
MU_SPAN_IMPL(float, mu_span_f32)
MU_SPAN_IMPL(double, mu_span_f64)
MU_SPAN_IMPL(size_t, mu_span_size)
MU_SPAN_IMPL(void*, mu_span_ptr)

/*
    View over a C array. Macro on purpose: MU_ARRAY_COUNT needs the real array
    type at the call site; passing a `type*` would silently compute garbage.
*/
#define mu_span_u8_from_array(arr) mu_span_u8_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_u16_from_array(arr) mu_span_u16_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_u32_from_array(arr) mu_span_u32_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_u64_from_array(arr) mu_span_u64_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_i8_from_array(arr) mu_span_i8_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_i16_from_array(arr) mu_span_i16_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_i32_from_array(arr) mu_span_i32_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_i64_from_array(arr) mu_span_i64_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_f32_from_array(arr) mu_span_f32_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_f64_from_array(arr) mu_span_f64_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_size_from_array(arr) mu_span_size_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))
#define mu_span_ptr_from_array(arr) mu_span_ptr_make((arr), (uint32_t)MU_ARRAY_COUNT(arr))

/* ===========================================================================
   mu_span (byte view) helpers
   =========================================================================== */

/* View over raw bytes. */
MU_INLINE mu_span mu_span_make(void* ptr, uint32_t byte_count)
{
    mu_span s;
    s.data  = ptr;
    s.count = byte_count;
    return s;
}

/* View over a C array's bytes. */
#define mu_span_from_array(arr) mu_span_make((arr), (uint32_t)(sizeof(arr)))

/* View over the used elements of a header-prefixed mu array, in bytes. */
#define mu_span_from_mu_array(arr) mu_span_make((arr), (uint32_t)(array_size(arr) * sizeof(*(arr))))

/* Cast a byte view onto a typed element view. Count shrinks to whole elements. */
MU_INLINE mu_span_u32 mu_span_as_u32(mu_span s)
{
    mu_span_u32 out;
    out.data  = (uint32_t*)s.data;
    out.count = (uint32_t)(s.count / sizeof(uint32_t));
    return out;
}

MU_INLINE mu_span_f32 mu_span_as_f32(mu_span s)
{
    mu_span_f32 out;
    out.data  = (float*)s.data;
    out.count = (uint32_t)(s.count / sizeof(float));
    return out;
}

/* Element-wise subspan of the byte view. Offset/count are in BYTES. */
MU_INLINE mu_span mu_span_slice(mu_span s, uint32_t offset, uint32_t byte_count)
{
    mu_span out;
    if(s.data == NULL || offset >= s.count)
    {
        out.data  = NULL;
        out.count = 0;
        return out;
    }
    uint64_t remaining = (uint64_t)s.count - (uint64_t)offset;
    out.data           = (char*)s.data + offset;
    out.count          = (uint32_t)MU_MIN((uint64_t)byte_count, remaining);
    return out;
}

MU_INLINE mu_span mu_span_first(mu_span s, uint32_t byte_count)
{
    return mu_span_slice(s, 0, byte_count);
}

MU_INLINE mu_span mu_span_last(mu_span s, uint32_t byte_count)
{
    if(byte_count >= s.count)
        return s;
    return mu_span_slice(s, s.count - byte_count, byte_count);
}

/* Byte-identical contents and equal length? */
MU_INLINE bool mu_span_equal(mu_span a, mu_span b)
{
    if(a.count != b.count)
        return false;
    if(a.count == 0)
        return true;
    return MU_MEMCMP(a.data, b.data, a.count) == 0;
}

/* Copy bytes into caller-owned dst, clamped, overlap-safe. Bytes written. */
MU_INLINE uint32_t mu_span_copy_to(mu_span s, void* dst, uint32_t dst_bytes)
{
    if(!s.data || !dst || dst_bytes == 0)
        return 0;

    uint32_t n = MU_MIN(s.count, dst_bytes);
    memmove(dst, s.data, n);
    return n;
}

MU_END_EXTERN_C

/* Void-byte span for byte-oriented APIs (document model, push constants). */
typedef struct ByteSpan {
    const void *data;
    uint32_t    size;
} ByteSpan;

#define BYTE_SPAN(value)                                                                                               \
    (ByteSpan) {.data = &(value), .size = (uint32_t)sizeof(value)}
