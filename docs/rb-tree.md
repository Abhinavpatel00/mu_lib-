# `mu_rb_tree.h` — intrusive red-black tree and interval tree

An allocation-free balanced ordered tree. `mu_rb_node` is embedded in your
object, so inserting/removing never calls `malloc`, and iteration is
pointer-driven.

```c
#include "mu.h"

typedef struct Entity
{
    mu_rb_node node;
    uint64_t   id;
} Entity;

static int cmp_entity(const mu_rb_node* a, const mu_rb_node* b)
{
    const Entity* ea = MU_CONTAINER_OF((mu_rb_node*)a, Entity, node);
    const Entity* eb = MU_CONTAINER_OF((mu_rb_node*)b, Entity, node);
    return (ea->id > eb->id) - (ea->id < eb->id);
}

mu_rb_tree tree;
mu_rb_tree_init(&tree);
mu_rb_tree_insert(&tree, &entity->node, cmp_entity);
mu_rb_tree_validate(&tree);
```

Comparator convention: `cmp(a, b)` returns `< 0` when `a` sorts before `b`,
`0` when equal, and `> 0` when `a` sorts after `b`.

## API

```c
void mu_rb_tree_init(mu_rb_tree* tree);
bool mu_rb_tree_is_empty(const mu_rb_tree* tree);

void mu_rb_tree_insert(mu_rb_tree* tree, mu_rb_node* node, mu_rb_cmp_fn cmp);
void mu_rb_tree_insert_at(mu_rb_tree* tree, mu_rb_node* parent,
                          mu_rb_node* node, bool insert_left);
void mu_rb_tree_remove(mu_rb_tree* tree, mu_rb_node* node);

mu_rb_node* mu_rb_tree_search(mu_rb_tree* tree, const void* key,
                              mu_rb_search_cmp_fn cmp);
mu_rb_node* mu_rb_tree_search_sloppy(mu_rb_tree* tree, const void* key,
                                     mu_rb_search_cmp_fn cmp);
mu_rb_node* mu_rb_tree_first(mu_rb_tree* tree);
mu_rb_node* mu_rb_tree_last(mu_rb_tree* tree);
mu_rb_node* mu_rb_node_next(mu_rb_node* node);
mu_rb_node* mu_rb_node_prev(mu_rb_node* node);
void mu_rb_tree_validate(mu_rb_tree* tree);
```

Iteration macros:

```c
mu_rb_tree_foreach(Entity, e, &tree, node) { use(e); }
mu_rb_tree_foreach_safe(Entity, e, &tree, node) { mu_rb_tree_remove(&tree, &e->node); }
mu_rb_tree_foreach_rev(Entity, e, &tree, node) { use(e); }
```

`_safe` variants cache the next node before the loop body, so removing or
freeing the current node is allowed.

## Augmented trees

`mu_rb_augmented_tree_insert()` and `mu_rb_augmented_tree_remove()` accept an
update callback. The callback recomputes per-node metadata from the node and
its children after structural changes. This is how the interval tree
maintains `max_end`.

```c
void update(mu_rb_node* node);
mu_rb_augmented_tree_insert(&tree, &node->node, cmp, update);
mu_rb_augmented_tree_remove(&tree, &node->node, update);
```

## Interval tree

Closed unsigned intervals (`start` and `end` are inclusive).

```c
mu_interval_node in;
in.interval.start = 100;
in.interval.end   = 200;
mu_interval_tree_insert(&tree, &in);

mu_interval q = {150, 250};
mu_interval_node* first = mu_interval_tree_first(&tree, q);
mu_interval_node* next  = mu_interval_node_next(first, q);
```

Overlap queries are O(log n + k) for k results. The augmentation tracks the
maximum end in each subtree.

## Threading and ownership

- Not thread-safe.
- Nodes must stay alive while linked.
- Do not move a node while it is linked.
- A node may be in at most one `mu_rb_tree` at a time.
- `mu_rb_tree_validate` is a debug helper; it asserts on structural errors.

Ported from Mesa `util/rb_tree.{h,c}` (MIT).
