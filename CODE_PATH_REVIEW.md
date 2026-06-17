# MoonShot Search Code Path Review

## HTTP Request: `GET /search?q=vector+retrieval&streams=AUTB`

### Entry Point: `ShennongHttp.cpp` - `search_json()`

**Location:** ShennongHttp.cpp, ~line 315-380

```cpp
std::string search_json(const unordered_map<string, string>& params) {
    auto qit = params.find("q");
    const string query = qit == params.end() ? "" : qit->second;
    // query = "vector retrieval"
    
    const auto streamsIt = params.find("streams");
    const string streams = streamsIt != params.end() && !streamsIt->second.empty()
        ? streamsIt->second
        : "AUTB";
    // streams = "AUTB"
    
    const auto started = chrono::steady_clock::now();
    
    vector<SearchResult> results;
    {
        lock_guard<mutex> lock(m_QueryMutex);
        
        // Step 1: Compile query string into EvalTree
        auto tree = unique_ptr<EvalTree>(
            m_Context.GetCompiler()->Compile(query.c_str(), streams.c_str())
        );
        
        if (tree && !tree->IsEmpty()) {
            // Step 2: Build ISR tree from EvalTree
            auto reader = m_Context.GetReader(tree.get());
            
            // Step 3: Execute ISR tree
            auto executor = unique_ptr<IndexSearchExecutor>(m_Context.GetExecutor());
            results = executor->Execute(reader, 0);  // topK=0 means no limit
        }
    }
    
    // Serialize results to JSON and return
}
```

---

## Step 1: Compile Query String → EvalTree

**Class:** IndexSearchCompiler (Compiler/IndexSearchCompiler.h)

```cpp
EvalTree* Compile(const char* queryString,
                  const char* streamSet = "AUT") {
    // queryString = "vector retrieval"
    // streamSet = "AUTB"
    
    // Parse stream set character by character
    auto streams = ParseStreamSet(streamSet);
    // ParseStreamSet("AUTB") → ["A", "U", "T", "B"]
    // HasVectorStream("AUTB") → false (no 'V')
    
    // Tokenize query string
    // "vector retrieval" → ["vector", "retrieval"]
    
    // Build EvalTree with bigram optimization
    // For each token pair (word1, word2):
    //   - Add Term nodes for each stream: "wordnSTREAM"
    //   - Create biigram: "word1\x1Fword2STREAM"
    //   - Structure: Or(bigram_streams, And(word1_streams, word2_streams))
    
    auto root = ParseExpression(queryString, streams);
    // Creates tree structure:
    //   Or[
    //     Or[vectorAUT, vectorBUT],  // biigram for (vector,retrieval)
    //     And[
    //       Or[vectorA, vectorU, vectorT, vectorB],   // unigram
    //       Or[retrievalA, retrievalU, retrievalT, retrievalB]
    //     ]
    //   ]
    
    auto tree = new EvalTree();
    tree->root = root;
    tree->vector_query = empty (no 'V' in streams)
    return tree;
}
```

**Key:** The `streams` parameter (**AUTB**) expands each query term into multiple **stream keys**:
- `A` = Anchor text stream
- `U` = URL stream
- `T` = Title/Text stream  
- `B` = Body text stream

For "vector retrieval":
- Unigrams: "vectorA", "vectorU", "vectorT", "vectorB", "retrievalA", "retrievalU", "retrievalT", "retrievalB"
- Bigram: "vector\x1FretrievalA", "vector\x1FretrievalU", etc.

---

## Step 2: Build ISR Tree from EvalTree

**Method:** `IndexContext::GetReader(EvalTree*)`

**Location:** IndexAccess/IndexContext.h, ~line 171-189

