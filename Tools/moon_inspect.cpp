#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

static const uint8_t MAGIC[8] = {'M','O','O','N','S','H','O','T'};
static const uint32_t FORMAT_VERSION = 5;
static const int PAGE_SIZE = 4096;
static const int IB_DATA_OFF = 8 + 50 * 4;
static const int IB_DATA_LEN = PAGE_SIZE - IB_DATA_OFF;
static const uint64_t HAS_MORE_FLAG = (1ULL << 63);
static const uint16_t CONT_MARKER = 0xFFFFu;

#pragma pack(push, 1)
struct FileHdr {
    uint8_t magic[8];
    uint32_t version, reserved;
    uint64_t num_documents, num_terms;
    uint64_t subindex_off, subindex_size;
    uint64_t pageskip_off, pageskip_size;
    uint64_t docdata_off, docdata_size;
    uint64_t blocks_off, num_blocks;
};

struct DocRec {
    uint64_t doc_id;
    float importance;
    uint32_t doc_len;
    uint16_t path_len;
    uint8_t pad[6];
    char path[232];
};
#pragma pack(pop)

static_assert(sizeof(FileHdr) == 96);
static_assert(sizeof(DocRec) == 256);

struct TermDirectoryEntry {
    std::string first_term;
    uint32_t term_header_block_id = 0;
};

struct TermHeader {
    std::string term;
    uint32_t doc_freq = 0;
    uint32_t posting_block_id = 0;
    uint32_t posting_offset = 0;
    uint32_t posting_length = 0;
    uint32_t skip_list_offset = 0;
    uint32_t continuation_block_count = 0;
    uint32_t flags = 0;
};

struct TermHeaderBlock { std::vector<TermHeader> headers; };
struct Posting { uint64_t doc_id; uint32_t tf; };

struct TermView {
    const TermHeader* header = nullptr;
    std::vector<Posting> postings;
};

struct BlockView {
    uint32_t seq = 0;
    bool has_more = false;
    bool is_continuation = false;
    std::vector<TermView> terms;
};

struct Index {
    FileHdr hdr{};
    std::vector<uint8_t> bytes;
    std::vector<TermDirectoryEntry> directory;
    std::vector<TermHeaderBlock> header_blocks;
    std::vector<TermHeader> headers;
    std::vector<uint64_t> pageskip;
    std::vector<DocRec> docs;
    std::vector<BlockView> blocks;
    std::string error;
};

static std::string esc(const std::string& s)
{
    std::string out;
    for (char ch : s) {
        if (ch == '<') out += "&lt;";
        else if (ch == '>') out += "&gt;";
        else if (ch == '&') out += "&amp;";
        else if (ch == '"') out += "&quot;";
        else out += ch;
    }
    return out;
}

static uint16_t read_u16(const uint8_t*& ptr, const uint8_t* end)
{
    if (ptr + 2 > end) return 0;
    uint16_t value = 0;
    std::memcpy(&value, ptr, 2);
    ptr += 2;
    return value;
}

static uint32_t read_u32(const uint8_t*& ptr, const uint8_t* end)
{
    if (ptr + 4 > end) return 0;
    uint32_t value = 0;
    std::memcpy(&value, ptr, 4);
    ptr += 4;
    return value;
}

static std::string read_string(const uint8_t*& ptr, const uint8_t* end, size_t size)
{
    if (ptr + size > end) return {};
    std::string value(reinterpret_cast<const char*>(ptr), size);
    ptr += size;
    return value;
}

static uint64_t vb_read(const uint8_t* data, size_t size, size_t& pos)
{
    uint64_t value = 0;
    uint32_t shift = 0;
    while (pos < size) {
        uint8_t byte = data[pos++];
        value |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) break;
        shift += 7;
    }
    return value;
}

static std::vector<Posting> decode_postings(const uint8_t* data, size_t size)
{
    std::vector<Posting> postings;
    uint64_t previous = 0;
    size_t pos = 0;
    while (pos < size && postings.size() < 12) {
        uint64_t delta = vb_read(data, size, pos);
        if (pos >= size) break;
        uint64_t tf = vb_read(data, size, pos);
        previous += delta;
        postings.push_back({previous, static_cast<uint32_t>(tf)});
    }
    return postings;
}

