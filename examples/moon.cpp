/*
 * moon — personal document search tool
 *
 * Usage:
 *   moon -idx <index> -file <filepath>       Index one file into <index>
 *   moon -idx <index> -dir <directory> -r    Index files recursively into <index>
 *   moon -idx <index> -i                     Search <index> with inverted index
 *   moon -idx <index> -v                     Search <index> with vector index
 *   moon -idx <index> -i -v                  Search <index> with both
 *   moon -file <filepath>              Index one file
 *   moon -dir <directory> -ext md,txt  Index matching files in one directory
 *   moon -dir <directory> -ext md -r   Index matching files recursively
 *   moon -sample-merge -dir <root> -out <index-path> [-ext cpp,h,rs]
 *   moon -i                               Interactive search mode
 *
 * Index stored at:
 *   Linux   : ~/moon.idx  (+ ~/moon.idx.meta)
 *   Windows : %USERPROFILE%\moon.idx
 */

#include "moonshot.h"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
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

// Returns the filename without extension — used as the Title stream.
static std::string Stem(const std::string& path)
{
    auto slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = name.rfind('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
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
    uint8_t recallMask = 0;
};

static constexpr uint8_t RECALL_INVERTED = 1;
static constexpr uint8_t RECALL_VECTOR = 2;

struct SearchOptions {
    bool inverted = false;
    bool vector = false;
};

static std::vector<SearchResult> ExecuteQuery(IndexContext& ctx,
                                              const std::string& query,
                                              const char* streams)
{
    std::unique_ptr<IndexSearchExecutor> executor(ctx.GetExecutor());
    return executor->Execute(ctx.GetReader(query.c_str(), streams), 0);
}

static float StaticRankForDoc(const IndexContext& ctx, uint64_t docId)
{
    const auto* entry = ctx.GetDocDataEntry(docId);
    return entry ? entry->DDE_StaticRank : 0.0f;
}

static const char* RecallPrefix(uint8_t recallMask)
{
    switch (recallMask) {
        case RECALL_INVERTED | RECALL_VECTOR: return "++";
        case RECALL_INVERTED: return "+-";
        case RECALL_VECTOR: return "-+";
        default: return "--";
    }
}

static void CollectSearchResults(IndexContext& ctx,
                                 const std::string& query,
                                 const SearchOptions& options,
                                 std::vector<SearchHit>& hits)
{
    if (ctx.DocumentCount() == 0)
        return;

    std::unordered_map<uint64_t, size_t> hitByDocId;

    auto addResults = [&](const std::vector<SearchResult>& results, uint8_t recallMask) {
        for (const auto& result : results) {
            auto [it, inserted] = hitByDocId.emplace(result.doc_id, hits.size());
            if (inserted) {
                hits.push_back({&ctx, result, recallMask});
                continue;
            }

            auto& hit = hits[it->second];
            if ((hit.recallMask & recallMask) == 0) {
                hit.result.score += result.score - StaticRankForDoc(ctx, result.doc_id);
                hit.recallMask |= recallMask;
            }
        }
    };

    if (options.inverted)
        addResults(ExecuteQuery(ctx, query, "AUTB"), RECALL_INVERTED);
    if (options.vector)
        addResults(ExecuteQuery(ctx, query, "V"), RECALL_VECTOR);
}

static void Search(IndexContext& ctx, const std::string& query, const SearchOptions& options)
{
    std::vector<SearchHit> results;
    CollectSearchResults(ctx, query, options, results);
    if (auto* delta = ctx.GetDeltaContext())
        CollectSearchResults(*delta, query, options, results);

    std::sort(results.begin(), results.end(), [](const SearchHit& left, const SearchHit& right) {
        if (left.result.score != right.result.score)
            return left.result.score > right.result.score;
        return left.result.doc_id < right.result.doc_id;
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
            std::cout << RecallPrefix(hit.recallMask) << " "
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

// ─── main ─────────────────────────────────────────────────────────────────────

struct IndexOptions {
    std::string filePath;
    std::string dirPath;
    std::vector<std::string> extensions = ParseExtensions("md,txt");
    uint64_t batchSize = 200;
    bool recursive = false;
};

struct SampleMergeOptions {
    std::string dirPath;
    std::string outPath;
    std::vector<std::string> extensions = ParseExtensions("cpp,h,rs");
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
            if (i + 1 >= args.size()) { error = "Usage: moon -b 200"; return false; }
            if (!ParseBatchSize(args[++i], options.batchSize)) { error = "-b must be a positive integer"; return false; }
        } else if (arg == "-r") {
            options.recursive = true;
        } else {
            error = "Unknown option: " + arg;
            return false;
        }
    }

    if (!options.filePath.empty())
        return true;
    if (!options.dirPath.empty())
        return true;
    error = "Usage: moon -file <filename> | moon -dir <directory> [-ext md,txt] [-r] [-b 200]";
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
        << "  moon [-idx <index>] -dir <dir> -ext md,txt [-r] [-b 200]\n"
        << "      Index files under <dir>. Use -r for recursive traversal.\n"
        << "      -ext is a comma-separated extension list without or with dots.\n"
        << "      -b controls how many new files are saved per delta batch.\n\n"
        << "Search an index:\n"
        << "  moon [-idx <index>] -i\n"
        << "      Open an interactive inverted-index search prompt.\n\n"
        << "  moon [-idx <index>] -v\n"
        << "      Open an interactive vector search prompt.\n\n"
        << "  moon [-idx <index>] -i -v\n"
        << "      Open an interactive hybrid search prompt. Prefixes show recall path:\n"
        << "      ++ both, +- inverted only, -+ vector only.\n\n"
        << "Base/delta/merge sample:\n"
        << "  moon -sample-merge -dir <root> -out <merged-index> [-ext cpp,h,rs]\n"
        << "      Recursively indexes matching files, builds a base index and delta\n"
        << "      index, merges them, reloads the merged output, and runs a sanity\n"
        << "      query. This is the simplest end-to-end MoonShot merge sample.\n\n"
        << "Examples:\n"
        << "  moon -idx .\\notes.idx -dir .\\docs -ext md,txt -r\n"
        << "  moon -idx .\\notes.idx -i\n"
        << "  moon -idx .\\notes.idx -v\n"
        << "  moon -idx .\\notes.idx -i -v\n"
        << "  moon -sample-merge -dir . -out .\\build\\moonshot-source-merge.idx -ext cpp,h,rs\n";
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
                // Build each batch as a compact delta, then release the batch file
                // handle before loading/merging so Windows can replace the final idx.
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
            totalSkipped = skipped;
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

    // ── sample base/delta merge ──────────────────────────────────────────────
    } else if (cmd == "-sample-merge") {
        SampleMergeOptions options;
        if (!ParseSampleMergeOptions(args, options, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return RunSampleMerge(options);

    // ── interactive search ────────────────────────────────────────────────────
    } else if (cmd == "-i" || cmd == "-v") {
        SearchOptions searchOptions;
        if (!ParseSearchOptions(args, searchOptions, error)) {
            std::cerr << error << "\n";
            return 1;
        }

        auto loadStart = std::chrono::steady_clock::now();
        IndexContext ctx("", idxPath.c_str());
        auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loadStart).count();
        uint64_t totalDocuments = CountStoredDocuments(ctx);
        if (auto* delta = ctx.GetDeltaContext())
            totalDocuments += CountStoredDocuments(*delta, ctx.DocumentCount());

        std::cout << "moon search — "
                  << totalDocuments
                  << " document(s)"
                  << " (loaded in " << loadMs << " ms)\n"
                  << "Mode: "
                  << (searchOptions.inverted && searchOptions.vector ? "inverted+vector"
                      : searchOptions.inverted ? "inverted"
                      : "vector") << "\n"
                  << std::flush;

        if (searchOptions.vector) {
            auto vectorStart = std::chrono::steady_clock::now();
            std::cout << "Building vector graph..." << std::flush;
            size_t vectorCount = ctx.VectorCount();
            if (auto* delta = ctx.GetDeltaContext())
                vectorCount += delta->VectorCount();
            const auto vectorMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - vectorStart).count();
            std::cout << " " << vectorCount << " vector(s) in " << vectorMs << " ms\n";
        }

        std::cout << "Type a query, or 'quit' to exit.\n";

        std::string line;
        while (true) {
            std::cout << "> ";
            std::cout.flush();
            if (!std::getline(std::cin, line)) break;
            if (line == "quit" || line == "exit" || line == "q") break;
            if (line.empty()) continue;
            Search(ctx, line, searchOptions);
        }

    } else {
        std::cerr << "Unknown option: " << cmd << "\n";
        PrintHelp(idxPath);
        return 1;
    }

    return 0;
}
