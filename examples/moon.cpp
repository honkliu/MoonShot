/*
 * moon — personal document search tool
 *
 * Usage:
 *   moon -idx <index> -file <filepath>       Index one file into <index>
 *   moon -idx <index> -dir <directory> -r    Index files recursively into <index>
 *   moon -idx <index> -i                     Search <index> with inverted index
 *   moon -idx <index> -v                     Search <index> with vector index
 *   moon -idx <index> -i -v                  Search <index> with both
 *   moon -file <filepath>                    Index one file
 *   moon -dir <directory> -ext md,txt        Index matching files in one directory
 *   moon -dir <directory> -ext md -r         Index matching files recursively
 *   moon -sample-merge -dir <root> -out <index-path> [-ext cpp,h,rs]
 *
 * Index stored at:
 *   Linux   : ~/moon.idx  (+ ~/moon.idx.meta)
 *   Windows : %USERPROFILE%\moon.idx
 */

#include "moonshot.h"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>

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

static std::string DeltaIndexPath(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return path.substr(0, dot) + ".delta" + path.substr(dot);
    return path + ".delta.idx";
}

static std::string BatchIndexPath(const std::string& path)
{
    return path + ".batch.tmp";
}

// ─── path→id map: loaded from existing .idx DocData ───────────────────────────

using PathMap = std::map<std::string, uint64_t>;  // filepath → sequential id
static constexpr uintmax_t MAX_INDEX_FILE_BYTES = 8ull * 1024ull * 1024ull;

struct FileItem {
    std::string path;
    uintmax_t   size = 0;
};

static std::filesystem::path FsPathFromUtf8(const std::string& path);

static PathMap LoadPathMapFromIndex(const std::string& idxPath, uint64_t& nextId)
{
    PathMap m;
    if (!IndexSerializer::IsValidIndex(idxPath.c_str()))
        return m;

    IndexContext ctx("", idxPath.c_str(), false);
    for (uint64_t id = 0; id < ctx.DocumentCount(); ++id) {
        const std::string path = ctx.GetDocPath(id);
        if (!path.empty()) {
            m[path] = id;
            nextId = std::max(nextId, id + 1);
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

static std::string PathToUtf8(const std::filesystem::path& path)
{
#ifdef _WIN32
    return WideToUtf8(path.wstring().c_str());
#else
    return path.string();
#endif
}

static std::string Trim(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

static std::string NormalizeExtension(std::string ext)
{
    ext = Trim(std::move(ext));
    while (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return ext;
}

static std::vector<std::string> ParseExtensions(const std::string& text)
{
    std::vector<std::string> extensions;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = NormalizeExtension(std::move(item));
        if (!item.empty())
            extensions.push_back(std::move(item));
    }
    return extensions;
}

static bool IsIndexableTextFile(const std::filesystem::path& path,
                                const std::vector<std::string>& extensions)
{
    std::wstring extw = path.extension().wstring();
    std::string ext;
#ifdef _WIN32
    ext = WideToUtf8(extw.c_str());
#else
    ext = path.extension().string();
#endif
    ext = NormalizeExtension(std::move(ext));
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

static void AddFileIfIndexable(const std::filesystem::path& path,
                               const std::vector<std::string>& extensions,
                               bool checkExtension,
                               std::vector<FileItem>& files)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return;
    if (checkExtension && !IsIndexableTextFile(path, extensions)) return;
    uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > MAX_INDEX_FILE_BYTES) return;
    files.push_back({AbsolutePath(PathToUtf8(path)), size});
}

static std::vector<FileItem> CollectSingleFile(const std::string& path)
{
    std::vector<FileItem> files;
    AddFileIfIndexable(FsPathFromUtf8(path), {}, false, files);
    return files;
}

static std::vector<FileItem> CollectDirectoryFiles(const std::string& path,
                                                   const std::vector<std::string>& extensions,
                                                   bool recursive)
{
    std::vector<FileItem> files;
    std::error_code ec;
    std::filesystem::path root = FsPathFromUtf8(path);
    if (!std::filesystem::is_directory(root, ec))
        return files;

    if (!recursive) {
        std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            AddFileIfIndexable(it->path(), extensions, true, files);
        }
        return files;
    }

    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_directory(ec) || ec) { ec.clear(); continue; }
        AddFileIfIndexable(it->path(), extensions, true, files);
    }
    return files;
}

static std::vector<FileItem> CollectSampleMergeFiles(const std::string& path,
                                                     const std::vector<std::string>& extensions)
{
    auto files = CollectDirectoryFiles(path, extensions, true);
    std::sort(files.begin(), files.end(), [](const FileItem& left, const FileItem& right) {
        return left.path < right.path;
    });
    return files;
}

// Returns the filename without extension, with common filename separators
// converted to spaces so rnr_alpaca_10k indexes title token "alpaca".
static std::string Stem(const std::string& path)
{
    auto slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = name.rfind('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    for (char& ch : name) {
        if (ch == '_' || ch == '-')
            ch = ' ';
    }
    return name;
}

// ─── index: build one batch delta ─────────────────────────────────────────────

static bool BuildIndexFile(const std::string& savePath,
                           const PathMap& pathMap,
                           uint64_t& kept,
                           uint64_t& skipped,
                           std::set<std::string>* reportedSkippedPaths = nullptr)
{
    IndexContext   ctx("", "", false);
    kept = 0;
    skipped = 0;
    const uint64_t total = static_cast<uint64_t>(pathMap.size());
    uint64_t processed = 0;

    for (auto& [fp, id] : pathMap) {
        ++processed;
        std::string content = ReadFile(fp);
        if (content.empty()) {
            if (!reportedSkippedPaths || reportedSkippedPaths->insert(fp).second)
                std::cerr << "  skipping (empty/unreadable): " << fp << "\n";
            ++skipped;
            continue;
        }
        Document doc;
        doc.doc_id = id;
        doc.path = fp;
        doc.title = Stem(fp);
        doc.body = std::move(content);
        ctx.AddDocument(doc);
        ++kept;

        if (processed == total || processed % 25 == 0) {
            std::cout << "  indexed " << processed << "/" << total
                      << " file(s) into memory\n";
        }
    }

    std::cout << "  writing index: " << savePath << "\n";
    auto saveStart = std::chrono::steady_clock::now();
    const bool saved = kept > 0 && ctx.SaveIndex(savePath.c_str());
    const auto saveMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - saveStart).count();
    std::cout << "  wrote index in " << saveMs << " ms\n";
    return saved;
}

