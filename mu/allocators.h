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

/* Two cursors, not one. `used` grows up from the front, `tail` counts bytes
 * reserved at the back, and the invariant is simply `used + tail <= capacity`:
 * the middle window is free and either side may claim from it.
 *
 * FIELD ORDER MATTERS FOR INITIALISATION: there is no mu_arena_init, so a
 * mu_arena is brought to life by zeroing it and setting base/capacity. `tail`
 * being garbage is not a memory-safety hazard (every reader treats
 * `tail > capacity` as "no room at the back" and refuses) but it does make the
 * arena look full, so zero-initialise: `mu_arena a = {0}; a.base = buf;
 * a.capacity = n;` */
typedef struct mu_arena {
	uint8_t* base;
	uint32_t capacity;
	uint32_t used;
	uint32_t tail; /* bytes reserved at the END of the buffer */
} mu_arena;

void* mu_arena_alloc(mu_arena* a, uint32_t size, uint32_t align);
void  mu_arena_reset(mu_arena* a);

/* Scratchpad at the far end. mu_arena_alloc_top carves downward from
 * base + capacity - tail, so a temporary buffer taken here cannot push the
 * long-lived frame upward or fragment it: the two sides only ever meet in the
 * middle, and the tail side can be unwound on its own. Same power-of-two
 * `align` contract as mu_arena_alloc, measured from `base` like everything
 * else in this file. */
void*    mu_arena_alloc_top(mu_arena* a, uint32_t size, uint32_t align);
uint32_t mu_arena_top_mark(const mu_arena* a);           /* save the tail cursor  */
void     mu_arena_top_rewind(mu_arena* a, uint32_t mark); /* drop everything above it */
uint32_t mu_arena_available(const mu_arena* a);           /* free middle, in bytes */

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
	uint32_t* free_next; /* array [num_blocks], free list next links */
	uint32_t* free_prev; /* array [num_blocks], free list prev links (O(1) unlink) */

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

/* Alignment is applied to the *offset*, so `base` must itself be aligned to the
 * largest `align` you intend to ask for (any malloc'd or static buffer is fine
 * up to 16 on the usual ABIs; over-align the buffer for more). */
void* mu_arena_alloc(mu_arena* a, uint32_t size, uint32_t align) {
	if (!a || !a->base || size == 0u) return NULL;
	if (a->tail > a->capacity) return NULL; /* fail closed: never alias the tail */
	if (align == 0u) align = 1u;
	uint32_t p = mu_align_up_u32(a->used, align);
	uint32_t limit = a->capacity - a->tail;
	if (p > limit || size > limit - p) return NULL;
	a->used = p + size;
	return a->base + p;
}

void mu_arena_reset(mu_arena* a) {
	if (!a) return;
	a->used = 0u;
	a->tail = 0u;
}

/* Carve from the back. `tail` is a contiguous reservation at the end of the
 * buffer, so the top side is a stack: base + capacity - tail is the lowest
 * address handed out from the back, everything above it is reserved.
 *
 * The alignment gap below the returned block (at most align-1 bytes, between
 * the block's start and the previous reservation edge) stays reserved until the
 * matching rewind. That is the price of tracking the back with one counter; the
 * price of not tracking it at all is that a temporary buffer pushes the frame
 * cursor up and the long-lived allocations above it are gone for good. */
void* mu_arena_alloc_top(mu_arena* a, uint32_t size, uint32_t align) {
	if (!a || !a->base || size == 0u) return NULL;
	if (a->tail > a->capacity) return NULL;
	if (align == 0u) align = 1u;
	uint32_t end = a->capacity - a->tail;
	if (end < a->used) return NULL;
	if (size > end - a->used) return NULL;
	uint32_t start = (end - size) & ~(align - 1u);
	if (start < a->used) return NULL; /* the alignment gap crossed the bottom cursor */
	a->tail = a->capacity - start;
	return a->base + start;
}

uint32_t mu_arena_top_mark(const mu_arena* a) {
	return a ? a->tail : 0u;
}

void mu_arena_top_rewind(mu_arena* a, uint32_t mark) {
	if (!a || mark > a->capacity) return; /* refuse a bogus mark, keep the arena valid */
	a->tail = mark;
}

