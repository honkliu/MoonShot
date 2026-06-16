/*
 * IndexSerializer — v7 paged Head/Leaf term table format.
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

static const uint8_t  MAGIC[8]       = {'M','O','O','N','S','H','O','T'};
static const uint32_t FORMAT_VERSION = 7;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static uint64_t file_pos(FILE* f) { return static_cast<uint64_t>(ftell(f)); }

static uint64_t decode_last_docid(const uint8_t* data, size_t len)
{
    size_t   pos  = 0;
    uint64_t prev = 0;
    while (pos < len) {
        uint64_t delta = 0; uint8_t shift = 0, b;
        do { if (pos >= len) goto done;
             b = data[pos++]; delta |= uint64_t(b & 0x7fu) << shift; shift += 7;
        } while (b & 0x80u);
        uint64_t tf = 0; shift = 0;
        do { if (pos >= len) goto done;
             b = data[pos++]; tf |= uint64_t(b & 0x7fu) << shift; shift += 7;
        } while (b & 0x80u);
        prev += delta;
    }
done:
    return prev;
}

static size_t leaf_entry_bytes(const LeafTermEntry& e)
{
    return 2u + e.LTE_Term.size() + 7u * sizeof(uint32_t);
}

static void encode_leaf_page(const std::vector<LeafTermEntry>& entries, LeafTermPage& page)
{
    std::memset(&page, 0, sizeof(page));
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
    static constexpr size_t DOC_REC_SIZE = 1024;
    static constexpr size_t DOC_PATH_MAX = 1000;
#pragma pack(push,1)
    struct DocRec { uint64_t doc_id; float importance; uint32_t doc_len;
                    uint16_t path_len; uint8_t _pad[6]; char path[DOC_PATH_MAX]; };
#pragma pack(pop)
    static_assert(sizeof(DocRec) == DOC_REC_SIZE, "");
    std::vector<DocRec> docdata;
    docdata.reserve(store.AllDocStats().size());
    for (const auto& [id, ds] : store.AllDocStats()) {
        DocRec r{};
        r.doc_id     = id;
        r.importance = ds.importance;
        r.doc_len    = ds.doc_len;
        size_t plen  = std::min(ds.path.size(), DOC_PATH_MAX - 1);
        r.path_len   = static_cast<uint16_t>(plen);
        if (plen) std::memcpy(r.path, ds.path.c_str(), plen);
        docdata.push_back(r);
    }

    /* ── Compute file offsets ─────────────────────────────────────────────*/
#pragma pack(push,1)
    struct Hdr {
        uint8_t  magic[8]; uint32_t version, reserved;
        uint64_t num_documents, num_terms;
        uint64_t subindex_off, subindex_size;
        uint64_t pageskip_off, pageskip_size;
        uint64_t docdata_off,  docdata_size;
        uint64_t blocks_off,   num_blocks;
    };
