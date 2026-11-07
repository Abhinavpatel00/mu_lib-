// Minimalist container library (C99 single header)
#ifndef MU_MIN_CONTAINERS_H
#define MU_MIN_CONTAINERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "mu_macros.h"

MU_BEGIN_EXTERN_C

// Stretchy buffer

typedef struct mu_array_header
{
    size_t size;
    size_t capacity;
} mu_array_header;

MU_INLINE mu_array_header *mu_array_header_(void *a)
{
    return (mu_array_header *)((uint8_t *)a - sizeof(mu_array_header));
}

/* C++ needs an explicit cast when storing void* back into a typed pointer. */
#if defined(_MSC_VER)
#  define MU_MIN_ARRAY_CAST(a, p) (decltype(a))(p)
#elif defined(__GNUC__) || defined(__clang__)
#  define MU_MIN_ARRAY_CAST(a, p) ((__typeof__(a))(p))
#else
#  define MU_MIN_ARRAY_CAST(a, p) (p)
#endif

#define mu_array_size(a) ((a) ? mu_array_header_(a)->size : 0)
#define mu_array_capacity(a) ((a) ? mu_array_header_(a)->capacity : 0)
#define mu_array_empty(a) (mu_array_size(a) == 0)

MU_INLINE size_t mu_array_next_capacity(size_t current, size_t min_capacity)
{
    size_t cap = (current == 0) ? 8u : current;
    while (cap < min_capacity)
        cap *= 2u;
    return cap;
}

MU_INLINE void *mu_array_grow(void *a, size_t elem_size, size_t min_capacity)
{
    size_t new_capacity = mu_array_next_capacity(mu_array_capacity(a), min_capacity);
    size_t new_size = sizeof(mu_array_header) + new_capacity * elem_size;

    if (a)
    {
        mu_array_header *h = mu_array_header_(a);
        h = (mu_array_header *)MU_REALLOC(h, new_size);
        h->capacity = new_capacity;
        return (void *)(h + 1);
    }

    mu_array_header *h = (mu_array_header *)MU_MALLOC(new_size);
    if (!h)
        return NULL;
    h->size = 0;
    h->capacity = new_capacity;
    return (void *)(h + 1);
}

MU_INLINE void *mu_array_reserve(void *a, size_t elem_size, size_t min_capacity)
{
    if (mu_array_capacity(a) < min_capacity)
        return mu_array_grow(a, elem_size, min_capacity);
    return a;
}

MU_INLINE void *mu_array_resize(void *a, size_t elem_size, size_t new_size)
{
    if (new_size > mu_array_capacity(a))
        a = mu_array_grow(a, elem_size, new_size);
    if (a)
        mu_array_header_(a)->size = new_size;
    return a;
}

MU_INLINE void mu_array_free(void *a)
{
    if (a)
        MU_FREE(mu_array_header_(a));
}

#define mu_array_ensure(a, n) ((a) = MU_MIN_ARRAY_CAST(a, mu_array_reserve((a), sizeof(*(a)), (n))))
#define mu_array_push(a, v)                                                                 \
    do                                                                                      \
    {                                                                                       \
        size_t mu__n = mu_array_size(a) + 1u;                                               \
        (a) = MU_MIN_ARRAY_CAST(a, mu_array_resize((a), sizeof(*(a)), mu__n));                                    \
        (a)[mu__n - 1u] = (v);                                                              \
    } while (0)
#define mu_array_pop(a)                                                                     \
    do                                                                                      \
    {                                                                                       \
        if (a && mu_array_size(a) > 0u)                                                     \
            mu_array_header_(a)->size -= 1u;                                                \
    } while (0)
#define mu_array_clear(a)                                                                   \
    do                                                                                      \
    {                                                                                       \
        if (a)                                                                              \
            mu_array_header_(a)->size = 0u;                                                 \
    } while (0)

// Dynamic string append buffer (char stretchy buffer)

