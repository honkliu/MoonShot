#ifndef INDEXSERIALIZER_H__
#define INDEXSERIALIZER_H__

#include "PostingStore.h"
#include "BlockTable.h"
#include <cstdint>
#include <vector>
#include <string>

/*
 * Binary index file format (version 20).
 *
 * Layout:
 *   [Header]           magic, version, fixed section offsets and counts
 *   [PathPrefix]       fixed 10-page sidecar, directory-prefix string table
 *   [HeadTermEntry]    fixed 32B records, count = IFH_HeadTermEntryCount
 *   [LeafTermBlock]    fixed 4096B blocks, count = IFH_LeafTermBlockCount
 *                          LTB_Directory[0..94]: LeafTermEntry offsets from block base
 *                          LTB_Directory[95]: entry count
 *                          LTB_Data: packed 32B LeafTermEntry records + LTE_Term bytes
 *   [DocData]          N x 256B records
 *   [Blocks]           raw IndexBlock structs
 *                        first block: packed docID-varbyte + scale16 uint8 tf pairs
 *                        continuation block: 12B IndexBlockContinuationHeader + pairs
 *   [TermMphfHeader]   one fixed MPHF descriptor
 *   [MPHF Disp]        int32 displacement per MPHF bucket
 *   [MPHF Entries]     4096B pages of fixed 32B TermMphfEntry records
 */

struct BuildBlocksResult {
    std::vector<IndexBlock>          BBR_IndexBlocks;
    std::vector<HeadTermEntry>       BBR_HeadTermEntries;
    std::vector<LeafTermBlock>       BBR_LeafTermBlocks;
    TermMphfHeader                   BBR_TermMphfHeader;
    std::vector<int32_t>             BBR_TermMphfDisplacements;
    std::vector<IndexBlock>          BBR_TermMphfEntryPages;
    uint64_t                         BBR_TotalTerms = 0;
};

class IndexSerializer {
public:
    using BlockResult = BuildBlocksResult;

    static bool Save(const IndexFileHeader& header,
                     const IndexBlockTable& blockTable,
                     const uint8_t* docData,
                     const uint8_t* pathPrefixSidecar,
                     const char* path);
    static bool IsValidIndex(const char* path);
    static BuildBlocksResult BuildBlocks(const PostingStore& store);
    static void BuildTermMphfFromLeafBlocks(const LeafTermBlock* leafBlocks,
                                            uint32_t leafBlockCount,
                                            BuildBlocksResult& result);

};

#endif

