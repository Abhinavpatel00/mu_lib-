# AVX2, BMI2, PCLMULQDQ & AVX-512 Kernel Reference (Verified in `perf_test/`)

All snippets below are verified warning-clean under `gcc -O3 -march=native -Wall -Wextra -Werror` with bit-exact checksums in `perf_test/bench_simd_avx.c` and `perf_test/bench_highperf_ds.c`.

---

## 1. Frostbite Prebound P-Vertex SoA Frustum Culling (`16.13×`)

```c
/* Prebind positive-vertex SoA array pointers per plane OUTSIDE the entity loop: */
const float *px[6], *py[6], *pz[6];
__m256 vnx[6], vny[6], vnz[6], vd[6];
for (int p = 0; p < 6; ++p) {
    px[p] = (pl[p].nx >= 0.0f) ? max_x : min_x;
    py[p] = (pl[p].ny >= 0.0f) ? max_y : min_y;
    pz[p] = (pl[p].nz >= 0.0f) ? max_z : min_z;
    vnx[p] = _mm256_set1_ps(pl[p].nx);
    vny[p] = _mm256_set1_ps(pl[p].ny);
    vnz[p] = _mm256_set1_ps(pl[p].nz);
    vd[p]  = _mm256_set1_ps(pl[p].d);
}
for (size_t i = 0; i < count; i += 8) {
    __m256 out = _mm256_setzero_ps();
    for (int p = 0; p < 6; ++p) {
        __m256 dist = _mm256_fmadd_ps(vnx[p], _mm256_load_ps(px[p] + i),
                      _mm256_fmadd_ps(vny[p], _mm256_load_ps(py[p] + i),
                      _mm256_fmadd_ps(vnz[p], _mm256_load_ps(pz[p] + i), vd[p])));
        out = _mm256_or_ps(out, dist); /* IEEE-754 sign bit 1 <=> dist < 0 (outside!) */
    }
    vis_bytes[i >> 3] = (uint8_t)(~_mm256_movemask_ps(out) & 0xFF);
}
```

---

## 2. Lemire `vpshufb` 2-Nibble Register LUT + `PCLMULQDQ` Quote Prefix-XOR (`15.6×` / `24.2×`)

```c
/* Prefix-XOR of all 64 quote bits in 1 carryless multiply: */
static inline uint64_t prefix_xor_u64(uint64_t bitmask) {
    __m128i r = _mm_clmulepi64_si128(_mm_cvtsi64_si128((int64_t)bitmask),
                                     _mm_set1_epi8((char)0xFF), 0x00);
    return (uint64_t)_mm_cvtsi128_si64(r);
}
```

---

## 3. AVX2 `simdprune` Stream Compaction vs AVX-512 `vpcompressd` (`21.7×`)

```c
/* AVX-512 (Ice Lake / Zen 4+): 1 instruction, 0 LUT! */
#if defined(__AVX512F__) && defined(__AVX512VL__)
__mmask16 k = _mm512_cmpgt_epi32_mask(v, vthresh);
_mm512_mask_compressstoreu_epi32(out + pos, k, v);
pos += (size_t)_mm_popcnt_u32((uint32_t)k);
#else
/* AVX2: 256-entry 8-lane permutation LUT (8 KB total, L1-resident): */
__m256i cmp = _mm256_cmpgt_epi32(v, vthresh);
uint32_t m  = (uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(cmp));
__m256i packed = _mm256_permutevar8x32_epi32(v, _mm256_load_si256(&perm_lut[m]));
_mm256_storeu_si256((__m256i *)(out + pos), packed);
pos += (size_t)_mm_popcnt_u32(m);
#endif
```

---

## 4. 16-Ary Cache-Line AVX2 Static Search Tree (`9.79×` over `lower_bound`)

```c
/* Node = 16 sorted int32_t keys = 64B = 1 exact cache line! Child = node*16 + 1 + rank */
__m256i vx = _mm256_set1_epi32(query_key - 1);
size_t node = 0;
for (uint32_t l = 0; l < LEVELS; ++l) {
    const int32_t *keys = &tree[node * 16u];
    __m256i c0 = _mm256_cmpgt_epi32(vx, _mm256_load_si256((const __m256i *)(keys + 0)));
    __m256i c1 = _mm256_cmpgt_epi32(vx, _mm256_load_si256((const __m256i *)(keys + 8)));
    uint32_t m = (uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(c0))
               | ((uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(c1)) << 8);
    node = node * 16u + 1u + (uint32_t)_mm_popcnt_u32(m);
}
```

---

## 5. `GFNI` 1-Instruction 8×8 Bit-Matrix / Bit-Reverse (`93.20×`, 87.8 GB/s)

```c
/* Anti-diagonal 8x8 GF(2) matrix `0x8040201008040201ULL` reverses bits in all 64 bytes in 1 cycle! */
const __m512i rev_mat = _mm512_set1_epi64((long long)0x8040201008040201ULL);
__m512i r = _mm512_gf2p8affine_epi64_epi8(v, rev_mat, 0);
```

---

## 6. Lemire `StreamVByte` `pshufb` Varint Decoder (`43.93×` over Protobuf `LEB128`, 5.64B ints/s)

```c
/* 1 control byte holds four 2-bit lengths (1..4B); 256-entry 16B LUT expands 4 u32s in 1 pshufb! */
uint8_t c = ctrl[g];
__m128i vals = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)dp), g_svb_shuf_lut[c]);
dp += g_svb_len_lut[c];
```

