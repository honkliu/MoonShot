#include "IndexSerializer.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

static const uint8_t  MAGIC[8]         = {'M','O','O','N','S','H','O','T'};
static const uint32_t FORMAT_VERSION   = 1;

/*
* File header — fixed 64 bytes.
*/
#pragma pack(push, 1)
struct FileHeader {
    uint8_t  magic[8];
    uint32_t version;
    uint32_t reserved;
    uint64_t num_documents;
    uint64_t num_terms;
    uint64_t docdata_offset;
    uint64_t termdict_offset;
    uint64_t postings_offset;
    uint64_t postings_size;
};
#pragma pack(pop)
static_assert(sizeof(FileHeader) == 64, "FileHeader size mismatch");

/*
* DocData record — fixed 16 bytes.
*/
#pragma pack(push, 1)
struct DocDataRecord {
    uint64_t doc_id;
    float    importance;
    uint32_t doc_len;
};
#pragma pack(pop)
static_assert(sizeof(DocDataRecord) == 16, "DocDataRecord size mismatch");

/*
* VarByte codec (local, not exported).
*
* Numbers are encoded with 7 payload bits per byte.
* The MSB = 1 means more bytes follow; MSB = 0 is the final byte.
*/
static void vb_encode(uint64_t v, std::vector<uint8_t>& buf)
{
    while (v >= 0x80u) {
        buf.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(v));
}

static uint64_t vb_decode(const uint8_t* data, size_t& pos)
{
    uint64_t result = 0;
    uint32_t shift  = 0;
    uint8_t  byte;
    do {
        byte    = data[pos++];
        result |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        shift  += 7;
    } while (byte & 0x80u);
    return result;
}

/*
* Helper: write a uint16_t in little-endian order.
*/
static void write_u16(FILE* f, uint16_t v)
{
    uint8_t buf[2] = { static_cast<uint8_t>(v & 0xFF),
                       static_cast<uint8_t>(v >> 8) };
    fwrite(buf, 1, 2, f);
}

