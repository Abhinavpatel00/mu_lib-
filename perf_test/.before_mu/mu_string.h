

#pragma once
/*
===============================================================================

    ====================

    Tiny dense-storage primitives for engine code.
    Built for the usual mess:
      - asset registries
      - entity metadata
      - stable string tables
      - intrusive update/render lists
      - freelists for pool allocators

    These are not "generic containers" in the bloated STL sense.
    They are low-level engine primitives:
      - predictable
      - serializable
      - cache-friendly
      - hostile to allocator abuse

    ===========================================================================
    OVERVIEW
    ===========================================================================

    1) mu_string_arena
       Packed string storage with stable indices.

       Use when:
         - storing asset names
         - material names
         - animation clip names
         - shader defines
         - tags / metadata

       Avoids:
         - one malloc per string
         - fragmented pointer soup
         - annoying serialization

       Layout:

            offsets[]                  data[]
         +----------+              +---+---+---+---+---+---+---+---+
         |    0     | -----------> | p | l | a | y | e | r | 0 | e |
         +----------+              +---+---+---+---+---+---+---+---+
         |    7     | --------------------------------------------^
         +----------+                                              |
         |   13     | ----------------------------------------+    |
         +----------+                                         |    |
                                                              v    v
                                                           "enemy" "fx"

         index 0 -> "player"
         index 1 -> "enemy"
         index 2 -> "fx"

    ---------------------------------------------------------------------------

    2) mu_pool_link
       Intrusive index-based doubly linked node.

       Use when:
         - objects live in arrays
         - pointers are unstable / unwanted
         - handles must remain stable
         - serialization matters

       This is not heap linked-list nonsense.
       Nodes live in dense arrays.
       Links are indices, not pointers.

       Sentinel list layout:

             head (sentinel)
            +-------------+
            | prev =  3   |
            | next =  1   |
            +-------------+
               ^       |
               |       v
            +------+ +------+ +------+
            |  3   |<|  1   |<|  2   |
            +------+ +------+ +------+

       Empty:
         head.next = head
         head.prev = head

       Detached node:
         node.next = node
         node.prev = node

    ---------------------------------------------------------------------------

    3) mu_freelist
       Stack-like free-slot list using mu_pool_link::next only.

       Use when:
         - recycling entity slots
         - pool allocators
         - instance handles
         - transient job nodes

       Layout:

           head -> 8 -> 4 -> 12 -> head

       pop() returns 8

       This is a stack.
       Not a list.
       Humans love confusing those. Don't.

===============================================================================
*/

#ifndef MU_CORE_CONTAINERS_H
#define MU_CORE_CONTAINERS_H
#include "mu_array.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ============================================================================
   Config
   ========================================================================== */


#ifndef MU_INVALID_INDEX
#define MU_INVALID_INDEX UINT32_MAX
#endif

/* User must provide:
 *
 *   mu_array_size(arr)
 *   mu_array_capacity(arr)
 *   mu_array_ensure(arr, count)
 *   mu_array_push(arr, value)
 *   mu_array_free(arr)
 */

/* ============================================================================
    mu_string_arena
    ========================================================================== */
typedef struct mu_string_arena
{
    char*    data;     /* packed string bytes                    */
    uint32_t size;     /* bytes used in data[]                   */
    uint32_t capacity; /* cached byte capacity (optional mirror) */

    uint32_t* offsets; /* string offsets into data[]             */
    uint32_t  count;   /* cached string count                    */
} mu_string_arena;

MU_INLINE void mu_string_arena_init(mu_string_arena* a)
{
    a->data     = NULL;
    a->size     = 0u;
    a->capacity = 0u;
    a->offsets  = NULL;
    a->count    = 0u;
}

MU_INLINE void mu_string_arena_free(mu_string_arena* a)
{
    array_free(a->data);
    array_free(a->offsets);

    a->data     = NULL;
    a->size     = 0u;
    a->capacity = 0u;
    a->offsets  = NULL;
    a->count    = 0u;
}

MU_INLINE void mu_string_arena_clear(mu_string_arena* a)
{
    a->size  = 0u;
    a->count = 0u;

    /* keep allocated memory, because reallocating every frame is clown behavior */
    if(a->data)
        a->data[0] = '\0';
}

