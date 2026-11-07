#pragma once
#ifndef MU_RB_TREE_H
#define MU_RB_TREE_H

/*
 * mu_rb_tree.h -- intrusive red-black tree and unsigned interval tree.
 *
 * Nodes are embedded in user objects. The tree never allocates. The optional
 * update callback lets callers maintain augmented per-node data (for example
 * interval max-end values) without a second pass.
 *
 * Comparator convention: cmp(a, b) returns <0 when a sorts before b, 0 when
 * equal, and >0 when a sorts after b.
 *
 * Ported from Mesa util/rb_tree.{h,c} (MIT).
 */

#include "mu_common.h"

MU_BEGIN_EXTERN_C

typedef struct mu_rb_node
{
    uintptr_t parent; /* parent pointer | color bit (1 = black) */
    struct mu_rb_node* left;
    struct mu_rb_node* right;
} mu_rb_node;

typedef struct mu_rb_tree
{
    mu_rb_node* root;
} mu_rb_tree;

typedef int (*mu_rb_cmp_fn)(const mu_rb_node* a, const mu_rb_node* b);
typedef int (*mu_rb_search_cmp_fn)(const mu_rb_node* node, const void* key);
typedef void (*mu_rb_update_fn)(mu_rb_node* node);

MU_INLINE mu_rb_node* mu_rb_node_parent(mu_rb_node* n)
{
    return (mu_rb_node*)(n->parent & ~(uintptr_t)1u);
}

MU_INLINE bool mu_rb_node_is_black(const mu_rb_node* n)
{
    return n == NULL || (n->parent & 1u) != 0;
}

MU_INLINE bool mu_rb_node_is_red(const mu_rb_node* n)
{
    return !mu_rb_node_is_black(n);
}

MU_INLINE void mu_rb_node_set_black(mu_rb_node* n)
{
    n->parent |= 1u;
}

MU_INLINE void mu_rb_node_set_red(mu_rb_node* n)
{
    n->parent &= ~(uintptr_t)1u;
}

MU_INLINE void mu_rb_node_copy_color(mu_rb_node* dst, const mu_rb_node* src)
{
    dst->parent = (dst->parent & ~(uintptr_t)1u) | (src->parent & 1u);
}

MU_INLINE void mu_rb_node_set_parent(mu_rb_node* n, mu_rb_node* parent)
{
    n->parent = (n->parent & 1u) | (uintptr_t)parent;
}

MU_INLINE void mu_rb_tree_init(mu_rb_tree* tree)
{
    tree->root = NULL;
}

MU_INLINE bool mu_rb_tree_is_empty(const mu_rb_tree* tree)
{
    return tree->root == NULL;
}

MU_INLINE mu_rb_node* mu_rb_node_minimum(mu_rb_node* node)
{
    while (node->left)
        node = node->left;
    return node;
}

MU_INLINE mu_rb_node* mu_rb_node_maximum(mu_rb_node* node)
{
    while (node->right)
        node = node->right;
    return node;
}

MU_INLINE mu_rb_node* mu_rb_tree_first(mu_rb_tree* tree)
{
    return tree->root ? mu_rb_node_minimum(tree->root) : NULL;
}

MU_INLINE mu_rb_node* mu_rb_tree_last(mu_rb_tree* tree)
{
    return tree->root ? mu_rb_node_maximum(tree->root) : NULL;
}

MU_INLINE mu_rb_node* mu_rb_node_next(mu_rb_node* node)
{
    mu_rb_node* parent;

    if (node->right)
        return mu_rb_node_minimum(node->right);

    parent = mu_rb_node_parent(node);
    while (parent && node == parent->right)
    {
        node = parent;
        parent = mu_rb_node_parent(node);
    }
    return parent;
}

