/*
 * bench_advanced_c_isa.c — Suite 6: Compiler/Loop Blocking, Hardware AES/CRC32/GFNI,
 *                          StreamVByte SIMD Compression, Bytecode Threading & Xor8 Filter
 * =======================================================================================
 * Single-threaded advanced C & desktop x86-64 ISA proofs (kept separate from multithreading):
 *   1. Loop Interchange & Cache/Register Tiling (512x512 FP32 Matrix Multiply):
 *      Naive i-j-k vs Interchanged i-k-j vs L1-Blocked + 4x16 AVX2 FMA Micro-Kernel (GotoBLAS/BLIS)
 *   2. Fast Desktop Hashing (64-Byte Keys & Records):
 *      FNV-1a Byte Loop vs wyhash (128b mul) vs 3-Lane CRC32C vs 4-Lane AES-NI (gxhash/MeowHash)
 *   3. Variable-Length u32 Compression (1M Netcode/Mesh/Posting Deltas):
 *      Protobuf LEB128 (0x80 branch/byte) vs Daniel Lemire StreamVByte (2-bit ctrl + pshufb LUT)
 *   4. Bytecode / Interpreter Dispatch (4M VM Ops across 16 Opcodes):
 *      Classic `while + switch(*ip++)` (1 shared BTB entry) vs Direct-Threaded Computed `goto *`
 *   5. Immutable Filter Membership (524K Keys, 0.39% FP):
 *      Classic 8-Probe Bloom Filter vs Daniel Lemire Static Xor8 Filter (9.8 bits/key, 3 MLP loads)
 *   6. Sub-Byte Bit-Matrix Transform / Byte Bit-Reversal (4 MB):
 *      Scalar Bit-Reverse vs AVX2 Nibble vpshufb vs Hardware GFNI `_mm256_gf2p8affine_epi64_epi8`
 */

#include "bench_common.h"
#include <wmmintrin.h> /* AES-NI */
#include <nmmintrin.h> /* SSE4.2 CRC32C */

#define REPS 7

/* =========================================================================
 * 1. 512x512 FP32 GEMM: i-j-k vs i-k-j vs L1-Tiled + 4x16 AVX2 FMA Micro-Kernel
 * ========================================================================= */
/* Disable automatic loop-interchange on the naive baseline so we actually measure i-j-k vs i-k-j! */
BENCH_NOINLINE __attribute__((optimize("no-loop-interchange,no-tree-vectorize")))
static void gemm_naive_ijk(const float * BENCH_RESTRICT A, const float * BENCH_RESTRICT B,
                           float * BENCH_RESTRICT C, size_t N)
{
    memset(C, 0, N * N * sizeof(float));
    for (size_t i = 0; i < N; ++i) {
        for (size_t j = 0; j < N; ++j) {
            float sum = 0.0f;
            for (size_t k = 0; k < N; ++k)
                sum += A[i * N + k] * B[k * N + j]; /* B jumped by 2048B every step! */
            C[i * N + j] = sum;
        }
    }
}

BENCH_NOINLINE static void gemm_interchanged_ikj(const float * BENCH_RESTRICT A, const float * BENCH_RESTRICT B,
                                                 float * BENCH_RESTRICT C, size_t N)
{
    memset(C, 0, N * N * sizeof(float));
    for (size_t i = 0; i < N; ++i) {
        for (size_t k = 0; k < N; ++k) {
            float aik = A[i * N + k];
            for (size_t j = 0; j < N; ++j)
                C[i * N + j] += aik * B[k * N + j]; /* Both B and C unit-stride! */
        }
    }
}

/* BLIS / GotoBLAS style L1-blocked + 4x16 (8 YMM accumulators!) AVX2 FMA Register Micro-Kernel */
BENCH_NOINLINE static void gemm_tiled_avx2_microkernel(const float * BENCH_RESTRICT A, const float * BENCH_RESTRICT B,
                                                       float * BENCH_RESTRICT C, size_t N)
{
    memset(C, 0, N * N * sizeof(float));
    const size_t KC = 64; /* 64 x 16 floats of B = 4 KB -> stays locked in L1d! */
    for (size_t kk = 0; kk < N; kk += KC) {
        for (size_t i = 0; i < N; i += 4) {
            for (size_t j = 0; j < N; j += 16) {
                __m256 c00 = _mm256_load_ps(&C[(i + 0) * N + j + 0]);
                __m256 c01 = _mm256_load_ps(&C[(i + 0) * N + j + 8]);
                __m256 c10 = _mm256_load_ps(&C[(i + 1) * N + j + 0]);
                __m256 c11 = _mm256_load_ps(&C[(i + 1) * N + j + 8]);
                __m256 c20 = _mm256_load_ps(&C[(i + 2) * N + j + 0]);
                __m256 c21 = _mm256_load_ps(&C[(i + 2) * N + j + 8]);
                __m256 c30 = _mm256_load_ps(&C[(i + 3) * N + j + 0]);
                __m256 c31 = _mm256_load_ps(&C[(i + 3) * N + j + 8]);

                for (size_t k = kk; k < kk + KC; ++k) {
                    __m256 b0 = _mm256_load_ps(&B[k * N + j + 0]);
                    __m256 b1 = _mm256_load_ps(&B[k * N + j + 8]);

                    __m256 a0 = _mm256_set1_ps(A[(i + 0) * N + k]);
                    c00 = _mm256_fmadd_ps(a0, b0, c00);
                    c01 = _mm256_fmadd_ps(a0, b1, c01);

                    __m256 a1 = _mm256_set1_ps(A[(i + 1) * N + k]);
                    c10 = _mm256_fmadd_ps(a1, b0, c10);
                    c11 = _mm256_fmadd_ps(a1, b1, c11);

                    __m256 a2 = _mm256_set1_ps(A[(i + 2) * N + k]);
                    c20 = _mm256_fmadd_ps(a2, b0, c20);
                    c21 = _mm256_fmadd_ps(a2, b1, c21);

                    __m256 a3 = _mm256_set1_ps(A[(i + 3) * N + k]);
                    c30 = _mm256_fmadd_ps(a3, b0, c30);
                    c31 = _mm256_fmadd_ps(a3, b1, c31);
                }

                _mm256_store_ps(&C[(i + 0) * N + j + 0], c00);
                _mm256_store_ps(&C[(i + 0) * N + j + 8], c01);
                _mm256_store_ps(&C[(i + 1) * N + j + 0], c10);
                _mm256_store_ps(&C[(i + 1) * N + j + 8], c11);
                _mm256_store_ps(&C[(i + 2) * N + j + 0], c20);
                _mm256_store_ps(&C[(i + 2) * N + j + 8], c21);
                _mm256_store_ps(&C[(i + 3) * N + j + 0], c30);
                _mm256_store_ps(&C[(i + 3) * N + j + 8], c31);
            }
        }
    }
}

