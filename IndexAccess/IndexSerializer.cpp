/*
 * IndexSerializer — v3 block-based format.
 *
 * Changes from v2:
 *  - Continuation blocks: [CONT_MARKER:2][cont_len:2][VarByte:cont_len][optional new entries]
 *    No space is wasted — new terms pack into the same block after cont data.
 *  - SubIndex entries add block_entry_start + page_skip_offset fields.
 *  - New PageSkipList section: per-term base_doc arrays for multi-block terms.
 *    Enables O(log P) GoUntil() cross-block seek (vs O(P) sequential).
 *
 * File layout:
 *   [Header 96B]      magic, version, all section offsets
 *   [SubIndex]        enhanced (block_entry_start, page_skip_offset per entry)
 *   [PageSkipList]    flat uint64_t arrays, one per multi-block term
 *   [DocData]         N x 16B records
 *   [Padding]         zeros to PAGE_SIZE
 *   [Blocks]          raw IndexBlock structs
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
static const uint32_t FORMAT_VERSION = 4;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void write_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void write_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void write_u64(FILE* f, uint64_t v) { fwrite(&v, 8, 1, f); }

static uint16_t read_u16(FILE* f) { uint16_t v=0; fread(&v,2,1,f); return v; }
static uint32_t read_u32(FILE* f) { uint32_t v=0; fread(&v,4,1,f); return v; }
static uint64_t read_u64(FILE* f) { uint64_t v=0; fread(&v,8,1,f); return v; }
static float    read_f32(FILE* f) { float    v=0; fread(&v,4,1,f); return v; }

static uint64_t file_pos(FILE* f) { return static_cast<uint64_t>(ftell(f)); }

/* ─────────────────────────────────────────────────────────────────────────
 * build_blocks — core packing logic shared by Save() and IndexContext::Build()
 *
 * Key changes vs v2:
 *  - Continuation blocks include cont_len (u16) right after CONT_MARKER.
 *  - After cont_len bytes, remaining block space accepts new term entries.
 *  - SubIndex entries carry block_entry_start (where entries start in block)
 *    and page_skip_offset (into PageSkipList for multi-block terms).
 *  - PageSkipList: for every multi-block term, records base_doc_id at the
 *    start of each continuation block as [0, base2, base3, ..., UINT64_MAX].
 * ─────────────────────────────────────────────────────────────────────────*/