MU_INLINE mu_rb_node* mu_rb_node_prev(mu_rb_node* node)
{
    mu_rb_node* parent;

    if (node->left)
        return mu_rb_node_maximum(node->left);

    parent = mu_rb_node_parent(node);
    while (parent && node == parent->left)
    {
        node = parent;
        parent = mu_rb_node_parent(node);
    }
    return parent;
}

MU_INLINE mu_rb_node* mu_rb_node_next_or_null(mu_rb_node* node)
{
    return node ? mu_rb_node_next(node) : NULL;
}

MU_INLINE mu_rb_node* mu_rb_node_prev_or_null(mu_rb_node* node)
{
    return node ? mu_rb_node_prev(node) : NULL;
}

#define mu_rb_node_data(type, node, field) \
    ((type*)((char*)(node) - MU_OFFSET_OF(type, field)))

#define mu_rb_tree_foreach(type, iter, tree, field)                                      \
    for (type* iter, *__mu_node = (type*)mu_rb_tree_first(tree);                         \
         __mu_node != NULL &&                                                            \
         (iter = mu_rb_node_data(type, (mu_rb_node*)__mu_node, field), true);            \
         __mu_node = (type*)mu_rb_node_next((mu_rb_node*)__mu_node))

#define mu_rb_tree_foreach_safe(type, iter, tree, field)                                 \
    for (type* iter, *__mu_node = (type*)mu_rb_tree_first(tree),                         \
               *__mu_next = (type*)mu_rb_node_next_or_null((mu_rb_node*)__mu_node);      \
         __mu_node != NULL &&                                                            \
         (iter = mu_rb_node_data(type, (mu_rb_node*)__mu_node, field), true);            \
         __mu_node = __mu_next,                                                          \
         __mu_next = (type*)mu_rb_node_next_or_null((mu_rb_node*)__mu_node))

#define mu_rb_tree_foreach_rev(type, iter, tree, field)                                  \
    for (type* iter, *__mu_node = (type*)mu_rb_tree_last(tree);                          \
         __mu_node != NULL &&                                                            \
         (iter = mu_rb_node_data(type, (mu_rb_node*)__mu_node, field), true);            \
         __mu_node = (type*)mu_rb_node_prev((mu_rb_node*)__mu_node))

#define mu_rb_tree_foreach_rev_safe(type, iter, tree, field)                             \
    for (type* iter, *__mu_node = (type*)mu_rb_tree_last(tree),                          \
               *__mu_prev = (type*)mu_rb_node_prev_or_null((mu_rb_node*)__mu_node);      \
         __mu_node != NULL &&                                                            \
         (iter = mu_rb_node_data(type, (mu_rb_node*)__mu_node, field), true);            \
         __mu_node = __mu_prev,                                                          \
         __mu_prev = (type*)mu_rb_node_prev_or_null((mu_rb_node*)__mu_node))

MU_INLINE void mu_rb_tree_splice(mu_rb_tree* tree, mu_rb_node* u, mu_rb_node* v)
{
    mu_rb_node* parent = mu_rb_node_parent(u);

    MU_ASSERT(u != NULL);
    if (!parent)
    {
        MU_ASSERT(tree->root == u);
        tree->root = v;
    }
    else if (u == parent->left)
    {
        parent->left = v;
    }
    else
    {
        MU_ASSERT(u == parent->right);
        parent->right = v;
    }

    if (v)
        mu_rb_node_set_parent(v, parent);
}

MU_INLINE void mu_rb_tree_rotate_left(mu_rb_tree* tree, mu_rb_node* x, mu_rb_update_fn update)
{
    mu_rb_node* y = x->right;

    MU_ASSERT(x && y);
    x->right = y->left;
    if (y->left)
        mu_rb_node_set_parent(y->left, x);
    mu_rb_tree_splice(tree, x, y);
    y->left = x;
    mu_rb_node_set_parent(x, y);

    if (update)
    {
        update(x);
        update(y);
    }
}

