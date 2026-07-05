#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

#include "IndexContext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t INVALID_SOCKET_FD = -1;
#endif

namespace {

struct Options {
    uint16_t port = 9000;
    std::string index_path;
    std::string gbe_host = "127.0.0.1";
    uint16_t gbe_port = 8765;
};

static constexpr size_t GBE_MAX_TEXT_BYTES = 65536;

struct ParsedRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> query;
};

static std::string home_dir()
{
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    return home ? home : "C:/Users/Default";
#else
    const char* home = std::getenv("HOME");
    return home ? home : "/tmp";
#endif
}

static std::string default_index_path()
{
    return (std::filesystem::path(home_dir()) / "moon.idx").string();
}

static std::string expand_user_path(const std::string& path)
{
    if (path == "~") return home_dir();
    if (path.rfind("~/", 0) == 0 || path.rfind("~\\", 0) == 0) {
        return (std::filesystem::path(home_dir()) / path.substr(2)).string();
    }
    return path;
}

static bool parse_port(const std::string& value, uint16_t& port)
{
    if (value.empty()) return false;
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > 65535) return false;
    port = static_cast<uint16_t>(parsed);
    return true;
}

static Options parse_args(int argc, char** argv)
{
    Options options;
    options.index_path = default_index_path();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            if (!parse_port(argv[++i], options.port)) {
                throw std::runtime_error("invalid --port value");
            }
        } else if (arg == "--index" && i + 1 < argc) {
            options.index_path = expand_user_path(argv[++i]);
        } else if ((arg == "--gbe-host" || arg == "--bge-host") && i + 1 < argc) {
            options.gbe_host = argv[++i];
        } else if ((arg == "--gbe-port" || arg == "--bge-port") && i + 1 < argc) {
            if (!parse_port(argv[++i], options.gbe_port)) {
                throw std::runtime_error("invalid " + arg + " value");
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: shennong [--port 9000] [--index ~/moon.idx] [--gbe-host 127.0.0.1] [--gbe-port 8765]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + arg);
        }
    }

    return options;
}

static std::string url_decode(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '+') {
            out.push_back(' ');
        } else if (input[i] == '%' && i + 2 < input.size()) {
            const auto hex = input.substr(i + 1, 2);
            char* end = nullptr;
            long value = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                out.push_back(static_cast<char>(value));
                i += 2;
            } else {
                out.push_back(input[i]);
            }
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

static std::unordered_map<std::string, std::string> parse_query(const std::string& query)
{
    std::unordered_map<std::string, std::string> values;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string part = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!part.empty()) {
            size_t eq = part.find('=');
            std::string key = url_decode(part.substr(0, eq));
            std::string value = eq == std::string::npos ? "" : url_decode(part.substr(eq + 1));
            values[std::move(key)] = std::move(value);
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return values;
}

static ParsedRequest parse_http_request(const std::string& request)
{
    std::istringstream in(request);
    std::string target;
    std::string version;
    ParsedRequest parsed;
    in >> parsed.method >> target >> version;

    size_t question = target.find('?');
    parsed.path = question == std::string::npos ? target : target.substr(0, question);
    if (question != std::string::npos) parsed.query = parse_query(target.substr(question + 1));
    return parsed;
}

static std::string json_escape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char ch : input) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                static const char* digits = "0123456789abcdef";
                out += "\\u00";
                out.push_back(digits[(ch >> 4) & 0x0f]);
                out.push_back(digits[ch & 0x0f]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    return out;
}

static int query_int(const std::unordered_map<std::string, std::string>& query,
                     const std::string& key,
                     int default_value,
                     int min_value,
                     int max_value)
{
    auto it = query.find(key);
    if (it == query.end()) return default_value;
    char* end = nullptr;
    long value = std::strtol(it->second.c_str(), &end, 10);
    if (!end || *end != '\0') return default_value;
    value = std::max<long>(min_value, std::min<long>(max_value, value));
    return static_cast<int>(value);
}

static std::vector<float> parse_vector_param(const std::string& value)
{
    std::vector<float> vector;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        std::string part = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) {
            char* end = nullptr;
            float parsed = std::strtof(part.c_str(), &end);
            if (end && *end == '\0') vector.push_back(parsed);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return vector;
}

