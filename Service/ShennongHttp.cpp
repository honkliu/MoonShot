#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include "moonshot.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

#ifdef _WIN32
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
#else
using socket_t = int;
static constexpr socket_t INVALID_SOCKET_FD = -1;
#endif

static constexpr size_t MAX_REQUEST_BYTES = 2 * 1024 * 1024;
static constexpr size_t MAX_PAGE_SIZE = 100;
static constexpr size_t MAX_EF_SEARCH = 1000000;
static constexpr size_t BGE_MAX_TEXT_BYTES = 65536;
static constexpr size_t MAX_DOCUMENT_BYTES = 1024 * 1024;
static constexpr long BGE_TIMEOUT_MS = 2000;

struct Options {
    std::string ListenAddress = "127.0.0.1";
    uint16_t Port = 9000;
    std::string IndexPath;
    std::string WebPath;
    std::string BgeHost = "127.0.0.1";
    uint16_t BgePort = 8765;
};

enum class SearchMode {
    Text,
    Vector,
    Hybrid,
};

struct SearchRequest {
    SearchMode Mode = SearchMode::Text;
    std::string Query;
    std::vector<float> Vector;
    std::string Streams = "AUTB";
    size_t Offset = 0;
    size_t Limit = 20;
    size_t EfSearch = 0;
};

struct SearchHit {
    size_t Rank = 0;
    uint64_t DocumentId = 0;
    float Score = 0.0f;
    std::string Path;
};

struct SearchResponse {
    SearchMode Mode = SearchMode::Text;
    double TookMs = 0.0;
    size_t Offset = 0;
    size_t Limit = 0;
    bool HasMore = false;
    std::vector<SearchHit> Results;
};

class ApiError : public std::runtime_error {
public:
    ApiError(int status, std::string code, std::string message)
        : std::runtime_error(std::move(message))
        , Status(status)
        , Code(std::move(code))
    {}

    int Status;
    std::string Code;
};

#ifdef _WIN32
class SocketRuntime {
public:
    SocketRuntime()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("WSAStartup failed");
    }

    ~SocketRuntime()
    {
        WSACleanup();
    }
};
#endif

static std::string HomeDirectory()
{
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    return home ? home : "C:/Users/Default";
#else
    const char* home = std::getenv("HOME");
    return home ? home : "/tmp";
#endif
}

static std::string DefaultIndexPath()
{
    return (std::filesystem::path(HomeDirectory()) / "moon.idx").string();
}

static std::string ExpandUserPath(const std::string& path)
{
    if (path == "~")
        return HomeDirectory();
    if (path.starts_with("~/") || path.starts_with("~\\"))
        return (std::filesystem::path(HomeDirectory()) / path.substr(2)).string();
    return path;
}

static uint16_t ParsePort(const std::string& value, std::string_view option)
{
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || !end || *end != '\0' || parsed < 1 || parsed > 65535)
        throw std::runtime_error("invalid " + std::string(option) + " value");
    return static_cast<uint16_t>(parsed);
}

static Options ParseArguments(int argc, char** argv)
{
    Options options;
    options.IndexPath = DefaultIndexPath();
    options.WebPath = (std::filesystem::absolute(argv[0]).parent_path() / "web").string();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            options.ListenAddress = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            options.Port = ParsePort(argv[++i], arg);
        } else if (arg == "--index" && i + 1 < argc) {
            options.IndexPath = ExpandUserPath(argv[++i]);
        } else if (arg == "--ui" && i + 1 < argc) {
            options.WebPath = ExpandUserPath(argv[++i]);
        } else if ((arg == "--bge-host" || arg == "--gbe-host") && i + 1 < argc) {
            options.BgeHost = argv[++i];
        } else if ((arg == "--bge-port" || arg == "--gbe-port") && i + 1 < argc) {
            options.BgePort = ParsePort(argv[++i], arg);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: shennong [--listen 127.0.0.1] [--port 9000] [--index ~/moon.idx] "
                         "[--ui <web-directory>] "
                         "[--bge-host 127.0.0.1] [--bge-port 8765]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + arg);
        }
    }

    return options;
}

using EmbeddingDeadline = std::chrono::steady_clock::time_point;

