#ifndef MU_ALLOCATORS_H
#define MU_ALLOCATORS_H

// C99 single-header allocator collection inspired by allocation-adventures docs.
// Define MU_ALLOCATORS_IMPLEMENTATION in one C/C++ file to enable function bodies.

#include "mu_macros.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------- Utilities ----------- */

MU_INLINE uint32_t mu_align_up_u32(uint32_t x, uint32_t a) {
	return (x + (a - 1u)) & ~(a - 1u);
}

MU_INLINE uintptr_t mu_align_up_ptr(uintptr_t x, uintptr_t a) {
	return (x + (a - 1u)) & ~(a - 1u);
}

MU_INLINE uint32_t mu_is_pow2_u32(uint32_t x) {
	return x && ((x & (x - 1u)) == 0u);
}

MU_INLINE uint32_t mu_next_pow2_u32(uint32_t x) {
	if (x <= 1u) return 1u;
	--x;
	x |= x >> 1u;
	x |= x >> 2u;
	x |= x >> 4u;
	x |= x >> 8u;
	x |= x >> 16u;
	return x + 1u;
}

/* ----------- Linear Arena (bump) ----------- */

typedef struct mu_arena {
	uint8_t* base;
	uint32_t capacity;
	uint32_t used;
} mu_arena;

void* mu_arena_alloc(mu_arena* a, uint32_t size, uint32_t align);
void  mu_arena_reset(mu_arena* a);

/* ----------- Fixed Block Pool ----------- */

typedef struct mu_pool {
	uint8_t* base;
	uint32_t block_size;
	uint32_t block_count;
	uint32_t free_head; /* index of first free block, or 0xFFFFFFFF */
} mu_pool;

int   mu_pool_init(mu_pool* p, void* buffer, uint32_t block_size, uint32_t block_count);
void* mu_pool_alloc(mu_pool* p);
void  mu_pool_free(mu_pool* p, void* ptr);

/* ----------- Buddy Allocator ----------- */

typedef struct mu_buddy {
	uint8_t* base;
	uint32_t total_size;
	uint32_t leaf_size;
	uint32_t num_levels; /* level 0 = root */

	uint32_t* free_head; /* array [num_levels], head indices or 0xFFFFFFFF */
	uint32_t* free_next; /* array [num_blocks], singly-linked free list nodes */

	uint8_t* split_map;     /* bitset for internal nodes */
	uint8_t* merge_xor_map; /* bitset for buddy pair xor */
} mu_buddy;

/* Returns required bytes for metadata arrays for a given arena. */
uint32_t mu_buddy_metadata_size(uint32_t total_size, uint32_t leaf_size);

/* Initializes buddy allocator with user-provided metadata buffer. */
int mu_buddy_init(mu_buddy* b,
				  void* arena, uint32_t arena_size,
				  uint32_t leaf_size,
				  void* metadata, uint32_t metadata_size);

void* mu_buddy_alloc(mu_buddy* b, uint32_t size, uint32_t align);
void  mu_buddy_free_known(mu_buddy* b, void* ptr, uint32_t size);
void  mu_buddy_free(mu_buddy* b, void* ptr);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MU_ALLOCATORS_H */

#ifdef MU_ALLOCATORS_IMPLEMENTATION

/* ----------- Linear Arena ----------- */

void* mu_arena_alloc(mu_arena* a, uint32_t size, uint32_t align) {
	if (!a || !a->base || size == 0u) return NULL;
	if (align == 0u) align = 1u;
	uint32_t p = mu_align_up_u32(a->used, align);
	if (p + size > a->capacity) return NULL;
	a->used = p + size;
	return a->base + p;
}

void mu_arena_reset(mu_arena* a) {
	if (!a) return;
	a->used = 0u;
}

/* ----------- Fixed Block Pool ----------- */

int mu_pool_init(mu_pool* p, void* buffer, uint32_t block_size, uint32_t block_count) {
	if (!p || !buffer || block_size < sizeof(uint32_t) || block_count == 0u) return 0;
	p->base = (uint8_t*)buffer;
	p->block_size = block_size;
	p->block_count = block_count;
	p->free_head = 0u;

	for (uint32_t i = 0u; i < block_count; ++i) {
		uint32_t* next = (uint32_t*)(p->base + i * block_size);
		*next = (i + 1u < block_count) ? (i + 1u) : 0xFFFFFFFFu;
	}
	return 1;
}