static void close_socket(socket_t fd)
{
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static bool send_all(socket_t fd, const std::string& data)
{
    const char* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
#ifdef _WIN32
        int sent = send(fd, ptr, static_cast<int>(std::min<size_t>(remaining, 64 * 1024)), 0);
#else
        ssize_t sent = send(fd, ptr, remaining, 0);
#endif
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

static bool send_all_bytes(socket_t fd, const char* data, size_t bytes)
{
    const char* ptr = data;
    size_t remaining = bytes;
    while (remaining > 0) {
#ifdef _WIN32
        int sent = send(fd, ptr, static_cast<int>(std::min<size_t>(remaining, 64 * 1024)), 0);
#else
        ssize_t sent = send(fd, ptr, remaining, 0);
#endif
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

static bool recv_all_bytes(socket_t fd, char* data, size_t bytes)
{
    char* ptr = data;
    size_t remaining = bytes;
    while (remaining > 0) {
#ifdef _WIN32
        int got = recv(fd, ptr, static_cast<int>(std::min<size_t>(remaining, 64 * 1024)), 0);
#else
        ssize_t got = recv(fd, ptr, remaining, 0);
#endif
        if (got <= 0) return false;
        ptr += got;
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

static bool embed_text_with_gbe_service(const std::string& text,
                                        const std::string& host,
                                        uint16_t port,
                                        std::vector<float>& vector)
{
    if (text.empty()) return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &resolved) != 0 || !resolved)
        return false;

    socket_t fd = INVALID_SOCKET_FD;
    for (addrinfo* cur = resolved; cur; cur = cur->ai_next) {
        socket_t candidate = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (candidate == INVALID_SOCKET_FD) continue;
        if (connect(candidate, cur->ai_addr, static_cast<int>(cur->ai_addrlen)) == 0) {
            fd = candidate;
            break;
        }
        close_socket(candidate);
    }
    freeaddrinfo(resolved);
    if (fd == INVALID_SOCKET_FD) return false;

    std::string payload = text;
    if (payload.size() > GBE_MAX_TEXT_BYTES)
        payload.resize(GBE_MAX_TEXT_BYTES);

    const uint32_t length = static_cast<uint32_t>(payload.size());
    uint32_t dim = 0;
    std::array<int8_t, DOC_VECTOR_DIM> response{};
    const bool ok = send_all_bytes(fd, reinterpret_cast<const char*>(&length), sizeof(length))
        && send_all_bytes(fd, payload.data(), payload.size())
        && recv_all_bytes(fd, reinterpret_cast<char*>(&dim), sizeof(dim))
        && dim == DOC_VECTOR_DIM
        && recv_all_bytes(fd, reinterpret_cast<char*>(response.data()), response.size());
    close_socket(fd);
    if (!ok) return false;

    vector.assign(DOC_VECTOR_DIM, 0.0f);
    for (size_t i = 0; i < DOC_VECTOR_DIM; ++i)
        vector[i] = static_cast<float>(response[i]) / 128.0f;
    return true;
}

static std::string http_response(int status, const std::string& status_text,
                                 const std::string& body,
                                 const std::string& content_type = "application/json; charset=utf-8")
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << status_text << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

class SearchService {
public:
    explicit SearchService(std::string index_path, std::string gbe_host, uint16_t gbe_port)
        : m_IndexPath(std::move(index_path))
        , m_GbeHost(std::move(gbe_host))
        , m_GbePort(gbe_port)
        , m_Context("", m_IndexPath.c_str())
    {
        if (m_Context.DocumentCount() == 0) {
            throw std::runtime_error("index loaded with zero docs or failed to load: " + m_IndexPath);
        }
    }

    std::string health_json()
    {
        std::ostringstream out;
        const IndexFileHeader& header = m_Context.GetIndexFileHeader();
        out << "{\"status\":\"ok\",\"index\":\"" << json_escape(m_IndexPath) << "\""
            << ",\"gbe_host\":\"" << json_escape(m_GbeHost) << "\""
            << ",\"gbe_port\":" << m_GbePort
            << ",\"documents\":" << header.IFH_NumDocuments
            << ",\"avg_doc_len\":" << header.IFH_AvgDocLength
            << ",\"vector_count\":" << m_Context.VectorCount()
            << ",\"vector_dim\":" << m_Context.VectorDimension()
            << "}";
        return out.str();
    }

    std::string search_json(const std::unordered_map<std::string, std::string>& params)
    {
        auto qit = params.find("q");
        const std::string query = qit == params.end() ? "" : qit->second;
        if (query.empty()) {
            return "{\"error\":\"missing q parameter\"}";
        }

        const auto streamsIt = params.find("streams");
        const std::string streams = streamsIt != params.end() && !streamsIt->second.empty()
            ? streamsIt->second
            : "AUTB";
        const int offset = query_int(params, "offset", 0, 0, 1000000000);
        const int limit = query_int(params, "limit", 20, 1, 1000);
        const int efSearch = query_int(params, "efSearch", 200, 1, 1000000);

        const auto started = std::chrono::steady_clock::now();

        std::vector<float> vector;
        const bool vectorReady = embed_text_with_gbe_service(query, m_GbeHost, m_GbePort, vector);
        auto task = m_Context.Enqueue(query.c_str(), vectorReady ? std::move(vector) : std::vector<float>{}, streams.c_str(), 0);
        std::vector<SearchResult> results = task.Wait();

        const auto finished = std::chrono::steady_clock::now();
        const double elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
        const int total = static_cast<int>(std::min<size_t>(results.size(), static_cast<size_t>(INT32_MAX)));
        const int begin = std::min(offset, total);
        const int end = std::min(begin + limit, total);

        std::ostringstream out;
        out << "{\"query\":\"" << json_escape(query) << "\""
            << ",\"streams\":\"" << json_escape(streams) << "\""
            << ",\"total\":" << total
            << ",\"offset\":" << begin
            << ",\"limit\":" << limit
            << ",\"elapsed_ms\":" << elapsed_ms
            << ",\"vector_ready\":" << (vectorReady ? "true" : "false")
            << ",\"results\":[";

        for (int i = begin; i < end; ++i) {
            if (i > begin) out << ',';
            const auto& result = results[static_cast<size_t>(i)];
            const std::string path = m_Context.GetDocPath(result.doc_id);
            out << "{\"rank\":" << (i + 1)
                << ",\"doc_id\":" << result.doc_id
                << ",\"score\":" << result.score
                << ",\"path\":\"" << json_escape(path) << "\"}";
        }

        out << "]}";
        return out.str();
    }

    std::string vector_search_json(const std::unordered_map<std::string, std::string>& params)
    {
        const int offset = query_int(params, "offset", 0, 0, 1000000000);
        const int limit = query_int(params, "limit", 20, 1, 1000);
        const int efSearch = query_int(params, "efSearch", 200, 1, 1000000);
        std::string queryText;
        std::unique_ptr<EvalTree> tree;

        auto vit = params.find("vector");
        if (vit != params.end() && !vit->second.empty()) {
            tree = std::make_unique<EvalTree>();
            tree->vector_query = parse_vector_param(vit->second);
        } else {
            auto qit = params.find("q");
            queryText = qit == params.end() ? "" : qit->second;
            if (queryText.empty()) return "{\"error\":\"missing q or vector parameter\"}";
            tree.reset(m_Context.Compile(queryText.c_str(), "V"));
        }

        if (!tree || !tree->HasVectorQuery()) return "{\"error\":\"empty query vector\"}";
        tree->vector_ef_search = static_cast<size_t>(efSearch);

        const auto started = std::chrono::steady_clock::now();
        const size_t vectorDim = tree->vector_query.size();
        auto task = m_Context.Enqueue("", std::move(tree->vector_query), "AUTB", 0);
        std::vector<SearchResult> results = task.Wait();
        const auto finished = std::chrono::steady_clock::now();
        const double elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
        const int total = static_cast<int>(std::min<size_t>(results.size(), static_cast<size_t>(INT32_MAX)));
        const int begin = std::min(offset, total);
        const int end = std::min(begin + limit, total);

        std::ostringstream out;
        out << "{\"query\":\"" << json_escape(queryText) << "\""
            << ",\"vector_dim\":" << vectorDim
            << ",\"vector_count\":" << m_Context.VectorCount()
            << ",\"efSearch\":" << efSearch
            << ",\"total\":" << total
            << ",\"offset\":" << begin
            << ",\"limit\":" << limit
            << ",\"elapsed_ms\":" << elapsed_ms
            << ",\"results\":[";
        for (int i = begin; i < end; ++i) {
            if (i > begin) out << ',';
            const auto& result = results[static_cast<size_t>(i)];
            const std::string path = m_Context.GetDocPath(result.doc_id);
            out << "{\"rank\":" << (i + 1)
                << ",\"doc_id\":" << result.doc_id
                << ",\"score\":" << result.score
                << ",\"path\":\"" << json_escape(path) << "\"}";
        }
        out << "]}";
        return out.str();
    }

private:
    std::string m_IndexPath;
    std::string m_GbeHost;
    uint16_t m_GbePort = 8765;
    IndexContext m_Context;
};

static std::string handle_request(SearchService& service, const ParsedRequest& request)
{
    if (request.method == "OPTIONS") {
        return http_response(204, "No Content", "");
    }
    if (request.method != "GET") {
        return http_response(405, "Method Not Allowed", "{\"error\":\"method not allowed\"}");
    }
    if (request.path == "/health") {
        return http_response(200, "OK", service.health_json());
    }
    if (request.path == "/search") {
        const auto body = service.search_json(request.query);
        const int status = body.rfind("{\"error\"", 0) == 0 ? 400 : 200;
        return http_response(status, status == 200 ? "OK" : "Bad Request", body);
    }
    if (request.path == "/vector-search") {
        const auto body = service.vector_search_json(request.query);
        const int status = body.rfind("{\"error\"", 0) == 0 ? 400 : 200;
        return http_response(status, status == 200 ? "OK" : "Bad Request", body);
    }
    if (request.path == "/" || request.path == "/help") {
        return http_response(200, "OK", "{\"service\":\"shennong\",\"endpoints\":[\"/health\",\"/search?q=usage&offset=0&limit=20&streams=AUTB\",\"/vector-search?q=usage&offset=0&limit=20\"]}");
    }
    return http_response(404, "Not Found", "{\"error\":\"not found\"}");
}

static socket_t create_listen_socket(uint16_t port)
{
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif

    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_FD) throw std::runtime_error("socket() failed");

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(fd);
        throw std::runtime_error("bind() failed");
    }
    if (listen(fd, 128) != 0) {
        close_socket(fd);
        throw std::runtime_error("listen() failed");
    }

    return fd;
}

static void serve_one_client(socket_t client, SearchService& service)
{
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 64 * 1024) {
#ifdef _WIN32
        int n = recv(client, buffer, sizeof(buffer), 0);
#else
        ssize_t n = recv(client, buffer, sizeof(buffer), 0);
#endif
        if (n <= 0) break;
        request.append(buffer, buffer + n);
    }

    try {
        auto response = handle_request(service, parse_http_request(request));
        send_all(client, response);
    } catch (const std::exception& ex) {
        send_all(client, http_response(500, "Internal Server Error", std::string("{\"error\":\"") + json_escape(ex.what()) + "\"}"));
    }

    close_socket(client);
}

static void serve_forever(SearchService& service, uint16_t port)
{
    socket_t server = create_listen_socket(port);
    std::cout << "Ready: http://localhost:" << port << "/search?q=usage&offset=0&limit=20\n";

    while (true) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int len = sizeof(client_addr);
#else
        socklen_t len = sizeof(client_addr);
#endif
        socket_t client = accept(server, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client == INVALID_SOCKET_FD) continue;
        std::thread([client, &service] { serve_one_client(client, service); }).detach();
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Options options = parse_args(argc, argv);
        if (!std::filesystem::is_regular_file(options.index_path)) {
            std::cerr << "index not found: " << options.index_path << "\n";
            return 2;
        }

        std::cout << "ShenNong HTTP service starting\n"
                  << "Index: " << options.index_path << "\n"
                  << "Listen: 0.0.0.0:" << options.port << "\n";

        SearchService service(options.index_path, options.gbe_host, options.gbe_port);
        std::cout << "Index loaded: " << service.health_json() << "\n";
        serve_forever(service, options.port);
    } catch (const std::exception& ex) {
        std::cerr << "shennong: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
