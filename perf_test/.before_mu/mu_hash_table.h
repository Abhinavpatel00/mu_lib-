
#pragma  once 

#include "mu_common.h"

typedef struct
{
    uint32_t  capacity;
    uint32_t  count;
    uint64_t* keys;
    uint64_t* values;
} hash_t;

#define HASH_EMPTY 0

// ------------------------------------------------------------
// Simple mix (not crypto, just decent)
// ------------------------------------------------------------
MU_INLINE uint64_t hash_u64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// ------------------------------------------------------------
MU_INLINE void hash_init(hash_t* h, uint32_t cap)
{
    h->capacity = cap;
    h->count    = 0;
    h->keys     = (uint64_t*)MU_CALLOC(cap, sizeof(uint64_t));
    h->values   = (uint64_t*)MU_CALLOC(cap, sizeof(uint64_t));
}

// ------------------------------------------------------------
MU_INLINE void hash_free(hash_t* h)
{
    MU_FREE(h->keys);
    MU_FREE(h->values);
}

// ------------------------------------------------------------
// Linear probing
//
// index = hash % capacity
// if collision → move forward
//
// Worst case: O(n)
// Average: O(1)
// ------------------------------------------------------------
MU_INLINE uint32_t hash_find_slot(hash_t* h, uint64_t key)
{
    uint32_t i = hash_u64(key) % h->capacity;

    while(h->keys[i] != HASH_EMPTY && h->keys[i] != key)
    {
        i = (i + 1) % h->capacity;
    }

    return i;
}

// ------------------------------------------------------------
MU_INLINE void hash_put(hash_t* h, uint64_t key, uint64_t value)
{
    uint32_t i = hash_find_slot(h, key);

    if(h->keys[i] == HASH_EMPTY)
    {
        h->count++;
    }

    h->keys[i]   = key;
    h->values[i] = value;
}

// ------------------------------------------------------------
MU_INLINE uint64_t hash_get(hash_t* h, uint64_t key, uint64_t def)
{
    uint32_t i = hash_find_slot(h, key);

    if(h->keys[i] == HASH_EMPTY)
        return def;

    return h->values[i];
}



/*
    ┌──────────────────────────────────────────────┐
    │              mu_hash32_static                │
    └──────────────────────────────────────────────┘
// taken  from our  machinery blog
    A fixed-size hash table with:
    - open addressing (linear probing)
    - no allocations (you provide memory)
    - no resizing (your problem, not mine)
    - O(1) average lookup (until you mess up load factor)

    Ideal for:
    - resource lookup (texture_id, mesh_id)
    - entity → component index mapping
    - hot-path systems where malloc = sin

    Not ideal for:
    - dynamic growth
    - deletion-heavy workloads
    - people who don't understand capacity planning
*/


/*
    Sentinel value representing EMPTY slot

    ASCII:
        keys: [FFFF][FFFF][FFFF] → all unused

    Why 0xFF?
    Because memset can blast it fast.
*/
#define MU_HASH_UNUSED 0xffffffffffffffffULL


typedef struct mu_hash32_static_t
{
    uint64_t* keys;    // hashed keys
    uint32_t* values;  // associated values
    uint32_t  n;       // capacity (fixed)

    /*
        MEMORY MODEL:

        keys:   [k0][k1][k2][k3]...[kn]
        values: [v0][v1][v2][v3]...[vn]

    */

} mu_hash32_static_t;


/*
    Initialize with user-provided memory

    You allocate memory → we just hook into it
*/
MU_INLINE void mu_hash32_static_init(mu_hash32_static_t* h, uint64_t* keys, uint32_t* values, uint32_t n)
{
    h->keys   = keys;
    h->values = values;
    h->n      = n;

    /*
        Immediately clear → mark all as unused
    */
    MU_MEMSET(h->keys, 0xFF, sizeof(uint64_t) * n);
}


/*
    Clear table (reset all keys)

    ASCII before:
        [K1][K2][K3]

    after:
        [--][--][--]
*/
MU_INLINE void mu_hash32_static_clear(mu_hash32_static_t* h)
{
    MU_MEMSET(h->keys, 0xFF, sizeof(uint64_t) * h->n);
}