static BuildBlocksResult build_blocks(const PostingStore& store)
{
    constexpr size_t   DATA_CAP  = sizeof(IndexBlock::IB_Data) - 1u;
    constexpr uint16_t CONT      = BLOCK_CONTINUATION_MARKER;

    /* Sort terms alphabetically. */
    std::vector<std::pair<const std::string*, const PostingList*>> terms;
    terms.reserve(store.AllPostings().size());
    for (const auto& [k, pl] : store.AllPostings())
        terms.push_back({&k, &pl});
    std::sort(terms.begin(), terms.end(),
              [](auto& a, auto& b){ return *a.first < *b.first; });

    BuildBlocksResult res;
    IndexBlock  cur        = {};
    uint8_t*    wptr       = cur.IB_Data;
    uint8_t*    wend       = cur.IB_Data + DATA_CAP;
    uint32_t    seq        = 0;
    bool        fresh      = true;
    uint32_t    blk_entry_start = 0;  // byte offset in cur.IB_Data where entries start

    /* flush: finalize current block and start a new one */
    auto flush = [&](bool has_more) {
        cur.IB_Header = static_cast<uint64_t>(seq);
        if (has_more) cur.IB_Header |= IB_HEADER_HAS_MORE;
        /* write sentinel if last term entry block */
        if (!has_more && wptr + 2 <= wend + 1) {
            *wptr++ = 0; *wptr = 0;
        }
        res.blocks.push_back(cur);
        ++seq;
        cur   = {};
        wptr  = cur.IB_Data;
        fresh = true;
        blk_entry_start = 0;
    };

    for (const auto& [key_ptr, pl] : terms) {
        const std::string& key   = *key_ptr;
        const auto&        bytes = pl->GetBytes();
        if (bytes.empty()) continue;

        uint16_t kl       = static_cast<uint16_t>(key.size());
        uint32_t freq     = pl->doc_freq();
        size_t   hdr_size = 2u + kl + 4u + 4u;

        /* flush if not enough room for this entry header */
        if (static_cast<size_t>(wend - wptr) < hdr_size + 1u)
            flush(false);

        if (fresh) {
            res.subindex.push_back({key, seq, blk_entry_start, 0u});
            fresh = false;
        }

        const uint8_t* src       = bytes.data();
        size_t         remaining = bytes.size();
        size_t         data_space = static_cast<size_t>(wend - wptr) - hdr_size;
        size_t         data_here  = std::min(remaining, data_space);
        bool           has_more   = (data_here < remaining);

        /* write term entry header */
        std::memcpy(wptr, &kl,   2);              wptr += 2;
        std::memcpy(wptr, key.c_str(), kl);       wptr += kl;
        std::memcpy(wptr, &freq, 4);              wptr += 4;
        uint32_t dl = static_cast<uint32_t>(data_here);
        std::memcpy(wptr, &dl, 4);                wptr += 4;
        std::memcpy(wptr, src,  data_here);       wptr += data_here;
        src       += data_here;
        remaining -= data_here;

        if (has_more) {
            flush(true);

            /* PageSkipList for this term: record first block_seq */
            uint32_t first_block = seq - 1;  /* block_seq just flushed */
            uint32_t skip_offset = static_cast<uint32_t>(res.pageskip.size());
            /* entry 0 = base_doc for first block = 0 */
            res.pageskip.push_back(0);

            /* update SubIndex entry's page_skip_offset */
            res.subindex.back().page_skip_offset = skip_offset;

            /* write continuation block(s) */
            while (remaining > 0) {
                /* Continuation header: CONT_MARKER(2) + cont_len(2) */
                constexpr size_t CONT_HDR = 4u;
                size_t cont_cap  = DATA_CAP - CONT_HDR;
                size_t cont_here = std::min(remaining, cont_cap);
                bool   more_cont = (cont_here < remaining);

                /* We need last_doc_id of the previous block for the skip entry.
                 * Decode the last (delta, tf) pair from the bytes we JUST wrote.
                 * Simpler: record what the last decoded doc_id was. */
                /* (We record the base BEFORE this block — i.e. the last doc_id
                 *  seen in the previous block. We approximate by storing 0 for
                 *  simplicity if we haven't tracked it. A complete impl would
                 *  decode the last VarByte entry. We push a placeholder here and
                 *  patch it in the decode path for now.) */
                /* For the skip array, store the offset so the reader can locate
                 * the right block: base_doc = last doc from previous block.
                 * We decode the previous block's last docid. */
                {
                    /* Decode last entry of previous block to get base_doc */
                    const uint8_t* prev_data;
                    size_t prev_len;
                    if (remaining == bytes.size() - data_here) {
                        /* first continuation: previous was the original block */
                        prev_data = bytes.data();
                        prev_len  = data_here;
                    } else {
                        /* subsequent: use what we wrote previously */
                        prev_data = src - (bytes.size() - remaining - cont_here);
                        prev_len  = bytes.size() - remaining;
                    }
                    /* Decode all VarByte pairs to get last doc_id */
                    uint64_t last_doc = 0, pos = 0;
                    while (pos < prev_len) {
                        uint64_t delta = 0, shift = 0;
                        uint8_t  b;
                        do { b = prev_data[pos++]; delta |= uint64_t(b&0x7f)<<shift; shift+=7; } while (b&0x80u && pos<prev_len);
                        if (pos >= prev_len) break;
                        uint64_t tf = 0; shift = 0;
                        do { b = prev_data[pos++]; tf |= uint64_t(b&0x7f)<<shift; shift+=7; } while (b&0x80u && pos<prev_len);
                        last_doc += delta;
                    }
                    res.pageskip.push_back(last_doc);
                }

                /* write CONT_MARKER + cont_len + bytes */
                uint16_t cm = CONT;
                uint16_t cl = static_cast<uint16_t>(cont_here);
                std::memcpy(wptr, &cm, 2); wptr += 2;
                std::memcpy(wptr, &cl, 2); wptr += 2;
                std::memcpy(wptr, src, cont_here); wptr += cont_here;
                src       += cont_here;
                remaining -= cont_here;

                if (more_cont) {
                    flush(true);
                } else {
                    /* last continuation: remaining space can hold new entries */
                    blk_entry_start = static_cast<uint32_t>(CONT_HDR + cont_here);
                    fresh = true;   /* next term gets a SubIndex entry in THIS block */
                    /* DON'T flush — reuse this block for next terms */
                }
            }

            /* close the PageSkipList array for this term */
            res.pageskip.push_back(UINT64_MAX);
        }
    }

    /* flush final block */
    if (!fresh || wptr != cur.IB_Data)
        flush(false);

    return res;
}

