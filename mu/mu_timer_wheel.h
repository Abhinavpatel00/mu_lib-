#pragma once
#ifndef MU_TIMER_WHEEL_H
#define MU_TIMER_WHEEL_H

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * mu_timer_wheel — O(1) 4-Level x 64-Slot Hierarchical Bitmasked Timer Wheel.
 *
 * WHY (Varghese & Lauck 1987; Linux Kernel / Juho Snellman):
 *   Managing 100K+ cooldowns, network retransmit timers, AI state timeouts, or
 *   coro sleeps in a binary heap costs O(log N) cache misses on EVERY start AND
 *   cancel (99% of network/cooldown timers are cancelled before firing!).
 *
 *   This wheel gives:
 *     - O(1) `schedule` (no heap sift, zero malloc — flat pooled index handles!)
 *     - O(1) `cancel` (intrusive index doubly-linked list unlink!)
 *     - O(1) idle-tick skip (`uint64_t slot_mask[4]` + `ctz64` jumps straight to
 *       the next occupied bucket when advancing large tick deltas!)
 *     - Horizon: 64^4 = 16,777,216 ticks (~3.2 days at 60 Hz, ~4.6 hours at 1 kHz;
 *       longer delays safely clamp into the top wheel and cascade down).
 * --------------------------------------------------------------------------- */

#define MU_TW_LEVELS   4u
#define MU_TW_BITS     6u
#define MU_TW_SLOTS    64u
#define MU_TW_MASK     63u
#define MU_TW_INVALID  UINT32_MAX

typedef void (*mu_timer_cb)(uint32_t timer_id, uint64_t user_data, void* ctx);

typedef struct mu_timer_node
{
    uint64_t expire_tick;
    uint64_t user_data;
    uint32_t next;        /* next node index in bucket / freelist, or MU_TW_INVALID */
    uint32_t prev;        /* prev node index in bucket, or MU_TW_INVALID            */
    uint16_t bucket_idx;  /* 0..255 (`level * 64 + slot`), or 0xFFFF if inactive    */
    uint16_t active;
} mu_timer_node;

typedef struct mu_timer_wheel
{
    uint64_t       now_tick;
    uint64_t       slot_mask[MU_TW_LEVELS];             /* 1 bit per non-empty slot! */
    uint32_t       buckets[MU_TW_LEVELS * MU_TW_SLOTS]; /* head index per slot       */
    mu_timer_node* nodes;
    uint32_t       capacity;
    uint32_t       free_head;
    uint32_t       active_count;
    uint32_t       owned;
} mu_timer_wheel;

MU_INLINE bool mu_timer_wheel_init_static(
    mu_timer_wheel* tw,
    mu_timer_node*  nodes_buf,
    uint32_t        capacity,
    uint64_t        start_tick)
{
    MU_ASSERT(tw);
    MU_MEMSET(tw, 0, sizeof(*tw));
    if (!nodes_buf || capacity == 0u)
        return false;

    tw->now_tick = start_tick;
    for (uint32_t i = 0; i < MU_TW_LEVELS * MU_TW_SLOTS; ++i)
        tw->buckets[i] = MU_TW_INVALID;

    for (uint32_t i = 0; i < capacity; ++i)
    {
        nodes_buf[i].expire_tick = 0ull;
        nodes_buf[i].user_data   = 0ull;
        nodes_buf[i].next        = (i + 1u < capacity) ? (i + 1u) : MU_TW_INVALID;
        nodes_buf[i].prev        = MU_TW_INVALID;
        nodes_buf[i].bucket_idx  = 0xFFFFu;
        nodes_buf[i].active      = 0u;
    }

    tw->nodes        = nodes_buf;
    tw->capacity     = capacity;
    tw->free_head    = 0u;
    tw->active_count = 0u;
    tw->owned        = 0u;
    return true;
}

MU_INLINE bool mu_timer_wheel_init(mu_timer_wheel* tw, uint32_t max_timers, uint64_t start_tick)
{
    MU_ASSERT(tw);
    if (max_timers == 0u)
        max_timers = 64u;

    mu_timer_node* buf = (mu_timer_node*)MU_MALLOC((size_t)max_timers * sizeof(mu_timer_node));
    if (!buf)
    {
        MU_MEMSET(tw, 0, sizeof(*tw));
        return false;
    }
    mu_timer_wheel_init_static(tw, buf, max_timers, start_tick);
    tw->owned = 1u;
    return true;
}

