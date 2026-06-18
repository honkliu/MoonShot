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
 *                          each block: [entry_count:4] then per entry:
 *                          [LeafTermEntry header][LTE_Term bytes]
 *   [DocData]          N x 1024B records
 *   [Padding]          to PAGE_SIZE
 *   [Blocks]           raw IndexBlock structs
 *                        IB_Data = packed varbyte pairs:
 *                        docID, tf, docID, tf, ...
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

    static bool Save(const PostingStore& store, const char* path);
    static bool IsValidIndex(const char* path);
    static BuildBlocksResult BuildBlocksForContext(const PostingStore& store);

    static bool Load(PostingStore&                           store,
                     const char*                            path,
                     std::vector<HeadTermEntry>*             headTermEntriesOut,
                     std::vector<LeafTermBlock>*             leafTermBlocksOut,
                     uint64_t*                              blocks_offset_out,
                     uint64_t*                              num_blocks_out = nullptr,
                     uint64_t*                              leaf_blocks_offset_out = nullptr,
                     uint64_t*                              num_leaf_blocks_out = nullptr,
                     uint64_t*                              docdata_offset_out = nullptr,
                     uint64_t*                              docdata_size_out = nullptr,
                     uint64_t*                              num_documents_out = nullptr,
                     IndexFileHeader*                       header_out = nullptr);

};

#endif