void* mu_pool_alloc(mu_pool* p) {
	if (!p || p->free_head == 0xFFFFFFFFu) return NULL;
	uint32_t idx = p->free_head;
	uint8_t* block = p->base + idx * p->block_size;
	p->free_head = *(uint32_t*)block;
	return block;
}

void mu_pool_free(mu_pool* p, void* ptr) {
	if (!p || !ptr) return;
	uint8_t* bptr = (uint8_t*)ptr;
	uint32_t idx = (uint32_t)((bptr - p->base) / p->block_size);
	uint32_t* next = (uint32_t*)bptr;
	*next = p->free_head;
	p->free_head = idx;
}

/* ----------- Buddy Allocator ----------- */

MU_INLINE uint32_t mu_buddy_num_levels(uint32_t total_size, uint32_t leaf_size) {
	uint32_t levels = 0u;
	while (total_size >= leaf_size) {
		total_size >>= 1u;
		++levels;
	}
	return levels;
}

MU_INLINE uint32_t mu_buddy_num_blocks(uint32_t num_levels) {
	return (1u << num_levels) - 1u;
}

MU_INLINE uint32_t mu_buddy_num_internal(uint32_t num_levels) {
	return (1u << (num_levels - 1u)) - 1u;
}

MU_INLINE uint32_t mu_bitset_bytes(uint32_t bits) {
	return (bits + 7u) / 8u;
}

MU_INLINE uint32_t mu_bit_get(const uint8_t* bits, uint32_t idx) {
	return (bits[idx >> 3u] >> (idx & 7u)) & 1u;
}

MU_INLINE void mu_bit_set(uint8_t* bits, uint32_t idx, uint32_t v) {
	uint32_t byte = idx >> 3u;
	uint8_t mask = (uint8_t)(1u << (idx & 7u));
	if (v) bits[byte] |= mask; else bits[byte] &= (uint8_t)~mask;
}

MU_INLINE void mu_bit_flip(uint8_t* bits, uint32_t idx) {
	bits[idx >> 3u] ^= (uint8_t)(1u << (idx & 7u));
}

MU_INLINE uint32_t mu_level_block_size(const mu_buddy* b, uint32_t level) {
	return b->total_size >> level;
}

MU_INLINE uint32_t mu_level_first_index(uint32_t level) {
	return (1u << level) - 1u;
}

MU_INLINE uint32_t mu_level_of_size(const mu_buddy* b, uint32_t size) {
	uint32_t level = 0u;
	uint32_t block = b->total_size;
	while (level + 1u < b->num_levels && (block >> 1u) >= size) {
		block >>= 1u;
		++level;
	}
	return level;
}

MU_INLINE uint32_t mu_block_index(const mu_buddy* b, uint32_t level, uint32_t index_in_level) {
	(void)b;
	return mu_level_first_index(level) + index_in_level;
}

MU_INLINE uint32_t mu_index_in_level(const mu_buddy* b, uint32_t level, uint32_t block_index) {
	(void)b;
	return block_index - mu_level_first_index(level);
}

MU_INLINE uint32_t mu_parent_index(uint32_t block_index) {
	return (block_index - 1u) >> 1u;
}

MU_INLINE uint32_t mu_left_child(uint32_t block_index) {
	return (block_index << 1u) + 1u;
}

MU_INLINE uint32_t mu_right_child(uint32_t block_index) {
	return (block_index << 1u) + 2u;
}

MU_INLINE uint32_t mu_buddy_index(uint32_t block_index) {
	return (block_index & 1u) ? (block_index + 1u) : (block_index - 1u);
}

MU_INLINE void mu_free_list_push(mu_buddy* b, uint32_t level, uint32_t block_index) {
	b->free_next[block_index] = b->free_head[level];
	b->free_head[level] = block_index;
}

