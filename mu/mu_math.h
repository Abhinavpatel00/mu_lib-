/*
 * mu_math.h — single-header 3D math for engines, raw float arrays.
 *
 * All types are raw float arrays. All operations use pointer-based API.
 * Zero struct overhead. Zero conversion at the shader boundary.
 * Data is already in the layout the GPU expects — direct memcpy to Vulkan
 * uniform buffers, push constants, and vertex buffers.
 *
 * TYPE LAYOUT (column-major, matches GLSL/Slang exactly):
 *   mu_vec2  = float[2]     8 B   packed
 *   mu_vec3  = float[3]    12 B   packed
 *   mu_vec4  = float[4]    16 B   16-aligned
 *   mu_quat  = float[4]    16 B   (x,y,z,w), identity = (0,0,0,1)
 *   mu_mat3  = float[9]    36 B   column-major
 *   mu_mat4  = float[16]   64 B   16-aligned, column-major
 *
 * API CONVENTION:
 *   Output is the first parameter for most ops: mu_vec3_add(r, a, b)
 *   For mul/inverse/transpose: dst is the last param: mu_mat4_mul(a, b, dst)
 *   Inputs are const float*.
 *
 * BACKENDS: AVX-512F -> AVX2+FMA -> SSE2 -> scalar. Selected by -march=.
 *
 * BENCHMARKS: See BENCHMARK_RESULTS.md. Computation speed is identical to
 * the struct version at -O3. Shader upload is up to 25x faster (zero copy
 * when your GPU object layout matches the UBO).
 */

#pragma once
#ifndef MU_MATH_H
#define MU_MATH_H

#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================== *
 * BACKEND DETECTION (same as mu_math.h)                                  *
 * ====================================================================== */

#if defined(MU_MATH_SCALAR)
#elif defined(__AVX512F__)
#  define MU_RAW_AVX512 1
#  define MU_RAW_AVX2   1
#  define MU_RAW_SSE    1
#  include <immintrin.h>
#elif defined(__AVX2__)
#  define MU_RAW_AVX2   1
#  define MU_RAW_SSE    1
#  include <immintrin.h>
#elif defined(__AVX__)
#  define MU_RAW_SSE    1
#  include <immintrin.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define MU_RAW_SSE    1
#  include <emmintrin.h>
#endif

#if defined(MU_RAW_SSE) && defined(__SSE4_1__)
#  define MU_RAW_SSE41  1
#endif
#if defined(MU_RAW_SSE) && defined(__SSE3__)
#  define MU_RAW_HADD   1
#endif
#if defined(MU_RAW_SSE) && defined(__FMA__)
#  define MU_RAW_FMA    1
#endif

/* ====================================================================== *
 * TYPES                                                                  *
 * ====================================================================== */

typedef float mu_vec2[2];
typedef float mu_vec3[3];
typedef float mu_vec4[4]  __attribute__((aligned(16)));
typedef float mu_quat[4]  __attribute__((aligned(16)));
typedef float mu_mat3[9];
typedef float mu_mat4[16] __attribute__((aligned(16)));
typedef float mu_plane[4];

typedef struct mu_frustum {
    mu_plane p[6];
} mu_frustum;

/* matrix-in-registers (hot path) */
typedef struct mu_mat4_hot {
#if defined(MU_RAW_SSE)
    __m128 c0, c1, c2, c3;
#else
    mu_vec4 c0, c1, c2, c3;
#endif
} mu_mat4_hot;

/* 4-wide palette */
#if defined(MU_RAW_AVX512)
typedef struct __attribute__((aligned(64))) mu_mat4x4 { __m512 col[4]; } mu_mat4x4;
#else
typedef struct __attribute__((aligned(64))) mu_mat4x4 { mu_mat4 m[4]; } mu_mat4x4;
#endif

/* ====================================================================== *
 * SCALAR HELPERS                                                         *
 * ====================================================================== */

#define MU_RAW_EPS 1e-6f

static inline float mu_radians(float deg) { return deg * 0.017453292519943295f; }
static inline float mu_degrees(float rad) { return rad * 57.29577951308232f; }
static inline float mu_lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float mu_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline float mu_rsqrt_f32(float x)
{
#if defined(MU_RAW_SSE)
    __m128 v = _mm_set_ss(x);
    __m128 r = _mm_rsqrt_ss(v);
    r = _mm_mul_ss(r, _mm_sub_ss(_mm_set_ss(1.5f),
                _mm_mul_ss(_mm_mul_ss(v, _mm_set_ss(0.5f)),
                    _mm_mul_ss(r, r))));
    return _mm_cvtss_f32(r);
#else
    float y = 1.0f / sqrtf(x);
    return y * (1.5f - 0.5f * x * y * y);
#endif
}

/* ====================================================================== *
 * VEC2                                                                   *
 * ====================================================================== */

static inline void mu_vec2_make(float* r, float x, float y) { r[0]=x; r[1]=y; }
static inline void mu_vec2_add(float* r, const float* a, const float* b) { r[0]=a[0]+b[0]; r[1]=a[1]+b[1]; }
static inline void mu_vec2_sub(float* r, const float* a, const float* b) { r[0]=a[0]-b[0]; r[1]=a[1]-b[1]; }
static inline void mu_vec2_scale(float* r, const float* a, float s) { r[0]=a[0]*s; r[1]=a[1]*s; }
static inline float mu_vec2_dot(const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]; }
static inline float mu_vec2_cross(const float* a, const float* b) { return a[0]*b[1]-a[1]*b[0]; }
static inline float mu_vec2_length2(const float* v) { return mu_vec2_dot(v,v); }
static inline float mu_vec2_length(const float* v) { return sqrtf(mu_vec2_length2(v)); }
static inline void mu_vec2_normalize(float* r, const float* v)
{
    float inv = 1.0f / sqrtf(mu_vec2_length2(v));
    mu_vec2_scale(r, v, inv);
}

/* ====================================================================== *
 * VEC3                                                                   *
 * ====================================================================== */

static inline void mu_vec3_make(float* r, float x, float y, float z) { r[0]=x; r[1]=y; r[2]=z; }
static inline void mu_vec3_splat(float* r, float s) { r[0]=r[1]=r[2]=s; }
static inline void mu_vec3_zero(float* r) { r[0]=r[1]=r[2]=0.0f; }
static inline void mu_vec3_copy(float* r, const float* s) { r[0]=s[0]; r[1]=s[1]; r[2]=s[2]; }

static inline void mu_vec3_from_vec4(float* r, const float* v) { r[0]=v[0]; r[1]=v[1]; r[2]=v[2]; }
static inline void mu_vec3_to_vec4(float* r, const float* v, float w) { r[0]=v[0]; r[1]=v[1]; r[2]=v[2]; r[3]=w; }

static inline void mu_vec3_add(float* r, const float* a, const float* b) { r[0]=a[0]+b[0]; r[1]=a[1]+b[1]; r[2]=a[2]+b[2]; }
static inline void mu_vec3_sub(float* r, const float* a, const float* b) { r[0]=a[0]-b[0]; r[1]=a[1]-b[1]; r[2]=a[2]-b[2]; }
static inline void mu_vec3_mul(float* r, const float* a, const float* b) { r[0]=a[0]*b[0]; r[1]=a[1]*b[1]; r[2]=a[2]*b[2]; }
static inline void mu_vec3_scale(float* r, const float* a, float s) { r[0]=a[0]*s; r[1]=a[1]*s; r[2]=a[2]*s; }
static inline void mu_vec3_negate(float* r, const float* a) { r[0]=-a[0]; r[1]=-a[1]; r[2]=-a[2]; }

static inline void mu_vec3_min(float* r, const float* a, const float* b)
{
    r[0] = a[0]<b[0]?a[0]:b[0]; r[1] = a[1]<b[1]?a[1]:b[1]; r[2] = a[2]<b[2]?a[2]:b[2];
}
static inline void mu_vec3_max(float* r, const float* a, const float* b)
{
    r[0] = a[0]>b[0]?a[0]:b[0]; r[1] = a[1]>b[1]?a[1]:b[1]; r[2] = a[2]>b[2]?a[2]:b[2];
}
static inline void mu_vec3_abs(float* r, const float* a) { r[0]=fabsf(a[0]); r[1]=fabsf(a[1]); r[2]=fabsf(a[2]); }

