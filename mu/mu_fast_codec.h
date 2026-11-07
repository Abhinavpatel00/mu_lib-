#pragma once
#ifndef MU_FAST_CODEC_H
#define MU_FAST_CODEC_H

#include "mu_common.h"

#if defined(__AES__) || defined(__SSE4_2__) || defined(__SSSE3__) || defined(__BMI2__)
#  include <immintrin.h>
#endif

MU_BEGIN_EXTERN_C

/* ---------------------------------------------------------------------------
 * mu_fast_codec — Hardware-Accelerated Hashing, FastMod, Morton & StreamVByte.
 *
 * INCLUDES:
 *   1. `mu_wyhash64` & `mu_hash_bytes_fast`:
 *      - `mu_wyhash64`: Deterministic cross-ISA 128-bit `mum` hash (~16 GB/s).
 *      - `mu_hash_bytes_fast`: Session-local ultra-fast hash using hardware
 *        4-lane AES-NI (~54 GB/s) or 3-lane CRC32C (~23 GB/s), falling back to
 *        `mu_wyhash64` on baseline C99!
 *   2. `mu_fastdiv_u32` / `mu_fastmod_u32` / `mu_fastrange32` / `mu_fastrange64`:
 *      Daniel Lemire division-free exact remainder & range mapping (3x–11.5x).
 *   3. `mu_morton2d_*` & `mu_morton3d_*`:
 *      BMI2 `_pdep_u32`/`_pext_u32` + magic-bits C99 fallback + Fabian Giesen
 *      dilated-integer `+1` neighbor stepping!
 *   4. `mu_streamvbyte_encode_u32` / `mu_streamvbyte_decode_u32`:
 *      Daniel Lemire StreamVByte SIMD `pshufb` integer compression (~5.6B ints/s
 *      on SSSE3/AVX2, bit-identical C99 scalar fallback!).
 * --------------------------------------------------------------------------- */

/* ========================================================================== *
 * 1. Deterministic `wyhash64` + Hardware-Accelerated `mu_hash_bytes_fast`
 * ========================================================================== */

MU_INLINE uint64_t mu_wyhash__mum(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
    uint64_t al = (uint32_t)a, ah = a >> 32;
    uint64_t bl = (uint32_t)b, bh = b >> 32;
    uint64_t p0 = al * bl, p1 = al * bh, p2 = ah * bl, p3 = ah * bh;
    uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
    uint64_t hi  = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    uint64_t lo  = (mid << 32) | (uint32_t)p0;
    return lo ^ hi;
#endif
}

MU_INLINE uint64_t mu_wyhash__r8(const uint8_t* p)
{
    uint64_t v;
    MU_MEMCPY(&v, p, 8);
    return v;
}

MU_INLINE uint64_t mu_wyhash__r4(const uint8_t* p)
{
    uint32_t v;
    MU_MEMCPY(&v, p, 4);
    return (uint64_t)v;
}

/* Cross-platform deterministic 64-bit bulk hash (Wang Yi wyhash v4 spirit). */
MU_INLINE uint64_t mu_wyhash64(const void* data, size_t len, uint64_t seed)
{
    const uint8_t* p  = (const uint8_t*)data;
    const uint64_t s0 = UINT64_C(0x2d358dccaa6c78a5);
    const uint64_t s1 = UINT64_C(0x8bb84b93962eacc9);
    const uint64_t s2 = UINT64_C(0x4b33a62ed433d4a3);
    const uint64_t s3 = UINT64_C(0x4d5a2da51de1aa47);

    seed ^= mu_wyhash__mum(seed ^ s0, s1);
    uint64_t a = 0, b = 0;

    if (MU_LIKELY(len <= 16u))
    {
        if (len >= 4u)
        {
            a = (mu_wyhash__r4(p) << 32) | mu_wyhash__r4(p + ((len >> 3) << 2));
            b = (mu_wyhash__r4(p + len - 4u) << 32) | mu_wyhash__r4(p + len - 4u - ((len >> 3) << 2));
        }
        else if (len > 0u)
        {
            a = ((uint64_t)p[0] << 16) | ((uint64_t)p[len >> 1] << 8) | (uint64_t)p[len - 1u];
        }
    }
    else
    {
        size_t i = len;
        if (i > 48u)
        {
            uint64_t see1 = seed, see2 = seed;
            do
            {
                seed = mu_wyhash__mum(mu_wyhash__r8(p) ^ s1, mu_wyhash__r8(p + 8u) ^ seed);
                see1 = mu_wyhash__mum(mu_wyhash__r8(p + 16u) ^ s2, mu_wyhash__r8(p + 24u) ^ see1);
                see2 = mu_wyhash__mum(mu_wyhash__r8(p + 32u) ^ s3, mu_wyhash__r8(p + 40u) ^ see2);
                p += 48u;
                i -= 48u;
            } while (i > 48u);
            seed ^= see1 ^ see2;
        }
        while (i > 16u)
        {
            seed = mu_wyhash__mum(mu_wyhash__r8(p) ^ s1, mu_wyhash__r8(p + 8u) ^ seed);
            p += 16u;
            i -= 16u;
        }
        a = mu_wyhash__r8(p + i - 16u);
        b = mu_wyhash__r8(p + i - 8u);
    }
    a ^= s1;
    b ^= seed;
    return mu_wyhash__mum(s0 ^ (uint64_t)len, mu_wyhash__mum(a, b));
}

