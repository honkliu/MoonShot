/*
 * moon — personal document search tool
 *
 * Usage:
 *   moon -name <filepath>   Index a .txt or .md file
 *   moon -i                 Interactive search mode
 *
 * Index stored at:
 *   Linux   : ~/moon.idx  (+ ~/moon.idx.meta)
 *   Windows : %USERPROFILE%\moon.idx
 */

#include "moonshot.h"
#include "IndexSerializer.h"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <chrono>
#include <queue>
#include <cstdio>

#ifdef _WIN32
#  include <windows.h>
#  include <conio.h>
#else
#  include <pwd.h>
#  include <unistd.h>
#  include <limits.h>
#  include <termios.h>
#endif

#ifdef _WIN32
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string WideToUtf8(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

static std::string HomeDir()
{
#ifdef _WIN32
    const char* p = getenv("USERPROFILE");
    return p ? p : "C:/Users/Default";
#else
    const char* p = getenv("HOME");
    if (p) return p;
    struct passwd* pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/tmp";
#endif
}

static char PathSep()
{
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

static char ReadSingleKey()
{
#ifdef _WIN32
    return static_cast<char>(_getch());
#else
    termios oldAttrs{};
    termios newAttrs{};
    if (tcgetattr(STDIN_FILENO, &oldAttrs) != 0) {
        char ch = 0;
        std::cin.get(ch);
        return ch;
    }
    newAttrs = oldAttrs;
    newAttrs.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newAttrs);
    char ch = 0;
    std::cin.get(ch);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldAttrs);
    return ch;
#endif
}

static std::string DefaultIdxPath()
{
    return HomeDir() + PathSep() + "moon.idx";
}

// ─── path→id map: loaded from existing .idx DocData ───────────────────────────

using PathMap = std::map<std::string, uint64_t>;  // filepath → sequential id
static constexpr uintmax_t MAX_INDEX_FILE_BYTES = 8ull * 1024ull * 1024ull;
static constexpr size_t DEFAULT_BATCH_SIZE = 1000;
static constexpr uintmax_t DEFAULT_BATCH_BYTES = 128ull * 1024ull * 1024ull;
static constexpr uint64_t DEFAULT_BATCH_TOKENS = 1000000ull;
static constexpr uint64_t DEFAULT_BATCH_POSTINGS = 750000ull;
static constexpr size_t DEFAULT_BATCH_UNIQUE_TERMS = 250000;

struct FileItem {
    std::string path;
    uintmax_t   size = 0;
};

static std::filesystem::path FsPathFromUtf8(const std::string& path);

static std::string ManifestPath(const std::string& idxPath)
{
    return idxPath + ".manifest";
}

static std::filesystem::path ShardDir(const std::string& idxPath)
{
    return FsPathFromUtf8(idxPath + ".shards");
}

static std::string ToUtf8Path(const std::filesystem::path& path)
{
#ifdef _WIN32
    return WideToUtf8(path.wstring().c_str());
#else
    return path.string();
#endif
}

static PathMap LoadPathMap(const std::string& idxPath, uint64_t& max_id)
{
    PathMap m;
    max_id = 0;
    if (!IndexSerializer::IsValidIndex(idxPath.c_str())) return m;

    PostingStore tmp;
    IndexSerializer::Load(tmp, idxPath.c_str(), nullptr, nullptr, nullptr);
    for (const auto& [id, ds] : tmp.AllDocStats()) {
        if (!ds.path.empty()) {
            m[ds.path] = id;
            max_id = std::max(max_id, id);
        }
    }
    return m;
}

// ─── file helpers ─────────────────────────────────────────────────────────────

static std::string ReadFile(const std::string& path)
{
#ifdef _WIN32
    std::ifstream f(Utf8ToWide(path));
#else
    std::ifstream f(path);
#endif
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string AbsolutePath(const std::string& path)
{
#ifdef _WIN32
    std::wstring input = Utf8ToWide(path);
    DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;

    std::wstring full(needed, L'\0');
    DWORD written = GetFullPathNameW(input.c_str(), needed, full.data(), nullptr);
    if (written == 0) return path;
    if (!full.empty() && full.back() == L'\0') full.pop_back();
    return WideToUtf8(full.c_str());
#else
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved))
        return resolved;
    if (!path.empty() && path[0] == '/')
        return path;

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        return std::string(cwd) + "/" + path;
    return path;
#endif
}

