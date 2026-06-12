/*
 * IndexSerializer — saves and loads the MoonShot index in a block-based
 * format that mirrors Tiger's file layout:
 *
 *   [FileHeader]   Fixed 64-byte header at byte 0.
 *   [SubIndex]     Packed (key_len, key, block_seq) entries — sparse,
 *                  one per posting block — sorted by term.
 *   [DocData]      Fixed 16-byte records: doc_id, importance, doc_len.
 *   [Padding]      Zero bytes to align the next section to PAGE_SIZE.
 *   [Blocks]       Raw IndexBlock structs (each PAGE_SIZE bytes).
 *                  FileBlockManager reads at base_offset + seq * PAGE_SIZE.
 *
 * The Header stores the byte offsets of every section so the file is
 * self-describing.  FileBlockManager is opened with base_offset pointing
 * at the Blocks section, so block_seq 0 maps directly to that offset.
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
static const uint32_t FORMAT_VERSION = 2;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void write_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void write_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void write_u64(FILE* f, uint64_t v) { fwrite(&v, 8, 1, f); }

static uint16_t read_u16(FILE* f) { uint16_t v=0; fread(&v,2,1,f); return v; }
static uint32_t read_u32(FILE* f) { uint32_t v=0; fread(&v,4,1,f); return v; }
static uint64_t read_u64(FILE* f) { uint64_t v=0; fread(&v,8,1,f); return v; }
static float    read_f32(FILE* f) { float    v=0; fread(&v,4,1,f); return v; }

static uint64_t file_pos(FILE* f) { return static_cast<uint64_t>(ftell(f)); }

/* ── VarByte codec ───────────────────────────────────────────────────────── */

static void vb_encode(uint64_t v, std::vector<uint8_t>& buf)
{
    while (v >= 0x80u) {
        buf.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(v));
}

/* ─────────────────────────────────────────────────────────────────────────
 * Save
 * ─────────────────────────────────────────────────────────────────────────
 *
 * Builds multi-term-per-block IndexBlocks sorted alphabetically, writes
 * them after the SubIndex and DocData sections.
 */
