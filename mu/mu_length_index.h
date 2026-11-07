#ifndef MU_LENGTH_INDEX_H
#define MU_LENGTH_INDEX_H

#include "mu_array.h"
#include "mu_span.h"

#define MU_LENGTH_BLOCK_CAPACITY 256u

typedef struct MuLengthBlock {
    uint32_t lengths[MU_LENGTH_BLOCK_CAPACITY];
    uint32_t count;
    uint32_t sum;
    uint32_t free_next;
} MuLengthBlock;

typedef struct MuLengthTotals {
    uint32_t count;
    uint32_t sum;
} MuLengthTotals;

typedef struct MuLengthPosition {
    uint32_t block;
    uint32_t slot;
    uint32_t ordinal;
    uint32_t offset;
} MuLengthPosition;

typedef struct MuLengthIndex {
    MuLengthBlock  *blocks;
    uint32_t       *order;
    MuLengthTotals *tree;
    uint32_t       *scratch;
    uint32_t        tree_base;
    uint32_t        free_head;
    uint64_t        rebuilds;
    uint64_t        length_writes;
} MuLengthIndex;

/* Nonempty sequences of unsigned lengths; zero lengths are supported.
 * Positions are borrowed and expire on mutation. Byte lookup at a boundary
 * selects the following nonempty element; total length selects the last element.
 * Initialize zeroed storage before use. Splice input is uint32_t records,
 * passed as a byte span, and must not alias index storage. */
MU_INLINE void mu_length_index_init(MuLengthIndex *index) {
    MU_MEMSET(index, 0, sizeof(*index));
    index->free_head = UINT32_MAX;
}

MU_INLINE void mu_length_index_destroy(MuLengthIndex *index) {
    array_free(index->blocks);
    array_free(index->order);
    array_free(index->tree);
    array_free(index->scratch);
    mu_length_index_init(index);
}

MU_INLINE void mu_length_index_rebuild(MuLengthIndex *index) {
    index->tree_base = 1;
    while (index->tree_base < array_size(index->order)) index->tree_base *= 2;
    array_reserve(index->tree, index->tree_base * 2);
    MU_MEMSET(index->tree, 0, index->tree_base * 2 * sizeof(*index->tree));
    for (uint32_t i = 0; i < array_size(index->order); ++i) {
        MuLengthBlock *block = &index->blocks[index->order[i]];
        MuLengthTotals totals;
        totals.count = block->count;
        totals.sum = block->sum;
        index->tree[index->tree_base + i] = totals;
    }
    for (uint32_t i = index->tree_base; --i;) {
        index->tree[i].count = index->tree[i * 2].count + index->tree[i * 2 + 1].count;
        index->tree[i].sum = index->tree[i * 2].sum + index->tree[i * 2 + 1].sum;
    }
    ++index->rebuilds;
}

MU_INLINE MuLengthPosition mu_length_index_find(const MuLengthIndex *index, uint32_t value, bool by_byte) {
    assert(index->tree && (by_byte ? value <= index->tree[1].sum : value < index->tree[1].count));
    if (by_byte && value == index->tree[1].sum)
        return mu_length_index_find(index, index->tree[1].count - 1, false);
    MuLengthPosition result;
    MU_MEMSET(&result, 0, sizeof(result));
    uint32_t node = 1;
    while (node < index->tree_base) {
        node *= 2;
        uint32_t weight = by_byte ? index->tree[node].sum : index->tree[node].count;
        if (value >= weight) {
            value -= weight;
            result.ordinal += index->tree[node].count;
            result.offset += index->tree[node].sum;
            ++node;
        }
    }
    result.block = node - index->tree_base;
    const MuLengthBlock *block = &index->blocks[index->order[result.block]];
    while (result.slot + 1 < block->count && value >= (by_byte ? block->lengths[result.slot] : 1u)) {
        value -= by_byte ? block->lengths[result.slot] : 1u;
        result.offset += block->lengths[result.slot++];
        ++result.ordinal;
    }
    return result;
}

MU_INLINE uint32_t mu_length_index_get(const MuLengthIndex *index, MuLengthPosition position) {
    return index->blocks[index->order[position.block]].lengths[position.slot];
}

MU_INLINE bool mu_length_index_next(const MuLengthIndex *index, MuLengthPosition *position) {
    if (position->ordinal + 1 == index->tree[1].count) return false;
    position->offset += mu_length_index_get(index, *position);
    ++position->ordinal;
    if (++position->slot == index->blocks[index->order[position->block]].count) {
        ++position->block;
        position->slot = 0;
    }
    return true;
}

