/*
 * IndexSerializer — v12 paged Head/Leaf term table format.
 *
 * Block IB_Data: raw posting bytes only (no key/doc_freq/data_len headers).
 * Head/Leaf term table: two-level term metadata structure.
 *   Level-1 head table: fixed sorted HeadTermEntry array.
 *   Level-2 leaf blocks: 4096-byte pages with directory offsets and packed entries.
 * A term's continuation segment starts at byte 0 of the next block and carries
 * IndexBlockContinuationHeader; the final continuation block can reuse its tail
 * for later term starts.
 */

#include "IndexSerializer.h"
#include "BlockTable.h"
#include "FileAccess.h"

#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <cassert>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static bool read_vbc_pair_end(const uint8_t* data, size_t size, size_t& offset)
{
    auto read_one = [&]() -> bool {
        while (offset < size) {
            const uint8_t byte = data[offset++];
            if ((byte & 0x80u) == 0)
                return true;
        }
        return false;
    };
    return read_one() && read_one();
}

static size_t posting_prefix_bytes(const uint8_t* data, size_t size, size_t capacity)
{
    size_t cursor = 0;
    size_t lastPairEnd = 0;
    const size_t limit = std::min(size, capacity);
    while (cursor < limit) {
        const size_t before = cursor;
        if (!read_vbc_pair_end(data, size, cursor) || cursor > limit) {
            cursor = before;
            break;
        }
        lastPairEnd = cursor;
    }
    return lastPairEnd;
}

/* ─────────────────────────────────────────────────────────────────────────
 * build_blocks
 *
 * 1. Pack posting bytes into IndexBlocks.
 *    First block: raw posting bytes at LTE_IndexOffset/LTE_IndexLength.
 *    Continuation segment: [IndexBlockContinuationHeader][posting bytes] at page start;
 *    the last continuation block may reuse remaining bytes for following terms.
 * 2. Build a flat sorted LeafTermEntry list (one LeafTermEntry per term).
 * 3. Organize into two-level Head/Leaf term table:
 *    HeadTermEntry: one entry per 4096-byte LeafTermBlock -> first term in block.
 *    LeafTermBlocks: variable entry count, never crossing 4096-byte block boundary.
 * ─────────────────────────────────────────────────────────────────────────*/
