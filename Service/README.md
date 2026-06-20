# ShenNong Match Service Design

## Objective

`shennong` is a long-running MoonShot HTTP/JSON search service. It loads one MoonShot index at startup, listens on a TCP port, accepts browser-friendly HTTP requests, searches the loaded index, and returns ranked document results with paths.

Default command:

```bash
shennong
```

Equivalent explicit command:

```bash
shennong --port 9000 --index ~/moon.idx
```

## Command Line Contract

Supported parameters:

| Parameter | Default | Meaning |
| :--- | ---: | :--- |
| `--port <port>` | `9000` | TCP port for the HTTP server. The service listens on `0.0.0.0:<port>`. |
| `--index <path>` | `~/moon.idx` | MoonShot index file loaded once during service startup. |

Current executable target:

```bash
cmake --build build --target shennong --config Debug
```

Windows debug binary:

```powershell
.\build\x64\Debug\shennong.exe --port 9000 --index "$env:USERPROFILE\moon.idx"
```

Startup should fail fast if:

- `--port` is not a valid TCP port.
- `--index` does not exist.
- The index file cannot be loaded or has an unsupported format version.
- The HTTP server cannot bind the requested port.

## HTTP API

### `GET /health`

Returns service health and loaded index metadata.

Example:

```bash
curl http://localhost:9000/health
```

Response:

```json
{
  "status": "ok",
  "index": "/home/bob/moon.idx",
  "documents": 19347,
  "avg_doc_len": 3185.7
}
```

### `GET /search`

Searches the loaded index and returns one page of ranked results.

Parameters:

| Parameter | Default | Meaning |
| :--- | ---: | :--- |
| `q` | required | Query text. |
| `offset` | `0` | Zero-based result offset. |
| `limit` | `20` | Result page size, clamped to `1..1000`. |
| `streams` | `AUTB` | Stream set passed to the MoonShot query compiler. |

Example:

```bash
curl "http://localhost:9000/search?q=usage&offset=0&limit=20&streams=AUTB"
```

In Windows `cmd.exe`, quote the URL or escape each `&` as `^&`. Otherwise `cmd.exe` treats `&` as a command separator and the service only receives `q=usage`.

```bat
curl "http://localhost:9000/search?q=usage&offset=0&limit=200"
curl http://localhost:9000/search?q=usage^&offset=0^&limit=200
```

Response:

```json
{
  "query": "usage",
  "streams": "AUTB",
  "total": 874,
  "offset": 0,
  "limit": 20,
  "elapsed_ms": 6.8,
  "results": [
    {
      "rank": 1,
      "doc_id": 16615,
      "score": 17.6526,
      "path": "Q:\\src\\...\\BulkDB-resource-usage.md"
    }
  ]
}
```

The service computes the full ranked result set to return `total`, then emits only the requested page. It does not hardcode top-20 truncation.

### `GET /vector-search`

Searches document embeddings stored inside `DocData`. Each `DocDataEntry` carries an int8 vector payload (`DDE_VectorData`) plus vector dimensions/format metadata, loaded with the main index. No vector sidecar is required for vector payloads.

Text query form:

```bash
curl "http://localhost:9000/vector-search?q=usage&offset=0&limit=20"
```

Explicit vector form:

```bash
curl "http://localhost:9000/vector-search?vector=0.1,0.2,0.3&offset=0&limit=20"
```

Response:

```json
{
  "query": "usage",
  "vector_dim": 128,
  "vector_count": 19347,
  "total": 19347,
  "offset": 0,
  "limit": 20,
  "elapsed_ms": 3.2,
  "results": [
    { "rank": 1, "doc_id": 42, "score": 0.83, "path": "/home/bob/a.md" }
  ]
}
```

## Legacy gRPC Surface

The repository still contains a gRPC proto/stub. It is not the primary browser-facing service path right now.

The current proto defines the `Wenda.ShenNong` gRPC service in `MatchService.proto`:

```proto
service ShenNong {
    rpc Query121(Question) returns (Answers) {}
    rpc Query129(Question) returns(stream Answers) {}
    rpc Query921(stream Question) returns (Answers) {}
    rpc Query929(stream Question) returns(stream Answers) {}
}
```

If gRPC is revived later, recommended first supported API:

| RPC | Use |
| :--- | :--- |
| `Query129(Question) returns (stream Answers)` | Primary search API. One query returns a stream of ranked results. |
| `Query121(Question) returns (Answers)` | Optional compatibility API. Can return only the top result. |
| `Query921` / `Query929` | Batch or interactive streaming queries. Implement after unary/server-streaming search works. |