static std::filesystem::path FsPathFromUtf8(const std::string& path)
{
#ifdef _WIN32
    return std::filesystem::path(Utf8ToWide(path));
#else
    return std::filesystem::path(path);
#endif
}

static bool IsIndexableTextFile(const std::filesystem::path& path)
{
    std::wstring extw = path.extension().wstring();
    std::string ext;
#ifdef _WIN32
    ext = WideToUtf8(extw.c_str());
#else
    ext = path.extension().string();
#endif
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return ext == ".txt" || ext == ".md";
}

static bool ShouldSkipDirectory(const std::filesystem::path& path)
{
    std::wstring namew = path.filename().wstring();
    std::string name;
#ifdef _WIN32
    name = WideToUtf8(namew.c_str());
#else
    name = path.filename().string();
#endif
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    return name == ".git"
        || name == ".vs"
        || name == ".vscode"
        || name == ".tools"
        || name == "node_modules"
        || name == ".npm"
        || name == ".nuget"
        || name == "packages"
        || name == "build"
        || name == "build_debug"
        || name == "debug"
        || name == "release"
        || name == "target"
        || name == "cxcache";
}

static std::vector<FileItem> CollectIndexableFiles(const std::string& path)
{
    std::vector<FileItem> files;
    std::error_code ec;
    std::filesystem::path root = FsPathFromUtf8(path);

    if (std::filesystem::is_regular_file(root, ec)) {
        if (IsIndexableTextFile(root)) {
            uintmax_t size = std::filesystem::file_size(root, ec);
            if (!ec && size <= MAX_INDEX_FILE_BYTES)
                files.push_back({AbsolutePath(path), size});
        }
        return files;
    }

    if (!std::filesystem::is_directory(root, ec))
        return files;

    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_directory(ec)) {
            if (!ec && ShouldSkipDirectory(it->path()))
                it.disable_recursion_pending();
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
        if (!IsIndexableTextFile(it->path())) continue;
        uintmax_t size = it->file_size(ec);
        if (ec) { ec.clear(); continue; }
        if (size > MAX_INDEX_FILE_BYTES) continue;
#ifdef _WIN32
    files.push_back({AbsolutePath(WideToUtf8(it->path().wstring().c_str())), size});
#else
    files.push_back({AbsolutePath(it->path().string()), size});
#endif
    }
    return files;
}