MU_INLINE void mu_timer_wheel_destroy(mu_timer_wheel* tw)
{
    if (!tw)
        return;
    if (tw->owned)
        MU_FREE(tw->nodes);
    MU_MEMSET(tw, 0, sizeof(*tw));
}

MU_INLINE void mu_timer_wheel__link(mu_timer_wheel* tw, uint32_t id, uint64_t exp)
{
    uint64_t now   = tw->now_tick;
    uint64_t delta = (exp > now) ? (exp - now) : 0ull;

    uint32_t level;
    uint32_t slot;
    if (delta < 64ull)
    {
        level = 0u;
        slot  = (uint32_t)(exp & MU_TW_MASK);
    }
    else if (delta < (1ull << 12))
    {
        level = 1u;
        slot  = (uint32_t)((exp >> 6) & MU_TW_MASK);
    }
    else if (delta < (1ull << 18))
    {
        level = 2u;
        slot  = (uint32_t)((exp >> 12) & MU_TW_MASK);
    }
    else
    {
        level = 3u;
        /* Clamp ultra-long timers (> 64^4 - 1) to farthest slot of level 3;
           when cascaded, `expire_tick > now_tick` re-inserts them until due! */
        uint64_t clamped = (delta >= (1ull << 24)) ? (now + (1ull << 24) - 1ull) : exp;
        slot             = (uint32_t)((clamped >> 18) & MU_TW_MASK);
    }

    uint32_t       bidx = level * MU_TW_SLOTS + slot;
    uint32_t       head = tw->buckets[bidx];
    mu_timer_node* nd   = &tw->nodes[id];

    nd->prev        = MU_TW_INVALID;
    nd->next        = head;
    nd->bucket_idx  = (uint16_t)bidx;
    if (head != MU_TW_INVALID)
        tw->nodes[head].prev = id;

    tw->buckets[bidx]     = id;
    tw->slot_mask[level] |= (UINT64_C(1) << slot);
}

MU_INLINE void mu_timer_wheel__unlink(mu_timer_wheel* tw, uint32_t id)
{
    mu_timer_node* nd   = &tw->nodes[id];
    uint32_t       bidx = nd->bucket_idx;
    MU_ASSERT(bidx < MU_TW_LEVELS * MU_TW_SLOTS);

    uint32_t prev = nd->prev;
    uint32_t next = nd->next;

    if (prev != MU_TW_INVALID)
        tw->nodes[prev].next = next;
    else
        tw->buckets[bidx] = next;

    if (next != MU_TW_INVALID)
        tw->nodes[next].prev = prev;

    if (tw->buckets[bidx] == MU_TW_INVALID)
    {
        uint32_t level = bidx >> 6;
        uint32_t slot  = bidx & MU_TW_MASK;
        tw->slot_mask[level] &= ~(UINT64_C(1) << slot);
    }

    nd->prev       = MU_TW_INVALID;
    nd->next       = MU_TW_INVALID;
    nd->bucket_idx = 0xFFFFu;
}

/* O(1) schedule: returns timer handle (`MU_TW_INVALID` if pool exhausted). */
MU_INLINE uint32_t mu_timer_schedule(mu_timer_wheel* tw, uint64_t delay_ticks, uint64_t user_data)
{
    MU_ASSERT(tw);
    if (tw->free_head == MU_TW_INVALID)
        return MU_TW_INVALID;

    uint32_t       id = tw->free_head;
    mu_timer_node* nd = &tw->nodes[id];
    tw->free_head     = nd->next;

    uint64_t exp   = tw->now_tick + (delay_ticks == 0ull ? 1ull : delay_ticks);
    nd->expire_tick = exp;
    nd->user_data   = user_data;
    nd->active      = 1u;
    ++tw->active_count;

    mu_timer_wheel__link(tw, id, exp);
    return id;
}

/* O(1) cancel: returns true if timer was active and cancelled. */
MU_INLINE bool mu_timer_cancel(mu_timer_wheel* tw, uint32_t timer_id)
{
    if (!tw || timer_id >= tw->capacity)
        return false;
    mu_timer_node* nd = &tw->nodes[timer_id];
    if (!nd->active)
        return false;

    mu_timer_wheel__unlink(tw, timer_id);
    nd->active    = 0u;
    nd->next      = tw->free_head;
    tw->free_head = timer_id;
    --tw->active_count;
    return true;
}

