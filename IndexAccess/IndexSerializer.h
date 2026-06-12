#ifndef INDEXSERIALIZER_H__
#define INDEXSERIALIZER_H__

#include "PostingStore.h"
#include "BlockTable.h"
#include <cstdint>
#include <vector>
#include <string>

/*
 * Binary index file format (version 2).
 *
 * Mirrors Tiger's file layout: every section after the header is stored
 * as fixed-size pages so FileBlockManager can do direct random I/O.
 *
 * Layout:
 *   [FileHeader]  64 bytes   — magic, version, section offsets
 *   [SubIndex]    variable   — packed (key_len, key, block_seq) entries,
 *                              sorted by term (one entry per block)
 *   [DocData]     N × 16 B  — per-document: doc_id, importance, doc_len
 *   [Padding]     0..4095 B — zero-fill to align Blocks to PAGE_SIZE
 *   [Blocks]      M × 4096 B — raw IndexBlock structs; block_seq 0 is at
 *                              blocks_offset in the file
 */
class IndexSerializer {
public:
    /*
     * Save the index to path.
     * Packs terms alphabetically into IndexBlocks (multiple terms per block),
     * writes the SubIndex, DocData, and Blocks sections.
     */
    static bool Save(const PostingStore& store, const char* path);

    /*
     * Load an index from path.
     * Restores DocData into store; returns SubIndex and blocks_offset so
     * IndexContext::LoadIndex can wire up the BlockTable and FileBlockManager.
     */
    static bool Load(PostingStore&              store,
                     const char*                path,
                     std::vector<SubIndexEntry>* subindex_out,
                     uint64_t*                   blocks_offset_out);

    /*
     * Quick magic-byte check.
     */
    static bool IsValidIndex(const char* path);
};

#endif