/* Fastest available in-memory hash for hash tables / dedup on the host CPU. */
MU_INLINE uint64_t mu_hash_bytes_fast(const void* data, size_t len, uint64_t seed)
{
#if defined(__AES__) && defined(__SSE2__)
    if (len >= 64u)
    {
        const uint8_t* p = (const uint8_t*)data;
        __m128i k0 = _mm_set_epi64x((long long)(seed ^ UINT64_C(0x9e3779b97f4a7c15)), (long long)seed);
        __m128i a0 = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(p +  0)), k0);
        __m128i a1 = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(p + 16)), _mm_shuffle_epi32(k0, 0x4E));
        __m128i a2 = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(p + 32)), _mm_shuffle_epi32(k0, 0xB1));
        __m128i a3 = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(p + 48)), _mm_shuffle_epi32(k0, 0x1B));
        p   += 64u;
        size_t rem = len - 64u;
        while (rem >= 64u)
        {
            a0 = _mm_aesenc_si128(a0, _mm_loadu_si128((const __m128i*)(p +  0)));
            a1 = _mm_aesenc_si128(a1, _mm_loadu_si128((const __m128i*)(p + 16)));
            a2 = _mm_aesenc_si128(a2, _mm_loadu_si128((const __m128i*)(p + 32)));
            a3 = _mm_aesenc_si128(a3, _mm_loadu_si128((const __m128i*)(p + 48)));
            p   += 64u;
            rem -= 64u;
        }
        __m128i m = _mm_aesenc_si128(_mm_aesenc_si128(a0, a1), _mm_aesenc_si128(a2, a3));
        m         = _mm_aesenc_si128(m, _mm_set_epi64x((long long)len, (long long)seed));
        uint64_t out[2];
        _mm_storeu_si128((__m128i*)out, m);
        uint64_t h = out[0] ^ out[1];
        if (rem > 0u)
            h = mu_wyhash64(p, rem, h);
        return h;
    }
#elif defined(__SSE4_2__) && defined(__x86_64__)
    if (len >= 24u)
    {
        const uint8_t* p   = (const uint8_t*)data;
        uint64_t       c0  = seed ^ (uint64_t)len;
        uint64_t       c1  = seed * UINT64_C(0x9e3779b97f4a7c15);
        uint64_t       c2  = ~seed;
        size_t         rem = len;
        while (rem >= 24u)
        {
            c0 = _mm_crc32_u64(c0, mu_wyhash__r8(p));
            c1 = _mm_crc32_u64(c1, mu_wyhash__r8(p + 8u));
            c2 = _mm_crc32_u64(c2, mu_wyhash__r8(p + 16u));
            p   += 24u;
            rem -= 24u;
        }
        uint64_t h = mu_wyhash__mum(c0 ^ (c1 << 21), c2 ^ (c1 >> 19));
        if (rem > 0u)
            h = mu_wyhash64(p, rem, h);
        return h;
    }
#endif
    return mu_wyhash64(data, len, seed);
}

/* ========================================================================== *
 * 2. Daniel Lemire `fastrange` & `fastmod` (Division-Free Exact Arithmetic)
 * ========================================================================== */

/* Map uniform 32-bit `word` into `[0, p)` without division (`mul + shr`). */
MU_INLINE uint32_t mu_fastrange32(uint32_t word, uint32_t p)
{
    return (uint32_t)(((uint64_t)word * (uint64_t)p) >> 32);
}

