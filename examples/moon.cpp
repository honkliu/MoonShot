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

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#endif

#include "moonshot.h"

#include <array>
#include <cstdint>
#include <cstring>
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
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <shellapi.h>
#  include <conio.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <pwd.h>
#  include <sys/socket.h>
#  include <sys/types.h>
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
    for (uint64_t id = 0; id < ctx.AllocateDocumentID(); ++id) {
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

struct SearchOptions {
    bool inverted = false;
    bool vector = false;
    bool bge = false;
    bool bgeSidecar = false;
    size_t topK = 1000;
    size_t vectorEf = 1000;
    std::string bgeHost = "127.0.0.1";
    uint16_t bgePort = 8765;
    std::string bgePython;
    std::string bgeScript;
    std::string bgeModel = "BAAI/bge-small-en-v1.5";
};

static constexpr size_t BGE_MAX_TEXT_BYTES = 65536;
static constexpr const char* BGE_DOCUMENT_MARKER = "__MOONSHOT_BGE_DOCUMENT__\n";

static bool EmbedDocumentWithBge(const std::string& text,
                                 const SearchOptions& options,
                                 std::vector<float>& vector);

// ─── index: build one batch delta ─────────────────────────────────────────────

static bool BuildIndexFile(const std::string& savePath,
                           const PathMap& pathMap,
                           const SearchOptions& embeddingOptions,
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
        ctx.AddDocument(doc, !embeddingOptions.bge);
        if (embeddingOptions.bge) {
            std::string embeddingText = doc.title;
            if (!doc.body.empty()) {
                if (!embeddingText.empty())
                    embeddingText.push_back('\n');
                embeddingText += doc.body;
            }
            if (embeddingText.size() > BGE_MAX_TEXT_BYTES - std::strlen(BGE_DOCUMENT_MARKER))
                embeddingText.resize(BGE_MAX_TEXT_BYTES - std::strlen(BGE_DOCUMENT_MARKER));

            std::vector<float> vector;
            if (EmbedDocumentWithBge(embeddingText, embeddingOptions, vector)) {
                ctx.GetWriter()->SetDocVector(id, std::move(vector));
            } else {
                std::cerr << "  warning: BGE document embedding failed; added without vector: " << fp << "\n";
            }
        }
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

static std::string SearchStreamSet(const SearchOptions& options)
{
    std::string streams;
    if (options.inverted) streams += "AUTB";
    if (options.vector && !options.bge) streams += "V";
    return streams;
}

static const char* SearchModeName(const SearchOptions& options)
{
    if (options.bge && options.inverted && options.vector) return "inverted+BGE";
    if (options.bge && options.vector) return "BGE vector";
    if (options.inverted && options.vector) return "inverted+vector";
    return options.inverted ? "inverted" : "vector";
}

static std::string QuoteShellArg(std::string text)
{
    if (text.find_first_of(" \t\"&()[]{}^=;!,`'") == std::string::npos)
        return text;

    std::string quoted = "\"";
    for (char ch : text) {
        if (ch == '"') quoted += "\\\"";
        else quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

static std::string DefaultBgePython()
{
#ifdef _WIN32
    const std::string local = ".\\.venv-bge\\Scripts\\python.exe";
#else
    const std::string local = "./.venv-bge/bin/python";
#endif
    return std::filesystem::is_regular_file(FsPathFromUtf8(local)) ? local : "python";
}

static std::string DefaultBgeScript()
{
    const std::string local = ".\\Tools\\embed_query.py";
    return std::filesystem::is_regular_file(FsPathFromUtf8(local)) ? local : "Tools/embed_query.py";
}

static bool ReadSingleI8Vector(const std::string& path, std::vector<float>& vector)
{
    static constexpr char Magic[8] = {'M','S','V','E','C','I','8','1'};
    std::ifstream input(FsPathFromUtf8(path), std::ios::binary);
    if (!input) return false;
    char magic[8]{};
    uint32_t dim = 0;
    uint32_t idBytes = 0;
    input.read(magic, sizeof(magic));
    input.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    input.read(reinterpret_cast<char*>(&idBytes), sizeof(idBytes));
    if (!input || std::memcmp(magic, Magic, sizeof(Magic)) != 0 || dim != DOC_VECTOR_DIM || idBytes == 0)
        return false;

    std::vector<char> idBuffer(idBytes);
    std::array<int8_t, DOC_VECTOR_DIM> payload{};
    input.read(idBuffer.data(), static_cast<std::streamsize>(idBuffer.size()));
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!input) return false;

    vector.assign(DOC_VECTOR_DIM, 0.0f);
    for (size_t i = 0; i < DOC_VECTOR_DIM; ++i)
        vector[i] = static_cast<float>(payload[i]) / 128.0f;
    return true;
}

static bool SocketSendAll(intptr_t socketHandle, const char* data, size_t size)
{
    while (size > 0) {
#ifdef _WIN32
        const int sent = send(static_cast<SOCKET>(socketHandle), data, static_cast<int>(size), 0);
#else
        const ssize_t sent = send(static_cast<int>(socketHandle), data, size, 0);
#endif
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

static bool SocketRecvAll(intptr_t socketHandle, char* data, size_t size)
{
    while (size > 0) {
#ifdef _WIN32
        const int got = recv(static_cast<SOCKET>(socketHandle), data, static_cast<int>(size), 0);
#else
        const ssize_t got = recv(static_cast<int>(socketHandle), data, size, 0);
#endif
        if (got <= 0)
            return false;
        data += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

static void CloseSocketHandle(intptr_t socketHandle)
{
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socketHandle));
#else
    close(static_cast<int>(socketHandle));
#endif
}

static bool EmbedTextWithBgeService(const std::string& text,
                                    const SearchOptions& options,
                                    bool documentMode,
                                    std::vector<float>& vector)
{
    if (text.empty())
        return false;

#ifdef _WIN32
    static bool wsaStarted = false;
    if (!wsaStarted) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
        wsaStarted = true;
    }
#endif

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string port = std::to_string(options.bgePort);
    if (getaddrinfo(options.bgeHost.c_str(), port.c_str(), &hints, &resolved) != 0 || !resolved)
        return false;

    intptr_t socketHandle = -1;
    for (addrinfo* cur = resolved; cur; cur = cur->ai_next) {
#ifdef _WIN32
        SOCKET s = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, cur->ai_addr, static_cast<int>(cur->ai_addrlen)) == 0) {
            socketHandle = static_cast<intptr_t>(s);
            break;
        }
        closesocket(s);
#else
        int s = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (s < 0)
            continue;
        if (connect(s, cur->ai_addr, cur->ai_addrlen) == 0) {
            socketHandle = static_cast<intptr_t>(s);
            break;
        }
        close(s);
#endif
    }
    freeaddrinfo(resolved);
    if (socketHandle == -1)
        return false;

    std::string requestPayload;
    if (documentMode)
        requestPayload = std::string(BGE_DOCUMENT_MARKER) + text;
    else
        requestPayload = text;
    if (requestPayload.size() > BGE_MAX_TEXT_BYTES)
        requestPayload.resize(BGE_MAX_TEXT_BYTES);

    const uint32_t length = static_cast<uint32_t>(requestPayload.size());
    bool ok = SocketSendAll(socketHandle, reinterpret_cast<const char*>(&length), sizeof(length))
        && SocketSendAll(socketHandle, requestPayload.data(), requestPayload.size());
    uint32_t dim = 0;
    std::array<int8_t, DOC_VECTOR_DIM> responsePayload{};
    ok = ok
        && SocketRecvAll(socketHandle, reinterpret_cast<char*>(&dim), sizeof(dim))
        && dim == DOC_VECTOR_DIM
        && SocketRecvAll(socketHandle, reinterpret_cast<char*>(responsePayload.data()), responsePayload.size());
    CloseSocketHandle(socketHandle);
    if (!ok)
        return false;

    vector.assign(DOC_VECTOR_DIM, 0.0f);
    for (size_t i = 0; i < DOC_VECTOR_DIM; ++i)
        vector[i] = static_cast<float>(responsePayload[i]) / 128.0f;
    return true;
}

static bool EmbedTextWithBge(const std::string& text,
                             const SearchOptions& options,
                             bool documentMode,
                             std::vector<float>& vector)
{
    if (!options.bgeSidecar) {
        if (EmbedTextWithBgeService(text, options, documentMode, vector))
            return true;
        std::cerr << "BGE embedding service unavailable at " << options.bgeHost << ":" << options.bgePort
                  << " (start Tools/bge_embedding_service.py or pass -bge-sidecar)\n";
        return false;
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tempDir = std::filesystem::temp_directory_path();
    const auto prefix = documentMode ? "moonshot_bge_doc_" : "moonshot_bge_query_";
    const auto textPath = tempDir / (std::string(prefix) + std::to_string(stamp) + ".txt");
    const auto vectorPath = tempDir / (std::string(prefix) + std::to_string(stamp) + ".i8bin");
    {
        std::ofstream out(textPath, std::ios::binary);
        if (!out) return false;
        out << text;
    }

    const std::string python = options.bgePython.empty() ? DefaultBgePython() : options.bgePython;
    const std::string script = options.bgeScript.empty() ? DefaultBgeScript() : options.bgeScript;
    const std::string command = QuoteShellArg(python)
        + " " + QuoteShellArg(script)
        + " --text-file " + QuoteShellArg(PathToUtf8(textPath))
        + " --output " + QuoteShellArg(PathToUtf8(vectorPath))
        + " --model " + QuoteShellArg(options.bgeModel)
        + (documentMode ? " --no-default-prefix" : "");
    const int exitCode = std::system(command.c_str());
    const bool ok = exitCode == 0 && ReadSingleI8Vector(PathToUtf8(vectorPath), vector);
    std::error_code ec;
    std::filesystem::remove(textPath, ec);
    ec.clear();
    std::filesystem::remove(vectorPath, ec);
    if (!ok)
        std::cerr << "BGE " << (documentMode ? "document" : "query") << " embedding failed; command was: " << command << "\n";
    return ok;
}

static bool EmbedQueryWithBge(const std::string& query,
                              const SearchOptions& options,
                              std::vector<float>& vector)
{
    return EmbedTextWithBge(query, options, false, vector);
}

static bool EmbedDocumentWithBge(const std::string& text,
                                 const SearchOptions& options,
                                 std::vector<float>& vector)
{
    return EmbedTextWithBge(text, options, true, vector);
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

    std::unique_ptr<IndexSearchExecutor> executor(ctx.GetExecutor());
    const std::string streams = SearchStreamSet(options);
    if (options.vector && options.bge) {
        std::vector<float> queryVector;
        if (!EmbedQueryWithBge(query, options, queryVector))
            return;

        std::unique_ptr<EvalTree> tree;
        if (!streams.empty()) {
            tree.reset(ctx.Compile(query.c_str(), streams.c_str(), QueryCompileMode::WeakAndBigram));
        } else {
            tree = std::make_unique<EvalTree>();
        }
        tree->vector_query = std::move(queryVector);
        tree->vector_ef_search = options.vectorEf;

        for (const auto& result : executor->Execute(ctx.GetReader(tree.get()), static_cast<int>(options.topK), &tree->vector_query))
            hits.push_back({&ctx, result});
        return;
    }

    if (!streams.empty()) {
        auto tree = std::unique_ptr<EvalTree>(ctx.Compile(query.c_str(), streams.c_str()));
        const std::vector<float>* vectorQuery = tree && tree->HasTextQuery() && tree->HasVectorQuery() ? &tree->vector_query : nullptr;
        for (const auto& result : executor->Execute(ctx.GetReader(tree.get()), 0, vectorQuery))
            hits.push_back({&ctx, result});
    }
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
        << "  /a <file>                  Add one document to memory; -bge also stores a BGE vector\n"
        << "  /a <dir> -e md,txt [-r]    Add documents from a directory; -bge also stores BGE vectors\n"
        << "  /s                         Save pending additions as delta and publish it\n"
        << "  /m                         Merge delta into the main index and reload\n"
        << "Queries:\n"
        << "  anything else              Search current base + delta\n";
}

static uint64_t NextInteractiveDocId(IndexContext& ctx)
{
    return ctx.AllocateDocumentID();
}

static bool AddInteractiveFiles(IndexContext& ctx,
                                const std::vector<FileItem>& files,
                                const SearchOptions& options,
                                uint64_t& kept,
                                uint64_t& skipped)
{
    uint64_t nextDocId = NextInteractiveDocId(ctx);
    std::string firstContent;
    uint64_t bgeVectors = 0;
    for (const auto& file : files) {
        if (!options.bge) {
            if (AddFileDocument(ctx, file, nextDocId, kept, skipped, &firstContent))
                ++nextDocId;
            continue;
        }

        std::string content = ReadFile(file.path);
        if (content.empty()) {
            std::cerr << "  skipping (empty/unreadable): " << file.path << "\n";
            ++skipped;
            continue;
        }

        if (firstContent.empty())
            firstContent = content;

        Document doc;
        doc.doc_id = nextDocId;
        doc.path = file.path;
        doc.title = Stem(file.path);
        doc.body = std::move(content);

        ctx.AddDocument(doc, false);

        std::string embeddingText = doc.title;
        if (!doc.body.empty()) {
            if (!embeddingText.empty())
                embeddingText.push_back('\n');
            embeddingText += doc.body;
        }
        if (embeddingText.size() > BGE_MAX_TEXT_BYTES - std::strlen(BGE_DOCUMENT_MARKER))
            embeddingText.resize(BGE_MAX_TEXT_BYTES - std::strlen(BGE_DOCUMENT_MARKER));

        std::vector<float> vector;
        if (EmbedDocumentWithBge(embeddingText, options, vector)) {
            ctx.GetWriter()->SetDocVector(nextDocId, std::move(vector));
            ++bgeVectors;
        } else {
            std::cerr << "  warning: BGE document embedding failed; added without vector: " << file.path << "\n";
        }
        ++kept;
        ++nextDocId;
    }
    if (options.bge)
        std::cout << "  embedded " << bgeVectors << " BGE document vector(s)\n";
    return kept > 0;
}

static bool HandleAddCommand(IndexContext& ctx, const SearchOptions& options, const std::vector<std::string>& args)
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
    AddInteractiveFiles(ctx, files, options, kept, skipped);
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
                                     const SearchOptions& options,
                                     const std::string& line,
                                     bool& shouldQuit)
{
    const auto args = SplitCommandLine(line);
    if (args.empty()) return true;
    if (args[0] == "/q") { shouldQuit = true; return true; }
    if (args[0] == "/h") { PrintInteractiveHelp(); return true; }
    if (args[0] == "/a") return HandleAddCommand(ctx, options, args);
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

    uint64_t totalDocuments = ctx.DocumentCount();
    if (auto* delta = ctx.GetDeltaContext())
        totalDocuments += delta->DocumentCount();

    std::cout << "moon search — "
              << totalDocuments
              << " document(s)"
              << " (loaded in " << loadMs << " ms)\n"
              << "Mode: " << SearchModeName(options) << "\n"
              << std::flush;

    if (options.vector && !options.inverted)
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
            HandleInteractiveCommand(ctx, idxPath, options, line, shouldQuit);
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
    SearchOptions embedding;
};

struct SampleMergeOptions {
    std::string dirPath;
    std::string outPath;
    std::vector<std::string> extensions = ParseExtensions("cpp,h,rs");
};

struct BeirBuildOptions {
    std::string dataPath;
    std::string docVectorsPath;
    uint64_t limit = 0;
    bool buildVectors = false;
};

struct BeirPatchVectorOptions {
    std::string sourceIndexPath;
    std::string docVectorsPath;
    uint64_t limit = 0;
};

struct BeirEvalOptions {
    std::string dataPath;
    std::string qrels = "test";
    std::string runOut;
    std::string dumpFeaturesPath;
    std::string queryVectorsPath;
    std::string streams = "TB";
    std::string mode = "weakandbigram";
    std::string weakAndShape = "flat";
    std::vector<int> at = {10, 100, 1000};
    uint64_t limit = 0;
    uint64_t vectorEf = 1000;
    bool noMphf = false;
    uint64_t leafCacheMb = 0;
    bool leafCacheMatchMphf = false;
};

static QueryCompileMode BeirCompileMode(const std::string& mode)
{
    if (mode == "weakandbigramboostdoc" || mode == "hybridboostdoc")
        return QueryCompileMode::WeakAndBigramBoostForDoc;
    if (mode == "weakandbigramboost" || mode == "hybridboost")
        return QueryCompileMode::WeakAndBigramBoost;
    return QueryCompileMode::WeakAndBigram;
}

static bool IsWeakAndBigramMode(const std::string& mode)
{
    return mode == "weakandbigram" || mode == "weakandbigramboost" || mode == "weakandbigramboostdoc";
}

static bool IsHybridMode(const std::string& mode)
{
    return mode == "hybrid" || mode == "hybridboost" || mode == "hybridboostdoc";
}

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

static float MoonDocDataPrior(const DocDataEntry& entry)
{
    const float docLength = static_cast<float>(std::max<uint32_t>(1, entry.DDE_BodyLength));
    static constexpr float TargetLogLength = 6.0f;
    static constexpr float Width = 4.0f;
    static constexpr float Weight = 0.15f;
    const float distance = std::abs(std::log2(docLength) - TargetLogLength);
    const float lengthQuality = std::max(0.0f, 1.0f - distance / Width);
    return Weight * lengthQuality
        + 0.10f * DocDataDecodeScore(entry.DDE_QualityScore)
        + 0.05f * DocDataDecodeScore(entry.DDE_AuthorityScore)
        - 0.10f * DocDataDecodeScore(entry.DDE_SpamScore);
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
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "-i") {
            options.inverted = true;
        } else if (arg == "-v") {
            options.vector = true;
        } else if (arg == "-bge") {
            options.bge = true;
            options.vector = true;
        } else if (arg == "-bge-sidecar") {
            options.bgeSidecar = true;
        } else if (arg == "-bge-python") {
            if (i + 1 >= args.size()) { error = "Usage: moon -v -bge -bge-python <python>"; return false; }
            options.bgePython = args[++i];
        } else if (arg == "-bge-script") {
            if (i + 1 >= args.size()) { error = "Usage: moon -v -bge -bge-script <embed_query.py>"; return false; }
            options.bgeScript = args[++i];
        } else if (arg == "-bge-model") {
            if (i + 1 >= args.size()) { error = "Usage: moon -v -bge -bge-model <model>"; return false; }
            options.bgeModel = args[++i];
        } else if (arg == "-bge-host") {
            if (i + 1 >= args.size()) { error = "Usage: moon -v -bge -bge-host <host>"; return false; }
            options.bgeHost = args[++i];
        } else if (arg == "-bge-port") {
            if (i + 1 >= args.size()) { error = "Usage: moon -v -bge -bge-port <port>"; return false; }
            uint64_t value = 0;
            if (!ParseUInt64(args[++i], value) || value == 0 || value > 65535) { error = "-bge-port must be 1..65535"; return false; }
            options.bgePort = static_cast<uint16_t>(value);
        } else if (arg == "-k") {
            if (i + 1 >= args.size()) { error = "Usage: moon -v -bge -k N"; return false; }
            uint64_t value = 0;
            if (!ParseUInt64(args[++i], value) || value == 0) { error = "-k must be positive"; return false; }
            options.topK = static_cast<size_t>(value);
        } else if (arg == "-ef") {
            if (i + 1 >= args.size()) { error = "Usage: moon -v -bge -ef N"; return false; }
            uint64_t value = 0;
            if (!ParseUInt64(args[++i], value) || value == 0) { error = "-ef must be positive"; return false; }
            options.vectorEf = static_cast<size_t>(value);
        } else {
            error = "Unknown search option: " + arg;
            return false;
        }
    }

    if (!options.inverted && !options.vector) {
        error = "Usage: moon [-idx <index>] -i [-v] | moon [-idx <index>] -v [-bge]";
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
        } else if (arg == "-bge") {
            options.embedding.bge = true;
        } else if (arg == "-bge-sidecar") {
            options.embedding.bgeSidecar = true;
        } else if (arg == "-bge-python") {
            if (i + 1 >= args.size()) { error = "Usage: moon -file <file> -bge -bge-python <python>"; return false; }
            options.embedding.bgePython = args[++i];
        } else if (arg == "-bge-script") {
            if (i + 1 >= args.size()) { error = "Usage: moon -file <file> -bge -bge-script <embed_query.py>"; return false; }
            options.embedding.bgeScript = args[++i];
        } else if (arg == "-bge-model") {
            if (i + 1 >= args.size()) { error = "Usage: moon -file <file> -bge -bge-model <model>"; return false; }
            options.embedding.bgeModel = args[++i];
        } else if (arg == "-bge-host") {
            if (i + 1 >= args.size()) { error = "Usage: moon -file <file> -bge -bge-host <host>"; return false; }
            options.embedding.bgeHost = args[++i];
        } else if (arg == "-bge-port") {
            if (i + 1 >= args.size()) { error = "Usage: moon -file <file> -bge -bge-port <port>"; return false; }
            uint64_t value = 0;
            if (!ParseUInt64(args[++i], value) || value == 0 || value > 65535) { error = "-bge-port must be 1..65535"; return false; }
            options.embedding.bgePort = static_cast<uint16_t>(value);
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
    error = "Usage: moon -file <filename> | moon -dir <directory> [-ext md,txt] [-r] [-b 10000] [-bge [-bge-host host] [-bge-port port]]";
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
        } else if (arg == "-doc-vectors") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-build -doc-vectors <docid-tab-vector.tsv>"; return false; }
            options.docVectorsPath = args[++i];
        } else if (arg == "-build-vectors") {
            options.buildVectors = true;
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

static bool ParseBeirPatchVectorOptions(const std::vector<std::string>& args,
                                        BeirPatchVectorOptions& options,
                                        std::string& error)
{
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-src-index") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-patch-vectors -src-index <index> -doc-vectors <vectors>"; return false; }
            options.sourceIndexPath = args[++i];
        } else if (arg == "-doc-vectors") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-patch-vectors -doc-vectors <vectors>"; return false; }
            options.docVectorsPath = args[++i];
        } else if (arg == "-limit") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-patch-vectors -limit N"; return false; }
            if (!ParseUInt64(args[++i], options.limit)) { error = "-limit must be a non-negative integer"; return false; }
        } else {
            error = "Unknown BEIR patch option: " + arg;
            return false;
        }
    }
    if (options.sourceIndexPath.empty() || options.docVectorsPath.empty()) {
        error = "Usage: moon [-idx <output-index>] -beir-patch-vectors -src-index <index> -doc-vectors <vectors>";
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
        } else if (arg == "-run-out") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -run-out <trec-run-path>"; return false; }
            options.runOut = args[++i];
        } else if (arg == "-dump-features") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -dump-features <features.tsv>"; return false; }
            options.dumpFeaturesPath = args[++i];
        } else if (arg == "-query-vectors") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -query-vectors <qid-tab-vector.tsv>"; return false; }
            options.queryVectorsPath = args[++i];
        } else if (arg == "-k") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -k 10,100,1000"; return false; }
            if (!ParseAtList(args[++i], options.at)) { error = "-k must be a comma-separated list of positive integers"; return false; }
        } else if (arg == "-streams") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -streams TB"; return false; }
            options.streams = args[++i];
        } else if (arg == "-mode") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -mode bow|weakandbigram|weakandbigramboost|weakandbigramboostdoc|vector|hybrid|hybridboost|hybridboostdoc|compile"; return false; }
            options.mode = args[++i];
            if (options.mode != "bow" && options.mode != "weakandbigram" && options.mode != "weakandbigramboost" && options.mode != "weakandbigramboostdoc" && options.mode != "vector" && options.mode != "hybrid" && options.mode != "hybridboost" && options.mode != "hybridboostdoc" && options.mode != "compile") { error = "-mode must be bow, weakandbigram, weakandbigramboost, weakandbigramboostdoc, vector, hybrid, hybridboost, hybridboostdoc, or compile"; return false; }
        } else if (arg == "-weakand-shape") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -weakand-shape flat|or|or-prune"; return false; }
            options.weakAndShape = args[++i];
            if (options.weakAndShape != "flat" && options.weakAndShape != "or" && options.weakAndShape != "or-prune") { error = "-weakand-shape must be flat, or, or-prune"; return false; }
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
        } else if (arg == "-vector-ef") {
            if (i + 1 >= args.size()) { error = "Usage: moon -beir-eval -vector-ef N"; return false; }
            if (!ParseUInt64(args[++i], options.vectorEf) || options.vectorEf == 0) { error = "-vector-ef must be a positive integer"; return false; }
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
        << "  moon [-idx <index>] -file <file> [-bge [-bge-host 127.0.0.1] [-bge-port 8765]]\n"
        << "      Index one file. With -bge, store document vectors from the configured BGE service.\n\n"
        << "  moon [-idx <index>] -dir <dir> -ext md,txt [-r] [-b 10000] [-bge [-bge-host 127.0.0.1] [-bge-port 8765]]\n"
        << "      Index files under <dir>. Use -r for recursive traversal.\n"
        << "      -ext is a comma-separated extension list without or with dots.\n"
        << "      -b controls how many new files are saved per delta batch; minimum 10000.\n"
        << "      With -bge, store document vectors from the configured BGE service.\n\n"
        << "Search an index:\n"
        << "  moon [-idx <index>] -i\n"
        << "      Open an interactive inverted-index search prompt.\n\n"
        << "  moon [-idx <index>] -v\n"
        << "      Open an interactive vector search prompt.\n\n"
        << "  moon [-idx <index>] -v -bge [-bge-host 127.0.0.1] [-bge-port 8765] [-k 1000] [-ef 1000]\n"
        << "      Open BGE vector search using the configured BGE embedding service.\n"
        << "      Use -bge-sidecar only for debugging one Python process per query.\n\n"
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
        << "  moon -idx <output-index> -beir-patch-vectors -src-index <index> -doc-vectors <vectors.i8bin>\n"
        << "      Copy an existing BEIR index and patch only DocData vector fields.\n\n"
        << "  moon [-idx <index>] -beir-eval -data <beir-dir> [-qrels test] [-run-out out.trec] [-k 10,100,1000] [-streams TB] [-mode bow|weakandbigram|weakandbigramboost|weakandbigramboostdoc|vector|hybrid|hybridboost|hybridboostdoc|compile] [-weakand-shape flat|or|or-prune] [-no-mphf] [-leaf-cache-mb N] [-leaf-cache-match-mphf] [-limit N]\n"
        << "      Evaluate Recall@k from BEIR queries.jsonl and qrels/<split>.tsv.\n"
        << "      Default mode is weakandbigram. bow is kept as a recall-ceiling baseline.\n\n"
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
using ExternalVector = std::array<float, DOC_VECTOR_DIM>;
using ExternalVectorMap = std::unordered_map<std::string, ExternalVector>;

struct ExternalVectorStream {
    std::ifstream input;
    bool binary = false;
    uint32_t dim = static_cast<uint32_t>(DOC_VECTOR_DIM);
    uint32_t idBytes = 0;
};
static bool OpenExternalVectorStream(const std::string& path, ExternalVectorStream& stream);
static bool ReadExternalVectorRecord(ExternalVectorStream& stream, std::string& id, ExternalVector& vector);
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

static bool ParseExternalVector(const std::string& text, ExternalVector& vector)
{
    vector.fill(0.0f);
    size_t dim = 0;
    size_t start = 0;
    while (start <= text.size() && dim < DOC_VECTOR_DIM) {
        const size_t comma = text.find(',', start);
        const std::string_view piece(text.data() + start,
            (comma == std::string::npos ? text.size() : comma) - start);
        if (!piece.empty()) {
            char* end = nullptr;
            std::string value(piece);
            const float parsed = std::strtof(value.c_str(), &end);
            if (!end || end == value.c_str())
                return false;
            vector[dim++] = parsed;
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }

    float norm = 0.0f;
    for (float value : vector)
        norm += value * value;
    if (norm <= 0.0f)
        return false;
    norm = std::sqrt(norm);
    for (float& value : vector)
        value /= norm;
    return true;
}

static bool LoadExternalVectors(const std::string& path, ExternalVectorMap& vectors)
{
    if (path.empty())
        return true;

    ExternalVectorStream stream;
    if (OpenExternalVectorStream(path, stream) && stream.binary) {
        std::string id;
        ExternalVector vector{};
        while (ReadExternalVectorRecord(stream, id, vector))
            vectors.emplace(id, vector);
        return true;
    }

    std::ifstream input(FsPathFromUtf8(path));
    if (!input)
        return false;

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty())
            continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
            continue;
        ExternalVector vector{};
        if (ParseExternalVector(line.substr(tab + 1), vector))
            vectors.emplace(line.substr(0, tab), vector);
    }
    return true;
}