BuildBlocksResult IndexSerializer::BuildBlocks(const PostingStore& store)
{
    constexpr size_t   DATA_CAP = sizeof(IndexBlock::IB_Data);

    /* Sort terms alphabetically */
    std::vector<std::pair<const std::string*, const PostingList*>> terms;
    terms.reserve(store.AllPostings().size());
    for (const auto& [k, pl] : store.AllPostings())
        terms.push_back({&k, &pl});
    std::sort(terms.begin(), terms.end(),
              [](auto& a, auto& b){ return *a.first < *b.first; });

    BuildBlocksResult res;
    IndexBlock cur   = {};
    size_t     wptr  = 0;
    uint32_t   seq   = 0;

    LeafTermBlock leafBlock{};
    size_t leafWriteOffset = 0;
    uint32_t leafEntryCount = 0;
    char firstLeafTerm[HEAD_TERM_KEY_MAX] = {};
    uint16_t firstLeafTermLength = 0;

    auto flush_leaf_block = [&]() {
        if (leafEntryCount == 0) return;
        leafBlock.LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] = static_cast<uint16_t>(leafEntryCount);
        HeadTermEntry head{};
        head.HTE_LeafTermBlockID = static_cast<uint32_t>(res.BBR_LeafTermBlocks.size());
        head.HTE_FirstTermLength = firstLeafTermLength;
        std::memcpy(head.HTE_FirstTerm, firstLeafTerm, head.HTE_FirstTermLength);
        res.BBR_HeadTermEntries.push_back(head);
        res.BBR_LeafTermBlocks.push_back(leafBlock);
        leafBlock = LeafTermBlock{};
        leafWriteOffset = 0;
        leafEntryCount = 0;
        firstLeafTermLength = 0;
        std::memset(firstLeafTerm, 0, sizeof(firstLeafTerm));
    };

    auto write_leaf_entry = [&](const std::string& term,
                                uint32_t docFreq,
                                uint32_t indexBlockID,
                                uint32_t indexOffset,
                                uint32_t indexLength,
                                uint32_t continuationBlockCount,
                                uint32_t flags) {
        const uint8_t termLength = static_cast<uint8_t>(term.size());
        const size_t entryBytes = sizeof(LeafTermEntry) + termLength;
        if (leafEntryCount > 0
            && (leafEntryCount >= LEAF_TERM_DIRECTORY_COUNT - 1
                || leafWriteOffset + entryBytes > sizeof(leafBlock.LTB_Data)))
            flush_leaf_block();

        if (leafEntryCount == 0) {
            firstLeafTermLength = termLength;
            std::memcpy(firstLeafTerm, term.data(), termLength);
        }

        leafBlock.LTB_Directory[leafEntryCount] = static_cast<uint16_t>(LEAF_TERM_DATA_OFFSET + leafWriteOffset);
        LeafTermEntry* entry = reinterpret_cast<LeafTermEntry*>(leafBlock.LTB_Data + leafWriteOffset);
        entry->LTE_DocFreq = docFreq;
        entry->LTE_IndexBlockID = indexBlockID;
        entry->LTE_IndexOffset = indexOffset;
        entry->LTE_IndexLength = indexLength;
        entry->LTE_ContinuationBlockCount = continuationBlockCount;
        entry->LTE_Flags = flags;
        entry->LTE_TermLength = termLength;
        std::memcpy(entry->LTE_Term, term.data(), termLength);
        leafWriteOffset += entryBytes;
        ++leafEntryCount;
        ++res.BBR_TotalTerms;
    };

    auto flush = [&]() {
        res.BBR_IndexBlocks.push_back(cur);
        ++seq; cur = {}; wptr = 0;
    };

    for (const auto& [key_ptr, pl] : terms) {
        const std::string& key   = *key_ptr;
        if (key.size() > HEAD_TERM_KEY_MAX)
            continue;
        const auto         bytes = pl->GetBytes();
        if (bytes.empty()) continue;

        uint32_t       doc_freq  = pl->doc_freq();
        const uint8_t* src       = bytes.data();
        size_t         remaining = bytes.size();

        if (wptr >= DATA_CAP) flush();

        size_t data_offset = wptr;
        size_t data_here   = posting_prefix_bytes(src, remaining, DATA_CAP - wptr);
        if (data_here == 0) {
            flush();
            data_offset = wptr;
            data_here = posting_prefix_bytes(src, remaining, DATA_CAP);
            if (data_here == 0) continue;
        }
        bool   has_more    = (data_here < remaining);

        const uint32_t indexBlockID = seq;
        std::memcpy(cur.IB_Data + wptr, src, data_here);
        wptr      += data_here;
        src       += data_here;
        remaining -= data_here;
        uint32_t continuationBlockCount = 0;

        if (has_more) {
            flush();

            while (remaining > 0) {
                constexpr size_t CONT_HDR = sizeof(IndexBlockContinuationHeader);
                size_t cont_cap  = DATA_CAP - CONT_HDR;
                size_t cont_here = posting_prefix_bytes(src, remaining, cont_cap);
                if (cont_here == 0) break;
                bool   more_cont = (cont_here < remaining);
                size_t cont_cursor = 0;
                uint64_t cont_max_docid = 0;
                while (cont_cursor < cont_here) {
                    cont_max_docid = 0;
                    uint8_t shift = 0;
                    while (true) {
                        const uint8_t byte = src[cont_cursor++];
                        cont_max_docid |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
                        if ((byte & 0x80u) == 0) break;
                        shift += 7;
                    }
                    while (src[cont_cursor++] & 0x80u) {}
                }

                auto* header = reinterpret_cast<IndexBlockContinuationHeader*>(cur.IB_Data + wptr);
                header->IBCH_MaxDocID = cont_max_docid;
                header->IBCH_DataLength = static_cast<uint32_t>(cont_here);
                wptr += sizeof(IndexBlockContinuationHeader);
                std::memcpy(cur.IB_Data + wptr, src, cont_here); wptr += cont_here;
                src       += cont_here;
                remaining -= cont_here;

                ++continuationBlockCount;

                if (more_cont) flush();
            }
        }

        write_leaf_entry(key,
                         doc_freq,
                         indexBlockID,
                         static_cast<uint32_t>(data_offset),
                         static_cast<uint32_t>(data_here),
                         continuationBlockCount,
                         0u);
    }

    if (wptr > 0) flush();
    flush_leaf_block();

    return res;
}

/* ── Save ─────────────────────────────────────────────────────────────────*/

bool IndexSerializer::Save(const IndexFileHeader& header,
                           const IndexBlockTable& blockTable,
                           const uint8_t* docData,
                           const char* path)
{
    if (!path || !*path) return false;

    FileAccess file(path);
    if (!file.InitWrite()) return false;

    if (!file.PutData(&header, sizeof(header))) return false;

    if (header.IFH_HeadTermEntryCount > 0) {
        const uint64_t bytes = sizeof(HeadTermEntry) * header.IFH_HeadTermEntryCount;
        if (!file.PutData(blockTable.m_HeadTermEntries.get(), bytes)) return false;
    }

    if (header.IFH_LeafTermBlockCount > 0) {
        const uint64_t bytes = sizeof(LeafTermBlock) * header.IFH_LeafTermBlockCount;
        if (!file.PutData(blockTable.m_LeafTermPool.BCP_Pages, bytes)) return false;
    }

    if (header.IFH_NumDocuments > 0) {
        const uint64_t bytes = DOC_REC_SIZE * header.IFH_NumDocuments;
        if (!file.PutData(docData, bytes)) return false;
    }

    if (header.IFH_IndexBlockCount > 0) {
        const uint64_t bytes = sizeof(IndexBlock) * header.IFH_IndexBlockCount;
        if (!file.PutData(blockTable.m_IndexPool.BCP_Pages, bytes)) return false;
    }

    return true;
}

bool IndexSerializer::IsValidIndex(const char* path)
{
    if (!path || !*path) return false;
    FileAccess file(path);
    if (!file.Init()) return false;
    uint8_t magic[8] = {};
    if (file.GetData(magic, sizeof(magic)) != sizeof(magic)) return false;
    return std::memcmp(magic, INDEX_FILE_MAGIC, 8) == 0;
}


