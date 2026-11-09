/*
 * bench_struct_vs_raw.c — benchmark: struct types vs raw float arrays
 *
 * Compares the performance of:
 *   1. mu_math.h style: struct types with named fields (mu_vec3.x, mu_mat4.col[0])
 *   2. mu_math_raw.h style: raw float arrays (v[0], m[4], etc.)
 *
 * Both use the same scalar implementations (no SIMD intrinsics in the raw
 * version) to keep the comparison fair — the difference is only in the
 * data layout: struct vs array.
 *
 * Build:
 *   gcc -O3 -march=native -o bench bench_struct_vs_raw.c -lm
 *   ./bench
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sched.h>
#include <time.h>
#include <stdint.h>

/* ========================================================================== *
 * STRUCT VERSION (mirrors mu_math.h layout)                                  *
 * ========================================================================== */

typedef struct { float x, y; } s_vec2;
typedef struct { float x, y, z; } s_vec3;
typedef struct __attribute__((aligned(16))) { float x, y, z, w; } s_vec4;
typedef s_vec4 s_quat;
typedef struct { s_vec3 col[3]; } s_mat3;
typedef struct __attribute__((aligned(16))) { s_vec4 col[4]; } s_mat4;

/* struct vec3 ops */
static inline s_vec3 s_v3(float x, float y, float z) { return (s_vec3){x,y,z}; }
static inline s_vec3 s_v3_add(s_vec3 a, s_vec3 b) { return s_v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline s_vec3 s_v3_sub(s_vec3 a, s_vec3 b) { return s_v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline s_vec3 s_v3_scale(s_vec3 a, float s) { return s_v3(a.x*s, a.y*s, a.z*s); }
static inline float  s_v3_dot(s_vec3 a, s_vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline s_vec3 s_v3_cross(s_vec3 a, s_vec3 b) {
    return s_v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline float s_v3_len2(s_vec3 v) { return s_v3_dot(v, v); }
static inline float s_v3_len(s_vec3 v) { return sqrtf(s_v3_len2(v)); }
static inline s_vec3 s_v3_norm(s_vec3 v) {
    float l = s_v3_len(v);
    if (l > 0.0f) { float i = 1.0f/l; return s_v3(v.x*i, v.y*i, v.z*i); }
    return s_v3(0,0,0);
}

/* struct mat4 ops */
static inline void s_m4_identity(s_mat4* m) {
    m->col[0] = (s_vec4){1,0,0,0};
    m->col[1] = (s_vec4){0,1,0,0};
    m->col[2] = (s_vec4){0,0,1,0};
    m->col[3] = (s_vec4){0,0,0,1};
}
static inline void s_m4_mul(const s_mat4* a, const s_mat4* b, s_mat4* dst) {
    int j, k;
    for (j = 0; j < 4; j++) {
        float r0=0,r1=0,r2=0,r3=0;
        float bv[4] = { (&b->col[j].x)[0], (&b->col[j].x)[1], (&b->col[j].x)[2], (&b->col[j].x)[3] };
        for (k = 0; k < 4; k++) {
            r0 += bv[k] * a->col[k].x;
            r1 += bv[k] * a->col[k].y;
            r2 += bv[k] * a->col[k].z;
            r3 += bv[k] * a->col[k].w;
        }
        dst->col[j] = (s_vec4){r0,r1,r2,r3};
    }
}
static inline s_vec4 s_m4_mulv(const s_mat4* m, s_vec4 v) {
    return s_v3(v.x,v.y,v.z), /* unused, just for type */
    (s_vec4){
        m->col[0].x*v.x + m->col[1].x*v.y + m->col[2].x*v.z + m->col[3].x*v.w,
        m->col[0].y*v.x + m->col[1].y*v.y + m->col[2].y*v.z + m->col[3].y*v.w,
        m->col[0].z*v.x + m->col[1].z*v.y + m->col[2].z*v.z + m->col[3].z*v.w,
        m->col[0].w*v.x + m->col[1].w*v.y + m->col[2].w*v.z + m->col[3].w*v.w
    };
}
static inline void s_m4_inverse(const s_mat4* m, s_mat4* dst) {
    float a=m->col[0].x,b=m->col[0].y,c=m->col[0].z,d=m->col[0].w;
    float e=m->col[1].x,f=m->col[1].y,g=m->col[1].z,h=m->col[1].w;
    float i=m->col[2].x,j=m->col[2].y,k=m->col[2].z,l=m->col[2].w;
    float mm=m->col[3].x,n=m->col[3].y,o=m->col[3].z,p=m->col[3].w;
    float c1=k*p-l*o,  c2=c*h-d*g,  c3=i*p-l*mm, c4=a*h-d*e;
    float c5=j*p-l*n,  c6=b*h-d*f,  c7=i*n-j*mm, c8=a*f-b*e;
    float c9=j*o-k*n,  c10=b*g-c*f, c11=i*o-k*mm,c12=a*g-c*e;
    float det = c8*c1 + c4*c9 + c10*c3 + c2*c7 - c12*c5 - c6*c11;
    float inv = (det != 0.0f) ? 1.0f/det : 0.0f;
    dst->col[0] = (s_vec4){ (f*c1-g*c5+h*c9)*inv, (-b*c1+c*c5-d*c9)*inv, (b*c2-c*c3+d*c7)*inv, (-b*c11+c*c12-d*c10)*inv };
    dst->col[1] = (s_vec4){ (-e*c1+g*c4-h*c8)*inv, (a*c1-c*c4+d*c8)*inv, (-a*c2+c*c3-d*c7)*inv, (a*c11-c*c12+d*c10)*inv };
    dst->col[2] = (s_vec4){ (e*c5-f*c4+h*c12)*inv, (-a*c5+b*c4-d*c12)*inv, (a*c6-b*c3+d*c9)*inv, (-a*c9+b*c11-c*c12)*inv };
    dst->col[3] = (s_vec4){ (-e*c9+f*c8-g*c12)*inv, (a*c9-b*c8+c*c12)*inv, (-a*c6+b*c11-c*c10)*inv, (a*c10-b*c11+c*c9)*inv };
}
static inline s_quat s_quat_mul(s_quat a, s_quat b) {
    return (s_vec4){
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

/* ========================================================================== *
 * RAW FLOAT VERSION (mirrors mu_math_raw.h layout)                           *
 * ========================================================================== */

typedef float r_vec3[3];
typedef float r_vec4[4];
typedef float r_quat[4];
typedef float r_mat4[16];

/* raw vec3 ops */
static inline void r_v3_make(float* out, float x, float y, float z) { out[0]=x; out[1]=y; out[2]=z; }
static inline void r_v3_add(float* out, const float* a, const float* b) { out[0]=a[0]+b[0]; out[1]=a[1]+b[1]; out[2]=a[2]+b[2]; }
static inline void r_v3_sub(float* out, const float* a, const float* b) { out[0]=a[0]-b[0]; out[1]=a[1]-b[1]; out[2]=a[2]-b[2]; }
static inline void r_v3_scale(float* out, const float* a, float s) { out[0]=a[0]*s; out[1]=a[1]*s; out[2]=a[2]*s; }
static inline float  r_v3_dot(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static inline void r_v3_cross(float* out, const float* a, const float* b) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}
static inline float r_v3_len2(const float* v) { return r_v3_dot(v, v); }
static inline float r_v3_len(const float* v) { return sqrtf(r_v3_len2(v)); }
static inline void r_v3_norm(float* out, const float* v) {
    float l = r_v3_len(v);
    if (l > 0.0f) { float i = 1.0f/l; out[0]=v[0]*i; out[1]=v[1]*i; out[2]=v[2]*i; }
    else { out[0]=out[1]=out[2]=0.0f; }
}

/* raw mat4 ops */
static inline void r_m4_identity(float* m) {
    memset(m, 0, 64);
    m[0]=m[5]=m[10]=m[15]=1.0f;
}
static inline void r_m4_mul(float* out, const float* a, const float* b) {
    int j, k;
    float tmp[16];
    for (j = 0; j < 4; j++) {
        for (k = 0; k < 4; k++) {
            tmp[j*4+k] = a[0*4+k]*b[j*4+0] + a[1*4+k]*b[j*4+1] + a[2*4+k]*b[j*4+2] + a[3*4+k]*b[j*4+3];
        }
    }
    memcpy(out, tmp, 64);
}
static inline void r_m4_mulv(float* out, const float* m, const float* v) {
    out[0] = m[0]*v[0] + m[4]*v[1] + m[8]*v[2] + m[12]*v[3];
    out[1] = m[1]*v[0] + m[5]*v[1] + m[9]*v[2] + m[13]*v[3];
    out[2] = m[2]*v[0] + m[6]*v[1] + m[10]*v[2]+ m[14]*v[3];
    out[3] = m[3]*v[0] + m[7]*v[1] + m[11]*v[2]+ m[15]*v[3];
}
static inline void r_m4_inverse(float* out, const float* m) {
    float a=m[0],b=m[1],c=m[2],d=m[3];
    float e=m[4],f=m[5],g=m[6],h=m[7];
    float i=m[8],j=m[9],k=m[10],l=m[11];
    float mm=m[12],n=m[13],o=m[14],p=m[15];
    float c1=k*p-l*o,  c2=c*h-d*g,  c3=i*p-l*mm, c4=a*h-d*e;
    float c5=j*p-l*n,  c6=b*h-d*f,  c7=i*n-j*mm, c8=a*f-b*e;
    float c9=j*o-k*n,  c10=b*g-c*f, c11=i*o-k*mm,c12=a*g-c*e;
    float det = c8*c1 + c4*c9 + c10*c3 + c2*c7 - c12*c5 - c6*c11;
    float inv = (det != 0.0f) ? 1.0f/det : 0.0f;
    out[0]=(f*c1-g*c5+h*c9)*inv; out[1]=(-b*c1+c*c5-d*c9)*inv; out[2]=(b*c2-c*c3+d*c7)*inv; out[3]=(-b*c11+c*c12-d*c10)*inv;
    out[4]=(-e*c1+g*c4-h*c8)*inv; out[5]=(a*c1-c*c4+d*c8)*inv; out[6]=(-a*c2+c*c3-d*c7)*inv; out[7]=(a*c11-c*c12+d*c10)*inv;
    out[8]=(e*c5-f*c4+h*c12)*inv; out[9]=(-a*c5+b*c4-d*c12)*inv; out[10]=(a*c6-b*c3+d*c9)*inv; out[11]=(-a*c9+b*c11-c*c12)*inv;
    out[12]=(-e*c9+f*c8-g*c12)*inv; out[13]=(a*c9-b*c8+c*c12)*inv; out[14]=(-a*c6+b*c11-c*c10)*inv; out[15]=(a*c10-b*c11+c*c9)*inv;
}
static inline void r_quat_mul(float* out, const float* a, const float* b) {
    out[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    out[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    out[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    out[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

/* ========================================================================== *
 * BENCH HARNESS                                                              *
 * ========================================================================== */

#define N 4096
#define REPS 21

static volatile double g_sink;
#define SINK(v) do { g_sink = (double)(v); } while(0)

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void pin_core(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof set, &set);
}

static uint32_t rng = 0xDEADBEEF;
static float frand(float lo, float hi) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return lo + (hi - lo) * ((float)(rng & 0xFFFFFF) / 16777215.0f);
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) - (x > y);
}

/* -------------------------------------------------------------------------- *
 * TEST DATA                                                                  *
 * -------------------------------------------------------------------------- */

/* struct arrays */
static s_vec3  s_v3a[N], s_v3b[N], s_v3out[N];
static s_vec4  s_v4a[N], s_v4b[N], s_v4out[N];
static s_mat4  s_m4a[N], s_m4b[N], s_m4out[N];
static s_quat  s_qa[N], s_qb[N], s_qout[N];

/* raw arrays */
static float r_v3a[N][3], r_v3b[N][3], r_v3out[N][3];
static float r_v4a[N][4], r_v4b[N][4], r_v4out[N][4];
static float r_m4a[N][16], r_m4b[N][16], r_m4out[N][16];
static float r_qa[N][4], r_qb[N][4], r_qout[N][4];

static void init_data(void) {
    int i;
    for (i = 0; i < N; i++) {
        /* struct */
        s_v3a[i] = s_v3(frand(-10,10), frand(-10,10), frand(-10,10));
        s_v3b[i] = s_v3(frand(-10,10), frand(-10,10), frand(-10,10));
        s_v4a[i] = (s_vec4){frand(-10,10), frand(-10,10), frand(-10,10), frand(-10,10)};
        s_v4b[i] = (s_vec4){frand(-10,10), frand(-10,10), frand(-10,10), frand(-10,10)};
        /* raw */
        r_v3_make(r_v3a[i], s_v3a[i].x, s_v3a[i].y, s_v3a[i].z);
        r_v3_make(r_v3b[i], s_v3b[i].x, s_v3b[i].y, s_v3b[i].z);
        r_v4a[i][0]=s_v4a[i].x; r_v4a[i][1]=s_v4a[i].y; r_v4a[i][2]=s_v4a[i].z; r_v4a[i][3]=s_v4a[i].w;
        r_v4b[i][0]=s_v4b[i].x; r_v4b[i][1]=s_v4b[i].y; r_v4b[i][2]=s_v4b[i].z; r_v4b[i][3]=s_v4b[i].w;
    }
    /* random matrices (well-conditioned: close to identity + small random) */
    for (i = 0; i < N; i++) {
        s_m4_identity(&s_m4a[i]);
        s_m4_identity(&s_m4b[i]);
        int r, c;
        for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) {
            (&s_m4a[i].col[c].x)[r] += frand(-0.5f, 0.5f);
            (&s_m4b[i].col[c].x)[r] += frand(-0.5f, 0.5f);
        }
        memcpy(r_m4a[i], &s_m4a[i], 64);
        memcpy(r_m4b[i], &s_m4b[i], 64);
    }
    /* random quaternions (normalized) */
    for (i = 0; i < N; i++) {
        s_qa[i] = (s_vec4){frand(-1,1), frand(-1,1), frand(-1,1), frand(-1,1)};
        s_qb[i] = (s_vec4){frand(-1,1), frand(-1,1), frand(-1,1), frand(-1,1)};
        float la = sqrtf(s_qa[i].x*s_qa[i].x + s_qa[i].y*s_qa[i].y + s_qa[i].z*s_qa[i].z + s_qa[i].w*s_qa[i].w);
        float lb = sqrtf(s_qb[i].x*s_qb[i].x + s_qb[i].y*s_qb[i].y + s_qb[i].z*s_qb[i].z + s_qb[i].w*s_qb[i].w);
        s_qa[i].x/=la; s_qa[i].y/=la; s_qa[i].z/=la; s_qa[i].w/=la;
        s_qb[i].x/=lb; s_qb[i].y/=lb; s_qb[i].z/=lb; s_qb[i].w/=lb;
        r_qa[i][0]=s_qa[i].x; r_qa[i][1]=s_qa[i].y; r_qa[i][2]=s_qa[i].z; r_qa[i][3]=s_qa[i].w;
        r_qb[i][0]=s_qb[i].x; r_qb[i][1]=s_qb[i].y; r_qb[i][2]=s_qb[i].z; r_qb[i][3]=s_qb[i].w;
    }
}

/* -------------------------------------------------------------------------- *
 * BENCH FUNCTIONS                                                            *
 * -------------------------------------------------------------------------- */

/* --- vec3 dot --- */
static double bench_struct_v3_dot(void) {
    double t0 = now_ns();
    float acc = 0;
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            acc += s_v3_dot(s_v3a[i], s_v3b[i]);
    SINK(acc);
    return (now_ns() - t0) / (100.0 * N);
}

static double bench_raw_v3_dot(void) {
    double t0 = now_ns();
    float acc = 0;
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            acc += r_v3_dot(r_v3a[i], r_v3b[i]);
    SINK(acc);
    return (now_ns() - t0) / (100.0 * N);
}

/* --- vec3 cross --- */
static double bench_struct_v3_cross(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            s_v3out[i] = s_v3_cross(s_v3a[i], s_v3b[i]);
    SINK(s_v3out[0].x);
    return (now_ns() - t0) / (100.0 * N);
}

static double bench_raw_v3_cross(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            r_v3_cross(r_v3out[i], r_v3a[i], r_v3b[i]);
    SINK(r_v3out[0][0]);
    return (now_ns() - t0) / (100.0 * N);
}

/* --- vec3 normalize --- */
static double bench_struct_v3_norm(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            s_v3out[i] = s_v3_norm(s_v3a[i]);
    SINK(s_v3out[0].x);
    return (now_ns() - t0) / (100.0 * N);
}

static double bench_raw_v3_norm(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            r_v3_norm(r_v3out[i], r_v3a[i]);
    SINK(r_v3out[0][0]);
    return (now_ns() - t0) / (100.0 * N);
}

/* --- mat4 mul --- */
static double bench_struct_m4_mul(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 10; rep++)
        for (int i = 0; i < N; i++)
            s_m4_mul(&s_m4a[i], &s_m4b[i], &s_m4out[i]);
    SINK(s_m4out[0].col[0].x);
    return (now_ns() - t0) / (10.0 * N);
}

static double bench_raw_m4_mul(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 10; rep++)
        for (int i = 0; i < N; i++)
            r_m4_mul(r_m4out[i], r_m4a[i], r_m4b[i]);
    SINK(r_m4out[0][0]);
    return (now_ns() - t0) / (10.0 * N);
}

/* --- mat4 * vec4 --- */
static double bench_struct_m4_mulv(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            s_v4out[i] = s_m4_mulv(&s_m4a[i], s_v4b[i]);
    SINK(s_v4out[0].x);
    return (now_ns() - t0) / (100.0 * N);
}

static double bench_raw_m4_mulv(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            r_m4_mulv(r_v4out[i], r_m4a[i], r_v4b[i]);
    SINK(r_v4out[0][0]);
    return (now_ns() - t0) / (100.0 * N);
}

/* --- mat4 inverse --- */
static double bench_struct_m4_inv(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 10; rep++)
        for (int i = 0; i < N; i++)
            s_m4_inverse(&s_m4a[i], &s_m4out[i]);
    SINK(s_m4out[0].col[0].x);
    return (now_ns() - t0) / (10.0 * N);
}

static double bench_raw_m4_inv(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 10; rep++)
        for (int i = 0; i < N; i++)
            r_m4_inverse(r_m4out[i], r_m4a[i]);
    SINK(r_m4out[0][0]);
    return (now_ns() - t0) / (10.0 * N);
}

/* --- quat mul --- */
static double bench_struct_quat_mul(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            s_qout[i] = s_quat_mul(s_qa[i], s_qb[i]);
    SINK(s_qout[0].x);
    return (now_ns() - t0) / (100.0 * N);
}

static double bench_raw_quat_mul(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++)
        for (int i = 0; i < N; i++)
            r_quat_mul(r_qout[i], r_qa[i], r_qb[i]);
    SINK(r_qout[0][0]);
    return (now_ns() - t0) / (100.0 * N);
}

/* -------------------------------------------------------------------------- *
 * RUNNER                                                                     *
 * -------------------------------------------------------------------------- */

typedef struct {
    const char* name;
    double (*struct_fn)(void);
    double (*raw_fn)(void);
} TestCase;

static void run_test(TestCase* tc) {
    double ts[REPS], tr[REPS];
    int r;

    /* warmup */
    for (r = 0; r < 3; r++) { tc->struct_fn(); tc->raw_fn(); }

    for (r = 0; r < REPS; r++) {
        ts[r] = tc->struct_fn();
        tr[r] = tc->raw_fn();
    }

    qsort(ts, REPS, sizeof(double), cmp_d);
    qsort(tr, REPS, sizeof(double), cmp_d);

    double med_s = ts[REPS/2];
    double med_r = tr[REPS/2];
    double min_s = ts[0];
    double min_r = tr[0];
    double ratio = med_s / med_r;

    printf("  %-28s  struct=%7.2f ns  raw=%7.2f ns  ratio=%6.3fx  %s\n",
           tc->name,
           med_s, med_r,
           ratio,
           ratio > 1.01 ? "RAW WINS" : (ratio < 0.99 ? "STRUCT WINS" : "~SAME"));
}

int main(void) {
    pin_core();

    printf("=================================================================\n");
    printf(" Struct types vs Raw float arrays — head-to-head benchmark\n");
    printf("=================================================================\n");
    printf(" N = %d items per batch, %d reps (median), core pinned\n", N, REPS);

#ifdef __clang__
    printf(" compiler: clang " __clang_version__ "\n");
#elif defined(__GNUC__)
    printf(" compiler: gcc " __VERSION__ "\n");
#endif

    printf(" sizes:  s_vec3 = %zu B   r_vec3 = %zu B\n", sizeof(s_vec3), sizeof(r_vec3));
    printf("         s_vec4 = %zu B   r_vec4 = %zu B\n", sizeof(s_vec4), sizeof(r_vec4));
    printf("         s_mat4 = %zu B   r_mat4 = %zu B\n", sizeof(s_mat4), sizeof(r_mat4));
    printf("         s_quat = %zu B   r_quat = %zu B\n", sizeof(s_quat), sizeof(r_quat));
    printf("-----------------------------------------------------------------\n");
    printf(" ratio > 1.0 means raw floats are faster\n");
    printf(" ratio < 1.0 means struct types are faster\n");
    printf("=================================================================\n\n");

    init_data();

    TestCase tests[] = {
        { "vec3 dot",         bench_struct_v3_dot,     bench_raw_v3_dot },
        { "vec3 cross",       bench_struct_v3_cross,   bench_raw_v3_cross },
        { "vec3 normalize",   bench_struct_v3_norm,    bench_raw_v3_norm },
        { "mat4 multiply",    bench_struct_m4_mul,     bench_raw_m4_mul },
        { "mat4 * vec4",      bench_struct_m4_mulv,    bench_raw_m4_mulv },
        { "mat4 inverse",     bench_struct_m4_inv,     bench_raw_m4_inv },
        { "quat multiply",    bench_struct_quat_mul,   bench_raw_quat_mul },
    };

    int ntests = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < ntests; i++)
        run_test(&tests[i]);

    printf("\n=================================================================\n");
    printf(" Done. ratio = struct_median / raw_median\n");
    printf("=================================================================\n");
    return 0;
}