static bool AddFileDocument(IndexContext& ctx,
                            const FileItem& file,
                            uint64_t docId,
                            uint64_t& kept,
                            uint64_t& skipped,
                            std::string* firstContent)
{
    std::string content = ReadFile(file.path);
    if (content.empty()) {
        std::cerr << "  skipping (empty/unreadable): " << file.path << "\n";
        ++skipped;
        return false;
    }

    if (firstContent && firstContent->empty())
        *firstContent = content;

    Document doc;
    doc.doc_id = docId;
    doc.path = file.path;
    doc.title = Stem(file.path);
    doc.body = std::move(content);
    ctx.AddDocument(doc);
    ++kept;
    return true;
}

// ─── search ──────────────────────────────────────────────────────────────────

struct SearchHit {
    const IndexContext* context = nullptr;
    SearchResult result;
};

struct SearchOptions {
    bool inverted = false;
    bool vector = false;
};

static uint64_t CountStoredDocuments(IndexContext& ctx, uint64_t firstDocId);

static std::string SearchStreamSet(const SearchOptions& options)
{
    std::string streams;
    if (options.inverted) streams += "AUTB";
    if (options.vector) streams += "V";
    return streams;
}

static const char* SearchModeName(const SearchOptions& options)
{
    if (options.inverted && options.vector) return "inverted+vector";
    return options.inverted ? "inverted" : "vector";
}

static std::string SourceMaskText(uint8_t sourceMask)
{
    std::string text = "-----";
    if (sourceMask & READER_SOURCE_ANCHOR) text[0] = 'A';
    if (sourceMask & READER_SOURCE_URL) text[1] = 'U';
    if (sourceMask & READER_SOURCE_TITLE) text[2] = 'T';
    if (sourceMask & READER_SOURCE_BODY) text[3] = 'B';
    if (sourceMask & READER_SOURCE_VECTOR) text[4] = 'V';
    return text;
}

static void CollectSearchResults(IndexContext& ctx,
                                 const std::string& query,
                                 const SearchOptions& options,
                                 std::vector<SearchHit>& hits)
{
    if (ctx.DocumentCount() == 0 && !ctx.HasDelta())
        return;

    const std::string streams = SearchStreamSet(options);
    if (streams.empty())
        return;

    std::unique_ptr<IndexSearchExecutor> executor(ctx.GetExecutor());
    for (const auto& result : executor->Execute(ctx.GetReader(query.c_str(), streams.c_str()), 0))
        hits.push_back({&ctx, result});
}

static void Search(IndexContext& ctx, const std::string& query, const SearchOptions& options)
{
    std::vector<SearchHit> results;
    CollectSearchResults(ctx, query, options, results);

    std::sort(results.begin(), results.end(), [](const SearchHit& left, const SearchHit& right) {
        if (left.result.score != right.result.score)
            return left.result.score > right.result.score;
        return ReaderDocumentIDValue(left.result.doc_id) < ReaderDocumentIDValue(right.result.doc_id);
    });

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
            const auto& hit = results[i];
            const std::string path = hit.context->GetDocPath(hit.result.doc_id);
            std::cout << SourceMaskText(ReaderDocumentIDSourceMask(hit.result.doc_id)) << " "
                      << (path.empty() ? "[unknown]" : path) << "\n";
        }

        if (end < results.size()) {
            std::cout << "-- press any key for next page, q to stop --" << std::flush;
            char ch = ReadSingleKey();
            std::cout << "\n";
            if (ch == 'q' || ch == 'Q') break;
        }
    }
}

static void WarmVectorGraph(IndexContext& ctx)
{
    auto vectorStart = std::chrono::steady_clock::now();
    std::cout << "Building vector graph..." << std::flush;
    ctx.Build();
    size_t vectorCount = ctx.VectorCount();
    if (auto* delta = ctx.GetDeltaContext()) {
        delta->Build();
        vectorCount += delta->VectorCount();
    }
    const auto vectorMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - vectorStart).count();
    std::cout << " " << vectorCount << " vector(s) in " << vectorMs << " ms\n";
}

static std::vector<std::string> SplitCommandLine(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string item;
    while (in >> item)
        out.push_back(std::move(item));
    return out;
}

static void PrintInteractiveHelp()
{
    std::cout
        << "Commands:\n"
        << "  /h                         Show this help\n"
        << "  /q                         Quit\n"
        << "  /a <file>                  Add one document to memory\n"
        << "  /a <dir> -e md,txt [-r]    Add documents from a directory\n"
        << "  /s                         Save pending additions as delta and publish it\n"
        << "  /m                         Merge delta into the main index and reload\n"
        << "Queries:\n"
        << "  anything else              Search current base + delta\n";
}

static uint64_t NextInteractiveDocId(IndexContext& ctx)
{
    uint64_t nextDocId = ctx.AllocateDocumentID();
    if (auto* delta = ctx.GetDeltaContext())
        nextDocId = std::max(nextDocId, delta->DocumentCount());
    return nextDocId;
}

static bool AddInteractiveFiles(IndexContext& ctx,
                                const std::vector<FileItem>& files,
                                uint64_t& kept,
                                uint64_t& skipped)
{
    uint64_t nextDocId = NextInteractiveDocId(ctx);
    std::string firstContent;
    for (const auto& file : files) {
        if (AddFileDocument(ctx, file, nextDocId, kept, skipped, &firstContent))
            ++nextDocId;
    }
    return kept > 0;
}

static bool HandleAddCommand(IndexContext& ctx, const std::vector<std::string>& args)
{
    if (args.size() < 2) {
        std::cout << "usage: /a <file> | /a <dir> -e md,txt [-r]\n";
        return true;
    }

    const std::string path = args[1];
    std::vector<std::string> extensions = ParseExtensions("md,txt");
    bool recursive = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "-e" && i + 1 < args.size()) {
            extensions = ParseExtensions(args[++i]);
        } else if (args[i] == "-r") {
            recursive = true;
        } else {
            std::cout << "unknown /a option: " << args[i] << "\n";
            return true;
        }
    }

    std::vector<FileItem> files;
    std::error_code ec;
    const auto fsPath = FsPathFromUtf8(path);
    if (std::filesystem::is_directory(fsPath, ec))
        files = CollectDirectoryFiles(path, extensions, recursive);
    else
        files = CollectSingleFile(path);

    if (files.empty()) {
        std::cout << "no readable indexable files found: " << path << "\n";
        return true;
    }

    uint64_t kept = 0;
    uint64_t skipped = 0;
    AddInteractiveFiles(ctx, files, kept, skipped);
    std::cout << "added " << kept << " document(s) to memory";
    if (skipped) std::cout << " (skipped " << skipped << ")";
    std::cout << "; run /s to publish as delta\n";
    return true;
}

