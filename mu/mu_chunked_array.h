
#pragma once

#include "mu_common.h"
// -----------------------------------------------------------------------------
// Data Structures Part 3: Arrays of arrays (fixed-size child chunks)
//
// Multiple logical arrays can share one chunk pool. Each logical array is
// represented by first/last chunk plus total count.
// -----------------------------------------------------------------------------

#define MU_CHUNKED_U32_NONE UINT32_MAX

#ifndef MU_ARRAY_OF_ARRAYS_CHUNK_SIZE
#define MU_ARRAY_OF_ARRAYS_CHUNK_SIZE 14u
#endif

typedef struct mu_chunked_u32_chunk
{
    uint32_t values[MU_ARRAY_OF_ARRAYS_CHUNK_SIZE];
    uint32_t used;
    uint32_t prev_chunk;
    uint32_t next_chunk;
    uint32_t free_next;
} mu_chunked_u32_chunk;

typedef struct mu_chunked_u32_pool
{
    mu_chunked_u32_chunk* chunks;
    uint32_t              chunk_count;
    uint32_t              chunk_capacity;
    uint32_t              free_head;
} mu_chunked_u32_pool;

typedef struct mu_chunked_u32_array
{
    uint32_t first_chunk;
    uint32_t last_chunk;
    uint32_t count;
} mu_chunked_u32_array;

typedef bool (*mu_chunked_u32_visit_fn)(uint32_t value, void* user);

void mu_chunked_u32_pool_init(mu_chunked_u32_pool* pool, uint32_t initial_capacity);
void mu_chunked_u32_pool_deinit(mu_chunked_u32_pool* pool);

void mu_chunked_u32_array_init(mu_chunked_u32_array* array);
void mu_chunked_u32_array_clear(mu_chunked_u32_pool* pool, mu_chunked_u32_array* array);
bool mu_chunked_u32_array_push(mu_chunked_u32_pool* pool, mu_chunked_u32_array* array, uint32_t value);
bool mu_chunked_u32_array_pop(mu_chunked_u32_pool* pool, mu_chunked_u32_array* array, uint32_t* out_value);
bool mu_chunked_u32_array_get(const mu_chunked_u32_pool* pool, const mu_chunked_u32_array* array, uint32_t index,
    uint32_t* out_value);
void mu_chunked_u32_array_visit(
    const mu_chunked_u32_pool* pool, const mu_chunked_u32_array* array, mu_chunked_u32_visit_fn visitor, void* user);

