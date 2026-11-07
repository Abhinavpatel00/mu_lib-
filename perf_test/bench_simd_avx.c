/*
 * bench_simd_avx.c — Desktop x86-64 Full SIMD Leverage (SSE4.2 / AVX2 / AVX-512 / PCLMUL / BMI2)
 * ==============================================================================================
 * Proves optimizations from:
 *   1. Frostbite / Embree 8-Wide SIMD Frustum Culling (SoA + AVX2 P-Vertex -> Bitmask) vs AoS Branchy
 *   2. Daniel Lemire Universal vpshufb 16-Byte Nibble LUT Classifier (AVX2 32B/iter & AVX-512 64B/iter)
 *   3. simdjson Quote-Masking via PCLMULQDQ Carryless Prefix-XOR vs Scalar Escape/Quote State Machine
 *   4. Stream Compaction / Filtering: Scalar Branchy vs AVX2 LUT vpermd (simdprune) vs AVX-512 vpcompressd
 *   5. SWAR / AVX2 8-Digit ASCII Integer Parsing (vpmaddubsw + vpmaddwd) vs strtoul / Scalar Loop
 *   6. Bitset Popcount at Scale: Scalar Kernighan vs POPCNTll vs AVX2 Harley-Seal vs AVX-512 vpopcntdq
 *   7. Fabian Giesen In-Register 8x8 Float Matrix Transpose (AoS <-> SoA Staging) vs Scalar Strided Copy
 */

#include "bench_common.h"
#include <immintrin.h>

#define REPS 7

/* =========================================================================
 * 1. Frostbite / Intel Embree 8-Wide SIMD Frustum Culling vs AoS Scalar
 * ========================================================================= */
typedef struct {
    float minx, miny, minz;
    float maxx, maxy, maxz;
} AABB_AoS;

typedef struct {
    float nx, ny, nz, d; /* plane eq: nx*x + ny*y + nz*z + d >= 0 is inside */
} Plane;

BENCH_NOINLINE static size_t cull_frustum_aos_scalar(const AABB_AoS * BENCH_RESTRICT boxes,
                                                     const Plane planes[6],
                                                     uint64_t * BENCH_RESTRICT vis_bits,
                                                     size_t n)
{
    memset(vis_bits, 0, (n / 64u) * sizeof(uint64_t));
    size_t vis_count = 0;
    for (size_t i = 0; i < n; ++i) {
        const AABB_AoS *b = &boxes[i];
        bool inside = true;
        for (int p = 0; p < 6; ++p) {
            /* Positive-vertex (p-vertex) test */
            float px = (planes[p].nx >= 0.0f) ? b->maxx : b->minx;
            float py = (planes[p].ny >= 0.0f) ? b->maxy : b->miny;
            float pz = (planes[p].nz >= 0.0f) ? b->maxz : b->minz;
            if (planes[p].nx * px + planes[p].ny * py + planes[p].nz * pz + planes[p].d < 0.0f) {
                inside = false;
                break;
            }
        }
        if (inside) {
            vis_bits[i >> 6] |= UINT64_C(1) << (i & 63u);
            ++vis_count;
        }
    }
    return vis_count;
}

/* Frostbite ("Culling the Battlefield"): SoA min/max streams, pre-select min/max stream pointers
 * per plane sign ONCE before the loop (eliminates all per-box sign selects!), 8 boxes per AVX2 pass,
 * pack 8-bit movemask directly into visibility bitset! */
BENCH_NOINLINE static size_t cull_frustum_soa_avx2(
    const float * BENCH_RESTRICT minx, const float * BENCH_RESTRICT miny, const float * BENCH_RESTRICT minz,
    const float * BENCH_RESTRICT maxx, const float * BENCH_RESTRICT maxy, const float * BENCH_RESTRICT maxz,
    const Plane planes[6], uint64_t * BENCH_RESTRICT vis_bits, size_t n)
{
    /* Pre-bind p-vertex stream per plane outside the loop! (Frostbite / Arseny Kapoulkine trick) */
    const float *px_stream[6], *py_stream[6], *pz_stream[6];
    __m256 vnx[6], vny[6], vnz[6], vd[6];
    for (int p = 0; p < 6; ++p) {
        px_stream[p] = (planes[p].nx >= 0.0f) ? maxx : minx;
        py_stream[p] = (planes[p].ny >= 0.0f) ? maxy : miny;
        pz_stream[p] = (planes[p].nz >= 0.0f) ? maxz : minz;
        vnx[p] = _mm256_set1_ps(planes[p].nx);
        vny[p] = _mm256_set1_ps(planes[p].ny);
        vnz[p] = _mm256_set1_ps(planes[p].nz);
        vd[p]  = _mm256_set1_ps(planes[p].d);
    }

    uint8_t *out_bytes = (uint8_t *)vis_bits;
    size_t vis_count = 0;
    for (size_t i = 0; i < n; i += 8) {
        __m256 outside = _mm256_setzero_ps();
        for (int p = 0; p < 6; ++p) {
            __m256 dist = _mm256_fmadd_ps(vnx[p], _mm256_load_ps(px_stream[p] + i), vd[p]);
            dist = _mm256_fmadd_ps(vny[p], _mm256_load_ps(py_stream[p] + i), dist);
            dist = _mm256_fmadd_ps(vnz[p], _mm256_load_ps(pz_stream[p] + i), dist);
            outside = _mm256_or_ps(outside, dist); /* sign bit = 1 iff dist < 0.0f! */
        }
        /* Sign bit of `outside` is 1 if ANY plane had dist < 0 -> invert 8-bit movemask! */
        unsigned mask = (~(unsigned)_mm256_movemask_ps(outside)) & 0xFFu;
        out_bytes[i >> 3] = (uint8_t)mask;
        vis_count += (size_t)__builtin_popcount(mask);
    }
    return vis_count;
}

