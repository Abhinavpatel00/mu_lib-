
#pragma once

#include "mu_common.h"

/*
USER POINTER (a)
        |
        v
+---------------------+-----------------------+
| header              | actual data           |
| size | capacity     | a[0] a[1] a[2] ...    |
+---------------------+-----------------------+
        ^
        |
   (a - header_size)

*/

/*
 * C++ needs an explicit cast when a void* result is stored back into a typed
 * pointer; C99 does not. MU_ARRAY_CAST resolves the target type from the
 * destination expression so the macros below stay valid in both languages.
 */
#if defined(_MSC_VER)
#  define MU_ARRAY_CAST(a, p) (decltype(a))(p)
#elif defined(__GNUC__) || defined(__clang__)
#  define MU_ARRAY_CAST(a, p) ((__typeof__(a))(p))
#else
#  define MU_ARRAY_CAST(a, p) (p)
#endif

typedef struct
{
    uint32_t size;
    uint32_t capacity;
    uint32_t _pad0;
    uint32_t _pad1;
} array_header_t;

#define array_header(a) ((array_header_t*)((char*)(a) - sizeof(array_header_t)))

#define array_size(a) ((a) ? array_header(a)->size : 0)
#define array_capacity(a) ((a) ? array_header(a)->capacity : 0)
#define array_clear(a) ((a) ? (array_header(a)->size = 0) : 0)
#define array_free(a) ((a) ? MU_FREE(array_header(a)), (a) = NULL : 0)

#define array_full(a) ((a) && array_size(a) >= array_capacity(a))

MU_INLINE void* array_grow(void* arr, size_t elem_size, uint32_t min_capacity)
{
    uint32_t new_capacity = 16;

    if(arr)
    {
        new_capacity = array_capacity(arr) * 2;
    }

    if(new_capacity < min_capacity)
        new_capacity = min_capacity;

    size_t new_size = sizeof(array_header_t) + new_capacity * elem_size;

    array_header_t* new_header;

    if(arr)
    {
        new_header = (array_header_t*)MU_REALLOC(array_header(arr), new_size);
    }
    else
    {
        new_header       = (array_header_t*)MU_MALLOC(new_size);
        new_header->size = 0;
    }

    new_header->capacity = new_capacity;

    return (char*)new_header + sizeof(array_header_t);
}
#define array_push(a, val)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if(!(a) || array_full(a))                                                                                      \
        {                                                                                                              \
            (a) = MU_ARRAY_CAST(a, array_grow((a), sizeof(*(a)), array_size(a) + 1));                                 \
        }                                                                                                              \
        (a)[array_header(a)->size++] = (val);                                                                          \
    } while(0)

#define array_reserve(a, n)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if(!(a) || array_capacity(a) < (n))                                                                            \
        {                                                                                                              \
            (a) = MU_ARRAY_CAST(a, array_grow((a), sizeof(*(a)), (n)));                                               \
        }                                                                                                              \
    } while(0)

#define array_pop(a) ((a) ? --array_header(a)->size : 0)

#define array_back(a) ((a)[array_header(a)->size - 1])