static inline void mu_vec3_lerp(float* r, const float* a, const float* b, float t)
{
    r[0]=a[0]+(b[0]-a[0])*t; r[1]=a[1]+(b[1]-a[1])*t; r[2]=a[2]+(b[2]-a[2])*t;
}
static inline void mu_vec3_muladd(float* r, const float* a, float s, const float* b)
{
    r[0]=a[0]*s+b[0]; r[1]=a[1]*s+b[1]; r[2]=a[2]*s+b[2];
}

static inline float mu_vec3_dot(const float* a, const float* b)
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static inline float mu_vec3_dot2(const float* a, const float* b)
{
    return a[0]*b[0] + a[1]*b[1];
}
static inline void mu_vec3_cross(float* r, const float* a, const float* b)
{
    r[0] = a[1]*b[2] - a[2]*b[1];
    r[1] = a[2]*b[0] - a[0]*b[2];
    r[2] = a[0]*b[1] - a[1]*b[0];
}

static inline float mu_vec3_length2(const float* v) { return mu_vec3_dot(v,v); }
static inline float mu_vec3_length(const float* v)  { return sqrtf(mu_vec3_dot(v,v)); }
static inline float mu_vec3_distance2(const float* a, const float* b)
{
    float dx=a[0]-b[0], dy=a[1]-b[1], dz=a[2]-b[2];
    return dx*dx+dy*dy+dz*dz;
}
static inline float mu_vec3_distance(const float* a, const float* b) { return sqrtf(mu_vec3_distance2(a,b)); }

static inline void mu_vec3_normalize(float* r, const float* v)
{
    float len = mu_vec3_length(v);
    if (!(len > 0.0f)) { mu_vec3_zero(r); return; }
    float inv = 1.0f / len;
    r[0]=v[0]*inv; r[1]=v[1]*inv; r[2]=v[2]*inv;
}
static inline void mu_vec3_normalize_fast(float* r, const float* v)
{
    float inv = mu_rsqrt_f32(mu_vec3_length2(v));
    r[0]=v[0]*inv; r[1]=v[1]*inv; r[2]=v[2]*inv;
}

static inline void mu_vec3_reflect(float* r, const float* v, const float* n)
{
    float d = mu_vec3_dot(v, n);
    r[0] = v[0] - 2.0f*d*n[0];
    r[1] = v[1] - 2.0f*d*n[1];
    r[2] = v[2] - 2.0f*d*n[2];
}
static inline void mu_vec3_project(float* r, const float* v, const float* onto)
{
    float d = mu_vec3_dot(v, onto);
    r[0]=onto[0]*d; r[1]=onto[1]*d; r[2]=onto[2]*d;
}

/* ====================================================================== *
 * VEC4                                                                   *
 * ====================================================================== */

static inline void mu_vec4_make(float* r, float x, float y, float z, float w)
{ r[0]=x; r[1]=y; r[2]=z; r[3]=w; }
static inline void mu_vec4_splat(float* r, float s) { r[0]=r[1]=r[2]=r[3]=s; }
static inline void mu_vec4_zero(float* r) { r[0]=r[1]=r[2]=r[3]=0.0f; }
static inline void mu_vec4_copy(float* r, const float* s) { r[0]=s[0]; r[1]=s[1]; r[2]=s[2]; r[3]=s[3]; }

