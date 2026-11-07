#ifndef MU_ECS_H
#define MU_ECS_H

/*
ECS: Data-oriented entity system in C99

This header follows the design in mu/docs/entity*.md:
- Entities are 30-bit weak handles (index + generation).
- Components live in dense arrays with an entity->dense map.
- Transforms use SoA arrays and store hierarchy links as indices.
- Transform updates are immediate: writing a local transform updates the subtree.

The comments include small ASCII sketches to keep the data layout tangible.
*/

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================
   Config
   ============================= */

#include "mu_macros.h"

/* forward declarations */
typedef struct mu_ecs_world mu_ecs_world;

#ifndef MU_ECS_ENTITY_INDEX_BITS
#define MU_ECS_ENTITY_INDEX_BITS 22u
#endif

#ifndef MU_ECS_ENTITY_GENERATION_BITS
#define MU_ECS_ENTITY_GENERATION_BITS 8u
#endif

#ifndef MU_ECS_INLINE_MASK_WORDS
#define MU_ECS_INLINE_MASK_WORDS 0u
#endif

#define MU_ECS_ENTITY_INDEX_MASK ((1u << MU_ECS_ENTITY_INDEX_BITS) - 1u)
#define MU_ECS_ENTITY_GENERATION_MASK ((1u << MU_ECS_ENTITY_GENERATION_BITS) - 1u)

/* =============================
   Entity IDs
   =============================

Bit layout (little endian):

|31                9|8         0|
+-------------------+-----------+
|   generation (8)  | index (22)|
+-------------------+-----------+
*/

typedef struct mu_ecs_entity {
    uint32_t id;
} mu_ecs_entity;

MU_INLINE uint32_t mu_ecs_entity_index(mu_ecs_entity e) {
    return e.id & MU_ECS_ENTITY_INDEX_MASK;
}

MU_INLINE uint32_t mu_ecs_entity_generation(mu_ecs_entity e) {
    return (e.id >> MU_ECS_ENTITY_INDEX_BITS) & MU_ECS_ENTITY_GENERATION_MASK;
}

MU_INLINE mu_ecs_entity mu_ecs_entity_make(uint32_t index, uint32_t generation) {
    mu_ecs_entity e;
    e.id = (generation << MU_ECS_ENTITY_INDEX_BITS) | (index & MU_ECS_ENTITY_INDEX_MASK);
    return e;
}

/* =============================
   Entity Manager
   =============================

We keep 1 byte per entity for generation, and a FIFO of free indices.

generation[]: [g0 g1 g2 ...]
free_queue : [idxA idxB idxC ...]

alive(e) => generation[e.index] == e.generation
*/

typedef struct mu_ecs_entity_manager {
    uint8_t* generation;
    uint32_t generation_count;
    uint32_t generation_capacity;

    uint32_t* free_queue;
    uint32_t free_head;
    uint32_t free_tail;
    uint32_t free_count;
    uint32_t free_capacity;

    uint32_t min_free;
} mu_ecs_entity_manager;

MU_INLINE void mu_ecs_entity_manager_init(mu_ecs_entity_manager* em, uint32_t min_free) {
    MU_MEMSET(em, 0, sizeof(*em));
    em->min_free = min_free;
}

MU_INLINE void mu_ecs_entity_manager_free(mu_ecs_entity_manager* em) {
    MU_FREE(em->generation);
    MU_FREE(em->free_queue);
    MU_MEMSET(em, 0, sizeof(*em));
}

MU_INLINE void mu_ecs_entity_manager_reserve(mu_ecs_entity_manager* em, uint32_t cap) {
    if (cap <= em->generation_capacity) {
        return;
    }
    uint32_t new_cap = em->generation_capacity ? em->generation_capacity : 64u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }
    em->generation = (uint8_t*)MU_REALLOC(em->generation, new_cap * sizeof(uint8_t));
    MU_MEMSET(em->generation + em->generation_capacity, 0,
                  (new_cap - em->generation_capacity) * sizeof(uint8_t));
    em->generation_capacity = new_cap;
}

MU_INLINE void mu_ecs_entity_manager_free_queue_reserve(mu_ecs_entity_manager* em, uint32_t cap) {
    if (cap <= em->free_capacity) {
        return;
    }
    uint32_t new_cap = em->free_capacity ? em->free_capacity : 64u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }
    uint32_t* new_q = (uint32_t*)MU_MALLOC(new_cap * sizeof(uint32_t));
    for (uint32_t i = 0; i < em->free_count; ++i) {
        uint32_t idx = (em->free_head + i) % em->free_capacity;
        new_q[i] = em->free_queue[idx];
    }
    MU_FREE(em->free_queue);
    em->free_queue = new_q;
    em->free_head = 0;
    em->free_tail = em->free_count;
    em->free_capacity = new_cap;
}

MU_INLINE mu_ecs_entity mu_ecs_entity_create(mu_ecs_entity_manager* em) {
    uint32_t idx;
    if (em->free_count > em->min_free) {
        idx = em->free_queue[em->free_head];
        em->free_head = (em->free_head + 1u) % em->free_capacity;
        --em->free_count;
    } else {
        mu_ecs_entity_manager_reserve(em, em->generation_count + 1u);
        idx = em->generation_count++;
        MU_ASSERT(idx < (1u << MU_ECS_ENTITY_INDEX_BITS));
    }
    return mu_ecs_entity_make(idx, em->generation[idx]);
}

