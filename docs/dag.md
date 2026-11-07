# `mu_dag.h` — dynamic directed acyclic graph

A mutable dependency graph with multiple roots ("heads"), edge payloads, and
bottom-up traversal. Useful for render/frame graphs, asset dependency graphs,
and job graph compilation.

```c
#include "mu.h"

typedef struct Pass
{
    mu_dag_node node;
    const char* name;
} Pass;

mu_dag dag;
mu_dag_init(&dag);

Pass a = {.name = "shadow"};
Pass b = {.name = "lighting"};
mu_dag_init_node(&dag, &a.node);
mu_dag_init_node(&dag, &b.node);

mu_dag_add_edge(&dag, &b.node, &a.node, 0);  /* b depends on a */
```

## Model

- Every node starts as a head (no parents).
- Adding an edge removes the child from the heads list.
- Removing the last parent edge promotes the child back to a head.
- Edges may point to the same child multiple times with different payloads.
- The graph does not own nodes.

```
heads -> [ lighting ] -> edge -> [ shadow ]
```

## API

```c
void mu_dag_init(mu_dag* dag);
void mu_dag_init_node(mu_dag* dag, mu_dag_node* node);
void mu_dag_node_fini(mu_dag_node* node);
void mu_dag_destroy(mu_dag* dag);

bool mu_dag_add_edge(mu_dag* dag, mu_dag_node* parent,
                     mu_dag_node* child, uintptr_t data);
bool mu_dag_add_edge_max_data(mu_dag* dag, mu_dag_node* parent,
                              mu_dag_node* child, uintptr_t data);
void mu_dag_remove_edge(mu_dag* dag, mu_dag_edge* edge);
void mu_dag_prune_head(mu_dag* dag, mu_dag_node* node);

bool mu_dag_traverse_bottom_up(mu_dag* dag,
                               void (*cb)(mu_dag_node*, void*), void* data);
bool mu_dag_validate(mu_dag* dag);
```

`mu_dag_add_edge` deduplicates identical `(child, data)` pairs.
`mu_dag_add_edge_max_data` merges duplicates by keeping the larger payload.

## Bottom-up traversal

`mu_dag_traverse_bottom_up` visits each reachable node exactly once, after all
of its children:

```c
static void run_pass(mu_dag_node* node, void* data)
{
    Pass* pass = MU_CONTAINER_OF(node, Pass, node);
    execute(pass);
}

mu_dag_traverse_bottom_up(&dag, run_pass, NULL);
```

Traversal is iterative; no recursion depth limit. It returns `false` if a
cycle is detected or the internal stack cannot grow.

## Validation and cycles

`mu_dag_validate` scans every node known to the graph, including pure cycles
that have no heads. It returns `false` on a cycle.

## Memory and ownership

- Nodes are caller-owned. Edges are dynamically allocated per node.
- Call `mu_dag_node_fini` for a single node, or `mu_dag_destroy` for the whole
  graph. `destroy` frees edge arrays but never frees node memory.
- Do not mutate the graph from inside the traversal callback.
- `mu_dag_prune_head` removes a head's edges and promotes newly parentless
  children.

Inspired by Mesa `util/dag.{h,c}` (MIT).
