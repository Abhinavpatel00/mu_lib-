
#pragma  once 
#include "mu_common.h"

// -----------------------------------------------------------------------------
// Data Structures Part 2: Indices (key -> many values)
//
// Uses a hash map from key to first node and a circular doubly linked list of
// nodes per key for O(1) add/remove when node index is known.
// -----------------------------------------------------------------------------

#define MU_MULTI_INDEX_NONE UINT32_MAX

typedef struct mu_multi_index_node
{
    uint64_t key;
    uint32_t value;
    uint32_t prev;
    uint32_t next;
    uint32_t alive;
} mu_multi_index_node;

typedef struct mu_multi_index
{
    mu_multi_index_node* nodes;
    uint32_t             node_count;
    uint32_t             node_capacity;
    uint32_t             free_head;

    uint64_t* map_keys;
    uint32_t* map_values;
    uint8_t*  map_states;
    uint32_t  map_capacity;
    uint32_t  map_count;
} mu_multi_index;

typedef bool (*mu_multi_index_visit_fn)(uint32_t value, uint32_t node_index, void* user);

void     mu_multi_index_init(mu_multi_index* index, uint32_t initial_node_capacity, uint32_t initial_map_capacity);
void     mu_multi_index_deinit(mu_multi_index* index);
uint32_t mu_multi_index_add(mu_multi_index* index, uint64_t key, uint32_t value);
bool     mu_multi_index_remove(mu_multi_index* index, uint32_t node_index);
uint32_t mu_multi_index_first(const mu_multi_index* index, uint64_t key);
uint32_t mu_multi_index_next(const mu_multi_index* index, uint32_t start_node, uint32_t node_index);
bool     mu_multi_index_node_valid(const mu_multi_index* index, uint32_t node_index);
uint32_t mu_multi_index_value(const mu_multi_index* index, uint32_t node_index);
uint64_t mu_multi_index_key(const mu_multi_index* index, uint32_t node_index);
uint32_t mu_multi_index_count_key(const mu_multi_index* index, uint64_t key);
void     mu_multi_index_visit_key(const mu_multi_index* index, uint64_t key, mu_multi_index_visit_fn visitor, void* user);

