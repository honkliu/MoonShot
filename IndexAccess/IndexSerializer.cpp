/*
 * IndexSerializer — v8 paged Head/Leaf term table format.
 *
 * Block IB_Data: raw posting bytes only (no key/doc_freq/data_len headers).
 * Head/Leaf term table: two-level term metadata structure.
 *   Level-1 head table: sorted (HTE_FirstTerm, HTE_LeafTermBlockID) pairs.
 *   Level-2 leaf blocks: 4096-byte pages, sorted by term and loaded on demand.
 * Continuation tails can be reused for new term starts; readers use continuation counts.
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

static bool read_varint(const uint8_t* data, size_t length, size_t& offset, uint64_t& value)
{
    value = 0;

    uint32_t shift = 0;
    while (offset < length && shift < 64) {
        const uint8_t byte = data[offset++];
        value |= static_cast<uint64_t>(byte & 0x7fu) << shift;

        if ((byte & 0x80u) == 0)
            return true;

        shift += 7;
    }

    return false;
}

static uint64_t decode_last_docid(const uint8_t* data, size_t length)
{
    size_t offset = 0;
    uint64_t docID = 0;

    while (offset < length) {
        uint64_t docDelta = 0;
        if (!read_varint(data, length, offset, docDelta))
            return docID;

        uint64_t termFrequency = 0;
        if (!read_varint(data, length, offset, termFrequency))
            return docID;

        docID += docDelta;
    }

    return docID;
}

static size_t leaf_entry_bytes(const LeafTermEntry& e)
{
    return 2u + e.LTE_Term.size() + 7u * sizeof(uint32_t);
}

static void encode_leaf_page(const std::vector<LeafTermEntry>& entries, LeafTermPage& page)
{
    page = LeafTermPage{};
    uint8_t* ptr = page.LTP_Data;
    auto write_u16 = [&](uint16_t v) { std::memcpy(ptr, &v, 2); ptr += 2; };
    auto write_u32 = [&](uint32_t v) { std::memcpy(ptr, &v, 4); ptr += 4; };

    write_u32(static_cast<uint32_t>(entries.size()));
    for (const auto& e : entries) {
        write_u16(static_cast<uint16_t>(e.LTE_Term.size()));
        std::memcpy(ptr, e.LTE_Term.data(), e.LTE_Term.size()); ptr += e.LTE_Term.size();
        write_u32(e.LTE_DocFreq);
        write_u32(e.LTE_IndexBlockID);
        write_u32(e.LTE_IndexOffset);
        write_u32(e.LTE_IndexLength);
        write_u32(e.LTE_PageSkipOffset);
        write_u32(e.LTE_ContinuationBlockCount);
        write_u32(e.LTE_Flags);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * build_blocks
 *
 * 1. Pack posting bytes into IndexBlocks (no key/freq/len headers).
 *    Continuation blocks carry [marker,len,bytes] at the current offset; the
 *    final continuation block can also host subsequent term starts.
 * 2. Build a flat sorted LeafTermEntry list (one LeafTermEntry per term).
 * 3. Organize into two-level Head/Leaf term table:
 *    HeadTermEntry: one entry per 4096-byte LeafTermPage -> first term in page.
 *    LeafTermPages: variable entry count, never crossing 4096-byte page boundary.
 * ─────────────────────────────────────────────────────────────────────────*/