static void run_case_frustum_cull(FILE *csv)
{
    const size_t N = 1u << 18; /* 262,144 AABBs */
    AABB_AoS *aos = bench_alloc_aligned(64, N * sizeof(AABB_AoS));
    float *minx = bench_alloc_aligned(64, N * sizeof(float));
    float *miny = bench_alloc_aligned(64, N * sizeof(float));
    float *minz = bench_alloc_aligned(64, N * sizeof(float));
    float *maxx = bench_alloc_aligned(64, N * sizeof(float));
    float *maxy = bench_alloc_aligned(64, N * sizeof(float));
    float *maxz = bench_alloc_aligned(64, N * sizeof(float));
    uint64_t *vis1 = bench_alloc_aligned(64, (N / 64u) * sizeof(uint64_t));
    uint64_t *vis2 = bench_alloc_aligned(64, (N / 64u) * sizeof(uint64_t));

    Plane planes[6] = {
        {  1.0f,  0.0f,  0.0f, 50.0f }, { -1.0f,  0.0f,  0.0f, 50.0f },
        {  0.0f,  1.0f,  0.0f, 50.0f }, {  0.0f, -1.0f,  0.0f, 50.0f },
        {  0.0f,  0.0f,  1.0f, 50.0f }, {  0.0f,  0.0f, -1.0f, 50.0f }
    };

    uint64_t seed = 0x4242424213371337ULL;
    for (size_t i = 0; i < N; ++i) {
        float cx = ((float)(splitmix64(&seed) & 1023u) - 512.0f) * 0.2f;
        float cy = ((float)(splitmix64(&seed) & 1023u) - 512.0f) * 0.2f;
        float cz = ((float)(splitmix64(&seed) & 1023u) - 512.0f) * 0.2f;
        minx[i] = cx - 1.5f; maxx[i] = cx + 1.5f;
        miny[i] = cy - 1.5f; maxy[i] = cy + 1.5f;
        minz[i] = cz - 1.5f; maxz[i] = cz + 1.5f;
        aos[i] = (AABB_AoS){ minx[i], miny[i], minz[i], maxx[i], maxy[i], maxz[i] };
    }

    bench_print_section("1. Frustum Culling 262K AABBs (6 Planes -> Bitset): AoS Early-Out vs Frostbite SoA AVX2",
                        "Frostbite ('Culling the Battlefield'), Intel Embree, Arseny Kapoulkine (zeux)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    size_t chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = cull_frustum_aos_scalar(aos, planes, vis1, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "simd_avx", "frustum_cull", "AoS Scalar + Plane Early-Out Branch", b_ns, b_cyc, N, base, chk1, "Mb/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = cull_frustum_soa_avx2(minx, miny, minz, maxx, maxy, maxz, planes, vis2, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "simd_avx", "frustum_cull", "Frostbite SoA + Prebound P-Vtx AVX2", b_ns, b_cyc, N, base, chk2, "Mb/s", 1.0);

    free(aos); free(minx); free(miny); free(minz); free(maxx); free(maxy); free(maxz); free(vis1); free(vis2);
}

/* =========================================================================
 * 2. Daniel Lemire Universal vpshufb Nibble-LUT Character Classification
 *    Count JSON structural + whitespace characters: ' ', '\t', '\n', '\r', '{', '}', '[', ']', ':', ','
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static size_t classify_scalar_switch(const uint8_t *buf, size_t len)
{
    size_t count = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = buf[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '{' || c == '}'  || c == '['  || c == ']'  ||
            c == ':' || c == ',')
            ++count;
    }
    return count;
}

/* Lemire / Mula simdjson technique:
 * Split each byte into lo_nibble = c & 0x0F, hi_nibble = (c >> 4) & 0x0F.
 * Look up a bitmask per nibble in TWO 16-byte vpshufb registers, AND them!
 * Classifies arbitrary sets of ASCII characters at 32 bytes per 5 instructions! */
BENCH_NOINLINE static size_t classify_lemire_vpshufb_avx2(const uint8_t * BENCH_RESTRICT buf, size_t len)
{
    /* Target chars and their (hi, lo) hex nibbles:
     * '\t'=0x09 (0,9), '\n'=0x0A (0,A), '\r'=0x0D (0,D), ' '=0x20 (2,0),
     * ',' =0x2C (2,C), ':' =0x3A (3,A), '[' =0x5B (5,B), ']'=0x5D (5,D),
     * '{' =0x7B (7,B), '}' =0x7D (7,D)
     * Assign bit classes:
     *   bit 0 (0x01): '\t'(0,9), '\n'(0,A), '\r'(0,D)  -> hi=0 has 0x01; lo=9,A,D has 0x01
     *   bit 1 (0x02): ' '(2,0), ','(2,C)               -> hi=2 has 0x02; lo=0,C   has 0x02
     *   bit 2 (0x04): ':'(3,A)                         -> hi=3 has 0x04; lo=A     has 0x04
     *   bit 3 (0x08): '['(5,B), ']'(5,D), '{'(7,B), '}'(7,D) -> hi=5,7 has 0x08; lo=B,D has 0x08
     */
    const __m256i lut_lo = _mm256_broadcastsi128_si256(_mm_setr_epi8(
        /* 0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F */
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x08, 0x02, 0x09, 0x00, 0x00));
    const __m256i lut_hi = _mm256_broadcastsi128_si256(_mm_setr_epi8(
        0x01, 0x00, 0x02, 0x04, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
    const __m256i nib_mask = _mm256_set1_epi8(0x0F);
    const __m256i zero     = _mm256_setzero_si256();

    size_t count = 0;
    for (size_t i = 0; i < len; i += 32) {
        __m256i v   = _mm256_load_si256((const __m256i *)(buf + i));
        __m256i lo  = _mm256_and_si256(v, nib_mask);
        __m256i hi  = _mm256_and_si256(_mm256_srli_epi16(v, 4), nib_mask);
        __m256i hit = _mm256_and_si256(_mm256_shuffle_epi8(lut_lo, lo),
                                       _mm256_shuffle_epi8(lut_hi, hi));
        /* Non-zero byte => matched! Compare against zero and invert 32-bit movemask */
        uint32_t m  = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, zero));
        count += (size_t)__builtin_popcount(m);
    }
    return count;
}

static void run_case_vpshufb_classify(FILE *csv)
{
    const size_t LEN = 1u << 22; /* 4 MiB JSON-like ASCII stream */
    uint8_t *buf = bench_alloc_aligned(64, LEN);
    const char sample[] = "{\"id\":12345,\"pos\":[12.5, 99.1, -4.0], \"name\":\"goblin_warrior\"}\n";
    size_t slen = strlen(sample);
    for (size_t i = 0; i < LEN; ++i) buf[i] = (uint8_t)sample[i % slen];

    bench_print_section("2. Multi-Class ASCII Token Scanner (4 MB JSON): Scalar Branchy vs Lemire AVX2 vpshufb Nibble LUT",
                        "Daniel Lemire & Wojciech Mula (simdjson, despacer)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    size_t chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = classify_scalar_switch(buf, LEN);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "simd_avx", "vpshufb_lut", "Scalar 10-Char Comparison Chain", b_ns, b_cyc, LEN, base, chk1, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = classify_lemire_vpshufb_avx2(buf, LEN);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "simd_avx", "vpshufb_lut", "Lemire AVX2 vpshufb 2-Nibble LUT (32B)", b_ns, b_cyc, LEN, base, chk2, "MB/s", 1.0);

    free(buf);
}

/* =========================================================================
 * 3. simdjson Quote-Masking via PCLMULQDQ Carryless Prefix-XOR
 *    Count colons ':' that are OUTSIDE double-quoted strings "..." (no escapes)
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static size_t count_unquoted_colons_scalar(const uint8_t *buf, size_t len)
{
    bool in_quote = false;
    size_t count = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = buf[i];
        if (c == '"') in_quote = !in_quote;
        else if (c == ':' && !in_quote) ++count;
    }
    return count;
}

/* simdjson crown jewel:
 * Carryless multiply of quote_bitmask by all-ones (0xFFFFFFFFFFFFFFFFULL)
 * computes the cumulative prefix-XOR of all 64 bits in ONE instruction (`pclmulqdq`)! */
BENCH_NOINLINE static size_t count_unquoted_colons_pclmul_avx2(const uint8_t * BENCH_RESTRICT buf, size_t len)
{
    const __m256i vquote = _mm256_set1_epi8('"');
    const __m256i vcolon = _mm256_set1_epi8(':');
    const __m128i all_ones = _mm_set1_epi8(-1);
    uint64_t prev_in_quote = 0; /* 0 or ~0ULL carried from previous 64-byte block */
    size_t count = 0;

    for (size_t i = 0; i < len; i += 64) {
        __m256i b0 = _mm256_load_si256((const __m256i *)(buf + i));
        __m256i b1 = _mm256_load_si256((const __m256i *)(buf + i + 32));

        uint64_t q0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b0, vquote));
        uint64_t q1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b1, vquote));
        uint64_t quote_bits = q0 | (q1 << 32);

        uint64_t c0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b0, vcolon));
        uint64_t c1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b1, vcolon));
        uint64_t colon_bits = c0 | (c1 << 32);

        /* Prefix-XOR of 64 bits via carryless multiply with ~0ULL! */
        __m128i qvec   = _mm_cvtsi64_si128((int64_t)quote_bits);
        uint64_t in_q  = (uint64_t)_mm_cvtsi128_si64(_mm_clmulepi64_si128(qvec, all_ones, 0));
        in_q ^= prev_in_quote;
        prev_in_quote = (uint64_t)((int64_t)in_q >> 63); /* arithmetic broadcast of bit 63! */

        uint64_t unquoted_colons = colon_bits & ~in_q;
        count += (size_t)__builtin_popcountll(unquoted_colons);
    }
    return count;
}

