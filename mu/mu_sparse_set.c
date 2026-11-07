
#include "mu_array.h"
#include "mu_sparse_set.h"
#include <string.h>

void mu_sparse_set_init(mu_sparse_set* set, uint32_t capacity)
{
    MU_ASSERT(set);

    set->dense    = NULL;
    set->sparse   = NULL;
    set->count    = 0;
    set->capacity = capacity;

    array_reserve(set->dense, capacity);
    array_reserve(set->sparse, capacity);

    /*
        We need actual logical size = capacity,
        because sparse[value] is indexed directly.

        dense is only iterated up to count,
        but giving it full size keeps both arrays symmetric
        and avoids weird half-initialized nonsense.
    */
    array_header(set->dense)->size  = capacity;
    array_header(set->sparse)->size = capacity;

    /*
        Sparse contents are technically "stale until proven valid",
        but zeroing removes debugger noise and makes the universe
        marginally less cursed.
    */
    MU_MEMSET(set->dense, 0, capacity * sizeof(uint32_t));
    MU_MEMSET(set->sparse, 0, capacity * sizeof(uint32_t));
}

void mu_sparse_set_destroy(mu_sparse_set* set)
{
    MU_ASSERT(set);

    array_free(set->dense);
    array_free(set->sparse);

    set->dense    = NULL;
    set->sparse   = NULL;
    set->count    = 0;
    set->capacity = 0;
}

void mu_sparse_set_clear(mu_sparse_set* set)
{
    MU_ASSERT(set);
    set->count = 0;
}

bool mu_sparse_set_contains(const mu_sparse_set* set, uint32_t value)
{
    MU_ASSERT(set);

    if(value >= set->capacity)
        return false;

    uint32_t idx = set->sparse[value];

    return (idx < set->count) && (set->dense[idx] == value);
}

bool mu_sparse_set_insert(mu_sparse_set* set, uint32_t value)
{
    MU_ASSERT(set);

    if(value >= set->capacity)
        return false;

    if(mu_sparse_set_contains(set, value))
        return false;

    uint32_t idx = set->count++;

    set->dense[idx]    = value;
    set->sparse[value] = idx;

    return true;
}

bool mu_sparse_set_remove(mu_sparse_set* set, uint32_t value)
{
    MU_ASSERT(set);

    if(!mu_sparse_set_contains(set, value))
        return false;

    uint32_t idx      = set->sparse[value];
    uint32_t last_idx = set->count - 1;
    uint32_t last_val = set->dense[last_idx];

    set->dense[idx]       = last_val;
    set->sparse[last_val] = idx;
    set->count--;

    return true;
}
