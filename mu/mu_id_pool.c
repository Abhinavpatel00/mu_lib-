#include "mu_id_pool.h"



bool mu_id_pool_is_id(const mu_id_pool* pool, uint32_t id)
{
    uint32_t i0 = 0;
    uint32_t i1 = pool->count - 1;

    for(;;)
    {
        uint32_t i = (i0 + i1) / 2;

        if(id < pool->ranges[i].first)
        {
            if(i == i0)
                return true;

            i1 = i - 1;
        }
        else if(id > pool->ranges[i].last)
        {
            if(i == i1)
                return true;

            i0 = i + 1;
        }
        else
        {
            return false;
        }
    }
}

uint32_t mu_id_pool_get_available_ids(const mu_id_pool* pool)
{
    uint32_t count = pool->count;

    for(uint32_t i = 0; i < pool->count; ++i)
    {
        count += pool->ranges[i].last - pool->ranges[i].first;
    }

    return count;
}
uint32_t mu_id_pool_get_largest_continuous_range(const mu_id_pool* pool)
{
    uint32_t max_count = 0;

    for(uint32_t i = 0; i < pool->count; ++i)
    {
        uint32_t count = pool->ranges[i].last - pool->ranges[i].first + 1;

        if(count > max_count)
            max_count = count;
    }

    return max_count;
}
/* internal helpers */

static void mu_id_pool_insert_range(mu_id_pool* pool, uint32_t index)
{
    if(pool->count >= pool->capacity)
    {
        pool->capacity = pool->capacity ? pool->capacity * 2 : 1;
        pool->ranges   = (mu_id_pool_range*)MU_REALLOC(pool->ranges, pool->capacity * sizeof(mu_id_pool_range));
        MU_ASSERT(pool->ranges);
    }

    MU_MEMMOVE(pool->ranges + index + 1, pool->ranges + index, (pool->count - index) * sizeof(mu_id_pool_range));

    pool->count++;
}

static void mu_id_pool_destroy_range(mu_id_pool* pool, uint32_t index)
{
    pool->count--;

    MU_MEMMOVE(pool->ranges + index, pool->ranges + index + 1, (pool->count - index) * sizeof(mu_id_pool_range));
}

/* public API */

void mu_id_pool_init(mu_id_pool* pool, uint32_t pool_size)
{
    MU_ASSERT(pool);
    MU_ASSERT(!pool->ranges);
    MU_ASSERT(pool_size);

    uint32_t max_id = pool_size - 1;

    pool->ranges = (mu_id_pool_range*)MU_MALLOC(sizeof(mu_id_pool_range));
    MU_ASSERT(pool->ranges);

    pool->ranges[0].first = 0;
    pool->ranges[0].last  = max_id;

    pool->count    = 1;
    pool->capacity = 1;
    pool->max_id   = max_id;
    pool->used_ids = 0;
}

void mu_id_pool_deinit(mu_id_pool* pool)
{
    MU_ASSERT(pool);
    MU_ASSERT(pool->used_ids == 0);

    if(pool->ranges)
    {
        MU_FREE(pool->ranges);
        pool->ranges   = NULL;
        pool->count    = 0;
        pool->capacity = 0;
        pool->max_id   = 0;
        pool->used_ids = 0;
    }
}

void mu_id_pool_destroy_all(mu_id_pool* pool)
{
    uint32_t pool_size = pool->max_id + 1;
    pool->used_ids     = 0;

    mu_id_pool_deinit(pool);
    mu_id_pool_init(pool, pool_size);
}

bool mu_id_pool_create_id(mu_id_pool* pool, uint32_t* out_id)
{
    if(pool->ranges[0].first <= pool->ranges[0].last)
    {
        *out_id = pool->ranges[0].first;

        if(pool->ranges[0].first == pool->ranges[0].last && pool->count > 1)
        {
            mu_id_pool_destroy_range(pool, 0);
        }
        else
        {
            pool->ranges[0].first++;
        }

        pool->used_ids++;
        return true;
    }

    return false;
}