MU_INLINE bool mu_string_arena_push(mu_string_arena* a, const char* s, uint32_t* out_index)
{
    MU_ASSERT(a);
    MU_ASSERT(s);

    const size_t len = strlen(s) + 1u;
    if((uint64_t)a->size + (uint64_t)len > UINT32_MAX)
        return false;

    const uint32_t offset = a->size;

    array_reserve(a->data, a->size + (uint32_t)len);
    if(!a->data)
        return false;

    MU_MEMCPY(a->data + a->size, s, len);
    a->size += (uint32_t)len;
    a->capacity = (uint32_t)array_capacity(a->data);

    array_push(a->offsets, offset);
    if(!a->offsets)
        return false;

    a->count = (uint32_t)array_size(a->offsets);

    if(out_index)
        *out_index = a->count - 1u;

    return true;
}

MU_INLINE const char* mu_string_arena_get(const mu_string_arena* a, uint32_t index)
{
    MU_ASSERT(a);

    if(!a->data || index >= a->count)
        return NULL;

    return a->data + a->offsets[index];
}

MU_INLINE uint32_t mu_string_arena_count(const mu_string_arena* a)
{
    return a ? a->count : 0u;
}

MU_INLINE uint32_t mu_string_arena_bytes(const mu_string_arena* a)
{
    return a ? a->size : 0u;
}

/* ============================================================================
   mu_pool_link
   ========================================================================== */

typedef struct mu_pool_link
{
    uint32_t prev;
    uint32_t next;
} mu_pool_link;

MU_INLINE void mu_pool_link_detach(mu_pool_link* links, uint32_t node)
{
    links[node].prev = node;
    links[node].next = node;
}

MU_INLINE bool mu_pool_link_is_detached(const mu_pool_link* links, uint32_t node)
{
    return links[node].prev == node && links[node].next == node;
}

/* ============================================================================
   Indexed intrusive list
   ========================================================================== */

MU_INLINE void mu_index_list_init(mu_pool_link* links, uint32_t head)
{
    links[head].next = head;
    links[head].prev = head;
}

MU_INLINE bool mu_index_list_empty(const mu_pool_link* links, uint32_t head)
{
    return links[head].next == head;
}

MU_INLINE void mu_index_list_insert_after(mu_pool_link* links, uint32_t at, uint32_t node)
{
    MU_ASSERT(mu_pool_link_is_detached(links, node));

    links[node].next = links[at].next;
    links[node].prev = at;

    links[links[at].next].prev = node;
    links[at].next             = node;
}

MU_INLINE void mu_index_list_insert_before(mu_pool_link* links, uint32_t at, uint32_t node)
{
    MU_ASSERT(mu_pool_link_is_detached(links, node));

    links[node].prev = links[at].prev;
    links[node].next = at;

    links[links[at].prev].next = node;
    links[at].prev             = node;
}

MU_INLINE void mu_index_list_remove(mu_pool_link* links, uint32_t node)
{
    MU_ASSERT(!mu_pool_link_is_detached(links, node));

    links[links[node].prev].next = links[node].next;
    links[links[node].next].prev = links[node].prev;

    mu_pool_link_detach(links, node);
}

/* ============================================================================
   Indexed freelist (stack)
   ========================================================================== */

MU_INLINE void mu_freelist_init(mu_pool_link* links, uint32_t head)
{
    links[head].next = head;
}

MU_INLINE bool mu_freelist_empty(const mu_pool_link* links, uint32_t head)
{
    return links[head].next == head;
}

MU_INLINE void mu_freelist_push(mu_pool_link* links, uint32_t head, uint32_t node)
{
    links[node].next = links[head].next;
    links[head].next = node;
}

MU_INLINE uint32_t mu_freelist_pop(mu_pool_link* links, uint32_t head)
{
    const uint32_t first = links[head].next;
    if(first == head)
        return MU_INVALID_INDEX;

    links[head].next  = links[first].next;
    links[first].next = first;
    return first;
}

#endif /* MU_CORE_CONTAINERS_H */
