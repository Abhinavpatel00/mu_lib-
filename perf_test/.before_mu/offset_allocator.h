// (C) Sebastian Aaltonen 2023
// MIT License (see file: OffsetAllocator/LICENSE)
// C99 port for vkutil

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration
// #define USE_16_BIT_NODE_INDICES

typedef uint8_t  oa_uint8;
typedef uint16_t oa_uint16;
typedef uint32_t oa_uint32;

#ifdef USE_16_BIT_NODE_INDICES
typedef oa_uint16 OA_NodeIndex;
#else
typedef oa_uint32 OA_NodeIndex;
#endif

enum
{
    OA_NUM_TOP_BINS          = 32,
    OA_BINS_PER_LEAF         = 8,
    OA_TOP_BINS_INDEX_SHIFT  = 3,
    OA_LEAF_BINS_INDEX_MASK  = 0x7,
    OA_NUM_LEAF_BINS         = OA_NUM_TOP_BINS * OA_BINS_PER_LEAF,
};

#define OA_NO_SPACE 0xffffffffu
#define OA_NODE_UNUSED ((OA_NodeIndex)0xffffffffu)

typedef struct OA_Allocation
{
    oa_uint32   offset;
    OA_NodeIndex metadata; // internal: node index
} OA_Allocation;

typedef struct OA_StorageReport
{
    oa_uint32 total_free_space;
    oa_uint32 largest_free_region;
} OA_StorageReport;

typedef struct OA_StorageReportFull
{
    struct
    {
        oa_uint32 size;
        oa_uint32 count;
    } free_regions[OA_NUM_LEAF_BINS];
} OA_StorageReportFull;

typedef struct OA_Node
{
    oa_uint32   data_offset;
    oa_uint32   data_size;
    OA_NodeIndex bin_list_prev;
    OA_NodeIndex bin_list_next;
    OA_NodeIndex neighbor_prev;
    OA_NodeIndex neighbor_next;
    bool        used;
} OA_Node;

typedef struct OA_Allocator
{
    oa_uint32   size;
    oa_uint32   max_allocs;
    oa_uint32   free_storage;

    oa_uint32   used_bins_top;
    oa_uint8    used_bins[OA_NUM_TOP_BINS];
    OA_NodeIndex bin_indices[OA_NUM_LEAF_BINS];

    OA_Node*     nodes;
    OA_NodeIndex* free_nodes;
    oa_uint32    free_offset;
} OA_Allocator;

void oa_init(OA_Allocator* allocator, oa_uint32 size, oa_uint32 max_allocs);
void oa_destroy(OA_Allocator* allocator);
void oa_reset(OA_Allocator* allocator);

OA_Allocation oa_allocate(OA_Allocator* allocator, oa_uint32 size);
OA_Allocation oa_allocate_aligned(OA_Allocator* allocator, oa_uint32 size, oa_uint32 alignment);
void oa_free(OA_Allocator* allocator, OA_Allocation allocation);

oa_uint32 oa_allocation_size(const OA_Allocator* allocator, OA_Allocation allocation);
OA_StorageReport oa_storage_report(const OA_Allocator* allocator);
OA_StorageReportFull oa_storage_report_full(const OA_Allocator* allocator);

#ifdef DEBUG
void oa_debug_validate(const OA_Allocator* allocator);
#else
static inline void oa_debug_validate(const OA_Allocator* allocator)
{
    (void)allocator;
}
#endif

#ifdef __cplusplus
}
#endif