static void run_case_pclmul_quotes(FILE *csv)
{
    const size_t LEN = 1u << 22; /* 4 MiB */
    uint8_t *buf = bench_alloc_aligned(64, LEN);
    /* Contains colons both inside and outside quotes; exactly 64-byte aligned pattern */
    const char blk[64] = "{\"url\":\"http://a:b:c/d\",\"port\":8080,\"msg\":\"x:y:z\",\"ok\":true}   \n";
    for (size_t i = 0; i < LEN; i += 64) memcpy(buf + i, blk, 64);

    bench_print_section("3. Quote-Aware Structural Parsing (4 MB): Scalar State-Machine vs simdjson PCLMULQDQ + AVX2",
                        "Geoff Langdale & Daniel Lemire (simdjson Prefix-XOR via pclmulqdq)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    size_t chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = count_unquoted_colons_scalar(buf, LEN);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "simd_avx", "pclmul_quotes", "Scalar Stateful Loop (in_quote flip)", b_ns, b_cyc, LEN, base, chk1, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = count_unquoted_colons_pclmul_avx2(buf, LEN);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "simd_avx", "pclmul_quotes", "AVX2 + PCLMULQDQ 64-Byte Prefix-XOR", b_ns, b_cyc, LEN, base, chk2, "MB/s", 1.0);

    free(buf);
}

/* =========================================================================
 * 4. Stream Compaction (Filter Active IDs where val > thresh):
 *    Scalar Branchy Push vs AVX2 256-Entry vpermd LUT (simdprune) vs AVX-512 vpcompressd
 * ========================================================================= */
static __m256i g_compress256_lut[256];

static void init_compress256_lut(void)
{
    for (unsigned mask = 0; mask < 256; ++mask) {
        int32_t idx[8] = {0,0,0,0,0,0,0,0};
        int pos = 0;
        for (int lane = 0; lane < 8; ++lane) {
            if ((mask >> lane) & 1u)
                idx[pos++] = lane;
        }
        g_compress256_lut[mask] = _mm256_loadu_si256((const __m256i *)idx);
    }
}

BENCH_NOINLINE __attribute__((optimize("no-if-conversion","no-tree-vectorize")))
static size_t filter_scalar_branchy(const int32_t * BENCH_RESTRICT in, int32_t * BENCH_RESTRICT out,
                                    size_t n, int32_t thresh)
{
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        if (in[i] > thresh)
            out[pos++] = in[i];
    }
    return pos;
}

