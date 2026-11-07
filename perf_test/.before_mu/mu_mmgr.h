/*
 * mu_mmgr.h
 *
 * Standalone C99 single-header conversion of Fluid Studios MemoryManager
 * (from mmgr.h + mmgr.c as integrated in The Forge).
 *
 * Usage:
 *   #define MU_MMGR_IMPLEMENTATION
 *   #include "mu_mmgr.h"
 *
 * Typical flow:
 *   mu_mmgr_init("my_app");
 *   void* p = MU_MMGR_MALLOC(256);
 *   p = MU_MMGR_REALLOC(p, 512);
 *   MU_MMGR_FREE(p);
 *   mu_mmgr_shutdown(); // emits leak report and asserts on leaks by default
 */

#pragma once

#include "mu_macros.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Linux/glibc gets execinfo by default. */
#ifndef MU_MMGR_BACKTRACE
#if defined(__linux__) && defined(__GLIBC__)
#define MU_MMGR_BACKTRACE 1
#else
#define MU_MMGR_BACKTRACE 0
#endif
#endif

#ifndef MU_MMGR_BACKTRACE_SIZE
#define MU_MMGR_BACKTRACE_SIZE 128
#endif

#ifndef MU_MMGR_ASSERT_ON_LEAK
#define MU_MMGR_ASSERT_ON_LEAK 1
#endif

#ifndef MU_MMGR_HASH_BITS
#define MU_MMGR_HASH_BITS 12u
#endif

/* Optional: mirror legacy controls from original source. */
/* #define MU_MMGR_STRESS_TEST */
/* #define MU_MMGR_TEST */
/* #define MU_MMGR_RANDOM_FAILURE 10.0 */
/* #define MU_MMGR_FORCE_EXACT_ALIGNMENT */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mu_mmgr_alloc_unit
{
    size_t                     actual_size;
    size_t                     reported_size;
    size_t                     alignment;
    size_t                     offset;
    void*                      actual_address;
    void*                      reported_address;
    char                       source_file[140];
    char                       source_func[140];
#if MU_MMGR_BACKTRACE
    void* backtrace_buffer[MU_MMGR_BACKTRACE_SIZE];
    int   backtrace_nptrs;
    int   backtrace_skip;
#endif
    unsigned int               source_line;
    unsigned int               allocation_type;
    bool                       break_on_dealloc;
    bool                       break_on_realloc;
    unsigned int               allocation_number;
    struct mu_mmgr_alloc_unit* next;
    struct mu_mmgr_alloc_unit* prev;
} mu_mmgr_alloc_unit;

typedef struct mu_mmgr_memory_stats
{
    unsigned int total_reported_memory;
    unsigned int total_actual_memory;
    unsigned int peak_reported_memory;
    unsigned int peak_actual_memory;
    unsigned int accumulated_reported_memory;
    unsigned int accumulated_actual_memory;
    unsigned int accumulated_alloc_unit_count;
    unsigned int total_alloc_unit_count;
    unsigned int peak_alloc_unit_count;
} mu_mmgr_memory_stats;

enum
{
    MU_MMGR_ALLOC_UNKNOWN,
    MU_MMGR_ALLOC_NEW,
    MU_MMGR_ALLOC_NEW_ARRAY,
    MU_MMGR_ALLOC_MALLOC,
    MU_MMGR_ALLOC_CALLOC,
    MU_MMGR_ALLOC_REALLOC,
    MU_MMGR_ALLOC_DELETE,
    MU_MMGR_ALLOC_DELETE_ARRAY,
    MU_MMGR_ALLOC_FREE,
};

/*
    Tracked allocation hooks (called by the MU_MMGR_* macros; call sites never
    pass file/line/func by hand).
*/
void* mu_mmgr_alloc(const char*        source_file,
                    const unsigned int source_line,
                    const char*        source_func,
                    const unsigned int allocation_type,
                    size_t             alignment,
                    size_t             reported_size);
void* mu_mmgr_realloc(const char*        source_file,
                      const unsigned int source_line,
                      const char*        source_func,
                      const unsigned int reallocation_type,
                      size_t             reported_size,
                      void*              reported_address);
void  mu_mmgr_free(const char*        source_file,
                   const unsigned int source_line,
                   const char*        source_func,
                   const unsigned int deallocation_type,
                   const void*        reported_address);

/* Attribution for allocations made outside the MU_MMGR_* macros. */
void  mu_mmgr_set_owner(const char* file, const unsigned int line, const char* func);
bool* mu_mmgr_break_on_realloc(void* reported_address);
bool* mu_mmgr_break_on_dealloc(void* reported_address);

/* Validation / accounting. */
bool         mu_mmgr_validate_address(const void* reported_address);
bool         mu_mmgr_validate_alloc_unit(const mu_mmgr_alloc_unit* alloc_unit);
bool         mu_mmgr_validate_all_units(void);
unsigned int mu_mmgr_calc_unused(const mu_mmgr_alloc_unit* alloc_unit);
unsigned int mu_mmgr_calc_all_unused(void);

/* Lifetime and reporting. */
bool                 mu_mmgr_init(const char* app_name);
void                 mu_mmgr_shutdown(void);
mu_mmgr_memory_stats mu_mmgr_stats_get(void);

void mu_mmgr_set_executable_name(const char* name, size_t length);
void mu_mmgr_set_log_file_directory(const char* directory);
void mu_mmgr_dump_alloc_unit(const mu_mmgr_alloc_unit* alloc_unit, const char* prefix);
void mu_mmgr_dump_memory_report(const char* filename, const bool overwrite);

/* Runtime option toggles; the returned pointer is writable. */
bool* mu_mmgr_always_validate_all(void);
bool* mu_mmgr_always_log_all(void);
bool* mu_mmgr_always_wipe_all(void);
bool* mu_mmgr_random_wipe(void);
void  mu_mmgr_break_on_allocation(unsigned int count);
void  mu_mmgr_set_stack_skip_count(int stack_depth);

/* Convenience macros for C-only call sites. */
#define MU_MMGR_MALLOC(sz) mu_mmgr_alloc(__FILE__, __LINE__, __func__, MU_MMGR_ALLOC_MALLOC, sizeof(void*), (sz))
#define MU_MMGR_MEMALIGN(al, sz) mu_mmgr_alloc(__FILE__, __LINE__, __func__, MU_MMGR_ALLOC_MALLOC, (al), (sz))
#define MU_MMGR_CALLOC(cnt, sz)                                                                                           \
    mu_mmgr_alloc(__FILE__, __LINE__, __func__, MU_MMGR_ALLOC_CALLOC, sizeof(void*), (cnt) * (sz))
#define MU_MMGR_REALLOC(ptr, sz) mu_mmgr_realloc(__FILE__, __LINE__, __func__, MU_MMGR_ALLOC_REALLOC, (sz), (ptr))
#define MU_MMGR_FREE(ptr) mu_mmgr_free(__FILE__, __LINE__, __func__, MU_MMGR_ALLOC_FREE, (ptr))

#ifdef __cplusplus
}
#endif