static bool SaveInteractiveDelta(IndexContext& ctx, const std::string& idxPath)
{
    if (!IndexSerializer::IsValidIndex(idxPath.c_str())) {
        if (!ctx.SaveIndex(idxPath.c_str())) {
            std::cout << "failed to save index: " << idxPath << "\n";
            return false;
        }
        ctx.LoadIndex(idxPath.c_str());
        std::cout << "saved and loaded index: " << idxPath << "\n";
        return true;
    }

    const std::string deltaPath = DeltaIndexPath(idxPath);
    if (!ctx.SaveIndex(deltaPath.c_str())) {
        std::cout << "failed to save delta: " << deltaPath << "\n";
        return false;
    }
    std::cout << "saved and published delta: " << deltaPath << "\n";
    return true;
}

static bool MergeInteractiveDelta(IndexContext& ctx, const std::string& idxPath)
{
    if (!ctx.HasDelta()) {
        std::cout << "no delta loaded\n";
        return true;
    }
    if (!ctx.Merge(idxPath.c_str())) {
        std::cout << "merge failed: " << idxPath << "\n";
        return false;
    }
    const std::string deltaPath = DeltaIndexPath(idxPath);
    std::error_code ec;
    std::filesystem::remove(FsPathFromUtf8(deltaPath), ec);
    ctx.LoadIndex(idxPath.c_str());
    std::cout << "merged delta into main index and reloaded: " << idxPath << "\n";
    return true;
}

static bool HandleInteractiveCommand(IndexContext& ctx,
                                     const std::string& idxPath,
                                     const std::string& line,
                                     bool& shouldQuit)
{
    const auto args = SplitCommandLine(line);
    if (args.empty()) return true;
    if (args[0] == "/q") { shouldQuit = true; return true; }
    if (args[0] == "/h") { PrintInteractiveHelp(); return true; }
    if (args[0] == "/a") return HandleAddCommand(ctx, args);
    if (args[0] == "/s") return SaveInteractiveDelta(ctx, idxPath);
    if (args[0] == "/m") return MergeInteractiveDelta(ctx, idxPath);
    std::cout << "unknown command: " << args[0] << " (try /h)\n";
    return true;
}

static int RunInteractiveSearch(const std::string& idxPath, const SearchOptions& options)
{
    auto loadStart = std::chrono::steady_clock::now();
    IndexContext ctx("", idxPath.c_str());
    const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - loadStart).count();

    uint64_t totalDocuments = CountStoredDocuments(ctx, 0);
    if (auto* delta = ctx.GetDeltaContext())
        totalDocuments += CountStoredDocuments(*delta, ctx.DocumentCount());

    std::cout << "moon search — "
              << totalDocuments
              << " document(s)"
              << " (loaded in " << loadMs << " ms)\n"
              << "Mode: " << SearchModeName(options) << "\n"
              << std::flush;

    if (options.vector)
        WarmVectorGraph(ctx);

    std::cout << "Type a query, or /h for commands.\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        std::cout.flush();
        if (!std::getline(std::cin, line)) break;
        line = Trim(std::move(line));
        if (line.empty()) continue;
        if (!line.empty() && line[0] == '/') {
            bool shouldQuit = false;
            HandleInteractiveCommand(ctx, idxPath, line, shouldQuit);
            if (shouldQuit) break;
            continue;
        }
        Search(ctx, line, options);
    }

    return 0;
}

// ─── main ─────────────────────────────────────────────────────────────────────

struct IndexOptions {
    std::string filePath;
    std::string dirPath;
    std::vector<std::string> extensions = ParseExtensions("md,txt");
    uint64_t batchSize = 10000;
    bool recursive = false;
};

struct SampleMergeOptions {
    std::string dirPath;
    std::string outPath;
    std::vector<std::string> extensions = ParseExtensions("cpp,h,rs");
};

struct BeirBuildOptions {
    std::string dataPath;
    uint64_t limit = 0;
};

struct BeirEvalOptions {
    std::string dataPath;
    std::string qrels = "test";
    std::string streams = "TB";
    std::string mode = "weakand";
    std::vector<int> at = {10, 100, 1000};
    uint64_t limit = 0;
    bool noMphf = false;
    uint64_t leafCacheMb = 0;
    bool leafCacheMatchMphf = false;
};

static bool ParseBatchSize(const std::string& text, uint64_t& batchSize)
{
    if (text.empty()) return false;
    char* end = nullptr;
    unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (!end || *end != '\0' || value == 0) return false;
    batchSize = static_cast<uint64_t>(value);
    return true;
}

static bool ParseUInt64(const std::string& text, uint64_t& value)
{
    if (text.empty()) return false;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

static bool ParseAtList(const std::string& text, std::vector<int>& values)
{
    values.clear();
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = Trim(std::move(item));
        if (item.empty()) continue;
        char* end = nullptr;
        long parsed = std::strtol(item.c_str(), &end, 10);
        if (!end || *end != '\0' || parsed <= 0) return false;
        values.push_back(static_cast<int>(parsed));
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return !values.empty();
}

static bool ParseSearchOptions(const std::vector<std::string>& args,
                               SearchOptions& options,
                               std::string& error)
{
    for (const auto& arg : args) {
        if (arg == "-i") {
            options.inverted = true;
        } else if (arg == "-v") {
            options.vector = true;
        } else {
            error = "Unknown search option: " + arg;
            return false;
        }
    }

    if (!options.inverted && !options.vector) {
        error = "Usage: moon [-idx <index>] -i [-v] | moon [-idx <index>] -v";
        return false;
    }

    return true;
}

static bool IsIndexCommand(const std::string& arg)
{
    return arg == "-file"
        || arg == "-dir"
        || arg == "-ext"
        || arg == "-b"
        || arg == "-r";
}

static bool ParseIndexOptions(const std::vector<std::string>& args,
                              IndexOptions& options,
                              std::string& error)
{
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-file") {
            if (i + 1 >= args.size()) { error = "Usage: moon -file <filename>"; return false; }
            options.filePath = args[++i];
        } else if (arg == "-dir") {
            if (i + 1 >= args.size()) { error = "Usage: moon -dir <directory> [-ext md,txt] [-r]"; return false; }
            options.dirPath = args[++i];
        } else if (arg == "-ext") {
            if (i + 1 >= args.size()) { error = "Usage: moon -ext md,txt,cpp,h"; return false; }
            options.extensions = ParseExtensions(args[++i]);
            if (options.extensions.empty()) { error = "-ext must include at least one extension"; return false; }
        } else if (arg == "-b") {
            if (i + 1 >= args.size()) { error = "Usage: moon -b 10000"; return false; }
            if (!ParseBatchSize(args[++i], options.batchSize)) { error = "-b must be a positive integer"; return false; }
            if (options.batchSize < 10000) { error = "-b must be at least 10000 for indexing performance"; return false; }
        } else if (arg == "-r") {
            options.recursive = true;
        } else {
            error = "Unknown option: " + arg;
            return false;
        }
    }

    if (!options.filePath.empty() && !options.dirPath.empty()) {
        error = "Use either -file or -dir, not both";
        return false;
    }
    if (!options.filePath.empty() || !options.dirPath.empty())
        return true;
    error = "Usage: moon -file <filename> | moon -dir <directory> [-ext md,txt] [-r] [-b 10000]";
    return false;
}