/* Map uniform 64-bit `word` into `[0, p)` without division (`128b mul`). */
MU_INLINE uint64_t mu_fastrange64(uint64_t word, uint64_t p)
{
#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((__uint128_t)word * (__uint128_t)p) >> 64);
#else
    uint64_t wl = (uint32_t)word, wh = word >> 32;
    uint64_t pl = (uint32_t)p,    ph = p >> 32;
    uint64_t p0 = wl * pl, p1 = wl * ph, p2 = wh * pl, p3 = wh * ph;
    uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
    return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
#endif
}

typedef struct mu_fastdiv_u32
{
    uint64_t M;
    uint32_t d;
} mu_fastdiv_u32;

MU_INLINE mu_fastdiv_u32 mu_fastdiv_u32_make(uint32_t d)
{
    MU_ASSERT(d > 0u);
    mu_fastdiv_u32 fd;
    fd.d = d;
    fd.M = UINT64_C(0xFFFFFFFFFFFFFFFF) / (uint64_t)d + 1ull;
    if (d == 1u)
        fd.M = 0ull; /* special case: 2^64 / 1 overflows u64; rem is always 0 */
    return fd;
}

/* Exact `a % fd.d` for ANY `a` and `d >= 1` using Lemire LKK19 direct remainder! */
MU_INLINE uint32_t mu_fastmod_u32(uint32_t a, mu_fastdiv_u32 fd)
{
    if (MU_UNLIKELY(fd.d == 1u))
        return 0u;
    uint64_t lowbits = fd.M * (uint64_t)a;
#if defined(__SIZEOF_INT128__)
    return (uint32_t)(((__uint128_t)lowbits * (__uint128_t)fd.d) >> 64);
#else
    return (uint32_t)mu_fastrange64(lowbits, (uint64_t)fd.d);
#endif
}

MU_INLINE bool mu_fast_is_divisible_u32(uint32_t a, mu_fastdiv_u32 fd)
{
    if (MU_UNLIKELY(fd.d == 1u))
        return true;
    return ((uint64_t)a * fd.M) <= (fd.M - 1ull);
}

/* ========================================================================== *
 * 3. 2D & 3D Morton (Z-Order) Codes + Dilated Neighbor Stepping
 * ========================================================================== */

#define MU_MORTON2D_X_MASK 0x55555555u
#define MU_MORTON2D_Y_MASK 0xAAAAAAAAu

MU_INLINE uint32_t mu_morton__part1by1(uint32_t x)
{
    x &= 0x0000FFFFu;
    x = (x ^ (x << 8)) & 0x00FF00FFu;
    x = (x ^ (x << 4)) & 0x0F0F0F0Fu;
    x = (x ^ (x << 2)) & 0x33333333u;
    x = (x ^ (x << 1)) & 0x55555555u;
    return x;
}

MU_INLINE uint32_t mu_morton__compact1by1(uint32_t x)
{
    x &= 0x55555555u;
    x = (x ^ (x >> 1)) & 0x33333333u;
    x = (x ^ (x >> 2)) & 0x0F0F0F0Fu;
    x = (x ^ (x >> 4)) & 0x00FF00FFu;
    x = (x ^ (x >> 8)) & 0x0000FFFFu;
    return x;
}

MU_INLINE uint32_t mu_morton2d_encode(uint16_t x, uint16_t y)
{
#if defined(__BMI2__)
    return _pdep_u32((uint32_t)x, MU_MORTON2D_X_MASK) |
           _pdep_u32((uint32_t)y, MU_MORTON2D_Y_MASK);
#else
    return mu_morton__part1by1((uint32_t)x) | (mu_morton__part1by1((uint32_t)y) << 1);
#endif
}

MU_INLINE void mu_morton2d_decode(uint32_t code, uint16_t* out_x, uint16_t* out_y)
{
#if defined(__BMI2__)
    if (out_x) *out_x = (uint16_t)_pext_u32(code, MU_MORTON2D_X_MASK);
    if (out_y) *out_y = (uint16_t)_pext_u32(code, MU_MORTON2D_Y_MASK);
#else
    if (out_x) *out_x = (uint16_t)mu_morton__compact1by1(code);
    if (out_y) *out_y = (uint16_t)mu_morton__compact1by1(code >> 1);
#endif
}

/* Fabian Giesen dilated-integer O(1) neighbor stepping without decoding! */
MU_INLINE uint32_t mu_morton2d_inc_x(uint32_t code)
{
    uint32_t nx = ((code | MU_MORTON2D_Y_MASK) + 1u) & MU_MORTON2D_X_MASK;
    return (code & MU_MORTON2D_Y_MASK) | nx;
}

MU_INLINE uint32_t mu_morton2d_inc_y(uint32_t code)
{
    uint32_t ny = ((code | MU_MORTON2D_X_MASK) + 2u) & MU_MORTON2D_Y_MASK;
    return (code & MU_MORTON2D_X_MASK) | ny;
}

