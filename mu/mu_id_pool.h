

#pragma  once 
#include "mu_common.h"

// What Problem It Solves
//
// You have IDs from:
//
// 0 ... maxID
//
// You want to:
//
// • allocate a single ID
// • allocate a contiguous block of IDs
// • free an ID or range
// • do it efficiently
// i mainly experimented with it for using it for bindless vulkan got it from nvpro and   https://www.humus.name/3D/MakeID.h
typedef struct
{
    uint32_t first;
    uint32_t last;
} mu_id_pool_range;

typedef struct
{
    mu_id_pool_range* ranges;
    uint32_t          count;
    uint32_t          capacity;
    uint32_t          max_id;
    uint32_t          used_ids;
} mu_id_pool;
void mu_id_pool_init(mu_id_pool* pool, uint32_t pool_size);
void mu_id_pool_deinit(mu_id_pool* pool);
void mu_id_pool_destroy_all(mu_id_pool* pool);

/* allocation */
bool mu_id_pool_create_id(mu_id_pool* pool, uint32_t* out_id);
bool mu_id_pool_create_range_id(mu_id_pool* pool, uint32_t* out_id, uint32_t count);

/* deallocation */
bool mu_id_pool_destroy_id(mu_id_pool* pool, uint32_t id);
bool mu_id_pool_destroy_range_id(mu_id_pool* pool, uint32_t id, uint32_t count);

/* queries */
bool mu_id_pool_is_range_available(const mu_id_pool* pool, uint32_t search_count);
void mu_id_pool_print_ranges(const mu_id_pool* pool);
void mu_id_pool_check_ranges(const mu_id_pool* pool);

uint32_t mu_id_pool_get_available_ids(const mu_id_pool* pool);
bool     mu_id_pool_is_id(const mu_id_pool* pool, uint32_t id);
uint32_t mu_id_pool_get_largest_continuous_range(const mu_id_pool* pool);