static inline void mu_vec4_add(float* r, const float* a, const float* b)
{
#if defined(MU_RAW_SSE)
    _mm_storeu_ps(r, _mm_add_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
#else
    r[0]=a[0]+b[0]; r[1]=a[1]+b[1]; r[2]=a[2]+b[2]; r[3]=a[3]+b[3];
#endif
}
static inline void mu_vec4_sub(float* r, const float* a, const float* b)
{
#if defined(MU_RAW_SSE)
    _mm_storeu_ps(r, _mm_sub_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
#else
    r[0]=a[0]-b[0]; r[1]=a[1]-b[1]; r[2]=a[2]-b[2]; r[3]=a[3]-b[3];
#endif
}
static inline void mu_vec4_mul(float* r, const float* a, const float* b)
{
#if defined(MU_RAW_SSE)
    _mm_storeu_ps(r, _mm_mul_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
#else
    r[0]=a[0]*b[0]; r[1]=a[1]*b[1]; r[2]=a[2]*b[2]; r[3]=a[3]*b[3];
#endif
}
static inline void mu_vec4_scale(float* r, const float* a, float s)
{
#if defined(MU_RAW_SSE)
    _mm_storeu_ps(r, _mm_mul_ps(_mm_loadu_ps(a), _mm_set1_ps(s)));
#else
    r[0]=a[0]*s; r[1]=a[1]*s; r[2]=a[2]*s; r[3]=a[3]*s;
#endif
}
static inline float mu_vec4_dot(const float* a, const float* b)
{
#if defined(MU_RAW_SSE41)
    __m128 p = _mm_dp_ps(_mm_loadu_ps(a), _mm_loadu_ps(b), 0xF1);
    return _mm_cvtss_f32(p);
#elif defined(MU_RAW_HADD)
    __m128 p = _mm_mul_ps(_mm_loadu_ps(a), _mm_loadu_ps(b));
    __m128 s = _mm_hadd_ps(p, p);
    s = _mm_hadd_ps(s, s);
    return _mm_cvtss_f32(s);
#else
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
#endif
}
static inline float mu_vec4_length2(const float* a) { return mu_vec4_dot(a,a); }
static inline float mu_vec4_length(const float* a) { return sqrtf(mu_vec4_dot(a,a)); }

static inline void mu_vec4_normalize(float* r, const float* a)
{
    float len = mu_vec4_length(a);
    if (len <= 1e-20f) { mu_vec4_zero(r); return; }
    float inv = 1.0f/len;
    r[0]=a[0]*inv; r[1]=a[1]*inv; r[2]=a[2]*inv; r[3]=a[3]*inv;
}

static inline void mu_vec4_lerp(float* r, const float* a, const float* b, float t)
{
#if defined(MU_RAW_SSE)
    __m128 va = _mm_loadu_ps(a);
    __m128 vt = _mm_set1_ps(t);
    _mm_storeu_ps(r, _mm_add_ps(va, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(b), va), vt)));
#else
    r[0]=a[0]+(b[0]-a[0])*t; r[1]=a[1]+(b[1]-a[1])*t;
    r[2]=a[2]+(b[2]-a[2])*t; r[3]=a[3]+(b[3]-a[3])*t;
#endif
}

/* ====================================================================== *
 * QUATERNION (xyzw)                                                      *
 * ====================================================================== */

static inline void mu_quat_identity(float* r) { r[0]=0; r[1]=0; r[2]=0; r[3]=1; }

static inline void mu_quat_axis_angle(float* r, const float* axis, float radians)
{
    float n[3];
    mu_vec3_normalize(n, axis);
    float h = 0.5f * radians;
    float s = sinf(h);
    r[0]=n[0]*s; r[1]=n[1]*s; r[2]=n[2]*s; r[3]=cosf(h);
}

static inline void mu_quat_mul(float* r, const float* a, const float* b)
{
    r[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    r[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    r[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    r[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

static inline void mu_quat_conjugate(float* r, const float* q) { r[0]=-q[0]; r[1]=-q[1]; r[2]=-q[2]; r[3]=q[3]; }
static inline float mu_quat_dot(const float* a, const float* b) { return mu_vec4_dot(a,b); }

static inline void mu_quat_normalize(float* r, const float* q)
{
    float inv = mu_rsqrt_f32(mu_vec4_dot(q,q));
    mu_vec4_scale(r, q, inv);
}

static inline void mu_quat_rotate_vec3(float* r, const float* q, const float* v)
{
    float qv[3], t[3], c[3];
    qv[0]=q[0]; qv[1]=q[1]; qv[2]=q[2];
    mu_vec3_cross(t, qv, v);
    mu_vec3_scale(t, t, 2.0f);
    mu_vec3_cross(c, qv, t);
    r[0] = v[0] + q[3]*t[0] + c[0];
    r[1] = v[1] + q[3]*t[1] + c[1];
    r[2] = v[2] + q[3]*t[2] + c[2];
}

static inline void mu_quat_slerp(float* r, const float* a, const float* b, float t)
{
    float d = mu_vec4_dot(a, b);
    float orig[4]; mu_vec4_copy(orig, a);
    float aa[4]; mu_vec4_copy(aa, a);

    if (fabsf(d) >= 1.0f) { mu_vec4_copy(r, a); return; }
    if (d < 0.0f) { mu_vec4_scale(aa, aa, -1.0f); d = -d; }

    float sin_theta = sqrtf(1.0f - d*d);
    if (fabsf(sin_theta) < 0.001f) { mu_vec4_lerp(r, orig, b, t); return; }

    float angle = acosf(d);
    float s0 = sinf((1.0f-t)*angle);
    float s1 = sinf(t*angle);
    float tmp[4];
    mu_vec4_scale(tmp, aa, s0);
    float tmp2[4];
    mu_vec4_scale(tmp2, b, s1);
    mu_vec4_add(r, tmp, tmp2);
    mu_vec4_scale(r, r, 1.0f/sin_theta);
}

static inline void mu_quat_nlerp(float* r, const float* a, const float* b, float t)
{
    float bb[4]; mu_vec4_copy(bb, b);
    if (mu_vec4_dot(a,b) < 0.0f) mu_vec4_scale(bb, bb, -1.0f);
    float tmp[4];
    mu_vec4_sub(tmp, bb, a);
    mu_vec4_scale(tmp, tmp, t);
    mu_vec4_add(r, a, tmp);
    float inv = mu_rsqrt_f32(mu_vec4_dot(r,r));
    mu_vec4_scale(r, r, inv);
}

/* ====================================================================== *
 * MAT3 (column-major, 9 floats: col0[0..2] col1[0..2] col2[0..2])      *
 * ====================================================================== */

static inline void mu_mat3_identity(float* r)
{
    r[0]=1; r[1]=0; r[2]=0;
    r[3]=0; r[4]=1; r[5]=0;
    r[6]=0; r[7]=0; r[8]=1;
}

#define MU__M3(c,row) m[(c)*3+(row)]

static inline void mu_mat3_mulv(float* r, const float* m, const float* v)
{
    r[0] = m[0]*v[0] + m[3]*v[1] + m[6]*v[2];
    r[1] = m[1]*v[0] + m[4]*v[1] + m[7]*v[2];
    r[2] = m[2]*v[0] + m[5]*v[1] + m[8]*v[2];
}

static inline void mu_mat3_mul(float* r, const float* a, const float* b)
{
    float tmp[9];
    mu_mat3_mulv(&tmp[0], a, &b[0]);
    mu_mat3_mulv(&tmp[3], a, &b[3]);
    mu_mat3_mulv(&tmp[6], a, &b[6]);
    memcpy(r, tmp, 36);
}

static inline void mu_mat3_from_mat4_linear(float* r, const float* m)
{
    /* m is a mu_mat4 (16 floats), extract upper-left 3x3 */
    r[0]=m[0]; r[1]=m[1]; r[2]=m[2];
    r[3]=m[4]; r[4]=m[5]; r[5]=m[6];
    r[6]=m[8]; r[7]=m[9]; r[8]=m[10];
}

static inline void mu_mat3_inverse(float* r, const float* m)
{
    float c0[3]={m[0],m[1],m[2]}, c1[3]={m[3],m[4],m[5]}, c2[3]={m[6],m[7],m[8]};
    float x12[3], x20[3], x01[3];
    mu_vec3_cross(x12, c1, c2);
    float det = mu_vec3_dot(c0, x12);
    float inv = (det != 0.0f) ? (1.0f/det) : 0.0f;
    mu_vec3_cross(x20, c2, c0);
    mu_vec3_cross(x01, c0, c1);
    /* rows of adjugate become columns (transposed) */
    r[0]=x12[0]*inv; r[1]=x20[0]*inv; r[2]=x01[0]*inv;
    r[3]=x12[1]*inv; r[4]=x20[1]*inv; r[5]=x01[1]*inv;
    r[6]=x12[2]*inv; r[7]=x20[2]*inv; r[8]=x01[2]*inv;
}

static inline void mu_mat3_transpose(float* r, const float* m)
{
    r[0]=m[0]; r[1]=m[3]; r[2]=m[6];
    r[3]=m[1]; r[4]=m[4]; r[5]=m[7];
    r[6]=m[2]; r[7]=m[5]; r[8]=m[8];
}

static inline void mu_mat3_inverse_transpose(float* r, const float* m)
{
    float inv[9];
    mu_mat3_inverse(inv, m);
    mu_mat3_transpose(r, inv);
}

static inline void mu_mat3_cofactor(float* r, const float* m)
{
    float c0[3]={m[0],m[1],m[2]}, c1[3]={m[3],m[4],m[5]}, c2[3]={m[6],m[7],m[8]};
    mu_vec3_cross(&r[0], c1, c2);
    mu_vec3_cross(&r[3], c2, c0);
    mu_vec3_cross(&r[6], c0, c1);
}

#undef MU__M3

/* ====================================================================== *
 * MAT4 CONSTRUCTION                                                      *
 * ====================================================================== */

static inline void mu_mat4_zero(float* r) { memset(r, 0, 64); }

static inline void mu_mat4_identity(float* r)
{
    memset(r, 0, 64);
    r[0]=r[5]=r[10]=r[15]=1.0f;
}

static inline void mu_mat4_from_quat(float* r, const float* q)
{
    float xx=q[0]*q[0], yy=q[1]*q[1], zz=q[2]*q[2];
    float xy=q[0]*q[1], xz=q[0]*q[2], yz=q[1]*q[2];
    float wx=q[3]*q[0], wy=q[3]*q[1], wz=q[3]*q[2];

    r[0]=1.0f-2.0f*(yy+zz); r[1]=2.0f*(xy+wz);       r[2]=2.0f*(xz-wy);       r[3]=0.0f;
    r[4]=2.0f*(xy-wz);       r[5]=1.0f-2.0f*(xx+zz); r[6]=2.0f*(yz+wx);       r[7]=0.0f;
    r[8]=2.0f*(xz+wy);       r[9]=2.0f*(yz-wx);       r[10]=1.0f-2.0f*(xx+yy); r[11]=0.0f;
    r[12]=0.0f;              r[13]=0.0f;              r[14]=0.0f;              r[15]=1.0f;
}

static inline void mu_mat4_trs(float* r, const float* translation, const float* rotation, const float* scale)
{
    mu_mat4_from_quat(r, rotation);
    r[0]*=scale[0]; r[1]*=scale[0]; r[2]*=scale[0];
    r[4]*=scale[1]; r[5]*=scale[1]; r[6]*=scale[1];
    r[8]*=scale[2]; r[9]*=scale[2]; r[10]*=scale[2];
    r[12]=translation[0]; r[13]=translation[1]; r[14]=translation[2]; r[15]=1.0f;
}

static inline void mu_mat4_translate(float* r, const float* t)
{
    mu_mat4_identity(r);
    r[12]=t[0]; r[13]=t[1]; r[14]=t[2];
}

static inline void mu_mat4_scale(float* r, const float* s)
{
    mu_mat4_identity(r);
    r[0]=s[0]; r[5]=s[1]; r[10]=s[2];
}

static inline void mu_mat4_rotate(float* r, const float* axis, float radians)
{
    float q[4];
    mu_quat_axis_angle(q, axis, radians);
    mu_mat4_from_quat(r, q);
}

static inline void mu_mat4_perspective(float* r, float fovy_rad, float aspect, float znear, float zfar)
{
    float f = 1.0f / tanf(0.5f * fovy_rad);
    mu_mat4_zero(r);
    r[0] = f / aspect;
    r[5] = f;
    float nf = 1.0f / (znear - zfar);
    r[10] = (zfar + znear) * nf;
    r[11] = -1.0f;
    r[14] = 2.0f * zfar * znear * nf;
}

/* Right-handed, zero-to-one depth (D3D / Vulkan / Metal / WebGPU): a point at
 * z = -znear maps to ndc z = 0, a point at z = -zfar maps to ndc z = 1. The
 * OpenGL [-1,1] variant above is `mu_mat4_perspective`. Vulkan additionally
 * wants the Y flip; apply it to the view matrix (or negate r[5]) at the call
 * site so this stays the same projection every other API sees. */
static inline void mu_mat4_perspective_zo(float* r, float fovy_rad, float aspect, float znear, float zfar)
{
    float f = 1.0f / tanf(0.5f * fovy_rad);
    float nf = 1.0f / (znear - zfar);
    mu_mat4_zero(r);
    r[0] = f / aspect;
    r[5] = f;
    r[10] = zfar * nf;
    r[11] = -1.0f;
    r[14] = zfar * znear * nf;
}

static inline void mu_mat4_ortho(float* r, float left, float right, float bottom, float top, float znear, float zfar)
{
    mu_mat4_identity(r);
    r[0] = 2.0f/(right-left);
    r[5] = 2.0f/(top-bottom);
    r[10] = -2.0f/(zfar-znear);
    r[12] = -(right+left)/(right-left);
    r[13] = -(top+bottom)/(top-bottom);
    r[14] = -(zfar+znear)/(zfar-znear);
}

static inline void mu_mat4_look_at(float* r, const float* eye, const float* target, const float* up)
{
    float f[3], s[3], u[3], tmp[3];
    mu_vec3_sub(tmp, target, eye);
    mu_vec3_normalize(f, tmp);
    mu_vec3_cross(s, f, up);
    mu_vec3_normalize(s, s);
    mu_vec3_cross(u, s, f);

    r[0]=s[0];  r[1]=u[0];  r[2]=-f[0]; r[3]=0.0f;
    r[4]=s[1];  r[5]=u[1];  r[6]=-f[1]; r[7]=0.0f;
    r[8]=s[2];  r[9]=u[2];  r[10]=-f[2]; r[11]=0.0f;
    r[12]=-mu_vec3_dot(s,eye);
    r[13]=-mu_vec3_dot(u,eye);
    r[14]= mu_vec3_dot(f,eye);
    r[15]=1.0f;
}

/* ====================================================================== *
 * MAT4 ARITHMETIC                                                        *
 * ====================================================================== */

static inline void mu_mat4_mul(const float* a, const float* b, float* dst)
{
#if defined(MU_RAW_SSE)
    __m128 l, v0, v1, v2, v3;
    __m128 r0 = _mm_loadu_ps(&b[0]);
    __m128 r1 = _mm_loadu_ps(&b[4]);
    __m128 r2 = _mm_loadu_ps(&b[8]);
    __m128 r3 = _mm_loadu_ps(&b[12]);

#define MU__SPLAT_X(v) _mm_shuffle_ps((v),(v),_MM_SHUFFLE(0,0,0,0))
#define MU__SPLAT_Y(v) _mm_shuffle_ps((v),(v),_MM_SHUFFLE(1,1,1,1))
#define MU__SPLAT_Z(v) _mm_shuffle_ps((v),(v),_MM_SHUFFLE(2,2,2,2))
#define MU__SPLAT_W(v) _mm_shuffle_ps((v),(v),_MM_SHUFFLE(3,3,3,3))
#if defined(MU_RAW_FMA)
#  define MU__FMA(a,b,c) _mm_fmadd_ps((a),(b),(c))
#else
#  define MU__FMA(a,b,c) _mm_add_ps(_mm_mul_ps((a),(b)),(c))
#endif

    l  = _mm_loadu_ps(&a[0]);
    v0 = _mm_mul_ps(MU__SPLAT_X(r0), l);
    v1 = _mm_mul_ps(MU__SPLAT_X(r1), l);
    v2 = _mm_mul_ps(MU__SPLAT_X(r2), l);
    v3 = _mm_mul_ps(MU__SPLAT_X(r3), l);
    l  = _mm_loadu_ps(&a[4]);
    v0 = MU__FMA(MU__SPLAT_Y(r0), l, v0);
    v1 = MU__FMA(MU__SPLAT_Y(r1), l, v1);
    v2 = MU__FMA(MU__SPLAT_Y(r2), l, v2);
    v3 = MU__FMA(MU__SPLAT_Y(r3), l, v3);
    l  = _mm_loadu_ps(&a[8]);
    v0 = MU__FMA(MU__SPLAT_Z(r0), l, v0);
    v1 = MU__FMA(MU__SPLAT_Z(r1), l, v1);
    v2 = MU__FMA(MU__SPLAT_Z(r2), l, v2);
    v3 = MU__FMA(MU__SPLAT_Z(r3), l, v3);
    l  = _mm_loadu_ps(&a[12]);
    v0 = MU__FMA(MU__SPLAT_W(r0), l, v0);
    v1 = MU__FMA(MU__SPLAT_W(r1), l, v1);
    v2 = MU__FMA(MU__SPLAT_W(r2), l, v2);
    v3 = MU__FMA(MU__SPLAT_W(r3), l, v3);

    _mm_storeu_ps(&dst[0], v0);
    _mm_storeu_ps(&dst[4], v1);
    _mm_storeu_ps(&dst[8], v2);
    _mm_storeu_ps(&dst[12], v3);
#undef MU__SPLAT_X
#undef MU__SPLAT_Y
#undef MU__SPLAT_Z
#undef MU__SPLAT_W
#undef MU__FMA
#else
    int j, k;
    for (j = 0; j < 4; j++) {
        float r0=0,r1=0,r2=0,r3=0;
        for (k = 0; k < 4; k++) {
            float s = b[j*4+k];
            r0 += s * a[k*4+0];
            r1 += s * a[k*4+1];
            r2 += s * a[k*4+2];
            r3 += s * a[k*4+3];
        }
        dst[j*4+0]=r0; dst[j*4+1]=r1; dst[j*4+2]=r2; dst[j*4+3]=r3;
    }
#endif
}

static inline void mu_mat4_mulv(float* r, const float* m, const float* v)
{
#if defined(MU_RAW_SSE)
#if defined(MU_RAW_FMA)
#  define MU__FMA(a,b,c) _mm_fmadd_ps((a),(b),(c))
#else
#  define MU__FMA(a,b,c) _mm_add_ps(_mm_mul_ps((a),(b)),(c))
#endif
    __m128 x = _mm_loadu_ps(v);
    __m128 r0 = _mm_mul_ps(_mm_shuffle_ps(x,x,_MM_SHUFFLE(0,0,0,0)), _mm_loadu_ps(&m[0]));
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(1,1,1,1)), _mm_loadu_ps(&m[4]), r0);
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(2,2,2,2)), _mm_loadu_ps(&m[8]), r0);
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(3,3,3,3)), _mm_loadu_ps(&m[12]), r0);
    _mm_storeu_ps(r, r0);
#undef MU__FMA
#else
    r[0] = m[0]*v[0]+m[4]*v[1]+m[8]*v[2]+m[12]*v[3];
    r[1] = m[1]*v[0]+m[5]*v[1]+m[9]*v[2]+m[13]*v[3];
    r[2] = m[2]*v[0]+m[6]*v[1]+m[10]*v[2]+m[14]*v[3];
    r[3] = m[3]*v[0]+m[7]*v[1]+m[11]*v[2]+m[15]*v[3];
#endif
}

