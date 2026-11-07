#pragma once
#ifndef MU_SPARSE_BITSET_H
#define MU_SPARSE_BITSET_H

/*
 * mu_sparse_bitset.h -- sparse bitset backed by an intrusive RB tree.
 *
 * Each node covers a fixed 1024-bit window. Windows with no set bits do not
 * exist, so a bitset over a huge sparse ID space only pays for touched
 * regions. Use this instead of mu_bitset/mu_chunked_bitset when IDs are large
 * and scattered; use a dense bitset when the domain is small or mostly full.
 *
 * Inspired by Mesa util/sparse_bitset.h (MIT).
 */

#include "mu_common.h"
#include "mu_rb_tree.h"

MU_BEGIN_EXTERN_C

#define MU_SPARSE_BITSET_LOG2_BITS_PER_NODE 10u
#define MU_SPARSE_BITSET_BITS_PER_NODE (UINT32_C(1) << MU_SPARSE_BITSET_LOG2_BITS_PER_NODE)
#define MU_SPARSE_BITSET_BIT_INDEX_MASK (MU_SPARSE_BITSET_BITS_PER_NODE - 1u)
#define MU_SPARSE_BITSET_OFFSET_MASK (~MU_SPARSE_BITSET_BIT_INDEX_MASK)
#define MU_SPARSE_BITSET_WORDS (MU_SPARSE_BITSET_BITS_PER_NODE / 64u)

typedef struct mu_sparse_bitset_node
{
    mu_rb_node node;
    uint32_t offset;
    uint64_t vals[MU_SPARSE_BITSET_WORDS];
} mu_sparse_bitset_node;

typedef struct mu_sparse_bitset
{
    mu_rb_tree tree;
} mu_sparse_bitset;

MU_INLINE unsigned mu_sparse_bitset__node_offset(const mu_rb_node* node)
{
    return MU_CONTAINER_OF(node, mu_sparse_bitset_node, node)->offset;
}

MU_INLINE int mu_sparse_bitset__compare(const mu_rb_node* a, const mu_rb_node* b)
{
    unsigned oa = mu_sparse_bitset__node_offset(a);
    unsigned ob = mu_sparse_bitset__node_offset(b);
    return (oa > ob) - (oa < ob);
}

MU_INLINE int mu_sparse_bitset__search_compare(const mu_rb_node* node, const void* key)
{
    unsigned offset = *(const unsigned*)key;
    unsigned node_offset = mu_sparse_bitset__node_offset(node);
    return (node_offset > offset) - (node_offset < offset);
}

MU_INLINE void mu_sparse_bitset_init(mu_sparse_bitset* s)
{
    MU_ASSERT(s != NULL);
    mu_rb_tree_init(&s->tree);
}

MU_INLINE mu_sparse_bitset_node* mu_sparse_bitset__get_node(mu_sparse_bitset* s,
                                                            unsigned offset)
{
    mu_rb_node* node = mu_rb_tree_search(&s->tree, &offset,
                                         mu_sparse_bitset__search_compare);
    return node ? MU_CONTAINER_OF(node, mu_sparse_bitset_node, node) : NULL;
}

MU_INLINE mu_sparse_bitset_node* mu_sparse_bitset__get_or_add(mu_sparse_bitset* s,
                                                             unsigned offset)
{
    mu_sparse_bitset_node* node = mu_sparse_bitset__get_node(s, offset);
    if (node)
        return node;

    node = (mu_sparse_bitset_node*)MU_CALLOC(1, sizeof(*node));
    if (!node)
        return NULL;

    node->offset = offset;
    mu_rb_tree_insert(&s->tree, &node->node, mu_sparse_bitset__compare);
    return node;
}

MU_INLINE bool mu_sparse_bitset__node_empty(const mu_sparse_bitset_node* node)
{
    unsigned i;
    for (i = 0; i < MU_SPARSE_BITSET_WORDS; ++i)
        if (node->vals[i] != 0)
            return false;
    return true;
}

