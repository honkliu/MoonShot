#ifndef INDEXSERIALIZER_H__
#define INDEXSERIALIZER_H__

#include "PostingStore.h"
#include "BlockTable.h"
#include <cstdint>
#include <vector>
#include <string>

/*
 * Binary index file format (version 5).
 *
 * Layout:
 *   [Header 96B]       magic, version, all section offsets
 *   [TermHeaderTable]  Two-level structure:
 *                        Level-1 dir:  [dir_count:4] then per entry:
 *                          [key_len:2][first_term:key_len][term_header_block_id:4]
 *                        Level-2 header blocks: [block_count:4] then per block:
 *                          [entry_count:4] then per entry:
 *                          [key_len:2][term:key_len][doc_freq:4]
 *                          [posting_block_id:4][posting_offset:4][posting_length:4]
 *                          [skip_list_offset:4][continuation_block_count:4][flags:4]
 *   [PageSkipList]     flat uint64_t arrays — per-term base_doc_id per cont. block
 *   [DocData]          N × 256B records
 *   [Padding]          to PAGE_SIZE
 *   [Blocks]           raw IndexBlock structs (IB_Data = packed posting bytes)
 */

struct BuildBlocksResult {
    std::vector<IndexBlock>          blocks;
    std::vector<TermDirectoryEntry>  term_directory;
    std::vector<TermHeaderBlock>     term_header_blocks;
    std::vector<uint64_t>            pageskip;
};

class IndexSerializer {
public:
    using BlockResult = BuildBlocksResult;

    static bool Save(const PostingStore& store, const char* path);

    static bool Load(PostingStore&                           store,
                     const char*                            path,
                     std::vector<TermDirectoryEntry>*        dir_out,
                     std::vector<TermHeaderBlock>*           blocks_out,
                     uint64_t*                              blocks_offset_out,
                     std::vector<uint64_t>*                 pageskip_out = nullptr);

    static bool IsValidIndex(const char* path);

    static BlockResult BuildBlocksForContext(const PostingStore& store);
};

#endif
