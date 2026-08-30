#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

using json = nlohmann::json;

struct Options {
    std::string Host = "127.0.0.1";
    int Port = 9000;
    std::string Mode = "text";
    size_t Limit = 20;
};

struct Result {
    size_t Rank = 0;
    std::string DocumentId;
    std::string Path;
    double Score = 0.0;
};

static Options ParseArguments(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            options.Host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            options.Port = std::atoi(argv[++i]);
        } else if (arg == "--mode" && i + 1 < argc) {
            options.Mode = argv[++i];
        } else if (arg == "--limit" && i + 1 < argc) {
            options.Limit = static_cast<size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: shennong_cli [--host 127.0.0.1] [--port 9000] "
                         "[--mode text|vector|hybrid] [--limit 20]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + arg);
        }
    }
    if (options.Port < 1 || options.Port > 65535)
        throw std::runtime_error("port is out of range");
    if (options.Mode != "text" && options.Mode != "vector" && options.Mode != "hybrid")
        throw std::runtime_error("mode must be text, vector, or hybrid");
    if (options.Limit < 1 || options.Limit > 100)
        throw std::runtime_error("limit must be between 1 and 100");
    return options;
}

static bool ParseResultReference(const std::string& line, size_t& number)
{
    if (line.size() < 2 || line[0] != '@')
        return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(line.c_str() + 1, &end, 10);
    if (!end || *end != '\0' || value == 0)
        return false;
    number = static_cast<size_t>(value);
    return number == value;
}

static std::string ErrorMessage(const httplib::Result& response)
{
    if (!response)
        return httplib::to_string(response.error());
    try {
        const json body = json::parse(response->body);
        return body.at("error").at("message").get<std::string>();
    } catch (...) {
        return "HTTP " + std::to_string(response->status);
    }
}

static void PageText(const std::string& text)
{
    size_t position = 0;
    size_t lines = 0;
    while (position < text.size()) {
        const size_t end = text.find('\n', position);
        std::cout << text.substr(position, end == std::string::npos ? end : end - position) << '\n';
        position = end == std::string::npos ? text.size() : end + 1;
        if (++lines % 20 == 0 && position < text.size()) {
            std::cout << "-- More -- (Enter to continue, q to stop) " << std::flush;
            std::string command;
            std::getline(std::cin, command);
            if (command == "q" || command == "Q")
                break;
        }
    }
}

static bool Search(httplib::Client& client,
                   const Options& options,
                   const std::string& query,
                   size_t offset,
                   std::vector<Result>& results,
                   bool& hasMore)
{
    const json request{
        {"mode", options.Mode},
        {"query", query},
        {"offset", offset},
        {"limit", options.Limit},
    };
    const auto response = client.Post("/v1/search", request.dump(), "application/json");
    if (!response || response->status != 200) {
        std::cout << "search failed: " << ErrorMessage(response) << '\n';
        return false;
    }

    const json body = json::parse(response->body);
    const size_t first = offset == 0 ? 0 : results.size();
    if (offset == 0)
        results.clear();
    for (const json& item : body.at("results")) {
        results.push_back({
            item.at("rank").get<size_t>(),
            item.at("document_id").get<std::string>(),
            item.at("path").is_null() ? "" : item.at("path").get<std::string>(),
            item.at("score").is_null() ? 0.0 : item.at("score").get<double>(),
        });
    }
    hasMore = body.at("has_more").get<bool>();
    std::cout << body.at("returned") << " result(s), " << std::fixed << std::setprecision(2)
              << body.at("took_ms").get<double>() << " ms\n";
    for (size_t i = first; i < results.size(); ++i) {
        std::cout << results[i].Rank << " " << std::fixed << std::setprecision(2)
                  << results[i].Score << " "
                  << (results[i].Path.empty() ? "[unknown]" : results[i].Path) << '\n';
    }
    if (hasMore)
        std::cout << "Type /n for more results.\n";
    return true;
}

static void ShowDocument(httplib::Client& client, const Result& result)
{
    const auto response = client.Get("/v1/documents/" + result.DocumentId);
    if (!response || response->status != 200) {
        std::cout << "document failed: " << ErrorMessage(response) << '\n';
        return;
    }
    const json body = json::parse(response->body);
    if (body.at("kind") == "url") {
        std::cout << body.at("url").get<std::string>() << '\n';
        return;
    }
    PageText(body.at("content").get<std::string>());
    if (body.value("truncated", false))
        std::cout << "[preview truncated]\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
#ifdef _WIN32
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
#endif
        const Options options = ParseArguments(argc, argv);
        httplib::Client client(options.Host, options.Port);
        client.set_connection_timeout(2);
        client.set_read_timeout(10);

        const auto ready = client.Get("/v1/health/ready");
        if (!ready || ready->status != 200)
            throw std::runtime_error("Shennong is unavailable: " + ErrorMessage(ready));
        const json status = json::parse(ready->body);
        std::cout << "Shennong search — " << status.at("documents") << " document(s) at http://"
                  << options.Host << ':' << options.Port << "\nMode: " << options.Mode
                  << "\nType a query, @N to show a result, /n for more, /h for help, or /q to quit.\n";

        std::string query;
        size_t offset = 0;
        bool hasMore = false;
        std::vector<Result> results;
        for (std::string line; std::cout << "> " && std::getline(std::cin, line);) {
            if (line.empty())
                continue;
            if (line == "/q")
                break;
            if (line == "/h") {
                std::cout << "query  Search\n@N     Show result N\n/n     Next page\n/q     Quit\n";
                continue;
            }
            if (line == "/n") {
                if (query.empty() || !hasMore) {
                    std::cout << "no more results\n";
                } else {
                    offset = results.size();
                    Search(client, options, query, offset, results, hasMore);
                }
                continue;
            }
            if (line[0] == '@') {
                size_t number = 0;
                if (!ParseResultReference(line, number) || number > results.size())
                    std::cout << "usage: @N for a result from the latest page\n";
                else
                    ShowDocument(client, results[number - 1]);
                continue;
            }
            query = std::move(line);
            offset = 0;
            results.clear();
            Search(client, options, query, offset, results, hasMore);
        }
    } catch (const std::exception& error) {
        std::cerr << "shennong_cli: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