MU_INLINE void mu_timer_wheel__cascade(mu_timer_wheel* tw, uint32_t level, uint32_t slot)
{
    uint32_t bidx = level * MU_TW_SLOTS + slot;
    uint32_t head = tw->buckets[bidx];
    if (head == MU_TW_INVALID)
        return;

    tw->buckets[bidx]     = MU_TW_INVALID;
    tw->slot_mask[level] &= ~(UINT64_C(1) << slot);

    while (head != MU_TW_INVALID)
    {
        uint32_t       id   = head;
        mu_timer_node* nd   = &tw->nodes[id];
        head                = nd->next;
        nd->bucket_idx      = 0xFFFFu;
        mu_timer_wheel__link(tw, id, nd->expire_tick);
    }
}

/* Advance wheel by 1 tick, firing all due callbacks. Returns number fired. */
MU_INLINE uint32_t mu_timer_wheel_step(mu_timer_wheel* tw, mu_timer_cb cb, void* ctx)
{
    MU_ASSERT(tw);
    uint64_t t    = ++tw->now_tick;
    uint32_t s0   = (uint32_t)(t & MU_TW_MASK);

    if (s0 == 0u)
    {
        uint32_t s1 = (uint32_t)((t >> 6) & MU_TW_MASK);
        mu_timer_wheel__cascade(tw, 1u, s1);
        if (s1 == 0u)
        {
            uint32_t s2 = (uint32_t)((t >> 12) & MU_TW_MASK);
            mu_timer_wheel__cascade(tw, 2u, s2);
            if (s2 == 0u)
            {
                uint32_t s3 = (uint32_t)((t >> 18) & MU_TW_MASK);
                mu_timer_wheel__cascade(tw, 3u, s3);
            }
        }
    }

    uint32_t head = tw->buckets[s0];
    if (head == MU_TW_INVALID)
        return 0u;

    tw->buckets[s0]    = MU_TW_INVALID;
    tw->slot_mask[0]  &= ~(UINT64_C(1) << s0);

    uint32_t fired = 0u;
    while (head != MU_TW_INVALID)
    {
        uint32_t       id = head;
        mu_timer_node* nd = &tw->nodes[id];
        head              = nd->next;

        if (MU_UNLIKELY(nd->expire_tick > t))
        {
            /* Clamped ultra-long timer not yet due — re-link! */
            mu_timer_wheel__link(tw, id, nd->expire_tick);
            continue;
        }

        uint64_t udata = nd->user_data;
        nd->active     = 0u;
        nd->bucket_idx = 0xFFFFu;
        nd->prev       = MU_TW_INVALID;
        nd->next       = tw->free_head;
        tw->free_head  = id;
        --tw->active_count;
        ++fired;

        if (cb)
            cb(id, udata, ctx);
    }
    return fired;
}

/* Advance up to `target_tick`, using `slot_mask[0]` CTZ to jump empty level-0 runs! */
MU_INLINE uint32_t mu_timer_wheel_advance_to(mu_timer_wheel* tw, uint64_t target_tick, mu_timer_cb cb, void* ctx)
{
    MU_ASSERT(tw);
    uint32_t total_fired = 0u;

    while (tw->now_tick < target_tick && tw->active_count > 0u)
    {
        uint64_t next_t = tw->now_tick + 1ull;
        uint32_t s0     = (uint32_t)(next_t & MU_TW_MASK);

        /* Fast-skip empty level-0 ticks before next 64-tick cascade boundary! */
        if (s0 != 0u)
        {
            uint64_t shifted = tw->slot_mask[0] >> s0;
            if (shifted == 0ull)
            {
                /* Rest of current 64-tick epoch in level 0 is completely empty! */
                uint64_t jump_to = (next_t | MU_TW_MASK); /* stops right before s0==0 cascade */
                if (jump_to > target_tick)
                    jump_to = target_tick;
                tw->now_tick = jump_to;
                continue;
            }
            else
            {
                uint32_t skip = (uint32_t)mu_trailing_zeroes_u64(shifted);
                if (skip > 0u)
                {
                    uint64_t jump_to = tw->now_tick + (uint64_t)skip;
                    if (jump_to > target_tick)
                        jump_to = target_tick;
                    tw->now_tick = jump_to;
                    if (tw->now_tick == target_tick)
                        break;
                }
            }
        }
        total_fired += mu_timer_wheel_step(tw, cb, ctx);
    }

    if (tw->now_tick < target_tick)
        tw->now_tick = target_tick;
    return total_fired;
}

MU_END_EXTERN_C

#endif /* MU_TIMER_WHEEL_H */