`Question.Question` is the search query string. `Question.Option` can later carry options such as stream set, page size, or top-k.

`Answers` should map one search result:

| Field | Source |
| :--- | :--- |
| `DocID` | `SearchResult.doc_id` |
| `AnswersID` | 1-based rank in the result stream |
| `AnswerScore` | `SearchResult.score` |
| `Answers` | document path from `PostingStore::GetDocPath(DocID)` |

## Runtime Architecture

```text
shennong process
  main.cpp
    parse --port / --index
    create SearchService
    bind HTTP socket on 0.0.0.0:<port>
    wait forever until shutdown

  SearchService
    loads IndexContext from index path once
    receives /search request
    compiles query with IndexSearchCompiler
    creates IndexReader from IndexContext
    executes search with IndexSearchExecutor
    returns JSON page with total/results/path
```

The index must be loaded once at process startup, not per query. Queries should share the loaded index data structures.

## Query Flow

For `GET /search`:

```text
client sends HTTP request
  q       = query text, e.g. "usage"
  offset  = result offset
  limit   = page size
  streams = optional stream set

server:
  validate query is not empty
  compile query against stream set "AUTB"
  get reader from loaded IndexContext
  execute search with no hard result cap
  compute total result count
  for each SearchResult in requested page:
    lookup path in DocData via PostingStore
    write JSON result
```

If a query matches 768 documents, the response includes `"total":768` and returns only `offset..offset+limit` result objects.

## Paging Semantics

The service should not silently truncate results to 20.

Recommended behavior:

- `/search` computes all matches and returns the total.
- `/search` returns only the requested page using `offset` and `limit`.
- Browser clients request the next page by increasing `offset`.
- `/vector-search` follows the same `offset` / `limit` paging contract.

## Threading Model

The current HTTP implementation is synchronous and protects query execution with a mutex while `IndexContext` thread-safety is not proven.

The old `MatchService` skeleton has three thread pools:

```text
QueueThreads
QueryThreads
DataThreads
```

Suggested ownership:

| Pool | Responsibility |
| :--- | :--- |
| `QueueThreads` | Lightweight request admission, validation, cancellation bookkeeping. |
| `QueryThreads` | Query compile + reader execution + ranking. |
| `DataThreads` | Optional file or future remote data fetch work. Not needed for basic index search. |

The HTTP implementation can later move to per-connection worker threads once reader-per-query concurrency is verified.

## State Ownership

Current implementation classes:

```cpp
struct Options {
    uint16_t port = 9000;
    std::string index_path = "~/moon.idx";
};

class SearchService {
public:
    explicit SearchService(std::string index_path);
    std::string health_json();
    std::string search_json(const QueryParams& params);

private:
    IndexContext index_;
    std::mutex query_mutex_;
};
```

`IndexContext` must remain alive for the full lifetime of the HTTP service.

If `IndexContext` is not thread-safe for concurrent readers, protect query execution with a mutex first. Once verified, move to reader-per-query concurrency.

## Error Handling

HTTP status mapping:

| Condition | HTTP status |
| :--- | :--- |
| Empty query | `400 Bad Request` |
| Query parse failure | `400 Bad Request` |
| Index not loaded | Process startup failure |
| Internal exception | `500 Internal Server Error` |
| Unknown endpoint | `404 Not Found` |

Startup errors should be printed to stderr and return a non-zero process exit code.

## Implementation Plan

1. Add graceful shutdown on Ctrl+C / SIGTERM.
2. Add worker threads after `IndexContext` query concurrency is verified.
3. Add `/file?path=...` if the browser client should open result files through this service.
4. Optionally add `/search-stream` using NDJSON for progressive result streaming.
5. Replace exact flat vector scan with HNSW/IVF once vector volume requires ANN latency.

## Example Usage

### Start the Service

Default:

```bash
shennong
```

Explicit:

```bash
shennong --port 9000 --index ~/moon.idx
```

Expected startup log:

```text
ShenNong service starting
Index: /home/bob/moon.idx
Listen: 0.0.0.0:9000
Index loaded: docs=19347 terms=24126901 blocks=29949
Ready
```

The process stays alive. Stop it with `Ctrl+C` or the platform service manager.

### Query the Service

The first implementation should expose `Query129`, because it is server-streaming and can return any number of matches without building one giant response.

Conceptual client request:

```text
rpc:      Wenda.ShenNong.Query129
address:  localhost:9000
message:
  QuestionID: 1
  Question:   "usage"
  Option:     "AUTB"
```

Conceptual streamed responses:

```text
AnswersID=1 DocID=42  AnswerScore=12.73 Answers=/home/bob/a.md
AnswersID=2 DocID=108 AnswerScore=11.19 Answers=/home/bob/b.md
AnswersID=3 DocID=300 AnswerScore=10.04 Answers=/home/bob/c.md
...
```

If the query matches 768 documents, the server should stream 768 `Answers` records. The client can display them as pages of 20, but the service itself should not silently truncate them.

### Example C++ Client Shape

```cpp
auto channel = grpc::CreateChannel("localhost:9000", grpc::InsecureChannelCredentials());
auto stub = Wenda::ShenNong::NewStub(channel);

Wenda::Question q;
q.set_questionid(1);
q.set_question("usage");
q.set_option("AUTB");

grpc::ClientContext ctx;
auto stream = stub->Query129(&ctx, q);

Wenda::Answers answer;
while (stream->Read(&answer)) {
    std::cout << answer.answersid() << " "
              << answer.docid() << " "
              << answer.answerscore() << " "
              << answer.answers() << "\n";
}

grpc::Status status = stream->Finish();
```

### Example grpcurl Shape

If server reflection is enabled, manual testing can use `grpcurl`:

```bash
grpcurl -plaintext \
  -d '{"QuestionID":1,"Question":"usage","Option":"AUTB"}' \
  localhost:9000 \
  Wenda.ShenNong/Query129
```

If reflection is not enabled, pass the proto file:

```bash
grpcurl -plaintext \
  -import-path Service \
  -proto MatchService.proto \
  -d '{"QuestionID":1,"Question":"usage","Option":"AUTB"}' \
  localhost:9000 \
  Wenda.ShenNong/Query129
```

### Client-Side Paging

For terminal or UI clients, paging belongs on the client side:

```text
read stream result 1-20
show page 1
wait for user action
show page 2
...
```

This keeps server semantics simple: one query returns the complete ranked result stream.

## Protocol Choice: HTTP/JSON, gRPC, or Something Else?

### Recommendation

Use HTTP/JSON as the primary API because the browser client must call the service directly. Browsers can use `fetch()` against `/search` without gRPC-Web, proxies, or generated client stubs.

Keep gRPC as an optional future native/internal API if high-throughput streaming clients need it.

### Why gRPC Fits ShenNong

| Requirement | gRPC fit |
| :--- | :--- |
| Long-running local or internal service | Good. Native process listens on one port. |
| Query returns many results | Good. Server-streaming maps directly to result streaming. |
| C++ service implementation | Good. Existing gRPC C++ stack is already present. |
| Strong schema | Good. `Question` and `Answers` are typed. |
| Future bidirectional query sessions | Good. `Query929` already models bidirectional streaming. |

### Downsides of gRPC

| Issue | Impact |
| :--- | :--- |
| Browser clients cannot call it directly without gRPC-Web or a proxy | Important if the main client is a browser. |
| Heavier build/dependency chain than HTTP | Already present, but still operationally heavier. |
| Manual testing needs grpcurl or a client stub | Slightly less convenient than curl. |

### HTTP/JSON Alternative

An HTTP service would look like:

```http
GET /search?q=usage&offset=0&limit=20
```

Response:

```json
{
  "total": 768,
  "offset": 0,
  "limit": 20,
  "results": [
    { "doc_id": 42, "score": 12.73, "path": "/home/bob/a.md" }
  ]
}
```

HTTP/JSON is better if:

- The main consumer is the browser viewer.
- You want `curl`-simple manual testing.
- You want first-class server-side paging with `offset` / `limit`.
- You do not need streaming or typed client stubs.

### Practical Direction

Recommended path for this repo:

1. Use `shennong` HTTP/JSON for browser clients.
2. Keep all search execution inside MoonShot `IndexContext` / `IndexSearchExecutor`.
3. Add gRPC later only if native/internal streaming clients need it.

That keeps browser integration simple without giving up MoonShot's native search engine.

## Notes

- Do not reload the index per query.
- Do not hardcode top-20 truncation in the service.
- Use HTTP paging for browser clients; consider NDJSON streaming later for broad progressive result streams.
- Keep `DocData.path` lookup server-side so clients receive usable paths directly.
- Keep the browser/WASM viewer separate from this service. The viewer is an inspection/debug tool; `shennong` is the production-style query service.
- `moon -name` writes URL/path tokens to the `U` stream so `site:` and `-site:` can work.
- `moon -name` writes document embeddings into `DocDataEntry::DDE_VectorData`; there is no vector payload sidecar.
- Existing indexes built before this change do not contain URL stream tokens or DocDataEntry embeddings. Re-run `moon -name <path>` to rebuild before testing `site:` / `-site:` or `/vector-search`.
