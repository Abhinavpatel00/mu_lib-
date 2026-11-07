#pragma once
#ifndef MU_DAG_H
#define MU_DAG_H

/*
 * mu_dag.h -- dynamic directed acyclic graph with bottom-up traversal.
 *
 * Nodes are caller-owned and may have multiple edges to the same child (with
 * different edge data). Nodes with no parents live on the graph's heads list.
 * Pruning a head removes its edges and promotes newly parentless children.
 *
 * Intended for render/frame graphs, asset dependency graphs, and job graphs.
 * Inspired by Mesa util/dag.{h,c} (MIT).
 */

#include "mu_common.h"

MU_BEGIN_EXTERN_C

struct mu_dag_node;

typedef struct mu_dag_edge
{
    struct mu_dag_node* child;
    uintptr_t data;
} mu_dag_edge;

typedef struct mu_dag_node
{
    struct mu_dag_node* prev;
    struct mu_dag_node* next;
    struct mu_dag_node* all_prev;
    struct mu_dag_node* all_next;
    mu_dag_edge* edges;
    uint32_t edge_count;
    uint32_t edge_capacity;
    uint32_t parent_count;
    uint32_t visit_gen;
    uint8_t state;
    uint8_t in_heads;
} mu_dag_node;

typedef struct mu_dag
{
    mu_dag_node* head;
    mu_dag_node* tail;
    mu_dag_node* nodes_head;
    mu_dag_node* nodes_tail;
    uint32_t visit_gen;
} mu_dag;

MU_INLINE void mu_dag_init(mu_dag* dag)
{
    MU_ASSERT(dag != NULL);
    dag->head = NULL;
    dag->tail = NULL;
    dag->nodes_head = NULL;
    dag->nodes_tail = NULL;
    dag->visit_gen = 0;
}

MU_INLINE void mu_dag__head_add_tail(mu_dag* dag, mu_dag_node* node)
{
    node->prev = dag->tail;
    node->next = NULL;
    if (dag->tail)
        dag->tail->next = node;
    else
        dag->head = node;
    dag->tail = node;
    node->in_heads = 1;
}

MU_INLINE void mu_dag__head_remove(mu_dag* dag, mu_dag_node* node)
{
    MU_ASSERT(dag != NULL && node != NULL);
    if (node->prev)
        node->prev->next = node->next;
    else
        dag->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        dag->tail = node->prev;
    node->prev = NULL;
    node->next = NULL;
    node->in_heads = 0;
}

MU_INLINE void mu_dag_init_node(mu_dag* dag, mu_dag_node* node)
{
    MU_ASSERT(dag != NULL && node != NULL);
    node->prev = NULL;
    node->next = NULL;
    node->all_prev = dag->nodes_tail;
    node->all_next = NULL;
    node->edges = NULL;
    node->edge_count = 0;
    node->edge_capacity = 0;
    node->parent_count = 0;
    node->visit_gen = 0;
    node->state = 0;
    node->in_heads = 0;
    mu_dag__head_add_tail(dag, node);
    if (dag->nodes_tail)
        dag->nodes_tail->all_next = node;
    else
        dag->nodes_head = node;
    dag->nodes_tail = node;
}

MU_INLINE void mu_dag_node_fini(mu_dag_node* node)
{
    if (!node)
        return;
    MU_FREE(node->edges);
    node->edges = NULL;
    node->edge_count = 0;
    node->edge_capacity = 0;
}