```cpp
shared_ptr<IndexReader> GetReader(EvalTree* evalTree) {
    if (!evalTree || evalTree->IsEmpty()) {
        auto empty = make_shared<AdvancedIndexReader>();
        return empty;
    }
    
    EnsureBuilt();  // Make sure index is prepared
    
    // AUTB has no vector, so:
    if (!evalTree->HasVectorQuery()) {
        return BuildIndexReader(evalTree->root);
        // Recursively build ISR tree from the EvalNode
    }
}
```

### Recursive Tree Building: `BuildIndexReader(EvalNode*)`

**Location:** IndexAccess/IndexContext.h, ~line 485-550

Each node type maps to a reader type:

#### **Term Node** → AdvancedIndexReader (leaf)

```cpp
case NodeType::Term: {
    auto* termNode = static_cast<TermNode*>(node.get());
    auto reader = make_shared<AdvancedIndexReader>();
    
    // Open the posting list for this (term + stream) key
    reader->Open(termNode->stream_key.c_str(),  // e.g. "vectorA"
                 &m_BlockTable,
                 this);
    return reader;
}
```

**Inside AdvancedIndexReader::Open()** (IndexAccess/AdvancedIndexReader.cpp):

1. Calls `BlockTable::FindTermData()` to locate the term in the index
2. Fetches the `IndexBlock` containing the posting data
3. Opens the UnifiedDecoder to iterate over postings (doc_id, term_freq pairs)
4. **Fail-fast asserts** (after your changes):
   - `assert(blockTable)` 
   - `assert(block)` after GetIndexBlock()
   - `assert(marker == BLOCK_CONTINUATION_MARKER)` in OpenContinuation()

#### **Or Node** → OrIndexReader (union of children)

```cpp
case NodeType::Or: {
    vector<shared_ptr<IndexReader>> children;
    
    for (auto& child : orNode->children) {
        children.push_back(BuildIndexReader(child));  // Recursive
    }
    
    if (children.size() == 1)
        return children[0];
    
    return make_shared<OrIndexReader>(children);
    // Combines postings from all children using document-at-a-time (DAAT)
    // Returns union of all matching documents
}
```

#### **And Node** → AndIndexReader (intersection of children)

```cpp
case NodeType::And: {
    vector<shared_ptr<IndexReader>> children;
    
    for (auto& child : andNode->children) {
        children.push_back(BuildIndexReader(child));  // Recursive
    }
    
    return make_shared<AndIndexReader>(children);
    // Intersection using DAAT pivot alignment
}
```

---

## Step 3: Execute ISR Tree

**Method:** `IndexSearchExecutor::Execute()`

**Location:** Executor/IndexSearchExecutor.cpp, ~line 20-48

```cpp
vector<SearchResult> Execute(shared_ptr<IndexReader> reader, int topK) {
    assert(reader);
    
    if (reader->IsEnd())
        return {};
    
    vector<SearchResult> results;
    
    while (!reader->IsEnd()) {
        uint64_t docId = reader->GetDocumentID();
        
        // **CRITICAL: Fail-fast asserts after your changes**
        assert(m_Context);
        const DocRecord* record = m_Context->GetDocRecord(docId);
        assert(record);  // No nullptr fallback
        
        // Score = BM25 + static rank bonus
        float score = reader->GetScore(record)
                    + DecodeFloat16(record->DR_StaticRankHalf);
        
        results.push_back({docId, score, ""});
        reader->GoNext();
    }
    
    SortAndTruncate(results, topK);
    return results;
}
```

### BM25 Scoring: `AdvancedIndexReader::GetScore()`

**Location:** IndexAccess/AdvancedIndexReader.cpp, ~line 160-180