MU_INLINE void mu_ecs_entity_create_batch(mu_ecs_entity_manager* em, mu_ecs_entity* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = mu_ecs_entity_create(em);
    }
}

MU_INLINE int mu_ecs_entity_alive(const mu_ecs_entity_manager* em, mu_ecs_entity e) {
    uint32_t idx = mu_ecs_entity_index(e);
    if (idx >= em->generation_count) {
        return 0;
    }
    return em->generation[idx] == (uint8_t)mu_ecs_entity_generation(e);
}

MU_INLINE void mu_ecs_entity_destroy(mu_ecs_entity_manager* em, mu_ecs_entity e) {
    uint32_t idx = mu_ecs_entity_index(e);
    if (idx >= em->generation_count) {
        return;
    }
    ++em->generation[idx];

    mu_ecs_entity_manager_free_queue_reserve(em, em->free_count + 1u);
    em->free_queue[em->free_tail] = idx;
    em->free_tail = (em->free_tail + 1u) % em->free_capacity;
    ++em->free_count;
}

/* =============================
   Dense Component Pool (AoS)
   =============================

Dense arrays + sparse map:

sparse[index] -> dense slot
entities[dense] -> entity id
payload[dense]  -> component data

Swap-erase keeps arrays compact.
*/

typedef struct mu_ecs_pool {
    uint32_t elem_size;

    uint32_t size;
    uint32_t capacity;

    mu_ecs_entity* entities;
    uint8_t* data;

    uint32_t* sparse;
    uint32_t sparse_capacity;
} mu_ecs_pool;

MU_INLINE void mu_ecs_pool_init(mu_ecs_pool* p, uint32_t elem_size) {
    MU_MEMSET(p, 0, sizeof(*p));
    p->elem_size = elem_size;
}

MU_INLINE void mu_ecs_pool_free(mu_ecs_pool* p) {
    MU_FREE(p->entities);
    MU_FREE(p->data);
    MU_FREE(p->sparse);
    MU_MEMSET(p, 0, sizeof(*p));
}

MU_INLINE void mu_ecs_pool_reserve(mu_ecs_pool* p, uint32_t cap) {
    if (cap <= p->capacity) {
        return;
    }
    uint32_t new_cap = p->capacity ? p->capacity : 64u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }
    p->entities = (mu_ecs_entity*)MU_REALLOC(p->entities, new_cap * sizeof(mu_ecs_entity));
    p->data = (uint8_t*)MU_REALLOC(p->data, new_cap * (size_t)p->elem_size);
    p->capacity = new_cap;
}

MU_INLINE void mu_ecs_pool_sparse_reserve(mu_ecs_pool* p, uint32_t cap) {
    if (cap <= p->sparse_capacity) {
        return;
    }
    uint32_t new_cap = p->sparse_capacity ? p->sparse_capacity : 64u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }
    p->sparse = (uint32_t*)MU_REALLOC(p->sparse, new_cap * sizeof(uint32_t));
    for (uint32_t i = p->sparse_capacity; i < new_cap; ++i) {
        p->sparse[i] = UINT32_MAX;
    }
    p->sparse_capacity = new_cap;
}

MU_INLINE int mu_ecs_pool_has(const mu_ecs_pool* p, mu_ecs_entity e) {
    uint32_t idx = mu_ecs_entity_index(e);
    if (idx >= p->sparse_capacity) {
        return 0;
    }
    uint32_t dense = p->sparse[idx];
    if (dense == UINT32_MAX || dense >= p->size) {
        return 0;
    }
    return p->entities[dense].id == e.id;
}

MU_INLINE void* mu_ecs_pool_get(const mu_ecs_pool* p, mu_ecs_entity e) {
    if (!mu_ecs_pool_has(p, e)) {
        return NULL;
    }
    uint32_t dense = p->sparse[mu_ecs_entity_index(e)];
    return p->data + (size_t)dense * p->elem_size;
}

MU_INLINE void* mu_ecs_pool_add(mu_ecs_pool* p, mu_ecs_entity e) {
    uint32_t idx = mu_ecs_entity_index(e);
    mu_ecs_pool_sparse_reserve(p, idx + 1u);

    uint32_t dense = p->sparse[idx];
    if (dense != UINT32_MAX && dense < p->size && p->entities[dense].id == e.id) {
        return p->data + (size_t)dense * p->elem_size;
    }

    mu_ecs_pool_reserve(p, p->size + 1u);
    dense = p->size++;

    p->entities[dense] = e;
    p->sparse[idx] = dense;

    void* slot = p->data + (size_t)dense * p->elem_size;
    MU_MEMSET(slot, 0, p->elem_size);
    return slot;
}

MU_INLINE void mu_ecs_pool_remove(mu_ecs_pool* p, mu_ecs_entity e) {
    if (!mu_ecs_pool_has(p, e)) {
        return;
    }
    uint32_t dense = p->sparse[mu_ecs_entity_index(e)];
    uint32_t last = p->size - 1u;

    if (dense != last) {
        p->entities[dense] = p->entities[last];
        MU_MEMCPY(p->data + (size_t)dense * p->elem_size,
                      p->data + (size_t)last * p->elem_size,
                      p->elem_size);

        p->sparse[mu_ecs_entity_index(p->entities[dense])] = dense;
    }

    p->sparse[mu_ecs_entity_index(e)] = UINT32_MAX;
    --p->size;
}