static void write_u32(FILE* f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

static void write_u64(FILE* f, uint64_t v)
{
    fwrite(&v, 8, 1, f);
}

static uint16_t read_u16(FILE* f)
{
    uint8_t buf[2]; fread(buf, 1, 2, f);
    return static_cast<uint16_t>(buf[0]) |
           (static_cast<uint16_t>(buf[1]) << 8);
}

static uint32_t read_u32(FILE* f) { uint32_t v; fread(&v, 4, 1, f); return v; }
static uint64_t read_u64(FILE* f) { uint64_t v; fread(&v, 8, 1, f); return v; }
static float    read_f32(FILE* f) { float    v; fread(&v, 4, 1, f); return v; }

/* ============================================================
 * Save
 * ============================================================ */
bool IndexSerializer::Save(const PostingStore& store, const char* path)
{
    if (!path || !*path)
        return false;

    /* 1. Collect terms sorted alphabetically. */
    struct TermEntry {
        std::string        key;
        const PostingList* list;
        uint64_t           data_offset;
        uint32_t           data_len;
    };

    std::vector<TermEntry> terms;
    terms.reserve(store.AllPostings().size());

    for (const auto& [k, pl] : store.AllPostings())
        terms.push_back({k, &pl, 0u, 0u});

    std::sort(terms.begin(), terms.end(),
        [](const TermEntry& a, const TermEntry& b){ return a.key < b.key; });

    /* 2. Encode all posting lists into a single byte buffer.
     *    Each list uses delta-compressed (doc_id_delta, tf) pairs. */
    std::vector<uint8_t> posting_buf;
    posting_buf.reserve(store.AllPostings().size() * 10);

    for (auto& te : terms) {
        te.data_offset = static_cast<uint64_t>(posting_buf.size());
        uint64_t prev  = 0;

        for (const auto& e : te.list->entries) {
            vb_encode(e.doc_id - prev, posting_buf);
            vb_encode(e.tf,            posting_buf);
            prev = e.doc_id;
        }

        te.data_len = static_cast<uint32_t>(posting_buf.size() - te.data_offset);
    }

    /* 3. Compute term-dict byte size. */
    uint64_t termdict_bytes = 0;
    for (const auto& te : terms)
        termdict_bytes += 2u + te.key.size() + 4u + 8u + 4u;

    /* 4. Fill header. */
    const auto& doc_stats   = store.AllDocStats();
    uint64_t    num_docs    = static_cast<uint64_t>(doc_stats.size());
    uint64_t    docdata_off  = sizeof(FileHeader);
    uint64_t    termdict_off = docdata_off + num_docs * sizeof(DocDataRecord);
    uint64_t    postings_off = termdict_off + termdict_bytes;

    FileHeader hdr;
    memcpy(hdr.magic, MAGIC, 8);
    hdr.version          = FORMAT_VERSION;
    hdr.reserved         = 0;
    hdr.num_documents    = num_docs;
    hdr.num_terms        = static_cast<uint64_t>(terms.size());
    hdr.docdata_offset   = docdata_off;
    hdr.termdict_offset  = termdict_off;
    hdr.postings_offset  = postings_off;
    hdr.postings_size    = static_cast<uint64_t>(posting_buf.size());

    /* 5. Write file. */
    FILE* f = fopen(path, "wb");

    if (!f)
        return false;

    fwrite(&hdr, sizeof(hdr), 1, f);

    /* DocData section */
    for (const auto& [doc_id, ds] : doc_stats) {
        DocDataRecord rec;
        rec.doc_id     = doc_id;
        rec.importance = ds.importance;
        rec.doc_len    = ds.doc_len;
        fwrite(&rec, sizeof(rec), 1, f);
    }

    /* TermDict section */
    for (const auto& te : terms) {
        write_u16(f, static_cast<uint16_t>(te.key.size()));
        fwrite(te.key.data(), 1, te.key.size(), f);
        write_u32(f, te.list->doc_freq());
        write_u64(f, te.data_offset);
        write_u32(f, te.data_len);
    }

    /* Postings section */
    if (!posting_buf.empty())
        fwrite(posting_buf.data(), 1, posting_buf.size(), f);

    fclose(f);
    return true;
}

bool IndexSerializer::Load(PostingStore& store, const char* path)
{
    if (!path || !*path)
        return false;

    FILE* f = fopen(path, "rb");

    if (!f)
        return false;

    /* Read and validate header. */
    FileHeader hdr;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (memcmp(hdr.magic, MAGIC, 8) != 0) {
        fclose(f);
        return false;
    }

    if (hdr.version != FORMAT_VERSION) {
        fclose(f);
        return false;
    }

    /* DocData section. */
    fseek(f, static_cast<long>(hdr.docdata_offset), SEEK_SET);

    for (uint64_t i = 0; i < hdr.num_documents; ++i) {
        DocDataRecord rec;

        if (fread(&rec, sizeof(rec), 1, f) != 1)
            break;

        store.AddDocTokens(rec.doc_id, rec.doc_len);
        store.SetDocImportance(rec.doc_id, rec.importance);
    }

    /* Read entire postings data into a buffer for random access. */
    std::vector<uint8_t> post_buf(static_cast<size_t>(hdr.postings_size));

    if (hdr.postings_size > 0) {
        fseek(f, static_cast<long>(hdr.postings_offset), SEEK_SET);
        fread(post_buf.data(), 1, post_buf.size(), f);
    }

    /* TermDict section. */
    fseek(f, static_cast<long>(hdr.termdict_offset), SEEK_SET);

    for (uint64_t i = 0; i < hdr.num_terms; ++i) {
        uint16_t    key_len  = read_u16(f);
        std::string key(key_len, '\0');
        fread(&key[0], 1, key_len, f);
        uint32_t doc_freq = read_u32(f);
        uint64_t data_off = read_u64(f);
        uint32_t data_len = read_u32(f);

        /* Decode the posting list from the buffer. */
        size_t   pos  = static_cast<size_t>(data_off);
        size_t   end  = pos + data_len;
        uint64_t prev = 0;

        while (pos < end) {
            uint64_t delta = vb_decode(post_buf.data(), pos);
            uint64_t tf    = vb_decode(post_buf.data(), pos);
            prev += delta;
            store.AddPosting(key, prev, static_cast<uint32_t>(tf));
        }

        (void)doc_freq;
    }

    fclose(f);
    return true;
}

bool IndexSerializer::IsValidIndex(const char* path)
{
    if (!path || !*path)
        return false;

    FILE* f = fopen(path, "rb");

    if (!f)
        return false;

    uint8_t magic[8] = {};
    fread(magic, 1, 8, f);
    fclose(f);

    return memcmp(magic, MAGIC, 8) == 0;
}