static uint64_t checksum_mat(const float *C, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n * n; i += 97) s += (double)C[i];
    return (uint64_t)(s + 0.5);
}

static void run_case_gemm(FILE *csv)
{
    const size_t N = 256; /* 256^3 * 2 = 33.55M FLOPs; fits cleanly & fast in repetition loop */
    const size_t FLOPS = 2ULL * N * N * N;
    float *A = bench_alloc_aligned(64, N * N * sizeof(float));
    float *B = bench_alloc_aligned(64, N * N * sizeof(float));
    float *C = bench_alloc_aligned(64, N * N * sizeof(float));

    uint64_t seed = 0x1020304050607080ULL;
    for (size_t i = 0; i < N * N; ++i) {
        A[i] = (float)(splitmix64(&seed) & 255u) * (1.0f / 256.0f);
        B[i] = (float)(splitmix64(&seed) & 255u) * (1.0f / 256.0f);
    }

    bench_print_section("1. Loop Order & Register Blocking (256x256 GEMM = 33.5M FLOPs): i-j-k vs i-k-j vs 4x16 AVX2",
                        "Algorithmica, Agner Fog, GotoBLAS / BLIS Register Micro-Kernel");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0, chk3 = 0;
    for (int r = 0; r < 3; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        gemm_naive_ijk(A, B, C, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        chk1 = checksum_mat(C, N);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "advanced_c_isa", "gemm_tiling", "Naive i-j-k (Strided B + 1 Accum)", b_ns, b_cyc, FLOPS, base, chk1, "MFLOP/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        gemm_interchanged_ikj(A, B, C, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        chk2 = checksum_mat(C, N);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "gemm_tiling", "Loop-Interchanged i-k-j (2 Lines Swapped)", b_ns, b_cyc, FLOPS, base, chk2, "MFLOP/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        gemm_tiled_avx2_microkernel(A, B, C, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        chk3 = checksum_mat(C, N);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "gemm_tiling", "L1-Tiled + 4x16 AVX2 FMA Micro-Kernel", b_ns, b_cyc, FLOPS, base, chk3, "MFLOP/s", 1.0);

    free(A); free(B); free(C);
}

/* =========================================================================
 * 2. High-Throughput Desktop Hashing (4 MB Stream):
 *    FNV-1a Byte Loop vs wyhash 128b Multiply vs SSE4.2 CRC32C vs AES-NI (Meow/gxhash)
 * ========================================================================= */
BENCH_NOINLINE static uint64_t hash_fnv1a(const uint8_t *buf, size_t len)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)buf[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static inline uint64_t wymum(uint64_t a, uint64_t b)
{
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

BENCH_NOINLINE static uint64_t hash_wyhash_stream(const uint8_t *buf, size_t len)
{
    uint64_t s0 = 0xa0761d6478bd642fULL, s1 = 0xe7037ed1a0b428dbULL;
    for (size_t i = 0; i < len; i += 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, buf + i +  0, 8); memcpy(&w1, buf + i +  8, 8);
        memcpy(&w2, buf + i + 16, 8); memcpy(&w3, buf + i + 24, 8);
        s0 = wymum(w0 ^ s0, w1 ^ 0x8ebc6af09c88c6e3ULL);
        s1 = wymum(w2 ^ s1, w3 ^ 0x589965cc75374cc3ULL);
    }
    return wymum(s0, s1 ^ len);
}

/* 3-way parallel hardware CRC32C (`crc32q`: 3-cycle latency, 1-cycle throughput!) */
BENCH_NOINLINE static uint64_t hash_crc32c_ilp3(const uint8_t *buf, size_t len)
{
    uint64_t c0 = 0xFFFFFFFFULL, c1 = 0x12345678ULL, c2 = 0x9ABCDEF0ULL;
    for (size_t i = 0; i + 24 <= len; i += 24) {
        uint64_t w0, w1, w2;
        memcpy(&w0, buf + i +  0, 8);
        memcpy(&w1, buf + i +  8, 8);
        memcpy(&w2, buf + i + 16, 8);
        c0 = _mm_crc32_u64(c0, w0);
        c1 = _mm_crc32_u64(c1, w1);
        c2 = _mm_crc32_u64(c2, w2);
    }
    return (c0 << 32) ^ (c1 << 16) ^ c2;
}

/* 4-lane hardware AES-NI (`aesenc`: Casey Muratori / Jeff Roberts MeowHash / Rust gxhash pattern!) */
BENCH_NOINLINE static uint64_t hash_aesni_4lane(const uint8_t *buf, size_t len)
{
    __m128i s0 = _mm_set1_epi64x(0x9e3779b97f4a7c15LL);
    __m128i s1 = _mm_set1_epi64x(0xbf58476d1ce4e5b9LL);
    __m128i s2 = _mm_set1_epi64x(0x94d049bb133111ebLL);
    __m128i s3 = _mm_set1_epi64x(0x6c62272e07bb0142LL);
    for (size_t i = 0; i < len; i += 64) {
        s0 = _mm_aesenc_si128(s0, _mm_load_si128((const __m128i *)(buf + i +  0)));
        s1 = _mm_aesenc_si128(s1, _mm_load_si128((const __m128i *)(buf + i + 16)));
        s2 = _mm_aesenc_si128(s2, _mm_load_si128((const __m128i *)(buf + i + 32)));
        s3 = _mm_aesenc_si128(s3, _mm_load_si128((const __m128i *)(buf + i + 48)));
    }
    __m128i m = _mm_aesenc_si128(_mm_aesenc_si128(s0, s1), _mm_aesenc_si128(s2, s3));
    return (uint64_t)_mm_cvtsi128_si64(m) ^ (uint64_t)_mm_extract_epi64(m, 1);
}

static void run_case_hashing(FILE *csv)
{
    const size_t LEN = 1u << 20; /* 1 MiB L2-resident asset/string/buffer hash */
    uint8_t *buf = bench_alloc_aligned(64, LEN);
    uint64_t seed = 0xfeedfacecafebabeULL;
    for (size_t i = 0; i < LEN; i += 8) {
        uint64_t w = splitmix64(&seed);
        memcpy(buf + i, &w, 8);
    }

    bench_print_section("2. High-Speed Buffer/String Hashing (1 MB): FNV-1a vs wyhash vs 3x CRC32C vs 4x AES-NI",
                        "Casey Muratori (MeowHash), Wang Yi (wyhash), Abseil/ClickHouse CRC32C, gxhash/ahash");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk = hash_fnv1a(buf, LEN);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "advanced_c_isa", "fast_hashing", "Scalar FNV-1a (Serial 1B/3-cyc mul)", b_ns, b_cyc, LEN, base, chk & 0xFFFFFFFFu, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk = hash_crc32c_ilp3(buf, LEN);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "fast_hashing", "Hardware SSE4.2 CRC32C (3-Lane ILP)", b_ns, b_cyc, LEN, base, chk & 0xFFFFFFFFu, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk = hash_wyhash_stream(buf, LEN);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "fast_hashing", "wyhash 128-Bit Multiply-Mix (2-Lane)", b_ns, b_cyc, LEN, base, chk & 0xFFFFFFFFu, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk = hash_aesni_4lane(buf, LEN);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "fast_hashing", "Hardware AES-NI _mm_aesenc (4-Lane)", b_ns, b_cyc, LEN, base, chk & 0xFFFFFFFFu, "MB/s", 1.0);

    free(buf);
}

/* =========================================================================
 * 3. Variable-Length Integer Compression (1M u32s):
 *    Protobuf LEB128 (`0x80` branch per byte) vs Daniel Lemire `StreamVByte` (`pshufb` LUT)
 * ========================================================================= */
static size_t leb128_encode(const uint32_t *in, size_t n, uint8_t *out)
{
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = in[i];
        while (v >= 0x80u) {
            out[pos++] = (uint8_t)((v & 0x7Fu) | 0x80u);
            v >>= 7;
        }
        out[pos++] = (uint8_t)v;
    }
    return pos;
}