static std::string doc_path(const DocRec& rec)
{
    size_t size = std::min<size_t>(rec.path_len, sizeof(rec.path));
    return std::string(rec.path, rec.path + size);
}

static Index parse_index(const char* path)
{
    Index index;
    FILE* file = std::fopen(path, "rb");
    if (!file) { index.error = "Cannot open file"; return index; }

    std::fseek(file, 0, SEEK_END);
    long file_size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (file_size < 0) { std::fclose(file); index.error = "Cannot stat file"; return index; }

    index.bytes.resize(static_cast<size_t>(file_size));
    std::fread(index.bytes.data(), 1, index.bytes.size(), file);
    std::fclose(file);

    const uint8_t* data = index.bytes.data();
    size_t size = index.bytes.size();
    if (size < sizeof(FileHdr)) { index.error = "File too small"; return index; }

    std::memcpy(&index.hdr, data, sizeof(FileHdr));
    if (std::memcmp(index.hdr.magic, MAGIC, 8) != 0) { index.error = "Bad magic"; return index; }
    if (index.hdr.version != FORMAT_VERSION) { index.error = "Unsupported version " + std::to_string(index.hdr.version); return index; }

    if (index.hdr.subindex_off + index.hdr.subindex_size <= size) {
        const uint8_t* ptr = data + index.hdr.subindex_off;
        const uint8_t* end = ptr + index.hdr.subindex_size;
        uint32_t dir_count = read_u32(ptr, end);
        index.directory.reserve(dir_count);
        for (uint32_t i = 0; i < dir_count && ptr < end; ++i) {
            uint16_t len = read_u16(ptr, end);
            std::string first = read_string(ptr, end, len);
            uint32_t block_id = read_u32(ptr, end);
            index.directory.push_back({std::move(first), block_id});
        }

        uint32_t block_count = read_u32(ptr, end);
        index.header_blocks.resize(block_count);
        for (uint32_t block_index = 0; block_index < block_count && ptr < end; ++block_index) {
            uint32_t entry_count = read_u32(ptr, end);
            index.header_blocks[block_index].headers.reserve(entry_count);
            for (uint32_t i = 0; i < entry_count && ptr < end; ++i) {
                TermHeader header;
                uint16_t len = read_u16(ptr, end);
                header.term = read_string(ptr, end, len);
                header.doc_freq = read_u32(ptr, end);
                header.posting_block_id = read_u32(ptr, end);
                header.posting_offset = read_u32(ptr, end);
                header.posting_length = read_u32(ptr, end);
                header.skip_list_offset = read_u32(ptr, end);
                header.continuation_block_count = read_u32(ptr, end);
                header.flags = read_u32(ptr, end);
                index.headers.push_back(header);
                index.header_blocks[block_index].headers.push_back(std::move(header));
            }
        }
    }

    if (index.hdr.pageskip_off + index.hdr.pageskip_size <= size) {
        size_t count = static_cast<size_t>(index.hdr.pageskip_size / sizeof(uint64_t));
        index.pageskip.resize(count);
        std::memcpy(index.pageskip.data(), data + index.hdr.pageskip_off, count * sizeof(uint64_t));
    }

    if (index.hdr.docdata_off + index.hdr.docdata_size <= size) {
        size_t count = static_cast<size_t>(index.hdr.docdata_size / sizeof(DocRec));
        index.docs.resize(count);
        std::memcpy(index.docs.data(), data + index.hdr.docdata_off, count * sizeof(DocRec));
    }

    std::unordered_map<uint32_t, std::vector<const TermHeader*>> terms_by_block;
    for (const auto& header : index.headers) {
        terms_by_block[header.posting_block_id].push_back(&header);
    }

    for (uint64_t seq = 0; seq < index.hdr.num_blocks; ++seq) {
        uint64_t block_offset = index.hdr.blocks_off + seq * PAGE_SIZE;
        if (block_offset + PAGE_SIZE > size) break;

        const uint8_t* block = data + block_offset;
        const uint8_t* ib_data = block + IB_DATA_OFF;
        uint64_t ib_header = 0;
        std::memcpy(&ib_header, block, 8);

        BlockView view;
        view.seq = static_cast<uint32_t>(seq);
        view.has_more = (ib_header & HAS_MORE_FLAG) != 0;
        uint16_t marker = 0;
        std::memcpy(&marker, ib_data, 2);
        view.is_continuation = marker == CONT_MARKER;

        auto found = terms_by_block.find(view.seq);
        if (found != terms_by_block.end()) {
            for (const TermHeader* header : found->second) {
                if (header->posting_offset >= IB_DATA_LEN) continue;
                size_t length = std::min<size_t>(header->posting_length, IB_DATA_LEN - header->posting_offset);
                TermView term;
                term.header = header;
                term.postings = decode_postings(ib_data + header->posting_offset, length);
                view.terms.push_back(std::move(term));
            }
        }
        index.blocks.push_back(std::move(view));
    }

    return index;
}