static bool ParseSampleMergeOptions(const std::vector<std::string>& args,
                                    SampleMergeOptions& options,
                                    std::string& error)
{
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-dir") {
            if (i + 1 >= args.size()) { error = "Usage: moon -sample-merge -dir <root> -out <index-path> [-ext cpp,h,rs]"; return false; }
            options.dirPath = args[++i];
        } else if (arg == "-out") {
            if (i + 1 >= args.size()) { error = "Usage: moon -sample-merge -dir <root> -out <index-path> [-ext cpp,h,rs]"; return false; }
            options.outPath = args[++i];
        } else if (arg == "-ext") {
            if (i + 1 >= args.size()) { error = "Usage: moon -sample-merge -dir <root> -out <index-path> [-ext cpp,h,rs]"; return false; }
            options.extensions = ParseExtensions(args[++i]);
            if (options.extensions.empty()) { error = "-ext must include at least one extension"; return false; }
        } else {
            error = "Unknown sample merge option: " + arg;
            return false;
        }
    }

    if (options.dirPath.empty() || options.outPath.empty()) {
        error = "Usage: moon -sample-merge -dir <root> -out <index-path> [-ext cpp,h,rs]";
        return false;
    }
    return true;
}

static bool ParseBeirBuildOptions(const std::vector<std::string>& args,
                                  BeirBuildOptions& options,
                                  std::string& error)
{
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-data") {
            if (i + 1 >= args.size()) { error = "Usage: moon [-idx <index>] -beir-build -data <beir-dir> [-limit N]"; return false; }
            options.dataPath = args[++i];
        } else if (arg == "-limit") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-build -limit N"; return false; }
            if (!ParseUInt64(args[++i], options.limit)) { error = "-limit must be a non-negative integer"; return false; }
        } else {
            error = "Unknown BEIR build option: " + arg;
            return false;
        }
    }

    if (options.dataPath.empty()) {
        error = "Usage: moon [-idx <index>] -beir-build -data <beir-dir> [-limit N]";
        return false;
    }
    return true;
}

static bool ParseBeirEvalOptions(const std::vector<std::string>& args,
                                 BeirEvalOptions& options,
                                 std::string& error)
{
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-data") {
            if (i + 1 >= args.size()) { error = "Usage: moon [-idx <index>] -beir-eval -data <beir-dir> [-qrels test] [-k 10,100,1000]"; return false; }
            options.dataPath = args[++i];
        } else if (arg == "-qrels") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -qrels <split-or-path>"; return false; }
            options.qrels = args[++i];
        } else if (arg == "-k") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -k 10,100,1000"; return false; }
            if (!ParseAtList(args[++i], options.at)) { error = "-k must be a comma-separated list of positive integers"; return false; }
        } else if (arg == "-streams") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -streams TB"; return false; }
            options.streams = args[++i];
        } else if (arg == "-mode") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -mode bow|weakand|compile"; return false; }
            options.mode = args[++i];
            if (options.mode != "bow" && options.mode != "weakand" && options.mode != "compile") { error = "-mode must be bow, weakand, or compile"; return false; }
        } else if (arg == "-no-mphf") {
            options.noMphf = true;
        } else if (arg == "-leaf-cache-mb") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -leaf-cache-mb N"; return false; }
            if (!ParseUInt64(args[++i], options.leafCacheMb) || options.leafCacheMb == 0) { error = "-leaf-cache-mb must be a positive integer"; return false; }
        } else if (arg == "-leaf-cache-match-mphf") {
            options.leafCacheMatchMphf = true;
        } else if (arg == "-limit") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -limit N"; return false; }
            if (!ParseUInt64(args[++i], options.limit)) { error = "-limit must be a non-negative integer"; return false; }
        } else {
            error = "Unknown BEIR eval option: " + arg;
            return false;
        }
    }

    if (options.dataPath.empty()) {
        error = "Usage: moon [-idx <index>] -beir-eval -data <beir-dir> [-qrels test] [-k 10,100,1000]";
        return false;
    }
    if (options.streams.empty()) {
        error = "-streams must not be empty";
        return false;
    }
    return true;
}

static std::string JoinExtensions(const std::vector<std::string>& extensions)
{
    std::string text;
    for (size_t i = 0; i < extensions.size(); ++i) {
        if (i) text += ",";
        text += extensions[i];
    }
    return text;
}

static void PrintHelp(const std::string& idxPath)
{
    std::cout
        << "moon — MoonShot command-line sample\n\n"
        << "Global option:\n"
        << "  -idx <index-path>                  Use this index instead of the default\n"
        << "                                     Default: " << idxPath << "\n\n"
        << "Build or update an index:\n"
        << "  moon [-idx <index>] -file <file>\n"
        << "      Index one file.\n\n"
        << "  moon [-idx <index>] -dir <dir> -ext md,txt [-r] [-b 10000]\n"
        << "      Index files under <dir>. Use -r for recursive traversal.\n"
        << "      -ext is a comma-separated extension list without or with dots.\n"
        << "      -b controls how many new files are saved per delta batch; minimum 10000.\n\n"
        << "Search an index:\n"
        << "  moon [-idx <index>] -i\n"
        << "      Open an interactive inverted-index search prompt.\n\n"
        << "  moon [-idx <index>] -v\n"
        << "      Open an interactive vector search prompt.\n\n"
        << "  moon [-idx <index>] -i -v\n"
        << "      Open an interactive hybrid search prompt. Prefixes use AUTBV mask:\n"
        << "      letters mark matched sources, '-' marks sources not matched.\n\n"
        << "Base/delta/merge sample:\n"
        << "  moon -sample-merge -dir <root> -out <merged-index> [-ext cpp,h,rs]\n"
        << "      Recursively indexes matching files, builds a base index and delta\n"
        << "      index, merges them, reloads the merged output, and runs a sanity\n"
        << "      query. This is the simplest end-to-end MoonShot merge sample.\n\n"
        << "BEIR recall evaluation:\n"
        << "  moon [-idx <index>] -beir-build -data <beir-dir> [-limit N]\n"
        << "      Build an index from BEIR corpus.jsonl. Stored doc paths are BEIR ids.\n\n"
        << "  moon [-idx <index>] -beir-eval -data <beir-dir> [-qrels test] [-k 10,100,1000] [-streams TB] [-mode bow|weakand|compile] [-no-mphf] [-leaf-cache-mb N] [-leaf-cache-match-mphf] [-limit N]\n"
        << "      Evaluate Recall@k from BEIR queries.jsonl and qrels/<split>.tsv.\n"
        << "      Default mode is weakand. bow is kept as a recall-ceiling baseline.\n\n"
        << "Examples:\n"
        << "  moon -idx .\\notes.idx -dir .\\docs -ext md,txt -r\n"
        << "  moon -idx .\\notes.idx -i\n"
        << "  moon -idx .\\notes.idx -v\n"
        << "  moon -idx .\\notes.idx -i -v\n"
        << "  moon -sample-merge -dir . -out .\\build\\moonshot-source-merge.idx -ext cpp,h,rs\n"
        << "  moon -idx .\\build\\beir-scifact.idx -beir-build -data .\\data\\scifact\n"
        << "  moon -idx .\\build\\beir-scifact.idx -beir-eval -data .\\data\\scifact -qrels test -k 10,100,1000\n";
}

