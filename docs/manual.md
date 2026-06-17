# MoonShot Search Engine SDK — v0.1.0

MoonShot is a C++ full-text search engine delivered as a linkable static
library.  It supports inverted-index text search with BM25 relevance
scoring, per-field (MetaStream) indexing, multi-phase query execution,
and binary index persistence.

---

## Table of Contents

1. [Repository layout](#1-repository-layout)
2. [Prerequisites](#2-prerequisites)
3. [Build](#3-build)
4. [Linking against MoonShot](#4-linking-against-moonshot)
5. [Quick start](#5-quick-start)
6. [Index persistence](#6-index-persistence)
7. [Indexing API](#7-indexing-api)
8. [Query syntax](#8-query-syntax)
9. [Search API](#9-search-api)
10. [BM25 scoring](#10-bm25-scoring)
11. [Multi-phase search](#11-multi-phase-search)
12. [Low-level ISR access](#12-low-level-isr-access)
13. [Public header reference](#13-public-header-reference)
14. [Index file format](#14-index-file-format)

---

## 1. Repository layout

```
MoonShot/
├── include/
│   └── moonshot.h              ← single public umbrella header
├── IndexAccess/                ← inverted index, ISR, cache, serialiser
│   ├── PostingStore.h          ← in-memory posting store
│   ├── IndexSerializer.h/cpp   ← binary save / load
│   ├── IndexContext.h          ← engine factory (owns PostingStore)
│   ├── AdvancedIndexWriter.h   ← writes tokens into PostingStore
│   ├── IsrImpl.h               ← TermIsr / AndIsr / OrIsr / NotIsr
│   ├── BlockTable.h            ← block cache (clock eviction)
│   ├── ElementFilter.h/cpp     ← bloom filter (MurmurHash3)
│   ├── HashFunctions.h/cpp     ← MurmurHash3 implementation
│   ├── MemOperation.h/cpp      ← page-locked memory allocation
│   └── FileBlockManager.h      ← block-level file I/O
├── Compiler/
│   ├── EvalExpression.h        ← EvalTree AST nodes
│   └── IndexSearchCompiler.h   ← text query → EvalTree
├── Executor/
│   └── IndexSearchExecutor.h   ← DAAT traversal + top-K ranking
├── Tokenizer/
│   └── Tokenizer.h             ← Tokenizer, SmartTokenizer, SimpleTokenizer
├── Utils/
│   ├── FileAccess.h/cpp        ← cross-platform block file I/O
│   └── Constants.h
├── examples/
│   ├── quickstart.cpp          ← 60-line minimal example
│   └── fulltext_search.cpp     ← comprehensive feature demo
├── Test/UnitTest/
│   └── IndexAccessUnitTest.cpp ← 10 test cases
├── docs/
│   ├── manual.md               ← this file
│   ├── query-syntax.md         ← query language specification
│   └── index-design.md         ← on-disk format specification
└── CMakeLists.txt
```

---

## 2. Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| CMake | ≥ 3.10 | |
| C++ compiler | C++17 | MSVC 2019+, GCC 9+, Clang 10+ |
| ICU (uc + i18n) | any | **Optional.** Enables Unicode-aware word breaking. Without it the built-in `SimpleTokenizer` is used automatically. |

---

## 3. Build

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build libraries + examples + tests
cmake --build build --config Release

# Install to a prefix (e.g. /usr/local or C:/MoonShot)
cmake --install build --prefix /usr/local
```

After install, the prefix contains:

```
<prefix>/
├── include/moonshot/
│   ├── moonshot.h
│   ├── IndexContext.h
│   ├── IndexSerializer.h
│   ├── PostingStore.h
│   ├── EvalExpression.h
│   ├── IndexSearchCompiler.h
│   ├── IndexSearchExecutor.h
│   ├── Tokenizer.h
│   └── ...  (all public headers)
├── lib/
│   ├── libUtils.a  (or Utils.lib on Windows)
│   ├── libTokenizer.a
│   ├── libExecutor.a
│   └── libIndexAccess.a
└── lib/cmake/MoonShot/
    ├── MoonShotConfig.cmake
    ├── MoonShotConfigVersion.cmake
    └── MoonShotTargets.cmake
```

---

## 4. Linking against MoonShot

### Option A — `add_subdirectory` (source tree)

```cmake
# CMakeLists.txt of your project
cmake_minimum_required(VERSION 3.10)
project(MyApp)

set(CMAKE_CXX_STANDARD 17)
add_subdirectory(MoonShot)              # path to the MoonShot source

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE MoonShot::MoonShot)
```

One `target_link_libraries` line gives your target all four libraries,
all public header directories, and the correct C++17 settings.

### Option B — installed package (`find_package`)

```cmake
find_package(MoonShot 0.1 REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE MoonShot::MoonShot)
```

Point CMake at the install prefix:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/usr/local
```

### Single include

Once linked, include the umbrella header:

```cpp
#include "moonshot.h"   // pulls in every public header
```

---

## 5. Quick start

```cpp
#include "moonshot.h"

int main()
{
    // 1. Create engine — pass an index file path for persistence
    IndexContext   engine("", "my_index.bin");
    SmartTokenizer tok;

    // 2. Index documents
    auto  writer = engine.GetWriter();

    writer->Write(tok.Tokenize("Rust systems programming"),   1, "Title");
    writer->Write(tok.Tokenize("Ownership prevents data races"), 1, "Body");
    writer->SetDocImportance(1, 0.9f);

    writer->Write(tok.Tokenize("Python data science"),        2, "Title");
    writer->Write(tok.Tokenize("Used for machine learning"),  2, "Body");
    writer->SetDocImportance(2, 0.7f);

    // 3. Search
    IndexSearchCompiler compiler;
    std::unique_ptr<EvalTree>            tree(compiler.Compile("rust", "AUTB"));
    std::unique_ptr<IndexSearchExecutor> exec(engine.GetExecutor());

    for (auto& r : exec->Execute(engine.GetReader(tree.get()), 10))
        printf("doc=%llu  score=%.3f\n", r.doc_id, r.score);

    // 4. Save to disk
    engine.SaveIndex();   // writes my_index.bin

    // 5. Reload on next startup
    IndexContext engine2("", "my_index.bin");  // auto-loads

    return 0;
}
```

---

## 6. Index Persistence

### Save

```cpp
// Using the path given at construction
engine.SaveIndex();

// Or save to a new path
engine.SaveIndex("archive.bin");
```

### Load

If an index file path is given at construction and the file exists, it
is loaded automatically:

```cpp
IndexContext engine("", "my_index.bin");  // loads on construction
```

You can also reload at any time:

```cpp
engine.LoadIndex("other.bin");   // replaces current in-memory state
```

### Index file

The binary format is described in full in `docs/index-design.md`.
It uses:

- A 64-byte file header with magic bytes `"MOONSHOT"`, version, and
  section offsets.
- A DocData section: 16 bytes per document (id, importance, length).
- A TermDict section: sorted term keys with per-term posting offsets.
- A Postings section: VarByte-delta-encoded `(docID, tf)` pairs packed
  end-to-end.

The format is forward-compatible; future versions add new sections
without breaking readers of older files.

---

## 7. Indexing API

### Creating a writer

```cpp
auto writer = engine.GetWriter();
// Returns shared_ptr<IndexWriter>; the concrete type is AdvancedIndexWriter.
// All methods (Write, SetDocImportance) are available directly on the base pointer.
```

### Writing a document field

```cpp
SmartTokenizer tok;

writer->Write(tok.Tokenize(text), doc_id, stream_name);
```

| Parameter | Type | Description |
|---|---|---|
| `words` | `vector<string>&&` | Token list from `tok.Tokenize(text)` |
| `doc_id` | `uint64_t` | Unique document identifier |
| `stream_name` | `const char*` | `"Title"`, `"Body"`, `"Anchor"`, `"URL"`, `"Meta"` (or abbreviations `T`, `B`, `A`, `U`, `M`) |

### MetaStreams

Each stream is indexed separately.  A term "fox" written to "Title"
creates posting key `"foxT"`; written to "Body" creates `"foxB"`.

| Stream | Abbrev | Typical use |
|---|---|---|
| Title | T | Page or document title |
| Body | B | Main text content |
| Anchor | A | In-bound link anchor text |
| URL | U | URL path tokens |
| Meta | M | HTML meta description / keywords |

### Document importance

An optional pre-computed quality score (PageRank, model score, etc.)
added to BM25 at rank time:

```cpp
writer->SetDocImportance(doc_id, 0.95f);
```

---

## 8. Query syntax

### Boolean operators

```
rust                         single term (searched in all streams of the set)
rust safety                  AND  — both terms must appear
rust AND safety              AND  — explicit keyword
rust OR go                   OR   — at least one term must appear
rust NOT unsafe              NOT  — exclude documents containing "unsafe"
-unsafe rust                 NOT  — minus prefix shorthand
```

### Field constraints

```
title:rust                   "rust" in Title stream only
body:rust                    "rust" in Body stream only
anchor:rust                  "rust" in Anchor stream only
```

### Combining

```
(rust OR go) safety          OR group AND'd with a term
title:rust body:safety       AND of two field-pinned terms
rust NOT unsafe title:system NOT with a field constraint
```

### Operator precedence (high → low)

```
field:  >  NOT  >  AND (implicit space or AND keyword)  >  OR
```

---

## 9. Search API

### 9.1 Compile

```cpp
IndexSearchCompiler compiler;   // stateless; create once and reuse

// second arg = stream set (which MetaStreams to search)
EvalTree* tree = compiler.Compile("rust safety", "AUTB");
delete tree;    // caller owns the returned pointer
```

### Stream sets

| String | Streams | Use |
|---|---|---|
| `"AUT"` | Anchor + URL + Title | Phase 1 — fast, high precision |
| `"AUTB"` | + Body | Phase 2 — more recall |
| `"T"` | Title only | |
| `"B"` | Body only | |
| `"TB"` | Title + Body | |

Use `std::unique_ptr<EvalTree>` to avoid manual delete:

```cpp
std::unique_ptr<EvalTree> tree(compiler.Compile("rust", "AUTB"));
```

### 9.2 Get a reader (ISR)

```cpp
// From an EvalTree
auto reader = engine.GetReader(tree.get());    // shared_ptr<IndexReader>

// From a single term (opens AUT streams)
auto reader = engine.GetReader("fox");
```

### 9.3 Execute

```cpp
std::unique_ptr<IndexSearchExecutor> exec(engine.GetExecutor());

std::vector<SearchResult> results = exec->Execute(reader, /*top_k=*/ 10);
```

### 9.4 `SearchResult`

```cpp
struct SearchResult {
    uint64_t    doc_id;    // matches the id passed to Write()
    float       score;     // BM25 + importance — higher is better
    std::string snippet;   // optional display text (set by the caller)
};
```

### 9.5 Complete pattern

```cpp
auto search = [&](const char* query, const char* streams = "AUTB") {
    std::unique_ptr<EvalTree>            tree(compiler.Compile(query, streams));
    std::unique_ptr<IndexSearchExecutor> exec(engine.GetExecutor());
    return exec->Execute(engine.GetReader(tree.get()), 10);
};

for (auto& r : search("rust safety"))
    printf("doc=%llu  score=%.3f\n", r.doc_id, r.score);
```

---

## 10. BM25 scoring

**Formula (Okapi BM25, k1 = 1.2, b = 0.75):**

```
IDF(t)       = ln( (N - df + 0.5) / (df + 0.5)  + 1 )
TF_norm(t,d) = tf * (k1+1) / (tf + k1*(1 - b + b*dl/avgdl))
Score(d)     = Σ_t  IDF(t) * TF_norm(t,d)  +  DocImportance(d)
```

**Inspect corpus statistics:**

```cpp
auto* store = engine.GetStore();
printf("documents  : %llu\n",  store->TotalDocs());
printf("avg length : %.1f\n",  store->AvgDocLen());
printf("df(rustT)  : %u\n",    store->DocFreq("rustT"));
printf("len(doc 1) : %u\n",    store->GetDocLen(1));
```

---

## 11. Multi-phase search

Phase 1 searches the high-signal AUT streams (fast, precise).  If too
few results are found, Phase 2 adds the Body stream (slower, more
recall).

**Manual:**

```cpp
auto tree1   = std::unique_ptr<EvalTree>(compiler.Compile("goroutine", "AUT"));
auto results = exec->Execute(engine.GetReader(tree1.get()), 10);

if (results.size() < 3) {
    auto tree2 = std::unique_ptr<EvalTree>(compiler.Compile("goroutine", "AUTB"));
    results    = exec->Execute(engine.GetReader(tree2.get()), 10);
}
```

**Automatic via `ExecutePhased`:**

```cpp
auto t1 = std::unique_ptr<EvalTree>(compiler.Compile("goroutine", "AUT"));
auto t2 = std::unique_ptr<EvalTree>(compiler.Compile("goroutine", "AUTB"));

//  ExecutePhased(phase1, phase2, top_k, min_results_before_fallback)
auto results = exec->ExecutePhased(
    engine.GetReader(t1.get()),
    engine.GetReader(t2.get()),
    10, 3
);
```

If Phase 1 yields ≥ 3 results, Phase 2 is never executed.  Otherwise
both result sets are merged (de-duplicated, higher score wins).

---

## 12. Low-level ISR access

For custom scoring or streaming use cases, you can walk posting lists
directly:

```cpp
auto reader = engine.GetReader("rust");   // OrIsr across AUT streams

while (!reader->IsEnd()) {
    uint64_t doc  = reader->GetDocumentID();
    uint32_t tf   = reader->GetTermFreq();
    const DocDataEntry* entry = engine.GetDocDataEntry(doc);
    float    score = reader->GetScore(entry);
    printf("doc=%-4llu  tf=%u  score=%.3f\n", doc, tf, score);
    reader->GoNext();
}
```

**Manual EvalTree:**

```cpp
// AND( TermNode("rustT"), TermNode("safetyB") )
auto and_node = std::make_shared<AndNode>();
and_node->children.push_back(std::make_shared<TermNode>("rustT"));
and_node->children.push_back(std::make_shared<TermNode>("safetyB"));

EvalTree tree;
tree.root = and_node;

auto results = exec->Execute(engine.GetReader(&tree), 10);
```

---

## 13. Public header reference

| Header | Contents |
|---|---|
| `moonshot.h` | Umbrella — includes everything below |
| `IndexContext.h` | `IndexContext` — engine factory + persistence |
| `IndexSerializer.h` | `IndexSerializer::Save` / `Load` / `IsValidIndex` |
| `EvalExpression.h` | `EvalTree`, `TermNode`, `AndNode`, `OrNode`, `NotNode` |
| `IndexSearchCompiler.h` | `IndexSearchCompiler` — text → `EvalTree` |
| `IndexSearchExecutor.h` | `IndexSearchExecutor` — scoring + top-K |
| `AdvancedIndexWriter.h` | `AdvancedIndexWriter` — writes tokens to `PostingStore` |
| `IsrImpl.h` | `TermIsr`, `AndIsr`, `OrIsr`, `NotIsr` |
| `PostingStore.h` | `PostingStore`, `PostingList`, `PostingEntry`, `DocStats` |
| `IndexReader.h` | `IndexReader` — ISR base interface |
| `SearchResult.h` | `SearchResult` |
| `Tokenizer.h` | `Tokenizer`, `SmartTokenizer`, `SimpleTokenizer` |
| `BlockTable.h` | `BlockCache`, `TermToBlock`, `RWSpinLock`, `IndexBlock` |
| `ElementFilter.h` | `ElementFilter` — bloom filter |
| `HashFunctions.h` | `MurmurHash3_*`, `Hash1`, `Hash2`, `HashMurmur3` |
| `MemOperation.h` | `PinedMemAlloc`, `PinedMemFree` — page-locked memory |
| `FileAccess.h` | `FileAccess` — cross-platform block file I/O |
| `FileBlockManager.h` | `FileBlockManager` — block-sequence abstraction over `FileAccess` |

---

## 14. Index file format

The binary index file written by `IndexSerializer::Save` has four
contiguous sections.

```text
Offset    Section          Contents
────────────────────────────────────────────────────────────────
0         File Header      64 bytes — magic, version, num_docs,
                           num_terms, section offsets
N₁        DocData          num_docs × 16 bytes
                           { doc_id:u64, importance:f32, doc_len:u32 }
N₂        TermDict         num_terms × variable
                           { key_len:u16, key[key_len], doc_freq:u32,
                             data_offset:u64, data_len:u32 }
N₃        Postings         packed VarByte streams
                           { delta_docid:varint, tf:varint } × N
```

**Magic bytes:** `4D 4F 4F 4E 53 48 4F 54` ("MOONSHOT")

**Posting encoding:** Each posting list is stored as delta-compressed
`(doc_id_delta, tf)` pairs using VarByte encoding.  Doc IDs are sorted
ascending; delta = current_id − previous_id.  This compresses typical
posting lists to 2–4 bytes per entry.

**Validate before loading:**

```cpp
if (!IndexSerializer::IsValidIndex("my_index.bin"))
    fprintf(stderr, "not a valid MoonShot index\n");
```