/* =============================
   Transform Component (SoA)
   =============================

Layout (single backing buffer, SoA slices):

+---------+-----------+-----------+-----------+------------+------------+------------+
| entity[]| local[]   | world[]   | parent[]  | first[]    | next[]     | prev[]     |
+---------+-----------+-----------+-----------+------------+------------+------------+

Hierarchy indices:

parent[i] ----> index of parent instance (-1 = root)
first_child[i] -> child0 -> child1 -> ... (linked by next_sibling)
prev_sibling/next_sibling keep sibling chains stable under swap-erase.
*/

typedef struct mu_ecs_instance {
    int32_t i;
} mu_ecs_instance;

#define MU_ECS_INSTANCE_INVALID (mu_ecs_instance){-1}

MU_INLINE int mu_ecs_instance_valid(mu_ecs_instance h) {
    return h.i >= 0;
}

typedef struct mu_ecs_transform_store {
    uint32_t size;
    uint32_t capacity;

    void* buffer;

    mu_ecs_entity* entity;
    float* local;  /* 16 floats per matrix */
    float* world;  /* 16 floats per matrix */
    int32_t* parent;
    int32_t* first_child;
    int32_t* next_sibling;
    int32_t* prev_sibling;

    uint32_t* sparse;
    uint32_t sparse_capacity;

    int32_t* stack;
    uint32_t stack_capacity;
} mu_ecs_transform_store;

MU_INLINE void mu_ecs_transform_init(mu_ecs_transform_store* t) {
    MU_MEMSET(t, 0, sizeof(*t));
}

MU_INLINE void mu_ecs_transform_free(mu_ecs_transform_store* t) {
    MU_FREE(t->buffer);
    MU_FREE(t->sparse);
    MU_FREE(t->stack);
    MU_MEMSET(t, 0, sizeof(*t));
}

MU_INLINE void mu_ecs_transform_sparse_reserve(mu_ecs_transform_store* t, uint32_t cap) {
    if (cap <= t->sparse_capacity) {
        return;
    }
    uint32_t new_cap = t->sparse_capacity ? t->sparse_capacity : 64u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }
    t->sparse = (uint32_t*)MU_REALLOC(t->sparse, new_cap * sizeof(uint32_t));
    for (uint32_t i = t->sparse_capacity; i < new_cap; ++i) {
        t->sparse[i] = UINT32_MAX;
    }
    t->sparse_capacity = new_cap;
}

MU_INLINE void mu_ecs_transform_stack_reserve(mu_ecs_transform_store* t, uint32_t cap) {
    if (cap <= t->stack_capacity) {
        return;
    }
    uint32_t new_cap = t->stack_capacity ? t->stack_capacity : 64u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }
    t->stack = (int32_t*)MU_REALLOC(t->stack, new_cap * sizeof(int32_t));
    t->stack_capacity = new_cap;
}

MU_INLINE void mu_ecs_transform_reserve(mu_ecs_transform_store* t, uint32_t cap) {
    if (cap <= t->capacity) {
        return;
    }
    uint32_t new_cap = t->capacity ? t->capacity : 64u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }

    size_t bytes = 0;
    bytes += new_cap * sizeof(mu_ecs_entity);
    bytes += new_cap * 16u * sizeof(float);
    bytes += new_cap * 16u * sizeof(float);
    bytes += new_cap * sizeof(int32_t);
    bytes += new_cap * sizeof(int32_t);
    bytes += new_cap * sizeof(int32_t);
    bytes += new_cap * sizeof(int32_t);

    void* new_buf = MU_MALLOC(bytes);
    uint8_t* p = (uint8_t*)new_buf;

    mu_ecs_entity* new_entity = (mu_ecs_entity*)p; p += new_cap * sizeof(mu_ecs_entity);
    float* new_local = (float*)p; p += new_cap * 16u * sizeof(float);
    float* new_world = (float*)p; p += new_cap * 16u * sizeof(float);
    int32_t* new_parent = (int32_t*)p; p += new_cap * sizeof(int32_t);
    int32_t* new_first = (int32_t*)p; p += new_cap * sizeof(int32_t);
    int32_t* new_next = (int32_t*)p; p += new_cap * sizeof(int32_t);
    int32_t* new_prev = (int32_t*)p; p += new_cap * sizeof(int32_t);

    if (t->size) {
        MU_MEMCPY(new_entity, t->entity, t->size * sizeof(mu_ecs_entity));
        MU_MEMCPY(new_local, t->local, t->size * 16u * sizeof(float));
        MU_MEMCPY(new_world, t->world, t->size * 16u * sizeof(float));
        MU_MEMCPY(new_parent, t->parent, t->size * sizeof(int32_t));
        MU_MEMCPY(new_first, t->first_child, t->size * sizeof(int32_t));
        MU_MEMCPY(new_next, t->next_sibling, t->size * sizeof(int32_t));
        MU_MEMCPY(new_prev, t->prev_sibling, t->size * sizeof(int32_t));
    }

    MU_FREE(t->buffer);
    t->buffer = new_buf;
    t->entity = new_entity;
    t->local = new_local;
    t->world = new_world;
    t->parent = new_parent;
    t->first_child = new_first;
    t->next_sibling = new_next;
    t->prev_sibling = new_prev;
    t->capacity = new_cap;
}

MU_INLINE void mu_ecs_transform_identity(float* m16) {
    MU_MEMSET(m16, 0, 16u * sizeof(float));
    m16[0] = 1.0f;
    m16[5] = 1.0f;
    m16[10] = 1.0f;
    m16[15] = 1.0f;
}

