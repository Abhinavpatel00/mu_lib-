#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_intern — string interning: (bytes) -> dense stable uint32 id.
 *
 * WHY: this is the "IDs over pointers" rule from AGENT.md made concrete. Once
 * a string is interned, every later comparison, hash and switch becomes an
 * integer compare on a uint32; every SoA table stores ids instead of char*;
 * no string ever needs to be freed or kept alive by hand.
 *
 * MEMORY MODEL: one contiguous byte arena + two parallel uint32 tables
 * (offsets[], lengths[]) + one open-addressed id table.
 *
 *     table[] pow2, 0 = empty else id+1     <- hot: probes are u32 reads
 *     offsets[id] / lengths[id]             <- warm
 *     arena bytes                           <- cold, only touched on compare
 *
 * A lookup never allocates. Only the FIRST sighting of a new string touches
 * the arena and possibly grows a table.
 *
 * CONTRACT: ids are stable for the lifetime of the map (they never get
 * recycled, so stored ids never dangle). Not thread safe.
 * --------------------------------------------------------------------------- */

typedef struct mu_intern
{
    uint32_t* table;       /* pow2; 0 = empty, else id + 1                  */
    uint32_t  table_mask;  /* table_size - 1                                */
    uint32_t* offsets;     /* id -> arena offset                            */
    uint32_t* lengths;     /* id -> byte length (no NUL)                    */
    uint32_t  count;
    uint32_t  id_capacity;
    char*     arena;
    uint32_t  arena_used;
    uint32_t  arena_capacity;
    uint32_t  owned;
} mu_intern;