static void CloseSocket(socket_t socket)
{
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

static bool WouldBlock()
{
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

static bool WaitForSocket(socket_t socket, bool writable, EmbeddingDeadline deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::microseconds::zero())
        return false;

    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(socket, &descriptors);
    timeval timeout{
        static_cast<long>(remaining.count() / 1000000),
        static_cast<long>(remaining.count() % 1000000),
    };
    return select(static_cast<int>(socket + 1),
                  writable ? nullptr : &descriptors,
                  writable ? &descriptors : nullptr,
                  nullptr,
                  &timeout) > 0;
}

static bool SendAll(socket_t socket,
                    const void* data,
                    size_t length,
                    EmbeddingDeadline deadline)
{
    const char* current = static_cast<const char*>(data);
    while (length > 0) {
        if (!WaitForSocket(socket, true, deadline))
            return false;
#ifdef _WIN32
        const int sent = send(socket, current,
                              static_cast<int>(std::min<size_t>(length, 64 * 1024)), 0);
#else
    const ssize_t sent = send(socket, current, length, MSG_NOSIGNAL);
#endif
    if (sent < 0 && WouldBlock())
        continue;
        if (sent <= 0)
            return false;
        current += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

static bool ConnectWithTimeout(socket_t socket,
                               const sockaddr* address,
                               socklen_t addressLength,
                               EmbeddingDeadline deadline)
{
#ifdef _WIN32
    u_long nonBlocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonBlocking) != 0)
        return false;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0)
        return false;
#endif

    const int result = connect(socket, address, addressLength);
    if (result != 0) {
#ifdef _WIN32
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
            return false;
#else
        if (errno != EINPROGRESS)
            return false;
#endif

        if (!WaitForSocket(socket, true, deadline))
            return false;

        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socketError), &errorLength) != 0
            || socketError != 0) {
            return false;
        }
    }

    return true;
}

static bool ReceiveAll(socket_t socket,
                       void* data,
                       size_t length,
                       EmbeddingDeadline deadline)
{
    char* current = static_cast<char*>(data);
    while (length > 0) {
        if (!WaitForSocket(socket, false, deadline))
            return false;
#ifdef _WIN32
        const int received = recv(socket, current,
                                  static_cast<int>(std::min<size_t>(length, 64 * 1024)), 0);
#else
        const ssize_t received = recv(socket, current, length, 0);
#endif
    if (received < 0 && WouldBlock())
        continue;
        if (received <= 0)
            return false;
        current += received;
        length -= static_cast<size_t>(received);
    }
    return true;
}

class BgeEmbeddingClient {
public:
    BgeEmbeddingClient(std::string host, uint16_t port)
        : m_Host(std::move(host))
        , m_Port(port)
    {
        m_Address.sin_family = AF_INET;
        m_Address.sin_port = htons(m_Port);
        if (inet_pton(AF_INET, m_Host.c_str(), &m_Address.sin_addr) != 1)
            throw std::runtime_error("--bge-host must be a numeric IPv4 address");
    }

    std::optional<std::vector<float>> Embed(std::string_view text) const
    {
        if (text.empty() || text.size() > BGE_MAX_TEXT_BYTES)
            return std::nullopt;

        const EmbeddingDeadline deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(BGE_TIMEOUT_MS);
        const socket_t socket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket == INVALID_SOCKET_FD)
            return std::nullopt;
        if (!ConnectWithTimeout(socket,
                                reinterpret_cast<const sockaddr*>(&m_Address),
                                sizeof(m_Address),
                                deadline)) {
            CloseSocket(socket);
            return std::nullopt;
        }

        const size_t payloadSize = text.size();
        const uint32_t length = static_cast<uint32_t>(payloadSize);
        uint32_t dimension = 0;
        std::array<int8_t, DOC_VECTOR_DIM> encoded{};
        const bool success = SendAll(socket, &length, sizeof(length), deadline)
            && SendAll(socket, text.data(), payloadSize, deadline)
            && ReceiveAll(socket, &dimension, sizeof(dimension), deadline)
            && dimension == DOC_VECTOR_DIM
            && ReceiveAll(socket, encoded.data(), encoded.size(), deadline);
        CloseSocket(socket);

        if (!success)
            return std::nullopt;

        std::vector<float> vector(DOC_VECTOR_DIM);
        for (size_t i = 0; i < vector.size(); ++i)
            vector[i] = static_cast<float>(encoded[i]) / 128.0f;
        return vector;
    }

    const std::string& Host() const { return m_Host; }
    uint16_t Port() const { return m_Port; }

