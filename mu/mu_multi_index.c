
#include "mu_hash_table.h"
#include "mu_multi_index.h"
enum
{
    MU_MULTI_INDEX_MAP_EMPTY = 0,
    MU_MULTI_INDEX_MAP_USED  = 1,
    MU_MULTI_INDEX_MAP_TOMB  = 2,
};

static uint32_t mu_multi_index_next_pow2_u32(uint32_t value)
{
    uint32_t n = 1;
    while(n < value)
        n <<= 1;
    return n;
}

static uint32_t mu_multi_index_map_find_slot(const mu_multi_index* index, uint64_t key)
{
    uint32_t cap       = index->map_capacity;
    uint32_t i         = (uint32_t)(hash_u64(key) % cap);
    uint32_t first_tomb = MU_MULTI_INDEX_NONE;

    for(;;)
    {
        uint8_t state = index->map_states[i];
        if(state == MU_MULTI_INDEX_MAP_EMPTY)
            return first_tomb != MU_MULTI_INDEX_NONE ? first_tomb : i;

        if(state == MU_MULTI_INDEX_MAP_TOMB)
        {
            if(first_tomb == MU_MULTI_INDEX_NONE)
                first_tomb = i;
        }
        else if(index->map_keys[i] == key)
        {
            return i;
        }

        i = (i + 1) % cap;
    }
}

static uint32_t mu_multi_index_map_get(const mu_multi_index* index, uint64_t key)
{
    uint32_t cap = index->map_capacity;
    if(cap == 0)
        return MU_MULTI_INDEX_NONE;

    uint32_t i = (uint32_t)(hash_u64(key) % cap);
    for(;;)
    {
        uint8_t state = index->map_states[i];
        if(state == MU_MULTI_INDEX_MAP_EMPTY)
            return MU_MULTI_INDEX_NONE;

        if(state == MU_MULTI_INDEX_MAP_USED && index->map_keys[i] == key)
            return index->map_values[i];

        i = (i + 1) % cap;
    }
}

static bool mu_multi_index_map_resize(mu_multi_index* index, uint32_t new_capacity)
{
    uint64_t* new_keys   = (uint64_t*)MU_MALLOC(sizeof(uint64_t) * new_capacity);
    uint32_t* new_values = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * new_capacity);
    uint8_t*  new_states = (uint8_t*)MU_MALLOC(sizeof(uint8_t) * new_capacity);

    if(!new_keys || !new_values || !new_states)
    {
        MU_FREE(new_keys);
        MU_FREE(new_values);
        MU_FREE(new_states);
        return false;
    }

    MU_MEMSET(new_states, MU_MULTI_INDEX_MAP_EMPTY, new_capacity * sizeof(uint8_t));

    uint64_t* old_keys    = index->map_keys;
    uint32_t* old_values  = index->map_values;
    uint8_t*  old_states  = index->map_states;
    uint32_t  old_capacity = index->map_capacity;

    index->map_keys     = new_keys;
    index->map_values   = new_values;
    index->map_states   = new_states;
    index->map_capacity = new_capacity;
    index->map_count    = 0;

    for(uint32_t i = 0; i < old_capacity; ++i)
    {
        if(old_states[i] == MU_MULTI_INDEX_MAP_USED)
        {
            uint32_t slot             = mu_multi_index_map_find_slot(index, old_keys[i]);
            index->map_keys[slot]     = old_keys[i];
            index->map_values[slot]   = old_values[i];
            index->map_states[slot]   = MU_MULTI_INDEX_MAP_USED;
            index->map_count         += 1;
        }
    }

    MU_FREE(old_keys);
    MU_FREE(old_values);
    MU_FREE(old_states);
    return true;
}

static bool mu_multi_index_map_set(mu_multi_index* index, uint64_t key, uint32_t value)
{
    if(index->map_capacity == 0)
    {
        if(!mu_multi_index_map_resize(index, 16))
            return false;
    }

    if((index->map_count + 1) * 10 >= index->map_capacity * 7)
    {
        if(!mu_multi_index_map_resize(index, index->map_capacity * 2))
            return false;
    }

    uint32_t slot = mu_multi_index_map_find_slot(index, key);
    if(index->map_states[slot] != MU_MULTI_INDEX_MAP_USED)
    {
        index->map_count += 1;
    }

    index->map_keys[slot]   = key;
    index->map_values[slot] = value;
    index->map_states[slot] = MU_MULTI_INDEX_MAP_USED;
    return true;
}

static void mu_multi_index_map_remove(mu_multi_index* index, uint64_t key)
{
    uint32_t cap = index->map_capacity;
    if(cap == 0)
        return;

    uint32_t i = (uint32_t)(hash_u64(key) % cap);
    for(;;)
    {
        uint8_t state = index->map_states[i];
        if(state == MU_MULTI_INDEX_MAP_EMPTY)
            return;

        if(state == MU_MULTI_INDEX_MAP_USED && index->map_keys[i] == key)
        {
            index->map_states[i] = MU_MULTI_INDEX_MAP_TOMB;
            index->map_count -= 1;
            return;
        }

        i = (i + 1) % cap;
    }
}

static bool mu_multi_index_grow_nodes(mu_multi_index* index, uint32_t min_capacity)
{
    uint32_t new_capacity = index->node_capacity ? index->node_capacity * 2 : 64;
    while(new_capacity < min_capacity)
        new_capacity *= 2;

    mu_multi_index_node* new_nodes =
        (mu_multi_index_node*)MU_REALLOC(index->nodes, sizeof(mu_multi_index_node) * new_capacity);
    if(!new_nodes)
        return false;

    index->nodes         = new_nodes;
    index->node_capacity = new_capacity;
    return true;
}

