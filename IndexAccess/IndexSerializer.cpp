/*
 * IndexSerializer — v5 two-level TermHeaderTable format.
 *
 * Block IB_Data: raw posting bytes only (no key/doc_freq/data_len headers).
 * TermHeaderTable: two-level term metadata structure.
 *   Level-1 dir: sorted (first_term, term_header_block_id) pairs.
 *   Level-2 blocks: fixed groups of TermHeader records, sorted by term.
 * Continuation blocks: dedicated (no new terms mixed in).
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
static const uint32_t FORMAT_VERSION = 5;

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

/* ─────────────────────────────────────────────────────────────────────────
 * build_blocks
 *
 * 1. Pack posting bytes into IndexBlocks (no key/freq/len headers).
 *    Continuation blocks are dedicated — no mixed new-term entries.
 * 2. Build a flat sorted TermHeader list (one TermHeader per term).
 * 3. Organize into two-level TermHeaderTable:
 *    TermDirectory: one entry per TermHeaderBlock → first term in that block.
 *    TermHeaderBlocks: fixed-size chunks of TERM_HEADERS_PER_BLOCK headers.
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

    /* Flat sorted TermHeader list built during packing, distributed into two levels later. */
    std::vector<TermHeader> flat;
    flat.reserve(terms.size());
    res.pageskip.push_back(UINT64_MAX); // offset 0 means no skip list

    auto flush = [&](bool has_more) {
        cur.IB_Header = static_cast<uint64_t>(seq);
        if (has_more) cur.IB_Header |= IB_HEADER_HAS_MORE;
        res.blocks.push_back(cur);
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

            uint32_t skip_offset = static_cast<uint32_t>(res.pageskip.size());
            res.pageskip.push_back(0u);
            flat.back().skip_list_offset = skip_offset;

            while (remaining > 0) {
                constexpr size_t CONT_HDR = 4u;
                size_t cont_cap  = DATA_CAP - CONT_HDR;
                size_t cont_here = std::min(remaining, cont_cap);
                bool   more_cont = (cont_here < remaining);

                size_t   bytes_before = bytes.size() - remaining;
                uint64_t base_doc     = decode_last_docid(bytes.data(), bytes_before);
                res.pageskip.push_back(base_doc);

                uint16_t cm = CONT, cl = static_cast<uint16_t>(cont_here);
                std::memcpy(cur.IB_Data + wptr, &cm, 2); wptr += 2;
                std::memcpy(cur.IB_Data + wptr, &cl, 2); wptr += 2;
                std::memcpy(cur.IB_Data + wptr, src, cont_here); wptr += cont_here;
                src       += cont_here;
                remaining -= cont_here;

                ++flat.back().continuation_block_count;

                flush(more_cont);
            }
            res.pageskip.push_back(UINT64_MAX);
        }
    }

    if (wptr > 0) flush(false);

    /* ── Build two-level TermHeaderTable ───────────────────────────────────
     *
     * TermHeaderBlocks are independent metadata blocks.  They are NOT grouped
     * by posting_block_id.  Each block holds up to TERM_HEADERS_PER_BLOCK term
     * headers and the directory stores the first term of each header block.
     */
    for (size_t i = 0; i < flat.size(); i += TERM_HEADERS_PER_BLOCK) {
        TermHeaderBlock block;
        size_t end = std::min(i + static_cast<size_t>(TERM_HEADERS_PER_BLOCK), flat.size());
        block.headers.reserve(end - i);
        for (size_t j = i; j < end; ++j)
            block.headers.push_back(std::move(flat[j]));

        if (!block.headers.empty()) {
            res.term_directory.push_back({block.headers.front().term,
                                          static_cast<uint32_t>(res.term_header_blocks.size())});
            res.term_header_blocks.push_back(std::move(block));
        }
    }

    return res;
}

/* ── Save ─────────────────────────────────────────────────────────────────*/

