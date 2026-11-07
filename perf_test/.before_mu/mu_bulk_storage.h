#pragma once

#include "mu_common.h"
// -----------------------------------------------------------------------------
// Data Structures Part 1: Bulk data with holes + weak handles
//
// Slot 0 is a freelist header. Free slots are linked through their slot memory
// (first u32 in slot storage). Generations support weak-handle validation.
// -----------------------------------------------------------------------------
//
// typedef struct mu_weak_handle
// {
//     uint32_t id;
//     uint32_t generation;
// } mu_weak_handle;
//
// typedef struct mu_bulk_storage
// {
//     uint8_t*  slots;
//     uint32_t* generations;
//     uint8_t*  live;
//     size_t    slot_size;
//     uint32_t  slot_capacity;
//     uint32_t  live_count;
//     uint32_t  next_unused;
// } mu_bulk_storage;
//
// typedef bool (*mu_bulk_storage_visit_fn)(uint32_t id, void* slot, void* user);
//
// bool         mu_bulk_storage_init(mu_bulk_storage* storage, size_t slot_size, uint32_t initial_slot_capacity);
// void         mu_bulk_storage_deinit(mu_bulk_storage* storage);
// uint32_t     mu_bulk_storage_alloc(mu_bulk_storage* storage);
// bool         mu_bulk_storage_free(mu_bulk_storage* storage, uint32_t id);
// void*        mu_bulk_storage_ptr(mu_bulk_storage* storage, uint32_t id);
// const void*  mu_bulk_storage_ptr_const(const mu_bulk_storage* storage, uint32_t id);
// bool         mu_bulk_storage_is_live(const mu_bulk_storage* storage, uint32_t id);
// mu_weak_handle mu_bulk_storage_make_handle(const mu_bulk_storage* storage, uint32_t id);
// bool         mu_bulk_storage_validate_handle(const mu_bulk_storage* storage, mu_weak_handle handle);
// void*        mu_bulk_storage_resolve_handle(mu_bulk_storage* storage, mu_weak_handle handle);
// const void*  mu_bulk_storage_resolve_handle_const(const mu_bulk_storage* storage, mu_weak_handle handle);
// void         mu_bulk_storage_visit_live(mu_bulk_storage* storage, mu_bulk_storage_visit_fn visitor, void* user);
//
//