static inline void mu_mat4_transform_point(float* r, const float* m, const float* p)
{
#if defined(MU_RAW_SSE)
#if defined(MU_RAW_FMA)
#  define MU__FMA(a,b,c) _mm_fmadd_ps((a),(b),(c))
#else
#  define MU__FMA(a,b,c) _mm_add_ps(_mm_mul_ps((a),(b)),(c))
#endif
    __m128 x = _mm_set_ps(1.0f, p[2], p[1], p[0]);
    __m128 r0 = _mm_mul_ps(_mm_shuffle_ps(x,x,_MM_SHUFFLE(0,0,0,0)), _mm_loadu_ps(&m[0]));
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(1,1,1,1)), _mm_loadu_ps(&m[4]), r0);
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(2,2,2,2)), _mm_loadu_ps(&m[8]), r0);
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(3,3,3,3)), _mm_loadu_ps(&m[12]), r0);
    r[0] = _mm_cvtss_f32(r0);
    r[1] = _mm_cvtss_f32(_mm_shuffle_ps(r0,r0,_MM_SHUFFLE(1,1,1,1)));
    r[2] = _mm_cvtss_f32(_mm_movehl_ps(r0,r0));
#undef MU__FMA
#else
    r[0] = m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12];
    r[1] = m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13];
    r[2] = m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14];