BENCH_NOINLINE static uint64_t leb128_decode_sum(const uint8_t *in, size_t n)
{
    size_t pos = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = 0, shift = 0;
        for (;;) {
            uint8_t b = in[pos++];
            v |= (uint32_t)(b & 0x7Fu) << shift;
            if ((b & 0x80u) == 0) break;
            shift += 7;
        }
        sum += v;
    }
    return sum;
}

static __m128i g_svb_shuf_lut[256];
static uint8_t g_svb_len_lut[256];

static void streamvbyte_init_tables(void)
{
    for (uint32_t code = 0; code < 256; ++code) {
        uint8_t shuf[16];
        uint8_t offset = 0;
        for (uint32_t k = 0; k < 4; ++k) {
            uint32_t len = ((code >> (k * 2)) & 3u) + 1u; /* 1..4 bytes */
            for (uint32_t b = 0; b < 4; ++b)
                shuf[k * 4 + b] = (b < len) ? offset++ : 0x80u; /* 0x80 zeros byte in pshufb! */
        }
        memcpy(&g_svb_shuf_lut[code], shuf, 16);
        g_svb_len_lut[code] = offset;
    }
}

static size_t streamvbyte_encode(const uint32_t *in, size_t n, uint8_t *ctrl_out, uint8_t *data_out)
{
    size_t dpos = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint8_t ctrl = 0;
        for (uint32_t k = 0; k < 4; ++k) {
            uint32_t v = in[i + k];
            uint32_t len = (v < (1u << 8)) ? 1u : (v < (1u << 16)) ? 2u : (v < (1u << 24)) ? 3u : 4u;
            ctrl |= (uint8_t)((len - 1u) << (k * 2));
            memcpy(data_out + dpos, &v, 4);
            dpos += len;
        }
        ctrl_out[i >> 2] = ctrl;
    }
    return dpos;
}

BENCH_NOINLINE static uint64_t streamvbyte_decode_simd_sum(const uint8_t * BENCH_RESTRICT ctrl,
                                                           const uint8_t * BENCH_RESTRICT data, size_t n)
{
    __m256i acc = _mm256_setzero_si256();
    const uint8_t *dp = data;
    size_t ngroups = n >> 2;
    for (size_t g = 0; g < ngroups; ++g) {
        uint8_t c = ctrl[g];
        __m128i raw  = _mm_loadu_si128((const __m128i *)dp);
        __m128i vals = _mm_shuffle_epi8(raw, g_svb_shuf_lut[c]); /* 4 varints decoded in 1 pshufb! */
        dp += g_svb_len_lut[c];
        acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(vals));
    }
    uint64_t lanes[4];
    _mm256_storeu_si256((__m256i *)lanes, acc);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