static bool OpenExternalVectorStream(const std::string& path, ExternalVectorStream& stream)
{
    static constexpr char Magic[8] = {'M','S','V','E','C','I','8','1'};
    stream = {};
    stream.input.open(FsPathFromUtf8(path), std::ios::binary);
    if (!stream.input)
        return false;

    char magic[8]{};
    stream.input.read(magic, sizeof(magic));
    if (stream.input.gcount() == sizeof(magic) && std::memcmp(magic, Magic, sizeof(Magic)) == 0) {
        stream.binary = true;
        stream.input.read(reinterpret_cast<char*>(&stream.dim), sizeof(stream.dim));
        stream.input.read(reinterpret_cast<char*>(&stream.idBytes), sizeof(stream.idBytes));
        return stream.input.good()
            && stream.dim == DOC_VECTOR_DIM
            && stream.idBytes > 0
            && stream.idBytes <= 1024;
    }

    stream.input.close();
    stream.input.open(FsPathFromUtf8(path));
    return static_cast<bool>(stream.input);
}

static bool ReadExternalVectorRecord(std::istream& input, std::string& id, ExternalVector& vector)
{
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty())
            continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
            continue;
        ExternalVector parsed{};
        if (!ParseExternalVector(line.substr(tab + 1), parsed))
            continue;
        id = line.substr(0, tab);
        vector = parsed;
        return true;
    }
    return false;
}