void mu_multi_index_init(mu_multi_index* index, uint32_t initial_node_capacity, uint32_t initial_map_capacity)
{
    MU_MEMSET(index, 0, sizeof(*index));
    index->free_head = MU_MULTI_INDEX_NONE;

    if(initial_node_capacity)
    {
        uint32_t cap = mu_multi_index_next_pow2_u32(initial_node_capacity);
        index->nodes = (mu_multi_index_node*)MU_MALLOC(sizeof(mu_multi_index_node) * cap);
        if(index->nodes)
            index->node_capacity = cap;
    }

    if(initial_map_capacity)
    {
        uint32_t cap = mu_multi_index_next_pow2_u32(initial_map_capacity);
        mu_multi_index_map_resize(index, cap < 16 ? 16 : cap);
    }
}

void mu_multi_index_deinit(mu_multi_index* index)
{
    MU_FREE(index->nodes);
    MU_FREE(index->map_keys);
    MU_FREE(index->map_values);
    MU_FREE(index->map_states);
    MU_MEMSET(index, 0, sizeof(*index));
    index->free_head = MU_MULTI_INDEX_NONE;
}

uint32_t mu_multi_index_add(mu_multi_index* index, uint64_t key, uint32_t value)
{
    uint32_t node_index;

    if(index->free_head != MU_MULTI_INDEX_NONE)
    {
        node_index       = index->free_head;
        index->free_head = index->nodes[node_index].next;
    }
    else
    {
        if(index->node_count == index->node_capacity)
        {
            if(!mu_multi_index_grow_nodes(index, index->node_count + 1))
                return MU_MULTI_INDEX_NONE;
        }

        node_index = index->node_count;
        index->node_count += 1;
    }

    uint32_t first = mu_multi_index_map_get(index, key);
    mu_multi_index_node* node = &index->nodes[node_index];

    node->key   = key;
    node->value = value;
    node->alive = 1;

    if(first == MU_MULTI_INDEX_NONE)
    {
        node->prev = node_index;
        node->next = node_index;
        if(!mu_multi_index_map_set(index, key, node_index))
        {
            node->alive = 0;
            node->next  = index->free_head;
            index->free_head = node_index;
            return MU_MULTI_INDEX_NONE;
        }
    }
    else
    {
        uint32_t second = index->nodes[first].next;
        node->prev = first;
        node->next = second;
        index->nodes[first].next = node_index;
        index->nodes[second].prev = node_index;
    }

    return node_index;
}

bool mu_multi_index_remove(mu_multi_index* index, uint32_t node_index)
{
    if(node_index >= index->node_count)
        return false;

    mu_multi_index_node* node = &index->nodes[node_index];
    if(!node->alive)
        return false;

    uint32_t prev = node->prev;
    uint32_t next = node->next;
    uint64_t key  = node->key;

    index->nodes[prev].next = next;
    index->nodes[next].prev = prev;

    uint32_t first = mu_multi_index_map_get(index, key);
    if(first == node_index)
    {
        if(next == node_index)
            mu_multi_index_map_remove(index, key);
        else
            mu_multi_index_map_set(index, key, next);
    }

    node->alive = 0;
    node->next  = index->free_head;
    index->free_head = node_index;
    return true;
}

uint32_t mu_multi_index_first(const mu_multi_index* index, uint64_t key)
{
    return mu_multi_index_map_get(index, key);
}

uint32_t mu_multi_index_next(const mu_multi_index* index, uint32_t start_node, uint32_t node_index)
{
    if(!mu_multi_index_node_valid(index, node_index))
        return MU_MULTI_INDEX_NONE;
    if(!mu_multi_index_node_valid(index, start_node))
        return MU_MULTI_INDEX_NONE;

    uint32_t next = index->nodes[node_index].next;
    return next == start_node ? MU_MULTI_INDEX_NONE : next;
}

bool mu_multi_index_node_valid(const mu_multi_index* index, uint32_t node_index)
{
    return node_index < index->node_count && index->nodes[node_index].alive != 0;
}

uint32_t mu_multi_index_value(const mu_multi_index* index, uint32_t node_index)
{
    return index->nodes[node_index].value;
}

uint64_t mu_multi_index_key(const mu_multi_index* index, uint32_t node_index)
{
    return index->nodes[node_index].key;
}

uint32_t mu_multi_index_count_key(const mu_multi_index* index, uint64_t key)
{
    uint32_t first = mu_multi_index_first(index, key);
    if(first == MU_MULTI_INDEX_NONE)
        return 0;

    uint32_t count = 0;
    uint32_t idx   = first;
    do
    {
        count += 1;
        idx = index->nodes[idx].next;
    } while(idx != first);

    return count;
}

void mu_multi_index_visit_key(const mu_multi_index* index, uint64_t key, mu_multi_index_visit_fn visitor, void* user)
{
    uint32_t first = mu_multi_index_first(index, key);
    if(first == MU_MULTI_INDEX_NONE)
        return;

    uint32_t idx = first;
    do
    {
        if(!visitor(index->nodes[idx].value, idx, user))
            return;
        idx = index->nodes[idx].next;
    } while(idx != first);
}