MU_INLINE void mu_ecs_transform_mul(float* out, const float* a, const float* b) {
    /* Column-major 4x4 multiply: out = a * b */
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c * 4 + r] =
                a[0 * 4 + r] * b[c * 4 + 0] +
                a[1 * 4 + r] * b[c * 4 + 1] +
                a[2 * 4 + r] * b[c * 4 + 2] +
                a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
}

MU_INLINE mu_ecs_instance mu_ecs_transform_find(const mu_ecs_transform_store* t, mu_ecs_entity e) {
    uint32_t idx = mu_ecs_entity_index(e);
    if (idx >= t->sparse_capacity) {
        return MU_ECS_INSTANCE_INVALID;
    }
    uint32_t dense = t->sparse[idx];
    if (dense == UINT32_MAX || dense >= t->size) {
        return MU_ECS_INSTANCE_INVALID;
    }
    if (t->entity[dense].id != e.id) {
        return MU_ECS_INSTANCE_INVALID;
    }
    return (mu_ecs_instance){(int32_t)dense};
}

MU_INLINE void mu_ecs_transform_link_child(mu_ecs_transform_store* t, int32_t parent, int32_t child) {
    t->parent[child] = parent;
    t->prev_sibling[child] = -1;
    t->next_sibling[child] = t->first_child[parent];
    if (t->first_child[parent] >= 0) {
        t->prev_sibling[t->first_child[parent]] = child;
    }
    t->first_child[parent] = child;
}

MU_INLINE void mu_ecs_transform_unlink(mu_ecs_transform_store* t, int32_t node) {
    int32_t p = t->parent[node];
    int32_t prev = t->prev_sibling[node];
    int32_t next = t->next_sibling[node];

    if (p >= 0) {
        if (t->first_child[p] == node) {
            t->first_child[p] = next;
        }
    }
    if (prev >= 0) {
        t->next_sibling[prev] = next;
    }
    if (next >= 0) {
        t->prev_sibling[next] = prev;
    }

    t->parent[node] = -1;
    t->prev_sibling[node] = -1;
    t->next_sibling[node] = -1;
}

MU_INLINE void mu_ecs_transform_propagate(mu_ecs_transform_store* t, int32_t root) {
    /*
    Immediate update: write local then recompute world for root and all descendants.
    We do a manual DFS with a tiny stack to avoid recursion depth surprises.
    */
    mu_ecs_transform_stack_reserve(t, t->size + 1u);

    uint32_t sp = 0;
    t->stack[sp++] = root;

    while (sp) {
        int32_t i = t->stack[--sp];
        int32_t p = t->parent[i];

        if (p >= 0) {
            mu_ecs_transform_mul(t->world + (size_t)i * 16u,
                                 t->world + (size_t)p * 16u,
                                 t->local + (size_t)i * 16u);
        } else {
            MU_MEMCPY(t->world + (size_t)i * 16u,
                          t->local + (size_t)i * 16u,
                          16u * sizeof(float));
        }

        for (int32_t c = t->first_child[i]; c >= 0; c = t->next_sibling[c]) {
            t->stack[sp++] = c;
        }
    }
}

MU_INLINE mu_ecs_instance mu_ecs_transform_add(mu_ecs_transform_store* t, mu_ecs_entity e) {
    uint32_t idx = mu_ecs_entity_index(e);
    mu_ecs_transform_sparse_reserve(t, idx + 1u);

    mu_ecs_instance existing = mu_ecs_transform_find(t, e);
    if (mu_ecs_instance_valid(existing)) {
        return existing;
    }

    mu_ecs_transform_reserve(t, t->size + 1u);
    uint32_t dense = t->size++;

    t->entity[dense] = e;
    t->sparse[idx] = dense;

    mu_ecs_transform_identity(t->local + (size_t)dense * 16u);
    mu_ecs_transform_identity(t->world + (size_t)dense * 16u);
    t->parent[dense] = -1;
    t->first_child[dense] = -1;
    t->next_sibling[dense] = -1;
    t->prev_sibling[dense] = -1;

    return (mu_ecs_instance){(int32_t)dense};
}

MU_INLINE void mu_ecs_transform_set_local(mu_ecs_transform_store* t, mu_ecs_instance h, const float* m16) {
    MU_MEMCPY(t->local + (size_t)h.i * 16u, m16, 16u * sizeof(float));
    mu_ecs_transform_propagate(t, h.i);
}

MU_INLINE void mu_ecs_transform_set_local_raw(mu_ecs_transform_store* t, mu_ecs_instance h, const float* m16) {
    /* Copy only, no propagation. Useful for batch spawn then one propagate per root. */
    MU_MEMCPY(t->local + (size_t)h.i * 16u, m16, 16u * sizeof(float));
}

MU_INLINE void mu_ecs_transform_set_parent(mu_ecs_transform_store* t, mu_ecs_instance child, mu_ecs_instance parent) {
    if (child.i == parent.i) {
        return;
    }
    if (mu_ecs_instance_valid(child)) {
        mu_ecs_transform_unlink(t, child.i);
    }
    if (mu_ecs_instance_valid(parent)) {
        mu_ecs_transform_link_child(t, parent.i, child.i);
        mu_ecs_transform_propagate(t, parent.i);
    } else if (mu_ecs_instance_valid(child)) {
        mu_ecs_transform_propagate(t, child.i);
    }
}

MU_INLINE void mu_ecs_transform_orphan_children(mu_ecs_transform_store* t, int32_t node) {
    for (int32_t c = t->first_child[node]; c >= 0; c = t->next_sibling[c]) {
        t->parent[c] = -1;
        t->prev_sibling[c] = -1;
    }
    t->first_child[node] = -1;
}