static bool ApplyGlobalOptions(std::vector<std::string>& args,
                               std::string& idxPath,
                               std::string& error)
{
    std::vector<std::string> filtered;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-idx") {
            if (i + 1 >= args.size()) {
                error = "Usage: moon -idx <index-path> <command>";
                return false;
            }
            idxPath = AbsolutePath(args[++i]);
        } else {
            filtered.push_back(args[i]);
        }
    }
    args = std::move(filtered);
    return true;
}

static std::string FirstUsableQueryToken(const std::string& text)
{
    SmartTokenizer tokenizer;
    auto tokens = tokenizer.Tokenize(text.c_str());
    for (const auto& token : tokens) {
        if (!token.empty())
            return token;
    }
    return {};
}

static bool HasSearchResults(IndexContext& ctx, const std::string& query)
{
    if (query.empty() || ctx.DocumentCount() == 0)
        return false;

    std::unique_ptr<EvalTree> tree(ctx.Compile(query.c_str(), "AUTBV"));
    if (!tree || tree->IsEmpty())
        return false;

    std::unique_ptr<IndexSearchExecutor> exec(ctx.GetExecutor());
    auto results = exec->Execute(ctx.GetReader(tree.get()), 5);
    return !results.empty();
}

static uint64_t CountStoredDocuments(IndexContext& ctx, uint64_t firstDocId = 0)
{
    uint64_t count = 0;
    for (uint64_t docId = firstDocId; docId < ctx.DocumentCount(); ++docId) {
        if (!ctx.GetDocPath(docId).empty())
            ++count;
    }
    return count;
}

static std::string BeirFilePath(const std::string& dataPath, const std::string& relativePath)
{
    auto path = FsPathFromUtf8(dataPath);
    path /= FsPathFromUtf8(relativePath);
    return PathToUtf8(path);
}

static int HexDigit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

static void AppendUtf8(uint32_t codepoint, std::string& out)
{
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

static bool ExtractJsonString(const std::string& line, const std::string& key, std::string& value)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = line.find(needle);
    if (keyPos == std::string::npos) return false;
    const size_t colon = line.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return false;
    size_t pos = colon + 1;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos >= line.size() || line[pos] != '"') return false;
    ++pos;

    value.clear();
    while (pos < line.size()) {
        char ch = line[pos++];
        if (ch == '"') return true;
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (pos >= line.size()) return false;
        const char esc = line[pos++];
        switch (esc) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
            if (pos + 4 > line.size()) return false;
            uint32_t codepoint = 0;
            for (int i = 0; i < 4; ++i) {
                const int digit = HexDigit(line[pos + i]);
                if (digit < 0) return false;
                codepoint = (codepoint << 4) | static_cast<uint32_t>(digit);
            }
            pos += 4;
            AppendUtf8(codepoint, value);
            break;
        }
        default:
            value.push_back(esc);
            break;
        }
    }
    return false;
}

using BeirQrels = std::unordered_map<std::string, std::unordered_set<std::string>>;

static bool LoadBeirQrels(const std::string& path, BeirQrels& qrels)
{
    std::ifstream in(FsPathFromUtf8(path));
    if (!in) return false;
    std::string line;
    bool firstLine = true;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (firstLine) {
            firstLine = false;
            if (line.find("query-id") != std::string::npos || line.find("corpus-id") != std::string::npos)
                continue;
        }
        std::vector<std::string> columns;
        std::stringstream ss(line);
        std::string column;
        while (std::getline(ss, column, '\t'))
            columns.push_back(std::move(column));
        if (columns.size() < 3) continue;
        char* end = nullptr;
        const double score = std::strtod(columns[2].c_str(), &end);
        if (!end || end == columns[2].c_str() || score <= 0.0) continue;
        qrels[columns[0]].insert(columns[1]);
    }
    return true;
}

static bool IsBeirStopword(const std::string& token)
{
    static const std::unordered_set<std::string> stopwords = {
        "a", "an", "and", "are", "as", "at", "be", "been", "by", "for", "from", "has", "have", "in",
        "into", "is", "it", "its", "of", "on", "or", "that", "the", "their", "there", "these", "this",
        "to", "was", "were", "with", "without", "can", "could", "may", "might", "must", "should", "than",
        "then", "which", "while", "during", "between", "within", "using", "used", "use"
    };
    return stopwords.count(token) != 0;
}

static std::vector<std::string> ParseBeirStreams(const std::string& streamSet)
{
    std::vector<std::string> streams;
    for (char ch : streamSet) {
        switch (ch) {
        case 'A': streams.emplace_back("A"); break;
        case 'U': streams.emplace_back("U"); break;
        case 'T': streams.emplace_back("T"); break;
        case 'B': streams.emplace_back("B"); break;
        case 'M': streams.emplace_back("M"); break;
        default: break;
        }
    }
    if (streams.empty())
        streams.emplace_back("T");
    return streams;
}

static std::shared_ptr<IndexReader> BuildBeirBowReader(IndexContext& ctx,
                                                       SmartTokenizer& tokenizer,
                                                       const std::string& query,
                                                       const std::string& streamSet)
{
    const auto streams = ParseBeirStreams(streamSet);
    const auto tokens = tokenizer.Tokenize(query.c_str());
    std::set<std::string> streamKeys;
    for (const auto& token : tokens) {
        if (token.size() <= 1 || IsBeirStopword(token))
            continue;
        for (const auto& stream : streams)
            streamKeys.insert(token + stream);
    }

    if (streamKeys.empty()) {
        for (const auto& token : tokens) {
            if (token.empty()) continue;
            for (const auto& stream : streams)
                streamKeys.insert(token + stream);
        }
    }
    if (streamKeys.empty())
        return nullptr;

    auto orNode = std::make_shared<OrNode>();
    for (const auto& key : streamKeys)
        orNode->children.push_back(std::make_shared<TermNode>(key, 1));

    EvalTree tree;
    tree.root = std::move(orNode);
    return ctx.GetReader(&tree);
}

static std::string ResolveBeirQrelsPath(const BeirEvalOptions& options)
{
    if (std::filesystem::is_regular_file(FsPathFromUtf8(options.qrels)))
        return options.qrels;
    return BeirFilePath(options.dataPath, "qrels/" + options.qrels + ".tsv");
}