/*
===============================================================================
mu_bulk_storage.h
===============================================================================

A sparse bulk-storage container with stable IDs and weak-handle validation.

This is your "store a lot of objects, delete them freely, never invalidate IDs
silently" container.

Think of it as:

    "array storage with holes, plus generation-checked handles"

It gives you:

- stable numeric IDs
- O(1) alloc
- O(1) free
- O(1) direct access
- weak handles that detect stale references
- iteration over only live slots

It does NOT give you:

- dense iteration
- cache-perfect SoA layout
- stable pointers across realloc
- magical protection from bad ownership decisions

This is for object lifetime management.
Not for SIMD worship.
Not for hot inner loops.
Not for pretending holes are free.

Good for:
- assets
- entities
- instances
- long-lived runtime objects
- "give me stable identity, not dense packing"

Bad for:
- ultra-hot contiguous iteration
- ECS archetype crunching
- places where holes become performance tax

------------------------------------------------------------------------------
Mental Model
------------------------------------------------------------------------------

You are managing a big slot array.

Some slots are alive.
Some are dead.
Dead slots are recycled through a freelist.
Each slot has a generation counter.
Weak handles store (id + generation).

If slot dies and gets reused:
    same id
    newer generation
    old handle becomes invalid

That is the whole trick.
Simple. Brutal. Effective.

------------------------------------------------------------------------------
High-Level Layout
------------------------------------------------------------------------------

    mu_bulk_storage
    ├── slots         raw object memory
    ├── generations   generation per slot
    ├── live          alive bit per slot
    ├── slot_size     bytes per slot
    ├── slot_capacity total slots allocated
    ├── live_count    active object count
    └── next_unused   next never-used slot id

Slot 0 is reserved.
Slot 0 is not a real object.
Slot 0 is freelist metadata.

Because naturally we hide bookkeeping in fake objects. Efficient and mildly rude.

------------------------------------------------------------------------------
Memory Layout
------------------------------------------------------------------------------

    slots:
        [slot0][slot1][slot2][slot3][slot4]...

    generations:
        [  x  ][  1  ][  4  ][  2  ][  9 ]...

    live:
        [  0  ][  1  ][  0  ][  1  ][  1 ]...

Where:

    slot0 = freelist head storage
    slotN = actual object bytes

ASCII:

          slots memory
    ┌──────┬──────┬──────┬──────┬──────┐
    │ hdr  │ obj1 │ obj2 │ obj3 │ obj4 │
    └──────┴──────┴──────┴──────┴──────┘
       ^
       slot 0 = freelist head

------------------------------------------------------------------------------
Weak Handle Model
------------------------------------------------------------------------------

A weak handle is:

    { id, generation }

Example:

    handle = { id = 42, generation = 7 }

To resolve:
    - id must be in range
    - slot must be live
    - generation must match

If object 42 dies and slot 42 is reused:

    old handle: {42, 7}
    new object: {42, 8}

Old handle becomes invalid automatically.

That is the entire point.

ASCII:

    old object
        id=42 gen=7
          ↓ freed

    new object reuses slot
        id=42 gen=8

    stale handle {42,7} != live slot {42,8}
        → rejected

No dangling object identity.
Just stale handles dying cleanly, like they should.

------------------------------------------------------------------------------
Freelist Model
------------------------------------------------------------------------------

Freed slots are recycled through an intrusive freelist.

The freelist head is stored in slot 0.
The "next free" pointer of each dead slot is stored in that slot's first u32.

So dead slot memory is temporarily reused as freelist linkage.

Because dead objects don't need dignity.

ASCII:

    free list:

        slot0(header) -> 7 -> 3 -> 12 -> null

Meaning:
    next allocation reuses slot 7
    then slot 3
    then slot 12

No extra allocation.
No side structure.
Just mildly aggressive reuse.

------------------------------------------------------------------------------
Allocation Path
------------------------------------------------------------------------------

mu_bulk_storage_alloc()

1. Check freelist head
2. If free slot exists:
       pop from freelist
3. Else:
       use next_unused
4. Mark slot live
5. Ensure generation != 0
6. Return id

ASCII:

    alloc()
       |
       +--> freelist not empty? ---- yes --> reuse dead slot
       |
       +--> no --> use next fresh slot

Result:
    O(1)

------------------------------------------------------------------------------
Free Path
------------------------------------------------------------------------------

mu_bulk_storage_free(id)

1. Validate id
2. Mark dead
3. Increment generation
4. Push slot into freelist
5. Reuse later

ASCII:

    free(7)
      ↓
    live[7] = 0
    gen[7]++
    slot7.next = freelist_head
    freelist_head = 7

Slot is now recyclable.
Old handles now fail validation.
Exactly what you want.

------------------------------------------------------------------------------
Growth Model
------------------------------------------------------------------------------

Storage grows by realloc.

When full:
    capacity *= 2

Then:
- slots grows
- generations grows
- live grows

New memory is zeroed.

Important:
Pointers into slot memory are NOT stable across growth.

IDs remain stable.
Handles remain stable.
Raw pointers do not.

If you cache slot pointers across growth, that's on you. Spectacularly bad idea.

------------------------------------------------------------------------------
Performance Model
------------------------------------------------------------------------------

Alloc:
    O(1)

Free:
    O(1)

Lookup by id:
    O(1)

Lookup by weak handle:
    O(1)

Iterate live:
    O(n) scan with hole skips

This is sparse-stable storage.
Not dense-fast storage.

Tradeoff:

    stable identity  >  perfect iteration locality

Use accordingly.

------------------------------------------------------------------------------
API Reference
===============================================================================
*/

#include "mu_common.h"

typedef struct mu_weak_handle
{
    /*
        Stable external reference.

        id         = slot index
        generation = stale-handle guard
    */
    uint32_t id;
    uint32_t generation;
} mu_weak_handle;

