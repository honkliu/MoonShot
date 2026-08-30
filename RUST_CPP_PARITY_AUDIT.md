# C++ → RustBlade parity audit

Date: 2026-08-29

## Scope and acceptance rule

The current C++ implementation is the source of truth. This audit compared every listed C++ source file against its RustBlade implementation, including types, fields, constants, defaults, binary offsets, control flow, scoring formulas, reader/cache state machines, serialization, merge behavior, vector search, and public API naming.

Validation in this pass was static only. Per the session constraint, no Cargo/CMake build or test was run, and generated WASM package files were not modified.

## File-by-file matrix

| C++ source | RustBlade source | Static status |
| --- | --- | --- |
| Configuration/ConfigParameters.h | RustBlade/src/Configuration/ConfigParameters.rs | Aligned empty configuration type |
| Compiler/EvalExpression.h | RustBlade/src/Compiler/EvalExpression.rs | Aligned node types, compile modes, weak-AND modes, parameters, defaults, and tree types |
| Compiler/IndexSearchCompiler.h | RustBlade/src/Compiler/IndexSearchCompiler.rs | Aligned parsing, fields, OR/NOT/minus handling, unigram/bigram, weak-AND, boost, vector compilation, and mode behavior |
| Embeddings/Embeddings.h | RustBlade/src/Embeddings/Embeddings.rs | Aligned embedding interfaces/formula and compact FreshDiskAnnVectorIndex node/link representation and algorithms |
| Executor/IndexSearchExecutor.h | RustBlade/src/Executor/IndexSearchExecutor.rs | Aligned public execution entries, signed topK behavior, bounded traversal, scoring parameters, source mask, and result handling |
| Executor/IndexSearchExecutor.cpp | RustBlade/src/Executor/IndexSearchExecutor.rs | Aligned document feature score, prior, cosine feature, heap replacement, and final ordering predicate |
| Tokenizer/Tokenizer.h | RustBlade/src/Tokenizer/Tokenizer.rs | Aligned tokenizer interface |
| Tokenizer/SmartTokenizer.cpp | RustBlade/src/Tokenizer/SmartTokenizer.rs | Aligned NFC/lowercase/word segmentation/stemming/indexability pipeline as closely as available Unicode runtimes permit |
| Utils/Constants.h | RustBlade/src/Utils/Constants.rs | Aligned constants |
| Utils/FileAccess.h | RustBlade/src/Utils/FileAccess.rs | Aligned API, cursor operations, positioned block reads, writes, and statistics surface |
| Utils/FileAccess.cpp | RustBlade/src/Utils/FileAccess.rs | Aligned active read/write behavior; platform implementation remains language/OS-specific |
| IndexAccess/AdvancedIndexReader.h | RustBlade/src/IndexAccess/AdvancedIndexReader.rs | Aligned fields, lifecycle, explicit block release, stream/span weights, and reader API |
| IndexAccess/AdvancedIndexReader.cpp | RustBlade/src/IndexAccess/AdvancedIndexReader.rs | Aligned lookup, continuation traversal, BM25 constants/formula, GoNext, and GoUntil |
| IndexAccess/AdvancedIndexWriter.h | RustBlade/src/IndexAccess/AdvancedIndexWriter.rs | Aligned writer type/API |
| IndexAccess/AdvancedIndexWriter.cpp | RustBlade/src/IndexAccess/AdvancedIndexWriter.rs | Aligned stream indexing, TF, bigrams, paths, vectors, and document statistics |
| IndexAccess/BlockTable.h | RustBlade/src/IndexAccess/BlockTable.rs | Aligned v20 physical definitions, term lookup, pools, requests, direct/worker paths, cache states, eviction, sequential windows, and statistics |
| IndexAccess/ElementFilter.h | RustBlade/src/IndexAccess/ElementFilter.rs | Aligned storage/API/defaults |
| IndexAccess/ElementFilter.cpp | RustBlade/src/IndexAccess/ElementFilter.rs | Aligned MSVC-x64 FNV-1a target behavior and two-position filter operations |
| IndexAccess/FileBlockManager.h | RustBlade/src/IndexAccess/FileBlockManager.rs | Aligned memory/file read-write abstraction |
| IndexAccess/HashFunctions.h | RustBlade/src/IndexAccess/HashFunctions.rs | Aligned declarations/API equivalents |
| IndexAccess/HashFunctions.cpp | RustBlade/src/IndexAccess/HashFunctions.rs | Aligned MurmurHash3 variants and wrapping arithmetic |
| IndexAccess/IndexContext.h | RustBlade/src/IndexAccess/IndexContext.rs | Aligned lifecycle, build/save/load, readers, query modes, base/delta, merge, paths, vector runtime, queues, 16 workers, and public configuration APIs |
| IndexAccess/IndexReader.h | RustBlade/src/IndexAccess/IndexReader.rs | Aligned document/source encoding and required reader contract |
| IndexAccess/IndexReaderImpl.h | RustBlade/src/IndexAccess/IndexReaderImpl.rs | Aligned And/Or/WeakAnd/Not/Boost/Vector DAAT state machines and score aggregation |
| IndexAccess/IndexSerializer.h | RustBlade/src/IndexAccess/IndexSerializer.rs | Aligned serializer API and v20 section model |
| IndexAccess/IndexSerializer.cpp | RustBlade/src/IndexAccess/IndexSerializer.rs | Aligned 136-byte header, 81,920-byte path sidecar, 256-byte DocData, term blocks, continuation packing, and empty-index output |
| IndexAccess/IndexWriter.h | RustBlade/src/IndexAccess/IndexWriter.rs | Aligned base writer contract |
| IndexAccess/MemOperation.h | RustBlade/src/IndexAccess/MemOperation.rs | Aligned pinned-memory API |
| IndexAccess/MemOperation.cpp | RustBlade/src/IndexAccess/MemOperation.rs | Aligned Windows/Unix allocation behavior through platform-specific Rust ownership |
| IndexAccess/PostingStore.h | RustBlade/src/IndexAccess/PostingStore.rs | Aligned posting/doc/vector/path/statistics data and TF8 encoding |
| IndexAccess/SearchResult.h | RustBlade/src/IndexAccess/SearchResult.rs | Aligned result fields/default meaning |
| IndexAccess/UnifiedDecoder.h | RustBlade/src/IndexAccess/UnifiedDecoder.rs | Aligned name, byte-level VarByte/TF8 advancement, state, and API |
| examples/moon.cpp | RustBlade/src/bin/moon.rs | CLI/index/search/delta/merge/vector, BGE service/sidecar, and BEIR build/patch/evaluation behavior aligned |
| Service/ShennongHttp.cpp | RustBlade/src/bin/shennong.rs | HTTP routes, parameters, defaults, status behavior, and text/vector/combined selection aligned; external GBE runtime remains optional |
| Tools/moon_wasm native adapter behavior | RustBlade/src/Tools/moon_wasm/WasmApi.rs | Shared IndexContext behavior and browser file-backed adapter aligned; generated package intentionally untouched |