BENCH_NOINLINE static size_t filter_avx2_simdprune(const int32_t * BENCH_RESTRICT in, int32_t * BENCH_RESTRICT out,
                                                   size_t n, int32_t thresh)
{
    __m256i vth = _mm256_set1_epi32(thresh);
    int32_t *dst = out;
    for (size_t i = 0; i < n; i += 8) {
        __m256i v    = _mm256_load_si256((const __m256i *)(in + i));
        __m256i cmp  = _mm256_cmpgt_epi32(v, vth);
        unsigned m   = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(cmp));
        __m256i perm = g_compress256_lut[m];
        __m256i pack = _mm256_permutevar8x32_epi32(v, perm);
        _mm256_storeu_si256((__m256i *)dst, pack);
        dst += __builtin_popcount(m);
    }
    return (size_t)(dst - out);
}

#if defined(__AVX512F__) && defined(__AVX512VL__)
BENCH_NOINLINE static size_t filter_avx512_vpcompressd(const int32_t * BENCH_RESTRICT in, int32_t * BENCH_RESTRICT out,
                                                       size_t n, int32_t thresh)
{
    __m512i vth = _mm512_set1_epi32(thresh);
    int32_t *dst = out;
    for (size_t i = 0; i < n; i += 16) {
        __m512i v      = _mm512_load_si512((const void *)(in + i));
        __mmask16 mask = _mm512_cmpgt_epi32_mask(v, vth);
        _mm512_mask_compressstoreu_epi32(dst, mask, v);
        dst += __builtin_popcount((unsigned)mask);
    }
    return (size_t)(dst - out);
}
#endif