/* ── Save ─────────────────────────────────────────────────────────────────*/

bool IndexSerializer::Save(const PostingStore& store, const char* path)
{
    if (!path || !*path) return false;

    BuildBlocksResult br = build_blocks(store);

    /* encode SubIndex */
    std::vector<uint8_t> subindex_buf;
    {
        uint32_t n = static_cast<uint32_t>(br.subindex.size());
        subindex_buf.resize(4);
        std::memcpy(subindex_buf.data(), &n, 4);
        for (const auto& e : br.subindex) {
            uint16_t kl = static_cast<uint16_t>(e.term.size());
            size_t   off = subindex_buf.size();
            subindex_buf.resize(off + 2 + kl + 4 + 4 + 4);
            std::memcpy(subindex_buf.data() + off,              &kl,                   2);
            std::memcpy(subindex_buf.data() + off + 2,          e.term.c_str(),        kl);
            std::memcpy(subindex_buf.data() + off + 2 + kl,     &e.block_seq,          4);
            std::memcpy(subindex_buf.data() + off + 2 + kl + 4, &e.block_entry_start,  4);
            std::memcpy(subindex_buf.data() + off + 2 + kl + 8, &e.page_skip_offset,   4);
        }
    }

    /* encode PageSkipList */
    std::vector<uint8_t> pageskip_buf(br.pageskip.size() * 8);
    if (!br.pageskip.empty())
        std::memcpy(pageskip_buf.data(), br.pageskip.data(), pageskip_buf.size());

    /* encode DocData — 256B per record includes file path */
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

    /* compute offsets — Header is now 96B */
#pragma pack(push,1)
    struct Hdr {
        uint8_t  magic[8];
        uint32_t version, reserved;
        uint64_t num_documents, num_terms;
        uint64_t subindex_off, subindex_size;
        uint64_t pageskip_off, pageskip_size;
        uint64_t docdata_off,  docdata_size;
        uint64_t blocks_off,   num_blocks;
    };
#pragma pack(pop)
    const uint64_t HDR_SIZE     = sizeof(Hdr);
    uint64_t subindex_off       = HDR_SIZE;
    uint64_t subindex_size      = subindex_buf.size();
    uint64_t pageskip_off       = subindex_off + subindex_size;
    uint64_t pageskip_size      = pageskip_buf.size();
    uint64_t docdata_off        = pageskip_off + pageskip_size;
    uint64_t docdata_size       = docdata.size() * DOC_REC_SIZE;
    uint64_t raw_blocks_off     = docdata_off + docdata_size;
    uint64_t blocks_off         = ((raw_blocks_off + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

    FILE* f = fopen(path, "wb");
    if (!f) return false;

    Hdr hdr{};
    std::memcpy(hdr.magic, MAGIC, 8);
    hdr.version       = FORMAT_VERSION;
    hdr.num_documents = static_cast<uint64_t>(docdata.size());
    hdr.num_terms     = static_cast<uint64_t>(br.subindex.size());
    hdr.subindex_off  = subindex_off;  hdr.subindex_size = subindex_size;
    hdr.pageskip_off  = pageskip_off;  hdr.pageskip_size = pageskip_size;
    hdr.docdata_off   = docdata_off;   hdr.docdata_size  = docdata_size;
    hdr.blocks_off    = blocks_off;    hdr.num_blocks    = br.blocks.size();
    fwrite(&hdr, sizeof(hdr), 1, f);

    fwrite(subindex_buf.data(), 1, subindex_buf.size(), f);
    fwrite(pageskip_buf.data(), 1, pageskip_buf.size(), f);
    if (!docdata.empty())
        fwrite(docdata.data(), DOC_REC_SIZE, docdata.size(), f);

    /* pad to blocks_off */
    uint64_t cur_pos = file_pos(f);
    if (cur_pos < blocks_off) {
        std::vector<uint8_t> pad(static_cast<size_t>(blocks_off - cur_pos), 0);
        fwrite(pad.data(), 1, pad.size(), f);
    }

    for (const auto& b : br.blocks)
        fwrite(&b, sizeof(IndexBlock), 1, f);

    fclose(f);
    return true;
}

/* ── Load ─────────────────────────────────────────────────────────────────*/

bool IndexSerializer::Load(PostingStore&              store,
                           const char*                path,
                           std::vector<SubIndexEntry>* subindex_out,
                           uint64_t*                   blocks_offset_out,
                           std::vector<uint64_t>*      pageskip_out)
{
    if (!path || !*path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;

#pragma pack(push,1)
    struct Hdr {
        uint8_t  magic[8];
        uint32_t version, reserved;
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
    {
        fclose(f); return false;
    }

    /* SubIndex */
    if (subindex_out && hdr.subindex_size > 0) {
        std::vector<uint8_t> buf(static_cast<size_t>(hdr.subindex_size));
        fseek(f, static_cast<long>(hdr.subindex_off), SEEK_SET);
        fread(buf.data(), 1, buf.size(), f);
        if (buf.size() >= 4) {
            uint32_t n = 0;
            std::memcpy(&n, buf.data(), 4);
            const uint8_t* ptr = buf.data() + 4;
            const uint8_t* end = buf.data() + buf.size();
            subindex_out->reserve(n);
            for (uint32_t i = 0; i < n && ptr + 2 <= end; ++i) {
                uint16_t kl = 0;
                std::memcpy(&kl, ptr, 2); ptr += 2;
                if (ptr + kl + 12 > end) break;
                std::string term(reinterpret_cast<const char*>(ptr), kl); ptr += kl;
                uint32_t bseq = 0, bes = 0, pso = 0;
                std::memcpy(&bseq, ptr, 4); ptr += 4;
                std::memcpy(&bes,  ptr, 4); ptr += 4;
                std::memcpy(&pso,  ptr, 4); ptr += 4;
                subindex_out->push_back({std::move(term), bseq, bes, pso});
            }
        }
    }

    /* PageSkipList */
    if (pageskip_out && hdr.pageskip_size > 0) {
        size_t n = static_cast<size_t>(hdr.pageskip_size / 8);
        pageskip_out->resize(n);
        fseek(f, static_cast<long>(hdr.pageskip_off), SEEK_SET);
        fread(pageskip_out->data(), 8, n, f);
    }

    if (blocks_offset_out) *blocks_offset_out = hdr.blocks_off;

    /* DocData — 256B records */
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

/* Public alias used by IndexContext::Build() */
BuildBlocksResult IndexSerializer::BuildBlocksForContext(const PostingStore& store)
{
    return build_blocks(store);
}