## v20 binary contract verified statically

- Header: 136 bytes
- Page: 4096 bytes
- Path-prefix sidecar: 20 pages / 81,920 bytes
- HeadTermEntry: 32 bytes
- LeafTermEntry fixed prefix: 16 bytes
- Leaf directory entries: 161
- DocDataEntry: 256 bytes
- Vector offset/dimension: 64 / 128
- Path offset: 192
- Continuation header: 12 bytes
- MPHF header/entry: 48 / 32 bytes
- Empty saved context: zero header plus zero sidecar, matching current C++ behavior

## Deliberately retained safety adaptations

These do not change legal-input search semantics and are not translated into C++ undefined behavior:

- Rust bounds checks for malformed VarByte and continuation payloads.
- Strict v20 section validation for damaged files.
- Rust ownership via Arc, Vec, Mutex, RwLock, and explicit mutation barriers.
- Locked sequential cache state instead of reproducing a potential C++ data race.

## Remaining limits to an absolute guarantee

The following cannot be guaranteed solely by source translation:

1. C++ unordered_map and Rust HashMap have no shared bucket/iteration ABI. Decoded paths and keyed postings agree, but path-prefix IDs and serialized sidecar order are not guaranteed byte-identical across toolchains.
2. Hash-colliding embedding tokens can be accumulated in different map iteration order, causing floating-point last-bit differences.
3. Equal-score result ordering is intentionally unspecified by the C++ score-only comparator and therefore cannot be guaranteed across STL and Rust sorting implementations.
4. ICU4C versus ICU4X/system normalization and libstemmer versus the Rust Porter2 translation require differential corpora to prove every Unicode/token/stem edge case.
5. MSVC std::hash<string_view> behavior is targeted; other C++ standard libraries do not promise the same hash.
6. Internal zero-norm ANN cosine can produce NaN in both implementations; NaN heap/sort behavior is not cross-language total ordering.
7. Non-queued BEIR evaluation with `-query-threads` greater than one remains unsupported because RustBlade does not expose independently shareable reader contexts; queued concurrent evaluation is supported.
8. Portable Rust CLI paging waits for Enter rather than using the C++ platform-specific single-key terminal APIs.
9. Compilation, linking, round-trip index tests, differential search tests, WASM execution, and browser behavior remain unverified because this session explicitly prohibited builds and tests.

## Static completion result

The core RustBlade library has one implementation per C++ component, C++-matching physical filenames and canonical module names, no standalone Rust ranking helper, no disabled legacy/parity modules, no standalone BM25 scorer, no old HNSW alias, and no duplicate root implementation files. Editor diagnostics report no errors.