#endif
}

static inline void mu_mat4_transform_dir(float* r, const float* m, const float* d)
{
#if defined(MU_RAW_SSE)
#if defined(MU_RAW_FMA)
#  define MU__FMA(a,b,c) _mm_fmadd_ps((a),(b),(c))
#else
#  define MU__FMA(a,b,c) _mm_add_ps(_mm_mul_ps((a),(b)),(c))
#endif
    __m128 x = _mm_set_ps(0.0f, d[2], d[1], d[0]);
    __m128 r0 = _mm_mul_ps(_mm_shuffle_ps(x,x,_MM_SHUFFLE(0,0,0,0)), _mm_loadu_ps(&m[0]));
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(1,1,1,1)), _mm_loadu_ps(&m[4]), r0);
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(2,2,2,2)), _mm_loadu_ps(&m[8]), r0);
    r[0] = _mm_cvtss_f32(r0);
    r[1] = _mm_cvtss_f32(_mm_shuffle_ps(r0,r0,_MM_SHUFFLE(1,1,1,1)));
    r[2] = _mm_cvtss_f32(_mm_movehl_ps(r0,r0));
#undef MU__FMA
#else
    r[0] = m[0]*d[0]+m[4]*d[1]+m[8]*d[2];
    r[1] = m[1]*d[0]+m[5]*d[1]+m[9]*d[2];
    r[2] = m[2]*d[0]+m[6]*d[1]+m[10]*d[2];
#endif
}

/* ---- hot path: matrix pre-loaded into registers ---- */

/* ---- affine multiply ---------------------------------------------------
 * B's fourth row is (0,0,0,1): the terms that would carry A's translation
 * into columns 0..2 are B[3][j] * A_col3 with B[3][j] == 0, so those three
 * adds disappear (9 FMA instead of 12). Column 3 is the transformed
 * translation and keeps its add. Result is bit-identical to mu_mat4_mul for an
 * affine B (the dropped terms are exact zeros). */
static inline void mu_mat4_mul_affine(float* dst, const float* a, const float* b)
{
#if defined(MU_RAW_SSE)
#if defined(MU_RAW_FMA)
#  define MU__FMA(a,b,c) _mm_fmadd_ps((a),(b),(c))
#else
#  define MU__FMA(a,b,c) _mm_add_ps(_mm_mul_ps((a),(b)),(c))
#endif
#  define MU__SX(v) _mm_shuffle_ps((v),(v),_MM_SHUFFLE(0,0,0,0))
#  define MU__SY(v) _mm_shuffle_ps((v),(v),_MM_SHUFFLE(1,1,1,1))
#  define MU__SZ(v) _mm_shuffle_ps((v),(v),_MM_SHUFFLE(2,2,2,2))
    __m128 r0 = _mm_loadu_ps(&b[0]);
    __m128 r1 = _mm_loadu_ps(&b[4]);
    __m128 r2 = _mm_loadu_ps(&b[8]);
    __m128 r3 = _mm_loadu_ps(&b[12]);
    __m128 l0 = _mm_loadu_ps(&a[0]);
    __m128 l1 = _mm_loadu_ps(&a[4]);
    __m128 l2 = _mm_loadu_ps(&a[8]);
    __m128 t  = _mm_loadu_ps(&a[12]);
    __m128 v0, v1, v2, v3;

    v0 = MU__FMA(MU__SY(r0), l1, _mm_mul_ps(MU__SX(r0), l0));
    v1 = MU__FMA(MU__SY(r1), l1, _mm_mul_ps(MU__SX(r1), l0));
    v2 = MU__FMA(MU__SY(r2), l1, _mm_mul_ps(MU__SX(r2), l0));

    v0 = MU__FMA(MU__SZ(r0), l2, v0);
    v1 = MU__FMA(MU__SZ(r1), l2, v1);
    v2 = MU__FMA(MU__SZ(r2), l2, v2);

    v3 = MU__FMA(MU__SZ(r3), l2,
                 MU__FMA(MU__SY(r3), l1, _mm_mul_ps(MU__SX(r3), l0)));
    v3 = _mm_add_ps(v3, t);

    _mm_storeu_ps(&dst[0], v0);
    _mm_storeu_ps(&dst[4], v1);
    _mm_storeu_ps(&dst[8], v2);
    _mm_storeu_ps(&dst[12], v3);
#  undef MU__SX
#  undef MU__SY
#  undef MU__SZ
#  undef MU__FMA
#else
    mu_mat4_mul(a, b, dst);
#endif
}

/* ---- 4-wide palette (lane == matrix) ---------------------------------- */
static inline void mu_mat4x4_pack(mu_mat4x4* out, const float* m, size_t stride_bytes)
{
#if defined(MU_RAW_AVX512)
    /* Four AoS matrices -> lane=matrix form is a 4x4 transpose at 128-bit
     * granularity: 4 loads + 8 vshuff32x4. _mm512_insertf32x4 costs 12
     * inserts, and that difference is the whole margin of the palette bench. */
    __m512 a0 = _mm512_loadu_ps((const float*)(const void*)((const char*)m));
    __m512 a1 = _mm512_loadu_ps((const float*)(const void*)((const char*)m + stride_bytes));
    __m512 a2 = _mm512_loadu_ps((const float*)(const void*)((const char*)m + 2 * stride_bytes));
    __m512 a3 = _mm512_loadu_ps((const float*)(const void*)((const char*)m + 3 * stride_bytes));
    __m512 t0 = _mm512_shuffle_f32x4(a0, a1, _MM_SHUFFLE(1, 0, 1, 0));
    __m512 t1 = _mm512_shuffle_f32x4(a0, a1, _MM_SHUFFLE(3, 2, 3, 2));
    __m512 t2 = _mm512_shuffle_f32x4(a2, a3, _MM_SHUFFLE(1, 0, 1, 0));
    __m512 t3 = _mm512_shuffle_f32x4(a2, a3, _MM_SHUFFLE(3, 2, 3, 2));

    out->col[0] = _mm512_shuffle_f32x4(t0, t2, _MM_SHUFFLE(2, 0, 2, 0));
    out->col[1] = _mm512_shuffle_f32x4(t0, t2, _MM_SHUFFLE(3, 1, 3, 1));
    out->col[2] = _mm512_shuffle_f32x4(t1, t3, _MM_SHUFFLE(2, 0, 2, 0));
    out->col[3] = _mm512_shuffle_f32x4(t1, t3, _MM_SHUFFLE(3, 1, 3, 1));
#else
    size_t k;
    for (k = 0; k < 4; ++k)
        memcpy(out->m[k], (const void*)((const char*)m + k * stride_bytes), sizeof(mu_mat4));
#endif
}

static inline void mu_mat4x4_unpack(float* m, size_t stride_bytes, const mu_mat4x4* p)
{
#if defined(MU_RAW_AVX512)
    /* the same lane transpose in reverse: it is its own inverse */
    __m512 t0 = _mm512_shuffle_f32x4(p->col[0], p->col[1], _MM_SHUFFLE(1, 0, 1, 0));
    __m512 t1 = _mm512_shuffle_f32x4(p->col[0], p->col[1], _MM_SHUFFLE(3, 2, 3, 2));
    __m512 t2 = _mm512_shuffle_f32x4(p->col[2], p->col[3], _MM_SHUFFLE(1, 0, 1, 0));
    __m512 t3 = _mm512_shuffle_f32x4(p->col[2], p->col[3], _MM_SHUFFLE(3, 2, 3, 2));

    _mm512_storeu_ps((float*)(void*)((char*)m),
                     _mm512_shuffle_f32x4(t0, t2, _MM_SHUFFLE(2, 0, 2, 0)));
    _mm512_storeu_ps((float*)(void*)((char*)m + stride_bytes),
                     _mm512_shuffle_f32x4(t0, t2, _MM_SHUFFLE(3, 1, 3, 1)));
    _mm512_storeu_ps((float*)(void*)((char*)m + 2 * stride_bytes),
                     _mm512_shuffle_f32x4(t1, t3, _MM_SHUFFLE(2, 0, 2, 0)));
    _mm512_storeu_ps((float*)(void*)((char*)m + 3 * stride_bytes),
                     _mm512_shuffle_f32x4(t1, t3, _MM_SHUFFLE(3, 1, 3, 1)));
#else
    size_t k;
    for (k = 0; k < 4; ++k)
        memcpy((void*)((char*)m + k * stride_bytes), p->m[k], sizeof(mu_mat4));
#endif
}