static BuildBlocksResult build_blocks(const PostingStore& store)
{
    constexpr size_t   DATA_CAP = sizeof(IndexBlock::IB_Data);
    constexpr uint16_t CONT     = BLOCK_CONTINUATION_MARKER;

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

    /* Flat sorted LeafTermEntry list built during packing, distributed into two levels later. */
    std::vector<LeafTermEntry> flat;
    flat.reserve(terms.size());
    res.BBR_PageSkipList.push_back(UINT64_MAX); // offset 0 means no skip list

    auto flush = [&](bool has_more) {
        cur.IB_Header = static_cast<uint64_t>(seq);
        if (has_more) cur.IB_Header |= IB_HEADER_HAS_MORE;
        res.BBR_IndexBlocks.push_back(cur);
        ++seq; cur = {}; wptr = 0;
    };

    for (const auto& [key_ptr, pl] : terms) {
        const std::string& key   = *key_ptr;
        const auto&        bytes = pl->GetBytes();
        if (bytes.empty()) continue;

        uint32_t       doc_freq  = pl->doc_freq();
        const uint8_t* src       = bytes.data();
        size_t         remaining = bytes.size();

        if (wptr >= DATA_CAP) flush(false);

        size_t data_offset = wptr;
        size_t data_here   = std::min(remaining, DATA_CAP - wptr);
        bool   has_more    = (data_here < remaining);

        std::memcpy(cur.IB_Data + wptr, src, data_here);
        wptr      += data_here;
        src       += data_here;
        remaining -= data_here;

        flat.push_back({key,
            doc_freq,
            seq,
            static_cast<uint32_t>(data_offset),
            static_cast<uint32_t>(data_here),
            0u,
            0u,
            0u});

        if (has_more) {
            flush(true);

            uint32_t skip_offset = static_cast<uint32_t>(res.BBR_PageSkipList.size());
            res.BBR_PageSkipList.push_back(0u);
            flat.back().LTE_PageSkipOffset = skip_offset;

            while (remaining > 0) {
                constexpr size_t CONT_HDR = 4u;
                size_t cont_cap  = DATA_CAP - CONT_HDR;
                size_t cont_here = std::min(remaining, cont_cap);
                bool   more_cont = (cont_here < remaining);

                size_t   bytes_before = bytes.size() - remaining;
                uint64_t base_doc     = decode_last_docid(bytes.data(), bytes_before);
                res.BBR_PageSkipList.push_back(base_doc);

                uint16_t cm = CONT, cl = static_cast<uint16_t>(cont_here);
                std::memcpy(cur.IB_Data + wptr, &cm, 2); wptr += 2;
                std::memcpy(cur.IB_Data + wptr, &cl, 2); wptr += 2;
                std::memcpy(cur.IB_Data + wptr, src, cont_here); wptr += cont_here;
                src       += cont_here;
                remaining -= cont_here;

                ++flat.back().LTE_ContinuationBlockCount;

                if (more_cont) flush(true);
            }
            res.BBR_PageSkipList.push_back(UINT64_MAX);
        }
    }

    if (wptr > 0) flush(false);

    /* ── Build paged Head/Leaf term table ─────────────────────────────────
     * LeafTermPages are physical 4096-byte pages. Entries never cross pages.
     */
    std::vector<LeafTermEntry> pageEntries;
    size_t pageBytes = sizeof(uint32_t); // entry_count
    for (auto& entry : flat) {
        size_t need = leaf_entry_bytes(entry);
        if (!pageEntries.empty() && pageBytes + need > PAGE_SIZE) {
            LeafTermPage page{};
            encode_leaf_page(pageEntries, page);
            res.BBR_HeadTermEntries.push_back({pageEntries.front().LTE_Term,
                static_cast<uint32_t>(res.BBR_LeafTermPages.size())});
            res.BBR_LeafTermPages.push_back(page);
            pageEntries.clear();
            pageBytes = sizeof(uint32_t);
        }

        pageBytes += need;
        pageEntries.push_back(std::move(entry));
    }
    if (!pageEntries.empty()) {
        LeafTermPage page{};
        encode_leaf_page(pageEntries, page);
        res.BBR_HeadTermEntries.push_back({pageEntries.front().LTE_Term,
            static_cast<uint32_t>(res.BBR_LeafTermPages.size())});
        res.BBR_LeafTermPages.push_back(page);
    }

    res.BBR_TotalTerms = flat.size();

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
     *   [dir_count:4]
     *     per dir entry: [key_len:2][HTE_FirstTerm:key_len][HTE_LeafTermBlockID:4]
     *   [leaf_page_count:4]
     *   [LeafTermPage x leaf_page_count]  // each page is exactly 4096 bytes
     */
    std::vector<uint8_t> head_leaf_table_buf;

    auto append = [&](const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        head_leaf_table_buf.insert(head_leaf_table_buf.end(), b, b + n);
    };
    auto append_u16 = [&](uint16_t v) { append(&v, 2); };
    auto append_u32 = [&](uint32_t v) { append(&v, 4); };

    /* Level-1 head table */
    append_u32(static_cast<uint32_t>(br.BBR_HeadTermEntries.size()));
    for (const auto& d : br.BBR_HeadTermEntries) {
        append_u16(static_cast<uint16_t>(d.HTE_FirstTerm.size()));
        append(d.HTE_FirstTerm.data(), d.HTE_FirstTerm.size());
        append_u32(d.HTE_LeafTermBlockID);
    }

    /* Level-2 leaf pages */
    append_u32(static_cast<uint32_t>(br.BBR_LeafTermPages.size()));
    if (!br.BBR_LeafTermPages.empty())
        append(br.BBR_LeafTermPages.data(), br.BBR_LeafTermPages.size() * sizeof(LeafTermPage));

    /* ── Encode PageSkipList ──────────────────────────────────────────────*/
    std::vector<uint8_t> pageskip_buf(br.BBR_PageSkipList.size() * 8);
    if (!br.BBR_PageSkipList.empty())
        std::memcpy(pageskip_buf.data(), br.BBR_PageSkipList.data(), pageskip_buf.size());

    /* ── Encode DocData ───────────────────────────────────────────────────*/
    std::vector<DocRecord> docdata;
    uint64_t maxDocID = 0;
    for (const auto& [id, _] : store.AllDocStats())
        maxDocID = std::max(maxDocID, id);
    if (!store.AllDocStats().empty())
        docdata.resize(static_cast<size_t>(maxDocID + 1));

    for (const auto& [id, ds] : store.AllDocStats()) {
        DocRecord r{};
        r.DR_DocID     = id;
        r.DR_DocLength = ds.doc_len;
        r.DR_StaticRankHalf = EncodeFloat16(ds.importance);
        r.DR_QualityScoreHalf = EncodeFloat16(0.0f);
        r.DR_FreshnessScoreHalf = EncodeFloat16(0.0f);
        r.DR_ClickScoreHalf = EncodeFloat16(0.0f);
        r.DR_EngagementScoreHalf = EncodeFloat16(0.0f);
        if (const auto* vector = store.GetDocVector(id);
            vector && !vector->empty() && vector->size() <= DOC_VECTOR_STORAGE_MAX_DIM) {
            r.DR_VectorDim = static_cast<uint16_t>(vector->size());
            r.DR_VectorFormat = 1;
            for (size_t i = 0; i < vector->size(); ++i) {
                const uint16_t encoded = EncodeFloat16((*vector)[i]);
                std::memcpy(&r.DR_VectorData[i * sizeof(uint16_t)], &encoded, sizeof(uint16_t));
            }
        }
        r.DR_PathLength = EncodeDocPath(ds.path, r.DR_Path);
        docdata[static_cast<size_t>(id)] = r;
    }

    /* ── Compute file offsets ─────────────────────────────────────────────*/
    const uint64_t HDR_SIZE  = sizeof(IndexFileHeader);
    uint64_t si_off          = HDR_SIZE;
    uint64_t si_size         = head_leaf_table_buf.size();
    uint64_t ps_off          = si_off + si_size;
    uint64_t ps_size         = pageskip_buf.size();
    uint64_t dd_off          = ps_off + ps_size;
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
    hdr.IFH_SubIndexOffset  = si_off;   hdr.IFH_SubIndexSize = si_size;
    hdr.IFH_PageSkipOffset  = ps_off;   hdr.IFH_PageSkipSize = ps_size;
    hdr.IFH_DocDataOffset   = dd_off;   hdr.IFH_DocDataSize  = dd_size;
    hdr.IFH_BlocksOffset    = blk_off;  hdr.IFH_NumBlocks    = br.BBR_IndexBlocks.size();
    fwrite(&hdr, sizeof(hdr), 1, f);

    fwrite(head_leaf_table_buf.data(), 1, head_leaf_table_buf.size(), f);
    fwrite(pageskip_buf.data(), 1, pageskip_buf.size(), f);
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
                           std::vector<LeafTermPage>*              leafTermPagesOut,
                           uint64_t*                              blocks_offset_out,
                           std::vector<uint64_t>*                 pageskip_out,
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

    IndexFileHeader hdr{};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1
        || std::memcmp(hdr.IFH_Magic, INDEX_FILE_MAGIC, 8) != 0
        || hdr.IFH_Version != INDEX_FORMAT_VERSION)
    {
        return false;
    }
    if (header_out) *header_out = hdr;

    /* ── Head/Leaf term table ─────────────────────────────────────────────*/
    if ((headTermEntriesOut || leafTermPagesOut || leaf_blocks_offset_out || num_leaf_blocks_out) && hdr.IFH_SubIndexSize > 0) {
        if (!seek_file(f, hdr.IFH_SubIndexOffset))
            return false;

        auto read_u16_file = [&]() -> uint16_t {
            uint16_t v = 0; fread(&v, 2, 1, f); return v;
        };
        auto read_u32_file = [&]() -> uint32_t {
            uint32_t v = 0; fread(&v, 4, 1, f); return v;
        };
        auto read_str_file = [&](size_t n) -> std::string {
            std::string s(n, '\0');
            if (n) fread(s.data(), 1, n, f);
            return s;
        };

        /* Level-1 head table — eagerly memory resident. */
        uint32_t dir_count = read_u32_file();
        if (headTermEntriesOut) headTermEntriesOut->reserve(dir_count);
        for (uint32_t i = 0; i < dir_count; ++i) {
            uint16_t    kl    = read_u16_file();
            std::string first = read_str_file(kl);
            uint32_t    bidx  = read_u32_file();
            if (headTermEntriesOut) headTermEntriesOut->push_back({std::move(first), bidx});
        }

        /* Level-2 leaf pages — only read into memory when explicitly requested. */
        uint32_t leaf_page_count = read_u32_file();
        uint64_t leaf_pages_offset = file_pos(f);
        if (leaf_blocks_offset_out) *leaf_blocks_offset_out = leaf_pages_offset;
        if (num_leaf_blocks_out) *num_leaf_blocks_out = leaf_page_count;
        if (leafTermPagesOut && leaf_page_count > 0) {
            leafTermPagesOut->resize(leaf_page_count);
            fread(leafTermPagesOut->data(), sizeof(LeafTermPage), leaf_page_count, f);
        }
    }

    /* ── PageSkipList ─────────────────────────────────────────────────────*/
    if (pageskip_out && hdr.IFH_PageSkipSize > 0) {
        size_t n = static_cast<size_t>(hdr.IFH_PageSkipSize / 8);
        pageskip_out->resize(n);
        if (!seek_file(f, hdr.IFH_PageSkipOffset))
            return false;
        fread(pageskip_out->data(), 8, n, f);
    }

    if (blocks_offset_out) *blocks_offset_out = hdr.IFH_BlocksOffset;
    if (num_blocks_out)    *num_blocks_out    = hdr.IFH_NumBlocks;
    if (docdata_offset_out) *docdata_offset_out = hdr.IFH_DocDataOffset;
    if (docdata_size_out)   *docdata_size_out   = hdr.IFH_DocDataSize;
    if (num_documents_out)  *num_documents_out  = hdr.IFH_NumDocuments;

    /* ── DocData ──────────────────────────────────────────────────────────*/
    {
        size_t n = static_cast<size_t>(hdr.IFH_DocDataSize / DOC_REC_SIZE);
        if (!seek_file(f, hdr.IFH_DocDataOffset))
            return false;
        std::vector<DocRecord> recs(n);
        if (n > 0) fread(recs.data(), DOC_REC_SIZE, n, f);
        for (const auto& r : recs) {
            if (r.DR_DocID >= n)
                continue;
            store.AddDocTokens(r.DR_DocID, r.DR_DocLength);
            store.SetDocImportance(r.DR_DocID, DecodeFloat16(r.DR_StaticRankHalf));
            if (r.DR_VectorDim > 0 && r.DR_VectorDim <= DOC_VECTOR_STORAGE_MAX_DIM && r.DR_VectorFormat == 1) {
                std::vector<float> vector(r.DR_VectorDim);
                for (size_t i = 0; i < r.DR_VectorDim; ++i) {
                    uint16_t encoded = 0;
                    std::memcpy(&encoded, &r.DR_VectorData[i * sizeof(uint16_t)], sizeof(uint16_t));
                    vector[i] = DecodeFloat16(encoded);
                }
                store.SetDocVector(r.DR_DocID, std::move(vector));
            }
            if (r.DR_PathLength > 0)
                store.SetDocPath(r.DR_DocID, DecodeDocPath(r));
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
