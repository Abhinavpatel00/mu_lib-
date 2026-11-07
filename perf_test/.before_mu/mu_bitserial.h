#pragma once

#include "mu_common.h"

/* ---------------------------------------------------------------------------
 * mu_bitserial — bit-level serialization (writer / reader) + varints.
 *
 * WHY: replay/demo snapshots, network state, packed asset records. Writing a
 * field as "N bits" instead of a whole struct is the difference between a
 * 60-byte update and a 9-byte one, and delta-against-baseline (which this
 * header gives you directly) is what makes repeated frames cheap.
 *
 * BIT ORDER: little-endian within each byte — bit b of the stream lives at
 * byte (bit_pos >> 3), bit (bit_pos & 7). The reader/writer agree on this and
 * nothing else about the layout matters.
 *
 * HOT PATH: write_bits holds a value in registers and only touches memory per
 * byte. When the cursor is byte-aligned (the common case) whole bytes are
 * stored directly instead of going through the bit-merge path.
 *
 * CONTRACT:
 *   - The buffer must be zeroed (mu_bit_writer_init does it) so OR-ing new
 *     bits into a byte is safe.
 *   - bits in [1..64]. 0-bit writes are a no-op.
 *   - No allocation ever. Bounds failures return false (checked, not UB).
 * --------------------------------------------------------------------------- */

typedef struct mu_bit_writer
{
    uint8_t* data;
    uint64_t bit_pos;
    uint64_t capacity_bits;
} mu_bit_writer;

typedef struct mu_bit_reader
{
    const uint8_t* data;
    uint64_t       bit_pos;
    uint64_t       capacity_bits;
} mu_bit_reader;

MU_INLINE void mu_bit_writer_init(mu_bit_writer* w, void* buffer, uint64_t capacity_bytes)
{
    MU_ASSERT(w);
    w->data          = (uint8_t*)buffer;
    w->bit_pos       = 0;
    w->capacity_bits = capacity_bytes * 8ull;
    if (buffer && capacity_bytes > 0)
        MU_MEMSET(buffer, 0, (size_t)capacity_bytes);
}

MU_INLINE void mu_bit_reader_init(mu_bit_reader* r, const void* buffer, uint64_t capacity_bytes)
{
    MU_ASSERT(r);
    r->data          = (const uint8_t*)buffer;
    r->bit_pos       = 0;
    r->capacity_bits = capacity_bytes * 8ull;
}

MU_INLINE uint64_t mu_bit_writer_bits_used(const mu_bit_writer* w)
{
    return w->bit_pos;
}

/* Whole bytes that are complete — the size you hand to send(). */
MU_INLINE uint32_t mu_bit_writer_bytes_used(const mu_bit_writer* w)
{
    return (uint32_t)((w->bit_pos + 7ull) >> 3);
}

MU_INLINE void mu_bit_writer_align_byte(mu_bit_writer* w)
{
    w->bit_pos = (w->bit_pos + 7ull) & ~7ull;
}

MU_INLINE void mu_bit_reader_align_byte(mu_bit_reader* r)
{
    r->bit_pos = (r->bit_pos + 7ull) & ~7ull;
}

/* ------------------------------------------------------------------ *
 * Write
 * ------------------------------------------------------------------ */

/* Store the low `bits` of `value`, LSB-first in the stream. */
MU_INLINE bool mu_bit_write_bits(mu_bit_writer* w, uint64_t value, uint32_t bits)
{
    MU_ASSERT(w);
    if (bits == 0u)
        return true;
    if (bits > 64u || !w->data)
        return false;
    if (w->bit_pos + bits > w->capacity_bits)
        return false;

    uint64_t v = (bits == 64u) ? value : (value & (((uint64_t)1 << bits) - 1ull));
    uint64_t p = w->bit_pos;

    /* Byte-aligned fast path: store whole bytes straight through. */
    if ((p & 7ull) == 0ull)
    {
        while (bits >= 8u)
        {
            w->data[p >> 3] = (uint8_t)v;
            v >>= 8;
            p += 8;
            bits -= 8u;
        }
        if (bits == 0u)
        {
            w->bit_pos = p;
            return true;
        }
    }

    while (bits > 0u)
    {
        uint32_t byte      = (uint32_t)(p >> 3);
        uint32_t offset    = (uint32_t)(p & 7ull);
        uint32_t take      = 8u - offset;
        if (take > bits)
            take = bits;

        uint8_t mask = (uint8_t)(((uint8_t)1 << take) - 1u);
        w->data[byte] = (uint8_t)((w->data[byte] & (uint8_t)~(mask << offset)) | (uint8_t)((v & mask) << offset));

        v >>= take;
        p += take;
        bits -= take;
    }

    w->bit_pos = p;
    return true;
}

