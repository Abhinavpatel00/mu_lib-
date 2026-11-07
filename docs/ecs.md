# ECS: `mu_ecs.h`

A compact, data-oriented entity component toolkit. Not an archetype-ECS
framework — it provides the storage primitives (entity manager, sparse-set
component pools, hierarchical transforms, prefab instantiation) and leaves
system scheduling to you (`mu_task_scheduler.h` pairs well).

Everything is `static inline`; config macros and allocation hooks come first:

```c
#define MU_ECS_ENTITY_INDEX_BITS      22u   /* entity index field width  */
#define MU_ECS_ENTITY_GENERATION_BITS 8u    /* staleness guard width     */
#define MU_ECS_INLINE_MASK_WORDS      0u    /* inline component masks    */

#define MU_ECS_MALLOC / MU_ECS_FREE / MU_ECS_REALLOC  /* override as needed */
```

Entity handles pack `(index, generation)` into 32 bits: 22-bit index,
8-bit generation — same staleness-detection idea as `mu_bulk_storage`
([sets-and-ids.md](sets-and-ids.md)).

---

## Entities

```c
typedef struct mu_ecs_entity { uint32_t id; } mu_ecs_entity;
```

```c
mu_ecs_entity_index(e)             /* unpack fields              */
mu_ecs_entity_generation(e)

mu_ecs_entity_manager_init(&em, min_free);
mu_ecs_entity_manager_free(&em);
mu_ecs_entity_manager_reserve(&em, cap);
mu_ecs_entity_manager_free_queue_reserve(&em, cap);

mu_ecs_entity e = mu_ecs_entity_create(&em);
mu_ecs_entity_create_batch(&em, out, count);   /* bulk create    */
mu_ecs_entity_alive(&em, e);                   /* generation-checked */
mu_ecs_entity_destroy(&em, e);                 /* bumps generation   */
```

Dead entities go to a free queue; `destroy` increments the generation so
outstanding handles to the same index become invalid, exactly like weak
handles elsewhere in the library.

---

## Component pools (`mu_ecs_pool`)

Sparse-set component storage: a dense packed array of components plus a
sparse entity→dense-index map. Iterating components touches only dense
memory.

```c
typedef struct mu_ecs_pool
{
    uint32_t  elem_size;
    /* dense: packed component data; sparse: entity index -> dense slot */
} mu_ecs_pool;
```

```c
mu_ecs_pool_init(&p, sizeof(MyComp));
mu_ecs_pool_free(&p);
mu_ecs_pool_reserve(&p, cap);          /* dense capacity           */
mu_ecs_pool_sparse_reserve(&p, cap);   /* entity index range       */

mu_ecs_pool_has(&p, e);                /* entity has component?    */
MyComp* c   = (MyComp*)mu_ecs_pool_get(&p, e);   /* NULL if absent */
void*   add = mu_ecs_pool_add(&p, e);  /* get-or-create slot       */
mu_ecs_pool_remove(&p, e);             /* swap-remove from dense   */
```

Remove is swap-delete (the dense slot is filled by the last component), so
component order changes — iterate for processing, never for stable ordering.

---

## Transforms (`mu_ecs_transform_store`)

Optional hierarchical transform layer: each entity may own a 4x4 local
matrix, parented into a forest, with global matrices computed by downward
propagation.

```c
typedef struct mu_ecs_instance { int32_t idx; } mu_ecs_instance;
#define MU_ECS_INSTANCE_INVALID ((mu_ecs_instance){-1})
```

### API

```c
mu_ecs_transform_init(&t);   mu_ecs_transform_free(&t);
mu_ecs_transform_sparse_reserve(&t, cap);   /* entity index space   */
mu_ecs_transform_stack_reserve(&t, cap);    /* hierarchy depth      */
mu_ecs_transform_reserve(&t, cap);          /* both                 */

mu_ecs_instance h = mu_ecs_transform_add(&t, e);       /* attach    */
mu_ecs_instance h = mu_ecs_transform_find(&t, e);      /* lookup    */
mu_ecs_instance_valid(h);

mu_ecs_transform_set_local(&t, h, m16);      /* copies + marks dirty */
mu_ecs_transform_set_local_raw(&t, h, m16);  /* no dirty marking     */
float* local  = ...; float global[16];
mu_ecs_transform_identity(m16);
mu_ecs_transform_mul(out16, a16, b16);

mu_ecs_transform_set_parent(&t, child, parent);  /* relink           */
mu_ecs_transform_link_child(&t, parent_idx, child_idx);
mu_ecs_transform_unlink(&t, node);
mu_ecs_transform_orphan_children(&t, node);      /* detach subtree   */

mu_ecs_transform_propagate(&t, root_idx);        /* recompute subtree */
mu_ecs_transform_remove(&t, e);
mu_ecs_transform_remove_at(&t, idx);
mu_ecs_transform_fix_moved_links(&t, moved, dst);/* internal, dense compaction */
```