bool IndexSerializer::Save(const PostingStore& store, const char* path)
{
    if (!path || !*path) return false;

    BuildBlocksResult br = build_blocks(store);

    /* ── Encode TermHeaderTable (two-level) ───────────────────────────────
     *
     * Format:
     *   [dir_count:4]
     *     per dir entry: [key_len:2][first_term:key_len][term_header_block_id:4]
     *   [block_count:4]
     *     per block: [entry_count:4]
     *       per entry: [key_len:2][term:key_len][doc_freq:4]
     *                  [posting_block_id:4][posting_offset:4][posting_length:4]
     *                  [skip_list_offset:4][continuation_block_count:4][flags:4]
     */
    std::vector<uint8_t> term_header_table_buf;

    auto append = [&](const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        term_header_table_buf.insert(term_header_table_buf.end(), b, b + n);
    };
    auto append_u16 = [&](uint16_t v) { append(&v, 2); };
    auto append_u32 = [&](uint32_t v) { append(&v, 4); };

    /* Level-1 directory */
    append_u32(static_cast<uint32_t>(br.term_directory.size()));
    for (const auto& d : br.term_directory) {
        append_u16(static_cast<uint16_t>(d.first_term.size()));
        append(d.first_term.data(), d.first_term.size());
        append_u32(d.term_header_block_id);
    }

    /* Level-2 blocks */
    append_u32(static_cast<uint32_t>(br.term_header_blocks.size()));
    for (const auto& blk : br.term_header_blocks) {
        append_u32(static_cast<uint32_t>(blk.headers.size()));
        for (const auto& e : blk.headers) {
            append_u16(static_cast<uint16_t>(e.term.size()));
            append(e.term.data(), e.term.size());
            append_u32(e.doc_freq);
            append_u32(e.posting_block_id);
            append_u32(e.posting_offset);
            append_u32(e.posting_length);
            append_u32(e.skip_list_offset);
            append_u32(e.continuation_block_count);
            append_u32(e.flags);
        }
    }

    /* ── Encode PageSkipList ──────────────────────────────────────────────*/
    std::vector<uint8_t> pageskip_buf(br.pageskip.size() * 8);
    if (!br.pageskip.empty())
        std::memcpy(pageskip_buf.data(), br.pageskip.data(), pageskip_buf.size());

    /* ── Encode DocData ───────────────────────────────────────────────────*/
    static constexpr size_t DOC_REC_SIZE = 256;
    static constexpr size_t DOC_PATH_MAX = 232;
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
    uint64_t si_size         = term_header_table_buf.size();
    uint64_t ps_off          = si_off + si_size;
    uint64_t ps_size         = pageskip_buf.size();
    uint64_t dd_off          = ps_off + ps_size;
    uint64_t dd_size         = docdata.size() * DOC_REC_SIZE;
    uint64_t raw_blk_off     = dd_off + dd_size;
    uint64_t blk_off         = ((raw_blk_off + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

    /* Count total terms across all TermHeaderBlocks */
    uint64_t total_terms = 0;
    for (const auto& blk : br.term_header_blocks) total_terms += blk.headers.size();

    FILE* f = fopen(path, "wb");
    if (!f) return false;

    Hdr hdr{};
    std::memcpy(hdr.magic, MAGIC, 8);
    hdr.version       = FORMAT_VERSION;
    hdr.num_documents = static_cast<uint64_t>(docdata.size());
    hdr.num_terms     = total_terms;
    hdr.subindex_off  = si_off;   hdr.subindex_size = si_size;
    hdr.pageskip_off  = ps_off;   hdr.pageskip_size = ps_size;
    hdr.docdata_off   = dd_off;   hdr.docdata_size  = dd_size;
    hdr.blocks_off    = blk_off;  hdr.num_blocks    = br.blocks.size();
    fwrite(&hdr, sizeof(hdr), 1, f);

    fwrite(term_header_table_buf.data(), 1, term_header_table_buf.size(), f);
    fwrite(pageskip_buf.data(), 1, pageskip_buf.size(), f);
    if (!docdata.empty())
        fwrite(docdata.data(), DOC_REC_SIZE, docdata.size(), f);

    uint64_t cur_pos = file_pos(f);
    if (cur_pos < blk_off) {
        std::vector<uint8_t> pad(static_cast<size_t>(blk_off - cur_pos), 0);
        fwrite(pad.data(), 1, pad.size(), f);
    }
    for (const auto& b : br.blocks)
        fwrite(&b, sizeof(IndexBlock), 1, f);

    fclose(f);
    return true;
}

/* ── Load ─────────────────────────────────────────────────────────────────*/

bool IndexSerializer::Load(PostingStore&                           store,
                           const char*                            path,
                           std::vector<TermDirectoryEntry>*        dir_out,
                           std::vector<TermHeaderBlock>*           blocks_out,
                           uint64_t*                              blocks_offset_out,
                           std::vector<uint64_t>*                 pageskip_out,
                           uint64_t*                              num_blocks_out)
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

    /* ── TermHeaderTable ──────────────────────────────────────────────────*/
    if ((dir_out || blocks_out) && hdr.subindex_size > 0) {
        std::vector<uint8_t> buf(static_cast<size_t>(hdr.subindex_size));
        fseek(f, static_cast<long>(hdr.subindex_off), SEEK_SET);
        fread(buf.data(), 1, buf.size(), f);

        const uint8_t* ptr = buf.data();
        const uint8_t* end = buf.data() + buf.size();

        auto read_u16 = [&]() -> uint16_t {
            if (ptr + 2 > end) return 0;
            uint16_t v; std::memcpy(&v, ptr, 2); ptr += 2; return v; };
        auto read_u32 = [&]() -> uint32_t {
            if (ptr + 4 > end) return 0;
            uint32_t v; std::memcpy(&v, ptr, 4); ptr += 4; return v; };
        auto read_str = [&](size_t n) -> std::string {
            if (ptr + n > end) return {};
            std::string s(reinterpret_cast<const char*>(ptr), n); ptr += n; return s; };

        /* Level-1 directory */
        uint32_t dir_count = read_u32();
        if (dir_out) dir_out->reserve(dir_count);
        for (uint32_t i = 0; i < dir_count && ptr < end; ++i) {
            uint16_t    kl    = read_u16();
            std::string first = read_str(kl);
            uint32_t    bidx  = read_u32();
            if (dir_out) dir_out->push_back({std::move(first), bidx});
        }

        /* Level-2 blocks */
        uint32_t blk_count = read_u32();
        if (blocks_out) blocks_out->resize(blk_count);
        for (uint32_t b = 0; b < blk_count && ptr < end; ++b) {
            uint32_t entry_count = read_u32();
            if (blocks_out) (*blocks_out)[b].headers.reserve(entry_count);
            for (uint32_t i = 0; i < entry_count && ptr < end; ++i) {
                uint16_t    kl   = read_u16();
                std::string term = read_str(kl);
                uint32_t freq    = read_u32();
                uint32_t pbid    = read_u32();
                uint32_t poff    = read_u32();
                uint32_t plen    = read_u32();
                uint32_t skip    = read_u32();
                uint32_t cont    = read_u32();
                uint32_t flags   = read_u32();
                if (blocks_out)
                    (*blocks_out)[b].headers.push_back(
                        {std::move(term), freq, pbid, poff, plen, skip, cont, flags});
            }
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
        static constexpr size_t REC = 256, PMAX = 232;
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