#ifdef MU_MMGR_IMPLEMENTATION

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if MU_MMGR_BACKTRACE
#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#else
#include <execinfo.h>
#endif
#endif

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION mu_mmgr_mutex;
static void              mu_mmgr_mutex_init(mu_mmgr_mutex* m)
{
    InitializeCriticalSection(m);
}
static void mu_mmgr_mutex_lock(mu_mmgr_mutex* m)
{
    EnterCriticalSection(m);
}
static void mu_mmgr_mutex_unlock(mu_mmgr_mutex* m)
{
    LeaveCriticalSection(m);
}
static int mu_mmgr_localtime_safe(const time_t* t, struct tm* out)
{
    return localtime_s(out, t);
}
#else
#include <pthread.h>
typedef pthread_mutex_t mu_mmgr_mutex;
static void             mu_mmgr_mutex_init(mu_mmgr_mutex* m)
{
    (void)pthread_mutex_init(m, NULL);
}
static void mu_mmgr_mutex_lock(mu_mmgr_mutex* m)
{
    (void)pthread_mutex_lock(m);
}
static void mu_mmgr_mutex_unlock(mu_mmgr_mutex* m)
{
    (void)pthread_mutex_unlock(m);
}
static int mu_mmgr_localtime_safe(const time_t* t, struct tm* out)
{
    return localtime_r(t, out) == NULL ? -1 : 0;
}
#endif

#ifndef MU_MMGR_LOG_TO_STDERR
#define MU_MMGR_LOG_TO_STDERR 0
#endif

#ifndef MU_MMGR_PATH_MAX
#define MU_MMGR_PATH_MAX 1024
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MU_MMGR_UNUSED(x) (void)(x)
#else
#define MU_MMGR_UNUSED(x) ((void)(x))
#endif

#ifdef MU_MMGR_STRESS_TEST
static bool               mu_mmgr_opt_random_wipe           = true;
static bool               mu_mmgr_opt_always_validate_all    = true;
static bool               mu_mmgr_opt_always_log_all         = true;
static bool               mu_mmgr_opt_always_wipe_all        = true;
static bool               mu_mmgr_opt_cleanup_log_on_first_run = true;
static const unsigned int mu_mmgr_padding_size          = 1024;
#else
static bool               mu_mmgr_opt_random_wipe           = false;
static bool               mu_mmgr_opt_always_validate_all    = false;
static bool               mu_mmgr_opt_always_log_all         = false;
static bool               mu_mmgr_opt_always_wipe_all        = true;
static bool               mu_mmgr_opt_cleanup_log_on_first_run = true;
static const unsigned int mu_mmgr_padding_size          = 4;
#endif

static unsigned int mu_mmgr_prefix_pattern   = 0xbaadf00d;
static unsigned int mu_mmgr_postfix_pattern  = 0xdeadc0de;
static unsigned int mu_mmgr_unused_pattern   = 0xfeedface;
static unsigned int mu_mmgr_released_pattern = 0xdeadbeef;

enum
{
    MU_MMGR_HASH_SIZE = (1u << MU_MMGR_HASH_BITS)
};

static const char* mu_mmgr_allocation_types[] = {"Unknown", "new",    "new[]",    "malloc", "calloc",
                                                "realloc", "delete", "delete[]", "free"};

static mu_mmgr_alloc_unit*  mu_mmgr_hash_table[MU_MMGR_HASH_SIZE];
static mu_mmgr_alloc_unit*  mu_mmgr_reservoir              = NULL;
static unsigned int mu_mmgr_current_allocation_count = 0;
static unsigned int mu_mmgr_break_on_allocation_count = 0;
static mu_mmgr_memory_stats      mu_mmgr_stats;
static const char*  mu_mmgr_source_file                     = "??";
static const char*  mu_mmgr_source_func                     = "??";
static unsigned int mu_mmgr_source_line                     = 0;
static mu_mmgr_alloc_unit** mu_mmgr_reservoir_buffer                = NULL;
static unsigned int mu_mmgr_reservoir_buffer_size            = 0;
static char*        mu_mmgr_log_memory                      = NULL;
static size_t       mu_mmgr_log_memory_length                = 0;
static const char*  mu_mmgr_app_name                        = NULL;
static char         mu_mmgr_executable_name[256]            = {0};
static char         mu_mmgr_log_directory[MU_MMGR_PATH_MAX] = {0};

static mu_mmgr_mutex mu_mmgr_alloc_mutex;
static mu_mmgr_mutex mu_mmgr_log_mutex;
static bool            mu_mmgr_alloc_mutex_init = false;
static bool            mu_mmgr_log_mutex_init   = false;

#if MU_MMGR_BACKTRACE
static int mu_mmgr_stack_skip_count = 0;
#if defined(_WIN32)
static HANDLE mu_mmgr_process_handle = NULL;
#endif
#endif

#define MU_MMGR_BUFFER_SIZE 2048
#define MU_MMGR_ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static void mu_mmgr_lock_alloc(void)
{
    if(!mu_mmgr_alloc_mutex_init)
    {
        mu_mmgr_mutex_init(&mu_mmgr_alloc_mutex);
        mu_mmgr_alloc_mutex_init = true;
    }
    mu_mmgr_mutex_lock(&mu_mmgr_alloc_mutex);
}

static void mu_mmgr_unlock_alloc(void)
{
    if(mu_mmgr_alloc_mutex_init)
    {
        mu_mmgr_mutex_unlock(&mu_mmgr_alloc_mutex);
    }
}

static void mu_mmgr_lock_log(void)
{
    if(!mu_mmgr_log_mutex_init)
    {
        mu_mmgr_mutex_init(&mu_mmgr_log_mutex);
        mu_mmgr_log_mutex_init = true;
    }
    mu_mmgr_mutex_lock(&mu_mmgr_log_mutex);
}

static void mu_mmgr_unlock_log(void)
{
    if(mu_mmgr_log_mutex_init)
    {
        mu_mmgr_mutex_unlock(&mu_mmgr_log_mutex);
    }
}

static char* mu_mmgr_strncpy(char* dst, size_t dst_size, const char* src, size_t count)
{
    size_t size = (dst_size < count) ? (dst_size - 1) : count;
    char*  ret  = strncpy(dst, src, size);
    ret[size]   = '\0';
    return ret;
}

static char* mu_mmgr_strcpy(char* dst, size_t dst_size, const char* src)
{
    size_t src_size = strlen(src);
    return mu_mmgr_strncpy(dst, dst_size, src, src_size);
}

static const char* mu_mmgr_source_file_basename(const char* source_file)
{
    const char* ptr = strrchr(source_file, '\\');
    if(ptr)
    {
        return ptr + 1;
    }
    ptr = strrchr(source_file, '/');
    if(ptr)
    {
        return ptr + 1;
    }
    return source_file;
}

static const char* mu_mmgr_owner_string(const char* source_file, const unsigned int source_line, const char* source_func)
{
    static char str[180];
    memset(str, 0, sizeof(str));
    snprintf(str, sizeof(str), "%s(%05u)::%s", mu_mmgr_source_file_basename(source_file), source_line, source_func);
    return str;
}

static const char* mu_mmgr_insert_commas(unsigned int value)
{
    static char str[30];
    memset(str, 0, sizeof(str));

    snprintf(str, sizeof(str), "%u", value);
    if(strlen(str) > 3)
    {
        memmove(&str[strlen(str) - 3], &str[strlen(str) - 4], 4);
        str[strlen(str) - 4] = ',';
    }
    if(strlen(str) > 7)
    {
        memmove(&str[strlen(str) - 7], &str[strlen(str) - 8], 8);
        str[strlen(str) - 8] = ',';
    }
    if(strlen(str) > 11)
    {
        memmove(&str[strlen(str) - 11], &str[strlen(str) - 12], 12);
        str[strlen(str) - 12] = ',';
    }

    return str;
}