static inline void mu_mat4x4_mul(mu_mat4x4* dst, const mu_mat4x4* a, const mu_mat4x4* b)
{
#if defined(MU_RAW_AVX512)
    const __m512i k0 = _mm512_set_epi32(12,12,12,12, 8, 8, 8, 8, 4, 4, 4, 4, 0, 0, 0, 0);
    const __m512i k1 = _mm512_set_epi32(13,13,13,13, 9, 9, 9, 9, 5, 5, 5, 5, 1, 1, 1, 1);
    const __m512i k2 = _mm512_set_epi32(14,14,14,14,10,10,10,10, 6, 6, 6, 6, 2, 2, 2, 2);
    const __m512i k3 = _mm512_set_epi32(15,15,15,15,11,11,11,11, 7, 7, 7, 7, 3, 3, 3, 3);
    __m512 d0, d1, d2, d3;

    d0 = _mm512_mul_ps(a->col[0], _mm512_permutexvar_ps(k0, b->col[0]));
    d1 = _mm512_mul_ps(a->col[0], _mm512_permutexvar_ps(k0, b->col[1]));
    d2 = _mm512_mul_ps(a->col[0], _mm512_permutexvar_ps(k0, b->col[2]));
    d3 = _mm512_mul_ps(a->col[0], _mm512_permutexvar_ps(k0, b->col[3]));

    d0 = _mm512_fmadd_ps(a->col[1], _mm512_permutexvar_ps(k1, b->col[0]), d0);
    d1 = _mm512_fmadd_ps(a->col[1], _mm512_permutexvar_ps(k1, b->col[1]), d1);
    d2 = _mm512_fmadd_ps(a->col[1], _mm512_permutexvar_ps(k1, b->col[2]), d2);
    d3 = _mm512_fmadd_ps(a->col[1], _mm512_permutexvar_ps(k1, b->col[3]), d3);

    d0 = _mm512_fmadd_ps(a->col[2], _mm512_permutexvar_ps(k2, b->col[0]), d0);
    d1 = _mm512_fmadd_ps(a->col[2], _mm512_permutexvar_ps(k2, b->col[1]), d1);
    d2 = _mm512_fmadd_ps(a->col[2], _mm512_permutexvar_ps(k2, b->col[2]), d2);
    d3 = _mm512_fmadd_ps(a->col[2], _mm512_permutexvar_ps(k2, b->col[3]), d3);

    d0 = _mm512_fmadd_ps(a->col[3], _mm512_permutexvar_ps(k3, b->col[0]), d0);
    d1 = _mm512_fmadd_ps(a->col[3], _mm512_permutexvar_ps(k3, b->col[1]), d1);
    d2 = _mm512_fmadd_ps(a->col[3], _mm512_permutexvar_ps(k3, b->col[2]), d2);
    d3 = _mm512_fmadd_ps(a->col[3], _mm512_permutexvar_ps(k3, b->col[3]), d3);

    dst->col[0] = d0;
    dst->col[1] = d1;
    dst->col[2] = d2;
    dst->col[3] = d3;
#else
    size_t k;
    for (k = 0; k < 4; ++k)
        mu_mat4_mul(a->m[k], b->m[k], dst->m[k]);
#endif
}

/* N independent multiplies, four at a time where the machine can: one multiply
 * is latency-bound, four saturate the FMA pipes. */
static inline void mu_mat4_mul_batch(float* dst, size_t stride_d,
                                     const float* a, size_t stride_a,
                                     const float* b, size_t stride_b, size_t count)
{
    size_t i = 0;

#if defined(MU_RAW_AVX512)
    for (; i + 4 <= count; i += 4)
    {
        mu_mat4x4 pa, pb, pd;

        mu_mat4x4_pack(&pa, (const float*)(const void*)((const char*)a + i * stride_a), stride_a);
        mu_mat4x4_pack(&pb, (const float*)(const void*)((const char*)b + i * stride_b), stride_b);
        mu_mat4x4_mul(&pd, &pa, &pb);
        mu_mat4x4_unpack((float*)(void*)((char*)dst + i * stride_d), stride_d, &pd);
    }
#endif
    for (; i < count; ++i)
        mu_mat4_mul((const float*)(const void*)((const char*)a + i * stride_a),
                    (const float*)(const void*)((const char*)b + i * stride_b),
                    (float*)(void*)((char*)dst + i * stride_d));
}

static inline mu_mat4_hot mu_mat4_make_hot(const float* m)
{
    mu_mat4_hot h;
#if defined(MU_RAW_SSE)
    h.c0 = _mm_loadu_ps(&m[0]);
    h.c1 = _mm_loadu_ps(&m[4]);
    h.c2 = _mm_loadu_ps(&m[8]);
    h.c3 = _mm_loadu_ps(&m[12]);
#else
    mu_vec4_copy(h.c0, &m[0]);
    mu_vec4_copy(h.c1, &m[4]);
    mu_vec4_copy(h.c2, &m[8]);
    mu_vec4_copy(h.c3, &m[12]);
#endif
    return h;
}

static inline void mu_mat4_hot_mulv(float* r, const mu_mat4_hot* h, const float* v)
{
#if defined(MU_RAW_SSE)
#if defined(MU_RAW_FMA)
#  define MU__FMA(a,b,c) _mm_fmadd_ps((a),(b),(c))
#else
#  define MU__FMA(a,b,c) _mm_add_ps(_mm_mul_ps((a),(b)),(c))
#endif
    __m128 x = _mm_loadu_ps(v);
    __m128 r0 = _mm_mul_ps(_mm_shuffle_ps(x,x,_MM_SHUFFLE(0,0,0,0)), h->c0);
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(1,1,1,1)), h->c1, r0);
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(2,2,2,2)), h->c2, r0);
    r0 = MU__FMA(_mm_shuffle_ps(x,x,_MM_SHUFFLE(3,3,3,3)), h->c3, r0);
    _mm_storeu_ps(r, r0);
#undef MU__FMA
#else
    r[0] = h->c0[0]*v[0]+h->c1[0]*v[1]+h->c2[0]*v[2]+h->c3[0]*v[3];
    r[1] = h->c0[1]*v[0]+h->c1[1]*v[1]+h->c2[1]*v[2]+h->c3[1]*v[3];
    r[2] = h->c0[2]*v[0]+h->c1[2]*v[1]+h->c2[2]*v[2]+h->c3[2]*v[3];
    r[3] = h->c0[3]*v[0]+h->c1[3]*v[1]+h->c2[3]*v[2]+h->c3[3]*v[3];
#endif
}

static inline void mu_mat4_hot_point(float* r, const mu_mat4_hot* h, const float* p)
{
#if defined(MU_RAW_SSE)
    __m128 x = _mm_set_ps(1.0f, p[2], p[1], p[0]);
    __m128 r0 = _mm_mul_ps(_mm_shuffle_ps(x,x,_MM_SHUFFLE(0,0,0,0)), h->c0);
    r0 = _mm_add_ps(r0, _mm_mul_ps(_mm_shuffle_ps(x,x,_MM_SHUFFLE(1,1,1,1)), h->c1));
    r0 = _mm_add_ps(r0, _mm_mul_ps(_mm_shuffle_ps(x,x,_MM_SHUFFLE(2,2,2,2)), h->c2));
    r0 = _mm_add_ps(r0, h->c3);
    r[0] = _mm_cvtss_f32(r0);
    r[1] = _mm_cvtss_f32(_mm_shuffle_ps(r0,r0,_MM_SHUFFLE(1,1,1,1)));
    r[2] = _mm_cvtss_f32(_mm_movehl_ps(r0,r0));
#else
    r[0] = h->c0[0]*p[0]+h->c1[0]*p[1]+h->c2[0]*p[2]+h->c3[0];
    r[1] = h->c0[1]*p[0]+h->c1[1]*p[1]+h->c2[1]*p[2]+h->c3[1];
    r[2] = h->c0[2]*p[0]+h->c1[2]*p[1]+h->c2[2]*p[2]+h->c3[2];
#endif
}