```cpp
float GetScore(const DocRecord* record) {
    assert(record);                  // NEW: fail-fast
    assert(m_Context);               // NEW: fail-fast
    assert(docLength > 0);           // NEW: fail-fast
    
    const IndexFileHeader& header = m_Context->GetIndexFileHeader();
    const uint64_t documentCount = header.IFH_NumDocuments;
    const float averageDocLength = header.IFH_AvgDocLength;
    
    assert(documentCount > 0);       // NEW: fail-fast
    assert(averageDocLength > 0.0f); // NEW: fail-fast
    
    const float tf = (float)GetTermFreq();
    const float df = (float)max(1u, m_DocFreq);
    const float totalDocs = (float)documentCount;
    const float dl = (float)max(1u, docLength);
    
    // Standard BM25 formula
    static constexpr float K1 = 1.2f, B = 0.75f;
    
    const float idf = max(0.0f,
        log((totalDocs - df + 0.5f) / (df + 0.5f) + 1.0f));
    
    const float tfNorm = tf * (K1 + 1.0f) /
        (tf + K1 * (1.0f - B + B * dl / averageDocLength));
        // OLD: (... ? averageDocLength : 1.0f))
        // NEW: Direct division (averageDocLength guaranteed > 0)
    
    return idf * tfNorm;
}
```

---

## Complete Data Flow Summary for "vector retrieval" + AUTB

```
HTTP Request
    ↓
search_json() — parse q="vector retrieval", streams="AUTB"
    ↓
Compiler.Compile("vector retrieval", "AUTB")
    ↓ Tokenize → ["vector", "retrieval"]
    ↓ ParseStreamSet("AUTB") → ["A", "U", "T", "B"]
    ↓ Build EvalTree: Or of:
      - Bigram streams (vector\x1Fretrieval in A,U,T,B)
      - AND(unigrams in A,U,T,B)
    ↓
GetReader(EvalTree*)
    ↓
BuildIndexReader(EvalNode*)  [Recursive]
    ├── Or Node
    │   ├── Or Node (biigram in 4 streams) → 4 AdvancedIndexReaders
    │   └── And Node (bigram + unigram) → AndIndexReader
    │       ├── Or(vectorA, vectorU, vectorT, vectorB) → OrIndexReader
    │       └── Or(retrievalA, retrievalU, retrievalT, retrievalB) → OrIndexReader
    │
    └── Result: OrIndexReader with 2 children:
        - OrIndexReader of bigram readers
        - AndIndexReader of (vector OR's) AND (retrieval OR's)
    ↓
Execute(IndexReader reader, topK=0)
    ↓
Loop: while (!reader->IsEnd())
    ├── GetDocumentID() — current doc from DAAT traversal
    ├── GetDocRecord() — assert(record) [FAIL-FAST]
    ├── GetScore()     — BM25 with fail-fast asserts
    └── GoNext()       — advance to next matching doc
    ↓
SortAndTruncate(results, topK=0)
    ↓ Sort by score descending
    ↓ Return all results (topK=0 = unlimited)
    ↓
JSON serialize + HTTP response
```

---

## Key Invariants Now Protected by Fail-Fast Assertions

After your changes, the following invariants are **enforced, not fallback**:

### In Execute():
- `m_Context != nullptr`
- `record != nullptr` for every docId returned by reader
- No default scoring without valid DocRecord

### In GetScore():
- `record != nullptr`
- `docLength > 0`  
- `documentCount > 0`
- `averageDocLength > 0.0f`
- No division by `averageDocLength <= 0`

### In AdvancedIndexReader::Open():
- `streamKey != nullptr`
- `blockTable != nullptr`
- `context != nullptr`
- `block != nullptr` after GetIndexBlock()

### In AdvancedIndexReader::GoNext():
- `next != nullptr` when HasMoreBlocks()

### In AdvancedIndexReader::OpenContinuation():
- `marker == BLOCK_CONTINUATION_MARKER` (no old format fallback)
- Index format must be consistent

---

## Performance Impact

- **Debug build:** Small overhead from `assert()` checks (typical 2-3%)
- **Release build:** No runtime cost (asserts compiled out with `-DNDEBUG`)
- **Fail-fast benefit:** Core dump on first corruption rather than silently wrong results