### Notes

- Matrix layout is column-major 4x4 (`float m16`), matching cglm/The Forge
  conventions.
- `propagate` recomputes `global = parent_global * local` for a subtree
  (iterative with an explicit stack, no recursion).
- Removing a transform unlinks children (`orphan_children` semantics) rather
  than destroying the subtree — you decide what happens to orphans.
- Transform removal swaps dense slots; `fix_moved_links` repairs hierarchy
  links pointing at moved instances.

---

## Spawn groups

Batch-create entities with per-entity component data in one shot:

```c
mu_ecs_spawn_group g = { /* components + counts + data pointers */ };
mu_ecs_entity entities[N];
mu_ecs_spawn_group_apply(&world, &g, entities);
```

Each component in the group is added to every created entity; the returned
entity array is in creation order. Use this instead of per-entity create/add
loops — it reserves capacity once and avoids intermediate reallocation.

---

## Prefabs

A prefab is a named component recipe: a list of component blocks, each
carrying a component kind, an element stride, and prototype data to copy
into new instances.

```c
typedef struct mu_ecs_prefab_component_block
{
    uint32_t   kind;         /* component id                      */
    /* stride, data, count for prototype instances             */
} mu_ecs_prefab_component_block;

typedef struct mu_ecs_prefab
{
    mu_ecs_prefab_component_block* blocks;
    uint32_t block_count;
} mu_ecs_prefab;
```

```c
mu_ecs_prefab_spawn(&world, &prefab, out_entities, count);
```

### Mask helpers

Component masks exist in two flavors: plain u64 bitmask words, and
`mu_ecs_inline_mask` (which supports inline words above a threshold via
`MU_ECS_INLINE_MASK_WORDS`):

```c
mu_ecs_prefab_mask_has(mask, words, kind);
mu_ecs_prefab_mask_has_any(mask, words, kinds, count);
mu_ecs_inline_mask_has(&mask, kind);
mu_ecs_inline_mask_array_has(masks, count, kind);
```

Use these for "does this entity match this system filter" checks without
touching pools.

---

## Resource registry

Global tables mapping component kinds to pools and prefab ids to prefabs —
the glue `mu_ecs_prefab_spawn` uses to find where to put instances:

```c
mu_ecs_resource_registry_init(&r);
mu_ecs_resource_registry_free(&r);
mu_ecs_resource_registry_pool_reserve(&r, cap);    /* kinds space   */
mu_ecs_resource_registry_prefab_reserve(&r, cap);  /* prefab space  */

mu_ecs_resource_registry_set_pool(&r, kind, &pool);
mu_ecs_resource_registry_set_prefab(&r, id, &prefab);
mu_ecs_resource_registry_get_pool(...)   /* used internally by spawn  */
```

---

## World

`mu_ecs_world` bundles an entity manager + registry (+ pools you register)
into one init/free:

```c
mu_ecs_world w;
mu_ecs_world_init(&w /*, config */);
mu_ecs_world_free(&w);
```

### Typical wiring

```c
mu_ecs_world w;
mu_ecs_world_init(&w);

mu_ecs_pool pool;
mu_ecs_pool_init(&pool, sizeof(Velocity));
mu_ecs_resource_registry_set_pool(&w.registry, KIND_VELOCITY, &pool);

mu_ecs_entity entities[128];
mu_ecs_prefab_spawn(&w, &velocity_prefab, entities, 128);

for(uint32_t i = 0; i < mu_ecs_pool_has ? : 0; ++i)
{
    /* iterate pool dense storage for system updates */
}

mu_ecs_world_free(&w);
```

### Design notes

- Pools are registered by integer `kind`; there is no compile-time type
  registry — keep kind ids in one enum alongside strides.
- Everything stores **entity handles**, never raw pointers, so pools and
  transforms can reallocate independently.
- Generation-checked handles everywhere: stale entity references fail
  `alive` / `has` checks instead of corrupting memory.