#pragma pack(pop)
    const uint64_t HDR_SIZE  = sizeof(Hdr);
    uint64_t si_off          = HDR_SIZE;
    uint64_t si_size         = head_leaf_table_buf.size();
    uint64_t ps_off          = si_off + si_size;
    uint64_t ps_size         = pageskip_buf.size();
    uint64_t dd_off          = ps_off + ps_size;
    uint64_t dd_size         = docdata.size() * DOC_REC_SIZE;
    uint64_t raw_blk_off     = dd_off + dd_size;
    uint64_t blk_off         = ((raw_blk_off + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

    FILE* f = fopen(path, "wb");
    if (!f) return false;

    Hdr hdr{};
    std::memcpy(hdr.magic, MAGIC, 8);
    hdr.version       = FORMAT_VERSION;
    hdr.num_documents = static_cast<uint64_t>(docdata.size());
    hdr.num_terms     = br.BBR_TotalTerms;
    hdr.subindex_off  = si_off;   hdr.subindex_size = si_size;
    hdr.pageskip_off  = ps_off;   hdr.pageskip_size = ps_size;
    hdr.docdata_off   = dd_off;   hdr.docdata_size  = dd_size;
    hdr.blocks_off    = blk_off;  hdr.num_blocks    = br.BBR_IndexBlocks.size();
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

    fclose(f);
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
                           uint64_t*                              num_leaf_blocks_out)
{
    if (!path || !*path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;

#pragma pack(push,1)
    struct Hdr {
        uint8_t  magic[8]; uint32_t version, reserved;
        uint64_t num_documents, num_terms;
        uint64_t subindex_off, subindex_size;
        uint64_t pageskip_off, pageskip_size;
        uint64_t docdata_off,  docdata_size;
        uint64_t blocks_off,   num_blocks;
    };
#pragma pack(pop)

    Hdr hdr{};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1
        || std::memcmp(hdr.magic, MAGIC, 8) != 0
        || hdr.version != FORMAT_VERSION)
    { fclose(f); return false; }

    /* ── Head/Leaf term table ─────────────────────────────────────────────*/
    if ((headTermEntriesOut || leafTermPagesOut || leaf_blocks_offset_out || num_leaf_blocks_out) && hdr.subindex_size > 0) {
        fseek(f, static_cast<long>(hdr.subindex_off), SEEK_SET);

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
        uint64_t leaf_pages_offset = static_cast<uint64_t>(ftell(f));
        if (leaf_blocks_offset_out) *leaf_blocks_offset_out = leaf_pages_offset;
        if (num_leaf_blocks_out) *num_leaf_blocks_out = leaf_page_count;
        if (leafTermPagesOut && leaf_page_count > 0) {
            leafTermPagesOut->resize(leaf_page_count);
            fread(leafTermPagesOut->data(), sizeof(LeafTermPage), leaf_page_count, f);
        }
    }

    /* ── PageSkipList ─────────────────────────────────────────────────────*/
    if (pageskip_out && hdr.pageskip_size > 0) {
        size_t n = static_cast<size_t>(hdr.pageskip_size / 8);
        pageskip_out->resize(n);
        fseek(f, static_cast<long>(hdr.pageskip_off), SEEK_SET);
        fread(pageskip_out->data(), 8, n, f);
    }

    if (blocks_offset_out) *blocks_offset_out = hdr.blocks_off;
    if (num_blocks_out)    *num_blocks_out    = hdr.num_blocks;

    /* ── DocData ──────────────────────────────────────────────────────────*/
    {
        static constexpr size_t REC = 1024, PMAX = 1000;
#pragma pack(push,1)
        struct DocRec { uint64_t doc_id; float importance; uint32_t doc_len;
                        uint16_t path_len; uint8_t _pad[6]; char path[PMAX]; };
#pragma pack(pop)
        static_assert(sizeof(DocRec) == REC, "");
        size_t n = static_cast<size_t>(hdr.docdata_size / REC);
        std::vector<DocRec> recs(n);
        fseek(f, static_cast<long>(hdr.docdata_off), SEEK_SET);
        if (n > 0) fread(recs.data(), REC, n, f);
        for (const auto& r : recs) {
            store.AddDocTokens(r.doc_id, r.doc_len);
            store.SetDocImportance(r.doc_id, r.importance);
            if (r.path_len > 0)
                store.SetDocPath(r.doc_id,
                    std::string(r.path, std::min<size_t>(r.path_len, PMAX - 1)));
        }
    }

    fclose(f);
    return true;
}

bool IndexSerializer::IsValidIndex(const char* path)
{
    if (!path || !*path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t magic[8] = {};
    fread(magic, 1, 8, f);
    fclose(f);
    return std::memcmp(magic, MAGIC, 8) == 0;
}

BuildBlocksResult IndexSerializer::BuildBlocksForContext(const PostingStore& store)
{
    return build_blocks(store);
}