static void run_case_streamvbyte(FILE *csv)
{
    streamvbyte_init_tables();
    const size_t N = 1u << 20; /* 1,048,576 compressed u32 deltas */
    uint32_t *src  = bench_alloc_aligned(64, N * sizeof(uint32_t));
    uint8_t  *leb  = bench_alloc_aligned(64, N * 5u + 64u);
    uint8_t  *ctrl = bench_alloc_aligned(64, (N / 4u) + 64u);
    uint8_t  *svbd = bench_alloc_aligned(64, N * 4u + 64u);

    uint64_t seed = 0x55aa55aa12345678ULL;
    for (size_t i = 0; i < N; ++i) {
        uint64_t r = splitmix64(&seed);
        /* Realistic mesh/posting/netcode delta distribution: mix of 1B, 2B, 3B, 4B values */
        uint32_t bucket = (uint32_t)(r & 3u);
        src[i] = (bucket == 0) ? (uint32_t)(r & 0x7Fu)
               : (bucket == 1) ? (uint32_t)(r & 0x3FFFu)
               : (bucket == 2) ? (uint32_t)(r & 0x1FFFFFu)
               :                 (uint32_t)(r & 0x0FFFFFFFu);
    }
    leb128_encode(src, N, leb);
    streamvbyte_encode(src, N, ctrl, svbd);

    bench_print_section("3. Variable-Length u32 Decode (1M Deltas): Protobuf LEB128 vs Lemire SIMD StreamVByte",
                        "Daniel Lemire ('StreamVByte: Faster Byte-Oriented Integer Compression'), Jeff Dean");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = leb128_decode_sum(leb, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "advanced_c_isa", "streamvbyte", "Protobuf LEB128 (0x80 Branch / Byte)", b_ns, b_cyc, N, base, chk1, "Mi/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = streamvbyte_decode_simd_sum(ctrl, svbd, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "streamvbyte", "Lemire StreamVByte (2b Ctrl + pshufb)", b_ns, b_cyc, N, base, chk2, "Mi/s", 1.0);

    free(src); free(leb); free(ctrl); free(svbd);
}

/* =========================================================================
 * 4. Bytecode / Interpreter Dispatch (2M Ops across 16 Opcodes):
 *    Classic `while + switch(*ip++)` (1 Shared Indirect Jump BTB!) vs
 *    Direct-Threaded Computed `goto *dispatch[*ip++]` (16 Separate BTB Predictors!)
 * ========================================================================= */
enum {
    OP_ADD1 = 0, OP_ADD3, OP_SUB1, OP_XOR5,
    OP_ROL3,     OP_ROR1, OP_MUL3, OP_ADD7,
    OP_SUB2,     OP_XOR9, OP_ADD11,OP_SHL1,
    OP_SHR1,     OP_ADDI, OP_XORI, OP_HALT = 15
};

BENCH_NOINLINE static uint64_t vm_run_switch(const uint8_t * BENCH_RESTRICT code)
{
    const uint8_t *ip = code;
    uint64_t acc = 1;
    for (;;) {
        switch (*ip++) {
            case OP_ADD1:  acc += 1; break;
            case OP_ADD3:  acc += 3; break;
            case OP_SUB1:  acc -= 1; break;
            case OP_XOR5:  acc ^= 5; break;
            case OP_ROL3:  acc = (acc << 3) | (acc >> 61); break;
            case OP_ROR1:  acc = (acc >> 1) | (acc << 63); break;
            case OP_MUL3:  acc = acc * 3u + 1u; break;
            case OP_ADD7:  acc += 7; break;
            case OP_SUB2:  acc -= 2; break;
            case OP_XOR9:  acc ^= 9; break;
            case OP_ADD11: acc += 11; break;
            case OP_SHL1:  acc = (acc << 1) ^ 1u; break;
            case OP_SHR1:  acc = (acc >> 1) + 5u; break;
            case OP_ADDI:  acc += *ip++; break;
            case OP_XORI:  acc ^= *ip++; break;
            case OP_HALT:  return acc;
        }
    }
}

/* Disable GCC jump-threading cross-merging (`no-gcse,no-crossjumping`) so GCC does NOT
 * collapse all 16 `goto *` tails back into 1 shared indirect jump! */
BENCH_NOINLINE __attribute__((optimize("no-gcse,no-crossjumping")))
static uint64_t vm_run_computed_goto(const uint8_t * BENCH_RESTRICT code)
{
    static const void * const table[16] = {
        &&L_ADD1, &&L_ADD3, &&L_SUB1,  &&L_XOR5,
        &&L_ROL3, &&L_ROR1, &&L_MUL3,  &&L_ADD7,
        &&L_SUB2, &&L_XOR9, &&L_ADD11, &&L_SHL1,
        &&L_SHR1, &&L_ADDI, &&L_XORI,  &&L_HALT
    };
    const uint8_t *ip = code;
    uint64_t acc = 1;
    #define DISPATCH() goto *table[*ip++]

    DISPATCH();
    L_ADD1:  acc += 1;                        DISPATCH();
    L_ADD3:  acc += 3;                        DISPATCH();
    L_SUB1:  acc -= 1;                        DISPATCH();
    L_XOR5:  acc ^= 5;                        DISPATCH();
    L_ROL3:  acc = (acc << 3) | (acc >> 61);  DISPATCH();
    L_ROR1:  acc = (acc >> 1) | (acc << 63);  DISPATCH();
    L_MUL3:  acc = acc * 3u + 1u;             DISPATCH();
    L_ADD7:  acc += 7;                        DISPATCH();
    L_SUB2:  acc -= 2;                        DISPATCH();
    L_XOR9:  acc ^= 9;                        DISPATCH();
    L_ADD11: acc += 11;                       DISPATCH();
    L_SHL1:  acc = (acc << 1) ^ 1u;           DISPATCH();
    L_SHR1:  acc = (acc >> 1) + 5u;           DISPATCH();
    L_ADDI:  acc += *ip++;                    DISPATCH();
    L_XORI:  acc ^= *ip++;                    DISPATCH();
    L_HALT:  return acc;
    #undef DISPATCH
}

static void run_case_vm_dispatch(FILE *csv)
{
    const size_t NOPS = 1u << 21; /* 2.1M bytecode instructions with realistic idiom bigrams */
    uint8_t *code = bench_alloc_aligned(64, NOPS * 2u + 16u);
    uint64_t seed = 0x9988776655443322ULL;
    size_t pos = 0;
    for (size_t i = 0; i < NOPS; ) {
        /* Realistic bytecode idioms (2-4 op sequences like LOAD_IMM -> MUL3 -> ADD1) */
        uint32_t pat = (uint32_t)(splitmix64(&seed) & 7u);
        if (pat == 0)      { code[pos++] = OP_ADDI; code[pos++] = 13; code[pos++] = OP_MUL3; code[pos++] = OP_ADD1; i += 3; }
        else if (pat == 1) { code[pos++] = OP_XORI; code[pos++] = 42; code[pos++] = OP_ROL3; code[pos++] = OP_SUB1; i += 3; }
        else if (pat == 2) { code[pos++] = OP_ADD3; code[pos++] = OP_SHL1; code[pos++] = OP_XOR5; i += 3; }
        else if (pat == 3) { code[pos++] = OP_ADD7; code[pos++] = OP_ROR1; code[pos++] = OP_SUB2; i += 3; }
        else               { code[pos++] = (uint8_t)(pat + 4u); ++i; }
    }
    code[pos++] = OP_HALT;

    bench_print_section("4. Bytecode VM Dispatch (2.1M Ops, 16 Opcodes): switch(*ip++) vs Direct Computed Goto",
                        "LuaJIT, CPython 3.13+, SQLite, Josh Haberman (upb), GCC -fno-gcse BTB Rule");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = vm_run_switch(code);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "advanced_c_isa", "vm_dispatch", "while + switch(*ip++) (1 Shared BTB)", b_ns, b_cyc, NOPS, base, chk1, "Mo/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = vm_run_computed_goto(code);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "vm_dispatch", "Computed goto *table[*ip++] (-fno-gcse)", b_ns, b_cyc, NOPS, base, chk2, "Mo/s", 1.0);

    free(code);
}

/* =========================================================================
 * 5. Static Set Filter (524K Keys, ~0.39% FP):
 *    Classic 8-Probe Bloom Filter (12 bits/key = 768 KB, 8 probes) vs
 *    Daniel Lemire Static Xor8 Filter (9.84 bits/key = 645 KB, 3 MLP loads!)
 * ========================================================================= */
typedef struct {
    uint64_t seed;
    uint32_t block_len;
    uint8_t *fp; /* 3 * block_len bytes */
} Xor8Filter;

static inline uint64_t xor_mix(uint64_t k)
{
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    return k ^ (k >> 33);
}

static inline uint32_t xor_rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

static bool xor8_build(Xor8Filter *f, const uint64_t *keys, size_t n)
{
    uint32_t cap = (uint32_t)((double)n * 1.23) + 32u;
    uint32_t bl  = cap / 3u;
    cap = bl * 3u;
    f->block_len = bl;
    f->fp = calloc(cap, 1);

    uint64_t *xormask = calloc(cap, sizeof(uint64_t));
    uint32_t *count   = calloc(cap, sizeof(uint32_t));
    uint32_t *q0 = malloc(bl * sizeof(uint32_t));
    uint32_t *q1 = malloc(bl * sizeof(uint32_t));
    uint32_t *q2 = malloc(bl * sizeof(uint32_t));
    uint64_t *stack_h = malloc(n * sizeof(uint64_t));
    uint32_t *stack_i = malloc(n * sizeof(uint32_t));

    bool ok = false;
    for (uint64_t attempt = 0; attempt < 16 && !ok; ++attempt) {
        f->seed = 0x13579bdf2468ace0ULL + attempt * 0x9e3779b97f4a7c15ULL;
        memset(xormask, 0, cap * sizeof(uint64_t));
        memset(count,   0, cap * sizeof(uint32_t));
        for (size_t i = 0; i < n; ++i) {
            uint64_t h = xor_mix(keys[i] + f->seed);
            uint32_t r0 = (uint32_t)h;
            uint32_t r1 = xor_rotl32((uint32_t)h, 21);
            uint32_t r2 = xor_rotl32((uint32_t)(h >> 32), 21);
            uint32_t i0 = (uint32_t)(((uint64_t)r0 * bl) >> 32);
            uint32_t i1 = (uint32_t)(((uint64_t)r1 * bl) >> 32) + bl;
            uint32_t i2 = (uint32_t)(((uint64_t)r2 * bl) >> 32) + 2u * bl;
            xormask[i0] ^= h; count[i0]++;
            xormask[i1] ^= h; count[i1]++;
            xormask[i2] ^= h; count[i2]++;
        }
        size_t s0 = 0, s1 = 0, s2 = 0, sp = 0;
        for (uint32_t i = 0; i < bl; ++i) {
            if (count[i] == 1)           q0[s0++] = i;
            if (count[bl + i] == 1)      q1[s1++] = bl + i;
            if (count[2u * bl + i] == 1) q2[s2++] = 2u * bl + i;
        }
        while (s0 + s1 + s2 > 0) {
            while (s0 > 0) {
                uint32_t idx = q0[--s0];
                if (count[idx] == 0) continue;
                uint64_t h = xormask[idx];
                stack_h[sp] = h; stack_i[sp++] = idx;
                uint32_t i1 = (uint32_t)(((uint64_t)xor_rotl32((uint32_t)h, 21) * bl) >> 32) + bl;
                uint32_t i2 = (uint32_t)(((uint64_t)xor_rotl32((uint32_t)(h >> 32), 21) * bl) >> 32) + 2u * bl;
                count[idx] = 0;
                xormask[i1] ^= h; if (--count[i1] == 1) q1[s1++] = i1;
                xormask[i2] ^= h; if (--count[i2] == 1) q2[s2++] = i2;
            }
            while (s1 > 0) {
                uint32_t idx = q1[--s1];
                if (count[idx] == 0) continue;
                uint64_t h = xormask[idx];
                stack_h[sp] = h; stack_i[sp++] = idx;
                uint32_t i0 = (uint32_t)(((uint64_t)(uint32_t)h * bl) >> 32);
                uint32_t i2 = (uint32_t)(((uint64_t)xor_rotl32((uint32_t)(h >> 32), 21) * bl) >> 32) + 2u * bl;
                count[idx] = 0;
                xormask[i0] ^= h; if (--count[i0] == 1) q0[s0++] = i0;
                xormask[i2] ^= h; if (--count[i2] == 1) q2[s2++] = i2;
            }
            while (s2 > 0) {
                uint32_t idx = q2[--s2];
                if (count[idx] == 0) continue;
                uint64_t h = xormask[idx];
                stack_h[sp] = h; stack_i[sp++] = idx;
                uint32_t i0 = (uint32_t)(((uint64_t)(uint32_t)h * bl) >> 32);
                uint32_t i1 = (uint32_t)(((uint64_t)xor_rotl32((uint32_t)h, 21) * bl) >> 32) + bl;
                count[idx] = 0;
                xormask[i0] ^= h; if (--count[i0] == 1) q0[s0++] = i0;
                xormask[i1] ^= h; if (--count[i1] == 1) q1[s1++] = i1;
            }
        }
        if (sp == n) {
            memset(f->fp, 0, cap);
            while (sp > 0) {
                --sp;
                uint64_t h = stack_h[sp];
                uint32_t idx = stack_i[sp];
                uint32_t i0 = (uint32_t)(((uint64_t)(uint32_t)h * bl) >> 32);
                uint32_t i1 = (uint32_t)(((uint64_t)xor_rotl32((uint32_t)h, 21) * bl) >> 32) + bl;
                uint32_t i2 = (uint32_t)(((uint64_t)xor_rotl32((uint32_t)(h >> 32), 21) * bl) >> 32) + 2u * bl;
                uint8_t fp8 = (uint8_t)(h ^ (h >> 32));
                f->fp[idx] = fp8 ^ f->fp[i0] ^ f->fp[i1] ^ f->fp[i2];
            }
            ok = true;
        }
    }
    free(xormask); free(count); free(q0); free(q1); free(q2); free(stack_h); free(stack_i);
    return ok;
}

BENCH_NOINLINE static uint64_t query_bloom8(const uint64_t *bits, size_t mask_bits, const uint64_t *q, size_t nq)
{
    uint64_t hits = 0;
    for (size_t i = 0; i < nq; ++i) {
        uint64_t h  = xor_mix(q[i]);
        uint64_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32) | 1u;
        int ok = 1;
        for (int k = 0; k < 8; ++k) {
            size_t b = (h1 + (uint64_t)k * h2) & mask_bits;
            if (!((bits[b >> 6] >> (b & 63u)) & 1ULL)) { ok = 0; break; }
        }
        hits += (uint64_t)ok;
    }
    return hits;
}

