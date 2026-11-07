#include "mu_bulk_storage.h"
MU_INLINE uint32_t* mu_bulk_storage_free_header(mu_bulk_storage* storage)
{
    return (uint32_t*)(storage->slots + storage->slot_size * 0u);
}

MU_INLINE uint32_t* mu_bulk_storage_slot_next_ptr(mu_bulk_storage* storage, uint32_t id)
{
    return (uint32_t*)(storage->slots + storage->slot_size * id);
}

static bool mu_bulk_storage_grow(mu_bulk_storage* storage, uint32_t min_capacity)
{
    uint32_t new_capacity = storage->slot_capacity ? storage->slot_capacity * 2u : 64u;
    while(new_capacity < min_capacity)
        new_capacity *= 2u;

    uint8_t*  new_slots       = (uint8_t*)MU_REALLOC(storage->slots, storage->slot_size * (size_t)new_capacity);
    uint32_t* new_generations = (uint32_t*)MU_REALLOC(storage->generations, sizeof(uint32_t) * (size_t)new_capacity);
    uint8_t*  new_live        = (uint8_t*)MU_REALLOC(storage->live, sizeof(uint8_t) * (size_t)new_capacity);

    if(!new_slots || !new_generations || !new_live)
    {
        return false;
    }

    uint32_t old_capacity  = storage->slot_capacity;
    storage->slots         = new_slots;
    storage->generations   = new_generations;
    storage->live          = new_live;
    storage->slot_capacity = new_capacity;

    MU_MEMSET(storage->slots + storage->slot_size * (size_t)old_capacity, 0, storage->slot_size * (size_t)(new_capacity - old_capacity));
    MU_MEMSET(storage->generations + old_capacity, 0, sizeof(uint32_t) * (size_t)(new_capacity - old_capacity));
    MU_MEMSET(storage->live + old_capacity, 0, sizeof(uint8_t) * (size_t)(new_capacity - old_capacity));

    return true;
}

bool mu_bulk_storage_init(mu_bulk_storage* storage, size_t slot_size, uint32_t initial_slot_capacity)
{
    MU_MEMSET(storage, 0, sizeof(*storage));
    if(slot_size < sizeof(uint32_t))
        return false;

    storage->slot_size = slot_size;

    uint32_t cap = initial_slot_capacity < 2u ? 2u : initial_slot_capacity;
    if(!mu_bulk_storage_grow(storage, cap))
        return false;

    storage->next_unused                  = 1u;
    *mu_bulk_storage_free_header(storage) = 0u;
    return true;
}

void mu_bulk_storage_deinit(mu_bulk_storage* storage)
{
    MU_FREE(storage->slots);
    MU_FREE(storage->generations);
    MU_FREE(storage->live);
    MU_MEMSET(storage, 0, sizeof(*storage));
}

uint32_t mu_bulk_storage_alloc(mu_bulk_storage* storage)
{
    uint32_t id = *mu_bulk_storage_free_header(storage);
    if(id != 0u)
    {
        *mu_bulk_storage_free_header(storage) = *mu_bulk_storage_slot_next_ptr(storage, id);
    }
    else
    {
        id = storage->next_unused;
        if(id >= storage->slot_capacity)
        {
            if(!mu_bulk_storage_grow(storage, storage->slot_capacity + 1u))
                return 0u;
        }
        storage->next_unused = id + 1u;
        if(storage->generations[id] == 0u)
            storage->generations[id] = 1u;
    }

    storage->live[id] = 1u;
    storage->live_count += 1u;
    return id;
}

bool mu_bulk_storage_free(mu_bulk_storage* storage, uint32_t id)
{
    if(id == 0u || id >= storage->next_unused)
        return false;
    if(!storage->live[id])
        return false;

    storage->live[id] = 0u;
    storage->live_count -= 1u;

    storage->generations[id] += 1u;
    if(storage->generations[id] == 0u)
        storage->generations[id] = 1u;

    uint32_t* slot_next                   = mu_bulk_storage_slot_next_ptr(storage, id);
    *slot_next                            = *mu_bulk_storage_free_header(storage);
    *mu_bulk_storage_free_header(storage) = id;
    return true;
}

void* mu_bulk_storage_ptr(mu_bulk_storage* storage, uint32_t id)
{
    if(id == 0u || id >= storage->next_unused)
        return NULL;
    if(!storage->live[id])
        return NULL;
    return storage->slots + storage->slot_size * (size_t)id;
}

const void* mu_bulk_storage_ptr_const(const mu_bulk_storage* storage, uint32_t id)
{
    if(id == 0u || id >= storage->next_unused)
        return NULL;
    if(!storage->live[id])
        return NULL;
    return storage->slots + storage->slot_size * (size_t)id;
}

bool mu_bulk_storage_is_live(const mu_bulk_storage* storage, uint32_t id)
{
    if(id == 0u || id >= storage->next_unused)
        return false;
    return storage->live[id] != 0u;
}

mu_weak_handle mu_bulk_storage_make_handle(const mu_bulk_storage* storage, uint32_t id)
{
    mu_weak_handle handle;
    handle.id         = id;
    handle.generation = (id < storage->next_unused) ? storage->generations[id] : 0u;
    return handle;
}

bool mu_bulk_storage_validate_handle(const mu_bulk_storage* storage, mu_weak_handle handle)
{
    if(handle.id == 0u || handle.id >= storage->next_unused)
        return false;
    if(!storage->live[handle.id])
        return false;
    return storage->generations[handle.id] == handle.generation;
}

void* mu_bulk_storage_resolve_handle(mu_bulk_storage* storage, mu_weak_handle handle)
{
    if(!mu_bulk_storage_validate_handle(storage, handle))
        return NULL;
    return storage->slots + storage->slot_size * (size_t)handle.id;
}

const void* mu_bulk_storage_resolve_handle_const(const mu_bulk_storage* storage, mu_weak_handle handle)
{
    if(!mu_bulk_storage_validate_handle(storage, handle))
        return NULL;
    return storage->slots + storage->slot_size * (size_t)handle.id;
}

void mu_bulk_storage_visit_live(mu_bulk_storage* storage, mu_bulk_storage_visit_fn visitor, void* user)
{
    for(uint32_t id = 1u; id < storage->next_unused; ++id)
    {
        if(!storage->live[id])
            continue;

        void* slot_ptr = storage->slots + storage->slot_size * (size_t)id;
        if(!visitor(id, slot_ptr, user))
            return;
    }
}