typedef struct mu_bulk_storage
{
    /*
        Raw slot memory.

        slot 0 is reserved for freelist header.
        slot N is object storage.
    */
    uint8_t* slots;

    /*
        Generation per slot.

        Incremented on free.
        Used to invalidate stale weak handles.
    */
    uint32_t* generations;

    /*
        Live bit per slot.

        0 = dead
        1 = alive
    */
    uint8_t* live;

    /*
        Size of one slot in bytes.
    */
    size_t slot_size;

    /*
        Total allocated slots, including slot 0.
    */
    uint32_t slot_capacity;

    /*
        Number of currently live objects.
    */
    uint32_t live_count;

    /*
        First never-used slot id.
    */
    uint32_t next_unused;
} mu_bulk_storage;

/*
    Visitor callback for iterating live slots.

    Return false to stop iteration early.
*/
typedef bool (*mu_bulk_storage_visit_fn)(uint32_t id, void* slot, void* user);

/*
===============================================================================
Lifetime
===============================================================================
*/

/*
    Initialize storage.

    - allocates slot memory
    - allocates generation array
    - allocates live array
    - reserves slot 0 for freelist header

    initial_slot_capacity includes slot 0.
    Minimum actual capacity is forced to 2.
*/
bool mu_bulk_storage_init(mu_bulk_storage* storage, size_t slot_size, uint32_t initial_slot_capacity);

/*
    Destroy storage and free all owned memory.

    Does not run destructors for slot contents.
    You own object teardown.
    This container owns bytes, not semantics.
*/
void mu_bulk_storage_deinit(mu_bulk_storage* storage);

/*
===============================================================================
Allocation / Free
===============================================================================
*/

/*
    Allocate one slot.

    Returns:
        0 on failure
        valid slot id on success

    Slot is marked live.
    Reuses freed slots first.
*/
uint32_t mu_bulk_storage_alloc(mu_bulk_storage* storage);

/*
    Free one live slot.

    - marks dead
    - increments generation
    - pushes into freelist

    Returns false if id invalid or already dead.
*/
bool mu_bulk_storage_free(mu_bulk_storage* storage, uint32_t id);

/*
===============================================================================
Direct Access
===============================================================================
*/

/*
    Resolve live slot by raw id.

    Returns NULL if:
        - id invalid
        - slot dead
*/
void* mu_bulk_storage_ptr(mu_bulk_storage* storage, uint32_t id);

/*
    Const version of mu_bulk_storage_ptr.
*/
const void* mu_bulk_storage_ptr_const(const mu_bulk_storage* storage, uint32_t id);

/*
    Check whether raw id is currently live.
*/
bool mu_bulk_storage_is_live(const mu_bulk_storage* storage, uint32_t id);

/*
===============================================================================
Weak Handles
===============================================================================
*/

/*
    Create weak handle from live/raw id.

    If id invalid, generation becomes 0.
*/
mu_weak_handle mu_bulk_storage_make_handle(const mu_bulk_storage* storage, uint32_t id);

/*
    Validate weak handle.

    Returns true only if:
        - id in range
        - slot live
        - generation matches
*/
bool mu_bulk_storage_validate_handle(const mu_bulk_storage* storage, mu_weak_handle handle);

/*
    Resolve weak handle to mutable slot pointer.

    Returns NULL if handle stale or invalid.
*/
void* mu_bulk_storage_resolve_handle(mu_bulk_storage* storage, mu_weak_handle handle);

/*
    Const version of mu_bulk_storage_resolve_handle.
*/
const void* mu_bulk_storage_resolve_handle_const(const mu_bulk_storage* storage, mu_weak_handle handle);

/*
===============================================================================
Iteration
===============================================================================
*/

/*
    Visit all live slots.

    Iterates [1 .. next_unused)
    Skips dead slots.
    Stops early if visitor returns false.

    Note:
        sparse scan
        not dense iteration
        choose this knowingly
*/
void mu_bulk_storage_visit_live(mu_bulk_storage* storage, mu_bulk_storage_visit_fn visitor, void* user);