BENCH_NOINLINE static uint64_t query_xor8(const Xor8Filter *f, const uint64_t *q, size_t nq)
{
    uint64_t hits = 0;
    uint32_t bl = f->block_len;
    const uint8_t *fp = f->fp;
    uint64_t seed = f->seed;
    for (size_t i = 0; i < nq; ++i) {
        uint64_t h  = xor_mix(q[i] + seed);
        uint32_t i0 = (uint32_t)(((uint64_t)(uint32_t)h * bl) >> 32);
        uint32_t i1 = (uint32_t)(((uint64_t)xor_rotl32((uint32_t)h, 21) * bl) >> 32) + bl;
        uint32_t i2 = (uint32_t)(((uint64_t)xor_rotl32((uint32_t)(h >> 32), 21) * bl) >> 32) + 2u * bl;
        uint8_t expect = (uint8_t)(h ^ (h >> 32));
        /* 3 independent loads dispatched simultaneously to MSHRs, ZERO branches! */
        hits += (expect == (uint8_t)(fp[i0] ^ fp[i1] ^ fp[i2]));
    }
    return hits;
}

static void run_case_xor_filter(FILE *csv)
{
    const size_t NKEYS = 1u << 20; /* 1M keys -> Bloom = 2 MB (> L2!), Xor8 = 1.23 MB */
    const size_t NQ    = 1u << 20;
    const size_t BLOOM_BITS = 1u << 24; /* 16M bits = 2 MB (16 bits/key, ~0.4% FP) */

    uint64_t *keys  = bench_alloc_aligned(64, NKEYS * sizeof(uint64_t));
    uint64_t *qs    = bench_alloc_aligned(64, NQ * sizeof(uint64_t));
    uint64_t *bloom = calloc(BLOOM_BITS / 64u, sizeof(uint64_t));
    Xor8Filter xf   = {0};

    uint64_t seed = 0x777788889999aaaaULL;
    for (size_t i = 0; i < NKEYS; ++i) {
        keys[i] = splitmix64(&seed);
        uint64_t h = xor_mix(keys[i]);
        uint64_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32) | 1u;
        for (int k = 0; k < 8; ++k) {
            size_t b = (h1 + (uint64_t)k * h2) & (BLOOM_BITS - 1u);
            bloom[b >> 6] |= 1ULL << (b & 63u);
        }
    }
    if (!xor8_build(&xf, keys, NKEYS)) { fprintf(stderr, "xor8_build failed\n"); exit(1); }

    for (size_t i = 0; i < NQ; ++i)
        qs[i] = (i & 1) ? keys[i % NKEYS] : splitmix64(&seed);

    bench_print_section("5. Immutable Set Filter (1M Keys, ~0.39% FP): Classic 8-Probe Bloom (2MB) vs Lemire Xor8 (1.2MB)",
                        "Thomas Mueller Graf & Daniel Lemire ('Xor Filters: Faster and Smaller Than Bloom Filters')");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = query_bloom8(bloom, BLOOM_BITS - 1u, qs, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "advanced_c_isa", "xor8_filter", "Classic 8-Probe Bloom (2.0 MB, Branchy)", b_ns, b_cyc, NQ, base, chk1, "Mq/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = query_xor8(&xf, qs, NQ);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "xor8_filter", "Lemire Static Xor8 (1.23 MB, 3 MLP Loads)", b_ns, b_cyc, NQ, base, chk2, "Mq/s", 1.0);

    free(keys); free(qs); free(bloom); free(xf.fp);
}