MU_INLINE void mu_ecs_transform_fix_moved_links(mu_ecs_transform_store* t, int32_t moved, int32_t dst) {
    int32_t p = t->parent[dst];
    int32_t prev = t->prev_sibling[dst];
    int32_t next = t->next_sibling[dst];

    if (p >= 0 && t->first_child[p] == moved) {
        t->first_child[p] = dst;
    }
    if (prev >= 0 && t->next_sibling[prev] == moved) {
        t->next_sibling[prev] = dst;
    }
    if (next >= 0 && t->prev_sibling[next] == moved) {
        t->prev_sibling[next] = dst;
    }

    for (int32_t c = t->first_child[dst]; c >= 0; c = t->next_sibling[c]) {
        t->parent[c] = dst;
    }
}

MU_INLINE void mu_ecs_transform_remove_at(mu_ecs_transform_store* t, int32_t idx) {
    int32_t last = (int32_t)t->size - 1;

    mu_ecs_transform_unlink(t, idx);
    mu_ecs_transform_orphan_children(t, idx);

    mu_ecs_entity removed_entity = t->entity[idx];
    t->sparse[mu_ecs_entity_index(removed_entity)] = UINT32_MAX;

    if (idx != last) {
        t->entity[idx] = t->entity[last];
        MU_MEMCPY(t->local + (size_t)idx * 16u, t->local + (size_t)last * 16u, 16u * sizeof(float));
        MU_MEMCPY(t->world + (size_t)idx * 16u, t->world + (size_t)last * 16u, 16u * sizeof(float));
        t->parent[idx] = t->parent[last];
        t->first_child[idx] = t->first_child[last];
        t->next_sibling[idx] = t->next_sibling[last];
        t->prev_sibling[idx] = t->prev_sibling[last];

        t->sparse[mu_ecs_entity_index(t->entity[idx])] = (uint32_t)idx;
        mu_ecs_transform_fix_moved_links(t, last, idx);
    }

    --t->size;
}

MU_INLINE void mu_ecs_transform_remove(mu_ecs_transform_store* t, mu_ecs_entity e) {
    mu_ecs_instance h = mu_ecs_transform_find(t, e);
    if (!mu_ecs_instance_valid(h)) {
        return;
    }
    mu_ecs_transform_remove_at(t, h.i);
}

/* =============================
   Spawn Helpers (group-by-component)
   =============================

Spawn data is grouped by component type:

entities: [e0 e1 e2 e3 ...]
comp A :  [A0 A1 A2 A3 ...]
comp B :  [B0 B1 B2 B3 ...]

This avoids per-entity structs and mirrors the docs: cache-friendly and easy to stream.
*/

/* The world struct must be complete before the spawn/prefab helpers below. */
struct mu_ecs_world {
    mu_ecs_entity_manager entities;
    mu_ecs_transform_store transforms;
};

typedef struct mu_ecs_spawn_block {
    uint32_t kind;          /* Caller-defined component id for prefab matching. */
    mu_ecs_pool* pool;      /* Destination component pool. */
    const void* data;       /* Optional array of component data. */
    uint32_t stride;        /* Bytes between items (0 = pool elem_size). */
} mu_ecs_spawn_block;

typedef struct mu_ecs_spawn_group {
    uint32_t count;

    const mu_ecs_entity* entities;   /* Optional. If NULL, entities are created. */
    const float* local_matrices;     /* Optional. count * 16 floats. */
    const int32_t* parent_index;     /* Optional. index into this group (-1 = root). */

    const mu_ecs_spawn_block* blocks;
    uint32_t block_count;
} mu_ecs_spawn_group;

MU_INLINE void mu_ecs_spawn_group_apply(mu_ecs_world* w, const mu_ecs_spawn_group* g, mu_ecs_entity* out_entities) {
    MU_ASSERT(w && g);
    MU_ASSERT(g->entities || out_entities);

    mu_ecs_entity* entities = (mu_ecs_entity*)(g->entities ? g->entities : out_entities);

    if (!g->entities) {
        mu_ecs_entity_create_batch(&w->entities, entities, g->count);
    }

    for (uint32_t b = 0; b < g->block_count; ++b) {
        const mu_ecs_spawn_block* block = &g->blocks[b];
        MU_ASSERT(block->pool);

        uint32_t stride = block->stride ? block->stride : block->pool->elem_size;
        const uint8_t* src = (const uint8_t*)block->data;

        for (uint32_t i = 0; i < g->count; ++i) {
            void* dst = mu_ecs_pool_add(block->pool, entities[i]);
            if (src) {
                MU_MEMCPY(dst, src + (size_t)i * stride, block->pool->elem_size);
            }
        }
    }

    if (g->local_matrices || g->parent_index) {
        mu_ecs_instance* inst = (mu_ecs_instance*)MU_MALLOC(g->count * sizeof(mu_ecs_instance));
        MU_ASSERT(inst);

        for (uint32_t i = 0; i < g->count; ++i) {
            inst[i] = mu_ecs_transform_add(&w->transforms, entities[i]);
            if (g->local_matrices) {
                mu_ecs_transform_set_local_raw(&w->transforms, inst[i], g->local_matrices + (size_t)i * 16u);
            }
        }

        if (g->parent_index) {
            for (uint32_t i = 0; i < g->count; ++i) {
                int32_t p = g->parent_index[i];
                if (p >= 0) {
                    mu_ecs_transform_link_child(&w->transforms, inst[p].i, inst[i].i);
                }
            }
        }

        for (uint32_t i = 0; i < g->count; ++i) {
            if (w->transforms.parent[inst[i].i] < 0) {
                mu_ecs_transform_propagate(&w->transforms, inst[i].i);
            }
        }

        MU_FREE(inst);
    }
}

