


#pragma once

#include "mu_common.h"
typedef struct
{
    uint8_t* memory;
    uint32_t size;
} mu_buffer;

typedef struct
{
    mu_buffer buffer;
    uint32_t  head;
} mu_linear_allocator;


MU_INLINE void mu_linear_init(mu_linear_allocator* a, void* memory, uint32_t size)
{
    a->buffer.memory = (uint8_t*)memory;
    a->buffer.size   = size;
    a->head          = 0;
}


MU_INLINE void* mu_linear_alloc(mu_linear_allocator* a, uint32_t size, uint32_t align)
{
    uint32_t head = MU_ALIGN_UP(a->head, align);

    if(head + size > a->buffer.size)
        return NULL;

    void* ptr = a->buffer.memory + head;
    a->head   = head + size;

    return ptr;
}
MU_INLINE void mu_linear_reset(mu_linear_allocator* a)
{
    a->head = 0;
}
typedef struct
{
    mu_buffer buffer;

    uint32_t head;
    uint32_t tail;

} mu_ring_allocator;
MU_INLINE void mu_ring_init(mu_ring_allocator* r, void* memory, uint32_t size)
{
    r->buffer.memory = (uint8_t*)memory;
    r->buffer.size   = size;
    r->head          = 0;
    r->tail          = 0;
}
MU_INLINE void* mu_ring_alloc(mu_ring_allocator* r, uint32_t size, uint32_t align, uint32_t* out_offset)
{
    uint32_t head = MU_ALIGN_UP(r->head, align);

    // Case 1: normal region
    if(head >= r->tail)
    {
        if(head + size <= r->buffer.size)
        {
            *out_offset = head;
            r->head     = head + size;
            return r->buffer.memory + head;
        }

        // wrap
        head = 0;
    }

    // Case 2: after wrap
    if(head + size <= r->tail)
    {
        *out_offset = head;
        r->head     = head + size;
        return r->buffer.memory + head;
    }

    return NULL;
}
MU_INLINE void mu_ring_free_to(mu_ring_allocator* r, uint32_t offset)
{
    r->tail = offset;
}

MU_INLINE uint32_t mu_ring_used(const mu_ring_allocator* r)
{
    if(r->head >= r->tail)
        return r->head - r->tail;

    return (r->buffer.size - r->tail) + r->head;
}