static bool ReadIndexHeaderOnly(const std::string& idxPath, IndexFileHeader& header)
{
    std::ifstream input(FsPathFromUtf8(idxPath), std::ios::binary);
    if (!input) return false;
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    return input.good()
        && std::memcmp(header.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC)) == 0
        && header.IFH_Version == INDEX_FORMAT_VERSION;
}

static uint64_t BeirEvalLeafCacheBytes(const std::string& idxPath, const BeirEvalOptions& options)
{
    if (options.leafCacheMb > 0)
        return options.leafCacheMb * 1024ull * 1024ull;

    if (!options.leafCacheMatchMphf)
        return 0;

    IndexFileHeader header{};
    if (!ReadIndexHeaderOnly(idxPath, header))
        return 0;

    const uint64_t mphfBytes = header.IFH_TermMphfHeaderCount * sizeof(TermMphfHeader)
        + header.IFH_TermMphfDisplacementCount * sizeof(int32_t)
        + header.IFH_TermMphfEntryPageCount * sizeof(IndexBlock);
    return LEAF_TERM_CACHE_BYTES + mphfBytes;
}

static int RunBeirBuild(const std::string& idxPath, const BeirBuildOptions& options)
{
    const std::string corpusPath = BeirFilePath(options.dataPath, "corpus.jsonl");
    std::ifstream corpus(FsPathFromUtf8(corpusPath));
    if (!corpus) {
        std::cerr << "BEIR corpus not found: " << corpusPath << "\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::remove(FsPathFromUtf8(idxPath), ec);
    ec.clear();
    std::filesystem::remove(FsPathFromUtf8(DeltaIndexPath(idxPath)), ec);

    IndexContext ctx("", "", false);
    std::string line;
    uint64_t docId = 0;
    uint64_t skipped = 0;
    auto start = std::chrono::steady_clock::now();
    while (std::getline(corpus, line)) {
        if (options.limit > 0 && docId >= options.limit) break;
        std::string id;
        std::string title;
        std::string text;
        if (!ExtractJsonString(line, "_id", id) || !ExtractJsonString(line, "text", text)) {
            ++skipped;
            continue;
        }
        ExtractJsonString(line, "title", title);
        Document doc;
        doc.doc_id = docId;
        doc.path = id;
        doc.title = title;
        doc.body = text;
        doc.importance = 0.1f;
        ctx.AddDocument(doc, false);
        ++docId;
        if (docId % 1000 == 0)
            std::cout << "  BEIR indexed " << docId << " docs\n";
    }

    if (docId == 0) {
        std::cerr << "BEIR corpus had no readable docs: " << corpusPath << "\n";
        return 1;
    }

    std::cout << "  writing BEIR index: " << idxPath << "\n";
    if (!ctx.SaveIndex(idxPath.c_str())) {
        std::cerr << "Failed to save BEIR index: " << idxPath << "\n";
        return 1;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "BEIR build complete docs=" << docId;
    if (skipped) std::cout << " skipped=" << skipped;
    std::cout << " elapsed_ms=" << elapsedMs << "\n";
    return 0;
}

static int RunBeirEval(const std::string& idxPath, const BeirEvalOptions& options)
{
    if (!IndexSerializer::IsValidIndex(idxPath.c_str())) {
        std::cerr << "BEIR index not found or invalid: " << idxPath << "\n";
        return 1;
    }

    const std::string queryPath = BeirFilePath(options.dataPath, "queries.jsonl");
    const std::string qrelsPath = ResolveBeirQrelsPath(options);
    BeirQrels qrels;
    if (!LoadBeirQrels(qrelsPath, qrels) || qrels.empty()) {
        std::cerr << "BEIR qrels not found or empty: " << qrelsPath << "\n";
        return 1;
    }

    std::ifstream queries(FsPathFromUtf8(queryPath));
    if (!queries) {
        std::cerr << "BEIR queries not found: " << queryPath << "\n";
        return 1;
    }

    const uint64_t leafCacheBytes = BeirEvalLeafCacheBytes(idxPath, options);
    IndexContext ctx("", "", false);
    if (leafCacheBytes > 0)
        ctx.SetLeafTermCacheBytes(leafCacheBytes);
    ctx.LoadIndex(idxPath.c_str());
    ctx.SetTermMphfEnabled(!options.noMphf);
    std::unique_ptr<IndexSearchExecutor> executor(ctx.GetExecutor());
    SmartTokenizer beirTokenizer;
    const int maxK = *std::max_element(options.at.begin(), options.at.end());
    std::vector<double> macroRecall(options.at.size(), 0.0);
    std::vector<uint64_t> microHits(options.at.size(), 0);
    uint64_t microRelevant = 0;
    uint64_t evaluated = 0;
    uint64_t missingQrels = 0;

    std::string line;
    auto start = std::chrono::steady_clock::now();
    while (std::getline(queries, line)) {
        std::string qid;
        std::string query;
        if (!ExtractJsonString(line, "_id", qid) || !ExtractJsonString(line, "text", query))
            continue;
        auto qrelIt = qrels.find(qid);
        if (qrelIt == qrels.end() || qrelIt->second.empty()) {
            ++missingQrels;
            continue;
        }
        if (options.limit > 0 && evaluated >= options.limit) break;

        std::shared_ptr<IndexReader> reader;
        if (options.mode == "bow") {
            reader = BuildBeirBowReader(ctx, beirTokenizer, query, options.streams);
        } else if (options.mode == "weakand") {
            reader = ctx.GetReader(query.c_str(), options.streams.c_str(), QueryCompileMode::WeakAnd);
        } else {
            reader = ctx.GetReader(query.c_str(), options.streams.c_str());
        }
        auto results = executor->Execute(reader, maxK);
        std::vector<uint64_t> cumulativeHits(options.at.size(), 0);
        uint64_t hitCount = 0;
        size_t nextAt = 0;
        for (size_t rank = 0; rank < results.size(); ++rank) {
            const std::string docIdText = ctx.GetDocPath(results[rank].doc_id);
            if (qrelIt->second.count(docIdText))
                ++hitCount;
            while (nextAt < options.at.size() && rank + 1 == static_cast<size_t>(options.at[nextAt])) {
                cumulativeHits[nextAt] = hitCount;
                ++nextAt;
            }
        }
        while (nextAt < options.at.size()) {
            cumulativeHits[nextAt] = hitCount;
            ++nextAt;
        }

        const uint64_t relevantCount = static_cast<uint64_t>(qrelIt->second.size());
        microRelevant += relevantCount;
        for (size_t i = 0; i < options.at.size(); ++i) {
            macroRecall[i] += static_cast<double>(cumulativeHits[i]) / static_cast<double>(relevantCount);
            microHits[i] += cumulativeHits[i];
        }
        ++evaluated;
        if (evaluated % 100 == 0)
            std::cout << "  BEIR evaluated " << evaluated << " queries\n";
    }

    if (evaluated == 0 || microRelevant == 0) {
        std::cerr << "No BEIR queries with qrels were evaluated\n";
        return 1;
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "BEIR eval index=" << idxPath
              << " data=" << options.dataPath
              << " qrels=" << qrelsPath
              << " streams=" << options.streams
              << " mode=" << options.mode
              << " mphf=" << (options.noMphf ? "off" : "on")
              << " leaf_cache_mb=" << (leafCacheBytes ? (leafCacheBytes / (1024ull * 1024ull)) : (LEAF_TERM_CACHE_BYTES / (1024ull * 1024ull)))
              << " queries=" << evaluated
              << " missing_qrels=" << missingQrels
              << " elapsed_ms=" << elapsedMs << "\n";
    std::cout << std::fixed << std::setprecision(4);
    for (size_t i = 0; i < options.at.size(); ++i) {
        const double macro = macroRecall[i] / static_cast<double>(evaluated);
        const double micro = static_cast<double>(microHits[i]) / static_cast<double>(microRelevant);
        std::cout << "Recall@" << options.at[i]
                  << " macro=" << macro
                  << " micro=" << micro
                  << " hits=" << microHits[i]
                  << "/" << microRelevant << "\n";
    }
    return 0;
}

static int RunSampleMerge(const SampleMergeOptions& options)
{
    const std::string outPath = AbsolutePath(options.outPath);
    const std::string basePath = outPath + ".base.tmp";
    const std::string deltaPath = DeltaIndexPath(basePath);

    std::error_code removeError;
    std::filesystem::remove(FsPathFromUtf8(outPath), removeError);
    removeError.clear();
    std::filesystem::remove(FsPathFromUtf8(basePath), removeError);
    removeError.clear();
    std::filesystem::remove(FsPathFromUtf8(deltaPath), removeError);

    auto files = CollectSampleMergeFiles(options.dirPath, options.extensions);
    if (files.empty()) {
        std::cerr << "No readable indexable files found under " << options.dirPath
                  << " (ext=" << JoinExtensions(options.extensions) << ")\n";
        return 1;
    }
    if (files.size() < 2) {
        std::cerr << "sample-merge needs at least two readable files under " << options.dirPath
                  << " (ext=" << JoinExtensions(options.extensions) << ")\n";
        return 1;
    }

    const size_t split = (files.size() + 1) / 2;
    std::cout << "sample-merge: collected " << files.size() << " file(s) <= "
              << (MAX_INDEX_FILE_BYTES / (1024 * 1024)) << "MB from " << options.dirPath
              << " (ext=" << JoinExtensions(options.extensions) << ")\n";
    std::cout << "sample-merge: staging base=" << basePath << " delta=" << deltaPath << "\n";

    uint64_t baseKept = 0;
    uint64_t baseSkipped = 0;
    uint64_t deltaKept = 0;
    uint64_t deltaSkipped = 0;
    std::string firstContent;

    {
        IndexContext base("", "", false);
        uint64_t nextDocId = 0;
        for (size_t i = 0; i < split; ++i) {
            if (AddFileDocument(base, files[i], nextDocId, baseKept, baseSkipped, &firstContent))
                ++nextDocId;
        }

        if (baseKept == 0 || !base.SaveIndex(basePath.c_str())) {
            std::cerr << "sample-merge: failed to save base index: " << basePath << "\n";
            return 1;
        }
        std::cout << "sample-merge: saved base docs=" << baseKept;
        if (baseSkipped) std::cout << " skipped=" << baseSkipped;
        std::cout << "\n";
    }

    {
        IndexContext delta("", basePath.c_str(), false);
        uint64_t nextDocId = delta.DocumentCount();
        for (size_t i = split; i < files.size(); ++i) {
            if (AddFileDocument(delta, files[i], nextDocId, deltaKept, deltaSkipped, &firstContent))
                ++nextDocId;
        }

        if (deltaKept == 0 || !delta.SaveIndex(deltaPath.c_str())) {
            std::cerr << "sample-merge: failed to save delta index: " << deltaPath << "\n";
            return 1;
        }
        std::cout << "sample-merge: saved delta docs=" << deltaKept;
        if (deltaSkipped) std::cout << " skipped=" << deltaSkipped;
        std::cout << "\n";
    }

    {
        IndexContext mergeContext("", basePath.c_str());
        if (!mergeContext.Merge(outPath.c_str())) {
            std::cerr << "sample-merge: merge failed: " << outPath << "\n";
            return 1;
        }
        std::cout << "sample-merge: merged output=" << outPath << "\n";
    }

    IndexContext merged("", outPath.c_str(), false);
    const uint64_t expectedDocs = baseKept + deltaKept;
    if (merged.DocumentCount() != expectedDocs) {
        std::cerr << "sample-merge: verification failed, expected " << expectedDocs
                  << " docs but loaded " << merged.DocumentCount() << "\n";
        return 1;
    }

    std::string sanityQuery = "include";
    if (!HasSearchResults(merged, sanityQuery)) {
        sanityQuery = "class";
        if (!HasSearchResults(merged, sanityQuery)) {
            sanityQuery = FirstUsableQueryToken(firstContent);
            if (!HasSearchResults(merged, sanityQuery)) {
                std::cerr << "sample-merge: verification failed, no search results for sanity query\n";
                return 1;
            }
        }
    }

    std::cout << "sample-merge: verified docs=" << expectedDocs
              << " query=\"" << sanityQuery << "\"\n"
              << "sample-merge: success\n";
    return 0;
}

static int RunIndexCommand(const std::string& idxPath, const IndexOptions& options)
{
    std::vector<FileItem> files;
    std::string inputPath;
    if (!options.filePath.empty()) {
        inputPath = options.filePath;
        files = CollectSingleFile(options.filePath);
    } else {
        inputPath = options.dirPath;
        files = CollectDirectoryFiles(options.dirPath, options.extensions, options.recursive);
    }

    if (files.empty()) {
        std::cerr << "No readable indexable files found: " << inputPath << "\n";
        return 1;
    }

    std::cout << "Collected " << files.size() << " file(s) <= "
              << (MAX_INDEX_FILE_BYTES / (1024 * 1024)) << "MB from "
              << inputPath;
    if (options.filePath.empty())
        std::cout << " (ext=" << JoinExtensions(options.extensions)
                  << ", recur=" << (options.recursive ? "true" : "false") << ")";
    std::cout << "\n";

    const std::string deltaPath = DeltaIndexPath(idxPath);

    if (IndexSerializer::IsValidIndex(idxPath.c_str()) && IndexSerializer::IsValidIndex(deltaPath.c_str())) {
        std::cout << "Merging pending delta into " << idxPath << "\n";
        IndexContext mergeContext("", idxPath.c_str());
        if (!mergeContext.Merge(idxPath.c_str())) {
            std::cerr << "Failed to merge pending delta into index: " << idxPath << "\n";
            return 1;
        }
        std::error_code removeError;
        std::filesystem::remove(FsPathFromUtf8(deltaPath), removeError);
    }

    uint64_t baseNextId = 0;
    uint64_t deltaNextId = 0;
    PathMap baseMap = LoadPathMapFromIndex(idxPath, baseNextId);
    PathMap deltaMap = LoadPathMapFromIndex(deltaPath, deltaNextId);
    PathMap indexedMap = baseMap;
    uint64_t nextId = baseNextId;
    for (const auto& [path, _] : deltaMap) {
        if (!indexedMap.count(path))
            indexedMap[path] = nextId++;
    }
    PathMap knownMap = indexedMap;

    uint64_t added = 0, existing = 0;
    std::vector<std::pair<std::string, uint64_t>> pendingFiles;
    for (const auto& file : files) {
        if (knownMap.count(file.path)) {
            ++existing;
        } else {
            const uint64_t docId = nextId++;
            pendingFiles.push_back({file.path, docId});
            knownMap[file.path] = docId;
            ++added;
        }
    }

    if (added == 0) {
        std::cout << "No new files to index\n";
        return 0;
    }

    uint64_t totalKept = 0;
    uint64_t totalSkipped = 0;
    uint64_t savedBatches = 0;
    std::set<std::string> reportedSkippedPaths;
    const std::string batchPath = BatchIndexPath(idxPath);
    for (size_t offset = 0; offset < pendingFiles.size(); offset += static_cast<size_t>(options.batchSize)) {
        const size_t end = std::min(offset + static_cast<size_t>(options.batchSize), pendingFiles.size());
        const size_t batchNewCount = end - offset;
        PathMap batchMap;
        for (size_t i = offset; i < end; ++i) {
            batchMap[pendingFiles[i].first] = pendingFiles[i].second;
            indexedMap[pendingFiles[i].first] = pendingFiles[i].second;
        }

        uint64_t kept = 0;
        uint64_t skipped = 0;
        std::cout << "Batch " << (savedBatches + 1) << ": adding "
                  << batchNewCount << " new file(s), merging "
                  << indexedMap.size() << " total document(s) into " << idxPath << "\n";
        if (!BuildIndexFile(batchPath, batchMap, kept, skipped, &reportedSkippedPaths)) {
            std::cerr << "Failed to save batch index: " << batchPath << "\n";
            return 1;
        }

        if (!IndexSerializer::IsValidIndex(idxPath.c_str())) {
            std::error_code replaceError;
            std::filesystem::rename(FsPathFromUtf8(batchPath), FsPathFromUtf8(idxPath), replaceError);
            if (replaceError) {
                std::cerr << "Failed to create index: " << idxPath << "\n";
                return 1;
            }
        } else {
            std::cout << "  staging delta: " << deltaPath << "\n";
            std::error_code deltaReplaceError;
            std::filesystem::remove(FsPathFromUtf8(deltaPath), deltaReplaceError);
            deltaReplaceError.clear();
            std::filesystem::rename(FsPathFromUtf8(batchPath), FsPathFromUtf8(deltaPath), deltaReplaceError);
            if (deltaReplaceError) {
                std::cerr << "Failed to stage delta index: " << deltaPath << "\n";
                return 1;
            }

            std::cout << "  merging delta into: " << idxPath << "\n";
            auto mergeStart = std::chrono::steady_clock::now();
            IndexContext mergeContext("", idxPath.c_str());
            if (!mergeContext.Merge(idxPath.c_str())) {
                std::cerr << "Failed to merge delta into index: " << idxPath << "\n";
                return 1;
            }
            const auto mergeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - mergeStart).count();
            std::cout << "  merged in " << mergeMs << " ms\n";
        }

        baseNextId = 0;
        baseMap = LoadPathMapFromIndex(idxPath, baseNextId);
        indexedMap = baseMap;
        std::error_code removeError;
        std::filesystem::remove(FsPathFromUtf8(deltaPath), removeError);

        totalKept = static_cast<uint64_t>(baseMap.size());
        totalSkipped += skipped;
        ++savedBatches;
    }

    std::cout << "Indexed input: " << inputPath << "\n"
              << "Files:   " << files.size()
              << " (new " << added << ", existing " << existing << ")\n"
              << "Batch size: " << options.batchSize << "\n"
              << "Saved batches: " << savedBatches << "\n"
              << "Saved:   " << totalKept << " document(s)";
    if (totalSkipped) std::cout << " (skipped " << totalSkipped << ")";
    std::cout << " to " << idxPath << "\n"
              << "Total:   " << totalKept
              << " indexed document(s)\n";

    return 0;
}

int main(int argc, char* argv[])
{
    std::string idxPath = DefaultIdxPath();

    // On Windows, argv uses the ANSI codepage and drops non-ASCII characters.
    // Use CommandLineToArgvW to get the real Unicode arguments instead.
    std::vector<std::string> args;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc; ++i)
            args.push_back(WideToUtf8(wargv[i]));
        LocalFree(wargv);
    }
#else
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);
#endif

    std::string error;
    if (!ApplyGlobalOptions(args, idxPath, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    const std::string cmd = args.empty() ? std::string() : args[0];
    if (cmd.empty()) {
        PrintHelp(idxPath);
        return 0;
    }

    // ── index files ───────────────────────────────────────────────────────────
    if (IsIndexCommand(cmd)) {
        IndexOptions options;
        if (!ParseIndexOptions(args, options, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return RunIndexCommand(idxPath, options);

    // ── sample base/delta merge ──────────────────────────────────────────────
    } else if (cmd == "-sample-merge") {
        SampleMergeOptions options;
        if (!ParseSampleMergeOptions(args, options, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return RunSampleMerge(options);

    // ── BEIR recall evaluation ──────────────────────────────────────────────
    } else if (cmd == "-beir-build") {
        BeirBuildOptions options;
        if (!ParseBeirBuildOptions(args, options, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return RunBeirBuild(idxPath, options);

    } else if (cmd == "-beir-eval") {
        BeirEvalOptions options;
        if (!ParseBeirEvalOptions(args, options, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return RunBeirEval(idxPath, options);

    // ── interactive search ────────────────────────────────────────────────────
    } else if (cmd == "-i" || cmd == "-v") {
        SearchOptions searchOptions;
        if (!ParseSearchOptions(args, searchOptions, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return RunInteractiveSearch(idxPath, searchOptions);

    } else {
        std::cerr << "Unknown option: " << cmd << "\n";
        PrintHelp(idxPath);
        return 1;
    }

    return 0;
}
