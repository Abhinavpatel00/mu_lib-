#include "mu_profile.h"
#include "mu_perf.h" // Replace with the header containing mu_time_now().

#include <stdio.h>
#include <string.h>

static MuProfileEntry g_entries[MU_PROFILE_CAPACITY];
static uint32_t g_entry_count;

// A scope is identified by both its function and its custom label.
// This lets one function contain multiple independently measured blocks.
static MuProfileEntry *mu_profile_find(const char *function, const char *name)
{
    for (uint32_t i = 0; i < g_entry_count; ++i) {
        MuProfileEntry *e = &g_entries[i];

        if (strcmp(e->function, function) == 0 &&
            strcmp(e->name, name) == 0)
            return e;
    }

    if (g_entry_count >= MU_PROFILE_CAPACITY)
        return NULL;

    MuProfileEntry *e = &g_entries[g_entry_count++];
    memset(e, 0, sizeof(*e));

    snprintf(e->function, sizeof(e->function), "%s", function);
    snprintf(e->name, sizeof(e->name), "%s", name);

    e->min_ns = UINT64_MAX;
    return e;
}

MuProfileScope mu_profile_begin(const char *function, const char *name)
{
    MuProfileScope scope = {
        .function = function,
        .name     = name,
        .start_ns = mu_time_now(),
        .active   = true
    };

    return scope;
}

void mu_profile_end(MuProfileScope *scope)
{
    if (!scope || !scope->active)
        return;

    const uint64_t elapsed = mu_time_now() - scope->start_ns;
    MuProfileEntry *e = mu_profile_find(scope->function, scope->name);

    if (e) {
        e->total_ns += elapsed;
        e->last_ns = elapsed;

        if (elapsed < e->min_ns) e->min_ns = elapsed;
        if (elapsed > e->max_ns) e->max_ns = elapsed;

        ++e->calls;
    }

    scope->active = false;
}

uint32_t mu_profile_count(void)
{
    return g_entry_count;
}

const MuProfileEntry *mu_profile_get(uint32_t index)
{
    return index < g_entry_count ? &g_entries[index] : NULL;
}

void mu_profile_reset(void)
{
    memset(g_entries, 0, sizeof(g_entries));
    g_entry_count = 0;
}
