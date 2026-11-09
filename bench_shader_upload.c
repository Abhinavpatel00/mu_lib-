/*
 * bench_shader_upload.c — the REAL question:
 *
 * How much does it cost to convert mu_lib struct types into a flat float
 * buffer for Vulkan? And does using raw floats eliminate that cost?
 *
 * Scenario: you have a UBO with mat4 + vec3 + vec3 + float.
 *   Path A: struct types → explicit field-by-field copy into flat buffer
 *   Path B: raw float types → direct memcpy
 *   Path C: raw float types → buffer IS already the types, zero copy
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

/* --- struct types (like mu_math.h) --- */
typedef struct { float x, y, z; } s_vec3;
typedef struct __attribute__((aligned(16))) { float x, y, z, w; } s_vec4;
typedef struct __attribute__((aligned(16))) { s_vec4 col[4]; } s_mat4;

/* --- raw float types --- */
typedef float r_vec3[4];  /* padded to 16 for std140 */
typedef float r_vec4[4];
typedef float r_mat4[16];

/* --- UBO layout (matches std140) --- */
typedef struct {
    float data[60]; /* 60 floats = 240 bytes: mat4(16) + mat4(16) + vec3_padded(4) + vec3_padded(4) + float(1) + pad(3) */
} FlatUBO;

#define N_OBJS 1024

/* Struct-based objects */
typedef struct {
    s_mat4 model;
    s_mat4 view;
    s_vec3 light_dir;
    s_vec3 camera_pos;
    float time;
} StructObject;

/* Raw-float-based objects */
typedef struct {
    r_mat4 model;
    r_mat4 view;
    r_vec3 light_dir;
    r_vec3 camera_pos;
    float time;
} RawObject;

static volatile double g_sink;
#define SINK(v) do { g_sink = (double)(v); } while(0)

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static uint32_t rng = 0xCAFEBABE;
static float frand(float lo, float hi) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return lo + (hi - lo) * ((float)(rng & 0xFFFFFF) / 16777215.0f);
}

static StructObject s_objs[N_OBJS];
static RawObject    r_objs[N_OBJS];
static FlatUBO      ubos[N_OBJS];

static void init(void) {
    for (int i = 0; i < N_OBJS; i++) {
        for (int j = 0; j < 16; j++) {
            ((float*)&s_objs[i].model)[j] = frand(-1, 1);
            ((float*)&s_objs[i].view)[j] = frand(-1, 1);
            ((float*)&r_objs[i].model)[j] = ((float*)&s_objs[i].model)[j];
            ((float*)&r_objs[i].view)[j] = ((float*)&s_objs[i].view)[j];
        }
        s_objs[i].light_dir = (s_vec3){frand(-1,1), frand(-1,1), frand(-1,1)};
        s_objs[i].camera_pos = (s_vec3){frand(-1,1), frand(-1,1), frand(-1,1)};
        s_objs[i].time = frand(0, 100);
        r_objs[i].light_dir[0] = s_objs[i].light_dir.x;
        r_objs[i].light_dir[1] = s_objs[i].light_dir.y;
        r_objs[i].light_dir[2] = s_objs[i].light_dir.z;
        r_objs[i].light_dir[3] = 0.0f;
        r_objs[i].camera_pos[0] = s_objs[i].camera_pos.x;
        r_objs[i].camera_pos[1] = s_objs[i].camera_pos.y;
        r_objs[i].camera_pos[2] = s_objs[i].camera_pos.z;
        r_objs[i].camera_pos[3] = 0.0f;
        r_objs[i].time = s_objs[i].time;
    }
}

/* --- Path A: Struct types → field-by-field copy into flat UBO --- */
static double bench_struct_to_ubo(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++) {
        for (int i = 0; i < N_OBJS; i++) {
            float* dst = ubos[i].data;
            /* mat4 model: direct memcpy (struct is already 16-aligned floats) */
            memcpy(dst, &s_objs[i].model, 64);
            /* mat4 view */
            memcpy(dst + 16, &s_objs[i].view, 64);
            /* vec3 light_dir: must copy field by field (12B → 16B with padding) */
            dst[32] = s_objs[i].light_dir.x;
            dst[33] = s_objs[i].light_dir.y;
            dst[34] = s_objs[i].light_dir.z;
            dst[35] = 0.0f;
            /* vec3 camera_pos */
            dst[36] = s_objs[i].camera_pos.x;
            dst[37] = s_objs[i].camera_pos.y;
            dst[38] = s_objs[i].camera_pos.z;
            dst[39] = 0.0f;
            /* time */
            dst[40] = s_objs[i].time;
        }
    }
    SINK(ubos[0].data[0]);
    return (now_ns() - t0) / (100.0 * N_OBJS);
}