private:
    std::string m_Host;
    uint16_t m_Port;
    sockaddr_in m_Address{};
};

static std::string_view SearchModeName(SearchMode mode)
{
    switch (mode) {
    case SearchMode::Text: return "text";
    case SearchMode::Vector: return "vector";
    case SearchMode::Hybrid: return "hybrid";
    }
    return "unknown";
}

static SearchMode ParseSearchMode(const json& body)
{
    if (!body.contains("mode") || !body["mode"].is_string())
        throw ApiError(422, "invalid_mode", "mode must be text, vector, or hybrid");

    const std::string mode = body["mode"].get<std::string>();
    if (mode == "text")
        return SearchMode::Text;
    if (mode == "vector")
        return SearchMode::Vector;
    if (mode == "hybrid")
        return SearchMode::Hybrid;
    throw ApiError(422, "invalid_mode", "mode must be text, vector, or hybrid");
}

static size_t ReadSize(const json& body,
                       std::string_view name,
                       size_t defaultValue,
                       size_t minimum,
                       size_t maximum)
{
    if (!body.contains(name))
        return defaultValue;

    const json& value = body.at(name);
    uint64_t parsed = 0;
    if (value.is_number_unsigned()) {
        parsed = value.get<uint64_t>();
    } else if (value.is_number_integer()) {
        const int64_t signedValue = value.get<int64_t>();
        if (signedValue < 0)
            throw ApiError(422, "invalid_pagination", std::string(name) + " is out of range");
        parsed = static_cast<uint64_t>(signedValue);
    } else {
        throw ApiError(422, "invalid_pagination", std::string(name) + " must be an integer");
    }

    if (parsed < minimum || parsed > maximum)
        throw ApiError(422, "invalid_pagination", std::string(name) + " is out of range");
    return static_cast<size_t>(parsed);
}

static std::string ParseFields(const json& body)
{
    if (!body.contains("fields"))
        return "AUTB";
    if (!body["fields"].is_array() || body["fields"].empty())
        throw ApiError(422, "invalid_fields", "fields must be a non-empty array");

    std::string streams;
    for (const json& fieldValue : body["fields"]) {
        if (!fieldValue.is_string())
            throw ApiError(422, "invalid_fields", "every field must be a string");

        const std::string field = fieldValue.get<std::string>();
        char stream = '\0';
        if (field == "anchor") stream = 'A';
        else if (field == "url") stream = 'U';
        else if (field == "title") stream = 'T';
        else if (field == "body") stream = 'B';
        else if (field == "meta") stream = 'M';
        else throw ApiError(422, "invalid_fields", "unknown search field: " + field);

        if (streams.find(stream) == std::string::npos)
            streams.push_back(stream);
    }
    return streams;
}

static std::vector<float> ParseVector(const json& body)
{
    if (!body.contains("vector"))
        return {};
    if (!body["vector"].is_array() || body["vector"].empty())
        throw ApiError(422, "invalid_vector", "vector must be a non-empty array");

    std::vector<float> vector;
    vector.reserve(body["vector"].size());
    for (const json& component : body["vector"]) {
        if (!component.is_number())
            throw ApiError(422, "invalid_vector", "every vector component must be numeric");
        const double value = component.get<double>();
        if (!std::isfinite(value)
            || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
            throw ApiError(422, "invalid_vector", "every vector component must be finite");
        }
        vector.push_back(static_cast<float>(value));
    }
    return vector;
}