MU_INLINE void mu_rb_tree_rotate_right(mu_rb_tree* tree, mu_rb_node* y, mu_rb_update_fn update)
{
    mu_rb_node* x = y->left;

    MU_ASSERT(y && x);
    y->left = x->right;
    if (x->right)
        mu_rb_node_set_parent(x->right, y);
    mu_rb_tree_splice(tree, y, x);
    x->right = y;
    mu_rb_node_set_parent(y, x);

    if (update)
    {
        update(y);
        update(x);
    }
}

MU_INLINE void mu_rb_augmented_tree_insert_at(mu_rb_tree* tree, mu_rb_node* parent,
                                              mu_rb_node* node, bool insert_left,
                                              mu_rb_update_fn update)
{
    mu_rb_node* z;

    MU_MEMSET(node, 0, sizeof(*node));
    if (update)
        update(node);

    if (!parent)
    {
        MU_ASSERT(tree->root == NULL);
        tree->root = node;
        mu_rb_node_set_black(node);
        return;
    }

    if (insert_left)
    {
        MU_ASSERT(parent->left == NULL);
        parent->left = node;
    }
    else
    {
        MU_ASSERT(parent->right == NULL);
        parent->right = node;
    }
    mu_rb_node_set_parent(node, parent);

    if (update)
    {
        mu_rb_node* p = parent;
        while (p)
        {
            update(p);
            p = mu_rb_node_parent(p);
        }
    }

    z = node;
    while (mu_rb_node_is_red(mu_rb_node_parent(z)))
    {
        mu_rb_node* z_p = mu_rb_node_parent(z);
        mu_rb_node* z_p_p = mu_rb_node_parent(z_p);

        MU_ASSERT(z_p_p != NULL);
        if (z_p == z_p_p->left)
        {
            mu_rb_node* y = z_p_p->right;
            if (mu_rb_node_is_red(y))
            {
                mu_rb_node_set_black(z_p);
                mu_rb_node_set_black(y);
                mu_rb_node_set_red(z_p_p);
                z = z_p_p;
            }
            else
            {
                if (z == z_p->right)
                {
                    z = z_p;
                    mu_rb_tree_rotate_left(tree, z, update);
                    z_p = mu_rb_node_parent(z);
                    z_p_p = mu_rb_node_parent(z_p);
                }
                mu_rb_node_set_black(z_p);
                mu_rb_node_set_red(z_p_p);
                mu_rb_tree_rotate_right(tree, z_p_p, update);
            }
        }
        else
        {
            mu_rb_node* y = z_p_p->left;
            if (mu_rb_node_is_red(y))
            {
                mu_rb_node_set_black(z_p);
                mu_rb_node_set_black(y);
                mu_rb_node_set_red(z_p_p);
                z = z_p_p;
            }
            else
            {
                if (z == z_p->left)
                {
                    z = z_p;
                    mu_rb_tree_rotate_right(tree, z, update);
                    z_p = mu_rb_node_parent(z);
                    z_p_p = mu_rb_node_parent(z_p);
                }
                mu_rb_node_set_black(z_p);
                mu_rb_node_set_red(z_p_p);
                mu_rb_tree_rotate_left(tree, z_p_p, update);
            }
        }
    }
    mu_rb_node_set_black(tree->root);
}

MU_INLINE void mu_rb_tree_insert_at(mu_rb_tree* tree, mu_rb_node* parent,
                                    mu_rb_node* node, bool insert_left)
{
    mu_rb_augmented_tree_insert_at(tree, parent, node, insert_left, NULL);
}

MU_INLINE void mu_rb_augmented_tree_insert(mu_rb_tree* tree, mu_rb_node* node,
                                           mu_rb_cmp_fn cmp, mu_rb_update_fn update)
{
    mu_rb_node* parent = NULL;
    mu_rb_node* x = tree->root;
    bool left = false;

    while (x)
    {
        parent = x;
        left = cmp(x, node) > 0;
        x = left ? x->left : x->right;
    }

    mu_rb_augmented_tree_insert_at(tree, parent, node, left, update);
}