static std::string css()
{
    return R"css(
body{font:13px Segoe UI,sans-serif;background:#1e1e1e;color:#d4d4d4;margin:0}
h1{font-size:16px;background:#252526;margin:0;padding:10px 14px;border-bottom:1px solid #3e3e42}
.tabs{display:flex;gap:6px;background:#2d2d30;padding:7px 14px;border-bottom:1px solid #3e3e42;position:sticky;top:0;z-index:2}
.tab{background:#3c3c3c;color:#ddd;border:1px solid #555;border-radius:3px;padding:4px 10px;cursor:pointer}.tab.active{background:#007acc;color:#fff}
.panel{display:none;padding:12px}.panel.active{display:block}
table{border-collapse:collapse;width:100%;margin-bottom:12px}th{background:#252526;color:#9cdcfe;text-align:left;position:sticky;top:39px}td,th{border-bottom:1px solid #2a2a2a;padding:4px 8px}tr:hover td{background:#2a2d2e}
.mono{font-family:Cascadia Code,Consolas,monospace}.num{color:#b5cea8}.key{color:#9cdcfe}.path{color:#ce9178}.dim{color:#888}.posting{color:#4ec9b0;font-size:11px;margin:1px 0}.badge{display:inline-block;background:#555;color:#fff;border-radius:3px;padding:1px 5px;font-size:11px;margin-left:5px}.more{background:#ca5010}.cont{background:#5c8a3a}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:8px}.kv{background:#252526;border:1px solid #3e3e42;padding:8px}.kv b{display:block;color:#9cdcfe;font-size:11px;margin-bottom:3px}.card{border:1px solid #3e3e42;background:#252526;margin-bottom:10px}.card-h{padding:6px 9px;background:#2d2d30;cursor:pointer}.card-b{display:none;padding:8px}.card.open .card-b{display:block}
)css";
}

static void emit(std::ostream& out, const Index& index, const char* path)
{
    const FileHdr& h = index.hdr;
    out << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>moon_inspect</title><style>" << css() << "</style></head><body>";
    out << "<h1>moon_inspect <span class='dim'>" << esc(path) << "</span></h1>";
    out << "<div class='tabs'><button class='tab active' onclick=\"tab('header',this)\">Header</button><button class='tab' onclick=\"tab('terms',this)\">Term Headers</button><button class='tab' onclick=\"tab('docs',this)\">DocData</button><button class='tab' onclick=\"tab('blocks',this)\">Posting Blocks</button></div>";

    auto kv = [&](const char* name, const std::string& value) {
        out << "<div class='kv'><b>" << name << "</b><span class='mono'>" << value << "</span></div>";
    };

    out << "<div class='panel active' id='header'><div class='grid'>";
    kv("Version", std::to_string(h.version));
    kv("Documents", std::to_string(h.num_documents));
    kv("Terms", std::to_string(h.num_terms));
    kv("TermHeaderTable", std::format("0x{:016X} ({} B)", h.subindex_off, h.subindex_size));
    kv("PageSkipList", std::format("0x{:016X} ({} B)", h.pageskip_off, h.pageskip_size));
    kv("DocData", std::format("0x{:016X} ({} B)", h.docdata_off, h.docdata_size));
    kv("Posting blocks", std::format("0x{:016X} count={}", h.blocks_off, h.num_blocks));
    kv("File size", std::to_string(index.bytes.size()) + " B");
    out << "</div></div>";

    out << "<div class='panel' id='terms'><h3>Term Directory</h3><table><tr><th>HeaderBlk</th><th>First term</th></tr>";
    for (const auto& entry : index.directory) {
        out << "<tr><td class='num'>" << entry.term_header_block_id << "</td><td class='mono key'>" << esc(entry.first_term) << "</td></tr>";
    }
    out << "</table><h3>TermHeaderBlocks</h3>";
    for (size_t block_id = 0; block_id < index.header_blocks.size(); ++block_id) {
        const auto& block = index.header_blocks[block_id];
        out << "<div class='card open'><div class='card-h'>TermHeaderBlock <span class='num'>" << block_id << "</span> <span class='dim'>" << block.headers.size() << " headers</span></div><div class='card-b'>";
        out << "<table><tr><th>Term</th><th>df</th><th>PostingBlk</th><th>Off</th><th>Len</th><th>Cont</th><th>SkipOff</th></tr>";
        for (const auto& term : block.headers) {
            out << "<tr onclick=\"showBlock(" << term.posting_block_id << ")\"><td class='mono key'>" << esc(term.term) << "</td><td class='num'>" << term.doc_freq << "</td><td class='num'>" << term.posting_block_id << "</td><td class='num'>" << term.posting_offset << "</td><td class='num'>" << term.posting_length << "</td><td class='num'>" << term.continuation_block_count << "</td><td class='num'>" << term.skip_list_offset << "</td></tr>";
        }
        out << "</table></div></div>";
    }
    out << "</div>";

    out << "<div class='panel' id='docs'><table><tr><th>DocID</th><th>Importance</th><th>DocLen</th><th>Path</th></tr>";
    for (const auto& doc : index.docs) {
        out << "<tr><td class='mono num'>" << doc.doc_id << "</td><td class='num'>" << doc.importance << "</td><td class='num'>" << doc.doc_len << "</td><td class='path'>" << esc(doc_path(doc)) << "</td></tr>";
    }
    out << "</table></div>";

    out << "<div class='panel' id='blocks'>";
    for (const auto& block : index.blocks) {
        out << "<div class='card' id='blk" << block.seq << "'><div class='card-h' onclick='this.parentNode.classList.toggle(\"open\")'>PostingBlock <span class='num'>" << block.seq << "</span>";
        if (block.has_more) out << "<span class='badge more'>HAS_MORE</span>";
        if (block.is_continuation) out << "<span class='badge cont'>continuation</span>";
        out << "<span class='dim'> " << block.terms.size() << " starting terms</span></div><div class='card-b'>";
        if (!block.terms.empty()) {
            out << "<table><tr><th>Term</th><th>df</th><th>Off</th><th>Len</th><th>Sample postings</th></tr>";
            for (const auto& view : block.terms) {
                const TermHeader& term = *view.header;
                out << "<tr><td class='mono key'>" << esc(term.term);
                if (term.continuation_block_count > 0) out << "<span class='badge more'>->" << term.continuation_block_count << "</span>";
                out << "</td><td class='num'>" << term.doc_freq << "</td><td class='num'>" << term.posting_offset << "</td><td class='num'>" << term.posting_length << "</td><td>";
                for (const auto& posting : view.postings) out << "<div class='posting'>doc=" << posting.doc_id << " tf=" << posting.tf << "</div>";
                out << "</td></tr>";
            }
            out << "</table>";
        }
        out << "</div></div>";
    }
    out << "</div><script>function tab(id,el){document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));document.getElementById(id).classList.add('active');el.classList.add('active')}function showBlock(seq){document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));document.getElementById('blocks').classList.add('active');document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));document.querySelectorAll('.tab')[3].classList.add('active');const b=document.getElementById('blk'+seq);if(b){b.classList.add('open');b.scrollIntoView({behavior:'smooth',block:'start'});}}</script></body></html>";
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: moon_inspect <index.idx> [-o report.html]\n";
        return 2;
    }

    const char* index_path = argv[1];
    const char* output_path = nullptr;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "-o") output_path = argv[i + 1];
    }

    Index index = parse_index(index_path);
    if (!index.error.empty()) {
        std::cerr << "moon_inspect: " << index.error << "\n";
        return 1;
    }

    if (output_path) {
        std::ofstream output(output_path, std::ios::binary);
        emit(output, index, index_path);
    } else {
        emit(std::cout, index, index_path);
    }
    return 0;
}