static void run_case_stream_compaction(FILE *csv)
{
    init_compress256_lut();
    const size_t N = 1u << 20; /* 1,048,576 int32s (~50% pass) */
    int32_t *in  = bench_alloc_aligned(64, N * sizeof(int32_t));
    int32_t *out = bench_alloc_aligned(64, (N + 32) * sizeof(int32_t));
    uint64_t seed = 0xdeadc0de12345678ULL;
    for (size_t i = 0; i < N; ++i) in[i] = (int32_t)(splitmix64(&seed) & 1023u);

    bench_print_section("4. Stream Compaction / Filter (1M int32, 50% random pass): Scalar vs AVX2 LUT vs AVX-512",
                        "Daniel Lemire (simdprune), Intel AVX-512 vpcompressd, Frostbite/GPU Indirect Cull");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    size_t chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = filter_scalar_branchy(in, out, N, 511);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(out[chk1 - 1]);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    uint64_t sum1 = (uint64_t)chk1 + (uint64_t)out[0] + (uint64_t)out[chk1 - 1];
    double base = (double)b_ns;
    bench_print_row(csv, "simd_avx", "stream_compact", "Scalar if(x > thresh) out[pos++]=x", b_ns, b_cyc, N, base, sum1, "Mi/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = filter_avx2_simdprune(in, out, N, 511);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(out[chk2 - 1]);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    uint64_t sum2 = (uint64_t)chk2 + (uint64_t)out[0] + (uint64_t)out[chk2 - 1];
    bench_print_row(csv, "simd_avx", "stream_compact", "AVX2 vpermd 256-LUT Left-Pack (simdprune)", b_ns, b_cyc, N, base, sum2, "Mi/s", 1.0);

#if defined(__AVX512F__) && defined(__AVX512VL__)
    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    size_t chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk3 = filter_avx512_vpcompressd(in, out, N, 511);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(out[chk3 - 1]);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    uint64_t sum3 = (uint64_t)chk3 + (uint64_t)out[0] + (uint64_t)out[chk3 - 1];
    bench_print_row(csv, "simd_avx", "stream_compact", "AVX-512 Hardware vpcompressd (16-wide)", b_ns, b_cyc, N, base, sum3, "Mi/s", 1.0);
#endif

    free(in); free(out);
}

/* =========================================================================
 * 5. 8-Digit ASCII Integer Parsing: Scalar Loop vs SWAR 64-bit vs AVX2 vpmaddubsw
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static uint64_t parse8_scalar_loop(const char *buf, size_t count)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *s = buf + i * 8u;
        uint32_t v = 0;
        for (int k = 0; k < 8; ++k)
            v = v * 10u + (uint32_t)(s[k] - '0');
        sum += v;
    }
    return sum;
}

/* Daniel Lemire SWAR 8-digit parser: 3 integer multiplies in standard 64-bit GPR! */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static uint64_t parse8_lemire_swar(const char *buf, size_t count)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t val;
        memcpy(&val, buf + i * 8u, 8);
        val = val - UINT64_C(0x3030303030303030);
        val = (val * 10u) + (val >> 8);
        val = ((val & UINT64_C(0x00FF00FF00FF00FF)) * UINT64_C(100) +
              ((val >> 16) & UINT64_C(0x00FF00FF00FF00FF))) & UINT64_C(0x0000FFFF0000FFFF);
        uint32_t res = (uint32_t)((val * UINT64_C(10000) + (val >> 32)) & 0xFFFFFFFFu);
        sum += res;
    }
    return sum;
}