/* ---- transpose, determinant ---- */

static inline void mu_mat4_transpose(const float* m, float* dst)
{
#if defined(MU_RAW_SSE)
    __m128 r0=_mm_loadu_ps(&m[0]), r1=_mm_loadu_ps(&m[4]);
    __m128 r2=_mm_loadu_ps(&m[8]), r3=_mm_loadu_ps(&m[12]);
    __m128 t0=_mm_unpacklo_ps(r0,r1), t1=_mm_unpackhi_ps(r0,r1);
    __m128 t2=_mm_unpacklo_ps(r2,r3), t3=_mm_unpackhi_ps(r2,r3);
    _mm_storeu_ps(&dst[0],  _mm_movelh_ps(t0,t2));
    _mm_storeu_ps(&dst[4],  _mm_movehl_ps(t2,t0));
    _mm_storeu_ps(&dst[8],  _mm_movelh_ps(t1,t3));
    _mm_storeu_ps(&dst[12], _mm_movehl_ps(t3,t1));
#else
    int i,j;
    for (i=0;i<4;i++) for (j=0;j<4;j++) dst[j*4+i] = m[i*4+j];
#endif
}

static inline float mu_mat4_determinant(const float* m)
{
    float a=m[0],b=m[1],c=m[2],d=m[3];
    float e=m[4],f=m[5],g=m[6],h=m[7];
    float i=m[8],j=m[9],k=m[10],l=m[11];
    float mm=m[12],n=m[13],o=m[14],p=m[15];
    float c1=k*p-l*o,  c2=c*h-d*g,  c3=i*p-l*mm, c4=a*h-d*e;
    float c5=j*p-l*n,  c6=b*h-d*f,  c7=i*n-j*mm, c8=a*f-b*e;
    float c9=j*o-k*n,  c10=b*g-c*f, c11=i*o-k*mm,c12=a*g-c*e;
    return c8*c1 + c4*c9 + c10*c3 + c2*c7 - c12*c5 - c6*c11;
}

/* ---- mat4 inverse (full cofactor, SSE-optimized) ---- */

/* 4x4 inverse by cofactor expansion.
 * Works for any invertible matrix. Column-major storage. */
static inline void mu_mat4_inverse(const float* m, float* dst)
{
    float a00=m[0],a01=m[1],a02=m[2],a03=m[3];
    float a10=m[4],a11=m[5],a12=m[6],a13=m[7];
    float a20=m[8],a21=m[9],a22=m[10],a23=m[11];
    float a30=m[12],a31=m[13],a32=m[14],a33=m[15];

    float b00 = a00*a11 - a01*a10, b01 = a00*a12 - a02*a10;
    float b02 = a00*a13 - a03*a10, b03 = a01*a12 - a02*a11;
    float b04 = a01*a13 - a03*a11, b05 = a02*a13 - a03*a12;
    float b06 = a20*a31 - a21*a30, b07 = a20*a32 - a22*a30;
    float b08 = a20*a33 - a23*a30, b09 = a21*a32 - a22*a31;
    float b10 = a21*a33 - a23*a31, b11 = a22*a33 - a23*a32;

    float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    if (det == 0.0f) { mu_mat4_identity(dst); return; }
    float inv = 1.0f / det;

    dst[0]  = ( a11*b11 - a12*b10 + a13*b09) * inv;
    dst[1]  = (-a01*b11 + a02*b10 - a03*b09) * inv;
    dst[2]  = ( a31*b05 - a32*b04 + a33*b03) * inv;
    dst[3]  = (-a21*b05 + a22*b04 - a23*b03) * inv;
    dst[4]  = (-a10*b11 + a12*b08 - a13*b07) * inv;
    dst[5]  = ( a00*b11 - a02*b08 + a03*b07) * inv;
    dst[6]  = (-a30*b05 + a32*b02 - a33*b01) * inv;
    dst[7]  = ( a20*b05 - a22*b02 + a23*b01) * inv;
    dst[8]  = ( a10*b10 - a11*b08 + a13*b06) * inv;
    dst[9]  = (-a00*b10 + a01*b08 - a03*b06) * inv;
    dst[10] = ( a30*b04 - a31*b02 + a33*b00) * inv;
    dst[11] = (-a20*b04 + a21*b02 - a23*b00) * inv;
    dst[12] = (-a10*b09 + a11*b07 - a12*b06) * inv;
    dst[13] = ( a00*b09 - a01*b07 + a02*b06) * inv;
    dst[14] = (-a30*b03 + a31*b01 - a32*b00) * inv;
    dst[15] = ( a20*b03 - a21*b01 + a22*b00) * inv;
}

/* ---- rigid inverse (transpose + negate translation) ---- */

static inline void mu_inverse_rigid(const float* m, float* dst)
{
    float tx=m[12], ty=m[13], tz=m[14];
    float aa=m[0],bb=m[1],cc=m[2];
    float dd=m[4],ee=m[5],ff=m[6];
    float gg=m[8],hh=m[9],ii=m[10];

    dst[0]=aa; dst[1]=dd; dst[2]=gg; dst[3]=0.0f;
    dst[4]=bb; dst[5]=ee; dst[6]=hh; dst[7]=0.0f;
    dst[8]=cc; dst[9]=ff; dst[10]=ii; dst[11]=0.0f;
    dst[12]=-(aa*tx+bb*ty+cc*tz);
    dst[13]=-(dd*tx+ee*ty+ff*tz);
    dst[14]=-(gg*tx+hh*ty+ii*tz);
    dst[15]=1.0f;
}

/* ---- affine inverse (transpose + scale fixup) ---- */

static inline void mu_inverse_affine(const float* m, float* dst)
{
    float a=m[0],b=m[1],c=m[2];
    float d=m[4],e=m[5],f=m[6];
    float g=m[8],h=m[9],i=m[10];
    float tx=-m[12], ty=-m[13], tz=-m[14];

    float ia = 1.0f/(a*a+b*b+c*c);
    float ib = 1.0f/(d*d+e*e+f*f);
    float ic = 1.0f/(g*g+h*h+i*i);

    dst[0]=a*ia; dst[1]=d*ib; dst[2]=g*ic; dst[3]=0.0f;
    dst[4]=b*ia; dst[5]=e*ib; dst[6]=h*ic; dst[7]=0.0f;
    dst[8]=c*ia; dst[9]=f*ib; dst[10]=i*ic; dst[11]=0.0f;
    dst[12]=a*ia*tx + b*ia*ty + c*ia*tz;
    dst[13]=d*ib*tx + e*ib*ty + f*ib*tz;
    dst[14]=g*ic*tx + h*ic*ty + i*ic*tz;
    dst[15]=1.0f;
}

/* ====================================================================== *
 * MAT4 NORMAL (cofactor, from mat4)                                      *
 * ====================================================================== */

static inline void mu_mat4_normal(float* r, const float* m, const float* n)
{
    float c0[3]={m[0],m[1],m[2]};
    float c1[3]={m[4],m[5],m[6]};
    float c2[3]={m[8],m[9],m[10]};
    float r0[3], r1[3], r2[3];
    mu_vec3_cross(r0, c1, c2);
    mu_vec3_cross(r1, c2, c0);
    mu_vec3_cross(r2, c0, c1);
    r[0] = r0[0]*n[0]+r1[0]*n[1]+r2[0]*n[2];
    r[1] = r0[1]*n[0]+r1[1]*n[1]+r2[1]*n[2];
    r[2] = r0[2]*n[0]+r1[2]*n[1]+r2[2]*n[2];
}

/* ====================================================================== *
 * FRUSTUM                                                                *
 * ====================================================================== */