static bool ReadExternalVectorRecord(ExternalVectorStream& stream, std::string& id, ExternalVector& vector)
{
    if (!stream.binary)
        return ReadExternalVectorRecord(stream.input, id, vector);

    std::vector<char> idBuffer(stream.idBytes);
    std::array<int8_t, DOC_VECTOR_DIM> payload{};
    stream.input.read(idBuffer.data(), static_cast<std::streamsize>(idBuffer.size()));
    if (!stream.input)
        return false;
    stream.input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!stream.input)
        return false;

    const auto nul = std::find(idBuffer.begin(), idBuffer.end(), '\0');
    id.assign(idBuffer.begin(), nul);
    for (size_t i = 0; i < DOC_VECTOR_DIM; ++i)
        vector[i] = static_cast<float>(payload[i]) / 128.0f;
    return !id.empty();
}

static std::vector<float> ToVector(const ExternalVector& vector)
{
    return std::vector<float>(vector.begin(), vector.end());
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

enum class BeirEvalPath {
    FeatureDump,
    Bow,
    WeakAndBigram,
    VectorOrHybrid,
    DefaultReader,
};

static BeirEvalPath SelectBeirEvalPath(const BeirEvalOptions& options, bool dumpFeatures)
{
    if (dumpFeatures)
        return BeirEvalPath::FeatureDump;
    if (options.mode == "bow")
        return BeirEvalPath::Bow;
    if (IsWeakAndBigramMode(options.mode))
        return BeirEvalPath::WeakAndBigram;
    if (options.mode == "vector" || IsHybridMode(options.mode))
        return BeirEvalPath::VectorOrHybrid;
    return BeirEvalPath::DefaultReader;
}

static void WriteBeirRun(std::ofstream* output,
                         IndexContext& ctx,
                         const std::string& qid,
                         const std::vector<SearchResult>& results,
                         const std::string& tag)
{
    if (!output || !*output)
        return;

    for (size_t rank = 0; rank < results.size(); ++rank) {
        const std::string docId = ctx.GetDocPath(results[rank].doc_id);
        if (docId.empty())
            continue;
        *output << qid << " Q0 " << docId << ' ' << (rank + 1) << ' '
                << std::setprecision(9) << results[rank].score << ' ' << tag << '\n';
    }
}

static void AddBeirRecallForResults(IndexContext& ctx,
                                    const std::vector<SearchResult>& results,
                                    const std::unordered_set<std::string>& relevant,
                                    const std::vector<int>& at,
                                    std::vector<double>& macroRecall,
                                    std::vector<uint64_t>& microHits,
                                    uint64_t& microRelevant)
{
    std::vector<uint64_t> cumulativeHits(at.size(), 0);
    uint64_t hitCount = 0;
    size_t nextAt = 0;
    for (size_t rank = 0; rank < results.size(); ++rank) {
        const std::string docIdText = ctx.GetDocPath(results[rank].doc_id);
        if (relevant.count(docIdText))
            ++hitCount;
        while (nextAt < at.size() && rank + 1 == static_cast<size_t>(at[nextAt])) {
            cumulativeHits[nextAt] = hitCount;
            ++nextAt;
        }
    }
    while (nextAt < at.size()) {
        cumulativeHits[nextAt] = hitCount;
        ++nextAt;
    }

    const uint64_t relevantCount = static_cast<uint64_t>(relevant.size());
    microRelevant += relevantCount;
    for (size_t i = 0; i < at.size(); ++i) {
        macroRecall[i] += static_cast<double>(cumulativeHits[i]) / static_cast<double>(relevantCount);
        microHits[i] += cumulativeHits[i];
    }
}

struct BeirCandidateFeature {
    const DocDataEntry* entry = nullptr;
    std::string docIdText;
    float weakScore = 0.0f;
    float bigramScore = 0.0f;
    uint8_t weakSourceMask = 0;
    uint8_t bigramSourceMask = 0;
};

static float RawBranchScore(const SearchResult& result, const DocDataEntry& entry)
{
    return result.score - DocDataDecodeScore(entry.DDE_StaticRank) - MoonDocDataPrior(entry);
}

static void AddFeatureRows(IndexContext& ctx,
                           std::unordered_map<uint64_t, BeirCandidateFeature>& rows,
                           const std::vector<SearchResult>& results,
                           bool isBigramBranch)
{
    for (const auto& result : results) {
        const uint64_t docId = ReaderDocumentIDValue(result.doc_id);
        const auto* entry = ctx.GetDocDataEntry(docId);
        if (!entry)
            continue;
        auto& row = rows[docId];
        row.entry = entry;
        if (row.docIdText.empty())
            row.docIdText = ctx.GetDocPath(docId);
        const float rawScore = RawBranchScore(result, *entry);
        if (isBigramBranch) {
            row.bigramScore = std::max(row.bigramScore, rawScore);
            row.bigramSourceMask |= ReaderDocumentIDSourceMask(result.doc_id);
        } else {
            row.weakScore = std::max(row.weakScore, rawScore);
            row.weakSourceMask |= ReaderDocumentIDSourceMask(result.doc_id);
        }
    }
}

static void AddFeatureRowsFromReader(IndexContext& ctx,
                                     std::unordered_map<uint64_t, BeirCandidateFeature>& rows,
                                     std::shared_ptr<IndexReader> reader,
                                     bool isBigramBranch)
{
    while (reader && !reader->IsEnd()) {
        const uint64_t docId = reader->GetDocumentID();
        const auto* entry = ctx.GetDocDataEntry(docId);
        if (entry) {
            auto& row = rows[docId];
            row.entry = entry;
            if (row.docIdText.empty())
                row.docIdText = ctx.GetDocPath(docId);
            const float rawScore = reader->GetScore(entry);
            if (isBigramBranch) {
                row.bigramScore = std::max(row.bigramScore, rawScore);
                row.bigramSourceMask |= reader->GetSourceMask();
            } else {
                row.weakScore = std::max(row.weakScore, rawScore);
                row.weakSourceMask |= reader->GetSourceMask();
            }
        }
        reader->GoNext();
    }
}

static void SplitWeakAndBigramRoots(EvalTree* tree,
                                    std::shared_ptr<EvalNode>& weakRoot,
                                    std::shared_ptr<EvalNode>& bigramRoot)
{
    if (!tree)
        return;
    if (tree->root && tree->root->GetType() == NodeType::Or) {
        auto* orNode = static_cast<OrNode*>(tree->root.get());
        if (!orNode->children.empty())
            weakRoot = orNode->children[0];
        if (orNode->children.size() > 1)
            bigramRoot = orNode->children[1];
    } else {
        weakRoot = tree->root;
    }
}

static std::unordered_map<uint64_t, BeirCandidateFeature> CollectBeirCandidateFeatures(
    IndexContext& ctx,
    EvalTree* tree)
{
    std::shared_ptr<EvalNode> weakRoot;
    std::shared_ptr<EvalNode> bigramRoot;
    SplitWeakAndBigramRoots(tree, weakRoot, bigramRoot);

    std::unordered_map<uint64_t, BeirCandidateFeature> rows;
    if (weakRoot) {
        EvalTree weakTree;
        weakTree.root = weakRoot;
        AddFeatureRowsFromReader(ctx, rows, ctx.GetReader(&weakTree), false);
    }
    if (bigramRoot) {
        EvalTree bigramTree;
        bigramTree.root = bigramRoot;
        AddFeatureRowsFromReader(ctx, rows, ctx.GetReader(&bigramTree), true);
    }
    return rows;
}

static void WriteFeatureRows(std::ofstream& output,
                             const BeirQrels& qrels,
                             const std::string& qid,
                             const std::unordered_map<uint64_t, BeirCandidateFeature>& rows)
{
    std::vector<uint64_t> docIds;
    docIds.reserve(rows.size());
    for (const auto& [docId, _] : rows)
        docIds.push_back(docId);
    std::sort(docIds.begin(), docIds.end());

    const auto qrelIt = qrels.find(qid);
    for (const uint64_t docId : docIds) {
        const auto& row = rows.at(docId);
        if (!row.entry || row.docIdText.empty())
            continue;
        const bool label = qrelIt != qrels.end() && qrelIt->second.count(row.docIdText) != 0;
        const float prior = MoonDocDataPrior(*row.entry);
        output << qid << '\t'
               << row.docIdText << '\t'
               << (label ? 1 : 0) << '\t'
               << row.weakScore << '\t'
               << row.bigramScore << '\t'
               << (row.weakScore != 0.0f ? 1 : 0) << '\t'
               << (row.bigramScore != 0.0f ? 1 : 0) << '\t'
               << static_cast<uint32_t>(row.weakSourceMask) << '\t'
               << static_cast<uint32_t>(row.bigramSourceMask) << '\t'
               << DocDataDecodeScore(row.entry->DDE_StaticRank) << '\t'
               << prior << '\t'
               << row.entry->DDE_BodyLength << '\t'
               << DocDataDecodeScore(row.entry->DDE_QualityScore) << '\t'
               << DocDataDecodeScore(row.entry->DDE_AuthorityScore) << '\t'
               << DocDataDecodeScore(row.entry->DDE_SpamScore) << '\t'
               << row.entry->DDE_TitleLength << '\t'
               << row.entry->DDE_BodyLength << '\t'
               << row.entry->DDE_DiversityScore << '\t'
               << row.entry->DDE_LengthQualityScore << '\n';
    }
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

    ExternalVectorStream docVectorInput;
    if (!options.docVectorsPath.empty()) {
        if (!OpenExternalVectorStream(options.docVectorsPath, docVectorInput)) {
            std::cerr << "Could not load document vectors: " << options.docVectorsPath << "\n";
            return 1;
        }
        std::cout << "  streaming external document vectors: " << options.docVectorsPath << "\n";
    }

    std::error_code ec;
    std::filesystem::remove(FsPathFromUtf8(idxPath), ec);
    ec.clear();
    std::filesystem::remove(FsPathFromUtf8(DeltaIndexPath(idxPath)), ec);

    IndexContext ctx("", "", false);
    std::string line;
    uint64_t docId = 0;
    uint64_t skipped = 0;
    uint64_t vectorDocs = 0;
    auto start = std::chrono::steady_clock::now();
    while (std::getline(corpus, line)) {
        if (options.limit > 0 && docId >= options.limit) break;
        std::string id;
        std::string url;
        std::string title;
        std::string text;
        if (!ExtractJsonString(line, "_id", id) || !ExtractJsonString(line, "text", text)) {
            ++skipped;
            continue;
        }
        ExtractJsonString(line, "url", url);
        ExtractJsonString(line, "title", title);
        Document doc;
        doc.doc_id = docId;
        doc.path = id;
        doc.url = url;
        doc.title = title;
        doc.body = text;
        doc.importance = 0.1f;
        const bool useBuiltInVector = options.buildVectors && !docVectorInput.input.is_open();
        ctx.AddDocument(doc, useBuiltInVector);
        if (docVectorInput.input.is_open()) {
            std::string vectorId;
            ExternalVector vector{};
            if (!ReadExternalVectorRecord(docVectorInput, vectorId, vector)) {
                std::cerr << "Document vector file ended before corpus doc " << id << "\n";
                return 1;
            }
            if (vectorId != id) {
                std::cerr << "Document vector id mismatch: expected " << id << " got " << vectorId << "\n";
                return 1;
            }
            auto writer = ctx.GetWriter();
            writer->SetDocVector(docId, ToVector(vector));
            ++vectorDocs;
        }
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
    if (docVectorInput.input.is_open() || options.buildVectors) std::cout << " vector_docs=" << vectorDocs;
    if (skipped) std::cout << " skipped=" << skipped;
    std::cout << " elapsed_ms=" << elapsedMs << "\n";
    return 0;
}

static int RunBeirPatchVectors(const std::string& idxPath, const BeirPatchVectorOptions& options)
{
    IndexFileHeader header{};
    if (!ReadIndexHeaderOnly(options.sourceIndexPath, header)) {
        std::cerr << "Source index not found or invalid: " << options.sourceIndexPath << "\n";
        return 1;
    }

    if (idxPath == options.sourceIndexPath) {
        std::cerr << "Output index must differ from source index for vector patching\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::copy_file(FsPathFromUtf8(options.sourceIndexPath),
                               FsPathFromUtf8(idxPath),
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        std::cerr << "Failed to copy source index: " << ec.message() << "\n";
        return 1;
    }

    ExternalVectorStream vectors;
    if (!OpenExternalVectorStream(options.docVectorsPath, vectors)) {
        std::cerr << "Could not open vector file: " << options.docVectorsPath << "\n";
        return 1;
    }

    std::fstream output(FsPathFromUtf8(idxPath), std::ios::binary | std::ios::in | std::ios::out);
    if (!output) {
        std::cerr << "Could not open output index for patching: " << idxPath << "\n";
        return 1;
    }

    uint64_t patched = 0;
    for (uint64_t docId = 0; docId < header.IFH_NumDocuments; ++docId) {
        if (options.limit > 0 && patched >= options.limit)
            break;

        std::string vectorId;
        ExternalVector vector{};
        if (!ReadExternalVectorRecord(vectors, vectorId, vector)) {
            std::cerr << "Vector file ended before docId " << docId << "\n";
            return 1;
        }

        const uint64_t entryOffset = header.IFH_DocDataOffset + docId * DOC_REC_SIZE;
        const uint64_t dimOffset = entryOffset + offsetof(DocDataEntry, DDE_VectorDim);
        const uint64_t formatOffset = entryOffset + offsetof(DocDataEntry, DDE_VectorFormat);
        const uint64_t dataOffset = entryOffset + offsetof(DocDataEntry, DDE_VectorData);

        const uint16_t dim = static_cast<uint16_t>(DOC_VECTOR_DIM);
        const uint16_t format = 1;
        output.seekp(dimOffset);
        output.write(reinterpret_cast<const char*>(&dim), sizeof(dim));
        output.seekp(formatOffset);
        output.write(reinterpret_cast<const char*>(&format), sizeof(format));

        std::array<int8_t, DOC_VECTOR_DIM> quantized{};
        for (size_t i = 0; i < DOC_VECTOR_DIM; ++i) {
            const float clipped = std::max(-128.0f, std::min(127.0f, vector[i] * 128.0f));
            quantized[i] = static_cast<int8_t>(std::round(clipped));
        }
        output.seekp(dataOffset);
        output.write(reinterpret_cast<const char*>(quantized.data()), DOC_VECTOR_STORAGE_MAX_DIM);
        if (!output) {
            std::cerr << "Failed while patching docId " << docId << "\n";
            return 1;
        }
        ++patched;
        if (patched % 100000 == 0)
            std::cout << "  patched " << patched << " vectors\n";
    }

    std::cout << "BEIR vector patch complete docs=" << patched << " output=" << idxPath << "\n";
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

    std::ofstream runOutput;
    if (!options.runOut.empty()) {
        runOutput.open(FsPathFromUtf8(options.runOut));
        if (!runOutput) {
            std::cerr << "Could not open BEIR run output: " << options.runOut << "\n";
            return 1;
        }
    }

    std::ofstream featureOutput;
    if (!options.dumpFeaturesPath.empty()) {
        if (options.mode != "weakandbigram") {
            std::cerr << "-dump-features currently supports -mode weakandbigram only\n";
            return 1;
        }
        featureOutput.open(FsPathFromUtf8(options.dumpFeaturesPath));
        if (!featureOutput) {
            std::cerr << "Could not open BEIR feature dump: " << options.dumpFeaturesPath << "\n";
            return 1;
        }
        featureOutput << "qid\tdocid\tlabel\tweak_score\tbigram_score\tweak_hit\tbigram_hit\tweak_source\tbigram_source"
                      << "\tstatic_rank\tdoc_prior\tdoc_len\tquality\tauthority\tspam\ttitle_len\tbody_len\tdiversity\tlength_quality\n";
    }

    ExternalVectorMap queryVectors;
    if (!options.queryVectorsPath.empty()) {
        if (!LoadExternalVectors(options.queryVectorsPath, queryVectors)) {
            std::cerr << "Could not load query vectors: " << options.queryVectorsPath << "\n";
            return 1;
        }
        std::cout << "  loaded external query vectors: " << queryVectors.size() << "\n";
    }

    const uint64_t leafCacheBytes = BeirEvalLeafCacheBytes(idxPath, options);
    IndexContext ctx("", "", false);
    if (leafCacheBytes > 0)
        ctx.SetLeafTermCacheBytes(leafCacheBytes);
    ctx.LoadIndex(idxPath.c_str());
    ctx.SetTermMphfEnabled(!options.noMphf);
    ctx.SetDirectBlockAccessEnabled(true);
    const QueryCompileMode compileMode = BeirCompileMode(options.mode);
    auto parameters = GetQueryCompileModeParameters(compileMode);
    ctx.SetQueryParameters(parameters);
    IndexSearchExecutor::SetScoringParameters(parameters);
    WeakAndBuildMode weakAndBuildMode = WeakAndBuildMode::FlatPruned;
    if (options.weakAndShape == "or")
        weakAndBuildMode = WeakAndBuildMode::OrChildren;
    else if (options.weakAndShape == "or-prune")
        weakAndBuildMode = WeakAndBuildMode::OrChildrenPruned;
    ctx.SetWeakAndBuildMode(weakAndBuildMode);
    std::unique_ptr<IndexSearchExecutor> executor(ctx.GetExecutor());
    SmartTokenizer beirTokenizer;
    const int maxK = *std::max_element(options.at.begin(), options.at.end());
    std::vector<double> macroRecall(options.at.size(), 0.0);
    std::vector<uint64_t> microHits(options.at.size(), 0);
    uint64_t microRelevant = 0;
    uint64_t evaluated = 0;
    uint64_t missingQrels = 0;
    const bool dumpFeatures = featureOutput.is_open();
    const BeirEvalPath evalPath = SelectBeirEvalPath(options, dumpFeatures);
    const bool isHybridEval = IsHybridMode(options.mode);
    const bool hasExternalQueryVectors = !queryVectors.empty();
    std::ofstream* runWriter = options.runOut.empty() ? nullptr : &runOutput;
    const std::string runTag = "moon-" + options.mode;

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

        if (evalPath == BeirEvalPath::FeatureDump) {
            auto tree = std::unique_ptr<EvalTree>(ctx.Compile(query.c_str(), options.streams.c_str(), QueryCompileMode::WeakAndBigram));
            const auto rows = CollectBeirCandidateFeatures(ctx, tree.get());

            WriteFeatureRows(featureOutput, qrels, qid, rows);
            ++evaluated;
            if (evaluated % 100 == 0)
                std::cout << "  BEIR dumped " << evaluated << " queries\n";
            continue;
        }

        std::vector<SearchResult> results;
        switch (evalPath) {
        case BeirEvalPath::Bow: {
            results = executor->Execute(BuildBeirBowReader(ctx, beirTokenizer, query, options.streams), maxK);
            break;
        }
        case BeirEvalPath::WeakAndBigram: {
            auto tree = std::unique_ptr<EvalTree>(ctx.Compile(query.c_str(), options.streams.c_str(), compileMode));
            results = executor->Execute(ctx.GetReader(tree.get()), maxK,
                tree->HasTextQuery() && tree->HasVectorQuery() ? &tree->vector_query : nullptr);
            break;
        }
        case BeirEvalPath::VectorOrHybrid: {
            std::vector<float> queryVector;
            auto vectorIt = queryVectors.find(qid);
            if (vectorIt != queryVectors.end()) {
                queryVector = ToVector(vectorIt->second);
            } else if (!hasExternalQueryVectors) {
                queryVector = ctx.CompileToVector(query.c_str());
            }

            std::unique_ptr<EvalTree> tree;
            if (isHybridEval) {
                tree.reset(ctx.Compile(query.c_str(), options.streams.c_str(), compileMode));
            } else {
                tree = std::make_unique<EvalTree>();
            }
            if (!queryVector.empty())
                tree->vector_query = std::move(queryVector);
            tree->vector_ef_search = static_cast<size_t>(options.vectorEf);

            results = executor->Execute(ctx.GetReader(tree.get()), maxK,
                tree->HasTextQuery() && tree->HasVectorQuery() ? &tree->vector_query : nullptr);

            if (results.size() > static_cast<size_t>(maxK))
                results.resize(static_cast<size_t>(maxK));
            break;
        }
        case BeirEvalPath::DefaultReader: {
            results = executor->Execute(ctx.GetReader(query.c_str(), options.streams.c_str()), maxK);
            break;
        }
        case BeirEvalPath::FeatureDump:
            break;
        }

        WriteBeirRun(runWriter, ctx, qid, results, runTag);
        AddBeirRecallForResults(ctx, results, qrelIt->second, options.at, macroRecall, microHits, microRelevant);
        ++evaluated;
        if (evaluated % 100 == 0)
            std::cout << "  BEIR evaluated " << evaluated << " queries\n";
    }

    if (dumpFeatures) {
        std::cout << "BEIR feature dump index=" << idxPath
                  << " data=" << options.dataPath
                  << " qrels=" << qrelsPath
                  << " streams=" << options.streams
                  << " mode=" << options.mode
                  << " bigram_weight=" << parameters.QMP_BigramWeight
                  << " queries=" << evaluated
                  << " output=" << options.dumpFeaturesPath << "\n";
        return 0;
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
              << " weakand_shape=" << options.weakAndShape
              << " bigram_weight=" << parameters.QMP_BigramWeight
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
        if (!BuildIndexFile(batchPath, batchMap, options.embedding, kept, skipped, &reportedSkippedPaths)) {
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

    } else if (cmd == "-beir-patch-vectors") {
        BeirPatchVectorOptions options;
        if (!ParseBeirPatchVectorOptions(args, options, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return RunBeirPatchVectors(idxPath, options);

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
