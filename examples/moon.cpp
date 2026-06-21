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

// ─── path→id map: loaded from existing .idx DocData ───────────────────────────

using PathMap = std::map<std::string, uint64_t>;  // filepath → sequential id
static constexpr uintmax_t MAX_INDEX_FILE_BYTES = 8ull * 1024ull * 1024ull;

struct FileItem {
    std::string path;
    uintmax_t   size = 0;
};

static std::filesystem::path FsPathFromUtf8(const std::string& path);

static std::string ManifestPath(const std::string& idxPath)
{
    return idxPath + ".manifest";
}

static PathMap LoadPathMap(const std::string& idxPath, uint64_t& max_id)
{
    PathMap m;
    max_id = 0;
    IndexContext ctx("", idxPath.c_str());
    for (uint64_t id = 0; id < ctx.DocumentCount(); ++id) {
        const std::string path = ctx.GetDocPath(id);
        if (!path.empty()) {
            m[path] = id;
            max_id = std::max(max_id, id + 1);
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
    IndexContext   ctx;
    uint64_t       kept = 0;
    uint64_t       skipped = 0;

    for (auto& [fp, id] : pathMap) {
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
    }

    ctx.SaveIndex(idxPath.c_str());
    std::cout << "Rebuilt index with " << kept << " readable document(s)";
    if (skipped) std::cout << " (skipped " << skipped << ")";
    std::cout << "\n";
}

// ─── search ──────────────────────────────────────────────────────────────────

static void Search(IndexContext& ctx, const std::string& query)
{
    if (ctx.DocumentCount() == 0) {
        std::cout << "(no results)\n";
        return;
    }

    auto* tree = ctx.Compile(query.c_str(), "AUTB");

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
            const std::string path = ctx.GetDocPath(r.doc_id);
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
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        if (wargc >= 2) cmd      = WideToUtf8(wargv[1]);
        if (wargc >= 3) filePath = WideToUtf8(wargv[2]);
        LocalFree(wargv);
    }
#else
    if (argc >= 2) cmd      = argv[1];
    if (argc >= 3) filePath = argv[2];
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

        std::error_code ec;
        std::filesystem::remove(FsPathFromUtf8(ManifestPath(idxPath)), ec);
        uint64_t max_id = 0;
        PathMap  pathMap = LoadPathMap(idxPath, max_id);

        uint64_t added = 0, existing = 0;
        for (const auto& file : files) {
            if (pathMap.count(file.path)) {
                ++existing;
            } else {
                pathMap[file.path] = max_id++;
                ++added;
            }
        }

        Rebuild(idxPath, pathMap);

        std::cout << "Indexed input: " << filePath << "\n"
                  << "Files:   " << files.size()
                  << " (new " << added << ", existing " << existing << ")\n"
                  << "Total:   " << pathMap.size()
                  << " document(s) in " << idxPath << "\n";

    // ── interactive search ────────────────────────────────────────────────────
    } else if (cmd == "-i") {
        auto loadStart = std::chrono::steady_clock::now();
        IndexContext ctx("", idxPath.c_str());
        auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loadStart).count();

        std::cout << "moon search — "
                  << ctx.DocumentCount()
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