/* Lemire / Mula AVX2 4x8-digit SIMD parser (32 ASCII bytes -> 4 uint32s per iteration!) */
BENCH_NOINLINE static uint64_t parse8_lemire_avx2(const char * BENCH_RESTRICT buf, size_t count)
{
    const __m256i ascii0 = _mm256_set1_epi8('0');
    const __m256i mul10  = _mm256_setr_epi8(
        10,1,10,1,10,1,10,1, 10,1,10,1,10,1,10,1,
        10,1,10,1,10,1,10,1, 10,1,10,1,10,1,10,1);
    const __m256i mul100 = _mm256_setr_epi16(
        100,1,100,1, 100,1,100,1, 100,1,100,1, 100,1,100,1);
    const __m256i mul10000 = _mm256_setr_epi16(
        10000,1,10000,1, 0,0,0,0, 10000,1,10000,1, 0,0,0,0);
    const __m256i zero = _mm256_setzero_si256();

    __m256i acc = _mm256_setzero_si256();
    for (size_t i = 0; i < count; i += 4) {
        __m256i v  = _mm256_sub_epi8(_mm256_load_si256((const __m256i *)(buf + i * 8u)), ascii0);
        __m256i t1 = _mm256_maddubs_epi16(v, mul10);     /* pairs -> 0..99 in epi16 */
        __m256i t2 = _mm256_madd_epi16(t1, mul100);      /* quads -> 0..9999 in epi32 (4 per 128b lane) */
        __m256i t3 = _mm256_packus_epi32(t2, zero);      /* [q0,q1,q2,q3, 0,0,0,0] in epi16 per 128b lane */
        __m256i t4 = _mm256_madd_epi16(t3, mul10000);    /* [num0, num1, 0, 0 | num2, num3, 0, 0] in epi32 */
        /* Widen low 2 u32s of each 128b lane to u64 via shuffle + add */
        __m128i lo = _mm256_castsi256_si128(t4);
        __m128i hi = _mm256_extracti128_si256(t4, 1);
        acc = _mm256_add_epi64(acc, _mm256_add_epi64(_mm256_cvtepu32_epi64(lo),
                                                     _mm256_cvtepu32_epi64(hi)));
    }
    uint64_t lanes[4];
    _mm256_storeu_si256((__m256i *)lanes, acc);
    return lanes[0] + lanes[1];
}

static void run_case_ascii_digits(FILE *csv)
{
    const size_t COUNT = 1u << 20; /* 1,048,576 8-digit numbers = 8 MiB */
    char *buf = bench_alloc_aligned(64, COUNT * 8u);
    uint64_t seed = 0x13579bdf2468ace0ULL;
    for (size_t i = 0; i < COUNT; ++i) {
        uint32_t v = (uint32_t)(splitmix64(&seed) % 100000000u);
        for (int k = 7; k >= 0; --k) {
            buf[i * 8u + (size_t)k] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }

    bench_print_section("5. 8-Digit ASCII Integer Parsing (1M ints): Scalar vs Lemire SWAR vs AVX2 vpmaddubsw",
                        "Daniel Lemire & Wojciech Mula (Fast SIMD Timestamp / Integer Parsing)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = parse8_scalar_loop(buf, COUNT);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "simd_avx", "parse_digits", "Scalar v = v*10 + (s[k]-'0') Loop", b_ns, b_cyc, COUNT, base, chk1, "Mi/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = parse8_lemire_swar(buf, COUNT);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "simd_avx", "parse_digits", "Lemire 64-bit SWAR (3 Multiplies)", b_ns, b_cyc, COUNT, base, chk2, "Mi/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk3 = parse8_lemire_avx2(buf, COUNT);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk3);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "simd_avx", "parse_digits", "Lemire AVX2 vpmaddubsw+vpmaddwd (4x8)", b_ns, b_cyc, COUNT, base, chk3, "Mi/s", 1.0);

    free(buf);
}

/* =========================================================================
 * 6. Bitset Popcount (512 KB Bitset = 4.19M bits):
 *    Byte-LUT vs Hardware 4x popcntq vs AVX2 vpshufb+vpsadbw vs AVX-512 vpopcntq
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static uint64_t popcnt_byte_lut(const uint8_t *bytes, const uint8_t lut[256], size_t nbytes)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < nbytes; ++i)
        sum += lut[bytes[i]];
    return sum;
}

BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static uint64_t popcnt_u64_unrolled4(const uint64_t *words, size_t nwords)
{
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (size_t i = 0; i < nwords; i += 4) {
        s0 += (uint64_t)__builtin_popcountll(words[i + 0]);
        s1 += (uint64_t)__builtin_popcountll(words[i + 1]);
        s2 += (uint64_t)__builtin_popcountll(words[i + 2]);
        s3 += (uint64_t)__builtin_popcountll(words[i + 3]);
    }
    return (s0 + s1) + (s2 + s3);
}

/* Wojciech Mula & Daniel Lemire AVX2 vpshufb + vpsadbw popcount */
BENCH_NOINLINE static uint64_t popcnt_mula_lemire_avx2(const uint64_t *words, size_t nwords)
{
    const __m256i lut = _mm256_setr_epi8(
        0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,
        0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4);
    const __m256i low_mask = _mm256_set1_epi8(0x0F);
    const __m256i zero     = _mm256_setzero_si256();
    __m256i total = _mm256_setzero_si256();

    const uint8_t *p = (const uint8_t *)words;
    size_t nbytes = nwords * 8u;
    size_t i = 0;
    while (i + 256 <= nbytes) {
        __m256i local = _mm256_setzero_si256();
        for (int k = 0; k < 8; ++k, i += 32) {
            __m256i v  = _mm256_load_si256((const __m256i *)(p + i));
            __m256i lo = _mm256_and_si256(v, low_mask);
            __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), low_mask);
            local = _mm256_add_epi8(local, _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo),
                                                           _mm256_shuffle_epi8(lut, hi)));
        }
        total = _mm256_add_epi64(total, _mm256_sad_epu8(local, zero));
    }
    uint64_t lanes[4];
    _mm256_storeu_si256((__m256i *)lanes, total);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512F__)