bool mu_id_pool_create_range_id(mu_id_pool* pool, uint32_t* out_id, uint32_t count)
{
    for(uint32_t i = 0; i < pool->count; ++i)
    {
        uint32_t range_count = 1 + pool->ranges[i].last - pool->ranges[i].first;

        if(count <= range_count)
        {
            *out_id = pool->ranges[i].first;

            if(count == range_count && i + 1 < pool->count)
            {
                mu_id_pool_destroy_range(pool, i);
            }
            else
            {
                pool->ranges[i].first += count;
            }

            pool->used_ids += count;
            return true;
        }
    }

    return false;
}

bool mu_id_pool_destroy_id(mu_id_pool* pool, uint32_t id)
{
    return mu_id_pool_destroy_range_id(pool, id, 1);
}

bool mu_id_pool_destroy_range_id(mu_id_pool* pool, uint32_t id, uint32_t count)
{
    uint32_t end_id = id + count;
    MU_ASSERT(end_id <= pool->max_id + 1);

    uint32_t i0 = 0;
    uint32_t i1 = pool->count - 1;

    for(;;)
    {
        uint32_t i = (i0 + i1) / 2;

        if(id < pool->ranges[i].first)
        {
            if(end_id >= pool->ranges[i].first)
            {
                if(end_id != pool->ranges[i].first)
                    return false;

                if(i > i0 && id - 1 == pool->ranges[i - 1].last)
                {
                    pool->ranges[i - 1].last = pool->ranges[i].last;
                    mu_id_pool_destroy_range(pool, i);
                }
                else
                {
                    pool->ranges[i].first = id;
                }

                pool->used_ids -= count;
                return true;
            }

            if(i != i0)
            {
                i1 = i - 1;
            }
            else
            {
                mu_id_pool_insert_range(pool, i);
                pool->ranges[i].first = id;
                pool->ranges[i].last  = end_id - 1;

                pool->used_ids -= count;
                return true;
            }
        }
        else if(id > pool->ranges[i].last)
        {
            if(id - 1 == pool->ranges[i].last)
            {
                if(i < i1 && end_id == pool->ranges[i + 1].first)
                {
                    pool->ranges[i].last = pool->ranges[i + 1].last;
                    mu_id_pool_destroy_range(pool, i + 1);
                }
                else
                {
                    pool->ranges[i].last += count;
                }

                pool->used_ids -= count;
                return true;
            }

            if(i != i1)
            {
                i0 = i + 1;
            }
            else
            {
                mu_id_pool_insert_range(pool, i + 1);
                pool->ranges[i + 1].first = id;
                pool->ranges[i + 1].last  = end_id - 1;

                pool->used_ids -= count;
                return true;
            }
        }
        else
        {
            return false;
        }
    }
}

bool mu_id_pool_is_range_available(const mu_id_pool* pool, uint32_t search_count)
{
    for(uint32_t i = 0; i < pool->count; ++i)
    {
        uint32_t count = pool->ranges[i].last - pool->ranges[i].first + 1;

        if(count >= search_count)
            return true;
    }

    return false;
}

void mu_id_pool_print_ranges(const mu_id_pool* pool)
{
    for(uint32_t i = 0; i < pool->count; ++i)
    {
        if(pool->ranges[i].first < pool->ranges[i].last)
            printf("%u-%u", pool->ranges[i].first, pool->ranges[i].last);
        else if(pool->ranges[i].first == pool->ranges[i].last)
            printf("%u", pool->ranges[i].first);
        else
            printf("-");

        if(i + 1 < pool->count)
            printf(", ");
    }

    printf("\n");
}

void mu_id_pool_check_ranges(const mu_id_pool* pool)
{
    for(uint32_t i = 0; i < pool->count; ++i)
    {
        MU_ASSERT(pool->ranges[i].last <= pool->max_id);

        if(pool->ranges[i].first == pool->ranges[i].last + 1)
            continue;

        MU_ASSERT(pool->ranges[i].first <= pool->ranges[i].last);
        MU_ASSERT(pool->ranges[i].first <= pool->max_id);
    }
}