static const char* mu_mmgr_memory_size_string(uint32_t size)
{
    static char str[90];
    if(size > (1024u * 1024u))
    {
        snprintf(str, sizeof(str), "%10s (%7.2fM)", mu_mmgr_insert_commas(size), ((float)size) / (1024.0f * 1024.0f));
    }
    else if(size > 1024u)
    {
        snprintf(str, sizeof(str), "%10s (%7.2fK)", mu_mmgr_insert_commas(size), ((float)size) / 1024.0f);
    }
    else
    {
        snprintf(str, sizeof(str), "%10s bytes     ", mu_mmgr_insert_commas(size));
    }
    return str;
}

static size_t mu_mmgr_calculate_actual_size(const size_t reported_size)
{
    return reported_size + mu_mmgr_padding_size * sizeof(uint32_t) * 2;
}

static void* mu_mmgr_calculate_reported_address(const void* actual_address)
{
    if(!actual_address)
    {
        return NULL;
    }
    return (void*)(((const char*)actual_address) + sizeof(uint32_t) * mu_mmgr_padding_size);
}

static void mu_mmgr_reset_globals(void)
{
    mu_mmgr_source_file = "??";
    mu_mmgr_source_line = 0;
    mu_mmgr_source_func = "??";
#if MU_MMGR_BACKTRACE
    mu_mmgr_stack_skip_count = 0;
#endif
}

static mu_mmgr_alloc_unit* mu_mmgr_find_alloc_unit(const void* reported_address)
{
    MU_ASSERT(reported_address != NULL);

    size_t      hash_index = (((size_t)reported_address) >> 4u) & (MU_MMGR_HASH_SIZE - 1u);
    mu_mmgr_alloc_unit* ptr       = mu_mmgr_hash_table[hash_index];
    while(ptr)
    {
        if(ptr->reported_address == reported_address)
        {
            return ptr;
        }
        ptr = ptr->next;
    }

    return NULL;
}

static char* mu_mmgr_log_to_memory(const char* log)
{
    const size_t log_length = strlen(log) + 1;

    if(mu_mmgr_log_memory == NULL)
    {
        mu_mmgr_log_memory       = (char*)calloc(1, sizeof(char));
        mu_mmgr_log_memory_length = 1;
    }

    if(!mu_mmgr_log_memory)
    {
        return NULL;
    }

    char* new_mem = (char*)realloc(mu_mmgr_log_memory, mu_mmgr_log_memory_length + log_length - 1);
    if(!new_mem)
    {
        return mu_mmgr_log_memory;
    }

    mu_mmgr_log_memory = new_mem;
    memcpy(mu_mmgr_log_memory + mu_mmgr_log_memory_length - 1, log, log_length);
    mu_mmgr_log_memory_length += log_length - 1;
    return mu_mmgr_log_memory;
}

static void mu_mmgr_do_cleanup_log_on_first_run(void)
{
    if(!mu_mmgr_opt_cleanup_log_on_first_run)
    {
        return;
    }

    mu_mmgr_opt_cleanup_log_on_first_run = false;

    (void)mu_mmgr_log_to_memory("--------------------------------------------------------------------------------\n");
    (void)mu_mmgr_log_to_memory("\n");
    (void)mu_mmgr_log_to_memory("This file contains a log of all memory operations performed during the last run.\n");
    (void)mu_mmgr_log_to_memory("\n");
    (void)mu_mmgr_log_to_memory("Search for [!] to find allocator errors.\n");
    (void)mu_mmgr_log_to_memory("--------------------------------------------------------------------------------\n");
}

static char* mu_mmgr_log(const char* format, ...)
{
    if(mu_mmgr_opt_cleanup_log_on_first_run)
    {
        mu_mmgr_do_cleanup_log_on_first_run();
    }

    mu_mmgr_lock_log();

    static char buffer[MU_MMGR_BUFFER_SIZE];
    va_list     ap;
    va_start(ap, format);
    int chars_written = vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    if(chars_written < 0)
    {
        chars_written = 0;
    }

    int newline_pos         = chars_written;
    int last_newline_pos     = (int)sizeof(buffer) - 2;
    newline_pos             = newline_pos > last_newline_pos ? last_newline_pos : newline_pos;
    buffer[newline_pos]     = '\n';
    buffer[newline_pos + 1] = '\0';

#if MU_MMGR_LOG_TO_STDERR
    fputs(buffer, stderr);
#endif

    char* log_address = mu_mmgr_log_to_memory(buffer);

    mu_mmgr_unlock_log();
    return log_address;
}

static void mu_mmgr_wipe_with_pattern(mu_mmgr_alloc_unit* alloc_unit, uint32_t pattern, const unsigned int original_reported_size)
{
    if(mu_mmgr_opt_random_wipe)
    {
        pattern = ((uint32_t)(rand() & 0xff) << 24) | ((uint32_t)(rand() & 0xff) << 16)
                  | ((uint32_t)(rand() & 0xff) << 8) | (uint32_t)(rand() & 0xff);
    }

    if(mu_mmgr_opt_always_wipe_all && alloc_unit->reported_size > original_reported_size)
    {
        uint32_t* lptr   = (uint32_t*)(((char*)alloc_unit->reported_address) + original_reported_size);
        int       length = (int)(alloc_unit->reported_size - original_reported_size);
        int       i;
        for(i = 0; i < (length >> 2); i++, lptr++)
        {
            *lptr = pattern;
        }

        unsigned int shift_count = 0;
        char*        cptr       = (char*)(lptr);
        for(i = 0; i < (length & 0x3); i++, cptr++, shift_count += 8)
        {
            *cptr = (char)((pattern & (0xffu << shift_count)) >> shift_count);
        }
    }

    uint8_t* pre  = (uint8_t*)alloc_unit->reported_address - mu_mmgr_padding_size * sizeof(uint32_t);
    uint8_t* post = (uint8_t*)alloc_unit->reported_address + alloc_unit->reported_size;

    size_t padding_bytes = mu_mmgr_padding_size * sizeof(uint32_t);
    size_t i;
    for(i = 0; i < padding_bytes; i++, pre++, post++)
    {
        *pre  = (uint8_t)((mu_mmgr_prefix_pattern >> ((i % sizeof(uint32_t)) * 8)) & 0xFFu);
        *post = (uint8_t)((mu_mmgr_postfix_pattern >> ((i % sizeof(uint32_t)) * 8)) & 0xFFu);
    }
}

