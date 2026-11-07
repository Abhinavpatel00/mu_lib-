#include "mu_offset_allocator.h"

#include <stdlib.h>
#include <string.h>

#ifdef DEBUG
#include <assert.h>
#define OA_ASSERT(x) assert(x)
//#define OA_DEBUG_VERBOSE
#else
#define OA_ASSERT(x)
#endif

#ifdef OA_DEBUG_VERBOSE
#include <stdio.h>
#endif

#ifdef _MSC_VER
#include <intrin.h>
#endif

static oa_uint32 oa_lzcnt_nonzero(oa_uint32 v)
{
#ifdef _MSC_VER
    unsigned long retVal;
    _BitScanReverse(&retVal, v);
    return 31u - retVal;
#else
    return (oa_uint32)__builtin_clz(v);
#endif
}

static oa_uint32 oa_tzcnt_nonzero(oa_uint32 v)
{
#ifdef _MSC_VER
    unsigned long retVal;
    _BitScanForward(&retVal, v);
    return (oa_uint32)retVal;
#else
    return (oa_uint32)__builtin_ctz(v);
#endif
}

// ------------------------------------------------------------
// SmallFloat (binning)
// ------------------------------------------------------------

enum
{
    OA_MANTISSA_BITS  = 3,
    OA_MANTISSA_VALUE = 1 << OA_MANTISSA_BITS,
    OA_MANTISSA_MASK  = OA_MANTISSA_VALUE - 1,
};

static oa_uint32 oa_uint_to_float_round_up(oa_uint32 size)
{
    oa_uint32 exp = 0;
    oa_uint32 mantissa = 0;

    if(size < OA_MANTISSA_VALUE)
    {
        mantissa = size;
    }
    else
    {
        oa_uint32 leadingZeros = oa_lzcnt_nonzero(size);
        oa_uint32 highestSetBit = 31u - leadingZeros;

        oa_uint32 mantissaStartBit = highestSetBit - OA_MANTISSA_BITS;
        exp = mantissaStartBit + 1;
        mantissa = (size >> mantissaStartBit) & OA_MANTISSA_MASK;

        oa_uint32 lowBitsMask = (1u << mantissaStartBit) - 1u;
        if((size & lowBitsMask) != 0)
            mantissa++;
    }

    return (exp << OA_MANTISSA_BITS) + mantissa;
}

static oa_uint32 oa_uint_to_float_round_down(oa_uint32 size)
{
    oa_uint32 exp = 0;
    oa_uint32 mantissa = 0;

    if(size < OA_MANTISSA_VALUE)
    {
        mantissa = size;
    }
    else
    {
        oa_uint32 leadingZeros = oa_lzcnt_nonzero(size);
        oa_uint32 highestSetBit = 31u - leadingZeros;

        oa_uint32 mantissaStartBit = highestSetBit - OA_MANTISSA_BITS;
        exp = mantissaStartBit + 1;
        mantissa = (size >> mantissaStartBit) & OA_MANTISSA_MASK;
    }

    return (exp << OA_MANTISSA_BITS) | mantissa;
}

static oa_uint32 oa_float_to_uint(oa_uint32 floatValue)
{
    oa_uint32 exponent = floatValue >> OA_MANTISSA_BITS;
    oa_uint32 mantissa = floatValue & OA_MANTISSA_MASK;
    if(exponent == 0)
    {
        return mantissa;
    }
    return (mantissa | OA_MANTISSA_VALUE) << (exponent - 1u);
}

// ------------------------------------------------------------
// Utility
// ------------------------------------------------------------

static oa_uint32 oa_find_lowest_set_bit_after(oa_uint32 bitMask, oa_uint32 startBitIndex)
{
    oa_uint32 maskBeforeStartIndex = (1u << startBitIndex) - 1u;
    oa_uint32 maskAfterStartIndex = ~maskBeforeStartIndex;
    oa_uint32 bitsAfter = bitMask & maskAfterStartIndex;
    if(bitsAfter == 0)
        return OA_NO_SPACE;
    return oa_tzcnt_nonzero(bitsAfter);
}

