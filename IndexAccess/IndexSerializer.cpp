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

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <cassert>
#include <memory>

/* ── helpers ─────────────────────────────────────────────────────────────── */

using FilePtr = std::unique_ptr<FILE, decltype(&std::fclose)>;

static FilePtr open_file(const char* path, const char* mode)
{
    return FilePtr(std::fopen(path, mode), &std::fclose);
}

static bool seek_file(FILE* file, uint64_t offset)
{
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
#endif
}

static uint64_t file_pos(FILE* file)
{
#if defined(_WIN32)
    return static_cast<uint64_t>(_ftelli64(file));
#else
    return static_cast<uint64_t>(std::ftell(file));
#endif
}

static uint64_t page_aligned_bytes(uint64_t bytes)
{
    return ((bytes + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
}

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
static BuildBlocksResult build_blocks(const PostingStore& store)
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
        const auto&        bytes = pl->GetBytes();
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

bool IndexSerializer::Save(const PostingStore& store, const char* path)
{
    if (!path || !*path) return false;

    BuildBlocksResult br = build_blocks(store);

    /* ── Encode Head/Leaf term table (paged two-level) ────────────────────
     *
     * Format:
    *   [HeadTermEntry x IFH_HeadTermEntryCount]  // fixed 32B
    *   [LeafTermBlock x IFH_LeafTermBlockCount]  // fixed 4096B
     */
    const uint64_t headTermEntryCount = br.BBR_HeadTermEntries.size();
    const uint64_t leafTermBlockCount = br.BBR_LeafTermBlocks.size();

    /* ── Encode DocData ───────────────────────────────────────────────────*/
    std::vector<DocDataEntry> docdata;
    uint64_t maxDocID = 0;
    for (const auto& [id, _] : store.AllDocStats())
        maxDocID = std::max(maxDocID, id);
    if (!store.AllDocStats().empty())
        docdata.resize(static_cast<size_t>(maxDocID + 1));

    for (const auto& [id, ds] : store.AllDocStats()) {
        DocDataEntry entry{};
        entry.DDE_DocID     = id;
        entry.DDE_DocLength = ds.doc_len;
        entry.DDE_StaticRank = ds.importance;
        entry.DDE_QualityScore = 0.0f;
        entry.DDE_FreshnessScore = 0.0f;
        entry.DDE_ClickScore = 0.0f;
        entry.DDE_EngagementScore = 0.0f;
        // Doc vectors are stored in the in-memory vector index, not in DocStats.
        // DDE_VectorData is int8_t[512], quantized from float32.
        if (const auto* docVector = store.GetDocVector(id);
            docVector && !docVector->empty() && docVector->size() <= DOC_VECTOR_STORAGE_MAX_DIM) {
            entry.DDE_VectorDim = static_cast<uint16_t>(docVector->size());
            entry.DDE_VectorFormat = 1;  // int8 quantized
            for (size_t i = 0; i < docVector->size(); ++i) {
                // Quantize float32 to int8: clamp to [-128, 127]
                const float val = (*docVector)[i];
                const float clipped = std::max(-128.0f, std::min(127.0f, val * 128.0f));
                entry.DDE_VectorData[i] = static_cast<int8_t>(std::round(clipped));
            }
        }
        entry.DDE_PathLength = static_cast<uint16_t>(std::min(ds.path.size(), DOC_PATH_MAX));
        if (entry.DDE_PathLength > 0)
            std::memcpy(entry.DDE_Path, ds.path.data(), entry.DDE_PathLength);
        docdata[static_cast<size_t>(id)] = entry;
    }

    /* ── Compute file offsets ─────────────────────────────────────────────*/
    const uint64_t HDR_SIZE  = sizeof(IndexFileHeader);
    uint64_t head_off        = HDR_SIZE;
    uint64_t leaf_off        = head_off + headTermEntryCount * sizeof(HeadTermEntry);
    uint64_t dd_off          = leaf_off + leafTermBlockCount * sizeof(LeafTermBlock);
    uint64_t dd_size         = docdata.size() * DOC_REC_SIZE;
    uint64_t raw_blk_off     = dd_off + dd_size;
    uint64_t blk_off         = page_aligned_bytes(raw_blk_off);

    FilePtr file = open_file(path, "wb");
    FILE* f = file.get();
    if (!f) return false;

    IndexFileHeader hdr{};
    std::memcpy(hdr.IFH_Magic, INDEX_FILE_MAGIC, 8);
    hdr.IFH_Version       = INDEX_FORMAT_VERSION;
    hdr.IFH_AvgDocLength  = store.AvgDocLen();
    hdr.IFH_NumDocuments = static_cast<uint64_t>(docdata.size());
    hdr.IFH_NumTerms     = br.BBR_TotalTerms;
    hdr.IFH_HeadTermEntryOffset = head_off; hdr.IFH_HeadTermEntryCount = headTermEntryCount;
    hdr.IFH_LeafTermBlockOffset = leaf_off; hdr.IFH_LeafTermBlockCount = leafTermBlockCount;
    hdr.IFH_DocDataOffset = dd_off;
    hdr.IFH_IndexBlockOffset = blk_off; hdr.IFH_IndexBlockCount = br.BBR_IndexBlocks.size();
    fwrite(&hdr, sizeof(hdr), 1, f);

    if (!br.BBR_HeadTermEntries.empty())
        fwrite(br.BBR_HeadTermEntries.data(), sizeof(HeadTermEntry), br.BBR_HeadTermEntries.size(), f);
    if (!br.BBR_LeafTermBlocks.empty())
        fwrite(br.BBR_LeafTermBlocks.data(), sizeof(LeafTermBlock), br.BBR_LeafTermBlocks.size(), f);
    if (!docdata.empty())
        fwrite(docdata.data(), DOC_REC_SIZE, docdata.size(), f);

    uint64_t cur_pos = file_pos(f);
    if (cur_pos < blk_off) {
        std::vector<uint8_t> pad(static_cast<size_t>(blk_off - cur_pos), 0);
        fwrite(pad.data(), 1, pad.size(), f);
    }
    for (const auto& b : br.BBR_IndexBlocks)
        fwrite(&b, sizeof(IndexBlock), 1, f);

    return true;
}

/* ── Load ─────────────────────────────────────────────────────────────────*/

bool IndexSerializer::Load(PostingStore&                           store,
                           const char*                            path,
                           std::vector<HeadTermEntry>*             headTermEntriesOut,
                           std::vector<LeafTermBlock>*             leafTermBlocksOut,
                           uint64_t*                              blocks_offset_out,
                           uint64_t*                              num_blocks_out,
                           uint64_t*                              leaf_blocks_offset_out,
                           uint64_t*                              num_leaf_blocks_out,
                           uint64_t*                              docdata_offset_out,
                           uint64_t*                              docdata_size_out,
                           uint64_t*                              num_documents_out,
                           IndexFileHeader*                       header_out)
{
    if (!path || !*path) return false;
    FilePtr file = open_file(path, "rb");
    FILE* f = file.get();
    if (!f) return false;

    IndexFileHeader local_header{};
    IndexFileHeader* hdr = header_out ? header_out : &local_header;
    if (fread(hdr, sizeof(*hdr), 1, f) != 1
        || std::memcmp(hdr->IFH_Magic, INDEX_FILE_MAGIC, 8) != 0
        || hdr->IFH_Version != INDEX_FORMAT_VERSION)
    {
        return false;
    }

    /* ── Head/Leaf term table ─────────────────────────────────────────────*/
    if (headTermEntriesOut && hdr->IFH_HeadTermEntryCount > 0) {
        const size_t count = static_cast<size_t>(hdr->IFH_HeadTermEntryCount);
        headTermEntriesOut->resize(count);
        if (!seek_file(f, hdr->IFH_HeadTermEntryOffset))
            return false;
        if (fread(headTermEntriesOut->data(), sizeof(HeadTermEntry), count, f) != count)
            return false;
    }

    if (leaf_blocks_offset_out) *leaf_blocks_offset_out = hdr->IFH_LeafTermBlockOffset;
    if (num_leaf_blocks_out) *num_leaf_blocks_out = hdr->IFH_LeafTermBlockCount;
    if (leafTermBlocksOut && hdr->IFH_LeafTermBlockCount > 0) {
        const size_t count = static_cast<size_t>(hdr->IFH_LeafTermBlockCount);
        leafTermBlocksOut->resize(count);
        if (!seek_file(f, hdr->IFH_LeafTermBlockOffset))
            return false;
        if (fread(leafTermBlocksOut->data(), sizeof(LeafTermBlock), count, f) != count)
            return false;
    }

    if (blocks_offset_out) *blocks_offset_out = hdr->IFH_IndexBlockOffset;
    if (num_blocks_out)    *num_blocks_out    = hdr->IFH_IndexBlockCount;
    if (docdata_offset_out) *docdata_offset_out = hdr->IFH_DocDataOffset;
    if (docdata_size_out)   *docdata_size_out   = hdr->IFH_NumDocuments * DOC_REC_SIZE;
    if (num_documents_out)  *num_documents_out  = hdr->IFH_NumDocuments;

    /* ── DocData ──────────────────────────────────────────────────────────*/
    {
        size_t n = static_cast<size_t>(hdr->IFH_NumDocuments);
        if (!seek_file(f, hdr->IFH_DocDataOffset))
            return false;
        std::vector<DocDataEntry> entries(n);
        if (n > 0) fread(entries.data(), DOC_REC_SIZE, n, f);
        for (size_t row = 0; row < entries.size(); ++row) {
            const auto& entry = entries[row];
            if (entry.DDE_DocID != row)
                continue;
            store.AddDocTokens(entry.DDE_DocID, entry.DDE_DocLength);
            store.SetDocImportance(entry.DDE_DocID, entry.DDE_StaticRank);
            if (entry.DDE_VectorDim > 0 && entry.DDE_VectorDim <= DOC_VECTOR_STORAGE_MAX_DIM && entry.DDE_VectorFormat == 1) {
                std::vector<float> vector(entry.DDE_VectorDim);
                for (size_t i = 0; i < entry.DDE_VectorDim; ++i) {
                    // Dequantize int8 back to float32
                    vector[i] = static_cast<float>(entry.DDE_VectorData[i]) / 128.0f;
                }
                store.SetDocVector(entry.DDE_DocID, std::move(vector));
            }
            if (entry.DDE_PathLength > 0 && entry.DDE_PathLength <= DOC_PATH_MAX)
                store.SetDocPath(entry.DDE_DocID, std::string(reinterpret_cast<const char*>(entry.DDE_Path), entry.DDE_PathLength));
        }
    }

    return true;
}

bool IndexSerializer::IsValidIndex(const char* path)
{
    if (!path || !*path) return false;
    FilePtr file = open_file(path, "rb");
    FILE* f = file.get();
    if (!f) return false;
    uint8_t magic[8] = {};
    fread(magic, 1, 8, f);
    return std::memcmp(magic, INDEX_FILE_MAGIC, 8) == 0;
}

BuildBlocksResult IndexSerializer::BuildBlocksForContext(const PostingStore& store)
{
    return build_blocks(store);
}