MU_INLINE bool mu_sparse_bitset_set(mu_sparse_bitset* s, uint32_t bit)
{
    unsigned offset = bit & MU_SPARSE_BITSET_OFFSET_MASK;
    unsigned index = bit & MU_SPARSE_BITSET_BIT_INDEX_MASK;
    mu_sparse_bitset_node* node;
    MU_ASSERT(s != NULL);

    node = mu_sparse_bitset__get_or_add(s, offset);
    if (!node)
        return false;

    node->vals[index / 64u] |= UINT64_C(1) << (index % 64u);
    return true;
}

MU_INLINE void mu_sparse_bitset_clear(mu_sparse_bitset* s, uint32_t bit)
{
    unsigned offset = bit & MU_SPARSE_BITSET_OFFSET_MASK;
    unsigned index = bit & MU_SPARSE_BITSET_BIT_INDEX_MASK;
    mu_sparse_bitset_node* node;
    MU_ASSERT(s != NULL);

    node = mu_sparse_bitset__get_node(s, offset);
    if (!node)
        return;

    node->vals[index / 64u] &= ~(UINT64_C(1) << (index % 64u));
    if (mu_sparse_bitset__node_empty(node))
    {
        mu_rb_tree_remove(&s->tree, &node->node);
        MU_FREE(node);
    }
}

MU_INLINE bool mu_sparse_bitset_test(const mu_sparse_bitset* s, uint32_t bit)
{
    unsigned offset = bit & MU_SPARSE_BITSET_OFFSET_MASK;
    unsigned index = bit & MU_SPARSE_BITSET_BIT_INDEX_MASK;
    mu_sparse_bitset_node* node;
    MU_ASSERT(s != NULL);

    node = mu_sparse_bitset__get_node((mu_sparse_bitset*)s, offset);
    return node && (node->vals[index / 64u] & (UINT64_C(1) << (index % 64u))) != 0;
}

MU_INLINE unsigned mu_sparse_bitset_count(const mu_sparse_bitset* s)
{
    unsigned count = 0;
    MU_ASSERT(s != NULL);

    mu_rb_tree_foreach(mu_sparse_bitset_node, node, (mu_rb_tree*)&s->tree, node)
    {
        unsigned i;
        for (i = 0; i < MU_SPARSE_BITSET_WORDS; ++i)
            count += mu_popcount_u64(node->vals[i]);
    }
    return count;
}

MU_INLINE bool mu_sparse_bitset_empty(const mu_sparse_bitset* s)
{
    MU_ASSERT(s != NULL);
    return mu_rb_tree_first((mu_rb_tree*)&s->tree) == NULL;
}

MU_INLINE void mu_sparse_bitset_clear_all(mu_sparse_bitset* s)
{
    MU_ASSERT(s != NULL);
    mu_rb_tree_foreach_safe(mu_sparse_bitset_node, node, &s->tree, node)
    {
        mu_rb_tree_remove(&s->tree, &node->node);
        MU_FREE(node);
    }
    mu_rb_tree_init(&s->tree);
}

MU_INLINE void mu_sparse_bitset_destroy(mu_sparse_bitset* s)
{
    if (s)
        mu_sparse_bitset_clear_all(s);
}

MU_INLINE bool mu_sparse_bitset_dup(mu_sparse_bitset* dst, const mu_sparse_bitset* src)
{
    MU_ASSERT(dst != NULL && src != NULL);
    mu_sparse_bitset_init(dst);

    mu_rb_tree_foreach(mu_sparse_bitset_node, node, (mu_rb_tree*)&src->tree, node)
    {
        mu_sparse_bitset_node* copy;
        if (mu_sparse_bitset__node_empty(node))
            continue;

        copy = (mu_sparse_bitset_node*)MU_MALLOC(sizeof(*copy));
        if (!copy)
        {
            mu_sparse_bitset_destroy(dst);
            return false;
        }

        MU_MEMCPY(copy, node, sizeof(*copy));
        copy->node.parent = 0;
        copy->node.left = NULL;
        copy->node.right = NULL;
        mu_rb_tree_insert(&dst->tree, &copy->node, mu_sparse_bitset__compare);
    }
    return true;
}

