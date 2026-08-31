# ShenNong HTTP Search Service

`shennong` loads one MoonShot index at startup and exposes a small HTTP/JSON API. Search execution is delegated to the public MoonShot SDK through `IndexContext::Enqueue()`; the service owns only transport validation, optional query embedding, pagination, and JSON serialization.

## Build and Run

```powershell
cmake --build build --config Release
.\build\x64\Release\shennong.exe --port 9000 --index "$env:USERPROFILE\moon.idx"
```

With a Visual Studio multi-configuration build, omitting `--config Release` builds Debug by default. Omitting `--target` builds the complete default target set, including examples, tests, Rust tools, Shennong clients, WASM assets, and web assets.

Supported options:

| Option | Default | Meaning |
| :--- | :--- | :--- |
| `--listen` | `127.0.0.1` | HTTP listen address. Use a non-loopback address only on a trusted network. |
| `--port` | `9000` | HTTP listen port. |
| `--index` | `~/moon.idx` | MoonShot index loaded at startup. |
| `--ui` | `web` beside the executable | Static search-interface directory. |
| `--bge-host` | `127.0.0.1` | Query embedding service numeric IPv4 address. `--gbe-host` remains an accepted spelling. |
| `--bge-port` | `8765` | Query embedding service port. `--gbe-port` remains an accepted spelling. |

## Health

```powershell
curl.exe -sS http://localhost:9000/v1/health/live
curl.exe -sS http://localhost:9000/v1/health/ready
```

`live` reports that the HTTP process is running. `ready` reports the loaded index metadata and configured embedding endpoint.

## Browser Search

Open `http://localhost:9000/`; Shennong redirects to `http://localhost:9000/ui/`. The integrated interface supports text, vector, and hybrid queries, 20-result previous/next pages through all matching results, a horizontally resizable split document preview, sanitized Markdown rendering, and best-effort URL framing. Markdown parsing uses pinned Marked, DOMPurify, KaTeX, and Mermaid browser scripts. KaTeX renders `$...$`, `$$...$$`, `\\(...\\)`, and `\\[...\\]`; fenced `mermaid` blocks render as diagrams. If a rendering script is unavailable, its content safely falls back to text or a code block.

## Command-Line Search

```powershell
cmake --build build --target shennong_cli --config Debug
.\build\x64\Debug\shennong_cli.exe --host 127.0.0.1 --port 9000 --mode text
```

Enter a query to search, `@N` to display result N from the latest page, `/n` for the next page, `/h` for help, or `/q` to quit. The client contains no MoonShot search logic; all queries and document reads go through Shennong.

For formatted Markdown in the terminal, install the established [Glow](https://github.com/charmbracelet/glow) renderer with `winget install charmbracelet.glow`. `@N` renders Markdown through Glow and pages it with the native terminal pager (`more.com` on Windows); if Glow or a pager is unavailable, the client falls back to its existing paged plain-text display.

## Search

All searches use:

```text
POST /v1/search
Content-Type: application/json
```

### Text

```powershell
curl.exe --fail-with-body -sS -X POST http://localhost:9000/v1/search -H "Content-Type: application/json" --data-raw '{"mode":"text","query":"distributed search engine","fields":["title","body"],"offset":0,"limit":20}'
```

Text search does not call the embedding service.

### Vector from query text

```powershell
curl.exe --fail-with-body -sS -X POST http://localhost:9000/v1/search -H "Content-Type: application/json" --data-raw '{"mode":"vector","query":"distributed search engine","offset":0,"limit":20,"ef_search":200}'
```

Shennong embeds the query once and passes the resulting vector to the SDK's vector search path.

### Explicit vector

```json
{
  "mode": "vector",
  "vector": [0.012, -0.031, 0.084],
  "offset": 0,
  "limit": 20,
  "ef_search": 200
}
```

The vector must contain exactly the dimension reported by `/v1/health/ready`—128 for the current index format—and must have finite, non-zero magnitude. For a full vector, save the JSON body to a file and call:

```powershell
curl.exe --fail-with-body -sS -X POST http://localhost:9000/v1/search -H "Content-Type: application/json" --data-binary "@vector-request.json"
```

### Hybrid

```powershell
curl.exe --fail-with-body -sS -X POST http://localhost:9000/v1/search -H "Content-Type: application/json" --data-raw '{"mode":"hybrid","query":"distributed search engine","fields":["anchor","url","title","body"],"offset":0,"limit":20}'
```

Current hybrid semantics are lexical retrieval followed by vector-aware scoring. Hybrid search does not union independent lexical and ANN result sets, and `ef_search` is therefore rejected in hybrid mode.

## Request Contract

| Field | Contract |
| :--- | :--- |
| `mode` | Required: `text`, `vector`, or `hybrid`. |
| `query` | Required for text and hybrid. In vector mode, use exactly one of `query` or `vector`. |
| `vector` | Optional numeric array. In hybrid mode it replaces query embedding. |
| `fields` | Lexical fields: `anchor`, `url`, `title`, `body`, `meta`. Default: anchor, URL, title, and body. |
| `offset` | Default `0`; must be smaller than the loaded index's document count. |
| `limit` | Default `20`; maximum `100`. |
| `ef_search` | Vector-only ANN budget; must cover the requested result window. |

Field names map to MoonShot stream letters only at the service boundary: anchor=`A`, URL=`U`, title=`T`, body=`B`, and meta=`M`.

## Response

```json
{
  "request_id": "1788119851645573-0",
  "mode": "text",
  "took_ms": 1.29,
  "offset": 0,
  "limit": 20,
  "returned": 20,
  "has_more": true,
  "next_offset": 20,
  "results": [
    {
      "rank": 1,
      "document_id": "7319",
      "score": 26.04,
      "path": "Q:\\documents\\search.md"
    }
  ]
}
```

`document_id` is the normalized stored ID and is encoded as a string so clients do not depend on JSON number precision or the index's current ID width. `score` is opaque and is comparable only within the same request and search mode. Exact total-hit counts are intentionally not computed; Shennong requests one extra SDK result to determine `has_more`.

## Errors

```json
{
  "request_id": "1788119851881389-1",
  "error": {
    "code": "invalid_vector_dimension",
    "message": "expected 128 dimensions but received 2"
  }
}
```

| Status | Meaning |
| :---: | :--- |
| `400` | Malformed JSON or unknown property. |
| `404` | Unknown endpoint. |
| `405` | Unsupported method. |
| `422` | Invalid mode, fields, pagination, vector, or mode-specific options. |
| `503` | Embedding or vector search is unavailable. |
| `500` | Unexpected internal search failure. |

## Documents

`GET /v1/documents/{document_id}` returns the indexed file contents or an HTTP/HTTPS URL descriptor. `?raw=1` returns file content directly. Shennong accepts only IDs resolved through the loaded index, reads regular text files only, and limits previews to 1 MiB.

## SDK Search Mapping

Shennong makes exactly one `IndexContext::Enqueue()` call per accepted request:

| Mode | SDK query | SDK vector | SDK streams |
| :--- | :--- | :--- | :--- |
| Text | Request query | Empty | Selected lexical fields |
| Vector | Empty | Explicit or embedded vector | Not used |
| Hybrid | Request query | Explicit or embedded vector | Selected lexical fields |

The SDK receives a bounded top-K of `offset + limit + 1`. Index loading, query compilation, reader construction, base/delta handling, scoring, ANN retrieval, and search workers remain owned by `IndexContext` and the MoonShot SDK.