/* =============================
    Prefab Overrides (Part 5 model)
   =============================

Prefab data is a read-only description for spawning entities.
Overrides let you keep a base prefab and patch per-entity component data,
with optional per-entity add/remove masks for component kinds.

The merge rule is per entity:
if override_mask[i] == 1 -> use override data
else -> fall back to base data
*/

typedef struct mu_ecs_prefab_component_block {
    uint32_t kind;              /* Component id, used to match override blocks. */
    mu_ecs_pool* pool;          /* Destination component pool. */
    const void* data;           /* Base or override component array. */
    uint32_t stride;            /* Bytes between items (0 = pool elem_size). */
    const uint8_t* override_mask; /* Optional: per entity mask. */
} mu_ecs_prefab_component_block;

#if MU_ECS_INLINE_MASK_WORDS > 0
typedef struct mu_ecs_inline_mask {
    uint64_t words[MU_ECS_INLINE_MASK_WORDS];
} mu_ecs_inline_mask;
#else
typedef struct mu_ecs_inline_mask {
    uint64_t words[1];
} mu_ecs_inline_mask;
#endif

typedef struct mu_ecs_prefab {
    uint32_t count;

    const float* local_matrices;      /* Optional: count * 16 floats. */
    const int32_t* parent_index;      /* Optional: parent index in prefab (-1 = root). */
    const uint8_t* local_override_mask;  /* Optional: per entity mask. */
    const uint8_t* parent_override_mask; /* Optional: per entity mask. */

    const uint64_t* add_mask;    /* Optional: per entity bitset to add components. */
    const uint64_t* remove_mask; /* Optional: per entity bitset to remove components. */
    const mu_ecs_inline_mask* add_mask_inline;    /* Optional: per entity inline masks. */
    const mu_ecs_inline_mask* remove_mask_inline; /* Optional: per entity inline masks. */
    uint32_t mask_words;         /* Number of uint64 words per entity in add/remove masks. */

    const mu_ecs_prefab_component_block* blocks;
    uint32_t block_count;
} mu_ecs_prefab;

MU_INLINE int mu_ecs_prefab_mask_has(const uint64_t* mask,
                                         uint32_t mask_words,
                                         uint32_t entity_index,
                                         uint32_t kind) {
    if (!mask || mask_words == 0) {
        return 0;
    }
    uint32_t word = kind / 64u;
    if (word >= mask_words) {
        return 0;
    }
    uint64_t bits = mask[entity_index * mask_words + word];
    return (int)((bits >> (kind & 63u)) & 1u);
}

MU_INLINE int mu_ecs_inline_mask_has(const mu_ecs_inline_mask* mask, uint32_t kind) {
#if MU_ECS_INLINE_MASK_WORDS == 0
    (void)mask;
    (void)kind;
    return 0;
#else
    if (!mask) {
        return 0;
    }
    uint32_t word = kind / 64u;
    if (word >= MU_ECS_INLINE_MASK_WORDS) {
        return 0;
    }
    return (int)((mask->words[word] >> (kind & 63u)) & 1u);
#endif
}

MU_INLINE int mu_ecs_inline_mask_array_has(const mu_ecs_inline_mask* masks,
                                               uint32_t entity_index,
                                               uint32_t kind) {
#if MU_ECS_INLINE_MASK_WORDS == 0
    (void)masks;
    (void)entity_index;
    (void)kind;
    return 0;
#else
    if (!masks) {
        return 0;
    }
    return mu_ecs_inline_mask_has(&masks[entity_index], kind);
#endif
}

MU_INLINE int mu_ecs_prefab_mask_has_any(const uint64_t* mask,
                                             const mu_ecs_inline_mask* inline_mask,
                                             uint32_t mask_words,
                                             uint32_t entity_index,
                                             uint32_t kind) {
    /* If both are provided, inline masks take precedence over pointer masks. */
    if (inline_mask) {
        return mu_ecs_inline_mask_array_has(inline_mask, entity_index, kind);
    }
    return mu_ecs_prefab_mask_has(mask, mask_words, entity_index, kind);
}

MU_INLINE const mu_ecs_prefab_component_block* mu_ecs_prefab_find_block(const mu_ecs_prefab* p, uint32_t kind) {
    if (!p) {
        return NULL;
    }
    for (uint32_t i = 0; i < p->block_count; ++i) {
        if (p->blocks[i].kind == kind) {
            return &p->blocks[i];
        }
    }
    return NULL;
}

MU_INLINE const void* mu_ecs_prefab_pick_data(const mu_ecs_prefab_component_block* base,
                                                  const mu_ecs_prefab_component_block* over,
                                                  uint32_t index,
                                                  uint32_t* out_stride) {
    if (over && over->data) {
        int use_override = 1;
        if (over->override_mask) {
            use_override = over->override_mask[index] != 0;
        }
        if (use_override) {
            *out_stride = over->stride ? over->stride : over->pool->elem_size;
            return (const uint8_t*)over->data + (size_t)index * (*out_stride);
        }
    }

    if (base && base->data) {
        *out_stride = base->stride ? base->stride : base->pool->elem_size;
        return (const uint8_t*)base->data + (size_t)index * (*out_stride);
    }

    *out_stride = 0;
    return NULL;
}

