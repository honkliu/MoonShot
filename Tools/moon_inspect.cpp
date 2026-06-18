#include "../IndexAccess/BlockTable.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct IndexEntry { uint64_t IE_DocID; uint32_t IE_TermFrequency; };

static constexpr size_t INDEX_BLOCK_DATA_OFFSET = offsetof(IndexBlock, IB_Data);
static constexpr size_t INDEX_BLOCK_DATA_BYTES = sizeof(IndexBlock::IB_Data);

struct TermView {
    const LeafTermEntry* header = nullptr;
    std::vector<IndexEntry> IndexEntrys;
};

struct BlockView {
    uint32_t seq = 0;
    bool has_more = false;
    bool is_continuation = false;
    std::vector<TermView> terms;
};

struct Index {
    IndexFileHeader hdr{};
    std::vector<uint8_t> bytes;
    std::vector<HeadTermEntry> directory;
    std::vector<std::vector<LeafTermEntry>> leaf_pages;
    std::vector<LeafTermEntry> leaf_entries;
    std::vector<DocDataEntry> docs;
    std::vector<BlockView> blocks;
    std::string error;
};

using FilePtr = std::unique_ptr<FILE, decltype(&std::fclose)>;

static FilePtr open_file(const char* path, const char* mode)
{
    return FilePtr(std::fopen(path, mode), &std::fclose);
}

static bool seek_file(FILE* file, int64_t offset, int origin)
{
#if defined(_WIN32)
    return _fseeki64(file, offset, origin) == 0;
#else
    return std::fseek(file, static_cast<long>(offset), origin) == 0;
#endif
}

static int64_t file_offset(FILE* file)
{
#if defined(_WIN32)
    return _ftelli64(file);
#else
    return std::ftell(file);
#endif
}

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