/* FNV-1a: 1 multiply + xor per byte, no table. */
MU_INLINE uint32_t mu_intern__hash(const char* s, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; ++i)
    {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

MU_INLINE bool mu_intern__equals(const mu_intern* m, uint32_t id, const char* s, uint32_t len)
{
    if (m->lengths[id] != len)
        return false;
    return MU_MEMCMP(m->arena + m->offsets[id], s, len) == 0;
}

MU_INLINE bool mu_intern_init(mu_intern* m, uint32_t arena_bytes, uint32_t table_slots)
{
    MU_ASSERT(m);
    m->table          = NULL;
    m->table_mask     = 0;
    m->offsets        = NULL;
    m->lengths        = NULL;
    m->count          = 0;
    m->id_capacity    = 0;
    m->arena          = NULL;
    m->arena_used     = 0;
    m->arena_capacity = arena_bytes;
    m->owned          = 1;

    /* Round table size up to a power of two so probing can mask instead of
       dividing (a modulo in the inner lookup loop is a real cost). */
    uint32_t slots = 64;
    while (slots < table_slots)
    {
        if (slots > 0x40000000u)
            return false;
        slots <<= 1;
    }

    m->table      = (uint32_t*)MU_MALLOC((size_t)slots * sizeof(uint32_t));
    m->offsets    = (uint32_t*)MU_MALLOC(256 * sizeof(uint32_t));
    m->lengths    = (uint32_t*)MU_MALLOC(256 * sizeof(uint32_t));
    m->arena      = (char*)MU_MALLOC(arena_bytes ? arena_bytes : 1u);
    if (!m->table || !m->offsets || !m->lengths || !m->arena)
    {
        MU_FREE(m->table);
        MU_FREE(m->offsets);
        MU_FREE(m->lengths);
        MU_FREE(m->arena);
        m->table       = NULL;
        m->offsets     = NULL;
        m->lengths     = NULL;
        m->arena       = NULL;
        m->owned       = 0;
        return false;
    }

    m->table_mask   = slots - 1u;
    m->id_capacity  = 256;
    MU_MEMSET(m->table, 0, (size_t)slots * sizeof(uint32_t));
    return true;
}

MU_INLINE void mu_intern_destroy(mu_intern* m)
{
    if (!m)
        return;
    if (m->owned)
    {
        MU_FREE(m->table);
        MU_FREE(m->offsets);
        MU_FREE(m->lengths);
        MU_FREE(m->arena);
    }
    m->table     = NULL;
    m->offsets   = NULL;
    m->lengths   = NULL;
    m->arena     = NULL;
    m->count     = 0;
    m->id_capacity = 0;
    m->arena_used  = 0;
    m->arena_capacity = 0;
    m->owned     = 0;
}

MU_INLINE uint32_t mu_intern_count(const mu_intern* m)
{
    return m ? m->count : 0u;
}

MU_INLINE const char* mu_intern_str(const mu_intern* m, uint32_t id)
{
    MU_ASSERT(m && id < m->count);
    return m->arena + m->offsets[id];
}

MU_INLINE uint32_t mu_intern_len(const mu_intern* m, uint32_t id)
{
    MU_ASSERT(m && id < m->count);
    return m->lengths[id];
}

/* ------------------------------------------------------------------ */

MU_INLINE bool mu_intern__grow_table(mu_intern* m)
{
    uint32_t old_slots = m->table_mask + 1u;
    uint32_t new_slots = old_slots << 1;
    if (new_slots < old_slots || new_slots == 0)
        return false;

    uint32_t* fresh = (uint32_t*)MU_MALLOC((size_t)new_slots * sizeof(uint32_t));
    if (!fresh)
        return false;
    MU_MEMSET(fresh, 0, (size_t)new_slots * sizeof(uint32_t));

    uint32_t mask = new_slots - 1u;
    for (uint32_t i = 0; i < old_slots; ++i)
    {
        uint32_t id_plus = m->table[i];
        if (id_plus == 0)
            continue;

        const char* s   = m->arena + m->offsets[id_plus - 1u];
        uint32_t    len = m->lengths[id_plus - 1u];
        uint32_t    at  = mu_intern__hash(s, len) & mask;
        while (fresh[at] != 0)
            at = (at + 1u) & mask;
        fresh[at] = id_plus;
    }

    MU_FREE(m->table);
    m->table      = fresh;
    m->table_mask = mask;
    return true;
}

MU_INLINE bool mu_intern__grow_ids(mu_intern* m)
{
    uint32_t cap = m->id_capacity * 2u;
    if (cap < m->id_capacity)
        return false;

    uint32_t* o = (uint32_t*)MU_REALLOC(m->offsets, (size_t)cap * sizeof(uint32_t));
    if (!o)
        return false;
    m->offsets = o;

    uint32_t* l = (uint32_t*)MU_REALLOC(m->lengths, (size_t)cap * sizeof(uint32_t));
    if (!l)
        return false;
    m->lengths     = l;
    m->id_capacity = cap;
    return true;
}

MU_INLINE bool mu_intern__grow_arena(mu_intern* m, uint32_t needed)
{
    uint32_t cap = m->arena_capacity ? m->arena_capacity : 256u;
    while (cap < needed)
    {
        if (cap > 0x40000000u)
            return false;
        cap <<= 1;
    }

    char* fresh = (char*)MU_REALLOC(m->arena, cap);
    if (!fresh)
        return false;
    m->arena          = fresh;
    m->arena_capacity = cap;
    return true;
}

/* Intern `len` bytes. Returns a stable id, or UINT32_MAX on allocation
   failure. `s` need not be NUL-terminated. */
MU_INLINE uint32_t mu_intern_get_n(mu_intern* m, const char* s, uint32_t len)
{
    MU_ASSERT(m);
    if (!s && len > 0)
        return MU_INVALID_INDEX;

    /* Keep the load factor under 0.7 before probing, so the chain stays short. */
    if (((uint64_t)(m->count + 1u) * 10ull) >= (uint64_t)(m->table_mask + 1u) * 7ull)
        if (!mu_intern__grow_table(m))
            return MU_INVALID_INDEX;

    uint32_t mask = m->table_mask;
    uint32_t at   = mu_intern__hash(s, len) & mask;
    while (m->table[at] != 0)
    {
        uint32_t id = m->table[at] - 1u;
        if (mu_intern__equals(m, id, s, len))
            return id;
        at = (at + 1u) & mask;
    }

    /* New string: make room in all three buffers, then commit. */
    if (m->count >= m->id_capacity && !mu_intern__grow_ids(m))
        return MU_INVALID_INDEX;

    uint32_t offset = m->arena_used;
    /* +1 for the NUL we append so mu_intern_str() works as a C string. */
    if ((uint64_t)offset + len + 1ull > m->arena_capacity && !mu_intern__grow_arena(m, offset + len + 1u))
        return MU_INVALID_INDEX;

    uint32_t id = m->count;
    m->offsets[id] = offset;
    m->lengths[id] = len;
    if (len > 0)
        MU_MEMCPY(m->arena + offset, s, len);
    m->arena[offset + len] = '\0'; /* only for the C-string convenience form */
    m->arena_used = offset + len + 1u;
    ++m->count;

    m->table[at] = id + 1u;
    return id;
}

MU_INLINE uint32_t mu_intern_get(mu_intern* m, const char* s)
{
    if (!s)
        return MU_INVALID_INDEX;
    return mu_intern_get_n(m, s, (uint32_t)strlen(s));
}

/* Find without interning: returns the id, or UINT32_MAX if never interned. */
MU_INLINE uint32_t mu_intern_find_n(const mu_intern* m, const char* s, uint32_t len)
{
    MU_ASSERT(m);
    if (!s && len > 0)
        return MU_INVALID_INDEX;

    uint32_t mask = m->table_mask;
    uint32_t at   = mu_intern__hash(s, len) & mask;
    while (m->table[at] != 0)
    {
        uint32_t id = m->table[at] - 1u;
        if (mu_intern__equals(m, id, s, len))
            return id;
        at = (at + 1u) & mask;
    }
    return MU_INVALID_INDEX;
}