MU_INLINE uint32_t mu_morton__part1by2(uint32_t x)
{
    x &= 0x000003FFu;
    x = (x ^ (x << 16)) & 0xFF0000FFu;
    x = (x ^ (x <<  8)) & 0x0300F00Fu;
    x = (x ^ (x <<  4)) & 0x030C30C3u;
    x = (x ^ (x <<  2)) & 0x09249249u;
    return x;
}

MU_INLINE uint32_t mu_morton__compact1by2(uint32_t x)
{
    x &= 0x09249249u;
    x = (x ^ (x >>  2)) & 0x030C30C3u;
    x = (x ^ (x >>  4)) & 0x0300F00Fu;
    x = (x ^ (x >>  8)) & 0xFF0000FFu;
    x = (x ^ (x >> 16)) & 0x000003FFu;
    return x;
}

/* 3D 10-bit-per-axis Morton code (`x, y, z` in `[0, 1023]` -> 30-bit Z-index). */
MU_INLINE uint32_t mu_morton3d_encode(uint16_t x, uint16_t y, uint16_t z)
{
#if defined(__BMI2__)
    return _pdep_u32((uint32_t)x, 0x09249249u) |
           _pdep_u32((uint32_t)y, 0x12492492u) |
           _pdep_u32((uint32_t)z, 0x24924924u);
#else
    return mu_morton__part1by2(x) | (mu_morton__part1by2(y) << 1) | (mu_morton__part1by2(z) << 2);
#endif
}

MU_INLINE void mu_morton3d_decode(uint32_t code, uint16_t* out_x, uint16_t* out_y, uint16_t* out_z)
{
#if defined(__BMI2__)
    if (out_x) *out_x = (uint16_t)_pext_u32(code, 0x09249249u);
    if (out_y) *out_y = (uint16_t)_pext_u32(code, 0x12492492u);
    if (out_z) *out_z = (uint16_t)_pext_u32(code, 0x24924924u);
#else
    if (out_x) *out_x = (uint16_t)mu_morton__compact1by2(code);
    if (out_y) *out_y = (uint16_t)mu_morton__compact1by2(code >> 1);
    if (out_z) *out_z = (uint16_t)mu_morton__compact1by2(code >> 2);
#endif
}

/* ========================================================================== *
 * 4. StreamVByte (Daniel Lemire SIMD `pshufb` Integer Codec)
 *
 * Wire format:
 *   - First `ceil(count / 4)` bytes: 2-bit length codes per integer
 *     (`00` = 1B, `01` = 2B, `10` = 3B, `11` = 4B), 4 ints packed per byte.
 *   - Followed immediately by packed little-endian data bytes (+ up to 16
 *     safe pad bytes so SIMD `_mm_loadu_si128` never reads past buffer end!).
 * ========================================================================== */

MU_INLINE size_t mu_streamvbyte_max_bytes(uint32_t count)
{
    size_t ctrl_bytes = ((size_t)count + 3u) >> 2;
    return ctrl_bytes + (size_t)count * 4u + 16u;
}

MU_INLINE uint32_t mu_streamvbyte__byte_len(uint32_t v)
{
    return (v < (1u << 8)) ? 1u : ((v < (1u << 16)) ? 2u : ((v < (1u << 24)) ? 3u : 4u));
}

/* Encodes `in[0..count)` into `out` (must have >= `mu_streamvbyte_max_bytes(count)`).
   Returns exact compressed byte size (excluding trailing 16B SIMD safety pad, though
   up to 16 zero pad bytes are written if caller provides `mu_streamvbyte_max_bytes`). */