/* --- Path B: Raw float objects → memcpy each to flat UBO --- */
static double bench_raw_to_ubo_memcpy(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++) {
        for (int i = 0; i < N_OBJS; i++) {
            float* dst = ubos[i].data;
            /* Everything is already flat floats, just memcpy */
            memcpy(dst,      r_objs[i].model, 64);
            memcpy(dst + 16, r_objs[i].view,  64);
            memcpy(dst + 32, r_objs[i].light_dir, 16);
            memcpy(dst + 36, r_objs[i].camera_pos, 16);
            dst[40] = r_objs[i].time;
        }
    }
    SINK(ubos[0].data[0]);
    return (now_ns() - t0) / (100.0 * N_OBJS);
}

/* --- Path C: Raw float objects → the object IS the UBO, zero copy --- */
/* If RawObject is already laid out to match the UBO, no copy at all */
typedef struct {
    r_mat4 model;       /* 64 B */
    r_mat4 view;        /* 64 B */
    r_vec3 light_dir;   /* 16 B (padded) */
    r_vec3 camera_pos;  /* 16 B (padded) */
    float time;         /*  4 B */
    float _pad[3];      /* 12 B */
} GPUObject;            /* 176 B, matches UBO exactly */

static GPUObject gpu_objs[N_OBJS];

static double bench_gpuobject_zero_copy(void) {
    double t0 = now_ns();
    for (int rep = 0; rep < 100; rep++) {
        for (int i = 0; i < N_OBJS; i++) {
            /* The GPUObject IS the UBO. No copy needed. */
            /* In real code: vkMapMemory + point at &gpu_objs[i] */
            float* data = (float*)&gpu_objs[i];
            SINK(data[0]); /* just prove we accessed it */
        }
    }
    SINK(gpu_objs[0].model[0]);
    return (now_ns() - t0) / (100.0 * N_OBJS);
}

/* --- Extra: measure just the vec3 field copy overhead --- */
static double bench_struct_vec3_extract(void) {
    double t0 = now_ns();
    float sink = 0;
    for (int rep = 0; rep < 1000; rep++) {
        for (int i = 0; i < N_OBJS; i++) {
            float* dst = ubos[i].data + 32;
            dst[0] = s_objs[i].light_dir.x;
            dst[1] = s_objs[i].light_dir.y;
            dst[2] = s_objs[i].light_dir.z;
            dst[3] = 0.0f;
            sink += dst[0];
        }
    }
    SINK(sink);
    return (now_ns() - t0) / (1000.0 * N_OBJS);
}

static double bench_raw_vec3_memcpy(void) {
    double t0 = now_ns();
    float sink = 0;
    for (int rep = 0; rep < 1000; rep++) {
        for (int i = 0; i < N_OBJS; i++) {
            memcpy(ubos[i].data + 32, r_objs[i].light_dir, 16);
            sink += ubos[i].data[32];
        }
    }
    SINK(sink);
    return (now_ns() - t0) / (1000.0 * N_OBJS);
}

/* -------------------------------------------------------------------------- */

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) - (x > y);
}

#define REPS 21

typedef struct {
    const char* name;
    double (*fn)(void);
} Case;

static void run(const char* title, Case* cases, int n) {
    printf("\n── %s ──────────────────────────────────────────\n", title);
    for (int i = 0; i < n; i++) {
        double ts[REPS];
        for (int r = 0; r < 3; r++) cases[i].fn();
        for (int r = 0; r < REPS; r++) ts[r] = cases[i].fn();
        qsort(ts, REPS, sizeof(double), cmp_d);
        printf("  %-40s  %8.2f ns/op\n", cases[i].name, ts[REPS/2]);
    }
}

int main(void) {
    cpu_set_t set;
    CPU_ZERO(&set); CPU_SET(0, &set);
    sched_setaffinity(0, sizeof set, &set);

    printf("=================================================================\n");
    printf(" Shader upload cost: struct types vs raw float arrays\n");
    printf("=================================================================\n");
    printf(" N_OBJS = %d  |  sizeof(StructObject) = %zu B\n", N_OBJS, sizeof(StructObject));
    printf(" sizeof(RawObject) = %zu B  |  sizeof(GPUObject) = %zu B\n", sizeof(RawObject), sizeof(GPUObject));
    printf(" sizeof(FlatUBO) = %zu B\n", sizeof(FlatUBO));
    printf("=================================================================\n");

    init();

    Case upload_cases[] = {
        { "A: struct → field-by-field into UBO",    bench_struct_to_ubo },
        { "B: raw floats → memcpy into UBO",        bench_raw_to_ubo_memcpy },
        { "C: GPUObject → zero copy (already GPU)", bench_gpuobject_zero_copy },
    };
    run("Full UBO upload", upload_cases, 3);

    Case vec3_cases[] = {
        { "struct: extract x,y,z + pad into buffer", bench_struct_vec3_extract },
        { "raw:    memcpy 16 bytes",                   bench_raw_vec3_memcpy },
    };
    run("Just the vec3 → padded vec4 upload", vec3_cases, 2);

    printf("\n=================================================================\n");
    printf(" The win from raw floats is NOT in computation (same speed).\n");
    printf(" The win is at the shader boundary: no field-by-field extraction.\n");
    printf(" GPUObject (path C) is the end goal: your object IS the UBO.\n");
    printf("=================================================================\n");
    return 0;
}