/*
    Insert or overwrite key → value

    Core idea:
        i = hash % n

        if occupied → probe forward

    ASCII collision chain:

        index:  0   1   2   3
                K1  K2  --  --

        insert K3:
            → 0 busy
            → 1 busy
            → 2 empty → place
*/
MU_INLINE void mu_hash32_static_set(mu_hash32_static_t* h, uint64_t key, uint32_t value)
{
    uint32_t i = key % h->n;

    /*
        Linear probing loop

        keep walking until:
        - same key (overwrite)
        - empty slot (insert)
    */
    while(h->keys[i] != key && h->keys[i] != MU_HASH_UNUSED)
    {
        i = (i + 1) % h->n;
    }

    h->keys[i]   = key;
    h->values[i] = value;
}


/*
    Lookup key → value

    Returns:
        0 if not found
        (so don't store 0 as a meaningful value unless you're into pain)
*/
MU_INLINE uint32_t mu_hash32_static_get(const mu_hash32_static_t* h, uint64_t key)
{
    uint32_t i = key % h->n;

    /*
        Probe until:
        - key found → return
        - empty slot → not found
    */
    while(h->keys[i] != key && h->keys[i] != MU_HASH_UNUSED)
    {
        i = (i + 1) % h->n;
    }

    return (h->keys[i] == MU_HASH_UNUSED) ? 0 : h->values[i];
}


/*
    ============================================================================
        mu_hash32.h
    ============================================================================

    Fixed-capacity owning hash table.
    Open addressing + linear probing.
    Heap-owned storage.
    No resizing.
    No allocator gymnastics.
    No "static" lie in the name.
    Just a small honest hash table for systems that need ownership.

    Good for:
        - asset caches
        - resource lookup
        - registries
        - path -> handle maps

    Not good for:
        - unbounded growth
        - deletion-heavy abuse
        - people who think "fixed capacity" is a suggestion

    ---------------------------------------------------------------------------
    Mental Model
    ---------------------------------------------------------------------------

        hash(key) % capacity
              |
              v
        ┌─────┬─────┬─────┬─────┬─────┐
        │  -- │ K17 │ K42 │  -- │  -- │
        └─────┴─────┴─────┴─────┴─────┘
                  ^
                  found slot

        collision?
            walk right until:
                - same key   -> overwrite
                - empty slot -> insert

    ---------------------------------------------------------------------------
    Memory Model
    ---------------------------------------------------------------------------

        mu_hash32_t
        ├── keys   -> [k0][k1][k2][k3]...[kn]
        ├── values -> [v0][v1][v2][v3]...[vn]
        ├── n      -> fixed capacity
        └── count  -> used slots

        Table owns keys/values.
        Caller owns table struct.
        Civilization barely holds.
*/

#include "mu_common.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define MU_HASH_UNUSED 0xffffffffffffffffULL

typedef struct mu_hash32_t
{
    uint64_t* keys;
    uint32_t* values;
    uint32_t  n;
    uint32_t  count;
} mu_hash32_t;

/* ============================================================================
   Internal Helpers
   ============================================================================ */

/*
    Simple 64-bit mix.
    Not crypto.
    Not magical.
    Just enough to stop your keys clustering like idiots.
*/
MU_INLINE uint64_t mu_hash64_mix(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/*
    Find slot for key.

    Returns:
        - matching key slot
        - or first empty slot

    ASCII:

        index = hash(key) % n

        [--][--][K1][K2][--]
                ^
              start

        if occupied:
            probe right until match or empty
*/
MU_INLINE uint32_t mu_hash32_find_slot(const mu_hash32_t* h, uint64_t key)
{
    uint32_t i = (uint32_t)(mu_hash64_mix(key) % h->n);

    while(h->keys[i] != key && h->keys[i] != MU_HASH_UNUSED)
        i = (i + 1) % h->n;

    return i;
}

/* ============================================================================
   Lifetime
   ============================================================================ */

/*
    Allocate and initialize table.

    Layout after init:

        keys   = [--][--][--][--]
        values = [??][??][??][??]
        count  = 0
*/
MU_INLINE bool mu_hash32_init(mu_hash32_t* h, uint32_t capacity)
{
    MU_ASSERT(h);
    MU_ASSERT(capacity > 0);

    h->keys   = (uint64_t*)MU_MALLOC(sizeof(uint64_t) * capacity);
    h->values = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * capacity);
    h->n      = capacity;
    h->count  = 0;

    if(!h->keys || !h->values)
    {
        MU_FREE(h->keys);
        MU_FREE(h->values);
        MU_MEMSET(h, 0, sizeof(*h));
        return false;
    }

    MU_MEMSET(h->keys, 0xFF, sizeof(uint64_t) * capacity);
    return true;
}