static SearchRequest ParseSearchRequest(const json& body, size_t documentCount)
{
    if (!body.is_object())
        throw ApiError(400, "invalid_request", "request body must be a JSON object");

    static constexpr std::array<std::string_view, 7> allowedFields{
        "mode", "query", "vector", "fields", "offset", "limit", "ef_search"
    };
    for (const auto& [name, _] : body.items()) {
        if (std::find(allowedFields.begin(), allowedFields.end(), name) == allowedFields.end())
            throw ApiError(400, "unknown_property", "unknown request property: " + name);
    }

    SearchRequest request;
    request.Mode = ParseSearchMode(body);
    if (body.contains("query")) {
        if (!body["query"].is_string())
            throw ApiError(422, "invalid_query", "query must be a string");
        request.Query = body["query"].get<std::string>();
    }
    request.Vector = ParseVector(body);
    request.Offset = ReadSize(body, "offset", 0, 0, documentCount - 1);
    request.Limit = ReadSize(body, "limit", 20, 1, MAX_PAGE_SIZE);

    const bool hasQuery = !request.Query.empty();
    const bool hasVector = !request.Vector.empty();
    const bool hasFields = body.contains("fields");
    const bool hasEfSearch = body.contains("ef_search");

    switch (request.Mode) {
    case SearchMode::Text:
        if (!hasQuery || hasVector)
            throw ApiError(422, "invalid_text_request", "text mode requires query and rejects vector");
        if (hasEfSearch)
            throw ApiError(422, "invalid_text_request", "ef_search is only valid in vector mode");
        request.Streams = ParseFields(body);
        break;

    case SearchMode::Vector:
        if (hasQuery == hasVector)
            throw ApiError(422, "invalid_vector_request", "vector mode requires exactly one of query or vector");
        if (hasFields)
            throw ApiError(422, "invalid_vector_request", "fields are not valid in vector mode");
        request.EfSearch = ReadSize(body, "ef_search",
                                    std::max<size_t>(200, request.Offset + request.Limit + 1),
                                    1, MAX_EF_SEARCH);
        if (request.EfSearch < request.Offset + request.Limit + 1)
            throw ApiError(422, "invalid_ef_search", "ef_search must cover the requested result window");
        break;

    case SearchMode::Hybrid:
        if (!hasQuery)
            throw ApiError(422, "invalid_hybrid_request", "hybrid mode requires query");
        if (hasEfSearch)
            throw ApiError(422, "invalid_hybrid_request", "ef_search does not affect current hybrid retrieval");
        request.Streams = ParseFields(body);
        break;
    }

    return request;
}

class SearchService {
public:
    SearchService(std::string indexPath, std::string bgeHost, uint16_t bgePort)
        : m_IndexPath(std::move(indexPath))
        , m_Embedding(std::move(bgeHost), bgePort)
        , m_Context("", m_IndexPath.c_str())
    {
        if (m_Context.DocumentCount() == 0)
            throw std::runtime_error("index loaded with zero documents: " + m_IndexPath);
    }

    SearchResponse Search(SearchRequest request)
    {
        const auto started = std::chrono::steady_clock::now();

        if ((request.Mode == SearchMode::Vector || request.Mode == SearchMode::Hybrid)
            && request.Vector.empty()) {
            if (request.Query.size() > BGE_MAX_TEXT_BYTES)
                throw ApiError(422, "query_too_long", "query is too large for embedding");
            auto vector = m_Embedding.Embed(request.Query);
            if (!vector)
                throw ApiError(503, "embedding_unavailable", "query embedding is unavailable");
            request.Vector = std::move(*vector);
        }

        if (!request.Vector.empty()) {
            const size_t dimension = m_Context.VectorDimension();
            if (dimension > 0 && request.Vector.size() != dimension) {
                throw ApiError(422, "invalid_vector_dimension",
                               "expected " + std::to_string(dimension)
                               + " dimensions but received "
                               + std::to_string(request.Vector.size()));
            }
            double squaredNorm = 0.0;
            for (float component : request.Vector)
                squaredNorm += static_cast<double>(component) * component;
            if (!(squaredNorm > 0.0) || !std::isfinite(squaredNorm))
                throw ApiError(422, "invalid_vector", "vector must have a finite non-zero magnitude");
            if (dimension == 0 || m_Context.VectorCount() == 0)
                throw ApiError(503, "vector_search_unavailable", "the loaded index has no vector index");
        }

        const size_t resultWindow = std::min<size_t>(m_Context.TotalDocumentCount(),
                             request.Offset + request.Limit + 1);
        const std::string query = request.Mode == SearchMode::Vector ? "" : request.Query;
        auto task = m_Context.Enqueue(query.c_str(),
                                      std::move(request.Vector),
                                      request.Streams.c_str(),
                                      static_cast<int>(resultWindow),
                                      QueryCompileMode::WeakAndBigramBoostForDoc,
                                      request.EfSearch);
        const std::vector<SearchResult> results = task.Wait();

        SearchResponse response;
        response.Mode = request.Mode;
        response.Offset = request.Offset;
        response.Limit = request.Limit;
        response.HasMore = results.size() > request.Offset + request.Limit;

        const size_t end = std::min(results.size(), request.Offset + request.Limit);
        response.Results.reserve(end > request.Offset ? end - request.Offset : 0);
        for (size_t i = request.Offset; i < end; ++i) {
            const SearchResult& result = results[i];
            const uint64_t documentId = ReaderDocumentIDValue(result.doc_id);
            response.Results.push_back({
                i + 1,
                documentId,
                result.score,
                m_Context.GetDocPath(documentId),
            });
        }

        response.TookMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return response;
    }