static void mu_mmgr_dump_line(FILE* file_to_write, const char* format, ...)
{
    va_list args;
    char    buffer[MU_MMGR_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

#if MU_MMGR_LOG_TO_STDERR
    fprintf(stderr, "%s\n", buffer);
#endif

    if(file_to_write)
    {
        fprintf(file_to_write, "%s\n", buffer);
        fflush(file_to_write);
    }
}

#if MU_MMGR_BACKTRACE
static void mu_mmgr_dump_backtrace(FILE* fh, const mu_mmgr_alloc_unit* ptr)
{
    if(!ptr->backtrace_nptrs)
    {
        return;
    }

#if defined(_WIN32)
    MU_MMGR_UNUSED(fh);
    MU_MMGR_UNUSED(ptr);
#else
    char** strings = backtrace_symbols(ptr->backtrace_buffer, ptr->backtrace_nptrs);
    if(strings)
    {
        int j;
        for(j = ptr->backtrace_skip; j < ptr->backtrace_nptrs; ++j)
        {
            if(fh)
            {
                mu_mmgr_dump_line(fh, "\t%s", strings[j]);
            }
            else
            {
                mu_mmgr_log("\t%s", strings[j]);
            }
        }
        free(strings);
    }
#endif
}
#endif

static void mu_mmgr_dump_allocations(FILE* fh)
{
    mu_mmgr_dump_line(fh, "Alloc.        Addr           Size           Addr           Size                        BreakOn BreakOn");
    mu_mmgr_dump_line(fh, "Number      Reported       Reported        Actual         Actual     Unused    Method  Dealloc Realloc  Allocated by");
    mu_mmgr_dump_line(fh, "------ ------------------ ---------- ------------------ ---------- ---------- -------- ------- ------- ---------------------------------------------------");

    for(unsigned int i = 0; i < MU_MMGR_HASH_SIZE; ++i)
    {
        mu_mmgr_alloc_unit* ptr = mu_mmgr_hash_table[i];
        while(ptr)
        {
            mu_mmgr_dump_line(fh, "% 6u 0x%016zX 0x%08zX 0x%016zX 0x%08zX 0x%08X %-8s    %c       %c    %s",
                             ptr->allocation_number, (size_t)ptr->reported_address, ptr->reported_size, (size_t)ptr->actual_address,
                             ptr->actual_size, mu_mmgr_calc_unused(ptr), mu_mmgr_allocation_types[ptr->allocation_type],
                             ptr->break_on_dealloc ? 'Y' : 'N', ptr->break_on_realloc ? 'Y' : 'N',
                             mu_mmgr_owner_string(ptr->source_file, ptr->source_line, ptr->source_func));
#if MU_MMGR_BACKTRACE
            mu_mmgr_dump_backtrace(fh, ptr);
#endif
            ptr = ptr->next;
        }
    }
}

static void mu_mmgr_build_path(char* dst, size_t dst_size, const char* filename)
{
    if(mu_mmgr_log_directory[0] != '\0')
    {
        size_t n = strlen(mu_mmgr_log_directory);
        if(n > 0 && (mu_mmgr_log_directory[n - 1] == '/' || mu_mmgr_log_directory[n - 1] == '\\'))
        {
            snprintf(dst, dst_size, "%s%s", mu_mmgr_log_directory, filename);
        }
        else
        {
            snprintf(dst, dst_size, "%s/%s", mu_mmgr_log_directory, filename);
        }
    }
    else
    {
        snprintf(dst, dst_size, "%s", filename);
    }
}

static void mu_mmgr_dump_leak_report(void)
{
    FILE* fh = NULL;
    char  output_file_name[MU_MMGR_PATH_MAX];
    memset(output_file_name, 0, sizeof(output_file_name));

    if(mu_mmgr_app_name && mu_mmgr_app_name[0] != '\0')
    {
        char local_name[300];
        snprintf(local_name, sizeof(local_name), "%s.memleaks", mu_mmgr_app_name);
        mu_mmgr_build_path(output_file_name, sizeof(output_file_name), local_name);
        fh = fopen(output_file_name, "wb");
        if(!fh)
        {
            snprintf(output_file_name, sizeof(output_file_name), "%s", "Failed to write report to file");
        }
    }

    time_t    t = time(NULL);
    struct tm tme;
    memset(&tme, 0, sizeof(tme));
    (void)mu_mmgr_localtime_safe(&t, &tme);

    mu_mmgr_dump_line(fh, " -------------------------------------------------------------------------------");
    mu_mmgr_dump_line(fh, "                Memory leak report %02d/%02d/%04d %02d:%02d:%02d:", tme.tm_mon + 1,
                     tme.tm_mday, tme.tm_year + 1900, tme.tm_hour, tme.tm_min, tme.tm_sec);
    mu_mmgr_dump_line(fh, "                %s", output_file_name[0] ? output_file_name : "(no file)");
    mu_mmgr_dump_line(fh, " -------------------------------------------------------------------------------");

    if(mu_mmgr_stats.total_alloc_unit_count)
    {
        mu_mmgr_dump_line(fh, "%u memory leak%s found:\n", mu_mmgr_stats.total_alloc_unit_count,
                         mu_mmgr_stats.total_alloc_unit_count == 1 ? "" : "s");
        mu_mmgr_dump_allocations(fh);
    }
    else
    {
        mu_mmgr_dump_line(fh, "Congratulations! No memory leaks found!");

        if(mu_mmgr_reservoir_buffer)
        {
            for(unsigned int i = 0; i < mu_mmgr_reservoir_buffer_size; ++i)
            {
                free(mu_mmgr_reservoir_buffer[i]);
            }
            free(mu_mmgr_reservoir_buffer);
            mu_mmgr_reservoir_buffer     = NULL;
            mu_mmgr_reservoir_buffer_size = 0;
            mu_mmgr_reservoir           = NULL;
        }
    }

    {
        char* all_memory_log = mu_mmgr_log("----All Allocations and Deallocations----");
        if(all_memory_log)
        {
            mu_mmgr_dump_line(fh, "%s", all_memory_log);
        }
    }

    if(fh)
    {
        fclose(fh);
    }

#if MU_MMGR_ASSERT_ON_LEAK
    MU_ASSERT(mu_mmgr_stats.total_alloc_unit_count == 0 && "Memory leaks found");
#endif
}

bool mu_mmgr_init(const char* app_name)
{
    mu_mmgr_app_name = app_name;
    mu_mmgr_do_cleanup_log_on_first_run();

#if MU_MMGR_BACKTRACE && defined(_WIN32)
    HANDLE current_process = GetCurrentProcess();
    DuplicateHandle(current_process, current_process, current_process, &mu_mmgr_process_handle, 0, TRUE, DUPLICATE_SAME_ACCESS);
#endif

    return true;
}

void mu_mmgr_shutdown(void)
{
    mu_mmgr_dump_leak_report();

#if MU_MMGR_BACKTRACE && defined(_WIN32)
    if(mu_mmgr_process_handle)
    {
        CloseHandle(mu_mmgr_process_handle);
        mu_mmgr_process_handle = NULL;
    }
#endif
}

bool* mu_mmgr_always_validate_all(void)
{
    return &mu_mmgr_opt_always_validate_all;
}
bool* mu_mmgr_always_log_all(void)
{
    return &mu_mmgr_opt_always_log_all;
}
bool* mu_mmgr_always_wipe_all(void)
{
    return &mu_mmgr_opt_always_wipe_all;
}
bool* mu_mmgr_random_wipe(void)
{
    return &mu_mmgr_opt_random_wipe;
}
void mu_mmgr_break_on_allocation(unsigned int count)
{
    mu_mmgr_break_on_allocation_count = count;
}

void mu_mmgr_set_owner(const char* file, const unsigned int line, const char* func)
{
    if(mu_mmgr_source_line && mu_mmgr_opt_always_log_all)
    {
        mu_mmgr_log("[I] NOTE! Possible destructor chain: previous owner is %s",
                    mu_mmgr_owner_string(mu_mmgr_source_file, mu_mmgr_source_line, mu_mmgr_source_func));
    }

    mu_mmgr_source_file = file ? file : "??";
    mu_mmgr_source_line = line;
    mu_mmgr_source_func = func ? func : "??";
}

void mu_mmgr_set_stack_skip_count(int stack_depth)
{
#if MU_MMGR_BACKTRACE
    if(!mu_mmgr_stack_skip_count)
    {
        mu_mmgr_stack_skip_count = stack_depth;
    }
#else
    MU_MMGR_UNUSED(stack_depth);
#endif
}

void* mu_mmgr_alloc(const char*        source_file,
                    const unsigned int source_line,
                    const char*        source_func,
                    const unsigned int allocation_type,
                    size_t             alignment,
                    size_t             reported_size)
{
    if(mu_mmgr_opt_cleanup_log_on_first_run)
    {
        MU_ASSERT(false && "Memory tracker not initialized");
        return NULL;
    }

    alignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;

    mu_mmgr_lock_alloc();

#ifdef MU_MMGR_TEST
    mu_mmgr_log("[D] ENTER: mu_mmgr_alloc()");
#endif

    mu_mmgr_current_allocation_count++;

    if(mu_mmgr_opt_always_log_all)
    {
        mu_mmgr_log("[+] %05u %8s of size 0x%08X(%08u) by %s", mu_mmgr_current_allocation_count,
                    mu_mmgr_allocation_types[allocation_type], (unsigned)reported_size, (unsigned)reported_size,
                    mu_mmgr_owner_string(source_file, source_line, source_func));
    }

    MU_ASSERT(mu_mmgr_current_allocation_count != mu_mmgr_break_on_allocation_count);

    if(!mu_mmgr_reservoir)
    {
        mu_mmgr_reservoir = (mu_mmgr_alloc_unit*)malloc(sizeof(mu_mmgr_alloc_unit) * 256u);
        MU_ASSERT(mu_mmgr_reservoir != NULL);

        if(!mu_mmgr_reservoir)
        {
            mu_mmgr_unlock_alloc();
            MU_ASSERT(false && "Unable to allocate RAM for internal memory tracking data");
            return NULL;
        }

        memset(mu_mmgr_reservoir, 0, sizeof(mu_mmgr_alloc_unit) * 256u);
        for(unsigned int i = 0; i < 255u; ++i)
        {
            mu_mmgr_reservoir[i].next = &mu_mmgr_reservoir[i + 1u];
        }

        mu_mmgr_alloc_unit** temp =
            (mu_mmgr_alloc_unit**)realloc(mu_mmgr_reservoir_buffer, (mu_mmgr_reservoir_buffer_size + 1u) * sizeof(mu_mmgr_alloc_unit*));
        MU_ASSERT(temp != NULL);
        if(temp)
        {
            mu_mmgr_reservoir_buffer                                = temp;
            mu_mmgr_reservoir_buffer[mu_mmgr_reservoir_buffer_size++] = mu_mmgr_reservoir;
        }
    }

    MU_ASSERT(mu_mmgr_reservoir != NULL);

    mu_mmgr_alloc_unit* au    = mu_mmgr_reservoir;
    mu_mmgr_reservoir = au->next;

    memset(au, 0, sizeof(*au));
    au->actual_size = mu_mmgr_calculate_actual_size(reported_size) + alignment;
#ifdef MU_MMGR_FORCE_EXACT_ALIGNMENT
    au->actual_size += alignment;
#endif

#ifdef MU_MMGR_RANDOM_FAILURE
    {
        double a = (double)rand();
        double b = RAND_MAX / 100.0 * MU_MMGR_RANDOM_FAILURE;
        if(a > b)
        {
            au->actual_address = malloc(au->actual_size);
        }
        else
        {
            mu_mmgr_log("[F] Random failure");
            au->actual_address = NULL;
        }
    }
#else
    au->actual_address = malloc(au->actual_size);
#endif

    au->reported_size     = reported_size;
    au->reported_address  = mu_mmgr_calculate_reported_address(au->actual_address);
    au->alignment        = alignment;
    au->allocation_type   = allocation_type;
    au->source_line       = source_line;
    au->allocation_number = mu_mmgr_current_allocation_count;

    size_t offset = ((size_t)au->reported_address) % alignment;
    if(offset)
    {
        au->reported_address = (uint8_t*)au->reported_address + (alignment - offset);
    }

#ifdef MU_MMGR_FORCE_EXACT_ALIGNMENT
    if(!((size_t)au->reported_address & alignment))
    {
        au->reported_address = (uint8_t*)au->reported_address + alignment;
        offset += alignment;
    }
#endif

    au->offset = offset;

    if(source_file)
    {
        mu_mmgr_strncpy(au->source_file, sizeof(au->source_file), mu_mmgr_source_file_basename(source_file), sizeof(au->source_file) - 1u);
    }
    else
    {
        mu_mmgr_strcpy(au->source_file, sizeof(au->source_file), "??");
    }

    if(source_func)
    {
        mu_mmgr_strncpy(au->source_func, sizeof(au->source_func), source_func, sizeof(au->source_func) - 1u);
    }
    else
    {
        mu_mmgr_strcpy(au->source_func, sizeof(au->source_func), "??");
    }

#if MU_MMGR_BACKTRACE
#if defined(_WIN32)
    au->backtrace_nptrs = CaptureStackBackTrace(mu_mmgr_stack_skip_count + 1, MU_MMGR_BACKTRACE_SIZE, au->backtrace_buffer, NULL);
    au->backtrace_skip = 0;
#else
    au->backtrace_nptrs = backtrace(au->backtrace_buffer, MU_MMGR_BACKTRACE_SIZE);
    au->backtrace_skip  = mu_mmgr_stack_skip_count + 1;
#endif
#endif

#ifndef MU_MMGR_RANDOM_FAILURE
    MU_ASSERT(au->actual_address != NULL);
#endif

    if(!au->actual_address)
    {
        mu_mmgr_unlock_alloc();
        MU_ASSERT(false && "Request for allocation failed. Out of memory.");
        return NULL;
    }

    MU_ASSERT(allocation_type != MU_MMGR_ALLOC_UNKNOWN);

    size_t hash_index = (((size_t)au->reported_address) >> 4u) & (MU_MMGR_HASH_SIZE - 1u);
    if(mu_mmgr_hash_table[hash_index])
    {
        mu_mmgr_hash_table[hash_index]->prev = au;
    }
    au->next                     = mu_mmgr_hash_table[hash_index];
    au->prev                     = NULL;
    mu_mmgr_hash_table[hash_index] = au;

    mu_mmgr_stats.total_reported_memory += (unsigned int)au->reported_size;
    mu_mmgr_stats.total_actual_memory += (unsigned int)au->actual_size;
    mu_mmgr_stats.total_alloc_unit_count++;
    if(mu_mmgr_stats.total_reported_memory > mu_mmgr_stats.peak_reported_memory)
    {
        mu_mmgr_stats.peak_reported_memory = mu_mmgr_stats.total_reported_memory;
    }
    if(mu_mmgr_stats.total_actual_memory > mu_mmgr_stats.peak_actual_memory)
    {
        mu_mmgr_stats.peak_actual_memory = mu_mmgr_stats.total_actual_memory;
    }
    if(mu_mmgr_stats.total_alloc_unit_count > mu_mmgr_stats.peak_alloc_unit_count)
    {
        mu_mmgr_stats.peak_alloc_unit_count = mu_mmgr_stats.total_alloc_unit_count;
    }
    mu_mmgr_stats.accumulated_reported_memory += (unsigned int)au->reported_size;
    mu_mmgr_stats.accumulated_actual_memory += (unsigned int)au->actual_size;
    mu_mmgr_stats.accumulated_alloc_unit_count++;

    mu_mmgr_wipe_with_pattern(au, mu_mmgr_unused_pattern, 0);

    if(allocation_type == MU_MMGR_ALLOC_CALLOC)
    {
        memset(au->reported_address, 0, au->reported_size);
    }

    if(mu_mmgr_opt_always_validate_all)
    {
        mu_mmgr_validate_all_units();
    }

    if(mu_mmgr_opt_always_log_all)
    {
        mu_mmgr_log("[+] ---->             addr 0x%08zX", (size_t)au->reported_address);
    }

    mu_mmgr_reset_globals();

#ifdef MU_MMGR_TEST
    mu_mmgr_log("[D] EXIT : mu_mmgr_alloc()");
#endif

    mu_mmgr_unlock_alloc();
    return au->reported_address;
}

void* mu_mmgr_realloc(const char*        source_file,
                      const unsigned int source_line,
                      const char*        source_func,
                      const unsigned int reallocation_type,
                      size_t             reported_size,
                      void*              reported_address)
{
    reported_size += sizeof(uint32_t) - 1u;
    reported_size &= ~(sizeof(uint32_t) - 1u);

    mu_mmgr_lock_alloc();

#ifdef MU_MMGR_TEST
    mu_mmgr_log("[D] ENTER: mu_mmgr_realloc()");
#endif

    if(!reported_address)
    {
        mu_mmgr_unlock_alloc();
        return mu_mmgr_alloc(source_file, source_line, source_func, reallocation_type, sizeof(void*), reported_size);
    }

    mu_mmgr_current_allocation_count++;
    MU_ASSERT(mu_mmgr_current_allocation_count != mu_mmgr_break_on_allocation_count);

    if(mu_mmgr_opt_always_log_all)
    {
        mu_mmgr_log("[~] %05u %8s of size 0x%08X(%08u) by %s", mu_mmgr_current_allocation_count,
                    mu_mmgr_allocation_types[reallocation_type], (unsigned)reported_size, (unsigned)reported_size,
                    mu_mmgr_owner_string(source_file, source_line, source_func));
    }

    mu_mmgr_alloc_unit* au = mu_mmgr_find_alloc_unit(reported_address);
    MU_ASSERT(au != NULL);
    if(!au)
    {
        mu_mmgr_unlock_alloc();
        MU_ASSERT(false && "Request to reallocate RAM that was never allocated");
        return NULL;
    }

    size_t alignment       = au->alignment;
    size_t old_reported_size = au->reported_size;

    MU_ASSERT(mu_mmgr_validate_alloc_unit(au));
    MU_ASSERT(reallocation_type != MU_MMGR_ALLOC_UNKNOWN);
    MU_ASSERT(au->allocation_type == MU_MMGR_ALLOC_MALLOC || au->allocation_type == MU_MMGR_ALLOC_CALLOC || au->allocation_type == MU_MMGR_ALLOC_REALLOC);
    MU_ASSERT(au->break_on_realloc == false);

    unsigned int original_reported_size = (unsigned int)au->reported_size;

    if(mu_mmgr_opt_always_log_all)
    {
        mu_mmgr_log("[~] ---->             from 0x%08X(%08u)", original_reported_size, original_reported_size);
    }

    void*  old_reported_address = reported_address;
    size_t new_actual_size      = mu_mmgr_calculate_actual_size(reported_size) + alignment;
#ifdef MU_MMGR_FORCE_EXACT_ALIGNMENT
    new_actual_size += alignment;
#endif

    size_t min_reported_size = old_reported_size < reported_size ? old_reported_size : reported_size;
    void*  old_data         = malloc(min_reported_size ? min_reported_size : 1u);
    if(old_data && min_reported_size)
    {
        memcpy(old_data, old_reported_address, min_reported_size);
    }

    void* new_actual_address = NULL;
#ifdef MU_MMGR_RANDOM_FAILURE
    {
        double a = (double)rand();
        double b = RAND_MAX / 100.0 * MU_MMGR_RANDOM_FAILURE;
        if(a > b)
        {
            new_actual_address = realloc(au->actual_address, new_actual_size);
        }
        else
        {
            mu_mmgr_log("[F] Random failure");
        }
    }
#else
    new_actual_address = realloc(au->actual_address, new_actual_size);
#endif

#ifndef MU_MMGR_RANDOM_FAILURE
    MU_ASSERT(new_actual_address != NULL);
#endif

    if(!new_actual_address)
    {
        if(old_data)
        {
            free(old_data);
        }
        mu_mmgr_unlock_alloc();
        MU_ASSERT(false && "Request for reallocation failed. Out of memory.");
        return NULL;
    }

    mu_mmgr_stats.total_reported_memory -= (unsigned int)au->reported_size;
    mu_mmgr_stats.total_actual_memory -= (unsigned int)au->actual_size;

    au->actual_size       = new_actual_size;
    au->actual_address    = new_actual_address;
    au->reported_size     = reported_size;
    au->reported_address  = mu_mmgr_calculate_reported_address(new_actual_address);
    au->allocation_type   = reallocation_type;
    au->source_line       = source_line;
    au->allocation_number = mu_mmgr_current_allocation_count;

    size_t offset = ((size_t)au->reported_address) % alignment;
    if(offset)
    {
        au->reported_address = (uint8_t*)au->reported_address + (alignment - offset);
    }

#ifdef MU_MMGR_FORCE_EXACT_ALIGNMENT
    if(!((size_t)au->reported_address & alignment))
    {
        au->reported_address = (uint8_t*)au->reported_address + alignment;
        offset += alignment;
    }
#endif

    if(offset != au->offset)
    {
        size_t size = old_reported_size < reported_size ? old_reported_size : reported_size;
        if(old_data && size)
        {
            memcpy(au->reported_address, old_data, size);
        }
        au->offset = offset;
    }

    if(old_data)
    {
        free(old_data);
    }

    if(source_file)
    {
        mu_mmgr_strncpy(au->source_file, sizeof(au->source_file), mu_mmgr_source_file_basename(source_file), sizeof(au->source_file) - 1u);
    }
    else
    {
        mu_mmgr_strcpy(au->source_file, sizeof(au->source_file), "??");
    }

    if(source_func)
    {
        mu_mmgr_strncpy(au->source_func, sizeof(au->source_func), source_func, sizeof(au->source_func) - 1u);
    }
    else
    {
        mu_mmgr_strcpy(au->source_func, sizeof(au->source_func), "??");
    }

#if MU_MMGR_BACKTRACE
#if defined(_WIN32)
    au->backtrace_nptrs = CaptureStackBackTrace(mu_mmgr_stack_skip_count + 1, MU_MMGR_BACKTRACE_SIZE, au->backtrace_buffer, NULL);
    au->backtrace_skip = 0;
#else
    au->backtrace_nptrs = backtrace(au->backtrace_buffer, MU_MMGR_BACKTRACE_SIZE);
    au->backtrace_skip  = mu_mmgr_stack_skip_count + 1;
#endif
#endif

    if(old_reported_address != au->reported_address)
    {
        size_t old_hash = (((size_t)old_reported_address) >> 4u) & (MU_MMGR_HASH_SIZE - 1u);
        if(mu_mmgr_hash_table[old_hash] == au)
        {
            mu_mmgr_hash_table[old_hash] = mu_mmgr_hash_table[old_hash]->next;
        }
        else
        {
            if(au->prev)
            {
                au->prev->next = au->next;
            }
            if(au->next)
            {
                au->next->prev = au->prev;
            }
        }

        size_t new_hash = (((size_t)au->reported_address) >> 4u) & (MU_MMGR_HASH_SIZE - 1u);
        if(mu_mmgr_hash_table[new_hash])
        {
            mu_mmgr_hash_table[new_hash]->prev = au;
        }
        au->next                   = mu_mmgr_hash_table[new_hash];
        au->prev                   = NULL;
        mu_mmgr_hash_table[new_hash] = au;
    }

    mu_mmgr_stats.total_reported_memory += (unsigned int)au->reported_size;
    mu_mmgr_stats.total_actual_memory += (unsigned int)au->actual_size;
    if(mu_mmgr_stats.total_reported_memory > mu_mmgr_stats.peak_reported_memory)
    {
        mu_mmgr_stats.peak_reported_memory = mu_mmgr_stats.total_reported_memory;
    }
    if(mu_mmgr_stats.total_actual_memory > mu_mmgr_stats.peak_actual_memory)
    {
        mu_mmgr_stats.peak_actual_memory = mu_mmgr_stats.total_actual_memory;
    }
    {
        int delta_reported_size = (int)(reported_size - original_reported_size);
        if(delta_reported_size > 0)
        {
            mu_mmgr_stats.accumulated_reported_memory += (unsigned int)delta_reported_size;
            mu_mmgr_stats.accumulated_actual_memory += (unsigned int)delta_reported_size;
        }
    }

    mu_mmgr_wipe_with_pattern(au, mu_mmgr_unused_pattern, original_reported_size);

    MU_ASSERT(mu_mmgr_validate_alloc_unit(au));

    if(mu_mmgr_opt_always_validate_all)
    {
        mu_mmgr_validate_all_units();
    }

    if(mu_mmgr_opt_always_log_all)
    {
        mu_mmgr_log("[~] ---->             addr 0x%08zX", (size_t)au->reported_address);
    }

    mu_mmgr_reset_globals();

#ifdef MU_MMGR_TEST
    mu_mmgr_log("[D] EXIT : mu_mmgr_realloc()");
#endif

    mu_mmgr_unlock_alloc();
    return au->reported_address;
}

void mu_mmgr_free(const char*        source_file,
                     const unsigned int source_line,
                     const char*        source_func,
                     const unsigned int deallocation_type,
                     const void*        reported_address)
{
    mu_mmgr_lock_alloc();

#ifdef MU_MMGR_TEST
    mu_mmgr_log("[D] ENTER: mu_mmgr_free()");
#endif

    if(mu_mmgr_opt_always_log_all)
    {
        mu_mmgr_log("[-] ----- %8s of addr 0x%08zX           by %s", mu_mmgr_allocation_types[deallocation_type],
                    (size_t)((void*)reported_address), mu_mmgr_owner_string(source_file, source_line, source_func));
    }

    if(reported_address)
    {
        mu_mmgr_alloc_unit* au = mu_mmgr_find_alloc_unit(reported_address);

        MU_ASSERT(au != NULL);
        if(!au)
        {
            mu_mmgr_unlock_alloc();
            MU_ASSERT(false && "Request to deallocate RAM that was never allocated");
            return;
        }

        MU_ASSERT(mu_mmgr_validate_alloc_unit(au));
        MU_ASSERT(deallocation_type != MU_MMGR_ALLOC_UNKNOWN);
        MU_ASSERT((deallocation_type == MU_MMGR_ALLOC_DELETE && au->allocation_type == MU_MMGR_ALLOC_NEW)
                    || (deallocation_type == MU_MMGR_ALLOC_DELETE_ARRAY && au->allocation_type == MU_MMGR_ALLOC_NEW_ARRAY)
                    || (deallocation_type == MU_MMGR_ALLOC_FREE && au->allocation_type == MU_MMGR_ALLOC_MALLOC)
                    || (deallocation_type == MU_MMGR_ALLOC_FREE && au->allocation_type == MU_MMGR_ALLOC_CALLOC)
                    || (deallocation_type == MU_MMGR_ALLOC_FREE && au->allocation_type == MU_MMGR_ALLOC_REALLOC)
                    || (deallocation_type == MU_MMGR_ALLOC_UNKNOWN));
        MU_ASSERT(au->break_on_dealloc == false);

        mu_mmgr_wipe_with_pattern(au, mu_mmgr_released_pattern, 0);

        free(au->actual_address);

        size_t hash_index = ((size_t)au->reported_address >> 4u) & (MU_MMGR_HASH_SIZE - 1u);
        if(mu_mmgr_hash_table[hash_index] == au)
        {
            mu_mmgr_hash_table[hash_index] = au->next;
        }
        else
        {
            if(au->prev)
            {
                au->prev->next = au->next;
            }
            if(au->next)
            {
                au->next->prev = au->prev;
            }
        }

        mu_mmgr_stats.total_reported_memory -= (unsigned int)au->reported_size;
        mu_mmgr_stats.total_actual_memory -= (unsigned int)au->actual_size;
        mu_mmgr_stats.total_alloc_unit_count--;

        memset(au, 0, sizeof(*au));
        au->next          = mu_mmgr_reservoir;
        mu_mmgr_reservoir = au;
    }

    mu_mmgr_reset_globals();

    if(mu_mmgr_opt_always_validate_all)
    {
        mu_mmgr_validate_all_units();
    }

#ifdef MU_MMGR_TEST
    mu_mmgr_log("[D] EXIT : mu_mmgr_free()");
#endif

    mu_mmgr_unlock_alloc();
}

bool* mu_mmgr_break_on_realloc(void* reported_address)
{
    mu_mmgr_alloc_unit* au = mu_mmgr_find_alloc_unit(reported_address);
    MU_ASSERT(au != NULL);
    MU_ASSERT(au->allocation_type == MU_MMGR_ALLOC_MALLOC || au->allocation_type == MU_MMGR_ALLOC_CALLOC || au->allocation_type == MU_MMGR_ALLOC_REALLOC);
    return &au->break_on_realloc;
}

bool* mu_mmgr_break_on_dealloc(void* reported_address)
{
    mu_mmgr_alloc_unit* au = mu_mmgr_find_alloc_unit(reported_address);
    MU_ASSERT(au != NULL);
    return &au->break_on_dealloc;
}

bool mu_mmgr_validate_address(const void* reported_address)
{
    return mu_mmgr_find_alloc_unit(reported_address) != NULL;
}

bool mu_mmgr_validate_alloc_unit(const mu_mmgr_alloc_unit* alloc_unit)
{
    uint8_t* pre       = (uint8_t*)alloc_unit->reported_address - mu_mmgr_padding_size * sizeof(uint32_t);
    uint8_t* post      = (uint8_t*)alloc_unit->reported_address + alloc_unit->reported_size;
    bool     error_flag = false;

    size_t padding_bytes = mu_mmgr_padding_size * sizeof(uint32_t);
    for(size_t i = 0; i < padding_bytes; ++i, ++pre, ++post)
    {
        uint8_t expected_prefix_byte = (uint8_t)((mu_mmgr_prefix_pattern >> ((i % sizeof(uint32_t)) * 8)) & 0xFFu);
        if(*pre != expected_prefix_byte)
        {
            mu_mmgr_log("[!] A memory allocation unit was corrupt because of an underrun:");
            mu_mmgr_dump_alloc_unit(alloc_unit, "  ");
            error_flag = true;
        }
        MU_ASSERT(*pre == expected_prefix_byte);

        uint8_t expected_postfix_byte = (uint8_t)((mu_mmgr_postfix_pattern >> ((i % sizeof(uint32_t)) * 8)) & 0xFFu);
        if(*post != expected_postfix_byte)
        {
            mu_mmgr_log("[!] A memory allocation unit was corrupt because of an overrun:");
            mu_mmgr_dump_alloc_unit(alloc_unit, "  ");
            error_flag = true;
        }
        MU_ASSERT(*post == expected_postfix_byte);
    }

    return !error_flag;
}

bool mu_mmgr_validate_all_units(void)
{
    unsigned int errors     = 0;
    unsigned int alloc_count = 0;

    for(unsigned int i = 0; i < MU_MMGR_HASH_SIZE; ++i)
    {
        mu_mmgr_alloc_unit* ptr = mu_mmgr_hash_table[i];
        while(ptr)
        {
            alloc_count++;
            if(!mu_mmgr_validate_alloc_unit(ptr))
            {
                errors++;
            }
            ptr = ptr->next;
        }
    }

    if(alloc_count != mu_mmgr_stats.total_alloc_unit_count)
    {
        mu_mmgr_log("[!] Memory tracking hash table corrupt!");
        errors++;
    }

    MU_ASSERT(alloc_count == mu_mmgr_stats.total_alloc_unit_count);
    MU_ASSERT(errors == 0);

    if(errors)
    {
        mu_mmgr_log("[!] While validating all allocation units, %u allocation unit(s) were found to have problems", errors);
    }

    return errors != 0;
}

unsigned int mu_mmgr_calc_unused(const mu_mmgr_alloc_unit* alloc_unit)
{
    const uint32_t* ptr   = (const uint32_t*)alloc_unit->reported_address;
    unsigned int    count = 0;

    for(unsigned int i = 0; i < alloc_unit->reported_size; i += (unsigned int)sizeof(uint32_t), ++ptr)
    {
        if(*ptr == mu_mmgr_unused_pattern)
        {
            count += (unsigned int)sizeof(uint32_t);
        }
    }

    return count;
}

unsigned int mu_mmgr_calc_all_unused(void)
{
    unsigned int total = 0;
    for(unsigned int i = 0; i < MU_MMGR_HASH_SIZE; ++i)
    {
        mu_mmgr_alloc_unit* ptr = mu_mmgr_hash_table[i];
        while(ptr)
        {
            total += mu_mmgr_calc_unused(ptr);
            ptr = ptr->next;
        }
    }
    return total;
}

void mu_mmgr_set_executable_name(const char* name, size_t length)
{
    if(!name)
    {
        mu_mmgr_executable_name[0] = '\0';
        return;
    }

    size_t copy_len = length;
    if(copy_len >= sizeof(mu_mmgr_executable_name))
    {
        copy_len = sizeof(mu_mmgr_executable_name) - 1u;
    }
    memcpy(mu_mmgr_executable_name, name, copy_len);
    mu_mmgr_executable_name[copy_len] = '\0';

    if(!mu_mmgr_app_name || mu_mmgr_app_name[0] == '\0')
    {
        mu_mmgr_app_name = mu_mmgr_executable_name;
    }
}

void mu_mmgr_set_log_file_directory(const char* directory)
{
    if(!directory)
    {
        mu_mmgr_log_directory[0] = '\0';
        return;
    }
    mu_mmgr_strcpy(mu_mmgr_log_directory, sizeof(mu_mmgr_log_directory), directory);
}

void mu_mmgr_dump_alloc_unit(const mu_mmgr_alloc_unit* alloc_unit, const char* prefix)
{
    const char* p = prefix ? prefix : "";
    mu_mmgr_log("[I] %sAddress (reported): %010p", p, alloc_unit->reported_address);
    mu_mmgr_log("[I] %sAddress (actual)  : %010p", p, alloc_unit->actual_address);
    mu_mmgr_log("[I] %sSize (reported)   : 0x%08X (%s)", p, (unsigned int)alloc_unit->reported_size,
                mu_mmgr_memory_size_string((unsigned int)alloc_unit->reported_size));
    mu_mmgr_log("[I] %sSize (actual)     : 0x%08X (%s)", p, (unsigned int)alloc_unit->actual_size,
                mu_mmgr_memory_size_string((unsigned int)alloc_unit->actual_size));
    mu_mmgr_log("[I] %sOwner             : %s(%u)::%s", p, alloc_unit->source_file, alloc_unit->source_line, alloc_unit->source_func);
#if MU_MMGR_BACKTRACE
    mu_mmgr_log("[I] %sBacktrace         :", p);
    mu_mmgr_dump_backtrace(NULL, alloc_unit);
#endif
    mu_mmgr_log("[I] %sAllocation type   : %s", p, mu_mmgr_allocation_types[alloc_unit->allocation_type]);
    mu_mmgr_log("[I] %sAllocation number : %u", p, alloc_unit->allocation_number);
}

void mu_mmgr_dump_memory_report(const char* filename, const bool overwrite)
{
    if(!filename)
    {
        return;
    }

    char output_path[MU_MMGR_PATH_MAX];
    mu_mmgr_build_path(output_path, sizeof(output_path), filename);

    FILE* fh = fopen(output_path, overwrite ? "wb" : "ab");
    if(!fh)
    {
        return;
    }

    time_t    t = time(NULL);
    struct tm tme;
    memset(&tme, 0, sizeof(tme));
    (void)mu_mmgr_localtime_safe(&t, &tme);

    fprintf(fh, " ----------------------------------------------------------------------------------------------------------------------------- -----\n");
    fprintf(fh, "|                                             Memory report for: %02d/%02d/%04d %02d:%02d:%02d                                           |\n",
            tme.tm_mon + 1, tme.tm_mday, tme.tm_year + 1900, tme.tm_hour, tme.tm_min, tme.tm_sec);
    fprintf(fh, " ----------------------------------------------------------------------------------------------------------------------------- -----\n\n");

    fprintf(fh, "Allocation unit count: %10s\n", mu_mmgr_insert_commas(mu_mmgr_stats.total_alloc_unit_count));
    fprintf(fh, "Reported to application: %s\n", mu_mmgr_memory_size_string(mu_mmgr_stats.total_reported_memory));
    fprintf(fh, "Actual total memory in use: %s\n", mu_mmgr_memory_size_string(mu_mmgr_stats.total_actual_memory));
    fprintf(fh, "Memory tracking overhead: %s\n",
            mu_mmgr_memory_size_string(mu_mmgr_stats.total_actual_memory - mu_mmgr_stats.total_reported_memory));
    fprintf(fh, "\n");

    mu_mmgr_dump_allocations(fh);

    fclose(fh);
}

mu_mmgr_memory_stats mu_mmgr_stats_get(void)
{
    return mu_mmgr_stats;
}

#endif /* MU_MMGR_IMPLEMENTATION */