/*
    Destroy owned memory.

    Caller still owns the struct.
    We just gut it cleanly.
*/
MU_INLINE void mu_hash32_destroy(mu_hash32_t* h)
{
    MU_ASSERT(h);

    MU_FREE(h->keys);
    MU_FREE(h->values);

    MU_MEMSET(h, 0, sizeof(*h));
}

/*
    Clear table contents, keep capacity.

    Before:
        [K1][K2][K3][--]

    After:
        [--][--][--][--]
*/
MU_INLINE void mu_hash32_clear(mu_hash32_t* h)
{
    MU_ASSERT(h);
    MU_ASSERT(h->keys);

    MU_MEMSET(h->keys, 0xFF, sizeof(uint64_t) * h->n);
    h->count = 0;
}

/* ============================================================================
   Queries
   ============================================================================ */

MU_INLINE bool mu_hash32_empty(const mu_hash32_t* h)
{
    return h->count == 0;
}

MU_INLINE uint32_t mu_hash32_count(const mu_hash32_t* h)
{
    return h->count;
}

MU_INLINE uint32_t mu_hash32_capacity(const mu_hash32_t* h)
{
    return h->n;
}

/* ============================================================================
   Insert / Lookup
   ============================================================================ */

/*
    Insert or overwrite.

    If key exists:
        overwrite value

    If key is new:
        insert and increment count

    No resize.
    If full, that's your bug.
*/
MU_INLINE void mu_hash32_set(mu_hash32_t* h, uint64_t key, uint32_t value)
{
    MU_ASSERT(h);
    MU_ASSERT(key != MU_HASH_UNUSED);
    MU_ASSERT(h->count < h->n); /* full table = your capacity planning sucked */

    uint32_t i = mu_hash32_find_slot(h, key);

    if(h->keys[i] == MU_HASH_UNUSED)
        h->count++;

    h->keys[i]   = key;
    h->values[i] = value;
}

/*
    Lookup key.

    Returns:
        true  -> found, writes out_value
        false -> not found

    This avoids the "0 means maybe missing maybe valid" nonsense.
*/
MU_INLINE bool mu_hash32_get(const mu_hash32_t* h, uint64_t key, uint32_t* out_value)
{
    MU_ASSERT(h);
    MU_ASSERT(out_value);

    uint32_t i = mu_hash32_find_slot(h, key);

    if(h->keys[i] == MU_HASH_UNUSED)
        return false;

    *out_value = h->values[i];
    return true;
}

MU_INLINE bool mu_hash32_contains(const mu_hash32_t* h, uint64_t key)
{
    uint32_t ignored;
    return mu_hash32_get(h, key, &ignored);
}

/* ============================================================================
   Removal
   ============================================================================ */

/*
    Remove key using backshift repair.

    Why repair?
    Because linear probing creates clusters.

    If you just blank a slot:

        [K1][K2][K3][--]

    remove K1 naively:

        [--][K2][K3][--]

    lookup(K3):
        starts at K1 slot
        sees empty
        wrongly concludes "not found"

    Brilliant. Broken.

    So after deletion we reinsert the cluster tail.

    Before:
        [K1][K2][K3][--]

    remove K1:
        [--][K2][K3][--]

    repair:
        remove K2 -> reinsert
        remove K3 -> reinsert

    After:
        [K2][K3][--][--]
*/
MU_INLINE void mu_hash32_remove(mu_hash32_t* h, uint64_t key)
{
    MU_ASSERT(h);

    uint32_t i = mu_hash32_find_slot(h, key);

    if(h->keys[i] == MU_HASH_UNUSED)
        return; /* not found */

    h->keys[i] = MU_HASH_UNUSED;
    h->count--;

    uint32_t j = (i + 1) % h->n;

    while(h->keys[j] != MU_HASH_UNUSED)
    {
        uint64_t rekey = h->keys[j];
        uint32_t reval = h->values[j];

        h->keys[j] = MU_HASH_UNUSED;
        h->count--;

        mu_hash32_set(h, rekey, reval);

        j = (j + 1) % h->n;
    }
}