// Returns the filename without extension — used as the Title stream.
static std::string Stem(const std::string& path)
{
    auto slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = name.rfind('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

// ─── index: rebuild from all known files ─────────────────────────────────────

static void Rebuild(const std::string& idxPath, const PathMap& pathMap)
{
    SmartTokenizer tok;
    IndexContext   ctx;
    auto           writer = ctx.GetWriter();
    uint64_t       kept = 0;
    uint64_t       skipped = 0;

    for (auto& [fp, id] : pathMap) {
        std::string content = ReadFile(fp);
        if (content.empty()) {
            std::cerr << "  skipping (unreadable): " << fp << "\n";
            ++skipped;
            continue;
        }
        writer->Write(tok.Tokenize(Stem(fp).c_str()), id, "Title");
        writer->Write(tok.Tokenize(content.c_str()),  id, "Body");
        // Path stored in DocData — no separate .meta file needed
        ctx.GetStore()->SetDocPath(id, fp);
        ++kept;
    }

    ctx.SaveIndex(idxPath.c_str());
    std::cout << "Rebuilt index with " << kept << " readable document(s)";
    if (skipped) std::cout << " (skipped " << skipped << ")";
    std::cout << "\n";
}

struct DocMeta {
    uint64_t    doc_id = 0;
    uint32_t    doc_len = 0;
    float       importance = 0.0f;
    std::string path;
};

static void write_u16(std::vector<uint8_t>& out, uint16_t v) { auto* p = reinterpret_cast<uint8_t*>(&v); out.insert(out.end(), p, p + 2); }
static void write_u32(std::vector<uint8_t>& out, uint32_t v) { auto* p = reinterpret_cast<uint8_t*>(&v); out.insert(out.end(), p, p + 4); }

static void vb_write(uint64_t v, std::vector<uint8_t>& out)
{
    while (v >= 0x80u) {
        out.push_back(static_cast<uint8_t>((v & 0x7fu) | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

static uint64_t decode_last_docid_bytes(const uint8_t* data, size_t len)
{
    size_t pos = 0;
    uint64_t prev = 0;
    while (pos < len) {
        uint64_t delta = 0; uint8_t shift = 0, b = 0;
        do { if (pos >= len) return prev; b = data[pos++]; delta |= uint64_t(b & 0x7fu) << shift; shift += 7; } while (b & 0x80u);
        uint64_t tf = 0; shift = 0;
        do { if (pos >= len) return prev; b = data[pos++]; tf |= uint64_t(b & 0x7fu) << shift; shift += 7; } while (b & 0x80u);
        prev += delta;
    }
    return prev;
}

static std::vector<uint8_t> EncodePostings(const std::vector<IndexEntry>& entries)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(entries.size() * 3);
    uint64_t prev = 0;
    for (const auto& e : entries) {
        vb_write(e.IE_DocID - prev, bytes);
        vb_write(e.IE_TermFrequency, bytes);
        prev = e.IE_DocID;
    }
    return bytes;
}

static void DumpRun(const PostingStore& store, const std::string& runPath)
{
    FILE* f = fopen(runPath.c_str(), "wb");
    if (!f) throw std::runtime_error("Cannot write run: " + runPath);
    std::vector<std::pair<const std::string*, const PostingList*>> terms;
    terms.reserve(store.AllPostings().size());
    for (const auto& [term, posting] : store.AllPostings()) terms.push_back({&term, &posting});
    std::sort(terms.begin(), terms.end(), [](auto& a, auto& b){ return *a.first < *b.first; });
    for (const auto& [term, posting] : terms) {
        uint32_t len = static_cast<uint32_t>(term->size());
        uint32_t count = static_cast<uint32_t>(posting->entries.size());
        fwrite(&len, 4, 1, f);
        fwrite(term->data(), 1, term->size(), f);
        fwrite(&count, 4, 1, f);
        if (count) fwrite(posting->entries.data(), sizeof(IndexEntry), count, f);
    }
    uint32_t zero = 0;
    fwrite(&zero, 4, 1, f);
    fclose(f);
}

class RunReader {
public:
    explicit RunReader(const std::string& path) : m_File(path, std::ios::binary) { readNext(); }
    bool valid() const { return m_Valid; }
    const std::string& term() const { return m_Term; }
    const std::vector<IndexEntry>& entries() const { return m_Entries; }
    void next() { readNext(); }
private:
    std::ifstream m_File;
    bool m_Valid = false;
    std::string m_Term;
    std::vector<IndexEntry> m_Entries;
    void readNext() {
        uint32_t len = 0;
        if (!m_File.read(reinterpret_cast<char*>(&len), 4) || len == 0) { m_Valid = false; return; }
        m_Term.assign(len, '\0');
        m_File.read(m_Term.data(), len);
        uint32_t count = 0;
        m_File.read(reinterpret_cast<char*>(&count), 4);
        m_Entries.resize(count);
        if (count) m_File.read(reinterpret_cast<char*>(m_Entries.data()), sizeof(IndexEntry) * count);
        m_Valid = true;
    }
};

struct HeadTermEntryOut { std::string HTE_FirstTerm; uint32_t HTE_LeafTermBlockID = 0; };

class HeadLeafTermTableWriter {
public:
    void add(const std::string& term,
             uint32_t docFreq,
             uint32_t indexBlockID,
             uint32_t indexOffset,
             uint32_t indexLength,
             uint32_t pageSkipOffset,
             uint32_t continuationBlockCount,
             uint32_t flags)
    {
        size_t entryBytes = 2u + term.size() + 7u * sizeof(uint32_t);
        if (m_Count > 0 && m_Group.size() + entryBytes > PAGE_SIZE) flush();

        if (m_Count == 0) {
            m_FirstTerm = term;
            m_Group.clear();
            write_u32(m_Group, 0); // entry_count placeholder
        }
        write_u16(m_Group, static_cast<uint16_t>(term.size()));
        m_Group.insert(m_Group.end(), term.begin(), term.end());
        write_u32(m_Group, docFreq);
        write_u32(m_Group, indexBlockID);
        write_u32(m_Group, indexOffset);
        write_u32(m_Group, indexLength);
        write_u32(m_Group, pageSkipOffset);
        write_u32(m_Group, continuationBlockCount);
        write_u32(m_Group, flags);
        ++m_Count;
    }

    std::vector<uint8_t> finish()
    {
        flush();
        std::vector<uint8_t> out;
        write_u32(out, static_cast<uint32_t>(m_HeadTermEntries.size()));
        for (const auto& d : m_HeadTermEntries) {
            write_u16(out, static_cast<uint16_t>(d.HTE_FirstTerm.size()));
            out.insert(out.end(), d.HTE_FirstTerm.begin(), d.HTE_FirstTerm.end());
            write_u32(out, d.HTE_LeafTermBlockID);
        }
        write_u32(out, m_LeafTermBlockCount);
        out.insert(out.end(), m_LeafTermPages.begin(), m_LeafTermPages.end());
        return out;
    }
private:
    std::vector<HeadTermEntryOut> m_HeadTermEntries;
    std::vector<uint8_t> m_LeafTermPages;
    std::vector<uint8_t> m_Group;
    std::string m_FirstTerm;
    uint32_t m_Count = 0;
    uint32_t m_LeafTermBlockCount = 0;
    void flush() {
        if (m_Count == 0) return;
        std::memcpy(m_Group.data(), &m_Count, 4);
        if (m_Group.size() < PAGE_SIZE) m_Group.resize(PAGE_SIZE, 0);
        m_HeadTermEntries.push_back({m_FirstTerm, m_LeafTermBlockCount++});
        m_LeafTermPages.insert(m_LeafTermPages.end(), m_Group.begin(), m_Group.end());
        m_Group.clear();
        m_FirstTerm.clear();
        m_Count = 0;
    }
};

static void SaveFromRuns(const std::string& idxPath,
                         const std::vector<std::string>& runPaths,
                         const std::vector<DocMeta>& docs)
{
    std::vector<std::unique_ptr<RunReader>> readers;
    for (const auto& run : runPaths) readers.push_back(std::make_unique<RunReader>(run));
    struct Item { std::string term; size_t reader = 0; };
    struct Greater { bool operator()(const Item& a, const Item& b) const { return a.term > b.term; } };
    std::priority_queue<Item, std::vector<Item>, Greater> pq;
    for (size_t i = 0; i < readers.size(); ++i)
        if (readers[i]->valid()) pq.push({readers[i]->term(), i});

    constexpr size_t DATA_CAP = sizeof(IndexBlock::IB_Data);
    std::vector<IndexBlock> blocks;
    std::vector<uint64_t> pageskip;
    pageskip.push_back(UINT64_MAX);
    HeadLeafTermTableWriter leafTermWriter;
    IndexBlock cur{};
    size_t wptr = 0;
    uint32_t seq = 0;
    uint64_t totalTerms = 0;

    auto flushBlock = [&](bool hasMore) {
        cur.IB_Header = seq;
        if (hasMore) cur.IB_Header |= IB_HEADER_HAS_MORE;
        blocks.push_back(cur);
        ++seq; cur = {}; wptr = 0;
    };

    while (!pq.empty()) {
        std::string term = pq.top().term;
        std::vector<IndexEntry> merged;
        while (!pq.empty() && pq.top().term == term) {
            size_t idx = pq.top().reader;
            pq.pop();
            const auto& entries = readers[idx]->entries();
            merged.insert(merged.end(), entries.begin(), entries.end());
            readers[idx]->next();
            if (readers[idx]->valid()) pq.push({readers[idx]->term(), idx});
        }
        std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b){ return a.IE_DocID < b.IE_DocID; });
        std::vector<IndexEntry> compact;
        for (const auto& e : merged) {
            if (!compact.empty() && compact.back().IE_DocID == e.IE_DocID) compact.back().IE_TermFrequency += e.IE_TermFrequency;
            else compact.push_back(e);
        }
        if (compact.empty()) continue;

        auto bytes = EncodePostings(compact);
        if (bytes.empty()) continue;
        if (wptr >= DATA_CAP) flushBlock(false);

        uint32_t indexBlockID = seq;
        uint32_t indexOffset = static_cast<uint32_t>(wptr);
        size_t src = 0;
        size_t first = std::min(bytes.size(), DATA_CAP - wptr);
        std::memcpy(cur.IB_Data + wptr, bytes.data(), first);
        wptr += first; src += first;
        uint32_t indexLength = static_cast<uint32_t>(first);
        uint32_t pageSkipOffset = 0;
        uint32_t contCount = 0;
        if (src < bytes.size()) {
            flushBlock(true);
            pageSkipOffset = static_cast<uint32_t>(pageskip.size());
            pageskip.push_back(0);
            while (src < bytes.size()) {
                constexpr size_t CONT_HDR = 4;
                size_t take = std::min(bytes.size() - src, DATA_CAP - CONT_HDR);
                bool more = take < bytes.size() - src;
                uint64_t baseDoc = decode_last_docid_bytes(bytes.data(), src);
                pageskip.push_back(baseDoc);
                uint16_t marker = BLOCK_CONTINUATION_MARKER;
                uint16_t len = static_cast<uint16_t>(take);
                std::memcpy(cur.IB_Data + wptr, &marker, 2); wptr += 2;
                std::memcpy(cur.IB_Data + wptr, &len, 2); wptr += 2;
                std::memcpy(cur.IB_Data + wptr, bytes.data() + src, take); wptr += take;
                src += take;
                ++contCount;
                if (more) flushBlock(true);
            }
            pageskip.push_back(UINT64_MAX);
        }
        leafTermWriter.add(term, static_cast<uint32_t>(compact.size()), indexBlockID, indexOffset, indexLength, pageSkipOffset, contCount, 0);
        ++totalTerms;
    }
    if (wptr > 0) flushBlock(false);

    auto si = leafTermWriter.finish();
    std::vector<uint8_t> ps(pageskip.size() * 8);
    if (!pageskip.empty()) std::memcpy(ps.data(), pageskip.data(), ps.size());

    constexpr size_t DOC_REC = 1024;
    constexpr size_t DOC_PATH = 1000;
    std::vector<uint8_t> docdata(docs.size() * DOC_REC, 0);
    for (size_t i = 0; i < docs.size(); ++i) {
        uint8_t* rec = docdata.data() + i * DOC_REC;
        std::memcpy(rec, &docs[i].doc_id, 8);
        std::memcpy(rec + 8, &docs[i].importance, 4);
        std::memcpy(rec + 12, &docs[i].doc_len, 4);
        uint16_t plen = static_cast<uint16_t>(std::min(docs[i].path.size(), DOC_PATH - 1));
        std::memcpy(rec + 16, &plen, 2);
        if (plen) std::memcpy(rec + 24, docs[i].path.data(), plen);
    }

#pragma pack(push,1)
    struct Hdr {
        uint8_t magic[8]; uint32_t version, reserved;
        uint64_t num_documents, num_terms;
        uint64_t subindex_off, subindex_size;
        uint64_t pageskip_off, pageskip_size;
        uint64_t docdata_off, docdata_size;
        uint64_t blocks_off, num_blocks;
    };
#pragma pack(pop)
    static const uint8_t MAGIC[8] = {'M','O','O','N','S','H','O','T'};
    uint64_t hdrSize = sizeof(Hdr);
    uint64_t siOff = hdrSize, siSize = si.size();
    uint64_t psOff = siOff + siSize, psSize = ps.size();
    uint64_t ddOff = psOff + psSize, ddSize = docdata.size();
    uint64_t rawBlocks = ddOff + ddSize;
    uint64_t blkOff = ((rawBlocks + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

    FILE* f = fopen(idxPath.c_str(), "wb");
    if (!f) throw std::runtime_error("Cannot write index: " + idxPath);
    Hdr hdr{};
    std::memcpy(hdr.magic, MAGIC, 8);
    hdr.version = 7;
    hdr.num_documents = docs.size();
    hdr.num_terms = totalTerms;
    hdr.subindex_off = siOff; hdr.subindex_size = siSize;
    hdr.pageskip_off = psOff; hdr.pageskip_size = psSize;
    hdr.docdata_off = ddOff; hdr.docdata_size = ddSize;
    hdr.blocks_off = blkOff; hdr.num_blocks = blocks.size();
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(si.data(), 1, si.size(), f);
    fwrite(ps.data(), 1, ps.size(), f);
    if (!docdata.empty()) fwrite(docdata.data(), 1, docdata.size(), f);
    uint64_t pos = static_cast<uint64_t>(ftell(f));
    if (pos < blkOff) {
        std::vector<uint8_t> pad(static_cast<size_t>(blkOff - pos), 0);
        fwrite(pad.data(), 1, pad.size(), f);
    }
    if (!blocks.empty()) fwrite(blocks.data(), sizeof(IndexBlock), blocks.size(), f);
    fclose(f);
}

// ─── search ──────────────────────────────────────────────────────────────────

static void Search(IndexContext& ctx, const std::string& query)
{
    IndexSearchCompiler compiler;
    auto* tree = compiler.Compile(query.c_str(), "AUTB");

    if (!tree || tree->IsEmpty()) {
        delete tree;
        std::cout << "(no results)\n";
        return;
    }

    auto reader  = ctx.GetReader(tree);
    auto results = ctx.GetExecutor()->Execute(reader, 0);
    delete tree;

    if (results.empty()) {
        std::cout << "(no results)\n";
        return;
    }

    constexpr size_t PAGE_SIZE_RESULTS = 20;
    std::cout << results.size() << " result(s)\n";
    for (size_t offset = 0; offset < results.size(); offset += PAGE_SIZE_RESULTS) {
        const size_t end = std::min(offset + PAGE_SIZE_RESULTS, results.size());
        if (results.size() > PAGE_SIZE_RESULTS) {
            std::cout << "-- showing " << (offset + 1) << "-" << end
                      << " of " << results.size() << " --\n";
        }

        for (size_t i = offset; i < end; ++i) {
            const auto& r = results[i];
            const std::string& path = ctx.GetStore()->GetDocPath(r.doc_id);
            std::cout << (path.empty() ? "[unknown]" : path) << "\n";
        }

        if (end < results.size()) {
            std::cout << "-- press any key for next page, q to stop --" << std::flush;
            char ch = ReadSingleKey();
            std::cout << "\n";
            if (ch == 'q' || ch == 'Q') break;
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

static void PrintHelp(const std::string& idxPath)
{
    std::cout
        << "moon — personal document search\n\n"
        << "  moon -name <file>   Index a .txt or .md file\n"
        << "  moon -i             Interactive search\n\n"
        << "Index: " << idxPath << "\n";
}

int main(int argc, char* argv[])
{
    std::string idxPath = DefaultIdxPath();

    // On Windows, argv uses the ANSI codepage and drops non-ASCII characters.
    // Use CommandLineToArgvW to get the real Unicode arguments instead.
    std::string cmd, filePath;
    size_t batchSize = DEFAULT_BATCH_SIZE;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        if (wargc >= 2) cmd      = WideToUtf8(wargv[1]);
        if (wargc >= 3) filePath = WideToUtf8(wargv[2]);
        for (int i = 3; i + 1 < wargc; ++i) {
            std::string arg = WideToUtf8(wargv[i]);
            if (arg == "-batch") batchSize = std::stoull(WideToUtf8(wargv[++i]));
        }
        LocalFree(wargv);
    }
#else
    if (argc >= 2) cmd      = argv[1];
    if (argc >= 3) filePath = argv[2];
    for (int i = 3; i + 1 < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-batch") batchSize = std::stoull(argv[++i]);
    }
#endif

    if (cmd.empty()) {
        PrintHelp(idxPath);
        return 0;
    }

    // ── index a file ──────────────────────────────────────────────────────────
    if (cmd == "-name") {
        if (filePath.empty()) {
            std::cerr << "Usage: moon -name <filepath>\n";
            return 1;
        }
        auto files = CollectIndexableFiles(filePath);
        if (files.empty()) {
            std::cerr << "No readable .txt/.md files found: " << filePath << "\n";
            return 1;
        }

        std::cout << "Collected " << files.size() << " .txt/.md file(s) <= "
                  << (MAX_INDEX_FILE_BYTES / (1024 * 1024)) << "MB under "
                  << filePath << "\n";

        if (files.size() > batchSize) {
            std::error_code ec;
            std::filesystem::remove(FsPathFromUtf8(ManifestPath(idxPath)), ec);
            std::filesystem::remove_all(ShardDir(idxPath), ec);

            std::filesystem::path runDir = FsPathFromUtf8(idxPath + ".runs");
            std::filesystem::remove_all(runDir, ec);
            std::filesystem::create_directories(runDir, ec);

            std::vector<std::string> runs;
            std::vector<DocMeta> docs;
            uint64_t nextDocId = 1;
            size_t batch = 0;
            std::vector<FileItem> pending;
            uintmax_t pendingBytes = 0;

            auto flushPending = [&]() {
                if (pending.empty()) return;
                SmartTokenizer tok;
                PostingStore store;
                uint64_t readable = 0;
                uint64_t runFiles = 0;
                uintmax_t runBytes = 0;
                auto dumpCurrentRun = [&]() {
                    if (readable == 0) return;
                    std::filesystem::path runPath = runDir / ("run_" + std::to_string(batch) + ".bin");
                    std::string run = ToUtf8Path(runPath);
                    DumpRun(store, run);
                    runs.push_back(run);
                    std::cout << "Wrote run " << batch << " with " << readable << " doc(s) from "
                              << runFiles << " file(s), " << (runBytes / (1024 * 1024)) << "MB input, "
                              << store.TotalTerms() << " tokens, "
                              << store.TotalPostingEntries() << " postings, "
                              << store.UniqueTermCount() << " terms\n";
                    ++batch;
                    store = PostingStore{};
                    readable = 0;
                    runFiles = 0;
                    runBytes = 0;
                };
                for (const auto& item : pending) {
                    std::string content = ReadFile(item.path);
                    if (content.empty()) continue;
                    uint64_t docId = nextDocId++;
                    auto title = tok.Tokenize(Stem(item.path).c_str());
                    auto body = tok.Tokenize(content.c_str());
                    AdvancedIndexWriter writer(std::shared_ptr<PostingStore>(&store, [](PostingStore*){}));
                    writer.Write(std::move(title), docId, "Title");
                    writer.Write(std::move(body), docId, "Body");
                    store.SetDocPath(docId, item.path);
                    docs.push_back({docId, store.GetDocLen(docId), 0.0f, item.path});
                    ++readable;
                    ++runFiles;
                    runBytes += item.size;
                    if (store.TotalTerms() >= DEFAULT_BATCH_TOKENS ||
                        store.TotalPostingEntries() >= DEFAULT_BATCH_POSTINGS ||
                        store.UniqueTermCount() >= DEFAULT_BATCH_UNIQUE_TERMS) {
                        dumpCurrentRun();
                    }
                }
                dumpCurrentRun();
                pending.clear();
                pendingBytes = 0;
            };

            for (const auto& file : files) {
                pending.push_back(file);
                pendingBytes += file.size;
                if (pending.size() >= batchSize || pendingBytes >= DEFAULT_BATCH_BYTES) {
                    flushPending();
                }
            }
            flushPending();
            SaveFromRuns(idxPath, runs, docs);
            std::filesystem::remove_all(runDir, ec);
            std::cout << "Indexed input: " << filePath << "\n"
                      << "Files:   " << files.size() << "\n"
                      << "Batch:   " << batchSize << "\n"
                      << "BatchBytes: " << (DEFAULT_BATCH_BYTES / (1024 * 1024)) << "MB\n"
                      << "BatchTokens: " << DEFAULT_BATCH_TOKENS << "\n"
                      << "BatchPostings: " << DEFAULT_BATCH_POSTINGS << "\n"
                      << "BatchUniqueTerms: " << DEFAULT_BATCH_UNIQUE_TERMS << "\n"
                      << "Index:   " << idxPath << "\n";
        } else {
            std::error_code ec;
            std::filesystem::remove(FsPathFromUtf8(ManifestPath(idxPath)), ec);
            uint64_t max_id = 0;
            PathMap  pathMap = LoadPathMap(idxPath, max_id);

            uint64_t added = 0, existing = 0;
            for (const auto& file : files) {
                if (pathMap.count(file.path)) {
                    ++existing;
                } else {
                    pathMap[file.path] = ++max_id;
                    ++added;
                }
            }

            Rebuild(idxPath, pathMap);

            std::cout << "Indexed input: " << filePath << "\n"
                      << "Files:   " << files.size()
                      << " (new " << added << ", existing " << existing << ")\n"
                      << "Total:   " << pathMap.size()
                      << " document(s) in " << idxPath << "\n";
        }

    // ── interactive search ────────────────────────────────────────────────────
    } else if (cmd == "-i") {
        auto loadStart = std::chrono::steady_clock::now();
        IndexContext ctx("", idxPath.c_str());
        auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loadStart).count();

        std::cout << "moon search — "
                  << ctx.GetStore()->AllDocStats().size()
                  << " document(s)"
                  << " (loaded in " << loadMs << " ms)\n"
                  << "Type a query, or 'quit' to exit.\n";

        std::string line;
        while (true) {
            std::cout << "> ";
            std::cout.flush();
            if (!std::getline(std::cin, line)) break;
            if (line == "quit" || line == "exit" || line == "q") break;
            if (line.empty()) continue;
            Search(ctx, line);
        }

    } else {
        std::cerr << "Unknown option: " << cmd << "\n";
        PrintHelp(idxPath);
        return 1;
    }

    return 0;
}