MU_INLINE bool mu_dag__reserve_edges(mu_dag_node* node, uint32_t needed)
{
    uint32_t capacity;
    mu_dag_edge* edges;

    if (needed <= node->edge_capacity)
        return true;

    capacity = node->edge_capacity ? node->edge_capacity : 4u;
    while (capacity < needed)
    {
        if (capacity > UINT32_MAX / 2u)
        {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }

#if SIZE_MAX <= UINT32_MAX
    if ((size_t)capacity > SIZE_MAX / sizeof(mu_dag_edge))
        return false;
#endif

    edges = (mu_dag_edge*)MU_REALLOC(node->edges, (size_t)capacity * sizeof(mu_dag_edge));
    if (!edges)
        return false;

    node->edges = edges;
    node->edge_capacity = capacity;
    return true;
}

MU_INLINE bool mu_dag_add_edge(mu_dag* dag, mu_dag_node* parent, mu_dag_node* child, uintptr_t data)
{
    uint32_t i;
    MU_ASSERT(dag != NULL && parent != NULL && child != NULL);

    for (i = 0; i < parent->edge_count; ++i)
        if (parent->edges[i].child == child && parent->edges[i].data == data)
            return true;

    if (!mu_dag__reserve_edges(parent, parent->edge_count + 1u))
        return false;

    if (child->in_heads)
        mu_dag__head_remove(dag, child);

    parent->edges[parent->edge_count].child = child;
    parent->edges[parent->edge_count].data = data;
    ++parent->edge_count;
    ++child->parent_count;
    return true;
}

MU_INLINE bool mu_dag_add_edge_max_data(mu_dag* dag, mu_dag_node* parent, mu_dag_node* child, uintptr_t data)
{
    uint32_t i;
    MU_ASSERT(dag != NULL && parent != NULL && child != NULL);

    for (i = 0; i < parent->edge_count; ++i)
    {
        if (parent->edges[i].child == child)
        {
            if (data > parent->edges[i].data)
                parent->edges[i].data = data;
            return true;
        }
    }
    return mu_dag_add_edge(dag, parent, child, data);
}

MU_INLINE void mu_dag_remove_edge(mu_dag* dag, mu_dag_edge* edge)
{
    mu_dag_node* child;
    MU_ASSERT(dag != NULL && edge != NULL);

    child = edge->child;
    if (!child)
        return;

    MU_ASSERT(child->parent_count > 0);
    --child->parent_count;
    if (child->parent_count == 0)
        mu_dag__head_add_tail(dag, child);

    edge->child = NULL;
    edge->data = 0;
}

MU_INLINE void mu_dag_prune_head(mu_dag* dag, mu_dag_node* node)
{
    uint32_t i;
    MU_ASSERT(dag != NULL && node != NULL);
    MU_ASSERT(node->parent_count == 0);
    MU_ASSERT(node->in_heads);

    mu_dag__head_remove(dag, node);
    for (i = 0; i < node->edge_count; ++i)
        mu_dag_remove_edge(dag, &node->edges[i]);
}

typedef struct mu_dag_frame
{
    mu_dag_node* node;
    uint32_t next_edge;
} mu_dag_frame;

MU_INLINE bool mu_dag__stack_push(mu_dag_frame** stack, uint32_t* count,
                                  uint32_t* capacity, mu_dag_frame frame)
{
    if (*count == *capacity)
    {
        uint32_t new_capacity = *capacity ? *capacity * 2u : 16u;
        mu_dag_frame* next;
        if (new_capacity <= *capacity)
            return false;
#if SIZE_MAX <= UINT32_MAX
        if ((size_t)new_capacity > SIZE_MAX / sizeof(mu_dag_frame))
            return false;
#endif
        next = (mu_dag_frame*)MU_REALLOC(*stack,
                                         (size_t)new_capacity * sizeof(mu_dag_frame));
        if (!next)
            return false;
        *stack = next;
        *capacity = new_capacity;
    }
    (*stack)[(*count)++] = frame;
    return true;
}

MU_INLINE bool mu_dag__push_node(mu_dag_frame** stack, uint32_t* count,
                                uint32_t* capacity, mu_dag_node* node,
                                uint32_t next_edge)
{
    mu_dag_frame frame;
    frame.node = node;
    frame.next_edge = next_edge;
    return mu_dag__stack_push(stack, count, capacity, frame);
}

MU_INLINE uint32_t mu_dag__next_generation(mu_dag* dag)
{
    ++dag->visit_gen;
    if (dag->visit_gen == 0)
        dag->visit_gen = 1;
    return dag->visit_gen;
}

MU_INLINE bool mu_dag_traverse_bottom_up(mu_dag* dag,
                                         void (*cb)(mu_dag_node* node, void* data),
                                         void* data)
{
    mu_dag_node* root;
    uint32_t gen;
    mu_dag_frame* stack = NULL;
    uint32_t stack_count = 0;
    uint32_t stack_capacity = 0;
    bool ok = true;

    MU_ASSERT(dag != NULL && cb != NULL);
    gen = mu_dag__next_generation(dag);

    for (root = dag->head; root && ok; root = root->next)
    {
        if (root->visit_gen == gen)
            continue;

        root->visit_gen = gen;
        root->state = 1;
        if (!mu_dag__push_node(&stack, &stack_count, &stack_capacity, root, 0))
        {
            ok = false;
            break;
        }

        while (stack_count > 0)
        {
            mu_dag_frame* frame = &stack[stack_count - 1];
            mu_dag_node* node = frame->node;

            if (frame->next_edge < node->edge_count)
            {
                mu_dag_node* child = node->edges[frame->next_edge++].child;
                if (!child)
                    continue;
                if (child->visit_gen != gen)
                {
                    child->visit_gen = gen;
                    child->state = 1;
                    if (!mu_dag__push_node(&stack, &stack_count, &stack_capacity, child, 0))
                    {
                        ok = false;
                        break;
                    }
                }
                else if (child->state == 1)
                {
                    MU_ASSERT(!"mu_dag: cycle detected");
                    ok = false;
                    break;
                }
            }
            else
            {
                node->state = 2;
                cb(node, data);
                --stack_count;
            }
        }
    }

    MU_FREE(stack);
    return ok;
}

MU_INLINE bool mu_dag_validate(mu_dag* dag)
{
    mu_dag_node* root;
    uint32_t gen;
    mu_dag_frame* stack = NULL;
    uint32_t stack_count = 0;
    uint32_t stack_capacity = 0;
    bool ok = true;

    MU_ASSERT(dag != NULL);
    gen = mu_dag__next_generation(dag);

    for (root = dag->nodes_head; root && ok; root = root->all_next)
    {
        if (root->visit_gen == gen)
            continue;

        root->visit_gen = gen;
        root->state = 1;
        if (!mu_dag__push_node(&stack, &stack_count, &stack_capacity, root, 0))
        {
            ok = false;
            break;
        }

        while (stack_count > 0)
        {
            mu_dag_frame* frame = &stack[stack_count - 1];
            mu_dag_node* node = frame->node;

            if (frame->next_edge < node->edge_count)
            {
                mu_dag_node* child = node->edges[frame->next_edge++].child;
                if (!child)
                    continue;
                if (child->visit_gen != gen)
                {
                    child->visit_gen = gen;
                    child->state = 1;
                    if (!mu_dag__push_node(&stack, &stack_count, &stack_capacity, child, 0))
                    {
                        ok = false;
                        break;
                    }
                }
                else if (child->state == 1)
                {
                    ok = false;
                    break;
                }
            }
            else
            {
                node->state = 2;
                --stack_count;
            }
        }
    }

    MU_FREE(stack);
    return ok;
}

MU_INLINE void mu_dag_destroy(mu_dag* dag)
{
    mu_dag_node* node;
    MU_ASSERT(dag != NULL);

    for (node = dag->nodes_head; node; )
    {
        mu_dag_node* next = node->all_next;
        mu_dag_node_fini(node);
        node->all_prev = NULL;
        node->all_next = NULL;
        node->prev = NULL;
        node->next = NULL;
        node->parent_count = 0;
        node->in_heads = 0;
        node->state = 0;
        node = next;
    }

    dag->head = NULL;
    dag->tail = NULL;
    dag->nodes_head = NULL;
    dag->nodes_tail = NULL;
}

MU_END_EXTERN_C

#endif /* MU_DAG_H */