/* Returns -1 on allocation failure, 0 if unchanged, 1 if dst gained bits. */
MU_INLINE int mu_sparse_bitset_merge(mu_sparse_bitset* dst, const mu_sparse_bitset* src)
{
    bool changed = false;
    MU_ASSERT(dst != NULL && src != NULL);

    mu_rb_tree_foreach(mu_sparse_bitset_node, node, (mu_rb_tree*)&src->tree, node)
    {
        mu_sparse_bitset_node* out;
        unsigned i;
        if (mu_sparse_bitset__node_empty(node))
            continue;

        out = mu_sparse_bitset__get_or_add(dst, node->offset);
        if (!out)
            return -1;

        for (i = 0; i < MU_SPARSE_BITSET_WORDS; ++i)
        {
            changed |= (node->vals[i] & ~out->vals[i]) != 0;
            out->vals[i] |= node->vals[i];
        }
    }
    return changed ? 1 : 0;
}

MU_INLINE bool mu_sparse_bitset_contains(const mu_sparse_bitset* a,
                                         const mu_sparse_bitset* b)
{
    MU_ASSERT(a != NULL && b != NULL);

    mu_rb_tree_foreach(mu_sparse_bitset_node, node, (mu_rb_tree*)&b->tree, node)
    {
        mu_sparse_bitset_node* other;
        unsigned i;
        if (mu_sparse_bitset__node_empty(node))
            continue;

        other = mu_sparse_bitset__get_node((mu_sparse_bitset*)a, node->offset);
        if (!other)
            return false;
        for (i = 0; i < MU_SPARSE_BITSET_WORDS; ++i)
            if ((node->vals[i] & ~other->vals[i]) != 0)
                return false;
    }
    return true;
}

MU_INLINE uint32_t mu_sparse_bitset__next_in_node(const mu_sparse_bitset_node* node,
                                                  unsigned from)
{
    unsigned word = from / 64u;
    unsigned shift = from % 64u;
    uint64_t bits;

    if (from >= MU_SPARSE_BITSET_BITS_PER_NODE)
        return MU_SPARSE_BITSET_BITS_PER_NODE;

    bits = node->vals[word] & (~UINT64_C(0) << shift);
    if (bits)
        return word * 64u + mu_trailing_zeroes_u64(bits);

    for (++word; word < MU_SPARSE_BITSET_WORDS; ++word)
        if (node->vals[word])
            return word * 64u + mu_trailing_zeroes_u64(node->vals[word]);

    return MU_SPARSE_BITSET_BITS_PER_NODE;
}

/* Find the first set bit >= *from. On success stores the bit in *from. */
MU_INLINE bool mu_sparse_bitset_next_set(const mu_sparse_bitset* s, uint32_t* from)
{
    unsigned offset;
    mu_rb_node* rb;
    MU_ASSERT(s != NULL && from != NULL);

    if (*from == UINT32_MAX)
        return false;

    offset = *from & MU_SPARSE_BITSET_OFFSET_MASK;
    rb = mu_rb_tree_search_sloppy((mu_rb_tree*)&s->tree, &offset,
                                  mu_sparse_bitset__search_compare);
    if (rb && mu_sparse_bitset__node_offset(rb) < offset)
        rb = mu_rb_node_next(rb);

    for (; rb; rb = mu_rb_node_next(rb))
    {
        mu_sparse_bitset_node* node = MU_CONTAINER_OF(rb, mu_sparse_bitset_node, node);
        unsigned start = (node->offset == offset) ? (*from & MU_SPARSE_BITSET_BIT_INDEX_MASK) : 0u;
        uint32_t bit = mu_sparse_bitset__next_in_node(node, start);
        if (bit < MU_SPARSE_BITSET_BITS_PER_NODE)
        {
            *from = node->offset + bit;
            return true;
        }
    }

    *from = UINT32_MAX;
    return false;
}

#define mu_sparse_bitset_foreach(s, bit)                                                \
    for (uint32_t bit, _mu_from = 0;                                                    \
         _mu_from != UINT32_MAX && mu_sparse_bitset_next_set((s), &_mu_from) &&         \
         ((bit = _mu_from), true);                                                      \
         _mu_from = (bit == UINT32_MAX) ? UINT32_MAX : bit + 1u)

MU_END_EXTERN_C

#endif /* MU_SPARSE_BITSET_H */
