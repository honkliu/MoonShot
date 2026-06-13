#ifndef INDEXSERIALIZER_H__
#define INDEXSERIALIZER_H__

#include "PostingStore.h"
#include "BlockTable.h"
#include <cstdint>
#include <vector>
#include <string>

/*
 * Binary index file format (version 3).
 *
 * Layout:
 *   [Header 96B]       magic, version, all section offsets
 *   [SubIndex]         (key_len, key, block_seq, block_entry_start, page_skip_offset) per entry
 *   [PageSkipList]     flat uint64_t arrays — per-term base_doc_id per cont. block
 *   [DocData]          N × 16 B — doc_id, importance, doc_len
 *   [Padding]          to PAGE_SIZE
 *   [Blocks]           raw IndexBlock structs
 */

struct BuildBlocksResult {
    std::vector<IndexBlock>    blocks;
    std::vector<SubIndexEntry> subindex;
    std::vector<uint64_t>      pageskip;
};

class IndexSerializer {
public:
    /* Internal type alias exposed for IndexContext::Build() */
    using BlockResult = BuildBlocksResult;

    static bool Save(const PostingStore& store, const char* path);

    static bool Load(PostingStore&              store,
                     const char*                path,
                     std::vector<SubIndexEntry>* subindex_out,
                     uint64_t*                   blocks_offset_out,
                     std::vector<uint64_t>*      pageskip_out = nullptr);

    static bool IsValidIndex(const char* path);

    /* Used by IndexContext::Build() to pack blocks without writing to disk. */
    static BlockResult BuildBlocksForContext(const PostingStore& store);
};

#endif
