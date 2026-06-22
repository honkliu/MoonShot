/*
 * moon — personal document search tool
 *
 * Usage:
 *   moon -file <filepath>              Index one file
 *   moon -dir <directory> -ext md,txt  Index matching files in one directory
 *   moon -dir <directory> -ext md -r   Index matching files recursively
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

// Returns the filename without extension — used as the Title stream.
static std::string Stem(const std::string& path)
{
    auto slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = name.rfind('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

// ─── index: rebuild from all known files ─────────────────────────────────────

static bool BuildIndexFile(const std::string& baseIdxPath,
                           const std::string& savePath,
                           const PathMap& pathMap,
                           bool saveAsDelta,
                           uint64_t& kept,
                           uint64_t& skipped)
{
    IndexContext   ctx("", saveAsDelta ? baseIdxPath.c_str() : "", false);
    kept = 0;
    skipped = 0;
    const uint64_t total = static_cast<uint64_t>(pathMap.size());
    uint64_t processed = 0;

    for (auto& [fp, id] : pathMap) {
        ++processed;
        std::string content = ReadFile(fp);
        if (content.empty()) {
            std::cerr << "  skipping (unreadable): " << fp << "\n";
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
    return kept > 0 && ctx.SaveIndex(savePath.c_str());
}

// ─── search ──────────────────────────────────────────────────────────────────

struct SearchHit {
    const IndexContext* context = nullptr;
    SearchResult result;
};

static void CollectSearchResults(IndexContext& ctx,
                                 const std::string& query,
                                 std::vector<SearchHit>& hits)
{
    if (ctx.DocumentCount() == 0)
        return;

    std::unique_ptr<EvalTree> tree(ctx.Compile(query.c_str(), "AUTB"));

    if (!tree || tree->IsEmpty())
        return;

    auto reader = ctx.GetReader(tree.get());
    std::unique_ptr<IndexSearchExecutor> executor(ctx.GetExecutor());
    auto results = executor->Execute(reader, 0);

    for (const auto& result : results)
        hits.push_back({&ctx, result});
}

static void Search(IndexContext& ctx, const std::string& query)
{
    std::vector<SearchHit> results;
    CollectSearchResults(ctx, query, results);
    if (auto* delta = ctx.GetDeltaContext())
        CollectSearchResults(*delta, query, results);

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

struct IndexOptions {
    std::string filePath;
    std::string dirPath;
    std::vector<std::string> extensions = ParseExtensions("md,txt");
    bool recursive = false;
};

static bool IsIndexCommand(const std::string& arg)
{
    return arg == "-file"
        || arg == "-dir"
        || arg == "-ext"
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
    error = "Usage: moon -file <filename> | moon -dir <directory> [-ext md,txt] [-r]";
    return false;
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
        << "moon — personal document search\n\n"
        << "  moon -file <file>               Index one file\n"
        << "  moon -dir <dir> -ext md,txt     Index files in one directory\n"
        << "  moon -dir <dir> -ext md -r      Index files recursively\n"
        << "  moon -i                             Interactive search\n\n"
        << "Index: " << idxPath << "\n";
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

    const std::string cmd = args.empty() ? std::string() : args[0];
    if (cmd.empty()) {
        PrintHelp(idxPath);
        return 0;
    }

    // ── index files ───────────────────────────────────────────────────────────
    if (IsIndexCommand(cmd)) {
        IndexOptions options;
        std::string error;
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
        const bool baseValid = IndexSerializer::IsValidIndex(idxPath.c_str());

        uint64_t baseNextId = 0;
        uint64_t deltaNextId = 0;
        PathMap baseMap = LoadPathMapFromIndex(idxPath, baseNextId);
        PathMap deltaMap = baseValid ? LoadPathMapFromIndex(deltaPath, deltaNextId) : PathMap{};
        PathMap knownMap = baseMap;
        if (baseValid) {
            for (const auto& [path, id] : deltaMap)
                knownMap.emplace(path, id);
        }

        uint64_t added = 0, existing = 0;
        PathMap targetMap = baseValid ? deltaMap : PathMap{};
        uint64_t nextId = baseValid ? deltaNextId : 0;
        for (const auto& file : files) {
            if (knownMap.count(file.path)) {
                ++existing;
            } else {
                targetMap[file.path] = nextId++;
                knownMap[file.path] = targetMap[file.path];
                ++added;
            }
        }

        if (added == 0) {
            std::cout << "No new files to index\n";
            return 0;
        }

        const std::string savePath = baseValid ? deltaPath : idxPath;
        uint64_t kept = 0;
        uint64_t skipped = 0;
        if (!BuildIndexFile(idxPath, savePath, targetMap, baseValid, kept, skipped)) {
            std::cerr << "Failed to save index: " << savePath << "\n";
            return 1;
        }

        std::cout << "Indexed input: " << inputPath << "\n"
                  << "Files:   " << files.size()
                  << " (new " << added << ", existing " << existing << ")\n"
                  << "Saved:   " << kept << " document(s)";
        if (skipped) std::cout << " (skipped " << skipped << ")";
        std::cout << " to " << savePath << "\n"
                  << "Total:   " << knownMap.size()
                  << " known document(s)\n";

    // ── interactive search ────────────────────────────────────────────────────
    } else if (cmd == "-i") {
        auto loadStart = std::chrono::steady_clock::now();
        IndexContext ctx("", idxPath.c_str());
        auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loadStart).count();
        uint64_t totalDocuments = ctx.DocumentCount();
        if (auto* delta = ctx.GetDeltaContext())
            totalDocuments += delta->DocumentCount();

        std::cout << "moon search — "
                  << totalDocuments
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