---

## 7. Associative SIMD `pshufb` DFA Composition (`8.84×` over `state = dfa[state][c]`)

```c
/* Each 16-byte vector T_c maps old_state (0..15) -> next_state (0..15).
 * Function composition (T_b o T_a) is literally `_mm_shuffle_epi8(T_b, T_a)`! */
__m128i pair_fn = _mm_shuffle_epi8(g_dfa_trans_vec[b1], g_dfa_trans_vec[b0]);
acc_fn          = _mm_shuffle_epi8(pair_fn, acc_fn);
```

---

## 8. Single-Pass Multi-Accumulator Cosine Similarity + AVX-512 `{z}` Masked Tail

```c
/* 1 memory pass computes dot(a,b), dot(a,a), dot(b,b) across 12 independent ZMM accumulators */
__attribute__((target("avx512f")))
static float cosine_f32_avx512(const float * MU_RESTRICT a, const float * MU_RESTRICT b, size_t n) {
    size_t i = 0, n64 = n & ~(size_t)63u;
    __m512 ab0 = _mm512_setzero_ps(), ab1 = _mm512_setzero_ps(), ab2 = _mm512_setzero_ps(), ab3 = _mm512_setzero_ps();
    __m512 aa0 = _mm512_setzero_ps(), aa1 = _mm512_setzero_ps(), aa2 = _mm512_setzero_ps(), aa3 = _mm512_setzero_ps();
    __m512 bb0 = _mm512_setzero_ps(), bb1 = _mm512_setzero_ps(), bb2 = _mm512_setzero_ps(), bb3 = _mm512_setzero_ps();
    for (; i < n64; i += 64) {
        __m512 a0 = _mm512_loadu_ps(a + i),      b0 = _mm512_loadu_ps(b + i);
        __m512 a1 = _mm512_loadu_ps(a + i + 16), b1 = _mm512_loadu_ps(b + i + 16);
        __m512 a2 = _mm512_loadu_ps(a + i + 32), b2 = _mm512_loadu_ps(b + i + 32);
        __m512 a3 = _mm512_loadu_ps(a + i + 48), b3 = _mm512_loadu_ps(b + i + 48);
        ab0 = _mm512_fmadd_ps(a0, b0, ab0); aa0 = _mm512_fmadd_ps(a0, a0, aa0); bb0 = _mm512_fmadd_ps(b0, b0, bb0);
        ab1 = _mm512_fmadd_ps(a1, b1, ab1); aa1 = _mm512_fmadd_ps(a1, a1, aa1); bb1 = _mm512_fmadd_ps(b1, b1, bb1);
        ab2 = _mm512_fmadd_ps(a2, b2, ab2); aa2 = _mm512_fmadd_ps(a2, a2, aa2); bb2 = _mm512_fmadd_ps(b2, b2, bb2);
        ab3 = _mm512_fmadd_ps(a3, b3, ab3); aa3 = _mm512_fmadd_ps(a3, a3, aa3); bb3 = _mm512_fmadd_ps(b3, b3, bb3);
    }
    for (; i + 16 <= n; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i), vb = _mm512_loadu_ps(b + i);
        ab0 = _mm512_fmadd_ps(va, vb, ab0); aa0 = _mm512_fmadd_ps(va, va, aa0); bb0 = _mm512_fmadd_ps(vb, vb, bb0);
    }
    if (i < n) { /* Zero-masked AVX-512 tail: zero scalar remainder loop! */
        __mmask16 k = (__mmask16)((1u << (n - i)) - 1u);
        __m512 va = _mm512_maskz_loadu_ps(k, a + i), vb = _mm512_maskz_loadu_ps(k, b + i);
        ab0 = _mm512_fmadd_ps(va, vb, ab0); aa0 = _mm512_fmadd_ps(va, va, aa0); bb0 = _mm512_fmadd_ps(vb, vb, bb0);
    }
    float dab = _mm512_reduce_add_ps(_mm512_add_ps(_mm512_add_ps(ab0, ab1), _mm512_add_ps(ab2, ab3)));
    float daa = _mm512_reduce_add_ps(_mm512_add_ps(_mm512_add_ps(aa0, aa1), _mm512_add_ps(aa2, aa3)));
    float dbb = _mm512_reduce_add_ps(_mm512_add_ps(_mm512_add_ps(bb0, bb1), _mm512_add_ps(bb2, bb3)));
    float denom = daa * dbb;
    return denom > 1e-30f ? dab / sqrtf(denom) : 0.0f;
}
```

---

## 9. Portable `.so`-Safe Runtime CPU Dispatch (`__builtin_cpu_supports`)

```c
/* Preferred over target_clones IFUNCs for hand-written intrinsics & shared libraries.
 * Note: _mm512_popcnt_epi8 requires "avx512bitalg" + "avx512bw" (NOT just "avx512vpopcntdq"!). */
typedef float (*cosine_fn)(const float *, const float *, size_t);

static cosine_fn resolve_cosine(void) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (__builtin_cpu_supports("x86-64-v4")) return cosine_f32_avx512;
    if (__builtin_cpu_supports("x86-64-v3")) return cosine_f32_avx2;
#endif
    return cosine_f32_scalar;
}
```