MU_INLINE void mu_rb_tree_insert(mu_rb_tree* tree, mu_rb_node* node, mu_rb_cmp_fn cmp)
{
    mu_rb_augmented_tree_insert(tree, node, cmp, NULL);
}

MU_INLINE void mu_rb_augmented_tree_remove(mu_rb_tree* tree, mu_rb_node* z,
                                           mu_rb_update_fn update)
{
    mu_rb_node* x;
    mu_rb_node* x_p;
    mu_rb_node* y = z;
    bool y_was_black = mu_rb_node_is_black(y);

    if (!z->left)
    {
        x = z->right;
        x_p = mu_rb_node_parent(z);
        mu_rb_tree_splice(tree, z, x);
    }
    else if (!z->right)
    {
        x = z->left;
        x_p = mu_rb_node_parent(z);
        mu_rb_tree_splice(tree, z, x);
    }
    else
    {
        y = mu_rb_node_minimum(z->right);
        y_was_black = mu_rb_node_is_black(y);
        x = y->right;

        if (mu_rb_node_parent(y) == z)
        {
            x_p = y;
        }
        else
        {
            x_p = mu_rb_node_parent(y);
            mu_rb_tree_splice(tree, y, x);
            y->right = z->right;
            mu_rb_node_set_parent(y->right, y);
        }

        MU_ASSERT(y->left == NULL);
        mu_rb_tree_splice(tree, z, y);
        y->left = z->left;
        mu_rb_node_set_parent(y->left, y);
        mu_rb_node_copy_color(y, z);
    }

    MU_ASSERT(x_p == NULL || x == x_p->left || x == x_p->right);

    if (update)
    {
        mu_rb_node* p = x_p;
        while (p)
        {
            update(p);
            p = mu_rb_node_parent(p);
        }
    }

    if (!y_was_black)
        return;

    while (x != tree->root && mu_rb_node_is_black(x))
    {
        if (x == x_p->left)
        {
            mu_rb_node* w = x_p->right;
            if (mu_rb_node_is_red(w))
            {
                mu_rb_node_set_black(w);
                mu_rb_node_set_red(x_p);
                mu_rb_tree_rotate_left(tree, x_p, update);
                MU_ASSERT(x == x_p->left);
                w = x_p->right;
            }

            if (mu_rb_node_is_black(w->left) && mu_rb_node_is_black(w->right))
            {
                mu_rb_node_set_red(w);
                x = x_p;
            }
            else
            {
                if (mu_rb_node_is_black(w->right))
                {
                    mu_rb_node_set_black(w->left);
                    mu_rb_node_set_red(w);
                    mu_rb_tree_rotate_right(tree, w, update);
                    w = x_p->right;
                }
                mu_rb_node_copy_color(w, x_p);
                mu_rb_node_set_black(x_p);
                mu_rb_node_set_black(w->right);
                mu_rb_tree_rotate_left(tree, x_p, update);
                x = tree->root;
            }
        }
        else
        {
            mu_rb_node* w = x_p->left;
            if (mu_rb_node_is_red(w))
            {
                mu_rb_node_set_black(w);
                mu_rb_node_set_red(x_p);
                mu_rb_tree_rotate_right(tree, x_p, update);
                MU_ASSERT(x == x_p->right);
                w = x_p->left;
            }

            if (mu_rb_node_is_black(w->right) && mu_rb_node_is_black(w->left))
            {
                mu_rb_node_set_red(w);
                x = x_p;
            }
            else
            {
                if (mu_rb_node_is_black(w->left))
                {
                    mu_rb_node_set_black(w->right);
                    mu_rb_node_set_red(w);
                    mu_rb_tree_rotate_left(tree, w, update);
                    w = x_p->left;
                }
                mu_rb_node_copy_color(w, x_p);
                mu_rb_node_set_black(x_p);
                mu_rb_node_set_black(w->left);
                mu_rb_tree_rotate_right(tree, x_p, update);
                x = tree->root;
            }
        }
        x_p = mu_rb_node_parent(x);
    }

    if (x)
        mu_rb_node_set_black(x);
}