bool IndexSerializer::Save(const PostingStore& store, const char* path)
{
    if (!path || !*path) return false;

    /* 1. Sort all terms. */
    struct TermEntry {
        std::string        key;
        const PostingList* list;
    };
    std::vector<TermEntry> terms;
    terms.reserve(store.AllPostings().size());
    for (const auto& [k, pl] : store.AllPostings())
        terms.push_back({k, &pl});
    std::sort(terms.begin(), terms.end(),
              [](const TermEntry& a, const TermEntry& b){ return a.key < b.key; });

    /* 2. Pack terms into IndexBlocks. ──────────────────────────────────── */
    /*    Each block may hold multiple terms; if a term's VarByte data
     *    doesn't fit, continuation blocks (marked 0xFFFF) chain together. */

    constexpr size_t DATA_CAP = sizeof(IndexBlock::IB_Data) - 1u;  /* -1 sentinel */
    constexpr uint16_t CONT   = BLOCK_CONTINUATION_MARKER;

    struct Block {
        IndexBlock blk;
    };
    std::vector<Block>        blocks;
    std::vector<SubIndexEntry> subindex;

    IndexBlock   cur = {};
    uint8_t*     wptr  = cur.IB_Data;
    uint8_t*     wend  = cur.IB_Data + DATA_CAP;
    uint32_t     seq   = 0;
    bool         fresh = true;

    auto flush = [&](bool has_more) {
        cur.IB_Header = static_cast<uint64_t>(seq);
        if (has_more) cur.IB_Header |= IB_HEADER_HAS_MORE;
        if (!has_more && wptr + 2 <= wend + 1) {
            *wptr++ = 0; *wptr = 0;   /* sentinel: key_len = 0 */
        }
        blocks.push_back({cur});
        ++seq;
        cur   = {};
        wptr  = cur.IB_Data;
        fresh = true;
    };

    for (const auto& te : terms) {
        const auto& bytes = te.list->GetBytes();
        uint16_t kl   = static_cast<uint16_t>(te.key.size());
        uint32_t freq = te.list->doc_freq();

        /* -- Write term entry header (key_len + key + doc_freq + data_len).
         *    Try to fit in current block; flush first if not enough room.    */
        size_t hdr_size = 2u + kl + 4u + 4u;   /* key_len + key + freq + dlen */

        if (static_cast<size_t>(wend - wptr) < hdr_size + 1u) {
            flush(false);
        }

        if (fresh) {
            subindex.push_back({te.key, seq});
            fresh = false;
        }

        size_t remaining = bytes.size();
        const uint8_t* src = bytes.data();

        /* space for posting data in THIS block after the header */
        size_t data_space = static_cast<size_t>(wend - wptr) - hdr_size;
        size_t data_here  = std::min(remaining, data_space);
        bool   has_more   = (data_here < remaining);

        /* Write: key_len(2) key(kl) doc_freq(4) data_len(4) data(data_here) */
        std::memcpy(wptr, &kl,   2); wptr += 2;
        std::memcpy(wptr, te.key.c_str(), kl); wptr += kl;
        std::memcpy(wptr, &freq, 4); wptr += 4;
        uint32_t dl = static_cast<uint32_t>(data_here);
        std::memcpy(wptr, &dl,   4); wptr += 4;
        std::memcpy(wptr, src,   data_here); wptr += data_here;

        src       += data_here;
        remaining -= data_here;

        if (has_more) {
            flush(true);
            /* Write continuation blocks until all data is written. */
            while (remaining > 0) {
                /* Continuation block: starts with 0xFFFF marker. */
                std::memcpy(wptr, &CONT, 2); wptr += 2;
                size_t cont_space = static_cast<size_t>(wend - wptr);
                size_t cont_here  = std::min(remaining, cont_space);
                bool   more_cont  = (cont_here < remaining);

                std::memcpy(wptr, src, cont_here);
                wptr      += cont_here;
                src       += cont_here;
                remaining -= cont_here;

                flush(more_cont);
            }
        }
    }
    /* Flush the last (possibly partial) block. */
    if (!fresh || wptr != cur.IB_Data)
        flush(false);

    /* 3. Encode SubIndex section. ───────────────────────────────────────── */
    std::vector<uint8_t> subindex_buf;
    subindex_buf.reserve(subindex.size() * 16);
    {
        uint32_t n = static_cast<uint32_t>(subindex.size());
        subindex_buf.resize(4);
        std::memcpy(subindex_buf.data(), &n, 4);
        for (const auto& e : subindex) {
            uint16_t kl = static_cast<uint16_t>(e.term.size());
            size_t off  = subindex_buf.size();
            subindex_buf.resize(off + 2 + kl + 4);
            std::memcpy(subindex_buf.data() + off,         &kl,        2);
            std::memcpy(subindex_buf.data() + off + 2,     e.term.c_str(), kl);
            std::memcpy(subindex_buf.data() + off + 2 + kl, &e.block_seq, 4);
        }
    }

    /* 4. Encode DocData section. */
#pragma pack(push, 1)
    struct DocRec { uint64_t doc_id; float importance; uint32_t doc_len; };
#pragma pack(pop)
    static_assert(sizeof(DocRec) == 16, "");
    std::vector<DocRec> docdata;
    docdata.reserve(store.AllDocStats().size());
    for (const auto& [id, ds] : store.AllDocStats())
        docdata.push_back({id, ds.importance, ds.doc_len});

    /* 5. Compute section offsets.
     *    Header = 72 bytes (8+8+8+8+16+16+8 fields).  SubIndex follows. */
#pragma pack(push, 1)
    struct Hdr {
        uint8_t  magic[8];
        uint32_t version, reserved;
        uint64_t num_documents, num_terms;
        uint64_t subindex_off, subindex_size;
        uint64_t docdata_off,  docdata_size;
        uint64_t blocks_off,   num_blocks;
    };
#pragma pack(pop)
    constexpr uint64_t HDR_SIZE = sizeof(Hdr);
    uint64_t subindex_off  = HDR_SIZE;
    uint64_t subindex_size = subindex_buf.size();
    uint64_t docdata_off   = subindex_off + subindex_size;
    uint64_t docdata_size  = docdata.size() * sizeof(DocRec);

    /* Align blocks section to PAGE_SIZE boundary. */
    uint64_t raw_blocks_off = docdata_off + docdata_size;
    uint64_t blocks_off     = ((raw_blocks_off + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

    /* 6. Write file. */
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    Hdr hdr{};
    std::memcpy(hdr.magic, MAGIC, 8);
    hdr.version      = FORMAT_VERSION;
    hdr.num_documents = static_cast<uint64_t>(docdata.size());
    hdr.num_terms     = static_cast<uint64_t>(terms.size());
    hdr.subindex_off  = subindex_off;
    hdr.subindex_size = subindex_size;
    hdr.docdata_off   = docdata_off;
    hdr.docdata_size  = docdata_size;
    hdr.blocks_off    = blocks_off;
    hdr.num_blocks    = static_cast<uint64_t>(blocks.size());
    fwrite(&hdr, sizeof(hdr), 1, f);

    /* SubIndex. */
    fwrite(subindex_buf.data(), 1, subindex_buf.size(), f);

    /* DocData. */
    if (!docdata.empty())
        fwrite(docdata.data(), sizeof(DocRec), docdata.size(), f);

    /* Padding to blocks_off. */
    uint64_t cur_pos = file_pos(f);
    if (cur_pos < blocks_off) {
        std::vector<uint8_t> pad(static_cast<size_t>(blocks_off - cur_pos), 0);
        fwrite(pad.data(), 1, pad.size(), f);
    }

    /* IndexBlocks. */
    for (const auto& b : blocks)
        fwrite(&b.blk, sizeof(IndexBlock), 1, f);

    fclose(f);
    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Load
 * ─────────────────────────────────────────────────────────────────────────
 *
 * Reads Header, SubIndex, and DocData sections into memory.
 * Sets up FileBlockManager pointing at the Blocks section so cache misses
 * reload pages directly from the .idx file.
 * Sets m_Built = true on the context so Build() is not re-run.
 */
bool IndexSerializer::Load(PostingStore&              store,
                           const char*                path,
                           std::vector<SubIndexEntry>* subindex_out,
                           uint64_t*                   blocks_offset_out)
{
    if (!path || !*path) return false;

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    /* Read and validate header. */
#pragma pack(push, 1)
    struct Hdr {
        uint8_t  magic[8];
        uint32_t version, reserved;
        uint64_t num_documents, num_terms;
        uint64_t subindex_off, subindex_size;
        uint64_t docdata_off,  docdata_size;
        uint64_t blocks_off,   num_blocks;
    };
#pragma pack(pop)

    Hdr hdr{};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1
        || std::memcmp(hdr.magic, MAGIC, 8) != 0
        || hdr.version != FORMAT_VERSION)
    {
        fclose(f);
        return false;
    }

    /* SubIndex — load entries into subindex_out. */
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
                if (ptr + kl + 4 > end) break;
                std::string term(reinterpret_cast<const char*>(ptr), kl);
                ptr += kl;
                uint32_t bseq = 0;
                std::memcpy(&bseq, ptr, 4); ptr += 4;
                subindex_out->push_back({std::move(term), bseq});
            }
        }
    }

    /* Pass blocks base offset to caller. */
    if (blocks_offset_out)
        *blocks_offset_out = hdr.blocks_off;

    /* DocData — restore into PostingStore for BM25 scoring. */
    {
#pragma pack(push, 1)
        struct DocRec { uint64_t doc_id; float importance; uint32_t doc_len; };
#pragma pack(pop)
        size_t n = static_cast<size_t>(hdr.docdata_size / sizeof(DocRec));
        std::vector<DocRec> recs(n);
        fseek(f, static_cast<long>(hdr.docdata_off), SEEK_SET);
        if (n > 0) fread(recs.data(), sizeof(DocRec), n, f);
        for (const auto& r : recs) {
            store.AddDocTokens(r.doc_id, r.doc_len);
            store.SetDocImportance(r.doc_id, r.importance);
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