    json ReadyStatus() const
    {
        const IndexFileHeader& header = m_Context.GetIndexFileHeader();
        return {
            {"status", "ready"},
            {"index", m_IndexPath},
            {"documents", header.IFH_NumDocuments},
            {"avg_doc_len", header.IFH_AvgDocLength},
            {"vector_count", m_Context.VectorCount()},
            {"vector_dim", m_Context.VectorDimension()},
            {"embedding", {
                {"configured", true},
                {"host", m_Embedding.Host()},
                {"port", m_Embedding.Port()},
            }},
        };
    }

    size_t DocumentCount() const
    {
        return static_cast<size_t>(m_Context.TotalDocumentCount());
    }

    json GetDocument(uint64_t documentId) const
    {
        const std::string path = m_Context.GetDocPath(documentId);
        if (path.empty())
            throw ApiError(404, "document_not_found", "document was not found");
        if (path.starts_with("http://") || path.starts_with("https://")) {
            return {
                {"document_id", std::to_string(documentId)},
                {"kind", "url"},
                {"url", path},
            };
        }

        const std::filesystem::path filePath(path);
        std::error_code error;
        if (!std::filesystem::is_regular_file(filePath, error) || error)
            throw ApiError(404, "document_not_found", "document file is unavailable");

        const uintmax_t fileSize = std::filesystem::file_size(filePath, error);
        if (error)
            throw ApiError(404, "document_not_found", "document file is unavailable");
        const size_t bytes = static_cast<size_t>(std::min<uintmax_t>(fileSize, MAX_DOCUMENT_BYTES));
        std::ifstream input(filePath, std::ios::binary);
        if (!input)
            throw ApiError(403, "document_unreadable", "document file cannot be read");
        std::string content(bytes, '\0');
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<size_t>(input.gcount()));
        if (content.find('\0') != std::string::npos)
            throw ApiError(415, "unsupported_document_type", "document is not a text file");

        std::string contentType = "text/plain; charset=utf-8";
        std::string extension = filePath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension == ".md" || extension == ".markdown")
            contentType = "text/markdown; charset=utf-8";
        else if (extension == ".html" || extension == ".htm")
            contentType = "text/html; charset=utf-8";

        return {
            {"document_id", std::to_string(documentId)},
            {"kind", "file"},
            {"path", path},
            {"content_type", contentType},
            {"content", std::move(content)},
            {"truncated", fileSize > bytes},
        };
    }

private:
    std::string m_IndexPath;
    BgeEmbeddingClient m_Embedding;
    IndexContext m_Context;
};

static std::string MakeRequestId()
{
    static std::atomic<uint64_t> sequence{0};
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(now) + "-" + std::to_string(sequence.fetch_add(1));
}

static json SearchResponseJson(const SearchResponse& response, const std::string& requestId)
{
    json results = json::array();
    for (const SearchHit& hit : response.Results) {
        json result{
            {"rank", hit.Rank},
            {"document_id", std::to_string(hit.DocumentId)},
            {"score", std::isfinite(hit.Score) ? json(hit.Score) : json(nullptr)},
            {"path", hit.Path.empty() ? json(nullptr) : json(hit.Path)},
        };
        results.push_back(std::move(result));
    }

    return {
        {"request_id", requestId},
        {"mode", SearchModeName(response.Mode)},
        {"took_ms", response.TookMs},
        {"offset", response.Offset},
        {"limit", response.Limit},
        {"returned", response.Results.size()},
        {"has_more", response.HasMore},
        {"next_offset", response.HasMore
            ? json(response.Offset + response.Results.size())
            : json(nullptr)},
        {"results", std::move(results)},
    };
}

static void SetJsonResponse(httplib::Response& response, int status, const json& body)
{
    response.status = status;
    response.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace),
                         "application/json; charset=utf-8");
}

static void SetErrorResponse(httplib::Response& response,
                             int status,
                             const std::string& requestId,
                             std::string_view code,
                             std::string_view message)
{
    SetJsonResponse(response, status, {
        {"request_id", requestId},
        {"error", {
            {"code", code},
            {"message", message},
        }},
    });
}