/* =========================================================================
 * 6. Sub-Byte Bit-Matrix Transform / Byte Bit-Reversal (32 KB L1-Resident x 64):
 *    Scalar Shift-Mask vs AVX2 Nibble vpshufb vs Ice Lake/Zen4 Hardware GFNI
 * ========================================================================= */
BENCH_NOINLINE __attribute__((optimize("no-tree-vectorize")))
static uint64_t bitrev_scalar(const uint8_t * BENCH_RESTRICT in, uint8_t * BENCH_RESTRICT out, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = in[i];
        b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
        b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
        b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
        out[i] = b;
    }
    return out[0] + out[n / 2] + out[n - 1];
}

BENCH_NOINLINE static uint64_t bitrev_avx2_vpshufb(const uint8_t * BENCH_RESTRICT in, uint8_t * BENCH_RESTRICT out, size_t n)
{
    const __m256i lut_lo = _mm256_setr_epi8(
        (char)0x00,(char)0x80,(char)0x40,(char)0xC0,(char)0x20,(char)0xA0,(char)0x60,(char)0xE0,
        (char)0x10,(char)0x90,(char)0x50,(char)0xD0,(char)0x30,(char)0xB0,(char)0x70,(char)0xF0,
        (char)0x00,(char)0x80,(char)0x40,(char)0xC0,(char)0x20,(char)0xA0,(char)0x60,(char)0xE0,
        (char)0x10,(char)0x90,(char)0x50,(char)0xD0,(char)0x30,(char)0xB0,(char)0x70,(char)0xF0);
    const __m256i lut_hi = _mm256_srli_epi16(lut_lo, 4);
    const __m256i mask0f = _mm256_set1_epi8(0x0F);

    for (size_t i = 0; i < n; i += 32) {
        __m256i v  = _mm256_load_si256((const __m256i *)(in + i));
        __m256i lo = _mm256_and_si256(v, mask0f);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f);
        __m256i r  = _mm256_or_si256(_mm256_shuffle_epi8(lut_lo, lo),
                                     _mm256_shuffle_epi8(lut_hi, hi));
        _mm256_store_si256((__m256i *)(out + i), r);
    }
    return out[0] + out[n / 2] + out[n - 1];
}

#if defined(__GFNI__) && defined(__AVX512VL__)
/* GFNI `vgf2p8affineqb`: multiplies each byte by an 8x8 anti-diagonal GF(2) matrix in ONE 1-cyc instruction! */
BENCH_NOINLINE static uint64_t bitrev_gfni_avx512(const uint8_t * BENCH_RESTRICT in, uint8_t * BENCH_RESTRICT out, size_t n)
{
    /* Anti-diagonal 8x8 bit matrix `0x8040201008040201ULL` reverses bits [7:0] -> [0:7] in 1 instruction! */
    const __m512i rev_mat = _mm512_set1_epi64((long long)0x8040201008040201ULL);
    for (size_t i = 0; i < n; i += 64) {
        __m512i v = _mm512_load_si512((const void *)(in + i));
        __m512i r = _mm512_gf2p8affine_epi64_epi8(v, rev_mat, 0);
        _mm512_store_si512((void *)(out + i), r);
    }
    return out[0] + out[n / 2] + out[n - 1];
}
#endif

static void run_case_gfni(FILE *csv)
{
    const size_t N = 1u << 14; /* 16 KB L1d-resident buffer x 64 passes = 1 MB */
    const size_t PASSES = 64u;
    uint8_t *in  = bench_alloc_aligned(64, N);
    uint8_t *out = bench_alloc_aligned(64, N);
    uint64_t seed = 0x1122334455667788ULL;
    for (size_t i = 0; i < N; ++i) in[i] = (uint8_t)splitmix64(&seed);

    bench_print_section("6. Sub-Byte Bit Reversal / GF(2) Matrix (16 KB L1 x 64): Scalar vs AVX2 vpshufb vs GFNI",
                        "Wojciech Mula & Daniel Lemire (simdutf / GFNI vgf2p8affineqb 1-Instruction Bit Matrix)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t p = 0; p < PASSES; ++p) {
            chk1 = bitrev_scalar(in, out, N);
            DO_NOT_OPTIMIZE(out[0]);
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "advanced_c_isa", "gfni_bitmatrix", "Scalar 3-Stage Shift/Mask Bit-Reverse", b_ns, b_cyc, N * PASSES, base, chk1, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t p = 0; p < PASSES; ++p) {
            chk2 = bitrev_avx2_vpshufb(in, out, N);
            DO_NOT_OPTIMIZE(out[0]);
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "gfni_bitmatrix", "AVX2 2-Nibble vpshufb Bit-Reverse", b_ns, b_cyc, N * PASSES, base, chk2, "MB/s", 1.0);

#if defined(__GFNI__) && defined(__AVX512VL__)
    uint64_t chk3 = 0;
    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        for (size_t p = 0; p < PASSES; ++p) {
            chk3 = bitrev_gfni_avx512(in, out, N);
            DO_NOT_OPTIMIZE(out[0]);
        }
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "gfni_bitmatrix", "GFNI vgf2p8affineqb (64B / 1 Instr!)", b_ns, b_cyc, N * PASSES, base, chk3, "MB/s", 1.0);
#endif

    free(in); free(out);
}