BENCH_NOINLINE static uint64_t popcnt_avx512_vpopcntq(const uint64_t *words, size_t nwords)
{
    __m512i acc0 = _mm512_setzero_si512();
    __m512i acc1 = _mm512_setzero_si512();
    for (size_t i = 0; i < nwords; i += 16) {
        acc0 = _mm512_add_epi64(acc0, _mm512_popcnt_epi64(_mm512_load_si512(words + i)));
        acc1 = _mm512_add_epi64(acc1, _mm512_popcnt_epi64(_mm512_load_si512(words + i + 8)));
    }
    return (uint64_t)_mm512_reduce_add_epi64(_mm512_add_epi64(acc0, acc1));
}
#endif

static void run_case_popcount(FILE *csv)
{
    const size_t NWORDS = 1u << 16; /* 65,536 u64 words = 512 KiB (L2 resident!) */
    uint64_t *words = bench_alloc_aligned(64, NWORDS * sizeof(uint64_t));
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint8_t)__builtin_popcount((unsigned)i);
    uint64_t seed = 0xabcdef1234567890ULL;
    for (size_t i = 0; i < NWORDS; ++i) words[i] = splitmix64(&seed);

    bench_print_section("6. Bitset Popcount (512 KB = 4.19M bits): Byte-LUT vs 4x POPCNTQ vs AVX2 Mula vs AVX-512",
                        "Wojciech Mula, Nathan Kurz & Daniel Lemire ('Faster Population Counts'), mu_bitset");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = popcnt_byte_lut((const uint8_t *)words, lut, NWORDS * 8u);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "simd_avx", "popcount", "Scalar 256-Byte Lookup Table", b_ns, b_cyc, NWORDS * 8u, base, chk1, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = popcnt_u64_unrolled4(words, NWORDS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "simd_avx", "popcount", "4x Unrolled Scalar Hardware popcntq", b_ns, b_cyc, NWORDS * 8u, base, chk2, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk3 = popcnt_mula_lemire_avx2(words, NWORDS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk3);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "simd_avx", "popcount", "Mula-Kurz-Lemire AVX2 vpshufb+vpsadbw", b_ns, b_cyc, NWORDS * 8u, base, chk3, "MB/s", 1.0);

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512F__)
    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    uint64_t chk4 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk4 = popcnt_avx512_vpopcntq(words, NWORDS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk4);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "simd_avx", "popcount", "AVX-512 Hardware vpopcntq (1024b/iter)", b_ns, b_cyc, NWORDS * 8u, base, chk4, "MB/s", 1.0);
#endif

    free(words);
}

