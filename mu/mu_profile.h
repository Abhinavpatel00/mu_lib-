#pragma once

#include "mu_common.h"
#include <stdbool.h>
#include <stdint.h>

#define MU_PROFILE_CAPACITY 256
#define MU_PROFILE_NAME_LEN 64
#define MU_PROFILE_FUNCTION_LEN 96

typedef struct MuProfileEntry {
    char function[MU_PROFILE_FUNCTION_LEN];
    char name[MU_PROFILE_NAME_LEN];

    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t last_ns;
    uint32_t calls;
} MuProfileEntry;

typedef struct MuProfileScope {
    const char *function;
    const char *name;
    uint64_t start_ns;
    bool active;
} MuProfileScope;

// Implemented once in mu_profile.c.
MuProfileScope mu_profile_begin(const char *function, const char *name);
void mu_profile_end(MuProfileScope *scope);

uint32_t mu_profile_count(void);
const MuProfileEntry *mu_profile_get(uint32_t index);
void mu_profile_reset(void);

// Usage: MU_SCOPE_TIMER("Camera update") { ... }
// The for-loop's final expression records the elapsed time.
#define MU_TIMER(name) \
    for (MuProfileScope _mu_scope = mu_profile_begin(__func__, name); \
         _mu_scope.active; \
         mu_profile_end(&_mu_scope))