MU_INLINE bool mu_bit_write_bool(mu_bit_writer* w, bool value)
{
    return mu_bit_write_bits(w, value ? 1u : 0u, 1u);
}

MU_INLINE bool mu_bit_write_u32(mu_bit_writer* w, uint32_t value, uint32_t bits)
{
    return mu_bit_write_bits(w, value, bits);
}

/* Unsigned LEB128: 7 payload bits per byte, low byte first, MSB = continue.
   Good for small counters that are usually < 128 (1 byte instead of 4). */
MU_INLINE bool mu_bit_write_varint(mu_bit_writer* w, uint64_t value)
{
    while (value >= 0x80ull)
    {
        if (!mu_bit_write_bits(w, (value & 0x7full) | 0x80ull, 8u))
            return false;
        value >>= 7;
    }
    return mu_bit_write_bits(w, value, 8u);
}

/* Fixed-width delta: how far `value` moved from `prev`, biased to stay
   non-negative so small moves encode in few bits. `bits` must cover the
   full 2*range. */
MU_INLINE bool mu_bit_write_delta(mu_bit_writer* w, uint32_t prev, uint32_t value, uint32_t bits)
{
    if (bits == 0u || bits > 64u)
        return false;

    uint64_t delta;
    if (value >= prev)
        delta = (uint64_t)(value - prev);
    else
        delta = ((uint64_t)1 << (bits - 1u)) + (uint64_t)(prev - value);

    if (bits < 64u && delta >= ((uint64_t)1 << bits))
        return false; /* does not fit: caller under-estimated the range */

    return mu_bit_write_bits(w, delta, bits);
}

/* ------------------------------------------------------------------ *
 * Read
 * ------------------------------------------------------------------ */

MU_INLINE bool mu_bit_read_bits(mu_bit_reader* r, uint32_t bits, uint64_t* out)
{
    MU_ASSERT(r && out);
    if (bits == 0u)
    {
        *out = 0;
        return true;
    }
    if (bits > 64u || !r->data)
        return false;
    if (r->bit_pos + bits > r->capacity_bits)
        return false;

    uint64_t result = 0;
    uint64_t p      = r->bit_pos;
    uint32_t got    = 0;

    /* Byte-aligned fast path mirrors the writer. */
    if ((p & 7ull) == 0ull)
    {
        while (bits >= 8u)
        {
            result |= (uint64_t)r->data[p >> 3] << got;
            got += 8u;
            p += 8;
            bits -= 8u;
        }
        if (bits == 0u)
        {
            r->bit_pos = p;
            *out       = result;
            return true;
        }
    }

    while (bits > 0u)
    {
        uint32_t byte   = (uint32_t)(p >> 3);
        uint32_t offset = (uint32_t)(p & 7ull);
        uint32_t take   = 8u - offset;
        if (take > bits)
            take = bits;

        uint8_t mask = (uint8_t)(((uint8_t)1 << take) - 1u);
        result |= (uint64_t)((r->data[byte] >> offset) & mask) << got;

        got += take;
        p += take;
        bits -= take;
    }

    r->bit_pos = p;
    *out       = result;
    return true;
}

MU_INLINE bool mu_bit_read_bool(mu_bit_reader* r, bool* out)
{
    uint64_t v;
    if (!mu_bit_read_bits(r, 1u, &v))
        return false;
    *out = (v != 0);
    return true;
}

MU_INLINE bool mu_bit_read_varint(mu_bit_reader* r, uint64_t* out)
{
    MU_ASSERT(r && out);
    uint64_t value = 0;
    uint32_t shift = 0;

    for (;;)
    {
        uint64_t byte;
        if (!mu_bit_read_bits(r, 8u, &byte))
            return false;

        value |= (byte & 0x7full) << shift;
        if ((byte & 0x80ull) == 0ull)
            break;

        shift += 7u;
        if (shift >= 64u)
            return false;
    }

    *out = value;
    return true;
}

MU_INLINE bool mu_bit_read_delta(mu_bit_reader* r, uint32_t bits, uint32_t prev, uint32_t* out)
{
    uint64_t delta;
    if (bits == 0u || bits > 64u)
        return false;
    if (!mu_bit_read_bits(r, bits, &delta))
        return false;

    uint64_t bias = (uint64_t)1 << (bits - 1u);
    if (delta >= bias)
        *out = prev - (uint32_t)(delta - bias);
    else
        *out = prev + (uint32_t)delta;
    return true;
}