/* =========================================================================
 * 7. Fabian Giesen In-Register 8x8 Float SIMD Transpose (AoS -> SoA Staging)
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static void transpose8_aos_to_soa_scalar(const float * BENCH_RESTRICT aos, float * BENCH_RESTRICT soa, size_t nblocks)
{
    for (size_t b = 0; b < nblocks; ++b) {
        const float *src = aos + b * 64u;
        float *dst = soa + b * 64u;
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c)
                dst[c * 8 + r] = src[r * 8 + c];
    }
}

BENCH_NOINLINE static void transpose8_aos_to_soa_avx2(const float * BENCH_RESTRICT aos, float * BENCH_RESTRICT soa, size_t nblocks)
{
    for (size_t b = 0; b < nblocks; ++b) {
        const float *src = aos + b * 64u;
        float *dst = soa + b * 64u;

        __m256 r0 = _mm256_load_ps(src +  0), r1 = _mm256_load_ps(src +  8);
        __m256 r2 = _mm256_load_ps(src + 16), r3 = _mm256_load_ps(src + 24);
        __m256 r4 = _mm256_load_ps(src + 32), r5 = _mm256_load_ps(src + 40);
        __m256 r6 = _mm256_load_ps(src + 48), r7 = _mm256_load_ps(src + 56);

        __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpackhi_ps(r0, r1);
        __m256 t2 = _mm256_unpacklo_ps(r2, r3), t3 = _mm256_unpackhi_ps(r2, r3);
        __m256 t4 = _mm256_unpacklo_ps(r4, r5), t5 = _mm256_unpackhi_ps(r4, r5);
        __m256 t6 = _mm256_unpacklo_ps(r6, r7), t7 = _mm256_unpackhi_ps(r6, r7);

        __m256 s0 = _mm256_shuffle_ps(t0, t2, 0x44), s1 = _mm256_shuffle_ps(t0, t2, 0xEE);
        __m256 s2 = _mm256_shuffle_ps(t1, t3, 0x44), s3 = _mm256_shuffle_ps(t1, t3, 0xEE);
        __m256 s4 = _mm256_shuffle_ps(t4, t6, 0x44), s5 = _mm256_shuffle_ps(t4, t6, 0xEE);
        __m256 s6 = _mm256_shuffle_ps(t5, t7, 0x44), s7 = _mm256_shuffle_ps(t5, t7, 0xEE);

        _mm256_store_ps(dst +  0, _mm256_permute2f128_ps(s0, s4, 0x20));
        _mm256_store_ps(dst +  8, _mm256_permute2f128_ps(s1, s5, 0x20));
        _mm256_store_ps(dst + 16, _mm256_permute2f128_ps(s2, s6, 0x20));
        _mm256_store_ps(dst + 24, _mm256_permute2f128_ps(s3, s7, 0x20));
        _mm256_store_ps(dst + 32, _mm256_permute2f128_ps(s0, s4, 0x31));
        _mm256_store_ps(dst + 40, _mm256_permute2f128_ps(s1, s5, 0x31));
        _mm256_store_ps(dst + 48, _mm256_permute2f128_ps(s2, s6, 0x31));
        _mm256_store_ps(dst + 56, _mm256_permute2f128_ps(s3, s7, 0x31));
    }
}

static void run_case_transpose(FILE *csv)
{
    const size_t NBLOCKS = 256u;  /* 256 8x8 blocks = 64 KB staging tile (L2-resident, ISPC/Oodle pattern!) */
    const size_t PASSES  = 256u;
    const size_t NFLOATS = NBLOCKS * 64u;
    float *aos = bench_alloc_aligned(64, NFLOATS * sizeof(float));
    float *soa1 = bench_alloc_aligned(64, NFLOATS * sizeof(float));
    float *soa2 = bench_alloc_aligned(64, NFLOATS * sizeof(float));
    for (size_t i = 0; i < NFLOATS; ++i) aos[i] = (float)(i & 255u);

    bench_print_section("7. L2-Resident AoS->SoA 8x8 Float Staging Transpose (64 KB x 256): Scalar vs AVX2 Shuffle",
                        "Fabian Giesen (SIMD Transposes), Intel ISPC aos_to_soa, id Software");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t p = 0; p < PASSES; ++p) {
            transpose8_aos_to_soa_scalar(aos, soa1, NBLOCKS);
            DO_NOT_OPTIMIZE(soa1[NFLOATS - 1]);
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    uint64_t chk1 = (uint64_t)(soa1[1] + soa1[8] + soa1[63] + soa1[NFLOATS - 9]);
    double base = (double)b_ns;
    bench_print_row(csv, "simd_avx", "transpose_8x8", "Scalar Strided 8x8 Transpose", b_ns, b_cyc, NFLOATS * PASSES, base, chk1, "Mf/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t p = 0; p < PASSES; ++p) {
            transpose8_aos_to_soa_avx2(aos, soa2, NBLOCKS);
            DO_NOT_OPTIMIZE(soa2[NFLOATS - 1]);
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    uint64_t chk2 = (uint64_t)(soa2[1] + soa2[8] + soa2[63] + soa2[NFLOATS - 9]);
    bench_print_row(csv, "simd_avx", "transpose_8x8", "In-Register AVX2 8x8 Shuffle Transpose", b_ns, b_cyc, NFLOATS * PASSES, base, chk2, "Mf/s", 1.0);

    free(aos); free(soa1); free(soa2);
}

int main(void)
{
    printf("====================================================================================\n");
    printf("  SUITE 3: DESKTOP x86-64 FULL SIMD LEVERAGE (AVX2 / AVX-512 / PCLMULQDQ) PROOFS\n");
    printf("====================================================================================\n");

    FILE *csv = fopen("results_proofs.csv", "a");
    run_case_frustum_cull(csv);
    run_case_vpshufb_classify(csv);
    run_case_pclmul_quotes(csv);
    run_case_stream_compaction(csv);
    run_case_ascii_digits(csv);
    run_case_popcount(csv);
    run_case_transpose(csv);
    if (csv) fclose(csv);
    return 0;
}