MU_INLINE void mu_array_vprintf(char **a, const char *fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);
    int n = vsnprintf(NULL, 0, fmt, args2);
    va_end(args2);

    if (n <= 0)
        return;

    size_t an = mu_array_size(*a);
    size_t needed = an + (size_t)n + 1u;
    *a = (char *)mu_array_reserve(*a, sizeof(char), needed);
    if (!*a)
        return;

    vsnprintf(*a + an, (size_t)n + 1u, fmt, args);
    *a = (char *)mu_array_resize(*a, sizeof(char), an + (size_t)n);
}

MU_INLINE void mu_array_printf(char **a, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mu_array_vprintf(a, fmt, args);
    va_end(args);
}

// Hash table (u64 -> u64), open addressing with linear probing

#define MU_HASH_EMPTY UINT64_MAX
#define MU_HASH_TOMBSTONE (UINT64_MAX - 1u)

typedef struct mu_hash64
{
    uint64_t *keys;
    uint64_t *values;
    size_t capacity;
    size_t count;
    size_t tombstones;
} mu_hash64;

MU_INLINE uint64_t mu_hash64_mix(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

MU_INLINE size_t mu_hash64_next_pow2(size_t v)
{
    size_t r = 1u;
    while (r < v)
        r <<= 1u;
    return r;
}

MU_INLINE bool mu_hash64_init(mu_hash64 *h, size_t capacity)
{
    size_t cap = mu_hash64_next_pow2(capacity < 8u ? 8u : capacity);
    h->keys = (uint64_t *)MU_MALLOC(sizeof(uint64_t) * cap);
    h->values = (uint64_t *)MU_MALLOC(sizeof(uint64_t) * cap);
    if (!h->keys || !h->values)
    {
        MU_FREE(h->keys);
        MU_FREE(h->values);
        h->keys = NULL;
        h->values = NULL;
        h->capacity = 0u;
        h->count = 0u;
        h->tombstones = 0u;
        return false;
    }
    for (size_t i = 0; i < cap; ++i)
        h->keys[i] = MU_HASH_EMPTY;
    h->capacity = cap;
    h->count = 0u;
    h->tombstones = 0u;
    return true;
}

MU_INLINE void mu_hash64_free(mu_hash64 *h)
{
    if (!h)
        return;
    MU_FREE(h->keys);
    MU_FREE(h->values);
    h->keys = NULL;
    h->values = NULL;
    h->capacity = 0u;
    h->count = 0u;
    h->tombstones = 0u;
}

MU_INLINE void mu_hash64_clear(mu_hash64 *h)
{
    if (!h || !h->keys)
        return;
    for (size_t i = 0; i < h->capacity; ++i)
        h->keys[i] = MU_HASH_EMPTY;
    h->count = 0u;
    h->tombstones = 0u;
}

MU_INLINE bool mu_hash64_rehash(mu_hash64 *h, size_t new_capacity)
{
    mu_hash64 nh;
    if (!mu_hash64_init(&nh, new_capacity))
        return false;

    for (size_t i = 0; i < h->capacity; ++i)
    {
        uint64_t key = h->keys[i];
        if (key == MU_HASH_EMPTY || key == MU_HASH_TOMBSTONE)
            continue;

        size_t mask = nh.capacity - 1u;
        size_t idx = (size_t)(mu_hash64_mix(key) & (uint64_t)mask);
        while (nh.keys[idx] != MU_HASH_EMPTY)
            idx = (idx + 1u) & mask;
        nh.keys[idx] = key;
        nh.values[idx] = h->values[i];
        nh.count += 1u;
    }

    mu_hash64_free(h);
    *h = nh;
    return true;
}

MU_INLINE bool mu_hash64_maybe_grow(mu_hash64 *h)
{
    if (!h || h->capacity == 0u)
        return false;
    size_t used = h->count + h->tombstones;
    if (used * 10u < h->capacity * 7u)
        return true;
    return mu_hash64_rehash(h, h->capacity * 2u);
}

MU_INLINE bool mu_hash64_set(mu_hash64 *h, uint64_t key, uint64_t value)
{
    if (key == MU_HASH_EMPTY || key == MU_HASH_TOMBSTONE)
        return false;
    if (!h->keys)
    {
        if (!mu_hash64_init(h, 16u))
            return false;
    }
    if (!mu_hash64_maybe_grow(h))
        return false;

    size_t mask = h->capacity - 1u;
    size_t idx = (size_t)(mu_hash64_mix(key) & (uint64_t)mask);
    size_t first_tombstone = (size_t)(-1);

    while (h->keys[idx] != MU_HASH_EMPTY)
    {
        if (h->keys[idx] == key)
        {
            h->values[idx] = value;
            return true;
        }
        if (h->keys[idx] == MU_HASH_TOMBSTONE && first_tombstone == (size_t)(-1))
            first_tombstone = idx;
        idx = (idx + 1u) & mask;
    }

    if (first_tombstone != (size_t)(-1))
    {
        idx = first_tombstone;
        h->tombstones -= 1u;
    }

    h->keys[idx] = key;
    h->values[idx] = value;
    h->count += 1u;
    return true;
}

MU_INLINE bool mu_hash64_get(const mu_hash64 *h, uint64_t key, uint64_t *out_value)
{
    if (!h || !h->keys || key == MU_HASH_EMPTY || key == MU_HASH_TOMBSTONE)
        return false;
    size_t mask = h->capacity - 1u;
    size_t idx = (size_t)(mu_hash64_mix(key) & (uint64_t)mask);

    while (h->keys[idx] != MU_HASH_EMPTY)
    {
        if (h->keys[idx] == key)
        {
            if (out_value)
                *out_value = h->values[idx];
            return true;
        }
        idx = (idx + 1u) & mask;
    }
    return false;
}

MU_INLINE uint64_t mu_hash64_get_or(const mu_hash64 *h, uint64_t key, uint64_t fallback)
{
    uint64_t v = fallback;
    if (mu_hash64_get(h, key, &v))
        return v;
    return fallback;
}

MU_INLINE bool mu_hash64_remove(mu_hash64 *h, uint64_t key)
{
    if (!h || !h->keys || key == MU_HASH_EMPTY || key == MU_HASH_TOMBSTONE)
        return false;
    size_t mask = h->capacity - 1u;
    size_t idx = (size_t)(mu_hash64_mix(key) & (uint64_t)mask);

    while (h->keys[idx] != MU_HASH_EMPTY)
    {
        if (h->keys[idx] == key)
        {
            h->keys[idx] = MU_HASH_TOMBSTONE;
            h->count -= 1u;
            h->tombstones += 1u;
            return true;
        }
        idx = (idx + 1u) & mask;
    }
    return false;
}

// Fixed-size hash table (caller owns arrays)

typedef struct mu_hash64_static
{
    uint64_t *keys;
    uint64_t *values;
    size_t capacity;
} mu_hash64_static;

MU_INLINE void mu_hash64_static_clear(mu_hash64_static *h)
{
    for (size_t i = 0; i < h->capacity; ++i)
        h->keys[i] = MU_HASH_EMPTY;
}

MU_INLINE bool mu_hash64_static_set(mu_hash64_static *h, uint64_t key, uint64_t value)
{
    if (key == MU_HASH_EMPTY || key == MU_HASH_TOMBSTONE)
        return false;
    size_t idx = (size_t)(mu_hash64_mix(key) % h->capacity);
    while (h->keys[idx] != MU_HASH_EMPTY && h->keys[idx] != key)
        idx = (idx + 1u) % h->capacity;
    h->keys[idx] = key;
    h->values[idx] = value;
    return true;
}

MU_INLINE bool mu_hash64_static_get(const mu_hash64_static *h, uint64_t key, uint64_t *out_value)
{
    if (key == MU_HASH_EMPTY || key == MU_HASH_TOMBSTONE)
        return false;
    size_t idx = (size_t)(mu_hash64_mix(key) % h->capacity);
    while (h->keys[idx] != MU_HASH_EMPTY && h->keys[idx] != key)
        idx = (idx + 1u) % h->capacity;
    if (h->keys[idx] == key)
    {
        if (out_value)
            *out_value = h->values[idx];
        return true;
    }
    return false;
}

MU_INLINE uint64_t mu_hash64_static_get_or(const mu_hash64_static *h, uint64_t key, uint64_t fallback)
{
    uint64_t v = fallback;
    if (mu_hash64_static_get(h, key, &v))
        return v;
    return fallback;
}

// Packed string block with offsets (arrays of strings)

typedef struct mu_string_block
{
    char *block;
    uint32_t used;
    uint32_t *offsets;
} mu_string_block;

MU_INLINE void mu_string_block_init(mu_string_block *b)
{
    b->block = NULL;
    b->used = 0u;
    b->offsets = NULL;
}

MU_INLINE void mu_string_block_free(mu_string_block *b)
{
    mu_array_free(b->block);
    mu_array_free(b->offsets);
    b->block = NULL;
    b->offsets = NULL;
    b->used = 0u;
}

MU_INLINE bool mu_string_block_push(mu_string_block *b, const char *s, uint32_t *out_index)
{
    size_t n = strlen(s) + 1u;
    if (b->used + n > UINT32_MAX)
        return false;

    mu_array_ensure(b->block, b->used + n);
    if (!b->block)
        return false;

    uint32_t offset = b->used;
    memcpy(b->block + b->used, s, n);
    b->used += (uint32_t)n;

    mu_array_push(b->offsets, offset);
    if (out_index)
        *out_index = (uint32_t)(mu_array_size(b->offsets) - 1u);
    return true;
}

MU_INLINE const char *mu_string_block_get(const mu_string_block *b, uint32_t index)
{
    if (!b->block || index >= mu_array_size(b->offsets))
        return NULL;
    return b->block + b->offsets[index];
}

// Index-based list helpers

typedef struct mu_index_link
{
    uint32_t prev;
    uint32_t next;
} mu_index_link;

MU_INLINE void mu_index_list_init(mu_index_link *links, uint32_t head)
{
    links[head].next = head;
    links[head].prev = head;
}

MU_INLINE bool mu_index_list_empty(const mu_index_link *links, uint32_t head)
{
    return links[head].next == head;
}

MU_INLINE void mu_index_list_insert_after(mu_index_link *links, uint32_t head, uint32_t node)
{
    links[node].next = links[head].next;
    links[node].prev = head;
    links[links[head].next].prev = node;
    links[head].next = node;
}

MU_INLINE void mu_index_list_remove(mu_index_link *links, uint32_t node)
{
    links[links[node].next].prev = links[node].prev;
    links[links[node].prev].next = links[node].next;
    links[node].next = node;
    links[node].prev = node;
}

// Freelist helpers (using mu_index_link::next)

MU_INLINE void mu_index_freelist_init(mu_index_link *links, uint32_t head)
{
    links[head].next = head;
}

MU_INLINE bool mu_index_freelist_empty(const mu_index_link *links, uint32_t head)
{
    return links[head].next == head;
}

MU_INLINE void mu_index_freelist_push(mu_index_link *links, uint32_t head, uint32_t node)
{
    links[node].next = links[head].next;
    links[head].next = node;
}

MU_INLINE uint32_t mu_index_freelist_pop(mu_index_link *links, uint32_t head)
{
    uint32_t first = links[head].next;
    if (first == head)
        return UINT32_MAX;
    links[head].next = links[first].next;
    links[first].next = first;
    return first;
}

MU_END_EXTERN_C

#endif // MU_MIN_CONTAINERS_H