uint32_t mu_arena_available(const mu_arena* a) {
	if (!a || a->tail > a->capacity || a->used > a->capacity - a->tail) return 0u;
	return a->capacity - a->tail - a->used;
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

/* Doubly linked: coalescing has to remove a *specific* block (the sibling) from
 * a list it is in the middle of, so a singly linked list would mean an O(n)
 * scan on every free that merges. With free_prev the unlink is O(1) and so is
 * the whole free path. Costs one uint32 per block of metadata. */
MU_INLINE void mu_free_list_push(mu_buddy* b, uint32_t level, uint32_t block_index) {
	uint32_t head = b->free_head[level];
	b->free_next[block_index] = head;
	b->free_prev[block_index] = 0xFFFFFFFFu;
	if (head != 0xFFFFFFFFu) b->free_prev[head] = block_index;
	b->free_head[level] = block_index;
}

MU_INLINE uint32_t mu_free_list_pop(mu_buddy* b, uint32_t level) {
	uint32_t head = b->free_head[level];
	if (head == 0xFFFFFFFFu) return 0xFFFFFFFFu;
	b->free_head[level] = b->free_next[head];
	if (b->free_head[level] != 0xFFFFFFFFu) b->free_prev[b->free_head[level]] = 0xFFFFFFFFu;
	b->free_next[head] = 0xFFFFFFFFu;
	b->free_prev[head] = 0xFFFFFFFFu;
	return head;
}

MU_INLINE void mu_free_list_unlink(mu_buddy* b, uint32_t level, uint32_t block_index) {
	uint32_t prev = b->free_prev[block_index];
	uint32_t next = b->free_next[block_index];
	if (prev != 0xFFFFFFFFu) b->free_next[prev] = next;
	else b->free_head[level] = next;
	if (next != 0xFFFFFFFFu) b->free_prev[next] = prev;
	b->free_next[block_index] = 0xFFFFFFFFu;
	b->free_prev[block_index] = 0xFFFFFFFFu;
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
	uint32_t free_next_bytes = num_blocks * sizeof(uint32_t) * 2u;
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
	b->free_prev = (uint32_t*)meta;
	meta += num_blocks * sizeof(uint32_t);

	uint32_t num_internal = mu_buddy_num_internal(levels);
	b->split_map = (uint8_t*)meta;
	meta += mu_bitset_bytes(num_internal);

	b->merge_xor_map = (uint8_t*)meta;

	for (uint32_t i = 0u; i < levels; ++i) b->free_head[i] = 0xFFFFFFFFu;
	for (uint32_t i = 0u; i < num_blocks; ++i) {
		b->free_next[i] = 0xFFFFFFFFu;
		b->free_prev[i] = 0xFFFFFFFFu;
	}
	MU_MEMSET(b->split_map, 0, mu_bitset_bytes(num_internal));
	MU_MEMSET(b->merge_xor_map, 0, mu_bitset_bytes(num_blocks / 2u));

	/* Root starts free. */
	mu_free_list_push(b, 0u, 0u);
	return 1;
}

/* Sibling pairs are (2i+1, 2i+2); their shared id is the parent index i. */
MU_INLINE uint32_t mu_buddy_pair(uint32_t block_index) {
	return mu_parent_index(block_index);
}

MU_INLINE uint8_t* mu_buddy_ptr_at(mu_buddy* b, uint32_t level, uint32_t block_index) {
	uint32_t index_in_level = mu_index_in_level(b, level, block_index);
	return b->base + (size_t)index_in_level * (size_t)mu_level_block_size(b, level);
}

/* INVARIANT (merge_xor_map): bit(pair) == 1 exactly when *one* of the two
 * siblings sits in that level's free list. It is 0 when both siblings are
 * allocated or merged into their parent.
 *
 * The old code toggled the bit on free and never touched it on alloc, which
 * only works if blocks are freed in the exact reverse of the allocation order.
 * Any other order (reuse a freed block, free it again, free a sibling that was
 * never handed out) left bit == 1 while the sibling was allocated, the merge
 * then ran anyway, and the parent went on the free list on top of a live
 * child -- the same bytes came out of two allocations. Maintained from both
 * sides (set on free-with-allocated-sibling, cleared on alloc and on merge) the
 * bit is meaningful for any order. A fresh split pushes only the right child
 * and hands the left one straight to the caller so the invariant also holds
 * across splitting.
 *
 * ALIGNMENT: block starts are multiples of the block size, measured from
 * `arena`, so an `align` above that only holds if the arena itself is aligned
 * to it. Same contract as mu_arena_alloc. The request is rounded up to a power
 * of two at least `align`, so a large `align` costs a large block. */
void* mu_buddy_alloc(mu_buddy* b, uint32_t size, uint32_t align) {
	if (!b || size == 0u) return NULL;
	if (align == 0u) align = 1u;

	uint32_t need = size;
	if (need < align) need = align;
	if (need < b->leaf_size) need = b->leaf_size;
	need = mu_next_pow2_u32(need);

	if (need > b->total_size) return NULL;

	uint32_t level = mu_level_of_size(b, need);

	if (b->free_head[level] != 0xFFFFFFFFu) {
		uint32_t block = mu_free_list_pop(b, level);
		if (block == 0xFFFFFFFFu) return NULL;
		/* We just took the one free sibling: neither is free any more. */
		if (level > 0u) mu_bit_set(b->merge_xor_map, mu_buddy_pair(block), 0u);
		return mu_buddy_ptr_at(b, level, block);
	}

	/* Split down from the nearest ancestor that still has a free block. */
	uint32_t from = level;
	while (from > 0u && b->free_head[from] == 0xFFFFFFFFu) --from;
	if (b->free_head[from] == 0xFFFFFFFFu) return NULL;

	uint32_t block = mu_free_list_pop(b, from);
	if (block == 0xFFFFFFFFu) return NULL;
	/* We are taking the one free sibling of this pair, exactly as in the
	 * direct-pop path above; without this the pair bit stays 1 and a later free
	 * of the sibling merges with a block that is now split and in use. */
	if (from > 0u) mu_bit_set(b->merge_xor_map, mu_buddy_pair(block), 0u);

	while (from < level) {
		uint32_t left = mu_left_child(block);
		uint32_t right = mu_right_child(block);
		mu_bit_set(b->split_map, block, 1u);
		mu_free_list_push(b, from + 1u, right);
		mu_bit_set(b->merge_xor_map, block, 1u); /* right is free, left is handed down */
		block = left;
		++from;
	}
	return mu_buddy_ptr_at(b, level, block);
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

MU_INLINE void mu_buddy_free_at_level(mu_buddy* b, uint32_t level, uint32_t block) {
	if (level == 0u) {
		mu_free_list_push(b, 0u, 0u);
		return;
	}

	uint32_t pair = mu_buddy_pair(block);

	if (mu_bit_get(b->merge_xor_map, pair) == 0u) {
		/* Sibling is not free: park this block and record that it now is. */
		mu_bit_set(b->merge_xor_map, pair, 1u);
		mu_free_list_push(b, level, block);
		return;
	}

	/* The sibling is free and sits in this level's list: unlink it, then merge. */
	mu_free_list_unlink(b, level, mu_buddy_index(block));
	mu_bit_set(b->merge_xor_map, pair, 0u);

	uint32_t parent = mu_parent_index(block);
	mu_bit_set(b->split_map, parent, 0u);
	mu_buddy_free_at_level(b, level - 1u, parent);
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

	mu_buddy_free_at_level(b, level, mu_block_index(b, level, index_in_level));
}

void mu_buddy_free(mu_buddy* b, void* ptr) {
	if (!b || !ptr) return;
	uint32_t level = mu_buddy_level_from_ptr(b, ptr);
	uint32_t block_size = mu_level_block_size(b, level);
	uint32_t offset = (uint32_t)((uint8_t*)ptr - b->base);
	uint32_t index_in_level = offset / block_size;

	mu_buddy_free_at_level(b, level, mu_block_index(b, level, index_in_level));
}

#endif /* MU_ALLOCATORS_IMPLEMENTATION */