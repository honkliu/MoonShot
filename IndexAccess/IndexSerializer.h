#ifndef INDEXSERIALIZER_H__
#define INDEXSERIALIZER_H__

#include "PostingStore.h"
#include "BlockTable.h"
#include <cstdint>
#include <vector>
#include <string>

/*
 * Binary index file format (version 12).
 *
 * Layout:
 *   [Header 88B]       magic, version, fixed section offsets and counts
 *   [HeadTermEntry]    fixed 32B records, count = IFH_HeadTermEntryCount
 *   [LeafTermBlock]    fixed 4096B blocks, count = IFH_LeafTermBlockCount
 *                          LTB_Directory[0..94]: LeafTermEntry offsets from block base
 *                          LTB_Directory[95]: entry count
 *                          LTB_Data: packed LeafTermEntry records + LTE_Term bytes
 *   [DocData]          N x 1024B records
 *   [Blocks]           raw IndexBlock structs
 *                        first block: packed varbyte docID/tf pairs
 *                        continuation block: 12B IndexBlockContinuationHeader + pairs
 */

struct BuildBlocksResult {
    std::vector<IndexBlock>          BBR_IndexBlocks;
    std::vector<HeadTermEntry>       BBR_HeadTermEntries;
    std::vector<LeafTermBlock>       BBR_LeafTermBlocks;
    uint64_t                         BBR_TotalTerms = 0;
};

class IndexSerializer {
public:
    using BlockResult = BuildBlocksResult;

    static bool Save(const IndexFileHeader& header,
                     const IndexBlockTable& blockTable,
                     const uint8_t* docData,
                     const char* path);
    static bool IsValidIndex(const char* path);
    static BuildBlocksResult BuildBlocks(const PostingStore& store);

};

#endif