static uint64_t ParseDocumentId(const std::string& value)
{
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (value.empty() || !end || *end != '\0' || errno == ERANGE)
        throw ApiError(400, "invalid_document_id", "document ID must be an unsigned integer");
    return static_cast<uint64_t>(parsed);
}

static void ConfigureServer(httplib::Server& server,
                            SearchService& service,
                            const std::string& webPath)
{
    server.set_payload_max_length(MAX_REQUEST_BYTES);
    server.set_default_headers({{"Cache-Control", "no-store"}});
    server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        response.set_redirect("/ui/");
    });
    if (!server.set_mount_point("/ui", webPath))
        throw std::runtime_error("failed to mount web UI directory: " + webPath);

    server.Get("/v1/health/live", [](const httplib::Request&, httplib::Response& response) {
        SetJsonResponse(response, 200, {{"status", "alive"}});
    });

    server.Get("/v1/health/ready", [&service](const httplib::Request&, httplib::Response& response) {
        SetJsonResponse(response, 200, service.ReadyStatus());
    });

    server.Post("/v1/search", [&service](const httplib::Request& request,
                                         httplib::Response& response) {
        const std::string requestId = MakeRequestId();
        try {
            const json body = json::parse(request.body);
            SearchRequest searchRequest = ParseSearchRequest(body, service.DocumentCount());
            SetJsonResponse(response, 200,
                            SearchResponseJson(service.Search(std::move(searchRequest)), requestId));
        } catch (const json::parse_error&) {
            SetErrorResponse(response, 400, requestId, "invalid_json", "request body is not valid JSON");
        } catch (const ApiError& error) {
            SetErrorResponse(response, error.Status, requestId, error.Code, error.what());
        } catch (const std::exception& error) {
            std::cerr << "request " << requestId << " failed: " << error.what() << '\n';
            SetErrorResponse(response, 500, requestId, "internal_error", "search failed");
        }
    });

    server.Get(R"(/v1/documents/([0-9]+))", [&service](const httplib::Request& request,
                                                       httplib::Response& response) {
        const std::string requestId = MakeRequestId();
        try {
            const json document = service.GetDocument(ParseDocumentId(request.matches[1].str()));
            if (request.has_param("raw") && request.get_param_value("raw") == "1"
                && document.at("kind") == "file") {
                response.status = 200;
                response.set_content(document.at("content").get<std::string>(),
                                     "text/plain; charset=utf-8");
            } else {
                SetJsonResponse(response, 200, document);
            }
        } catch (const ApiError& error) {
            SetErrorResponse(response, error.Status, requestId, error.Code, error.what());
        } catch (const std::exception& error) {
            std::cerr << "request " << requestId << " failed: " << error.what() << '\n';
            SetErrorResponse(response, 500, requestId, "internal_error", "document retrieval failed");
        }
    });

    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (response.status == 404) {
            SetErrorResponse(response, 404, MakeRequestId(), "not_found", "endpoint not found");
        } else if (response.status == 405) {
            SetErrorResponse(response, 405, MakeRequestId(), "method_not_allowed", "method not allowed");
        }
    });
}

} // namespace

int main(int argc, char** argv)
{
    try {
#ifdef _WIN32
        SocketRuntime socketRuntime;
#endif
        const Options options = ParseArguments(argc, argv);
        if (!std::filesystem::is_regular_file(options.IndexPath)) {
            std::cerr << "index not found: " << options.IndexPath << '\n';
            return 2;
        }

        std::cout << "ShenNong HTTP service starting\n"
                  << "Index: " << options.IndexPath << '\n'
                  << "Listen: " << options.ListenAddress << ':' << options.Port << '\n';

        SearchService service(options.IndexPath, options.BgeHost, options.BgePort);
        httplib::Server server;
        ConfigureServer(server, service, options.WebPath);

        std::cout << "Search UI: http://localhost:" << options.Port << "/ui/\n"
                  << "Ready: http://localhost:" << options.Port << "/v1/health/ready\n";
        if (!server.listen(options.ListenAddress, options.Port))
            throw std::runtime_error("failed to bind HTTP server");
    } catch (const std::exception& error) {
        std::cerr << "shennong: " << error.what() << '\n';
        return 1;
    }
    return 0;
}