static bool section_fits(size_t file_size, uint64_t offset, uint64_t count, size_t entry_size, size_t& byte_count)
{
    byte_count = 0;
    if (offset > file_size) return false;
    size_t remaining = file_size - static_cast<size_t>(offset);
    if (entry_size != 0 && count > remaining / entry_size) return false;
    byte_count = static_cast<size_t>(count) * entry_size;
    return true;
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

static std::vector<IndexEntry> decode_IndexEntries(const uint8_t* data, size_t size)
{
    std::vector<IndexEntry> entries;
    size_t pos = 0;
    while (pos < size && entries.size() < 12) {
        uint64_t docId = vb_read(data, size, pos);
        if (pos >= size) break;
        uint64_t tf = vb_read(data, size, pos);
        entries.push_back({docId, static_cast<uint32_t>(tf)});
    }
    return entries;
}

static std::string doc_path(const DocDataEntry& entry)
{
    if (entry.DDE_PathLength == 0 || entry.DDE_PathLength > DOC_PATH_MAX)
        return {};

    return std::string(reinterpret_cast<const char*>(entry.DDE_Path), entry.DDE_PathLength);
}

static Index parse_index(const char* path)
{
    Index index;
    FilePtr file = open_file(path, "rb");
    if (!file) {
        index.error = "Cannot open file";
        return index;
    }

    if (!seek_file(file.get(), 0, SEEK_END)) {
        index.error = "Cannot seek file";
        return index;
    }
    int64_t file_size = file_offset(file.get());
    if (file_size < 0) {
        index.error = "Cannot stat file";
        return index;
    }
    if (!seek_file(file.get(), 0, SEEK_SET)) {
        index.error = "Cannot rewind file";
        return index;
    }

    index.bytes.resize(static_cast<size_t>(file_size));
    size_t bytes_read = std::fread(index.bytes.data(), 1, index.bytes.size(), file.get());
    if (bytes_read != index.bytes.size()) {
        index.error = "Cannot read file";
        return index;
    }

    const uint8_t* data = index.bytes.data();
    size_t size = index.bytes.size();
    if (size < sizeof(IndexFileHeader)) {
        index.error = "File too small";
        return index;
    }

    std::memcpy(&index.hdr, data, sizeof(IndexFileHeader));
    if (std::memcmp(index.hdr.IFH_Magic, INDEX_FILE_MAGIC, 8) != 0) {
        index.error = "Bad magic";
        return index;
    }
    if (index.hdr.IFH_Version != INDEX_FORMAT_VERSION) {
        index.error = "Unsupported version " + std::to_string(index.hdr.IFH_Version);
        return index;
    }

    size_t section_bytes = 0;
    if (section_fits(size, index.hdr.IFH_HeadTermEntryOffset, index.hdr.IFH_HeadTermEntryCount, sizeof(HeadTermEntry), section_bytes)) {
        size_t count = static_cast<size_t>(index.hdr.IFH_HeadTermEntryCount);
        index.directory.resize(count);
        std::memcpy(index.directory.data(), data + index.hdr.IFH_HeadTermEntryOffset, section_bytes);
    }

    if (section_fits(size, index.hdr.IFH_LeafTermBlockOffset, index.hdr.IFH_LeafTermBlockCount, sizeof(LeafTermBlock), section_bytes)) {
        size_t block_count = static_cast<size_t>(index.hdr.IFH_LeafTermBlockCount);
        index.leaf_pages.resize(block_count);
        for (size_t block_index = 0; block_index < block_count; ++block_index) {
            const uint8_t* page_start = data + index.hdr.IFH_LeafTermBlockOffset + block_index * sizeof(LeafTermBlock);
            const uint8_t* page_end = page_start + PAGE_SIZE;
            const uint8_t* ptr = page_start;
            if (ptr + sizeof(uint32_t) > page_end) {
                index.error = "Corrupt leaf term page: missing entry count";
                return index;
            }
            uint32_t entry_count = read_u32(ptr, page_end);
            if (entry_count > LEAF_TERM_ENTRY_MAX) {
                index.error = "Corrupt leaf term page: too many entries";
                return index;
            }
            auto& page = index.leaf_pages[block_index];
            page.reserve(entry_count);
            for (uint32_t i = 0; i < entry_count; ++i) {
                if (ptr + sizeof(uint16_t) > page_end) {
                    index.error = "Corrupt leaf term page: missing term length";
                    return index;
                }
                LeafTermEntry header;
                uint16_t len = read_u16(ptr, page_end);
                if (len > HEAD_TERM_KEY_MAX || ptr + len + 6 * sizeof(uint32_t) > page_end) {
                    index.error = "Corrupt leaf term page: bad entry length";
                    return index;
                }
                header.LTE_Term = read_string(ptr, page_end, len);
                header.LTE_DocFreq = read_u32(ptr, page_end);
                header.LTE_IndexBlockID = read_u32(ptr, page_end);
                header.LTE_IndexOffset = read_u32(ptr, page_end);
                header.LTE_IndexLength = read_u32(ptr, page_end);
                header.LTE_ContinuationBlockCount = read_u32(ptr, page_end);
                header.LTE_Flags = read_u32(ptr, page_end);
                page.push_back(header);
                index.leaf_entries.push_back(std::move(header));
            }
            if (page.size() != entry_count) {
                index.error = "Corrupt leaf term page: truncated entries";
                return index;
            }
        }
    }

    if (section_fits(size, index.hdr.IFH_DocDataOffset, index.hdr.IFH_NumDocuments, sizeof(DocDataEntry), section_bytes)) {
        size_t count = static_cast<size_t>(index.hdr.IFH_NumDocuments);
        index.docs.resize(count);
        std::memcpy(index.docs.data(), data + index.hdr.IFH_DocDataOffset, section_bytes);
    }

    std::unordered_map<uint32_t, std::vector<const LeafTermEntry*>> terms_by_block;
    for (const auto& header : index.leaf_entries) {
        terms_by_block[header.LTE_IndexBlockID].push_back(&header);
    }

    const uint64_t indexBlockCount = index.hdr.IFH_IndexBlockCount;
    for (uint64_t seq = 0; seq < indexBlockCount; ++seq) {
        if (index.hdr.IFH_IndexBlockOffset > size) break;
        uint64_t remaining_blocks = (size - static_cast<size_t>(index.hdr.IFH_IndexBlockOffset)) / PAGE_SIZE;
        if (seq >= remaining_blocks) break;
        uint64_t block_offset = index.hdr.IFH_IndexBlockOffset + seq * PAGE_SIZE;

        const uint8_t* block = data + block_offset;
        const uint8_t* ib_data = block + INDEX_BLOCK_DATA_OFFSET;
        uint64_t ib_header = 0;
        std::memcpy(&ib_header, block, 8);

        BlockView view;
        view.seq = static_cast<uint32_t>(seq);
        view.has_more = (ib_header & IB_HEADER_HAS_MORE) != 0;
        uint16_t marker = 0;
        std::memcpy(&marker, ib_data, 2);
        view.is_continuation = marker == BLOCK_CONTINUATION_MARKER;

        auto found = terms_by_block.find(view.seq);
        if (found != terms_by_block.end()) {
            for (const LeafTermEntry* header : found->second) {
                if (header->LTE_IndexOffset >= INDEX_BLOCK_DATA_BYTES) continue;
                size_t length = std::min<size_t>(header->LTE_IndexLength, INDEX_BLOCK_DATA_BYTES - header->LTE_IndexOffset);
                TermView term;
                term.header = header;
                term.IndexEntrys = decode_IndexEntries(ib_data + header->LTE_IndexOffset, length);
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
.mono{font-family:Cascadia Code,Consolas,monospace}.num{color:#b5cea8}.key{color:#9cdcfe}.path{color:#ce9178}.dim{color:#888}.IndexEntry{color:#4ec9b0;font-size:11px;margin:1px 0}.badge{display:inline-block;background:#555;color:#fff;border-radius:3px;padding:1px 5px;font-size:11px;margin-left:5px}.more{background:#ca5010}.cont{background:#5c8a3a}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:8px}.kv{background:#252526;border:1px solid #3e3e42;padding:8px}.kv b{display:block;color:#9cdcfe;font-size:11px;margin-bottom:3px}.card{border:1px solid #3e3e42;background:#252526;margin-bottom:10px}.card-h{padding:6px 9px;background:#2d2d30;cursor:pointer}.card-b{display:none;padding:8px}.card.open .card-b{display:block}
)css";
}

static void emit(std::ostream& out, const Index& index, const char* path)
{
    const IndexFileHeader& h = index.hdr;
    std::unordered_map<uint64_t, const DocDataEntry*> docs_by_id;
    docs_by_id.reserve(index.docs.size());
    for (const auto& doc : index.docs) docs_by_id.emplace(doc.DDE_DocID, &doc);

    out << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>moon_inspect</title><style>" << css() << "</style></head><body>";
    out << "<h1>moon_inspect <span class='dim'>" << esc(path) << "</span></h1>";
    out << "<div class='tabs'><button class='tab active' onclick=\"tab('header',this)\">Header</button><button class='tab' onclick=\"tab('terms',this)\">Leaf Terms</button><button class='tab' onclick=\"tab('docs',this)\">DocData</button><button class='tab' onclick=\"tab('blocks',this)\">IndexEntry Blocks</button></div>";

    auto kv = [&](const char* name, const std::string& value) {
        out << "<div class='kv'><b>" << name << "</b><span class='mono'>" << value << "</span></div>";
    };

    out << "<div class='panel active' id='header'><div class='grid'>";
    kv("Version", std::to_string(h.IFH_Version));
    kv("Documents", std::to_string(h.IFH_NumDocuments));
    kv("Average DocLength", std::to_string(h.IFH_AvgDocLength));
    kv("Terms", std::to_string(h.IFH_NumTerms));
    kv("HeadTermEntry", std::format("0x{:016X} count={}", h.IFH_HeadTermEntryOffset, h.IFH_HeadTermEntryCount));
    kv("LeafTermBlock", std::format("0x{:016X} count={}", h.IFH_LeafTermBlockOffset, h.IFH_LeafTermBlockCount));
    kv("DocData", std::format("0x{:016X} count={}", h.IFH_DocDataOffset, h.IFH_NumDocuments));
    kv("IndexEntry blocks", std::format("0x{:016X} count={}", h.IFH_IndexBlockOffset, h.IFH_IndexBlockCount));
    kv("File size", std::to_string(index.bytes.size()) + " B");
    out << "</div></div>";

    out << "<div class='panel' id='terms'><h3>Term Directory</h3><table><tr><th>Leaf block</th><th>First term</th></tr>";
    for (const auto& entry : index.directory) {
        out << "<tr><td class='num'>" << entry.HTE_LeafTermBlockID << "</td><td class='mono key'>" << esc(std::string(entry.FirstTerm())) << "</td></tr>";
    }
    out << "</table><h3>LeafTermBlocks</h3>";
    for (size_t page_id = 0; page_id < index.leaf_pages.size(); ++page_id) {
        const auto& page = index.leaf_pages[page_id];
        out << "<div class='card open'><div class='card-h'>LeafTermBlock <span class='num'>" << page_id << "</span> <span class='dim'>" << page.size() << " entries</span></div><div class='card-b'>";
        out << "<table><tr><th>Term</th><th>df</th><th>IndexEntryBlk</th><th>Off</th><th>Len</th><th>Cont</th></tr>";
        for (const auto& term : page) {
            out << "<tr onclick=\"showBlock(" << term.LTE_IndexBlockID << ")\"><td class='mono key'>" << esc(term.LTE_Term) << "</td><td class='num'>" << term.LTE_DocFreq << "</td><td class='num'>" << term.LTE_IndexBlockID << "</td><td class='num'>" << term.LTE_IndexOffset << "</td><td class='num'>" << term.LTE_IndexLength << "</td><td class='num'>" << term.LTE_ContinuationBlockCount << "</td></tr>";
        }
        out << "</table></div></div>";
    }
    out << "</div>";

    out << "<div class='panel' id='docs'><table><tr><th>DocID</th><th>Importance</th><th>DocLen</th><th>Path</th></tr>";
    for (const auto& doc : index.docs) {
        out << "<tr><td class='mono num'>" << doc.DDE_DocID << "</td><td class='num'>" << doc.DDE_StaticRank << "</td><td class='num'>" << doc.DDE_DocLength << "</td><td class='path'>" << esc(doc_path(doc)) << "</td></tr>";
    }
    out << "</table></div>";

    out << "<div class='panel' id='blocks'>";
    for (const auto& block : index.blocks) {
        out << "<div class='card' id='blk" << block.seq << "'><div class='card-h' onclick='this.parentNode.classList.toggle(\"open\")'>IndexEntryBlock <span class='num'>" << block.seq << "</span>";
        if (block.has_more) out << "<span class='badge more'>HAS_MORE</span>";
        if (block.is_continuation) out << "<span class='badge cont'>continuation</span>";
        out << "<span class='dim'> " << block.terms.size() << " starting terms</span></div><div class='card-b'>";
        if (!block.terms.empty()) {
            out << "<table><tr><th>Term</th><th>df</th><th>Off</th><th>Len</th><th>Decoded postings</th></tr>";
            for (const auto& view : block.terms) {
                const LeafTermEntry& term = *view.header;
                out << "<tr><td class='mono key'>" << esc(term.LTE_Term);
                if (term.LTE_ContinuationBlockCount > 0) out << "<span class='badge more'>->" << term.LTE_ContinuationBlockCount << "</span>";
                out << "</td><td class='num'>" << term.LTE_DocFreq << "</td><td class='num'>" << term.LTE_IndexOffset << "</td><td class='num'>" << term.LTE_IndexLength << "</td><td class='dim'>" << view.IndexEntrys.size() << " sample rows</td></tr>";
                out << "<tr><td></td><td colspan='4'><table class='postings'><tr><th>DocID</th><th>TF</th><th>Path</th></tr>";
                for (const auto& IndexEntry : view.IndexEntrys) {
                    out << "<tr class='IndexEntry'><td class='mono num'>" << IndexEntry.IE_DocID << "</td><td class='num'>" << IndexEntry.IE_TermFrequency << "</td><td class='path'>";
                    auto doc = docs_by_id.find(IndexEntry.IE_DocID);
                    if (doc != docs_by_id.end()) out << esc(doc_path(*doc->second));
                    else out << "<span class='dim'>DocData not loaded for this DocID</span>";
                    out << "</td></tr>";
                }
                out << "</table></td></tr>";
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