MU_INLINE void mu_ecs_prefab_spawn(mu_ecs_world* w,
                                       const mu_ecs_prefab* base,
                                       const mu_ecs_prefab* over,
                                       mu_ecs_entity* out_entities) {
    MU_ASSERT(w && base && out_entities);
    if (over && over->count) {
        MU_ASSERT(over->count == base->count);
    }

    uint32_t count = base->count;
    mu_ecs_entity_create_batch(&w->entities, out_entities, count);

    for (uint32_t b = 0; b < base->block_count; ++b) {
        const mu_ecs_prefab_component_block* base_block = &base->blocks[b];
        const mu_ecs_prefab_component_block* over_block = mu_ecs_prefab_find_block(over, base_block->kind);

        MU_ASSERT(base_block->pool);
        for (uint32_t i = 0; i < count; ++i) {
            if (over && mu_ecs_prefab_mask_has_any(over->remove_mask,
                                                   over->remove_mask_inline,
                                                   over->mask_words,
                                                   i,
                                                   base_block->kind)) {
                continue;
            }
            uint32_t stride = 0;
            const void* src = mu_ecs_prefab_pick_data(base_block, over_block, i, &stride);
            void* dst = mu_ecs_pool_add(base_block->pool, out_entities[i]);
            if (src) {
                MU_MEMCPY(dst, src, base_block->pool->elem_size);
            }
        }
    }

    if (over) {
        for (uint32_t b = 0; b < over->block_count; ++b) {
            const mu_ecs_prefab_component_block* over_block = &over->blocks[b];
            if (mu_ecs_prefab_find_block(base, over_block->kind)) {
                continue;
            }
            MU_ASSERT(over_block->pool);
            for (uint32_t i = 0; i < count; ++i) {
                if (mu_ecs_prefab_mask_has_any(over->remove_mask,
                                               over->remove_mask_inline,
                                               over->mask_words,
                                               i,
                                               over_block->kind)) {
                    continue;
                }
                if ((over->add_mask || over->add_mask_inline) &&
                    !mu_ecs_prefab_mask_has_any(over->add_mask,
                                                over->add_mask_inline,
                                                over->mask_words,
                                                i,
                                                over_block->kind)) {
                    continue;
                }
                uint32_t stride = over_block->stride ? over_block->stride : over_block->pool->elem_size;
                const void* src = (const uint8_t*)over_block->data + (size_t)i * stride;
                void* dst = mu_ecs_pool_add(over_block->pool, out_entities[i]);
                if (over_block->data && (!over_block->override_mask || over_block->override_mask[i])) {
                    MU_MEMCPY(dst, src, over_block->pool->elem_size);
                }
            }
        }
    }

    if (base->local_matrices || base->parent_index || (over && (over->local_matrices || over->parent_index))) {
        mu_ecs_instance* inst = (mu_ecs_instance*)MU_MALLOC(count * sizeof(mu_ecs_instance));
        MU_ASSERT(inst);

        for (uint32_t i = 0; i < count; ++i) {
            inst[i] = mu_ecs_transform_add(&w->transforms, out_entities[i]);

            const float* base_local = base->local_matrices;
            const float* over_local = over ? over->local_matrices : NULL;
            int use_override = 0;
            if (over_local) {
                use_override = over->local_override_mask ? (over->local_override_mask[i] != 0) : 1;
            }
            if (use_override) {
                mu_ecs_transform_set_local_raw(&w->transforms, inst[i], over_local + (size_t)i * 16u);
            } else if (base_local) {
                mu_ecs_transform_set_local_raw(&w->transforms, inst[i], base_local + (size_t)i * 16u);
            }
        }

        const int32_t* base_parent = base->parent_index;
        const int32_t* over_parent = over ? over->parent_index : NULL;
        for (uint32_t i = 0; i < count; ++i) {
            int use_override = 0;
            if (over_parent) {
                use_override = over->parent_override_mask ? (over->parent_override_mask[i] != 0) : 1;
            }
            int32_t p = use_override ? (over_parent ? over_parent[i] : -1) : (base_parent ? base_parent[i] : -1);
            if (p >= 0) {
                mu_ecs_transform_link_child(&w->transforms, inst[p].i, inst[i].i);
            }
        }

        for (uint32_t i = 0; i < count; ++i) {
            if (w->transforms.parent[inst[i].i] < 0) {
                mu_ecs_transform_propagate(&w->transforms, inst[i].i);
            }
        }

        MU_FREE(inst);
    }
}

/* =============================
   Resource Registry (lightweight)
   =============================

Registry for common pools and prefabs indexed by integer id.
Use it to avoid carrying pool pointers through spawn setups.
*/

typedef struct mu_ecs_resource_registry {
    mu_ecs_pool** pools;
    uint32_t pool_capacity;

    const mu_ecs_prefab** prefabs;
    uint32_t prefab_capacity;
} mu_ecs_resource_registry;

MU_INLINE void mu_ecs_resource_registry_init(mu_ecs_resource_registry* r) {
    MU_MEMSET(r, 0, sizeof(*r));
}

MU_INLINE void mu_ecs_resource_registry_free(mu_ecs_resource_registry* r) {
    MU_FREE(r->pools);
    MU_FREE(r->prefabs);
    MU_MEMSET(r, 0, sizeof(*r));
}

