#ifndef INDEXSERIALIZER_H__
#define INDEXSERIALIZER_H__

#include "PostingStore.h"
#include "BlockTable.h"
#include <cstdint>
#include <vector>
#include <string>

/*
 * Binary index file format (version 9).
 *
 * Layout:
 *   [Header 96B]       magic, version, all section offsets
 *   [Head/Leaf Table]  Two-level structure:
 *                        Level-1 head table: [entry_count:4] then per entry:
 *                          [key_len:2][HTE_FirstTerm:key_len][HTE_LeafTermBlockID:4]
 *                        Level-2 leaf blocks: [leaf_page_count:4] then N x 4096B pages:
 *                          each page: [entry_count:4] then per entry:
 *                          [key_len:2][LTE_Term:key_len][LTE_DocFreq:4]
 *                          [LTE_IndexBlockID:4][LTE_IndexOffset:4][LTE_IndexLength:4]
 *                          [LTE_PageSkipOffset:4][LTE_ContinuationBlockCount:4][LTE_Flags:4]
 *   [PageSkipList]     flat uint64_t arrays — per-term base_doc_id per cont. block
 *   [DocData]          N × 1024B records
 *   [Padding]          to PAGE_SIZE
 *   [Blocks]           raw IndexBlock structs (IB_Data = packed posting bytes)
 */

struct BuildBlocksResult {
    std::vector<IndexBlock>          BBR_IndexBlocks;
    std::vector<HeadTermEntry>       BBR_HeadTermEntries;
    std::vector<LeafTermPage>        BBR_LeafTermPages;
    std::vector<uint64_t>            BBR_PageSkipList;
    uint64_t                         BBR_TotalTerms = 0;
};

class IndexSerializer {
public:
    using BlockResult = BuildBlocksResult;

    static bool Save(const PostingStore& store, const char* path);

    static bool Load(PostingStore&                           store,
                     const char*                            path,
                     std::vector<HeadTermEntry>*             headTermEntriesOut,
                     std::vector<LeafTermPage>*              leafTermPagesOut,
                     uint64_t*                              blocks_offset_out,
                     std::vector<uint64_t>*                 pageskip_out = nullptr,
                     uint64_t*                              num_blocks_out = nullptr,
                     uint64_t*                              leaf_blocks_offset_out = nullptr,
                     uint64_t*                              num_leaf_blocks_out = nullptr,
                     uint64_t*                              docdata_offset_out = nullptr,
                     uint64_t*                              docdata_size_out = nullptr,
                     uint64_t*                              num_documents_out = nullptr,
                     IndexFileHeader*                       header_out = nullptr);

    static bool LoadLayout(const char* path, IndexLayoutInfo& out);

    static bool ReadSections(const char* path,
                             uint64_t docdata_offset,
                             uint8_t* docdata_out,
                             uint64_t docdata_bytes,
                             uint64_t leaf_blocks_offset,
                             uint8_t* leaf_blocks_out,
                             uint64_t leaf_blocks_bytes,
                             uint64_t blocks_offset,
                             uint8_t* blocks_out,
                             uint64_t blocks_bytes);