MU_INLINE void mu_length_index_set(MuLengthIndex *index, MuLengthPosition position, uint32_t length) {
    MuLengthBlock *block = &index->blocks[index->order[position.block]];
    uint32_t delta = length - block->lengths[position.slot];
    block->lengths[position.slot] = length;
    block->sum += delta;
    for (uint32_t node = index->tree_base + position.block; node; node /= 2)
        index->tree[node].sum += delta;
    ++index->length_writes;
}

MU_INLINE void mu_length_index_splice(MuLengthIndex *index, uint32_t first, uint32_t removed, ByteSpan lengths) {
    assert(lengths.size % sizeof(uint32_t) == 0 && (lengths.data || !lengths.size));
    uint32_t inserted = lengths.size / sizeof(uint32_t);
    uint32_t old_count = index->tree ? index->tree[1].count : 0;
    assert(first <= old_count && removed <= old_count - first && old_count - removed + inserted > 0);
    uint32_t begin = 0, end = 0, prefix = 0, suffix = 0;
    if (old_count) {
        MuLengthPosition a = mu_length_index_find(index, first == old_count ? first - 1 : first, false);
        MuLengthPosition b = mu_length_index_find(index, first + removed == old_count ? old_count - 1 : first + removed, false);
        begin = a.block;
        end = b.block + 1;
        prefix = a.slot + (first == old_count);
        suffix = first + removed == old_count ? 0 : index->blocks[index->order[b.block]].count - b.slot;
        /* Low-water merging leaves hysteresis after evenly distributed splits. */
        if (prefix + inserted + suffix < MU_LENGTH_BLOCK_CAPACITY / 4) {
            if (end < array_size(index->order)) suffix += index->blocks[index->order[end++]].count;
            else if (begin) prefix += index->blocks[index->order[--begin]].count;
        }
    }
    uint32_t count = prefix + inserted + suffix;
    array_reserve(index->scratch, count);
    uint32_t out = 0;
    for (uint32_t i = begin; out < prefix; ++i) {
        MuLengthBlock *block = &index->blocks[index->order[i]];
        uint32_t n = block->count < prefix - out ? block->count : prefix - out;
        MU_MEMCPY(index->scratch + out, block->lengths, n * sizeof(uint32_t));
        out += n;
    }
    if (inserted) MU_MEMCPY(index->scratch + out, lengths.data, lengths.size);
    out += inserted;
    uint32_t tail = count;
    for (uint32_t i = end; tail > out;) {
        MuLengthBlock *block = &index->blocks[index->order[--i]];
        uint32_t n = block->count < tail - out ? block->count : tail - out;
        tail -= n;
        MU_MEMCPY(index->scratch + tail, block->lengths + block->count - n, n * sizeof(uint32_t));
    }
    if (end - begin == 1 && count <= MU_LENGTH_BLOCK_CAPACITY) {
        MuLengthBlock *block = &index->blocks[index->order[begin]];
        uint32_t sum = 0;
        for (uint32_t i = 0; i < count; ++i) sum += index->scratch[i];
        for (uint32_t node = index->tree_base + begin; node; node /= 2) {
            index->tree[node].count += count - block->count;
            index->tree[node].sum += sum - block->sum;
        }
        MU_MEMCPY(block->lengths, index->scratch, count * sizeof(uint32_t));
        block->count = count;
        block->sum = sum;
    } else {
        for (uint32_t i = begin; i < end; ++i) {
            uint32_t id = index->order[i];
            index->blocks[id].free_next = index->free_head;
            index->free_head = id;
        }
        uint32_t blocks = (count + MU_LENGTH_BLOCK_CAPACITY - 1) / MU_LENGTH_BLOCK_CAPACITY;
        uint32_t order_count = array_size(index->order);
        array_reserve(index->order, order_count - (end - begin) + blocks);
        memmove(index->order + begin + blocks, index->order + end, (order_count - end) * sizeof(uint32_t));
        array_header(index->order)->size = order_count - (end - begin) + blocks;
        out = 0;
        for (uint32_t i = 0; i < blocks; ++i) {
            uint32_t id = index->free_head;
            if (id == UINT32_MAX) {
                MuLengthBlock fresh;
                MU_MEMSET(&fresh, 0, sizeof(fresh));
                id = array_size(index->blocks);
                array_push(index->blocks, fresh);
            } else index->free_head = index->blocks[id].free_next;
            index->order[begin + i] = id;
            MuLengthBlock *block = &index->blocks[id];
            block->count = (count - out + blocks - i - 1) / (blocks - i);
            block->sum = 0;
            MU_MEMCPY(block->lengths, index->scratch + out, block->count * sizeof(uint32_t));
            for (uint32_t j = 0; j < block->count; ++j) block->sum += block->lengths[j];
            out += block->count;
        }
        mu_length_index_rebuild(index);
    }
    index->length_writes += count;
}

#endif
