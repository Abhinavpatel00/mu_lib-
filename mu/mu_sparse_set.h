
#pragma once
#include "mu_common.h"

/*
    mu_sparse_set
    =============
    Dense+sparse packed set for O(1) insert/remove/contains.

    This stores integer IDs in range [0, capacity).

    Why this exists:
    ----------------
    You want:
      - O(1) insert
      - O(1) remove
      - O(1) contains
      - O(n) tight iteration over active items

    You do NOT want:
      - hash table overhead
      - branchy garbage
      - cache-miss roulette
      - pretending linked lists are acceptable in 2026

    Layout:
    -------

        dense (packed active values)
        index:   0   1   2   3
               +---+---+---+---+
        dense: | 7 | 2 | 9 | 4 |
               +---+---+---+---+
                 ^   ^   ^   ^
                 active values

        sparse (value -> dense index)
        index:   0 1 2 3 4 5 6 7 8 9
               +---------------------+
        sparse:| x x 1 x 3 x x 0 x 2 |
               +---------------------+

        Example:
          sparse[7] = 0  -> value 7 is in dense[0]
          sparse[2] = 1  -> value 2 is in dense[1]
          sparse[9] = 2  -> value 9 is in dense[2]
          sparse[4] = 3  -> value 4 is in dense[3]

    Membership check:
    -----------------
        value exists iff:

            sparse[value] < count
        and dense[sparse[value]] == value

    That second check matters.
    Without it, stale sparse entries lie to you. Like most APIs.

    Remove:
    -------
        Remove is swap-delete:

            dense[idx] = dense[last]
            sparse[moved_value] = idx
            count--

        No shifting.
        No drama.
        O(1).

    Mental model:
    -------------
        sparse = "where is value X?"
        dense  = "what active values exist right now?"
*/

typedef struct
{
    uint32_t* dense;     /* packed active values      */
    uint32_t* sparse;    /* value -> dense index      */
    uint32_t  count;     /* active element count      */
    uint32_t  capacity;  /* max legal value + 1       */
} mu_sparse_set;


/* lifecycle */
void mu_sparse_set_init(mu_sparse_set* set, uint32_t capacity);
void mu_sparse_set_destroy(mu_sparse_set* set);

/* operations */
void mu_sparse_set_clear(mu_sparse_set* set);
bool mu_sparse_set_contains(const mu_sparse_set* set, uint32_t value);
bool mu_sparse_set_insert(mu_sparse_set* set, uint32_t value);
bool mu_sparse_set_remove(mu_sparse_set* set, uint32_t value);

/* iteration */
MU_INLINE uint32_t mu_sparse_set_at(const mu_sparse_set* set, uint32_t index)
{
    return set->dense[index];
}

