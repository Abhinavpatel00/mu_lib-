
#include "mu_chunked_array.h"
static bool mu_chunked_u32_pool_grow(mu_chunked_u32_pool* pool, uint32_t min_capacity)
{
    uint32_t old_capacity = pool->chunk_capacity;
    uint32_t new_capacity = old_capacity ? old_capacity * 2 : 64;
    while(new_capacity < min_capacity)
        new_capacity *= 2;

    mu_chunked_u32_chunk* new_chunks = (mu_chunked_u32_chunk*)MU_REALLOC(pool->chunks, sizeof(mu_chunked_u32_chunk) * new_capacity);
    if(!new_chunks)
        return false;

    pool->chunks = new_chunks;

    for(uint32_t i = old_capacity; i < new_capacity; ++i)
    {
        pool->chunks[i].used       = 0;
        pool->chunks[i].prev_chunk = MU_CHUNKED_U32_NONE;
        pool->chunks[i].next_chunk = MU_CHUNKED_U32_NONE;
        pool->chunks[i].free_next  = (i + 1 < new_capacity) ? (i + 1) : pool->free_head;
    }

    pool->free_head      = old_capacity;
    pool->chunk_capacity = new_capacity;
    return true;
}

static uint32_t mu_chunked_u32_pool_alloc_chunk(mu_chunked_u32_pool* pool)
{
    if(pool->free_head == MU_CHUNKED_U32_NONE)
    {
        if(!mu_chunked_u32_pool_grow(pool, pool->chunk_capacity + 1))
            return MU_CHUNKED_U32_NONE;
    }

    uint32_t              chunk_index = pool->free_head;
    mu_chunked_u32_chunk* chunk       = &pool->chunks[chunk_index];
    pool->free_head                   = chunk->free_next;

    chunk->used       = 0;
    chunk->prev_chunk = MU_CHUNKED_U32_NONE;
    chunk->next_chunk = MU_CHUNKED_U32_NONE;
    chunk->free_next  = MU_CHUNKED_U32_NONE;

    if(chunk_index >= pool->chunk_count)
        pool->chunk_count = chunk_index + 1;

    return chunk_index;
}

static void mu_chunked_u32_pool_free_chunk(mu_chunked_u32_pool* pool, uint32_t chunk_index)
{
    mu_chunked_u32_chunk* chunk = &pool->chunks[chunk_index];
    chunk->used                 = 0;
    chunk->prev_chunk           = MU_CHUNKED_U32_NONE;
    chunk->next_chunk           = MU_CHUNKED_U32_NONE;
    chunk->free_next            = pool->free_head;
    pool->free_head             = chunk_index;
}

void mu_chunked_u32_pool_init(mu_chunked_u32_pool* pool, uint32_t initial_capacity)
{
    MU_MEMSET(pool, 0, sizeof(*pool));
    pool->free_head = MU_CHUNKED_U32_NONE;

    if(initial_capacity)
        mu_chunked_u32_pool_grow(pool, initial_capacity);
}

void mu_chunked_u32_pool_deinit(mu_chunked_u32_pool* pool)
{
    MU_FREE(pool->chunks);
    MU_MEMSET(pool, 0, sizeof(*pool));
    pool->free_head = MU_CHUNKED_U32_NONE;
}

void mu_chunked_u32_array_init(mu_chunked_u32_array* array)
{
    array->first_chunk = MU_CHUNKED_U32_NONE;
    array->last_chunk  = MU_CHUNKED_U32_NONE;
    array->count       = 0;
}

void mu_chunked_u32_array_clear(mu_chunked_u32_pool* pool, mu_chunked_u32_array* array)
{
    uint32_t chunk_index = array->first_chunk;
    while(chunk_index != MU_CHUNKED_U32_NONE)
    {
        uint32_t next = pool->chunks[chunk_index].next_chunk;
        mu_chunked_u32_pool_free_chunk(pool, chunk_index);
        chunk_index = next;
    }

    mu_chunked_u32_array_init(array);
}

bool mu_chunked_u32_array_push(mu_chunked_u32_pool* pool, mu_chunked_u32_array* array, uint32_t value)
{
    if(array->last_chunk == MU_CHUNKED_U32_NONE)
    {
        uint32_t first = mu_chunked_u32_pool_alloc_chunk(pool);
        if(first == MU_CHUNKED_U32_NONE)
            return false;

        array->first_chunk = first;
        array->last_chunk  = first;
    }

    uint32_t              last_index = array->last_chunk;
    mu_chunked_u32_chunk* last       = &pool->chunks[last_index];
    if(last->used == MU_ARRAY_OF_ARRAYS_CHUNK_SIZE)
    {
        uint32_t next = mu_chunked_u32_pool_alloc_chunk(pool);
        if(next == MU_CHUNKED_U32_NONE)
            return false;

        pool->chunks[next].prev_chunk       = last_index;
        pool->chunks[last_index].next_chunk = next;
        array->last_chunk                   = next;
        last                                = &pool->chunks[next];
    }

    last->values[last->used] = value;
    last->used += 1;
    array->count += 1;
    return true;
}

bool mu_chunked_u32_array_pop(mu_chunked_u32_pool* pool, mu_chunked_u32_array* array, uint32_t* out_value)
{
    if(array->count == 0)
        return false;

    uint32_t              last_index = array->last_chunk;
    mu_chunked_u32_chunk* last       = &pool->chunks[last_index];
    last->used -= 1;

    if(out_value)
        *out_value = last->values[last->used];

    array->count -= 1;

    if(last->used == 0)
    {
        uint32_t prev = last->prev_chunk;
        if(prev != MU_CHUNKED_U32_NONE)
        {
            pool->chunks[prev].next_chunk = MU_CHUNKED_U32_NONE;
            array->last_chunk             = prev;
        }
        else
        {
            array->first_chunk = MU_CHUNKED_U32_NONE;
            array->last_chunk  = MU_CHUNKED_U32_NONE;
        }

        mu_chunked_u32_pool_free_chunk(pool, last_index);
    }

    return true;
}

bool mu_chunked_u32_array_get(const mu_chunked_u32_pool* pool, const mu_chunked_u32_array* array, uint32_t index, uint32_t* out_value)
{
    if(index >= array->count)
        return false;

    uint32_t chunk_index = array->first_chunk;
    uint32_t local_index = index;
    while(chunk_index != MU_CHUNKED_U32_NONE)
    {
        const mu_chunked_u32_chunk* chunk = &pool->chunks[chunk_index];
        if(local_index < chunk->used)
        {
            *out_value = chunk->values[local_index];
            return true;
        }

        local_index -= chunk->used;
        chunk_index = chunk->next_chunk;
    }

    return false;
}

void mu_chunked_u32_array_visit(const mu_chunked_u32_pool* pool, const mu_chunked_u32_array* array, mu_chunked_u32_visit_fn visitor, void* user)
{
    uint32_t chunk_index = array->first_chunk;
    while(chunk_index != MU_CHUNKED_U32_NONE)
    {
        const mu_chunked_u32_chunk* chunk = &pool->chunks[chunk_index];
        for(uint32_t i = 0; i < chunk->used; ++i)
        {
            if(!visitor(chunk->values[i], user))
                return;
        }
        chunk_index = chunk->next_chunk;
    }
}
