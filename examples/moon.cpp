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
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <pwd.h>
#  include <unistd.h>
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

static std::string DefaultIdxPath()
{
    return HomeDir() + PathSep() + "moon.idx";
}

// ─── path→id map: loaded from existing .idx DocData ───────────────────────────

using PathMap = std::map<std::string, uint64_t>;  // filepath → sequential id

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

    for (auto& [fp, id] : pathMap) {
        std::string content = ReadFile(fp);
        if (content.empty()) {
            std::cerr << "  skipping (unreadable): " << fp << "\n";
            continue;
        }
        writer->Write(tok.Tokenize(Stem(fp).c_str()), id, "Title");
        writer->Write(tok.Tokenize(content.c_str()),  id, "Body");
        // Path stored in DocData — no separate .meta file needed
        ctx.GetStore()->SetDocPath(id, fp);
    }

    ctx.SaveIndex(idxPath.c_str());
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
    auto results = ctx.GetExecutor()->Execute(reader, 20);
    delete tree;

    if (results.empty()) {
        std::cout << "(no results)\n";
        return;
    }

    for (auto& r : results) {
        const std::string& path = ctx.GetStore()->GetDocPath(r.doc_id);
        std::cout << (path.empty() ? "[unknown]" : path) << "\n";
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
        std::string fp = filePath;

        if (ReadFile(fp).empty()) {
            std::cerr << "Cannot read file: " << fp << "\n";
            return 1;
        }

        uint64_t max_id = 0;
        PathMap  pathMap = LoadPathMap(idxPath, max_id);

        if (pathMap.count(fp))
            std::cout << "Re-indexing: " << fp << "\n";
        else
            pathMap[fp] = ++max_id;  // sequential id

        Rebuild(idxPath, pathMap);

        std::cout << "Indexed: " << fp << "\n"
                  << "Total:   " << pathMap.size()
                  << " document(s) in " << idxPath << "\n";

    // ── interactive search ────────────────────────────────────────────────────
    } else if (cmd == "-i") {
        IndexContext ctx("", idxPath.c_str());

        std::cout << "moon search — "
                  << ctx.GetStore()->AllDocStats().size()
                  << " document(s)\nType a query, or 'quit' to exit.\n";

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