MU_INLINE void mu_ecs_resource_registry_pool_reserve(mu_ecs_resource_registry* r, uint32_t cap) {
    if (cap <= r->pool_capacity) {
        return;
    }
    uint32_t new_cap = r->pool_capacity ? r->pool_capacity : 32u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }
    r->pools = (mu_ecs_pool**)MU_REALLOC(r->pools, new_cap * sizeof(mu_ecs_pool*));
    for (uint32_t i = r->pool_capacity; i < new_cap; ++i) {
        r->pools[i] = NULL;
    }
    r->pool_capacity = new_cap;
}

MU_INLINE void mu_ecs_resource_registry_prefab_reserve(mu_ecs_resource_registry* r, uint32_t cap) {
    if (cap <= r->prefab_capacity) {
        return;
    }
    uint32_t new_cap = r->prefab_capacity ? r->prefab_capacity : 32u;
    while (new_cap < cap) {
        new_cap *= 2u;
    }
    r->prefabs = (const mu_ecs_prefab**)MU_REALLOC(r->prefabs, new_cap * sizeof(mu_ecs_prefab*));
    for (uint32_t i = r->prefab_capacity; i < new_cap; ++i) {
        r->prefabs[i] = NULL;
    }
    r->prefab_capacity = new_cap;
}

MU_INLINE void mu_ecs_resource_registry_set_pool(mu_ecs_resource_registry* r, uint32_t kind, mu_ecs_pool* pool) {
    mu_ecs_resource_registry_pool_reserve(r, kind + 1u);
    r->pools[kind] = pool;
}

MU_INLINE mu_ecs_pool* mu_ecs_resource_registry_get_pool(const mu_ecs_resource_registry* r, uint32_t kind) {
    if (!r || kind >= r->pool_capacity) {
        return NULL;
    }
    return r->pools[kind];
}

MU_INLINE void mu_ecs_resource_registry_set_prefab(mu_ecs_resource_registry* r, uint32_t id, const mu_ecs_prefab* prefab) {
    mu_ecs_resource_registry_prefab_reserve(r, id + 1u);
    r->prefabs[id] = prefab;
}

MU_INLINE const mu_ecs_prefab* mu_ecs_resource_registry_get_prefab(const mu_ecs_resource_registry* r, uint32_t id) {
    if (!r || id >= r->prefab_capacity) {
        return NULL;
    }
    return r->prefabs[id];
}

MU_INLINE void mu_ecs_prefab_blocks_bind_pools(mu_ecs_prefab_component_block* blocks,
                                                   uint32_t block_count,
                                                   const mu_ecs_resource_registry* r) {
    if (!blocks || !r) {
        return;
    }
    for (uint32_t i = 0; i < block_count; ++i) {
        if (!blocks[i].pool) {
            blocks[i].pool = mu_ecs_resource_registry_get_pool(r, blocks[i].kind);
        }
    }
}

/* =============================
   World (minimal)
   =============================

A world groups entity manager + component stores.
Expand this with your own components and systems.
*/

MU_INLINE void mu_ecs_world_init(mu_ecs_world* w, uint32_t min_free) {
    mu_ecs_entity_manager_init(&w->entities, min_free);
    mu_ecs_transform_init(&w->transforms);
}

MU_INLINE void mu_ecs_world_free(mu_ecs_world* w) {
    mu_ecs_transform_free(&w->transforms);
    mu_ecs_entity_manager_free(&w->entities);
}

/* =============================
   Example
   =============================

Example: create two entities, add transforms, parent child under root.

    mu_ecs_world world;
    mu_ecs_world_init(&world, 32);

    mu_ecs_entity root = mu_ecs_entity_create(&world.entities);
    mu_ecs_entity child = mu_ecs_entity_create(&world.entities);

    mu_ecs_instance root_t = mu_ecs_transform_add(&world.transforms, root);
    mu_ecs_instance child_t = mu_ecs_transform_add(&world.transforms, child);

    float root_m[16];
    float child_m[16];
    mu_ecs_transform_identity(root_m);
    mu_ecs_transform_identity(child_m);

    mu_ecs_transform_set_local(&world.transforms, root_t, root_m);
    mu_ecs_transform_set_local(&world.transforms, child_t, child_m);
    mu_ecs_transform_set_parent(&world.transforms, child_t, root_t);

    mu_ecs_world_free(&world);

Example: per-entity add/remove masks (inline bitset) in prefab override.

    #define MU_ECS_INLINE_MASK_WORDS 1

    enum { COMP_RENDER = 0, COMP_PHYS = 1, COMP_TAG = 2 };

    mu_ecs_inline_mask add_mask[2] = {0};
    mu_ecs_inline_mask remove_mask[2] = {0};

    add_mask[0].words[0] = (1ull << COMP_TAG);
    remove_mask[1].words[0] = (1ull << COMP_PHYS);

    mu_ecs_prefab over = {0};
    over.count = 2;
    over.add_mask_inline = add_mask;
    over.remove_mask_inline = remove_mask;

Example: registry binding for prefab blocks by kind.

    mu_ecs_resource_registry reg;
    mu_ecs_resource_registry_init(&reg);
    mu_ecs_resource_registry_set_pool(&reg, COMP_RENDER, &render_pool);

    mu_ecs_prefab_component_block blocks[1] = {
        { COMP_RENDER, NULL, render_data, sizeof(RenderComp), NULL }
    };
    mu_ecs_prefab_blocks_bind_pools(blocks, 1, &reg);

    mu_ecs_resource_registry_free(&reg);
*/

#ifdef __cplusplus
}
#endif

#endif /* MU_ECS_H */