static oa_uint32 oa_align_up(oa_uint32 value, oa_uint32 alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static oa_uint32 oa_insert_node_into_bin(OA_Allocator* allocator, oa_uint32 size, oa_uint32 dataOffset)
{
    oa_uint32 binIndex = oa_uint_to_float_round_down(size);

    oa_uint32 topBinIndex = binIndex >> OA_TOP_BINS_INDEX_SHIFT;
    oa_uint32 leafBinIndex = binIndex & OA_LEAF_BINS_INDEX_MASK;

    if(allocator->bin_indices[binIndex] == OA_NODE_UNUSED)
    {
        allocator->used_bins[topBinIndex] |= 1u << leafBinIndex;
        allocator->used_bins_top |= 1u << topBinIndex;
    }

    oa_uint32 topNodeIndex = allocator->bin_indices[binIndex];
    oa_uint32 nodeIndex = allocator->free_nodes[allocator->free_offset--];
#ifdef OA_DEBUG_VERBOSE
    printf("Getting node %u from freelist[%u]\n", nodeIndex, allocator->free_offset + 1u);
#endif

    allocator->nodes[nodeIndex].data_offset = dataOffset;
    allocator->nodes[nodeIndex].data_size = size;
    allocator->nodes[nodeIndex].bin_list_prev = OA_NODE_UNUSED;
    allocator->nodes[nodeIndex].bin_list_next = (OA_NodeIndex)topNodeIndex;
    allocator->nodes[nodeIndex].neighbor_prev = OA_NODE_UNUSED;
    allocator->nodes[nodeIndex].neighbor_next = OA_NODE_UNUSED;
    allocator->nodes[nodeIndex].used = false;

    if(topNodeIndex != OA_NODE_UNUSED)
        allocator->nodes[topNodeIndex].bin_list_prev = (OA_NodeIndex)nodeIndex;
    allocator->bin_indices[binIndex] = (OA_NodeIndex)nodeIndex;

    allocator->free_storage += size;
#ifdef OA_DEBUG_VERBOSE
    printf("Free storage: %u (+%u) (insert_node)\n", allocator->free_storage, size);
#endif

    return nodeIndex;
}

static void oa_remove_node_from_bin(OA_Allocator* allocator, oa_uint32 nodeIndex)
{
    OA_Node* node = &allocator->nodes[nodeIndex];

    if(node->bin_list_prev != OA_NODE_UNUSED)
    {
        allocator->nodes[node->bin_list_prev].bin_list_next = node->bin_list_next;
        if(node->bin_list_next != OA_NODE_UNUSED)
            allocator->nodes[node->bin_list_next].bin_list_prev = node->bin_list_prev;
    }
    else
    {
        oa_uint32 binIndex = oa_uint_to_float_round_down(node->data_size);
        oa_uint32 topBinIndex = binIndex >> OA_TOP_BINS_INDEX_SHIFT;
        oa_uint32 leafBinIndex = binIndex & OA_LEAF_BINS_INDEX_MASK;

        allocator->bin_indices[binIndex] = node->bin_list_next;
        if(node->bin_list_next != OA_NODE_UNUSED)
            allocator->nodes[node->bin_list_next].bin_list_prev = OA_NODE_UNUSED;

        if(allocator->bin_indices[binIndex] == OA_NODE_UNUSED)
        {
            allocator->used_bins[topBinIndex] &= ~(1u << leafBinIndex);
            if(allocator->used_bins[topBinIndex] == 0)
                allocator->used_bins_top &= ~(1u << topBinIndex);
        }
    }

#ifdef OA_DEBUG_VERBOSE
    printf("Putting node %u into freelist[%u] (remove_node)\n", nodeIndex, allocator->free_offset + 1u);
#endif
    allocator->free_nodes[++allocator->free_offset] = (OA_NodeIndex)nodeIndex;

    allocator->free_storage -= node->data_size;
#ifdef OA_DEBUG_VERBOSE
    printf("Free storage: %u (-%u) (remove_node)\n", allocator->free_storage, node->data_size);
#endif
}

static void oa_unlink_node_from_bin(OA_Allocator* allocator, oa_uint32 nodeIndex)
{
    OA_Node* node = &allocator->nodes[nodeIndex];

    if(node->bin_list_prev != OA_NODE_UNUSED)
    {
        allocator->nodes[node->bin_list_prev].bin_list_next = node->bin_list_next;
        if(node->bin_list_next != OA_NODE_UNUSED)
            allocator->nodes[node->bin_list_next].bin_list_prev = node->bin_list_prev;
    }
    else
    {
        oa_uint32 binIndex = oa_uint_to_float_round_down(node->data_size);
        oa_uint32 topBinIndex = binIndex >> OA_TOP_BINS_INDEX_SHIFT;
        oa_uint32 leafBinIndex = binIndex & OA_LEAF_BINS_INDEX_MASK;

        allocator->bin_indices[binIndex] = node->bin_list_next;
        if(node->bin_list_next != OA_NODE_UNUSED)
            allocator->nodes[node->bin_list_next].bin_list_prev = OA_NODE_UNUSED;

        if(allocator->bin_indices[binIndex] == OA_NODE_UNUSED)
        {
            allocator->used_bins[topBinIndex] &= ~(1u << leafBinIndex);
            if(allocator->used_bins[topBinIndex] == 0)
                allocator->used_bins_top &= ~(1u << topBinIndex);
        }
    }

    node->bin_list_prev = OA_NODE_UNUSED;
    node->bin_list_next = OA_NODE_UNUSED;
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

void oa_init(OA_Allocator* allocator, oa_uint32 size, oa_uint32 max_allocs)
{
    if(!allocator)
        return;

    allocator->size = size;
    allocator->max_allocs = max_allocs;
    allocator->nodes = NULL;
    allocator->free_nodes = NULL;

    if(sizeof(OA_NodeIndex) == 2)
    {
        OA_ASSERT(max_allocs <= 65536u);
    }

    oa_reset(allocator);
}

void oa_destroy(OA_Allocator* allocator)
{
    if(!allocator)
        return;

    free(allocator->nodes);
    free(allocator->free_nodes);
    allocator->nodes = NULL;
    allocator->free_nodes = NULL;
    allocator->size = 0;
    allocator->max_allocs = 0;
    allocator->free_storage = 0;
    allocator->used_bins_top = 0;
    allocator->free_offset = 0;
}

void oa_reset(OA_Allocator* allocator)
{
    if(!allocator)
        return;

    allocator->free_storage = 0;
    allocator->used_bins_top = 0;
    allocator->free_offset = allocator->max_allocs - 1u;

    for(oa_uint32 i = 0; i < OA_NUM_TOP_BINS; i++)
        allocator->used_bins[i] = 0;

    for(oa_uint32 i = 0; i < OA_NUM_LEAF_BINS; i++)
        allocator->bin_indices[i] = OA_NODE_UNUSED;

    free(allocator->nodes);
    free(allocator->free_nodes);

    allocator->nodes = (OA_Node*)malloc(sizeof(OA_Node) * allocator->max_allocs);
    allocator->free_nodes = (OA_NodeIndex*)malloc(sizeof(OA_NodeIndex) * allocator->max_allocs);

    for(oa_uint32 i = 0; i < allocator->max_allocs; i++)
        allocator->free_nodes[i] = (OA_NodeIndex)(allocator->max_allocs - i - 1u);

    oa_insert_node_into_bin(allocator, allocator->size, 0);

    oa_debug_validate(allocator);
}

OA_Allocation oa_allocate(OA_Allocator* allocator, oa_uint32 size)
{
    if(!allocator)
        return (OA_Allocation){ .offset = OA_NO_SPACE, .metadata = OA_NODE_UNUSED };

    if(allocator->free_offset == 0)
        return (OA_Allocation){ .offset = OA_NO_SPACE, .metadata = OA_NODE_UNUSED };

    oa_uint32 minBinIndex = oa_uint_to_float_round_up(size);
    oa_uint32 minTopBinIndex = minBinIndex >> OA_TOP_BINS_INDEX_SHIFT;
    oa_uint32 minLeafBinIndex = minBinIndex & OA_LEAF_BINS_INDEX_MASK;

    oa_uint32 topBinIndex = minTopBinIndex;
    oa_uint32 leafBinIndex = OA_NO_SPACE;

    if(allocator->used_bins_top & (1u << topBinIndex))
        leafBinIndex = oa_find_lowest_set_bit_after(allocator->used_bins[topBinIndex], minLeafBinIndex);

    if(leafBinIndex == OA_NO_SPACE)
    {
        topBinIndex = oa_find_lowest_set_bit_after(allocator->used_bins_top, minTopBinIndex + 1u);
        if(topBinIndex == OA_NO_SPACE)
            return (OA_Allocation){ .offset = OA_NO_SPACE, .metadata = OA_NODE_UNUSED };

        leafBinIndex = oa_tzcnt_nonzero(allocator->used_bins[topBinIndex]);
    }

    oa_uint32 binIndex = (topBinIndex << OA_TOP_BINS_INDEX_SHIFT) | leafBinIndex;

    oa_uint32 nodeIndex = allocator->bin_indices[binIndex];
    OA_Node* node = &allocator->nodes[nodeIndex];
    oa_uint32 nodeTotalSize = node->data_size;
    node->data_size = size;
    node->used = true;
    allocator->bin_indices[binIndex] = node->bin_list_next;
    if(node->bin_list_next != OA_NODE_UNUSED)
        allocator->nodes[node->bin_list_next].bin_list_prev = OA_NODE_UNUSED;

    allocator->free_storage -= nodeTotalSize;
#ifdef OA_DEBUG_VERBOSE
    printf("Free storage: %u (-%u) (allocate)\n", allocator->free_storage, nodeTotalSize);
#endif

    if(allocator->bin_indices[binIndex] == OA_NODE_UNUSED)
    {
        allocator->used_bins[topBinIndex] &= ~(1u << leafBinIndex);
        if(allocator->used_bins[topBinIndex] == 0)
            allocator->used_bins_top &= ~(1u << topBinIndex);
    }

    oa_uint32 reminderSize = nodeTotalSize - size;
    if(reminderSize > 0)
    {
        oa_uint32 newNodeIndex = oa_insert_node_into_bin(allocator, reminderSize, node->data_offset + size);

        if(node->neighbor_next != OA_NODE_UNUSED)
            allocator->nodes[node->neighbor_next].neighbor_prev = (OA_NodeIndex)newNodeIndex;

        allocator->nodes[newNodeIndex].neighbor_prev = (OA_NodeIndex)nodeIndex;
        allocator->nodes[newNodeIndex].neighbor_next = node->neighbor_next;
        node->neighbor_next = (OA_NodeIndex)newNodeIndex;
    }

    oa_debug_validate(allocator);
    return (OA_Allocation){ .offset = node->data_offset, .metadata = (OA_NodeIndex)nodeIndex };
}

OA_Allocation oa_allocate_aligned(OA_Allocator* allocator, oa_uint32 size, oa_uint32 alignment)
{
    if(!allocator)
        return (OA_Allocation){ .offset = OA_NO_SPACE, .metadata = OA_NODE_UNUSED };

    if(allocator->free_offset == 0)
        return (OA_Allocation){ .offset = OA_NO_SPACE, .metadata = OA_NODE_UNUSED };

    if(alignment == 0)
        alignment = 1u;

    OA_ASSERT((alignment & (alignment - 1u)) == 0);

    oa_uint32 minBinIndex = oa_uint_to_float_round_up(size);
    oa_uint32 minTopBinIndex = minBinIndex >> OA_TOP_BINS_INDEX_SHIFT;
    oa_uint32 minLeafBinIndex = minBinIndex & OA_LEAF_BINS_INDEX_MASK;

    oa_uint32 selectedNodeIndex = OA_NODE_UNUSED;
    oa_uint32 alignedOffset = 0;
    oa_uint32 leadingSize = 0;
    oa_uint32 trailingSize = 0;

    for(oa_uint32 topBinIndex = minTopBinIndex; topBinIndex < OA_NUM_TOP_BINS; topBinIndex++)
    {
        if((allocator->used_bins_top & (1u << topBinIndex)) == 0)
        {
            minLeafBinIndex = 0;
            continue;
        }

        oa_uint32 leafMask = allocator->used_bins[topBinIndex];
        oa_uint32 leafBinIndex = oa_find_lowest_set_bit_after(leafMask, minLeafBinIndex);
        while(leafBinIndex != OA_NO_SPACE)
        {
            oa_uint32 binIndex = (topBinIndex << OA_TOP_BINS_INDEX_SHIFT) | leafBinIndex;
            oa_uint32 nodeIndex = allocator->bin_indices[binIndex];
            while(nodeIndex != OA_NODE_UNUSED)
            {
                OA_Node* node = &allocator->nodes[nodeIndex];
                oa_uint32 nodeStart = node->data_offset;
                oa_uint32 nodeEnd = node->data_offset + node->data_size;
                oa_uint32 aligned = oa_align_up(nodeStart, alignment);

                if(aligned + size <= nodeEnd)
                {
                    oa_uint32 lead = aligned - nodeStart;
                    oa_uint32 trail = nodeEnd - (aligned + size);
                    oa_uint32 neededNodes = (lead > 0 ? 1u : 0u) + (trail > 0 ? 1u : 0u);
                    oa_uint32 availableNodes = allocator->free_offset + 1u;

                    if(neededNodes <= availableNodes)
                    {
                        selectedNodeIndex = nodeIndex;
                        alignedOffset = aligned;
                        leadingSize = lead;
                        trailingSize = trail;
                        goto oa_aligned_found;
                    }
                }

                nodeIndex = node->bin_list_next;
            }

            leafBinIndex = oa_find_lowest_set_bit_after(leafMask, leafBinIndex + 1u);
        }

        minLeafBinIndex = 0;
    }

    return (OA_Allocation){ .offset = OA_NO_SPACE, .metadata = OA_NODE_UNUSED };

oa_aligned_found:
    {
        OA_Node* node = &allocator->nodes[selectedNodeIndex];
        oa_uint32 nodeTotalSize = node->data_size;

        oa_unlink_node_from_bin(allocator, selectedNodeIndex);
        allocator->free_storage -= nodeTotalSize;

        OA_NodeIndex neighborPrev = node->neighbor_prev;
        OA_NodeIndex neighborNext = node->neighbor_next;

        node->data_offset = alignedOffset;
        node->data_size = size;
        node->used = true;

        if(leadingSize > 0)
        {
            oa_uint32 leadingNodeIndex = oa_insert_node_into_bin(allocator, leadingSize, alignedOffset - leadingSize);
            allocator->nodes[leadingNodeIndex].neighbor_prev = neighborPrev;
            allocator->nodes[leadingNodeIndex].neighbor_next = (OA_NodeIndex)selectedNodeIndex;
            node->neighbor_prev = (OA_NodeIndex)leadingNodeIndex;

            if(neighborPrev != OA_NODE_UNUSED)
                allocator->nodes[neighborPrev].neighbor_next = (OA_NodeIndex)leadingNodeIndex;
        }
        else
        {
            node->neighbor_prev = neighborPrev;
        }

        if(trailingSize > 0)
        {
            oa_uint32 trailingNodeIndex = oa_insert_node_into_bin(allocator, trailingSize, alignedOffset + size);
            allocator->nodes[trailingNodeIndex].neighbor_prev = (OA_NodeIndex)selectedNodeIndex;
            allocator->nodes[trailingNodeIndex].neighbor_next = neighborNext;
            node->neighbor_next = (OA_NodeIndex)trailingNodeIndex;

            if(neighborNext != OA_NODE_UNUSED)
                allocator->nodes[neighborNext].neighbor_prev = (OA_NodeIndex)trailingNodeIndex;
        }
        else
        {
            node->neighbor_next = neighborNext;
        }

        oa_debug_validate(allocator);
        return (OA_Allocation){ .offset = node->data_offset, .metadata = (OA_NodeIndex)selectedNodeIndex };
    }
}

void oa_free(OA_Allocator* allocator, OA_Allocation allocation)
{
    if(!allocator || allocation.metadata == OA_NODE_UNUSED)
        return;

    if(!allocator->nodes)
        return;

    oa_uint32 nodeIndex = allocation.metadata;
    OA_Node* node = &allocator->nodes[nodeIndex];

    OA_ASSERT(node->used == true);

    oa_uint32 offset = node->data_offset;
    oa_uint32 size = node->data_size;

    if(node->neighbor_prev != OA_NODE_UNUSED && allocator->nodes[node->neighbor_prev].used == false)
    {
        OA_Node* prevNode = &allocator->nodes[node->neighbor_prev];
        offset = prevNode->data_offset;
        size += prevNode->data_size;

        oa_remove_node_from_bin(allocator, node->neighbor_prev);
        OA_ASSERT(prevNode->neighbor_next == nodeIndex);
        node->neighbor_prev = prevNode->neighbor_prev;
    }

    if(node->neighbor_next != OA_NODE_UNUSED && allocator->nodes[node->neighbor_next].used == false)
    {
        OA_Node* nextNode = &allocator->nodes[node->neighbor_next];
        size += nextNode->data_size;

        oa_remove_node_from_bin(allocator, node->neighbor_next);
        OA_ASSERT(nextNode->neighbor_prev == nodeIndex);
        node->neighbor_next = nextNode->neighbor_next;
    }

    oa_uint32 neighborNext = node->neighbor_next;
    oa_uint32 neighborPrev = node->neighbor_prev;

#ifdef OA_DEBUG_VERBOSE
    printf("Putting node %u into freelist[%u] (free)\n", nodeIndex, allocator->free_offset + 1u);
#endif
    allocator->free_nodes[++allocator->free_offset] = (OA_NodeIndex)nodeIndex;

    oa_uint32 combinedNodeIndex = oa_insert_node_into_bin(allocator, size, offset);

    if(neighborNext != OA_NODE_UNUSED)
    {
        allocator->nodes[combinedNodeIndex].neighbor_next = (OA_NodeIndex)neighborNext;
        allocator->nodes[neighborNext].neighbor_prev = (OA_NodeIndex)combinedNodeIndex;
    }
    if(neighborPrev != OA_NODE_UNUSED)
    {
        allocator->nodes[combinedNodeIndex].neighbor_prev = (OA_NodeIndex)neighborPrev;
        allocator->nodes[neighborPrev].neighbor_next = (OA_NodeIndex)combinedNodeIndex;
    }

    oa_debug_validate(allocator);
}

oa_uint32 oa_allocation_size(const OA_Allocator* allocator, OA_Allocation allocation)
{
    if(!allocator || allocation.metadata == OA_NODE_UNUSED)
        return 0;
    if(!allocator->nodes)
        return 0;
    return allocator->nodes[allocation.metadata].data_size;
}

OA_StorageReport oa_storage_report(const OA_Allocator* allocator)
{
    oa_uint32 largestFreeRegion = 0;
    oa_uint32 freeStorage = 0;

    if(allocator && allocator->free_offset > 0)
    {
        freeStorage = allocator->free_storage;
        if(allocator->used_bins_top)
        {
            oa_uint32 topBinIndex = 31u - oa_lzcnt_nonzero(allocator->used_bins_top);
            oa_uint32 leafBinIndex = 31u - oa_lzcnt_nonzero(allocator->used_bins[topBinIndex]);
            largestFreeRegion = oa_float_to_uint((topBinIndex << OA_TOP_BINS_INDEX_SHIFT) | leafBinIndex);
            OA_ASSERT(freeStorage >= largestFreeRegion);
        }
    }

    return (OA_StorageReport){ .total_free_space = freeStorage, .largest_free_region = largestFreeRegion };
}

OA_StorageReportFull oa_storage_report_full(const OA_Allocator* allocator)
{
    OA_StorageReportFull report;
    if(!allocator)
    {
        memset(&report, 0, sizeof(report));
        return report;
    }

    for(oa_uint32 i = 0; i < OA_NUM_LEAF_BINS; i++)
    {
        oa_uint32 count = 0;
        oa_uint32 nodeIndex = allocator->bin_indices[i];
        while(nodeIndex != OA_NODE_UNUSED)
        {
            nodeIndex = allocator->nodes[nodeIndex].bin_list_next;
            count++;
        }
        report.free_regions[i].size = oa_float_to_uint(i);
        report.free_regions[i].count = count;
    }
    return report;
}

#ifdef DEBUG
void oa_debug_validate(const OA_Allocator* allocator)
{
    if(!allocator || !allocator->nodes)
        return;

    oa_uint32 computedFreeStorage = 0;
    oa_uint32 computedUsedBinsTop = 0;
    oa_uint8 computedUsedBins[OA_NUM_TOP_BINS];
    memset(computedUsedBins, 0, sizeof(computedUsedBins));

    for(oa_uint32 binIndex = 0; binIndex < OA_NUM_LEAF_BINS; binIndex++)
    {
        oa_uint32 topBinIndex = binIndex >> OA_TOP_BINS_INDEX_SHIFT;
        oa_uint32 leafBinIndex = binIndex & OA_LEAF_BINS_INDEX_MASK;

        oa_uint32 nodeIndex = allocator->bin_indices[binIndex];
        if(nodeIndex != OA_NODE_UNUSED)
            computedUsedBins[topBinIndex] |= (1u << leafBinIndex);

        while(nodeIndex != OA_NODE_UNUSED)
        {
            OA_Node* node = &allocator->nodes[nodeIndex];

            OA_ASSERT(node->used == false);
            OA_ASSERT(oa_uint_to_float_round_down(node->data_size) == binIndex);

            if(node->bin_list_prev != OA_NODE_UNUSED)
                OA_ASSERT(allocator->nodes[node->bin_list_prev].bin_list_next == nodeIndex);
            if(node->bin_list_next != OA_NODE_UNUSED)
                OA_ASSERT(allocator->nodes[node->bin_list_next].bin_list_prev == nodeIndex);

            computedFreeStorage += node->data_size;
            nodeIndex = node->bin_list_next;
        }
    }

    for(oa_uint32 topBinIndex = 0; topBinIndex < OA_NUM_TOP_BINS; topBinIndex++)
    {
        if(computedUsedBins[topBinIndex] != 0)
            computedUsedBinsTop |= (1u << topBinIndex);
    }

    OA_ASSERT(computedFreeStorage == allocator->free_storage);
    OA_ASSERT(computedUsedBinsTop == allocator->used_bins_top);
    for(oa_uint32 i = 0; i < OA_NUM_TOP_BINS; i++)
        OA_ASSERT(computedUsedBins[i] == allocator->used_bins[i]);
}
#endif