MU_INLINE uint32_t mu_free_list_pop(mu_buddy* b, uint32_t level) {
	uint32_t head = b->free_head[level];
	if (head == 0xFFFFFFFFu) return 0xFFFFFFFFu;
	b->free_head[level] = b->free_next[head];
	b->free_next[head] = 0xFFFFFFFFu;
	return head;
}

uint32_t mu_buddy_metadata_size(uint32_t total_size, uint32_t leaf_size) {
	if (!mu_is_pow2_u32(total_size) || !mu_is_pow2_u32(leaf_size) || total_size < leaf_size) {
		return 0u;
	}
	uint32_t levels = mu_buddy_num_levels(total_size, leaf_size);
	if (levels < 1u) return 0u;
	uint32_t num_blocks = mu_buddy_num_blocks(levels);
	uint32_t num_internal = mu_buddy_num_internal(levels);
	uint32_t free_head_bytes = levels * sizeof(uint32_t);
	uint32_t free_next_bytes = num_blocks * sizeof(uint32_t);
	uint32_t split_bytes = mu_bitset_bytes(num_internal);
	uint32_t xor_bytes = mu_bitset_bytes(num_blocks / 2u);
	return free_head_bytes + free_next_bytes + split_bytes + xor_bytes;
}

int mu_buddy_init(mu_buddy* b,
				  void* arena, uint32_t arena_size,
				  uint32_t leaf_size,
				  void* metadata, uint32_t metadata_size) {
	if (!b || !arena || !metadata) return 0;
	if (!mu_is_pow2_u32(arena_size) || !mu_is_pow2_u32(leaf_size) || arena_size < leaf_size) return 0;

	uint32_t levels = mu_buddy_num_levels(arena_size, leaf_size);
	if (levels < 1u) return 0;

	uint32_t need = mu_buddy_metadata_size(arena_size, leaf_size);
	if (metadata_size < need) return 0;

	uint8_t* meta = (uint8_t*)metadata;
	b->base = (uint8_t*)arena;
	b->total_size = arena_size;
	b->leaf_size = leaf_size;
	b->num_levels = levels;

	b->free_head = (uint32_t*)meta;
	meta += levels * sizeof(uint32_t);

	uint32_t num_blocks = mu_buddy_num_blocks(levels);
	b->free_next = (uint32_t*)meta;
	meta += num_blocks * sizeof(uint32_t);

	uint32_t num_internal = mu_buddy_num_internal(levels);
	b->split_map = (uint8_t*)meta;
	meta += mu_bitset_bytes(num_internal);

	b->merge_xor_map = (uint8_t*)meta;

	for (uint32_t i = 0u; i < levels; ++i) b->free_head[i] = 0xFFFFFFFFu;
	for (uint32_t i = 0u; i < num_blocks; ++i) b->free_next[i] = 0xFFFFFFFFu;
	MU_MEMSET(b->split_map, 0, mu_bitset_bytes(num_internal));
	MU_MEMSET(b->merge_xor_map, 0, mu_bitset_bytes(num_blocks / 2u));

	/* Root starts free. */
	mu_free_list_push(b, 0u, 0u);
	return 1;
}

MU_INLINE uint32_t mu_buddy_split_to_level(mu_buddy* b, uint32_t level) {
	if (level == 0u) return 0xFFFFFFFFu;
	if (b->free_head[level - 1u] == 0xFFFFFFFFu) {
		uint32_t parent = mu_buddy_split_to_level(b, level - 1u);
		if (parent == 0xFFFFFFFFu) return 0xFFFFFFFFu;
		(void)parent;
	}

	uint32_t block = mu_free_list_pop(b, level - 1u);
	if (block == 0xFFFFFFFFu) return 0xFFFFFFFFu;

	uint32_t left = mu_left_child(block);
	uint32_t right = mu_right_child(block);

	mu_bit_set(b->split_map, block, 1u);
	mu_free_list_push(b, level, right);
	mu_free_list_push(b, level, left);

	return block;
}