MU_INLINE size_t mu_streamvbyte_encode_u32(const uint32_t* MU_RESTRICT in, uint32_t count, uint8_t* MU_RESTRICT out)
{
    if (count == 0u)
        return 0u;
    MU_ASSERT(in && out);

    size_t   ctrl_len = ((size_t)count + 3u) >> 2;
    uint8_t* ctrl     = out;
    uint8_t* data     = out + ctrl_len;

    uint32_t i = 0u;
    for (; i + 4u <= count; i += 4u)
    {
        uint32_t v0 = in[i + 0u], v1 = in[i + 1u], v2 = in[i + 2u], v3 = in[i + 3u];
        uint32_t l0 = mu_streamvbyte__byte_len(v0);
        uint32_t l1 = mu_streamvbyte__byte_len(v1);
        uint32_t l2 = mu_streamvbyte__byte_len(v2);
        uint32_t l3 = mu_streamvbyte__byte_len(v3);

        *ctrl++ = (uint8_t)(((l0 - 1u)      ) |
                            ((l1 - 1u) << 2u) |
                            ((l2 - 1u) << 4u) |
                            ((l3 - 1u) << 6u));

        /* Unconditional 4B overlapping little-endian stores (advance by exact `li`): */
        MU_MEMCPY(data, &v0, 4); data += l0;
        MU_MEMCPY(data, &v1, 4); data += l1;
        MU_MEMCPY(data, &v2, 4); data += l2;
        MU_MEMCPY(data, &v3, 4); data += l3;
    }

    if (i < count)
    {
        uint8_t cbyte = 0u;
        for (uint32_t sub = 0u; i < count; ++i, ++sub)
        {
            uint32_t v  = in[i];
            uint32_t ln = mu_streamvbyte__byte_len(v);
            cbyte |= (uint8_t)((ln - 1u) << (sub * 2u));
            for (uint32_t b = 0u; b < ln; ++b)
                *data++ = (uint8_t)(v >> (b * 8u));
        }
        *ctrl++ = cbyte;
    }

    size_t used = (size_t)(data - out);
    MU_MEMSET(data, 0, 16u); /* safe 16B SIMD overrun pad */
    return used;
}

MU_INLINE void mu_streamvbyte__build_lut_row(uint8_t code, uint8_t shuf[16], uint8_t* out_step)
{
    uint8_t off = 0u;
    for (uint32_t k = 0u; k < 4u; ++k)
    {
        uint8_t len = (uint8_t)(((code >> (k * 2u)) & 3u) + 1u);
        for (uint8_t b = 0u; b < 4u; ++b)
            shuf[k * 4u + b] = (b < len) ? (uint8_t)(off + b) : (uint8_t)0x80u;
        off = (uint8_t)(off + len);
    }
    *out_step = off;
}

/* Decodes `count` uint32_t values from `in` into `out`. Returns compressed bytes consumed. */
MU_INLINE size_t mu_streamvbyte_decode_u32(const uint8_t* MU_RESTRICT in, uint32_t count, uint32_t* MU_RESTRICT out)
{
    if (count == 0u)
        return 0u;
    MU_ASSERT(in && out);

    size_t         ctrl_len = ((size_t)count + 3u) >> 2;
    const uint8_t* ctrl     = in;
    const uint8_t* data     = in + ctrl_len;
    uint32_t       i        = 0u;

#if defined(__SSSE3__)
    /* Lazily initialize 256-entry 16B shuffle LUT (4 KB fits comfortably in L1d): */
    static uint8_t  s_shuf[256][16];
    static uint8_t  s_step[256];
    static uint32_t s_ready = 0u;
    if (MU_UNLIKELY(!s_ready))
    {
        for (uint32_t c = 0u; c < 256u; ++c)
            mu_streamvbyte__build_lut_row((uint8_t)c, s_shuf[c], &s_step[c]);
        s_ready = 1u;
    }

    for (; i + 4u <= count; i += 4u)
    {
        uint8_t c    = *ctrl++;
        __m128i raw  = _mm_loadu_si128((const __m128i*)data);
        __m128i mask = _mm_loadu_si128((const __m128i*)s_shuf[c]);
        __m128i vals = _mm_shuffle_epi8(raw, mask);
        _mm_storeu_si128((__m128i*)(out + i), vals);
        data += s_step[c];
    }
#else
    for (; i + 4u <= count; i += 4u)
    {
        uint8_t c = *ctrl++;
        for (uint32_t sub = 0u; sub < 4u; ++sub)
        {
            uint32_t ln = ((uint32_t)(c >> (sub * 2u)) & 3u) + 1u;
            uint32_t v  = 0u;
            for (uint32_t b = 0u; b < ln; ++b)
                v |= (uint32_t)(*data++) << (b * 8u);
            out[i + sub] = v;
        }
    }
#endif

    if (i < count)
    {
        uint8_t c = *ctrl++;
        for (uint32_t sub = 0u; i < count; ++i, ++sub)
        {
            uint32_t ln = ((uint32_t)(c >> (sub * 2u)) & 3u) + 1u;
            uint32_t v  = 0u;
            for (uint32_t b = 0u; b < ln; ++b)
                v |= (uint32_t)(*data++) << (b * 8u);
            out[i] = v;
        }
    }
    return (size_t)(data - in);
}

MU_END_EXTERN_C

#endif /* MU_FAST_CODEC_H */