MU_INLINE void mu_rb_tree_remove(mu_rb_tree* tree, mu_rb_node* node)
{
    mu_rb_augmented_tree_remove(tree, node, NULL);
}

MU_INLINE mu_rb_node* mu_rb_tree_search(mu_rb_tree* tree, const void* key,
                                        mu_rb_search_cmp_fn cmp)
{
    mu_rb_node* x = tree->root;

    while (x)
    {
        int c = cmp(x, key);
        if (c > 0)
        {
            x = x->left;
        }
        else if (c < 0)
        {
            x = x->right;
        }
        else
        {
            for (;;)
            {
                mu_rb_node* prev = mu_rb_node_prev(x);
                if (!prev || cmp(prev, key) != 0)
                    return x;
                x = prev;
            }
        }
    }
    return NULL;
}

MU_INLINE mu_rb_node* mu_rb_tree_search_sloppy(mu_rb_tree* tree, const void* key,
                                               mu_rb_search_cmp_fn cmp)
{
    mu_rb_node* y = NULL;
    mu_rb_node* x = tree->root;

    while (x)
    {
        int c;
        y = x;
        c = cmp(x, key);
        if (c > 0)
            x = x->left;
        else if (c < 0)
            x = x->right;
        else
            return x;
    }
    return y;
}

typedef struct mu_interval
{
    unsigned start;
    unsigned end; /* inclusive */
} mu_interval;

typedef struct mu_interval_node
{
    mu_rb_node node;
    mu_interval interval;
    unsigned max_end;
} mu_interval_node;

MU_INLINE int mu_interval_compare(mu_interval a, mu_interval b)
{
    if (a.end < b.start)
        return -1;
    if (b.end < a.start)
        return 1;
    return 0;
}

MU_INLINE int mu_interval_node_compare(const mu_rb_node* a, const mu_rb_node* b)
{
    const mu_interval_node* ia = mu_rb_node_data(mu_interval_node, a, node);
    const mu_interval_node* ib = mu_rb_node_data(mu_interval_node, b, node);
    return (ia->interval.start > ib->interval.start)
         - (ia->interval.start < ib->interval.start);
}

MU_INLINE int mu_interval_search_compare(const mu_rb_node* node, const void* key)
{
    const mu_interval_node* in = mu_rb_node_data(mu_interval_node, node, node);
    return mu_interval_compare(in->interval, *(const mu_interval*)key);
}

MU_INLINE bool mu_interval_max_compare(const mu_rb_node* node, const void* key)
{
    const mu_interval_node* in = mu_rb_node_data(mu_interval_node, node, node);
    return in->max_end >= ((const mu_interval*)key)->start;
}

MU_INLINE void mu_interval_node_update_max(mu_rb_node* node)
{
    mu_interval_node* in = mu_rb_node_data(mu_interval_node, node, node);
    in->max_end = in->interval.end;
    if (node->left)
    {
        mu_interval_node* left = mu_rb_node_data(mu_interval_node, node->left, node);
        in->max_end = MU_MAX(in->max_end, left->max_end);
    }
    if (node->right)
    {
        mu_interval_node* right = mu_rb_node_data(mu_interval_node, node->right, node);
        in->max_end = MU_MAX(in->max_end, right->max_end);
    }
}

MU_INLINE void mu_interval_tree_insert(mu_rb_tree* tree, mu_interval_node* node)
{
    mu_rb_augmented_tree_insert(tree, &node->node, mu_interval_node_compare,
                                mu_interval_node_update_max);
}

MU_INLINE void mu_interval_tree_remove(mu_rb_tree* tree, mu_interval_node* node)
{
    mu_rb_augmented_tree_remove(tree, &node->node, mu_interval_node_update_max);
}