static inline void mu_frustum_from_view_proj(mu_frustum* f, const float* m)
{
    /* Extract rows from column-major matrix */
    float r0[4] = {m[0],m[4],m[8],m[12]};
    float r1[4] = {m[1],m[5],m[9],m[13]};
    float r2[4] = {m[2],m[6],m[10],m[14]};
    float r3[4] = {m[3],m[7],m[11],m[15]};
    int i;

    mu_vec4_add(f->p[0], r3, r0);  /* left */
    mu_vec4_sub(f->p[1], r3, r0);  /* right */
    mu_vec4_add(f->p[2], r3, r1);  /* bottom */
    mu_vec4_sub(f->p[3], r3, r1);  /* top */
    mu_vec4_add(f->p[4], r3, r2);  /* near */
    mu_vec4_sub(f->p[5], r3, r2);  /* far */

    for (i = 0; i < 6; i++) {
        float inv = mu_rsqrt_f32(f->p[i][0]*f->p[i][0]+f->p[i][1]*f->p[i][1]+f->p[i][2]*f->p[i][2]);
        mu_vec4_scale(f->p[i], f->p[i], inv);
    }
}

static inline int mu_frustum_test_sphere(const mu_frustum* f, const float* center, float radius)
{
    int i;
    for (i = 0; i < 6; i++) {
        float d = f->p[i][0]*center[0]+f->p[i][1]*center[1]+f->p[i][2]*center[2]+f->p[i][3];
        if (d < -radius) return 0;
    }
    return 1;
}

/* ====================================================================== *
 * SoA BATCH KERNELS                                                      *
 * ====================================================================== */

static inline void mu_xform_points3_soa(const float* m,
    const float* xin, const float* yin, const float* zin,
    float* xout, float* yout, float* zout, size_t n)
{
    size_t i;
#if defined(MU_RAW_AVX512)
    {
        const __m512 m00=_mm512_set1_ps(m[0]),m01=_mm512_set1_ps(m[4]),m02=_mm512_set1_ps(m[8]),m03=_mm512_set1_ps(m[12]);
        const __m512 m10=_mm512_set1_ps(m[1]),m11=_mm512_set1_ps(m[5]),m12=_mm512_set1_ps(m[9]),m13=_mm512_set1_ps(m[13]);
        const __m512 m20=_mm512_set1_ps(m[2]),m21=_mm512_set1_ps(m[6]),m22=_mm512_set1_ps(m[10]),m23=_mm512_set1_ps(m[14]);
        for (i=0;i+16<=n;i+=16) {
            __m512 x=_mm512_loadu_ps(xin+i), y=_mm512_loadu_ps(yin+i), z=_mm512_loadu_ps(zin+i);
            _mm512_storeu_ps(xout+i, _mm512_fmadd_ps(m02,z,_mm512_fmadd_ps(m01,y,_mm512_fmadd_ps(m00,x,m03))));
            _mm512_storeu_ps(yout+i, _mm512_fmadd_ps(m12,z,_mm512_fmadd_ps(m11,y,_mm512_fmadd_ps(m10,x,m13))));
            _mm512_storeu_ps(zout+i, _mm512_fmadd_ps(m22,z,_mm512_fmadd_ps(m21,y,_mm512_fmadd_ps(m20,x,m23))));
        }
        for (;i<n;++i) {
            xout[i]=m[0]*xin[i]+m[4]*yin[i]+m[8]*zin[i]+m[12];
            yout[i]=m[1]*xin[i]+m[5]*yin[i]+m[9]*zin[i]+m[13];
            zout[i]=m[2]*xin[i]+m[6]*yin[i]+m[10]*zin[i]+m[14];
        }
        return;
    }
#elif defined(MU_RAW_AVX2)
    {
        const __m256 m00=_mm256_set1_ps(m[0]),m01=_mm256_set1_ps(m[4]),m02=_mm256_set1_ps(m[8]),m03=_mm256_set1_ps(m[12]);
        const __m256 m10=_mm256_set1_ps(m[1]),m11=_mm256_set1_ps(m[5]),m12=_mm256_set1_ps(m[9]),m13=_mm256_set1_ps(m[13]);
        const __m256 m20=_mm256_set1_ps(m[2]),m21=_mm256_set1_ps(m[6]),m22=_mm256_set1_ps(m[10]),m23=_mm256_set1_ps(m[14]);
        for (i=0;i+8<=n;i+=8) {
            __m256 x=_mm256_loadu_ps(xin+i), y=_mm256_loadu_ps(yin+i), z=_mm256_loadu_ps(zin+i);
            _mm256_storeu_ps(xout+i, _mm256_fmadd_ps(m02,z,_mm256_fmadd_ps(m01,y,_mm256_fmadd_ps(m00,x,m03))));
            _mm256_storeu_ps(yout+i, _mm256_fmadd_ps(m12,z,_mm256_fmadd_ps(m11,y,_mm256_fmadd_ps(m10,x,m13))));
            _mm256_storeu_ps(zout+i, _mm256_fmadd_ps(m22,z,_mm256_fmadd_ps(m21,y,_mm256_fmadd_ps(m20,x,m23))));
        }
        for (;i<n;++i) {
            xout[i]=m[0]*xin[i]+m[4]*yin[i]+m[8]*zin[i]+m[12];
            yout[i]=m[1]*xin[i]+m[5]*yin[i]+m[9]*zin[i]+m[13];
            zout[i]=m[2]*xin[i]+m[6]*yin[i]+m[10]*zin[i]+m[14];
        }
        return;
    }
#else
    for (i=0;i<n;++i) {
        xout[i]=m[0]*xin[i]+m[4]*yin[i]+m[8]*zin[i]+m[12];
        yout[i]=m[1]*xin[i]+m[5]*yin[i]+m[9]*zin[i]+m[13];
        zout[i]=m[2]*xin[i]+m[6]*yin[i]+m[10]*zin[i]+m[14];
    }
#endif
}

static inline void mu_xform_normals3_soa(const float* nm,
    const float* xin, const float* yin, const float* zin,
    float* xout, float* yout, float* zout, size_t n)
{
    size_t i;
    for (i=0;i<n;++i) {
        xout[i]=nm[0]*xin[i]+nm[3]*yin[i]+nm[6]*zin[i];
        yout[i]=nm[1]*xin[i]+nm[4]*yin[i]+nm[7]*zin[i];
        zout[i]=nm[2]*xin[i]+nm[5]*yin[i]+nm[8]*zin[i];
    }
}

static inline void mu_normalize3_soa(
    const float* xin, const float* yin, const float* zin,
    float* xout, float* yout, float* zout, size_t n)
{
    size_t i;
    for (i=0;i<n;++i) {
        float inv = mu_rsqrt_f32(xin[i]*xin[i]+yin[i]*yin[i]+zin[i]*zin[i]);
        xout[i]=xin[i]*inv; yout[i]=yin[i]*inv; zout[i]=zin[i]*inv;
    }
}

static inline void mu_quat_rotate_vec3_soa(const float* q,
    const float* xin, const float* yin, const float* zin,
    float* xout, float* yout, float* zout, size_t n)
{
    float ux=q[0], uy=q[1], uz=q[2];
    float w2=2.0f*q[3];
    float u2x=2.0f*ux, u2y=2.0f*uy, u2z=2.0f*uz;
    size_t i;
    for (i=0;i<n;++i) {
        float cx = uy*zin[i] - uz*yin[i];
        float cy = uz*xin[i] - ux*zin[i];
        float cz = ux*yin[i] - uy*xin[i];
        float X = xin[i] + w2*cx;
        float Y = yin[i] + w2*cy;
        float Z = zin[i] + w2*cz;
        xout[i] = X + (u2y*cz - u2z*cy);
        yout[i] = Y + (u2z*cx - u2x*cz);
        zout[i] = Z + (u2x*cy - u2y*cx);
    }
}

static inline size_t mu_frustum_cull_spheres_compact(const mu_frustum* f,
    const float* cx, const float* cy, const float* cz, float radius,
    uint32_t* out, size_t n)
{
    size_t i, count=0;
    for (i=0;i<n;++i) {
        float center[3] = {cx[i], cy[i], cz[i]};
        if (mu_frustum_test_sphere(f, center, radius))
            out[count++] = (uint32_t)i;
    }
    return count;
}

#ifdef __cplusplus
}
#endif

#endif /* MU_MATH_H */
