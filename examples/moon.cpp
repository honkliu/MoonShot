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
#include <cstdlib>
#include <fstream>
#include <iostream>
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

static std::string MetaPath(const std::string& idxPath)
{
    return idxPath + ".meta";
}

// ─── doc-id: FNV-1a 64-bit hash of the canonical file path ───────────────────

static uint64_t DocId(const std::string& path)
{
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : path) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// ─── metadata: docid ↔ filepath (binary, portable) ───────────────────────────

using MetaMap = std::unordered_map<uint64_t, std::string>;

static MetaMap LoadMeta(const std::string& path)
{
    MetaMap m;
    std::ifstream f(path, std::ios::binary);
    if (!f) return m;

    uint64_t n = 0;
    f.read(reinterpret_cast<char*>(&n), sizeof(n));

    for (uint64_t i = 0; i < n && f; ++i) {
        uint64_t id  = 0;
        uint32_t len = 0;
        f.read(reinterpret_cast<char*>(&id),  sizeof(id));
        f.read(reinterpret_cast<char*>(&len), sizeof(len));
        std::string p(len, '\0');
        f.read(p.data(), static_cast<std::streamsize>(len));
        m[id] = std::move(p);
    }
    return m;
}

static void SaveMeta(const std::string& path, const MetaMap& m)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Cannot write meta: " << path << "\n"; return; }

    uint64_t n = m.size();
    f.write(reinterpret_cast<const char*>(&n), sizeof(n));

    for (auto& [id, p] : m) {
        uint32_t len = static_cast<uint32_t>(p.size());
        f.write(reinterpret_cast<const char*>(&id),  sizeof(id));
        f.write(reinterpret_cast<const char*>(&len), sizeof(len));
        f.write(p.data(), static_cast<std::streamsize>(len));
    }
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

// ─── index: always rebuild from all known files (clean + correct) ─────────────

static void Rebuild(const std::string& idxPath, MetaMap& meta)
{
    SmartTokenizer tok;
    IndexContext   ctx;           // fresh in-memory, no file path
    auto           writer = ctx.GetWriter();

    for (auto& [id, fp] : meta) {
        std::string content = ReadFile(fp);
        if (content.empty()) {
            std::cerr << "  skipping (unreadable): " << fp << "\n";
            continue;
        }
        // Filename stem → Title stream (matches filename-based queries)
        writer->Write(tok.Tokenize(Stem(fp).c_str()), id, "Title");
        // File content   → Body stream
        writer->Write(tok.Tokenize(content.c_str()),  id, "Body");
    }

    ctx.SaveIndex(idxPath.c_str());
}

// ─── search ──────────────────────────────────────────────────────────────────

static void Search(IndexContext& ctx, const MetaMap& meta, const std::string& query)
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
        auto it = meta.find(r.doc_id);
        std::cout << (it != meta.end() ? it->second : "[unknown]") << "\n";
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
        std::string fp   = filePath;
        MetaMap     meta = LoadMeta(MetaPath(idxPath));
        uint64_t    id   = DocId(fp);

        if (meta.count(id))
            std::cout << "Re-indexing: " << fp << "\n";

        // Verify the file is readable before committing
        if (ReadFile(fp).empty()) {
            std::cerr << "Cannot read file: " << fp << "\n";
            return 1;
        }

        meta[id] = fp;
        Rebuild(idxPath, meta);
        SaveMeta(MetaPath(idxPath), meta);

        std::cout << "Indexed: " << fp << "\n"
                  << "Total:   " << meta.size() << " document(s) in " << idxPath << "\n";

    // ── interactive search ────────────────────────────────────────────────────
    } else if (cmd == "-i") {
        MetaMap     meta = LoadMeta(MetaPath(idxPath));
        IndexContext ctx("", idxPath.c_str());

        std::cout << "moon search — " << meta.size() << " document(s)\n"
                  << "Type a query, or 'quit' to exit.\n";

        std::string line;
        while (true) {
            std::cout << "> ";
            std::cout.flush();
            if (!std::getline(std::cin, line)) break;
            if (line == "quit" || line == "exit" || line == "q") break;
            if (line.empty()) continue;
            Search(ctx, meta, line);
        }

    } else {
        std::cerr << "Unknown option: " << cmd << "\n";
        PrintHelp(idxPath);
        return 1;
    }

    return 0;
}