MU_INLINE mu_rb_node* mu_rb_node_min_intersecting(
    mu_rb_node* node, const void* interval,
    int (*cmp_interval)(const mu_rb_node*, const void*),
    bool (*cmp_max)(const mu_rb_node*, const void*))
{
    if (!cmp_max(node, interval))
        return NULL;

    while (node)
    {
        int cmp = cmp_interval(node, interval);

        if (cmp > 0)
        {
            node = node->left;
            continue;
        }

        if (node->left && cmp_max(node->left, interval))
        {
            node = node->left;
            continue;
        }

        if (cmp == 0)
            return node;

        node = node->right;
        if (node && !cmp_max(node, interval))
            return NULL;
    }
    return NULL;
}

MU_INLINE mu_rb_node* mu_rb_node_next_intersecting(
    mu_rb_node* node, const void* interval,
    int (*cmp_interval)(const mu_rb_node*, const void*),
    bool (*cmp_max)(const mu_rb_node*, const void*))
{
    for (;;)
    {
        if (node->right)
        {
            mu_rb_node* next = mu_rb_node_min_intersecting(node->right, interval,
                                                           cmp_interval, cmp_max);
            if (next)
                return next;
        }

        {
            mu_rb_node* parent = mu_rb_node_parent(node);
            while (parent && node == parent->right)
            {
                node = parent;
                parent = mu_rb_node_parent(node);
            }

            if (!parent)
                return NULL;

            {
                int cmp = cmp_interval(parent, interval);
                if (cmp == 0)
                    return parent;
                if (cmp > 0)
                    return NULL;
                node = parent;
            }
        }
    }
}

MU_INLINE mu_interval_node* mu_interval_tree_first(mu_rb_tree* tree, mu_interval interval)
{
    mu_rb_node* node;
    if (!tree->root)
        return NULL;
    node = mu_rb_node_min_intersecting(tree->root, &interval,
                                       mu_interval_search_compare,
                                       mu_interval_max_compare);
    return node ? mu_rb_node_data(mu_interval_node, node, node) : NULL;
}

MU_INLINE mu_interval_node* mu_interval_node_next(mu_interval_node* node, mu_interval interval)
{
    mu_rb_node* next = mu_rb_node_next_intersecting(&node->node, &interval,
                                                    mu_interval_search_compare,
                                                    mu_interval_max_compare);
    return next ? mu_rb_node_data(mu_interval_node, next, node) : NULL;
}

#define mu_interval_tree_foreach(type, iter, interval, tree, field)                     \
    for (type* iter, *__mu_node = (type*)mu_interval_tree_first(tree, interval);        \
         __mu_node != NULL &&                                                           \
         (iter = mu_rb_node_data(type, (mu_interval_node*)__mu_node, field), true);     \
         __mu_node = (type*)mu_interval_node_next((mu_interval_node*)__mu_node, interval))

MU_INLINE void mu_rb_tree__validate_node(mu_rb_node* node, int black_depth)
{
    if (!node)
    {
        MU_ASSERT(black_depth == 0);
        return;
    }

    if (mu_rb_node_is_black(node))
    {
        --black_depth;
    }
    else
    {
        MU_ASSERT(mu_rb_node_is_black(node->left));
        MU_ASSERT(mu_rb_node_is_black(node->right));
    }

    mu_rb_tree__validate_node(node->left, black_depth);
    mu_rb_tree__validate_node(node->right, black_depth);
}

MU_INLINE void mu_rb_tree_validate(mu_rb_tree* tree)
{
    unsigned black_depth = 0;
    mu_rb_node* n;

    if (!tree->root)
        return;

    MU_ASSERT(mu_rb_node_is_black(tree->root));
    for (n = tree->root; n; n = n->left)
        if (mu_rb_node_is_black(n))
            ++black_depth;

    mu_rb_tree__validate_node(tree->root, (int)black_depth);
}

MU_END_EXTERN_C

#endif /* MU_RB_TREE_H */