void* mu_buddy_alloc(mu_buddy* b, uint32_t size, uint32_t align) {
	if (!b || size == 0u) return NULL;
	if (align == 0u) align = 1u;

	uint32_t need = size;
	if (need < align) need = align;
	if (need < b->leaf_size) need = b->leaf_size;
	need = mu_next_pow2_u32(need);

	if (need > b->total_size) return NULL;

	uint32_t level = mu_level_of_size(b, need);
	if (b->free_head[level] == 0xFFFFFFFFu) {
		if (mu_buddy_split_to_level(b, level) == 0xFFFFFFFFu) return NULL;
	}

	uint32_t block = mu_free_list_pop(b, level);
	if (block == 0xFFFFFFFFu) return NULL;

	uint32_t index_in_level = mu_index_in_level(b, level, block);
	uint32_t offset = index_in_level * mu_level_block_size(b, level);
	return b->base + offset;
}

MU_INLINE uint32_t mu_buddy_level_from_ptr(mu_buddy* b, void* ptr) {
	uint8_t* p = (uint8_t*)ptr;
	uint32_t offset = (uint32_t)(p - b->base);
	uint32_t level = 0u;
	uint32_t block_size = b->total_size;

	while (level + 1u < b->num_levels) {
		uint32_t index = offset / block_size;
		uint32_t block_index = mu_block_index(b, level, index);
		if (!mu_bit_get(b->split_map, block_index)) return level;
		block_size >>= 1u;
		++level;
	}
	return b->num_levels - 1u;
}

MU_INLINE void mu_buddy_free_at_level(mu_buddy* b, uint32_t level, uint32_t index_in_level) {
	uint32_t block = mu_block_index(b, level, index_in_level);

	/* Toggle buddy-pair xor bit for this level. */
	if (level > 0u) {
		uint32_t pair = block / 2u;
		mu_bit_flip(b->merge_xor_map, pair);
	}

	if (level == 0u) {
		mu_free_list_push(b, level, block);
		return;
	}

	uint32_t buddy = mu_buddy_index(block);
	uint32_t pair = block / 2u;
	uint32_t can_merge = (mu_bit_get(b->merge_xor_map, pair) == 0u);

	if (!can_merge) {
		mu_free_list_push(b, level, block);
		return;
	}

	/* Remove buddy from free list by linear scan if needed. */
	uint32_t prev = 0xFFFFFFFFu;
	uint32_t cur = b->free_head[level];
	while (cur != 0xFFFFFFFFu && cur != buddy) {
		prev = cur;
		cur = b->free_next[cur];
	}
	if (cur == buddy) {
		if (prev == 0xFFFFFFFFu) b->free_head[level] = b->free_next[cur];
		else b->free_next[prev] = b->free_next[cur];
		b->free_next[cur] = 0xFFFFFFFFu;
	}

	mu_bit_set(b->split_map, mu_parent_index(block), 0u);

	uint32_t parent = mu_parent_index(block);
	uint32_t parent_level = level - 1u;
	uint32_t parent_index_in_level = mu_index_in_level(b, parent_level, parent);
	mu_buddy_free_at_level(b, parent_level, parent_index_in_level);
}

void mu_buddy_free_known(mu_buddy* b, void* ptr, uint32_t size) {
	if (!b || !ptr || size == 0u) return;
	uint32_t need = size;
	if (need < b->leaf_size) need = b->leaf_size;
	need = mu_next_pow2_u32(need);
	if (need > b->total_size) return;

	uint32_t level = mu_level_of_size(b, need);
	uint32_t block_size = mu_level_block_size(b, level);
	uint32_t offset = (uint32_t)((uint8_t*)ptr - b->base);
	uint32_t index_in_level = offset / block_size;

	mu_buddy_free_at_level(b, level, index_in_level);
}

void mu_buddy_free(mu_buddy* b, void* ptr) {
	if (!b || !ptr) return;
	uint32_t level = mu_buddy_level_from_ptr(b, ptr);
	uint32_t block_size = mu_level_block_size(b, level);
	uint32_t offset = (uint32_t)((uint8_t*)ptr - b->base);
	uint32_t index_in_level = offset / block_size;

	mu_buddy_free_at_level(b, level, index_in_level);
}

#endif /* MU_ALLOCATORS_IMPLEMENTATION */