/* =========================================================================
 * 7. Parallel Finite-State-Machine Execution via Associative SIMD `pshufb` Composition:
 *    Table-Driven Scalar DFA (`state = dfa[state][byte]`, serial 4-cyc L1 chain!) vs
 *    SIMD Transition-Function Composition (`(T_b2 o T_b1)[s] = _mm_shuffle_epi8(T_b2, T_b1)`)
 *    (Sources: Mytkowicz et al. ASPLOS, Geoff Langdale Hyperscan, Per Vognsen)
 * ========================================================================= */
/* 8-state lexer DFA (e.g. UTF-8 validator / number-ident-comment scanner):
 * Input classified into 4 character classes (0..3); each class has a 16-byte transition vector! */
static uint8_t g_dfa_table[16][256];
static __m128i g_dfa_trans_vec[256];

static void dfa_init(void)
{
    static const uint8_t rules[4][16] = {
        /* class 0 (other):   */ { 0,0,2,0, 4,0,0,0, 0,0,0,0, 0,0,0,0 },
        /* class 1 (letter):  */ { 1,1,2,1, 4,1,1,1, 0,0,0,0, 0,0,0,0 },
        /* class 2 (slash):   */ { 3,0,5,2, 4,2,0,0, 0,0,0,0, 0,0,0,0 },
        /* class 3 (newline): */ { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
    };
    for (int c = 0; c < 256; ++c) {
        int cls = (c == '\n') ? 3 : (c == '/') ? 2
                : ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? 1 : 0;
        uint8_t vec[16];
        for (int s = 0; s < 16; ++s) {
            g_dfa_table[s][c] = rules[cls][s];
            vec[s] = rules[cls][s];
        }
        memcpy(&g_dfa_trans_vec[c], vec, 16);
    }
}

BENCH_NOINLINE static uint64_t dfa_run_scalar(const uint8_t * BENCH_RESTRICT in, size_t n)
{
    uint32_t state = 0;
    for (size_t i = 0; i < n; ++i)
        state = g_dfa_table[state][in[i]]; /* Serial dependent load every byte! */
    return state;
}

/* Break the serial chain into 4 parallel chunks OR 8-step tree composition per iteration! */
BENCH_NOINLINE static uint64_t dfa_run_simd_pshufb_compose(const uint8_t * BENCH_RESTRICT in, size_t n)
{
    /* Split buffer into 4 macroscopic segments, compose 4 transitions per step in 4 independent XMM pipes,
     * then compose the 4 segment summary functions `F3(F2(F1(F0(0))))` in 3 instructions at the end! */
    size_t seg = n >> 2;
    const uint8_t *p0 = in, *p1 = in + seg, *p2 = in + 2 * seg, *p3 = in + 3 * seg;
    __m128i id = _mm_setr_epi8(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
    __m128i f0 = id, f1 = id, f2 = id, f3 = id;

    for (size_t i = 0; i < seg; i += 2) {
        __m128i t0 = _mm_shuffle_epi8(g_dfa_trans_vec[p0[i + 1]], g_dfa_trans_vec[p0[i]]);
        __m128i t1 = _mm_shuffle_epi8(g_dfa_trans_vec[p1[i + 1]], g_dfa_trans_vec[p1[i]]);
        __m128i t2 = _mm_shuffle_epi8(g_dfa_trans_vec[p2[i + 1]], g_dfa_trans_vec[p2[i]]);
        __m128i t3 = _mm_shuffle_epi8(g_dfa_trans_vec[p3[i + 1]], g_dfa_trans_vec[p3[i]]);
        f0 = _mm_shuffle_epi8(t0, f0);
        f1 = _mm_shuffle_epi8(t1, f1);
        f2 = _mm_shuffle_epi8(t2, f2);
        f3 = _mm_shuffle_epi8(t3, f3);
    }
    __m128i f01   = _mm_shuffle_epi8(f1, f0);
    __m128i f23   = _mm_shuffle_epi8(f3, f2);
    __m128i total = _mm_shuffle_epi8(f23, f01);
    uint8_t final_map[16];
    _mm_storeu_si128((__m128i *)final_map, total);
    return final_map[0]; /* exact final state starting from state 0! */
}

static void run_case_simd_dfa(FILE *csv)
{
    dfa_init();
    const size_t N = 1u << 20; /* 1 MB text stream */
    uint8_t *buf = bench_alloc_aligned(64, N);
    const char *sample = "ident // comment line\nfoo / bar baz\nalpha beta // note\nxyz ";
    size_t slen = strlen(sample);
    for (size_t i = 0; i < N; ++i) buf[i] = (uint8_t)sample[i % slen];
    memcpy(buf + N - 8, "//hello!", 8); /* ends inside comment state 4 (non-zero checksum!) */

    bench_print_section("7. Finite State Machine (1 MB DFA): Serial Table Lookup vs Associative SIMD pshufb Compose",
                        "Mytkowicz et al. (ASPLOS Parallel FSM), Geoff Langdale (Hyperscan), Per Vognsen");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = dfa_run_scalar(buf, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "advanced_c_isa", "simd_dfa_compose", "Table DFA state = dfa[state][c]", b_ns, b_cyc, N, base, chk1, "MB/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = dfa_run_simd_pshufb_compose(buf, N);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "simd_dfa_compose", "4-Stream Associative pshufb DFA", b_ns, b_cyc, N, base, chk2, "MB/s", 1.0);

    free(buf);
}

/* =========================================================================
 * 8. Roaring Bitmap Hybrid Containers vs Sorted uint32 Posting-List Intersection:
 *    16 Chunks of 65,536 Universe (8 Dense >=4096 + 8 Sparse <256):
 *    Sorted Array Two-Pointer Merge vs Roaring Hybrid (Bitset AVX2 + Sorted u16)
 * ========================================================================= */
typedef struct {
    bool      is_bitset;
    uint32_t  len;
    uint16_t *arr;       /* used when len < 4096 */
    uint64_t *bits;      /* 1024 words = 8 KB when len >= 4096 */
} RoaringChunk;

BENCH_NOINLINE static uint64_t intersect_sorted_u32(const uint32_t *a, size_t na,
                                                    const uint32_t *b, size_t nb)
{
    size_t ia = 0, ib = 0;
    uint64_t count = 0;
    while (ia < na && ib < nb) {
        uint32_t va = a[ia], vb = b[ib];
        if (va == vb) { ++count; ++ia; ++ib; }
        else if (va < vb) ++ia;
        else              ++ib;
    }
    return count;
}

BENCH_NOINLINE static uint64_t intersect_roaring_hybrid(const RoaringChunk *ca, const RoaringChunk *cb, size_t nchunks)
{
    uint64_t total = 0;
    for (size_t c = 0; c < nchunks; ++c) {
        if (ca[c].is_bitset && cb[c].is_bitset) {
            /* Dense x Dense: 1024 words = 8 KB bitwise AND + hardware popcntq! */
            const uint64_t *ba = ca[c].bits, *bb = cb[c].bits;
            for (size_t w = 0; w < 1024; w += 4) {
                total += (uint64_t)__builtin_popcountll(ba[w + 0] & bb[w + 0])
                       + (uint64_t)__builtin_popcountll(ba[w + 1] & bb[w + 1])
                       + (uint64_t)__builtin_popcountll(ba[w + 2] & bb[w + 2])
                       + (uint64_t)__builtin_popcountll(ba[w + 3] & bb[w + 3]);
            }
        } else if (!ca[c].is_bitset && !cb[c].is_bitset) {
            /* Sparse x Sparse: tiny L1-resident uint16 merge */
            size_t ia = 0, ib = 0, na = ca[c].len, nb = cb[c].len;
            const uint16_t *aa = ca[c].arr, *ab = cb[c].arr;
            while (ia < na && ib < nb) {
                uint16_t va = aa[ia], vb = ab[ib];
                if (va == vb) { ++total; ++ia; ++ib; }
                else if (va < vb) ++ia;
                else              ++ib;
            }
        } else {
            /* Sparse x Dense: O(|sparse|) direct bit-tests into the 8 KB L1-resident bitset! */
            const RoaringChunk *sp = ca[c].is_bitset ? &cb[c] : &ca[c];
            const RoaringChunk *dn = ca[c].is_bitset ? &ca[c] : &cb[c];
            for (size_t i = 0; i < sp->len; ++i) {
                uint16_t v = sp->arr[i];
                total += (dn->bits[v >> 6] >> (v & 63u)) & 1ULL;
            }
        }
    }
    return total;
}

static void run_case_roaring(FILE *csv)
{
    const size_t NCHUNKS = 16; /* Universe = 1,048,576 IDs */
    RoaringChunk ca[16] = {0}, cb[16] = {0};
    uint32_t *sa = malloc(1048576 * sizeof(uint32_t));
    uint32_t *sb = malloc(1048576 * sizeof(uint32_t));
    size_t na = 0, nb = 0;

    uint64_t seed = 0xabcdef1122334455ULL;
    for (size_t c = 0; c < NCHUNKS; ++c) {
        bool dense_a = (c < 8);               /* chunks 0..7 dense (~16K IDs), 8..15 sparse (~128 IDs) */
        bool dense_b = (c < 4) || (c >= 12);  /* tests Dense-Dense, Dense-Sparse, AND Sparse-Sparse! */

        uint64_t * tmp_a = calloc(1024, 8), *tmp_b = calloc(1024, 8);
        size_t cnt_a = 0, cnt_b = 0;
        for (uint32_t v = 0; v < 65536; ++v) {
            if ((splitmix64(&seed) & 0xFFFFu) < (dense_a ? 16384u : 128u)) {
                tmp_a[v >> 6] |= 1ULL << (v & 63u);
                sa[na++] = (uint32_t)(c << 16) | v;
                ++cnt_a;
            }
            if ((splitmix64(&seed) & 0xFFFFu) < (dense_b ? 16384u : 128u)) {
                tmp_b[v >> 6] |= 1ULL << (v & 63u);
                sb[nb++] = (uint32_t)(c << 16) | v;
                ++cnt_b;
            }
        }
        ca[c].len = (uint32_t)cnt_a;
        ca[c].is_bitset = (cnt_a >= 4096);
        if (ca[c].is_bitset) ca[c].bits = tmp_a;
        else {
            ca[c].arr = malloc(cnt_a * sizeof(uint16_t));
            size_t k = 0;
            for (uint32_t v = 0; v < 65536; ++v) if ((tmp_a[v >> 6] >> (v & 63u)) & 1ULL) ca[c].arr[k++] = (uint16_t)v;
            free(tmp_a);
        }

        cb[c].len = (uint32_t)cnt_b;
        cb[c].is_bitset = (cnt_b >= 4096);
        if (cb[c].is_bitset) cb[c].bits = tmp_b;
        else {
            cb[c].arr = malloc(cnt_b * sizeof(uint16_t));
            size_t k = 0;
            for (uint32_t v = 0; v < 65536; ++v) if ((tmp_b[v >> 6] >> (v & 63u)) & 1ULL) cb[c].arr[k++] = (uint16_t)v;
            free(tmp_b);
        }
    }

    bench_print_section("8. Set Intersection (1M ID Universe, Mixed Dense/Sparse): Sorted u32 Merge vs Roaring Bitmap",
                        "Daniel Lemire et al. (Roaring Bitmaps: Adaptive 16-bit Array / 8KB Bitset Containers)");

    uint64_t b_ns = UINT64_MAX, b_cyc = UINT64_MAX, chk1 = 0, chk2 = 0;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk1 = intersect_sorted_u32(sa, na, sb, nb);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk1);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    double base = (double)b_ns;
    bench_print_row(csv, "advanced_c_isa", "roaring_bitmap", "Sorted uint32[] Two-Pointer Merge", b_ns, b_cyc, na + nb, base, chk1, "Me/s", 1.0);

    b_ns = UINT64_MAX; b_cyc = UINT64_MAX;
    for (int r = 0; r < REPS; ++r) {
        uint64_t t0 = bench_now_ns(), c0 = bench_rdtsc_start();
        chk2 = intersect_roaring_hybrid(ca, cb, NCHUNKS);
        uint64_t dc = bench_rdtsc_end() - c0, dn = bench_now_ns() - t0;
        DO_NOT_OPTIMIZE(chk2);
        if (dn < b_ns) b_ns = dn;
        if (dc < b_cyc) b_cyc = dc;
    }
    bench_print_row(csv, "advanced_c_isa", "roaring_bitmap", "Roaring Hybrid (8KB Bitset + u16 Array)", b_ns, b_cyc, na + nb, base, chk2, "Me/s", 1.0);

    for (size_t c = 0; c < NCHUNKS; ++c) {
        free(ca[c].arr); free(ca[c].bits);
        free(cb[c].arr); free(cb[c].bits);
    }
    free(sa); free(sb);
}

int main(void)
{
    printf("====================================================================================\n");
    printf("  SUITE 6: COMPILER/LOOP TILING, AES/CRC32/GFNI ISA, STREAMVBYTE, VM & XOR8 PROOFS\n");
    printf("====================================================================================\n");

    FILE *csv = fopen("results_proofs.csv", "a");

    run_case_gemm(csv);
    run_case_hashing(csv);
    run_case_streamvbyte(csv);
    run_case_vm_dispatch(csv);
    run_case_xor_filter(csv);
    run_case_gfni(csv);
    run_case_simd_dfa(csv);
    run_case_roaring(csv);

    if (csv) fclose(csv);
    return 0;
}
