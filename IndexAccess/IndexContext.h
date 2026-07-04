#ifndef INDDEXCONTEXT_H__
#define INDDEXCONTEXT_H__

#include "AdvancedIndexReader.h"
#include "AdvancedIndexWriter.h"
#include "IndexReaderImpl.h"
#include "PostingStore.h"
#include "IndexSerializer.h"
#include "EvalExpression.h"
#include "IndexSearchExecutor.h"
#include "IndexSearchCompiler.h"
#include "ConfigParameters.h"
#include "Tokenizer.h"
#include "Embeddings.h"
#include "FileAccess.h"
#include "MemOperation.h"

#include <memory>
#include <array>
#include <string>
#include <string_view>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <utility>
#include <unordered_map>
#include <thread>
#include <vector>

using std::shared_ptr;
using std::make_shared;
using std::string;

struct Document
{
    uint64_t    doc_id = UINT64_MAX;
    std::string path;
    std::string url;
    std::string title;
    std::string body;
    std::string anchor;
    std::string meta;
    float       importance = 0.0f;
};

class SearchTask
{
public:
    SearchTask() = default;

    std::vector<SearchResult> Wait()
    {
        return m_State ? m_State->Future.get() : std::vector<SearchResult>{};
    }

    bool Valid() const
    {
        return m_State && m_State->Future.valid();
    }

private:
    friend class IndexContext;

    struct State
    {
        std::string Query;
        std::vector<float> Vector;
        std::string Streams = "AUTB";
        int TopK = 1000;
        QueryCompileMode Mode = QueryCompileMode::WeakAndBigramBoostForDoc;
        size_t VectorEfSearch = 1000;
        std::promise<std::vector<SearchResult>> Promise;
        std::shared_future<std::vector<SearchResult>> Future;
    };

    explicit SearchTask(std::shared_ptr<State> state)
        : m_State(std::move(state))
    {}

    std::shared_ptr<State> m_State;
};

/*
* IndexContext — central factory that owns BlockTable and PostingStore.
*
* Write path:  GetWriter() → AdvancedIndexWriter → PostingStore
* Build step:  Build() encodes PostingStore entries into IndexBlocks
*              and populates the BlockTable + TermToBlock mapping.
* Read path:   GetReader() / GetReader(EvalTree*)
*              → BuildIndexReader() recursively converts EvalTree to ISR tree
*                  TermNode → AdvancedIndexReader (leaf, owns block + decoder)
*                  AndNode  → AndIndexReader
*                  OrNode   → OrIndexReader
*                  NotNode  → NotIndexReader
*
* AdvancedIndexReader is the leaf ISR, equivalent to REF's ISRWord.
* Composite readers aggregate BM25 and term frequency from the leaves.
*/
class IndexContext
{
public:
    explicit IndexContext(const char* configFile = "",
                         const char* indexFile   = "",
                         bool        loadDelta   = true)
        : m_Store(make_shared<PostingStore>())
        , m_Params(make_shared<ConfigParameters>())
        , m_BlockTable(512)
        , m_Compiler(&m_Tokenizer)
        , m_Executor(this)
        , m_IndexPath(indexFile ? indexFile : "")
        , m_Built(false)
        , m_LoadedFromDisk(false)
        , m_LoadDelta(loadDelta)
        , m_VectorBuilt(false)
    {
        if (!m_IndexPath.empty())
            LoadIndex();
    }

    ~IndexContext()
    {
        StopSearchWorkers();
        ClearPinnedIndexData();
        ClearWritePinnedIndexData();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
        return make_shared<AdvancedIndexWriter>(m_Store);
    }

    uint64_t AllocateDocumentID() const
    {
        uint64_t nextId = DocDataFirstDocId(m_DocData, m_IndexFileHeader) + m_IndexFileHeader.IFH_NumDocuments;
        if (m_DeltaContext)
            nextId = std::max(nextId, DocDataFirstDocId(m_DeltaContext->m_DocData, m_DeltaContext->m_IndexFileHeader) + m_DeltaContext->m_IndexFileHeader.IFH_NumDocuments);

        for (const auto& [docId, _] : m_Store->AllDocStats())
            nextId = std::max(nextId, docId + 1);

        return nextId;
    }

    uint64_t AddDocument(const Document& doc, bool buildVector = true)
    {
        const uint64_t docId = doc.doc_id == UINT64_MAX ? AllocateDocumentID() : doc.doc_id;
        auto writer = GetWriter();

        auto titleTokens = m_Tokenizer.Tokenize(doc.title.c_str());
        const std::string url = doc.url.empty() ? doc.path : doc.url;
        auto urlTokens = m_Tokenizer.Tokenize(url.c_str());
        auto anchorTokens = m_Tokenizer.Tokenize(doc.anchor.c_str());
        auto bodyTokens = m_Tokenizer.Tokenize(doc.body.c_str());
        auto metaTokens = m_Tokenizer.Tokenize(doc.meta.c_str());

        writer->Write(titleTokens, docId, "Title");
        writer->Write(urlTokens, docId, "URL");
        writer->Write(anchorTokens, docId, "Anchor");
        writer->Write(bodyTokens, docId, "Body");
        writer->Write(metaTokens, docId, "Meta");
        writer->SetDocImportance(docId, doc.importance);
        if (buildVector) {
            std::vector<std::string> embeddingTokens;
            embeddingTokens.reserve(titleTokens.size() + urlTokens.size() + anchorTokens.size() + bodyTokens.size() + metaTokens.size());
            embeddingTokens.insert(embeddingTokens.end(), titleTokens.begin(), titleTokens.end());
            embeddingTokens.insert(embeddingTokens.end(), urlTokens.begin(), urlTokens.end());
            embeddingTokens.insert(embeddingTokens.end(), anchorTokens.begin(), anchorTokens.end());
            embeddingTokens.insert(embeddingTokens.end(), bodyTokens.begin(), bodyTokens.end());
            embeddingTokens.insert(embeddingTokens.end(), metaTokens.begin(), metaTokens.end());
            writer->SetDocVector(docId, m_VectorIndex.GetModel()->Embed(embeddingTokens));
        }
        if (!doc.path.empty())
            m_Store->SetDocPath(docId, doc.path);

        m_WriteBuilt = false;
        if (!m_LoadedFromDisk)
            m_Built = false;

        return docId;
    }

    /*
    * Build() — delegates to IndexSerializer::BuildBlocks so the
    * block packing logic is in one place.
    */
    void Build()
    {
        if (m_Built) {
            BuildVectorRuntime();
            return;
        }

        if (BuildRuntime(m_BlockTable, m_VectorIndex, m_IndexFileHeader, m_DocData, m_PathPrefixSidecar, m_PathPrefixes, m_Built)) {
            m_LoadedFromDisk = false;
            m_VectorBuilt = true;
        }
    }

    /*
    * Build an ISR tree from a compiled EvalTree.
    * Equivalent to REF's ISRCreatorDocShard::CreateMatchIsr().
    */
    shared_ptr<IndexReader> GetReader(EvalTree* evalTree)
    {
        if (!evalTree || evalTree->IsEmpty()) {
            auto empty = make_shared<AdvancedIndexReader>();
            return empty;
        }

        auto baseReader = BuildLocalReader(evalTree);
        if (HasDelta()) {
            std::vector<shared_ptr<IndexReader>> children;
            children.push_back(baseReader);
            children.push_back(m_DeltaContext->BuildLocalReader(evalTree));
            return make_shared<OrIndexReader>(std::move(children));
        }

        return baseReader;
    }

    shared_ptr<IndexReader> BuildLocalReader(EvalTree* evalTree)
    {
        if (!evalTree || evalTree->IsEmpty()) {
            auto empty = make_shared<AdvancedIndexReader>();
            return empty;
        }

        if (evalTree->HasTextQuery() && evalTree->HasVectorQuery())
            return BuildIndexReader(evalTree->root);

        if (evalTree->HasVectorQuery())
            return BuildVectorIndexReader(evalTree->vector_query, evalTree->vector_ef_search);

        return BuildIndexReader(evalTree->root);
    }

    Tokenizer*           GetTokenizer() { return &m_Tokenizer; }
    IndexSearchCompiler* GetCompiler()  { return &m_Compiler; }
    IndexSearchExecutor* GetExecutor()  { return new IndexSearchExecutor(this); }

    EvalTree* Compile(const char* queryString,
                      const char* streamSet = "AUT",
                      QueryCompileMode mode = QueryCompileMode::Default)
    {
        return m_Compiler.Compile(queryString, streamSet, m_VectorIndex.GetModel(), mode);
    }

    /* Compile query string and return an ISR tree ready for traversal. */
    shared_ptr<IndexReader> GetReader(const char* queryString,
                                      const char* streamSet = "AUT",
                                      QueryCompileMode mode = QueryCompileMode::Default)
    {
        auto tree = std::unique_ptr<EvalTree>(Compile(queryString, streamSet, mode));
        return GetReader(tree.get());
    }

    /* Open a reader for one specific stream key (e.g. "raceA", "carT"). */
    shared_ptr<IndexReader> GetStreamReader(const char* streamKey)
    {
        auto baseReader = BuildLocalStreamReader(streamKey);
        if (HasDelta()) {
            std::vector<shared_ptr<IndexReader>> children;
            children.push_back(baseReader);
            children.push_back(m_DeltaContext->BuildLocalStreamReader(streamKey));
            return make_shared<OrIndexReader>(std::move(children));
        }
        return baseReader;
    }

    shared_ptr<IndexReader> BuildLocalStreamReader(const char* streamKey)
    {
        auto reader = make_shared<AdvancedIndexReader>();
        reader->Open(streamKey, &m_BlockTable, this);
        return reader;
    }

    SearchTask Enqueue(const char* query,
                       std::vector<float> vector = {},
                       const char* streams = "AUTB",
                       int topK = 1000,
                       QueryCompileMode mode = QueryCompileMode::WeakAndBigramBoostForDoc)
    {
        auto state = std::make_shared<SearchTask::State>();
        state->Query = query ? query : "";
        state->Vector = std::move(vector);
        state->Streams = streams && *streams ? streams : "AUTB";
        state->TopK = topK;
        state->Mode = mode;
        state->Future = state->Promise.get_future().share();

        EnsureSearchWorkersStarted();

        {
            std::lock_guard<std::mutex> lock(m_SearchQueueMutex);
            m_SearchQueue.push_back(state);
        }
        m_SearchQueueCv.notify_one();
        return SearchTask(state);
    }

    PostingStore* GetStore() { return m_Store.get(); }

    std::string GetDocPath(uint64_t docId) const
    {
        docId = ReaderDocumentIDValue(docId);
        const uint64_t firstDocId = DocDataFirstDocId(m_DocData, m_IndexFileHeader);
        if (m_DocData && docId >= firstDocId && docId - firstDocId < m_IndexFileHeader.IFH_NumDocuments) {
            const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + (docId - firstDocId) * DOC_REC_SIZE);
            if (entry->DDE_DocID == docId)
                return DecodeDocPath(*entry, m_PathPrefixes);
        }
        return m_DeltaContext ? m_DeltaContext->GetDocPath(docId) : std::string{};
    }

    const DocDataEntry* GetDocDataEntry(uint64_t docId) const
    {
        docId = ReaderDocumentIDValue(docId);
        const uint64_t firstDocId = DocDataFirstDocId(m_DocData, m_IndexFileHeader);
        if (!m_DocData || docId < firstDocId || docId - firstDocId >= m_IndexFileHeader.IFH_NumDocuments)
            return m_DeltaContext ? m_DeltaContext->GetDocDataEntry(docId) : nullptr;

        const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + (docId - firstDocId) * DOC_REC_SIZE);
        if (entry->DDE_DocID == docId)
            return entry;
        return m_DeltaContext ? m_DeltaContext->GetDocDataEntry(docId) : nullptr;
    }

    const uint8_t* RawDocData() const { return m_DocData; }

    const IndexFileHeader& GetIndexFileHeader() const { return m_IndexFileHeader; }
    uint64_t DocumentCount() const { return m_IndexFileHeader.IFH_NumDocuments; }

    uint32_t GetStreamDocFreq(const char* streamKey)
    {
        uint32_t indexBlockID = 0;
        uint32_t indexOffset = 0;
        uint32_t indexLength = 0;
        uint32_t docFreq = 0;
        uint32_t continuationBlocks = 0;
        uint64_t totalDocFreq = 0;
        if (m_BlockTable.FindTermData(streamKey,
                                      &indexBlockID,
                                      &indexOffset,
                                      &indexLength,
                                      &docFreq,
                                      &continuationBlocks)) {
            totalDocFreq += docFreq;
        }
        if (m_DeltaContext)
            totalDocFreq += m_DeltaContext->GetStreamDocFreq(streamKey);
        return static_cast<uint32_t>(std::min<uint64_t>(totalDocFreq, UINT32_MAX));
    }

    void SetTermMphfEnabled(bool enabled)
    {
        m_BlockTable.SetTermMphfEnabled(enabled);
        if (m_DeltaContext)
            m_DeltaContext->SetTermMphfEnabled(enabled);
    }

    void SetDirectBlockAccessEnabled(bool enabled)
    {
        m_BlockTable.SetDirectBlockAccessEnabled(enabled);
        if (m_DeltaContext)
            m_DeltaContext->SetDirectBlockAccessEnabled(enabled);
    }

    IndexBlockTable::BlockAccessStats GetBlockAccessStats() const
    {
        return m_BlockTable.GetBlockAccessStats();
    }

    void SetWeakAndBuildMode(WeakAndBuildMode mode)
    {
        m_WeakAndBuildMode = mode;
        if (m_DeltaContext)
            m_DeltaContext->SetWeakAndBuildMode(mode);
    }

    void SetQueryParameters(const QueryCompileModeParameters& parameters)
    {
        m_QueryParameters = parameters;
        if (m_DeltaContext)
            m_DeltaContext->SetQueryParameters(m_QueryParameters);
    }

    float GetSpanWeight(uint32_t wordSpan) const
    {
        return wordSpan >= 2 ? m_QueryParameters.QMP_BigramWeight : m_QueryParameters.QMP_UnigramWeight;
    }

    const QueryCompileModeParameters& GetQueryParameters() const { return m_QueryParameters; }

    float GetAverageStreamLength(char stream) const
    {
        EnsureStreamLengthStats();
        switch (stream) {
        case 'T': return m_AvgTitleLength;
        case 'B': return m_AvgBodyLength;
        case 'U': return m_AvgUrlLength;
        case 'A': return m_AvgAnchorLength;
        case 'M': return m_AvgMetaLength;
        default: return std::max(1.0f, m_IndexFileHeader.IFH_AvgDocLength);
        }
    }

    static uint32_t GetStreamLength(const DocDataEntry& entry, char stream)
    {
        const uint32_t docLength = std::max<uint32_t>(1, entry.DDE_BodyLength);
        auto featureLength = [&](size_t index) {
            switch (index) {
            case 0: return entry.DDE_TitleLength;
            case 1: return entry.DDE_BodyLength;
            case 4: return entry.DDE_UrlLength;
            case 5: return entry.DDE_AnchorLength;
            case 6: return entry.DDE_MetaLength;
            default: return 0u;
            }
        };
        uint32_t streamLength = 0;
        switch (stream) {
        case 'T': streamLength = featureLength(0); break;
        case 'B': streamLength = featureLength(1); break;
        case 'U': streamLength = featureLength(4); break;
        case 'A': streamLength = featureLength(5); break;
        case 'M': streamLength = featureLength(6); break;
        default: break;
        }
        return streamLength > 0 ? streamLength : docLength;
    }

    void SetLeafTermCacheBytes(uint64_t bytes)
    {
        m_LeafTermCacheBytes = bytes > 0 ? bytes : LEAF_TERM_CACHE_BYTES;
        if (m_DeltaContext)
            m_DeltaContext->SetLeafTermCacheBytes(bytes);
    }

    std::vector<float> CompileToVector(const char* queryString)
    {
        return m_Compiler.CompileToVector(queryString, m_VectorIndex.GetModel());
    }

    std::vector<VectorSearchResult> VectorSearch(const std::vector<float>& query,
                                                 size_t topK = 20,
                                                 VectorMetric metric = VectorMetric::Cosine,
                                                 size_t efSearch = 200)
    {
        BuildVectorRuntime();
        return m_VectorIndex.Search(query, topK, metric, efSearch);
    }

    size_t VectorCount() const { return m_VectorIndex.Size(); }
    size_t VectorDimension() const { return m_VectorIndex.Dimension(); }

    bool HasDelta() const
    {
        return m_DeltaContext && m_DeltaContext->DocumentCount() > 0;
    }

    IndexContext* GetDeltaContext()
    {
        return HasDelta() ? m_DeltaContext.get() : nullptr;
    }

    bool SaveIndex()
    {
        if (m_IndexPath.empty()) return false;
        return SaveIndex(m_IndexPath.c_str());
    }

    bool SaveIndex(const char* path)
    {
        if (!path || !*path) return false;
        const std::string savePath(path);
        const bool savingDeltaIndex = !m_IndexPath.empty() && savePath == DeltaIndexPath(m_IndexPath);
        const bool overwritingLoadedIndex = !savingDeltaIndex && m_LoadedFromDisk && m_IndexPath == savePath;
        if (!savingDeltaIndex)
            m_IndexPath = savePath;

        if (!BuildRuntime(m_WriteBlockTable, m_WriteVectorIndex, m_WriteIndexFileHeader, m_WriteDocData, m_WritePathPrefixSidecar, m_WritePathPrefixes, m_WriteBuilt, false))
            return false;

        if (overwritingLoadedIndex) {
            ClearPinnedIndexData();
            m_BlockTable.SetBlockMemory(nullptr, nullptr);
            m_Executor = IndexSearchExecutor(this);
            m_Built = false;
            m_LoadedFromDisk = false;
        }

        if (!IndexSerializer::Save(m_WriteIndexFileHeader, m_WriteBlockTable, m_WriteDocData, m_WritePathPrefixSidecar.data(), path)) return false;

        if (!savingDeltaIndex)
            return true;

        auto delta = std::make_unique<IndexContext>("", "", false);
        delta->m_IndexPath = savePath;
        delta->m_BlockTable.HandOverBlockTable(m_WriteBlockTable);
        delta->m_VectorIndex = std::move(m_WriteVectorIndex);
        delta->m_IndexFileHeader = m_WriteIndexFileHeader;
        delta->m_DocData = m_WriteDocData;
        delta->m_PathPrefixSidecar = m_WritePathPrefixSidecar;
        delta->m_PathPrefixes = m_WritePathPrefixes;
        delta->m_Built = true;
        delta->m_LoadedFromDisk = true;
        delta->m_VectorBuilt = false;
        delta->m_Executor = IndexSearchExecutor(delta.get());

        m_WriteDocData = nullptr;
        m_WriteIndexFileHeader = {};
        m_WritePathPrefixSidecar = {};
        m_WritePathPrefixes.clear();
        m_WriteBuilt = false;
        m_WriteVectorIndex.Clear();

        m_DeltaContext.reset();
        m_DeltaContext = std::move(delta);
        return true;
    }

    bool Merge(const char* outputPath)
    {
        if (!outputPath || !*outputPath) return false;
        if (!m_Built || !m_DocData) return false;
        if (!HasDelta()) return false;
        if (!m_DeltaContext->m_Built || !m_DeltaContext->m_DocData) return false;

        // Step 1: Prepare the merge inputs and final temp file.
        // The base index is this loaded context. The delta index is the hidden
        // delta context loaded from <base>.delta.idx. The final output is first
        // written to <output>.tmp so the old index is not destroyed by a failed
        // merge.
        const auto mergeStart = std::chrono::steady_clock::now();
        const std::string tempPath = std::string(outputPath) + ".tmp";
        const std::string prefix(outputPath);
        const std::string headTempPath = prefix + ".head.tmp";
        const std::string leafTempPath = prefix + ".leaf.tmp";
        const std::string indexTempPath = prefix + ".blocks.tmp";
        const std::string docDataTempPath = prefix + ".docdata.tmp";
        const std::string mphfEntryTempPath = prefix + ".mphf.entries.tmp";
        std::remove(tempPath.c_str());
        std::remove(headTempPath.c_str());
        std::remove(leafTempPath.c_str());
        std::remove(indexTempPath.c_str());
        std::remove(docDataTempPath.c_str());
        std::remove(mphfEntryTempPath.c_str());

        auto cleanupTempFiles = [&]() {
            std::remove(tempPath.c_str());
            std::remove(headTempPath.c_str());
            std::remove(leafTempPath.c_str());
            std::remove(indexTempPath.c_str());
            std::remove(docDataTempPath.c_str());
            std::remove(mphfEntryTempPath.c_str());
        };

        IndexContext& delta = *m_DeltaContext;
        std::cout << "  merge input: baseDocs=" << m_IndexFileHeader.IFH_NumDocuments
              << " deltaDocs=" << delta.m_IndexFileHeader.IFH_NumDocuments
              << " baseLeafBlocks=" << m_IndexFileHeader.IFH_LeafTermBlockCount
              << " deltaLeafBlocks=" << delta.m_IndexFileHeader.IFH_LeafTermBlockCount
              << " baseIndexBlocks=" << m_IndexFileHeader.IFH_IndexBlockCount
              << " deltaIndexBlocks=" << delta.m_IndexFileHeader.IFH_IndexBlockCount
              << "\n";

        // Step 2: Write DocData to a temp stream. Path prefix ids are local to
        // each index, so decode base/delta full paths and re-intern them into
        // the merged sidecar while copying DocData records.
        uint64_t mergedDocs = 0;
        float avgDocLength = 1.0f;
        std::array<uint8_t, PATH_PREFIX_SIDECAR_BYTES> mergedPathPrefixSidecar{};
        std::vector<std::string> mergedPathPrefixes;
        {
            auto stepStart = std::chrono::steady_clock::now();
            FileAccess docFile(docDataTempPath.c_str());
            if (!docFile.InitWrite()) { cleanupTempFiles(); return false; }

            const uint64_t baseDocCount = m_IndexFileHeader.IFH_NumDocuments;
            const uint64_t deltaFirstDocId = DocDataFirstDocId(delta.m_DocData, delta.m_IndexFileHeader);
            const uint64_t deltaDocCount = delta.m_IndexFileHeader.IFH_NumDocuments;
            if (deltaDocCount > 0 && deltaFirstDocId != baseDocCount) { cleanupTempFiles(); return false; }
            mergedDocs = baseDocCount + deltaDocCount;

            uint64_t totalDocLength = static_cast<uint64_t>(static_cast<double>(m_IndexFileHeader.IFH_AvgDocLength) * static_cast<double>(baseDocCount));
            PathPrefixBuildState pathState;

            auto writeDocDataRecords = [&](const uint8_t* sourceDocData,
                                           const IndexFileHeader& sourceHeader,
                                           const std::vector<std::string>& sourcePrefixes) -> bool {
                if (!sourceDocData || sourceHeader.IFH_NumDocuments == 0)
                    return true;
                const uint64_t firstDocId = DocDataFirstDocId(sourceDocData, sourceHeader);
                for (uint64_t slot = 0; slot < sourceHeader.IFH_NumDocuments; ++slot) {
                    DocDataEntry entry{};
                    std::memcpy(&entry, sourceDocData + slot * DOC_REC_SIZE, sizeof(entry));
                    const uint64_t expectedDocId = firstDocId + slot;
                    if (entry.DDE_DocID == expectedDocId) {
                        const std::string fullPath = DecodeDocPath(entry, sourcePrefixes);
                        EncodeDocPath(entry, fullPath, pathState);
                    }
                    if (!docFile.PutData(&entry, sizeof(entry)))
                        return false;
                }
                return true;
            };

            if (!writeDocDataRecords(m_DocData, m_IndexFileHeader, m_PathPrefixes)) { cleanupTempFiles(); return false; }

            if (deltaDocCount > 0) {
                if (!writeDocDataRecords(delta.m_DocData, delta.m_IndexFileHeader, delta.m_PathPrefixes)) { cleanupTempFiles(); return false; }

                totalDocLength += static_cast<uint64_t>(static_cast<double>(delta.m_IndexFileHeader.IFH_AvgDocLength) * static_cast<double>(deltaDocCount));
            }

            mergedPathPrefixes = pathState.Prefixes;
            BuildPathPrefixSidecarBytes(mergedPathPrefixes, mergedPathPrefixSidecar);

            avgDocLength = mergedDocs ? static_cast<float>(totalDocLength) / static_cast<float>(mergedDocs) : 1.0f;
            std::cout << "  merge step2 wrote docdata bytes=" << (mergedDocs * DOC_REC_SIZE)
                      << " docs=" << mergedDocs
                      << " in " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stepStart).count()
                      << " ms\n";
        }

        // Step 3: Do a 3-way physical merge: base LeafTermEntry stream + delta
        // LeafTermEntry stream -> merged leaf/index/head temp streams. The two
        // input streams are walked in term order directly from their resident
        // LeafTermBlock pages; no full term table and no decoded PostingStore is
        // built. For each output term, mergedCursor uses a bounded merge-local
        // IndexBlockTable as its posting byte view: base-only and delta-only
        // terms copy that term's raw posting bytes into it, while equal terms
        // append base bytes then delta bytes. Delta postings already use final
        // doc ids, so there is no doc-id remap and no varbyte decode/re-encode.
        //
        // The merged posting bytes and LeafTermEntry records are packed into a
        // fixed merge-local block table. When either side fills, the current
        // batch is appended to the temp streams and the same term is retried.
        uint64_t headEntryCount = 0;
        uint64_t totalTerms = 0;
        uint32_t indexBlockCount = 0;
        {
            auto stepStart = std::chrono::steady_clock::now();
            uint64_t baseOnlyTerms = 0;
            uint64_t deltaOnlyTerms = 0;
            uint64_t mergedPairTerms = 0;
            uint64_t retryDumps = 0;
            uint64_t finalDumps = 0;
            uint64_t dumpedLeafBlocks = 0;
            uint64_t dumpedIndexBlocks = 0;
            auto headFile = std::make_unique<FileAccess>(headTempPath.c_str());
            auto indexFile = std::make_unique<FileAccess>(indexTempPath.c_str());
            auto leafFile = std::make_unique<FileAccess>(leafTempPath.c_str());
            if (!headFile->InitWrite() || !indexFile->InitWrite() || !leafFile->InitWrite()) { cleanupTempFiles(); return false; }

            LeafTermBlockView baseCursor(m_BlockTable);
            LeafTermBlockView deltaCursor(delta.m_BlockTable);
            constexpr uint32_t MERGED_LEAF_BLOCK_LIMIT = 32 * 1024;
            constexpr uint32_t MERGED_INDEX_BLOCK_LIMIT = 32 * 1024;
            IndexBlockTable mergedBlockTable;
            auto* mergedIndexBlocks = mergedBlockTable.Init(BlockKind::Index, nullptr, 0, MERGED_INDEX_BLOCK_LIMIT, MERGED_INDEX_BLOCK_LIMIT);
            auto* mergedLeafBlocks = mergedBlockTable.Init(BlockKind::LeafTerm, nullptr, 0, MERGED_LEAF_BLOCK_LIMIT, MERGED_LEAF_BLOCK_LIMIT);
            if (!mergedIndexBlocks || !mergedLeafBlocks) {
                cleanupTempFiles();
                return false;
            }
            std::memset(mergedIndexBlocks, 0, static_cast<size_t>(MERGED_INDEX_BLOCK_LIMIT) * sizeof(IndexBlock));
            std::memset(mergedLeafBlocks, 0, static_cast<size_t>(MERGED_LEAF_BLOCK_LIMIT) * sizeof(LeafTermBlock));
            LeafTermBlockView mergedCursor(mergedBlockTable, false);

            auto dumpBatch = [&]() -> bool {
                const uint32_t usedLeafBlocks = mergedCursor.UsedLeafBlockCount();
                const uint32_t usedIndexBlocks = mergedCursor.UsedIndexBlockCount();
                if (usedLeafBlocks == 0 && usedIndexBlocks == 0) return true;

                std::vector<HeadTermEntry> heads(usedLeafBlocks);
                const auto* leafBlocks = reinterpret_cast<const LeafTermBlock*>(mergedLeafBlocks);
                for (uint32_t leafBlockID = 0; leafBlockID < usedLeafBlocks; ++leafBlockID) {
                    const auto& leaf = leafBlocks[leafBlockID];
                    const uint32_t entryCount = std::min<uint32_t>(leaf.LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1], LEAF_TERM_DIRECTORY_COUNT - 1);
                    const auto* leafBase = reinterpret_cast<const uint8_t*>(&leaf);
                    const auto* firstEntry = reinterpret_cast<const LeafTermEntry*>(leafBase + leaf.LTB_Directory[0]);
                    heads[leafBlockID].HTE_LeafTermBlockID = static_cast<uint32_t>(headEntryCount + leafBlockID);
                    heads[leafBlockID].HTE_FirstTermLength = firstEntry->LTE_TermLength;
                    std::memcpy(heads[leafBlockID].HTE_FirstTerm, firstEntry->LTE_Term, firstEntry->LTE_TermLength);
                    totalTerms += entryCount;
                }

                if (!headFile->PutData(heads.data(), static_cast<uint64_t>(usedLeafBlocks) * sizeof(HeadTermEntry))) return false;
                if (!leafFile->PutData(mergedLeafBlocks, static_cast<uint64_t>(usedLeafBlocks) * sizeof(LeafTermBlock))) return false;
                headEntryCount += usedLeafBlocks;

                if (!indexFile->PutData(mergedIndexBlocks, static_cast<uint64_t>(usedIndexBlocks) * sizeof(IndexBlock))) return false;
                indexBlockCount += usedIndexBlocks;
                dumpedLeafBlocks += usedLeafBlocks;
                dumpedIndexBlocks += usedIndexBlocks;

                std::memset(mergedIndexBlocks, 0, static_cast<size_t>(MERGED_INDEX_BLOCK_LIMIT) * sizeof(IndexBlock));
                std::memset(mergedLeafBlocks, 0, static_cast<size_t>(MERGED_LEAF_BLOCK_LIMIT) * sizeof(LeafTermBlock));
                mergedCursor.ResetWrite(indexBlockCount);
                return true;
            };

            while (baseCursor.Current() || deltaCursor.Current()) {
                const auto* baseCurrent = baseCursor.Current();
                const auto* deltaCurrent = deltaCursor.Current();
                const bool takeBase = baseCurrent && (!deltaCurrent || !(deltaCursor < baseCursor));
                const bool takeDelta = deltaCurrent && (!baseCurrent || !(baseCursor < deltaCursor));
                bool advanceBase = false;
                bool advanceDelta = false;

                if (takeBase && takeDelta) {
                    if (mergedCursor.CanAddLeafTermEntry(baseCursor)
                        && mergedCursor.AddIndex(baseCursor, deltaCursor)
                        && mergedCursor.AddLeafTermEntry(baseCursor)) {
                        advanceBase = true;
                        advanceDelta = true;
                        ++mergedPairTerms;
                    } else {
                        ++retryDumps;
                        if (!dumpBatch()) { cleanupTempFiles(); return false; }
                    }
                } else if (takeBase) {
                    if (mergedCursor.CanAddLeafTermEntry(baseCursor)
                        && mergedCursor.AddIndex(baseCursor)
                        && mergedCursor.AddLeafTermEntry(baseCursor)) {
                        advanceBase = true;
                        ++baseOnlyTerms;
                    } else {
                        ++retryDumps;
                        if (!dumpBatch()) { cleanupTempFiles(); return false; }
                    }
                } else if (takeDelta) {
                    if (mergedCursor.CanAddLeafTermEntry(deltaCursor)
                        && mergedCursor.AddIndex(deltaCursor)
                        && mergedCursor.AddLeafTermEntry(deltaCursor)) {
                        advanceDelta = true;
                        ++deltaOnlyTerms;
                    } else {
                        ++retryDumps;
                        if (!dumpBatch()) { cleanupTempFiles(); return false; }
                    }
                } else {
                    // Never hit here: loop condition guarantees at least one cursor has data.
                    cleanupTempFiles();
                    return false;
                }
                if (advanceBase) ++baseCursor;
                if (advanceDelta) ++deltaCursor;
            }

            if (mergedCursor.UsedLeafBlockCount() > 0 || mergedCursor.UsedIndexBlockCount() > 0)
                ++finalDumps;
            if (!dumpBatch()) { cleanupTempFiles(); return false; }
            headFile.reset();
            indexFile.reset();
            leafFile.reset();
            std::cout << "  merge step3 wrote leaf/index batches in "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stepStart).count()
                      << " ms"
                      << " terms(base=" << baseOnlyTerms
                      << ", delta=" << deltaOnlyTerms
                      << ", equal=" << mergedPairTerms
                      << ", total=" << totalTerms
                      << ") dumps(retry=" << retryDumps
                      << ", final=" << finalDumps
                      << ") blocks(leaf=" << dumpedLeafBlocks
                      << ", index=" << dumpedIndexBlocks
                      << ")\n";
        }

        // Step 4: Assemble <output>.tmp in v14 physical order, patch the header,
        // release old file handles, then rename <output>.tmp to the final path.
        {
            auto stepStart = std::chrono::steady_clock::now();
            FileAccess output(tempPath.c_str());
            if (!output.InitWrite()) { cleanupTempFiles(); return false; }

            IndexFileHeader header{};
            if (!output.PutData(&header, sizeof(header))) { cleanupTempFiles(); return false; }
            if (!output.PutData(mergedPathPrefixSidecar.data(), PATH_PREFIX_SIDECAR_BYTES)) { cleanupTempFiles(); return false; }
            if (!AppendFile(output, headTempPath)) { cleanupTempFiles(); return false; }
            if (!AppendFile(output, leafTempPath)) { cleanupTempFiles(); return false; }
            if (!AppendFile(output, docDataTempPath)) { cleanupTempFiles(); return false; }
            if (!AppendFile(output, indexTempPath)) { cleanupTempFiles(); return false; }

            BuildBlocksResult mphfResult;
            if (headEntryCount > UINT32_MAX) { cleanupTempFiles(); return false; }

            std::memcpy(header.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC));
            header.IFH_Version = INDEX_FORMAT_VERSION;
            header.IFH_NumDocuments = mergedDocs;
            header.IFH_NumTerms = totalTerms;
            header.IFH_AvgDocLength = avgDocLength;
            header.IFH_HeadTermEntryOffset = sizeof(IndexFileHeader) + PATH_PREFIX_SIDECAR_BYTES;
            header.IFH_HeadTermEntryCount = headEntryCount;
            header.IFH_LeafTermBlockOffset = header.IFH_HeadTermEntryOffset + header.IFH_HeadTermEntryCount * sizeof(HeadTermEntry);
            header.IFH_LeafTermBlockCount = headEntryCount;
            header.IFH_DocDataOffset = header.IFH_LeafTermBlockOffset + header.IFH_LeafTermBlockCount * sizeof(LeafTermBlock);
            header.IFH_IndexBlockOffset = header.IFH_DocDataOffset + mergedDocs * DOC_REC_SIZE;
            header.IFH_IndexBlockCount = indexBlockCount;
            header.IFH_TermMphfHeaderOffset = header.IFH_IndexBlockOffset + static_cast<uint64_t>(indexBlockCount) * sizeof(IndexBlock);
            header.IFH_TermMphfHeaderCount = (!mphfResult.BBR_TermMphfDisplacements.empty() && !mphfResult.BBR_TermMphfEntryPages.empty()) ? 1 : 0;
            header.IFH_TermMphfDisplacementOffset = header.IFH_TermMphfHeaderOffset + header.IFH_TermMphfHeaderCount * sizeof(TermMphfHeader);
            header.IFH_TermMphfDisplacementCount = mphfResult.BBR_TermMphfDisplacements.size();
            header.IFH_TermMphfEntryOffset = header.IFH_TermMphfDisplacementOffset + header.IFH_TermMphfDisplacementCount * sizeof(int32_t);
            header.IFH_TermMphfEntryPageCount = mphfResult.BBR_TermMphfEntryPages.size();

            if (header.IFH_TermMphfHeaderCount > 0
                && (!output.PutData(&mphfResult.BBR_TermMphfHeader, sizeof(TermMphfHeader))
                    || !output.PutData(mphfResult.BBR_TermMphfDisplacements.data(), header.IFH_TermMphfDisplacementCount * sizeof(int32_t))
                    || !output.PutData(mphfResult.BBR_TermMphfEntryPages.data(), header.IFH_TermMphfEntryPageCount * sizeof(IndexBlock)))) {
                cleanupTempFiles();
                return false;
            }

            if (!output.SetPosition(0) || !output.PutData(&header, sizeof(header))) {
                cleanupTempFiles();
                return false;
            }
            const uint64_t outputBytes = header.IFH_TermMphfEntryOffset + header.IFH_TermMphfEntryPageCount * sizeof(IndexBlock);
            std::cout << "  merge step4 assembled final index in "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stepStart).count()
                      << " ms bytes=" << outputBytes
                      << " head=" << headEntryCount
                      << " leaf=" << header.IFH_LeafTermBlockCount
                      << " index=" << indexBlockCount
                      << "\n";
        }

        std::remove(headTempPath.c_str());
        std::remove(leafTempPath.c_str());
        std::remove(indexTempPath.c_str());
        std::remove(docDataTempPath.c_str());
        std::remove(mphfEntryTempPath.c_str());

        ResetLoadedRuntimeState();
        std::remove(outputPath);
        if (std::rename(tempPath.c_str(), outputPath) != 0) {
            cleanupTempFiles();
            return false;
        }
        std::cout << "  merge total completed in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - mergeStart).count()
                  << " ms\n";
        return true;
    }

    void LoadIndex()
    {
        if (m_IndexPath.empty()) return;

        ClearPinnedIndexData();

        FileAccess indexFile(m_IndexPath.c_str());
        if (!indexFile.Init()
            || indexFile.GetData(&m_IndexFileHeader, static_cast<int>(sizeof(m_IndexFileHeader))) != static_cast<int>(sizeof(m_IndexFileHeader))
            || std::memcmp(m_IndexFileHeader.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC)) != 0
            || m_IndexFileHeader.IFH_Version != INDEX_FORMAT_VERSION)
        {
            std::cerr << "Failed to load index: " << m_IndexPath
                      << " (unsupported/corrupt format; rebuild with current moon.exe)\n";
            ResetLoadedRuntimeState();
            return;
        }

        if (!indexFile.SetPosition(sizeof(IndexFileHeader))
            || indexFile.GetData(m_PathPrefixSidecar.data(), PATH_PREFIX_SIDECAR_BYTES) != static_cast<int>(PATH_PREFIX_SIDECAR_BYTES)
            || !LoadPathPrefixSidecarBytes(m_PathPrefixSidecar.data(), m_PathPrefixes))
        {
            std::cerr << "Failed to read PathPrefix sidecar for: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        std::unique_ptr<HeadTermEntry[]> headTermEntries;
        if (m_IndexFileHeader.IFH_HeadTermEntryCount > 0) {
            if (m_IndexFileHeader.IFH_HeadTermEntryCount > UINT64_MAX / sizeof(HeadTermEntry))
            {
                std::cerr << "Invalid HeadTermEntry count in: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }
            const uint64_t headBytes = m_IndexFileHeader.IFH_HeadTermEntryCount * sizeof(HeadTermEntry);
            if (headBytes > static_cast<uint64_t>(std::numeric_limits<int>::max())
                || !indexFile.SetPosition(m_IndexFileHeader.IFH_HeadTermEntryOffset))
            {
                std::cerr << "Failed to read HeadTermEntry table for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            headTermEntries.reset(new HeadTermEntry[static_cast<size_t>(m_IndexFileHeader.IFH_HeadTermEntryCount)]);
            if (indexFile.GetData(headTermEntries.get(), static_cast<int>(headBytes)) != static_cast<int>(headBytes))
            {
                std::cerr << "Failed to read HeadTermEntry table for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }
        }

        if (m_IndexFileHeader.IFH_HeadTermEntryCount > UINT32_MAX
            || m_IndexFileHeader.IFH_LeafTermBlockCount > UINT32_MAX
            || m_IndexFileHeader.IFH_IndexBlockCount > UINT32_MAX
            || m_IndexFileHeader.IFH_TermMphfDisplacementCount > UINT32_MAX
            || m_IndexFileHeader.IFH_TermMphfEntryPageCount > UINT32_MAX)
        {
            std::cerr << "Index section count exceeds runtime limit in: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        ClearBlockPageDataOnly();

        const uint32_t indexBlockCount = static_cast<uint32_t>(m_IndexFileHeader.IFH_IndexBlockCount);
        const uint32_t leafTermBlockCount = static_cast<uint32_t>(m_IndexFileHeader.IFH_LeafTermBlockCount);
        const uint32_t mphfDisplacementCount = static_cast<uint32_t>(m_IndexFileHeader.IFH_TermMphfDisplacementCount);
        const uint32_t mphfEntryPageCount = static_cast<uint32_t>(m_IndexFileHeader.IFH_TermMphfEntryPageCount);
        const uint32_t indexBlockLoadCount = std::min(indexBlockCount, INDEX_BLOCK_CACHE_SLOT_COUNT);
        const uint32_t leafTermBlockLoadCount = std::min(leafTermBlockCount, LeafTermBlockCacheSlotCount());

        auto* indexBlocks = m_BlockTable.Init(BlockKind::Index, m_IndexPath.c_str(),
                  m_IndexFileHeader.IFH_IndexBlockOffset, indexBlockCount, indexBlockLoadCount);
        auto* leafTermBlocks = m_BlockTable.Init(BlockKind::LeafTerm, m_IndexPath.c_str(),
                  m_IndexFileHeader.IFH_LeafTermBlockOffset, leafTermBlockCount, leafTermBlockLoadCount);
        if ((indexBlockLoadCount && !indexBlocks) || (leafTermBlockLoadCount && !leafTermBlocks)) {
            std::cerr << "Failed to allocate pinned index/leaf runtime cache memory for: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        const uint64_t indexBlockBytes = static_cast<uint64_t>(indexBlockLoadCount) * sizeof(IndexBlock);
        if (indexBlockBytes > 0
            && (indexBlockBytes > static_cast<uint64_t>(std::numeric_limits<int>::max())
                || !indexFile.SetPosition(m_IndexFileHeader.IFH_IndexBlockOffset)
                || indexFile.GetData(indexBlocks, static_cast<int>(indexBlockBytes)) != static_cast<int>(indexBlockBytes)))
        {
            std::cerr << "Failed to read IndexBlocks for: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        const uint64_t leafTermBlockBytes = static_cast<uint64_t>(leafTermBlockLoadCount) * sizeof(LeafTermBlock);
        if (leafTermBlockBytes > 0
            && (leafTermBlockBytes > static_cast<uint64_t>(std::numeric_limits<int>::max())
                || !indexFile.SetPosition(m_IndexFileHeader.IFH_LeafTermBlockOffset)
                || indexFile.GetData(leafTermBlocks, static_cast<int>(leafTermBlockBytes)) != static_cast<int>(leafTermBlockBytes)))
        {
            std::cerr << "Failed to read LeafTermBlocks for: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        m_BlockTable.SetHeadTermEntries(std::move(headTermEntries), static_cast<uint32_t>(m_IndexFileHeader.IFH_HeadTermEntryCount));

        if (m_IndexFileHeader.IFH_TermMphfHeaderCount > 0) {
            if (m_IndexFileHeader.IFH_TermMphfHeaderCount != 1
                || mphfDisplacementCount == 0
                || mphfEntryPageCount == 0)
            {
                std::cerr << "Invalid TermMPHF header/counts in: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            TermMphfHeader mphfHeader{};
            if (!indexFile.SetPosition(m_IndexFileHeader.IFH_TermMphfHeaderOffset)
                || indexFile.GetData(&mphfHeader, static_cast<int>(sizeof(mphfHeader))) != static_cast<int>(sizeof(mphfHeader))
                || mphfHeader.TMH_Magic != TERM_MPHF_MAGIC)
            {
                std::cerr << "Failed to read TermMPHF header for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            const uint64_t displacementBytes = static_cast<uint64_t>(mphfDisplacementCount) * sizeof(int32_t);
            if (displacementBytes > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                std::cerr << "Invalid TermMPHF displacement count in: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            std::unique_ptr<int32_t[]> displacements(new int32_t[mphfDisplacementCount]);
            if (!indexFile.SetPosition(m_IndexFileHeader.IFH_TermMphfDisplacementOffset)
                || indexFile.GetData(displacements.get(), static_cast<int>(displacementBytes)) != static_cast<int>(displacementBytes))
            {
                std::cerr << "Failed to read TermMPHF displacements for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            const uint64_t entryPageBytes = static_cast<uint64_t>(mphfEntryPageCount) * sizeof(IndexBlock);
            if (entryPageBytes > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                std::cerr << "Invalid TermMPHF entry page count in: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            auto* entryPages = static_cast<uint8_t*>(PinnedMemAlloc(entryPageBytes));
            if (!entryPages) {
                std::cerr << "Failed to allocate TermMPHF entry pages for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            if (!indexFile.SetPosition(m_IndexFileHeader.IFH_TermMphfEntryOffset)
                || indexFile.GetData(entryPages, static_cast<int>(entryPageBytes)) != static_cast<int>(entryPageBytes))
            {
                PinnedMemFree(entryPages);
                std::cerr << "Failed to read TermMPHF entry pages for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            m_BlockTable.SetTermMphf(mphfHeader, std::move(displacements), mphfDisplacementCount, entryPages, mphfEntryPageCount);
        }

        ClearDocDataOnly();

        if (m_IndexFileHeader.IFH_NumDocuments > UINT64_MAX / DOC_REC_SIZE) {
            std::cerr << "Invalid DocData count in: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }
        const uint64_t docdata_bytes = m_IndexFileHeader.IFH_NumDocuments * DOC_REC_SIZE;

        if (docdata_bytes > 0) {
            m_DocData = static_cast<uint8_t*>(PinnedMemAlloc(docdata_bytes));
            if (!m_DocData) {
                std::cerr << "Failed to allocate pinned DocData memory for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }

            if (!indexFile.SetPosition(m_IndexFileHeader.IFH_DocDataOffset)
                || !ReadFileData(indexFile, m_DocData, docdata_bytes)) {
                std::cerr << "Failed to read DocData for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }
        }

        if (m_IndexFileHeader.IFH_NumDocuments > 0 && !m_DocData) {
            std::cerr << "Invalid DocData entries in: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }
        m_VectorIndex.SetDocData(m_DocData, DocDataFirstDocId(m_DocData, m_IndexFileHeader));
        m_VectorBuilt = false;

        m_Executor = IndexSearchExecutor(this);
        m_Built    = true;
        m_LoadedFromDisk = true;
        LoadDeltaIndex();
    }

    void LoadIndex(const char* path)
    {
        if (!path || !*path) return;
        m_IndexPath = path;
        LoadIndex();
    }

private:
    shared_ptr<PostingStore>     m_Store;
    shared_ptr<ConfigParameters> m_Params;
    IndexBlockTable              m_BlockTable;
    FreshDiskAnnVectorIndex      m_VectorIndex;
    SmartTokenizer               m_Tokenizer;
    IndexSearchCompiler          m_Compiler;
    IndexSearchExecutor          m_Executor;
    std::string                  m_IndexPath;
    bool                         m_Built;
    bool                         m_LoadedFromDisk;
    bool                         m_LoadDelta;
    bool                         m_VectorBuilt;
    IndexFileHeader              m_IndexFileHeader{};
    uint8_t*                     m_DocData = nullptr;
    std::array<uint8_t, PATH_PREFIX_SIDECAR_BYTES> m_PathPrefixSidecar{};
    std::vector<std::string>     m_PathPrefixes;
    std::unique_ptr<IndexContext> m_DeltaContext;

    IndexBlockTable              m_WriteBlockTable;
    FreshDiskAnnVectorIndex      m_WriteVectorIndex;
    IndexFileHeader              m_WriteIndexFileHeader{};
    uint8_t*                     m_WriteDocData = nullptr;
    std::array<uint8_t, PATH_PREFIX_SIDECAR_BYTES> m_WritePathPrefixSidecar{};
    std::vector<std::string>     m_WritePathPrefixes;
    bool                         m_WriteBuilt = false;
    uint64_t                     m_LeafTermCacheBytes = LEAF_TERM_CACHE_BYTES;
    WeakAndBuildMode             m_WeakAndBuildMode = WeakAndBuildMode::FlatPruned;
    QueryCompileModeParameters   m_QueryParameters = kWeakAndBigramParameters;
    std::mutex                   m_SearchWorkerMutex;
    std::mutex                   m_SearchQueueMutex;
    std::condition_variable      m_SearchQueueCv;
    std::deque<std::shared_ptr<SearchTask::State>> m_SearchQueue;
    std::vector<std::thread>     m_SearchWorkers;
    bool                         m_StopSearchWorkers = false;
    std::mutex                   m_VectorRuntimeMutex;
    mutable const uint8_t*        m_StreamLengthStatsDocData = nullptr;
    mutable uint64_t              m_StreamLengthStatsDocCount = 0;
    mutable float                 m_AvgTitleLength = 1.0f;
    mutable float                 m_AvgBodyLength = 1.0f;
    mutable float                 m_AvgUrlLength = 1.0f;
    mutable float                 m_AvgAnchorLength = 1.0f;
    mutable float                 m_AvgMetaLength = 1.0f;

    static constexpr uint32_t INDEX_BLOCK_CACHE_SLOT_COUNT =
        static_cast<uint32_t>(INDEX_BLOCK_CACHE_BYTES / sizeof(IndexBlock));
    static constexpr uint32_t LEAF_TERM_BLOCK_CACHE_SLOT_COUNT =
        static_cast<uint32_t>(LEAF_TERM_CACHE_BYTES / sizeof(LeafTermBlock));

    void EnsureSearchWorkersStarted(uint32_t workerCount = 4)
    {
        std::lock_guard<std::mutex> lock(m_SearchWorkerMutex);
        if (!m_SearchWorkers.empty())
            return;

        SetDirectBlockAccessEnabled(false);
        EnsureStreamLengthStats();
        m_StopSearchWorkers = false;
        workerCount = std::max<uint32_t>(1, workerCount);
        m_SearchWorkers.reserve(workerCount);
        for (uint32_t i = 0; i < workerCount; ++i)
            m_SearchWorkers.emplace_back([this] { SearchWorkerLoop(); });
    }

    void StopSearchWorkers()
    {
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(m_SearchWorkerMutex);
            if (m_SearchWorkers.empty())
                return;
            {
                std::lock_guard<std::mutex> queueLock(m_SearchQueueMutex);
                m_StopSearchWorkers = true;
            }
            m_SearchQueueCv.notify_all();
            workers.swap(m_SearchWorkers);
        }

        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }

    void SearchWorkerLoop()
    {
        SmartTokenizer tokenizer;
        IndexSearchCompiler compiler(&tokenizer);
        IndexSearchExecutor executor(this);

        while (true) {
            std::shared_ptr<SearchTask::State> task;
            {
                std::unique_lock<std::mutex> lock(m_SearchQueueMutex);
                m_SearchQueueCv.wait(lock, [this] {
                    return m_StopSearchWorkers || !m_SearchQueue.empty();
                });
                if (m_StopSearchWorkers && m_SearchQueue.empty())
                    return;
                task = std::move(m_SearchQueue.front());
                m_SearchQueue.pop_front();
            }

            try {
                task->Promise.set_value(ExecuteSearchTask(*task, compiler, executor));
            } catch (...) {
                task->Promise.set_exception(std::current_exception());
            }
        }
    }

    std::vector<SearchResult> ExecuteSearchTask(SearchTask::State& task,
                                                IndexSearchCompiler& compiler,
                                                IndexSearchExecutor& executor)
    {
        std::unique_ptr<EvalTree> tree;
        if (task.Query.empty() && !task.Vector.empty()) {
            tree = std::make_unique<EvalTree>();
        } else {
            tree.reset(compiler.Compile(task.Query.c_str(), task.Streams.c_str(), nullptr, task.Mode));
        }

        if (!tree)
            return {};
        if (!task.Vector.empty())
            tree->vector_query = task.Vector;
        tree->vector_ef_search = task.VectorEfSearch;
        if (tree->IsEmpty())
            return {};

        std::shared_ptr<IndexReader> reader;
        if (!tree->HasTextQuery() && tree->HasVectorQuery()) {
            std::lock_guard<std::mutex> lock(m_VectorRuntimeMutex);
            reader = GetReader(tree.get());
        } else {
            reader = GetReader(tree.get());
        }

        const std::vector<float>* vectorQuery = tree->HasTextQuery() && tree->HasVectorQuery()
            ? &tree->vector_query
            : nullptr;
        return executor.Execute(reader, task.TopK, vectorQuery);
    }

    static uint64_t DocDataFirstDocId(const uint8_t* docData, const IndexFileHeader& header)
    {
        return docData && header.IFH_NumDocuments > 0
            ? reinterpret_cast<const DocDataEntry*>(docData)->DDE_DocID
            : 0;
    }

    struct PathPrefixBuildState {
        std::unordered_map<std::string, uint16_t> PrefixToId;
        std::vector<std::string> Prefixes;
        uint32_t StringBytes = 0;
    };

    static void SplitPathForSidecar(const std::string& fullPath, std::string& prefix, std::string& filename)
    {
        const size_t slash = fullPath.find_last_of("/\\");
        if (slash == std::string::npos) {
            prefix.clear();
            filename = fullPath;
            return;
        }
        prefix = fullPath.substr(0, slash);
        filename = fullPath.substr(slash + 1);
    }

    static bool PathPrefixSidecarCanHold(uint32_t prefixCount, uint32_t stringBytes)
    {
        const uint64_t bytes = sizeof(PathPrefixSidecarHeader)
            + static_cast<uint64_t>(prefixCount) * sizeof(PathPrefixSidecarEntry)
            + stringBytes;
        return bytes <= PATH_PREFIX_SIDECAR_BYTES;
    }

    static uint16_t InternPathPrefix(PathPrefixBuildState& state, const std::string& prefix)
    {
        auto it = state.PrefixToId.find(prefix);
        if (it != state.PrefixToId.end())
            return it->second;
        if (state.Prefixes.size() >= DOC_PATH_PREFIX_INVALID)
            return DOC_PATH_PREFIX_INVALID;
        const uint32_t nextCount = static_cast<uint32_t>(state.Prefixes.size() + 1);
        const uint32_t nextStringBytes = state.StringBytes + static_cast<uint32_t>(prefix.size());
        if (!PathPrefixSidecarCanHold(nextCount, nextStringBytes))
            return DOC_PATH_PREFIX_INVALID;
        const uint16_t id = static_cast<uint16_t>(state.Prefixes.size());
        state.Prefixes.push_back(prefix);
        state.PrefixToId.emplace(prefix, id);
        state.StringBytes = nextStringBytes;
        return id;
    }

    static void EncodeDocPath(DocDataEntry& entry, const std::string& fullPath, PathPrefixBuildState& state)
    {
        entry.DDE_PathLength = 0;
        std::memset(entry.DDE_Path, 0, sizeof(entry.DDE_Path));
        if (fullPath.empty())
            return;

        std::string prefix;
        std::string filename;
        SplitPathForSidecar(fullPath, prefix, filename);
        const uint16_t prefixId = InternPathPrefix(state, prefix);
        const uint16_t filenameBytes = static_cast<uint16_t>(std::min(filename.size(), DOC_PATH_FILENAME_MAX));
        std::memcpy(entry.DDE_Path, &prefixId, sizeof(prefixId));
        if (filenameBytes > 0)
            std::memcpy(entry.DDE_Path + DOC_PATH_PREFIX_ID_BYTES, filename.data(), filenameBytes);
        entry.DDE_PathLength = static_cast<uint16_t>(DOC_PATH_PREFIX_ID_BYTES + filenameBytes);
    }

    static void BuildPathPrefixSidecarBytes(const std::vector<std::string>& prefixes,
                                            std::array<uint8_t, PATH_PREFIX_SIDECAR_BYTES>& sidecar)
    {
        sidecar = {};
        PathPrefixSidecarHeader header{};
        std::memcpy(header.PPSH_Magic, PATH_PREFIX_SIDECAR_MAGIC, sizeof(PATH_PREFIX_SIDECAR_MAGIC));
        header.PPSH_Version = PATH_PREFIX_SIDECAR_VERSION;
        header.PPSH_PrefixCount = static_cast<uint16_t>(std::min<size_t>(prefixes.size(), DOC_PATH_PREFIX_INVALID));
        header.PPSH_EntryOffset = sizeof(PathPrefixSidecarHeader);
        header.PPSH_StringOffset = header.PPSH_EntryOffset + header.PPSH_PrefixCount * sizeof(PathPrefixSidecarEntry);
        uint32_t cursor = 0;
        for (uint16_t i = 0; i < header.PPSH_PrefixCount; ++i)
            cursor += static_cast<uint32_t>(prefixes[i].size());
        header.PPSH_StringBytes = cursor;
        std::memcpy(sidecar.data(), &header, sizeof(header));

        cursor = 0;
        for (uint16_t i = 0; i < header.PPSH_PrefixCount; ++i) {
            PathPrefixSidecarEntry entry{};
            entry.PPSE_Offset = cursor;
            entry.PPSE_Length = static_cast<uint16_t>(prefixes[i].size());
            const size_t entryOffset = header.PPSH_EntryOffset + static_cast<size_t>(i) * sizeof(PathPrefixSidecarEntry);
            std::memcpy(sidecar.data() + entryOffset, &entry, sizeof(entry));
            if (entry.PPSE_Length > 0) {
                std::memcpy(sidecar.data() + header.PPSH_StringOffset + cursor, prefixes[i].data(), entry.PPSE_Length);
                cursor += entry.PPSE_Length;
            }
        }
    }

    static bool LoadPathPrefixSidecarBytes(const uint8_t* sidecar, std::vector<std::string>& prefixes)
    {
        prefixes.clear();
        if (!sidecar)
            return false;
        PathPrefixSidecarHeader header{};
        std::memcpy(&header, sidecar, sizeof(header));
        if (std::memcmp(header.PPSH_Magic, PATH_PREFIX_SIDECAR_MAGIC, sizeof(PATH_PREFIX_SIDECAR_MAGIC)) != 0
            || header.PPSH_Version != PATH_PREFIX_SIDECAR_VERSION
            || header.PPSH_EntryOffset < sizeof(PathPrefixSidecarHeader)
            || header.PPSH_StringOffset > PATH_PREFIX_SIDECAR_BYTES
            || header.PPSH_StringBytes > PATH_PREFIX_SIDECAR_BYTES
            || header.PPSH_StringOffset + header.PPSH_StringBytes > PATH_PREFIX_SIDECAR_BYTES)
            return false;
        const uint64_t entryBytes = static_cast<uint64_t>(header.PPSH_PrefixCount) * sizeof(PathPrefixSidecarEntry);
        if (header.PPSH_EntryOffset + entryBytes > PATH_PREFIX_SIDECAR_BYTES)
            return false;

        prefixes.reserve(header.PPSH_PrefixCount);
        for (uint16_t i = 0; i < header.PPSH_PrefixCount; ++i) {
            PathPrefixSidecarEntry entry{};
            const size_t entryOffset = header.PPSH_EntryOffset + static_cast<size_t>(i) * sizeof(PathPrefixSidecarEntry);
            std::memcpy(&entry, sidecar + entryOffset, sizeof(entry));
            if (entry.PPSE_Offset + entry.PPSE_Length > header.PPSH_StringBytes)
                return false;
            const char* text = reinterpret_cast<const char*>(sidecar + header.PPSH_StringOffset + entry.PPSE_Offset);
            prefixes.emplace_back(text, entry.PPSE_Length);
        }
        return true;
    }

    static std::string DecodeDocPath(const DocDataEntry& entry, const std::vector<std::string>& prefixes)
    {
        if (entry.DDE_PathLength == 0 || entry.DDE_PathLength > DOC_PATH_MAX)
            return {};
        if (entry.DDE_PathLength < DOC_PATH_PREFIX_ID_BYTES)
            return std::string(reinterpret_cast<const char*>(entry.DDE_Path), entry.DDE_PathLength);

        uint16_t prefixId = DOC_PATH_PREFIX_INVALID;
        std::memcpy(&prefixId, entry.DDE_Path, sizeof(prefixId));
        const char* filenameData = reinterpret_cast<const char*>(entry.DDE_Path + DOC_PATH_PREFIX_ID_BYTES);
        const size_t filenameLen = entry.DDE_PathLength - DOC_PATH_PREFIX_ID_BYTES;
        const std::string filename(filenameData, filenameLen);
        if (prefixId == DOC_PATH_PREFIX_INVALID || prefixId >= prefixes.size())
            return filename;
        if (prefixes[prefixId].empty())
            return filename;
        const char last = prefixes[prefixId].back();
        if (last == '/' || last == '\\')
            return prefixes[prefixId] + filename;
        const char sep = prefixes[prefixId].find('\\') != std::string::npos ? '\\' : '/';
        return prefixes[prefixId] + sep + filename;
    }

    uint32_t LeafTermBlockCacheSlotCount() const
    {
        const uint64_t slots = m_LeafTermCacheBytes / sizeof(LeafTermBlock);
        return static_cast<uint32_t>(std::min<uint64_t>(slots, UINT32_MAX));
    }

    void ClearDocDataOnly(uint8_t*& docData, FreshDiskAnnVectorIndex& vectorIndex)
    {
        if (docData)
            PinnedMemFree(docData);
        docData = nullptr;
        vectorIndex.Clear();
    }

    void ClearDocDataOnly()
    {
        ClearDocDataOnly(m_DocData, m_VectorIndex);
    }

    void ClearPinnedIndexData()
    {
        ClearDocDataOnly();
        ClearBlockPageDataOnly();
        m_IndexFileHeader = {};
        m_PathPrefixSidecar = {};
        m_PathPrefixes.clear();
        m_VectorBuilt = false;
    }

    void ClearWritePinnedIndexData()
    {
        ClearDocDataOnly(m_WriteDocData, m_WriteVectorIndex);
        m_WriteBlockTable.SetBlockMemory(nullptr, nullptr);
        m_WriteIndexFileHeader = {};
        m_WritePathPrefixSidecar = {};
        m_WritePathPrefixes.clear();
        m_WriteBuilt = false;
    }

    void ClearBlockPageDataOnly()
    {
    }

    void ResetLoadedRuntimeState()
    {
        m_DeltaContext.reset();
        ClearPinnedIndexData();
        m_BlockTable.SetBlockMemory(nullptr, nullptr);
        m_Executor = IndexSearchExecutor(this);
        m_Built = false;
        m_LoadedFromDisk = false;
        m_VectorBuilt = false;
    }

    void BuildVectorRuntime()
    {
        if (m_VectorBuilt) return;
        const bool showProgress = m_IndexFileHeader.IFH_NumDocuments >= 100000;
        const auto start = std::chrono::steady_clock::now();
        if (showProgress)
            std::cout << "\n  vector runtime: scanning " << m_IndexFileHeader.IFH_NumDocuments << " DocData records\n";
        m_VectorIndex.Clear();
        const uint64_t firstDocId = DocDataFirstDocId(m_DocData, m_IndexFileHeader);
        m_VectorIndex.SetDocData(m_DocData, firstDocId);
        if (m_DocData) {
            uint64_t vectorDocCount = 0;
            for (uint64_t slot = 0; slot < m_IndexFileHeader.IFH_NumDocuments; ++slot) {
                const uint64_t docId = firstDocId + slot;
                const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + slot * DOC_REC_SIZE);
                if (entry->DDE_DocID == docId && entry->DDE_VectorDim == DOC_VECTOR_DIM && entry->DDE_VectorFormat != 0)
                    ++vectorDocCount;
            }
            if (vectorDocCount < std::numeric_limits<uint32_t>::max())
                m_VectorIndex.Reserve(static_cast<size_t>(vectorDocCount));

            for (uint64_t slot = 0; slot < m_IndexFileHeader.IFH_NumDocuments; ++slot) {
                const uint64_t docId = firstDocId + slot;
                const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + slot * DOC_REC_SIZE);
                if (entry->DDE_DocID == docId && entry->DDE_VectorDim == DOC_VECTOR_DIM && entry->DDE_VectorFormat != 0)
                    m_VectorIndex.Add(docId);
                if (showProgress && docId > 0 && docId % 100000 == 0)
                    std::cout << "  vector runtime: added through doc " << docId << "\n";
            }
        }
        m_VectorBuilt = true;
        if (showProgress) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << "  vector runtime: built " << m_VectorIndex.Size() << " vectors in " << elapsedMs << " ms\n";
        }
    }

    void EnsureStreamLengthStats() const
    {
        if (m_StreamLengthStatsDocData == m_DocData
            && m_StreamLengthStatsDocCount == m_IndexFileHeader.IFH_NumDocuments)
            return;

        m_StreamLengthStatsDocData = m_DocData;
        m_StreamLengthStatsDocCount = m_IndexFileHeader.IFH_NumDocuments;
        m_AvgTitleLength = 1.0f;
        m_AvgBodyLength = std::max(1.0f, m_IndexFileHeader.IFH_AvgDocLength);
        m_AvgUrlLength = std::max(1.0f, m_IndexFileHeader.IFH_AvgDocLength);
        m_AvgAnchorLength = std::max(1.0f, m_IndexFileHeader.IFH_AvgDocLength);
        m_AvgMetaLength = std::max(1.0f, m_IndexFileHeader.IFH_AvgDocLength);
        if (!m_DocData || m_IndexFileHeader.IFH_NumDocuments == 0)
            return;

        double titleTotal = 0.0;
        double bodyTotal = 0.0;
        double urlTotal = 0.0;
        double anchorTotal = 0.0;
        double metaTotal = 0.0;
        uint64_t titleDocs = 0;
        uint64_t bodyDocs = 0;
        uint64_t urlDocs = 0;
        uint64_t anchorDocs = 0;
        uint64_t metaDocs = 0;
        const uint64_t firstDocId = DocDataFirstDocId(m_DocData, m_IndexFileHeader);
        for (uint64_t slot = 0; slot < m_IndexFileHeader.IFH_NumDocuments; ++slot) {
            const uint64_t docId = firstDocId + slot;
            const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + slot * DOC_REC_SIZE);
            if (entry->DDE_DocID != docId)
                continue;
            auto addLength = [](float value, double& total, uint64_t& count) {
                if (value > 0.0f) {
                    total += static_cast<double>(value);
                    ++count;
                }
            };
            addLength(static_cast<float>(entry->DDE_TitleLength), titleTotal, titleDocs);
            addLength(static_cast<float>(entry->DDE_BodyLength), bodyTotal, bodyDocs);
            addLength(static_cast<float>(entry->DDE_UrlLength), urlTotal, urlDocs);
            addLength(static_cast<float>(entry->DDE_AnchorLength), anchorTotal, anchorDocs);
            addLength(static_cast<float>(entry->DDE_MetaLength), metaTotal, metaDocs);
        }

        if (titleDocs > 0) m_AvgTitleLength = static_cast<float>(titleTotal / static_cast<double>(titleDocs));
        if (bodyDocs > 0) m_AvgBodyLength = static_cast<float>(bodyTotal / static_cast<double>(bodyDocs));
        if (urlDocs > 0) m_AvgUrlLength = static_cast<float>(urlTotal / static_cast<double>(urlDocs));
        if (anchorDocs > 0) m_AvgAnchorLength = static_cast<float>(anchorTotal / static_cast<double>(anchorDocs));
        if (metaDocs > 0) m_AvgMetaLength = static_cast<float>(metaTotal / static_cast<double>(metaDocs));
    }

    static std::string DeltaIndexPath(const std::string& path)
    {
        const size_t slash = path.find_last_of("/\\");
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            return path.substr(0, dot) + ".delta" + path.substr(dot);
        return path + ".delta.idx";
    }

    void LoadDeltaIndex()
    {
        m_DeltaContext.reset();

        if (!m_LoadDelta || m_IndexPath.empty()) return;

        const std::string deltaPath = DeltaIndexPath(m_IndexPath);
        if (deltaPath == m_IndexPath) return;
        if (!IndexSerializer::IsValidIndex(deltaPath.c_str())) return;

        auto delta = std::make_unique<IndexContext>("", deltaPath.c_str(), false);
        if (delta->m_LoadedFromDisk && delta->DocumentCount() > 0)
            m_DeltaContext = std::move(delta);
    }

    void ResetRuntimeState(IndexBlockTable& blockTable,
                           FreshDiskAnnVectorIndex& vectorIndex,
                           IndexFileHeader& header,
                           uint8_t*& docData,
                           std::array<uint8_t, PATH_PREFIX_SIDECAR_BYTES>& pathPrefixSidecar,
                           std::vector<std::string>& pathPrefixes,
                           bool& built)
    {
        ClearDocDataOnly(docData, vectorIndex);
        blockTable.SetBlockMemory(nullptr, nullptr);
        header = {};
        pathPrefixSidecar = {};
        pathPrefixes.clear();
        built = false;
    }

    bool BuildRuntime(IndexBlockTable& blockTable,
                      FreshDiskAnnVectorIndex& vectorIndex,
                      IndexFileHeader& header,
                      uint8_t*& docData,
                      std::array<uint8_t, PATH_PREFIX_SIDECAR_BYTES>& pathPrefixSidecar,
                      std::vector<std::string>& pathPrefixes,
                      bool& built,
                      bool buildVectorIndex = true)
    {
        if (built) return true;

        if (!m_Store || m_Store->AllDocStats().empty()) {
            built = true;
            return true;
        }

        uint64_t minDocId = UINT64_MAX;
        uint64_t maxDocId = 0;
        for (const auto& [docId, _] : m_Store->AllDocStats()) {
            minDocId = std::min(minDocId, docId);
            maxDocId = std::max(maxDocId, docId);
        }
        const uint64_t docDataFirstDocId = minDocId == UINT64_MAX ? 0 : minDocId;
        const uint64_t docDataRecordCount = maxDocId >= docDataFirstDocId ? (maxDocId - docDataFirstDocId + 1) : 0;

        ClearDocDataOnly(docData, vectorIndex);
        docData = static_cast<uint8_t*>(PinnedMemAlloc(docDataRecordCount * DOC_REC_SIZE));
        if (!docData) {
            ResetRuntimeState(blockTable, vectorIndex, header, docData, pathPrefixSidecar, pathPrefixes, built);
            return false;
        }
        std::memset(docData, 0, static_cast<size_t>(docDataRecordCount * DOC_REC_SIZE));
        for (uint64_t slot = 0; slot < docDataRecordCount; ++slot)
            reinterpret_cast<DocDataEntry*>(docData + slot * DOC_REC_SIZE)->DDE_DocID = UINT32_MAX;

        uint64_t totalLen = 0;
        PathPrefixBuildState pathState;
        for (const auto& [docId, stats] : m_Store->AllDocStats()) {
            auto* entry = reinterpret_cast<DocDataEntry*>(docData + (docId - docDataFirstDocId) * DOC_REC_SIZE);
            assert(docId <= UINT32_MAX);
            entry->DDE_DocID = static_cast<uint32_t>(docId);
            entry->DDE_StaticRank = DocDataEncodeScore(stats.importance);
            const float docLength = static_cast<float>(std::max<uint32_t>(1, stats.doc_len));
            const float diversity = std::min(1.0f, static_cast<float>(stats.unique_terms) / docLength);
            const float logLength = std::log2(docLength);
            const float lengthQuality = std::max(0.0f, 1.0f - std::abs(logLength - 6.0f) / 4.0f);
            entry->DDE_QualityScore = DocDataEncodeScore(0.6f * lengthQuality + 0.4f * diversity);
            entry->DDE_AuthorityScore = DocDataEncodeScore(stats.title_len > 0
                ? std::min(1.0f, std::log2(1.0f + static_cast<float>(stats.title_len)) / 5.0f)
                : 0.0f);
            const float repetitionPenalty = std::max(0.0f, 0.35f - diversity) / 0.35f;
            const float overlongPenalty = std::max(0.0f, (logLength - 10.0f) / 4.0f);
            entry->DDE_SpamScore = DocDataEncodeScore(std::min(1.0f, repetitionPenalty + overlongPenalty));
            entry->DDE_TitleLength = stats.title_len;
            entry->DDE_BodyLength = stats.body_len;
            entry->DDE_UrlLength = stats.url_len;
            entry->DDE_AnchorLength = stats.anchor_len;
            entry->DDE_MetaLength = stats.meta_len;
            entry->DDE_DiversityScore = diversity;
            entry->DDE_LengthQualityScore = lengthQuality;
            if (m_Store->HasDocVector(docId)) {
                entry->DDE_VectorDim = static_cast<uint16_t>(DOC_VECTOR_DIM);
                entry->DDE_VectorFormat = 1;
                std::memcpy(entry->DDE_VectorData, m_Store->GetDocVector(docId), DOC_VECTOR_STORAGE_MAX_DIM);
            }
            EncodeDocPath(*entry, stats.path, pathState);
            totalLen += stats.doc_len;
        }
        pathPrefixes = pathState.Prefixes;
        BuildPathPrefixSidecarBytes(pathPrefixes, pathPrefixSidecar);

        const uint64_t totalDocs = docDataRecordCount;
        std::memcpy(header.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC));
        header.IFH_Version = INDEX_FORMAT_VERSION;
        header.IFH_NumDocuments = totalDocs;
        header.IFH_AvgDocLength = totalDocs ? static_cast<float>(totalLen) / static_cast<float>(totalDocs) : 1.0f;

        auto br = IndexSerializer::BuildBlocks(*m_Store);
        if (br.BBR_IndexBlocks.size() > UINT32_MAX
            || br.BBR_LeafTermBlocks.size() > UINT32_MAX
            || br.BBR_HeadTermEntries.size() > UINT32_MAX
            || br.BBR_TermMphfDisplacements.size() > UINT32_MAX
            || br.BBR_TermMphfEntryPages.size() > UINT32_MAX) {
            std::cerr << "Built index section count exceeds runtime limit\n";
            ResetRuntimeState(blockTable, vectorIndex, header, docData, pathPrefixSidecar, pathPrefixes, built);
            return false;
        }

        const uint32_t indexBlockCount = static_cast<uint32_t>(br.BBR_IndexBlocks.size());
        const uint32_t leafTermBlockCount = static_cast<uint32_t>(br.BBR_LeafTermBlocks.size());
        const uint32_t headCount = static_cast<uint32_t>(br.BBR_HeadTermEntries.size());
        const uint32_t mphfDisplacementCount = static_cast<uint32_t>(br.BBR_TermMphfDisplacements.size());
        const uint32_t mphfEntryPageCount = static_cast<uint32_t>(br.BBR_TermMphfEntryPages.size());

        header.IFH_NumTerms = br.BBR_TotalTerms;
        header.IFH_HeadTermEntryOffset = sizeof(IndexFileHeader) + PATH_PREFIX_SIDECAR_BYTES;
        header.IFH_HeadTermEntryCount = headCount;
        header.IFH_LeafTermBlockOffset = header.IFH_HeadTermEntryOffset + static_cast<uint64_t>(headCount) * sizeof(HeadTermEntry);
        header.IFH_LeafTermBlockCount = leafTermBlockCount;
        header.IFH_DocDataOffset = header.IFH_LeafTermBlockOffset + static_cast<uint64_t>(leafTermBlockCount) * sizeof(LeafTermBlock);
        header.IFH_IndexBlockOffset = header.IFH_DocDataOffset + docDataRecordCount * DOC_REC_SIZE;
        header.IFH_IndexBlockCount = indexBlockCount;
        header.IFH_TermMphfHeaderOffset = header.IFH_IndexBlockOffset + static_cast<uint64_t>(indexBlockCount) * sizeof(IndexBlock);
        header.IFH_TermMphfHeaderCount = mphfDisplacementCount > 0 && mphfEntryPageCount > 0 ? 1 : 0;
        header.IFH_TermMphfDisplacementOffset = header.IFH_TermMphfHeaderOffset + header.IFH_TermMphfHeaderCount * sizeof(TermMphfHeader);
        header.IFH_TermMphfDisplacementCount = mphfDisplacementCount;
        header.IFH_TermMphfEntryOffset = header.IFH_TermMphfDisplacementOffset + static_cast<uint64_t>(mphfDisplacementCount) * sizeof(int32_t);
        header.IFH_TermMphfEntryPageCount = mphfEntryPageCount;

        auto* indexBlocks = blockTable.Init(BlockKind::Index, nullptr, 0, indexBlockCount, indexBlockCount);
        auto* leafTermBlocks = blockTable.Init(BlockKind::LeafTerm, nullptr, 0, leafTermBlockCount, leafTermBlockCount);
        if ((indexBlockCount > 0 && !indexBlocks) || (leafTermBlockCount > 0 && !leafTermBlocks)) {
            ResetRuntimeState(blockTable, vectorIndex, header, docData, pathPrefixSidecar, pathPrefixes, built);
            return false;
        }

        if (indexBlockCount > 0)
            std::memcpy(indexBlocks, br.BBR_IndexBlocks.data(), static_cast<size_t>(indexBlockCount) * sizeof(IndexBlock));
        if (leafTermBlockCount > 0)
            std::memcpy(leafTermBlocks, br.BBR_LeafTermBlocks.data(), static_cast<size_t>(leafTermBlockCount) * sizeof(LeafTermBlock));

        std::unique_ptr<HeadTermEntry[]> headTermEntries;
        if (headCount > 0) {
            headTermEntries.reset(new HeadTermEntry[headCount]);
            std::memcpy(headTermEntries.get(), br.BBR_HeadTermEntries.data(), static_cast<size_t>(headCount) * sizeof(HeadTermEntry));
        }

        blockTable.SetHeadTermEntries(std::move(headTermEntries), headCount);

        if (header.IFH_TermMphfHeaderCount > 0) {
            std::unique_ptr<int32_t[]> displacements(new int32_t[mphfDisplacementCount]);
            std::memcpy(displacements.get(), br.BBR_TermMphfDisplacements.data(), static_cast<size_t>(mphfDisplacementCount) * sizeof(int32_t));
            auto* entryPages = static_cast<uint8_t*>(PinnedMemAlloc(static_cast<uint64_t>(mphfEntryPageCount) * sizeof(IndexBlock)));
            if (!entryPages) {
                ResetRuntimeState(blockTable, vectorIndex, header, docData, pathPrefixSidecar, pathPrefixes, built);
                return false;
            }
            std::memcpy(entryPages, br.BBR_TermMphfEntryPages.data(), static_cast<size_t>(mphfEntryPageCount) * sizeof(IndexBlock));
            blockTable.SetTermMphf(br.BBR_TermMphfHeader, std::move(displacements), mphfDisplacementCount, entryPages, mphfEntryPageCount);
        }

        if (buildVectorIndex) {
            vectorIndex.SetDocData(docData, docDataFirstDocId);
            for (const auto& [docId, _] : m_Store->AllDocStats()) {
                if (m_Store->HasDocVector(docId))
                    vectorIndex.Add(docId);
            }
        }

        built = true;
        return true;
    }

    class LeafTermBlockView {
    public:
        explicit LeafTermBlockView(IndexBlockTable& blockTable, bool readCurrent = true)
            : m_BlockTable(&blockTable)
            , m_LeafTermBlockCount(blockTable.m_LeafTermPool.BCP_TotalBlockCount)
            , m_IndexBlockCount(blockTable.m_IndexPool.BCP_SlotCount)
        {
            m_IndexBuffer = static_cast<uint8_t*>(PinnedMemAlloc(POSTING_SCRATCH_BYTES));
            if (readCurrent)
                GoCurrent();
        }

        ~LeafTermBlockView()
        {
            if (m_IndexBuffer)
                PinnedMemFree(m_IndexBuffer);
            ReleaseCurrentLeaf();
        }

        const LeafTermEntry* Current() const { return m_CurrentEntry; }

        void Advance()
        {
            if (!m_CurrentEntry) return;
            ++m_EntryIndex;
            GoCurrent();
        }

        LeafTermBlockView& operator++()
        {
            Advance();
            return *this;
        }

        bool CanAddLeafTermEntry(const LeafTermBlockView& cursor) const
        {
            const auto* source = cursor.Current();
            if (source->LTE_TermLength > HEAD_TERM_KEY_MAX) return true;

            const size_t entryBytes = sizeof(LeafTermEntry) + source->LTE_TermLength;
            if (entryBytes > sizeof(LeafTermBlock::LTB_Data)) return false;

            uint32_t leafBlockID = m_LeafBlockID;
            if (m_LeafEntryCount > 0
                && (m_LeafEntryCount >= LEAF_TERM_DIRECTORY_COUNT - 1 || m_LeafWriteOffset + entryBytes > sizeof(LeafTermBlock::LTB_Data))) {
                ++leafBlockID;
            }
            return leafBlockID < m_LeafTermBlockCount;
        }

        bool AddLeafTermEntry(const LeafTermBlockView& cursor)
        {
            const auto* source = cursor.Current();
            if (source->LTE_TermLength > HEAD_TERM_KEY_MAX) return true;

            const uint8_t termLength = source->LTE_TermLength;
            const size_t entryBytes = sizeof(LeafTermEntry) + termLength;

            if (m_LeafEntryCount > 0
                && (m_LeafEntryCount >= LEAF_TERM_DIRECTORY_COUNT - 1 || m_LeafWriteOffset + entryBytes > sizeof(LeafTermBlock::LTB_Data))) {
                auto* currentLeaf = reinterpret_cast<LeafTermBlock*>(m_BlockTable->m_LeafTermPool.BCP_Pages + m_LeafBlockID * sizeof(LeafTermBlock));
                currentLeaf->LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] = static_cast<uint16_t>(m_LeafEntryCount);
                ++m_LeafBlockID;
                m_LeafWriteOffset = 0;
                m_LeafEntryCount = 0;
            }

            if (m_LeafBlockID >= m_LeafTermBlockCount || entryBytes > sizeof(LeafTermBlock::LTB_Data))
                return false;

            auto* leaf = reinterpret_cast<LeafTermBlock*>(m_BlockTable->m_LeafTermPool.BCP_Pages + m_LeafBlockID * sizeof(LeafTermBlock));
            leaf->LTB_Directory[m_LeafEntryCount] = static_cast<uint16_t>(LEAF_TERM_DATA_OFFSET + m_LeafWriteOffset);
            auto* entry = reinterpret_cast<LeafTermEntry*>(leaf->LTB_Data + m_LeafWriteOffset);
            std::memcpy(entry, source, entryBytes);
            entry->LTE_DocFreq = m_DocFreq;
            entry->LTE_IndexBlockID = m_IndexBlockBase + m_PostingIndexBlockID;
            assert(m_PostingIndexOffset <= UINT16_MAX);
            assert(m_PostingIndexLength <= UINT16_MAX);
            assert(m_PostingContinuationBlockCount <= UINT16_MAX);
            entry->LTE_IndexOffset = static_cast<uint16_t>(m_PostingIndexOffset);
            entry->LTE_IndexLength = static_cast<uint16_t>(m_PostingIndexLength);
            entry->LTE_ContinuationBlockCount = static_cast<uint16_t>(m_PostingContinuationBlockCount);
            m_LeafWriteOffset += entryBytes;
            ++m_LeafEntryCount;
            leaf->LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] = static_cast<uint16_t>(m_LeafEntryCount);
            m_PostingIndexLength = 0;
            m_PostingContinuationBlockCount = 0;
            return true;
        }

        uint32_t UsedLeafBlockCount() const
        {
            return m_LeafEntryCount > 0 ? m_LeafBlockID + 1 : m_LeafBlockID;
        }

        uint32_t UsedIndexBlockCount() const
        {
            return m_HasIndexWrite ? m_IndexBlockID + 1 : 0;
        }

        void ResetWrite(uint32_t indexBlockBase)
        {
            m_PostingIndexBlockID = 0;
            m_PostingIndexOffset = 0;
            m_PostingIndexLength = 0;
            m_PostingContinuationBlockCount = 0;
            m_DocFreq = 0;
            m_LeafBlockID = 0;
            m_LeafWriteOffset = 0;
            m_LeafEntryCount = 0;
            m_IndexBlockID = 0;
            m_IndexOffsetInBlock = 0;
            m_IndexBlockBase = indexBlockBase;
            m_HasIndexWrite = false;
        }

        /*
        * Writes locate the target buffer the same way reads do: m_IndexBlockID
        * selects the IndexBlock page, and m_IndexOffsetInBlock is the byte
        * offset inside that 4096-byte block.
        */
        bool AddIndex(const LeafTermBlockView& cursor)
        {
            /*
            * Read the logical posting stream into reusable pinned scratch before
            * any output metadata is committed. Scratch bytes are overwritten on
            * each call and are disposable on failure.
            */
            const size_t bytesNeeded = cursor.PostingBytesSize();
            if (!m_IndexBuffer || bytesNeeded > POSTING_SCRATCH_BYTES) return false;

            size_t bytesWritten = 0;
            cursor.ReadPostingBytesTo(m_IndexBuffer, bytesWritten);
            if (!AddIndex(bytesWritten)) return false;

            m_DocFreq = cursor.Current()->LTE_DocFreq;
            return true;
        }

        bool AddIndex(const LeafTermBlockView& smallCursor, const LeafTermBlockView& bigCursor)
        {
            const size_t smallBytes = smallCursor.PostingBytesSize();
            const size_t bigBytes = bigCursor.PostingBytesSize();
            if (!m_IndexBuffer || smallBytes > POSTING_SCRATCH_BYTES || bigBytes > POSTING_SCRATCH_BYTES - smallBytes) return false;

            /*
            * Build the merged logical posting stream in source order. The two
            * cursors already point at the same term; this function only shapes
            * their raw bytes into destination index blocks.
            */
            size_t mergedBytesWritten = 0;
            /*
            * ReadPostingBytesTo appends at mergedBytesWritten and advances it,
            * so the big postings are placed immediately after the small postings.
            */
            smallCursor.ReadPostingBytesTo(m_IndexBuffer, mergedBytesWritten);
            bigCursor.ReadPostingBytesTo(m_IndexBuffer, mergedBytesWritten);
            if (!AddIndex(mergedBytesWritten)) return false;

            m_DocFreq = smallCursor.Current()->LTE_DocFreq + bigCursor.Current()->LTE_DocFreq;
            return true;
        }

        bool operator<(const LeafTermBlockView& other) const { return CurrentTerm() < other.CurrentTerm(); }
        bool operator>(const LeafTermBlockView& other) const { return other < *this; }
        bool operator==(const LeafTermBlockView& other) const { return CurrentTerm() == other.CurrentTerm(); }

    private:
        static constexpr size_t POSTING_SCRATCH_PAGE_COUNT = 100;
        static constexpr size_t POSTING_SCRATCH_BYTES = POSTING_SCRATCH_PAGE_COUNT * PAGE_SIZE;

        IndexBlockTable* m_BlockTable = nullptr;
        uint64_t m_LeafTermBlockCount = 0;
        uint64_t m_IndexBlockCount = 0;
        const LeafTermBlock* m_CurrentLeaf = nullptr;
        const LeafTermEntry* m_CurrentEntry = nullptr;
        uint32_t m_LeafBlockID = 0;
        uint32_t m_EntryIndex = 0;
        uint32_t m_LeafSlot = UINT32_MAX;
        uint32_t m_PostingIndexBlockID = 0;
        uint32_t m_PostingIndexOffset = 0;
        uint32_t m_PostingIndexLength = 0;
        uint32_t m_PostingContinuationBlockCount = 0;
        uint32_t m_DocFreq = 0;
        size_t m_LeafWriteOffset = 0;
        uint32_t m_LeafEntryCount = 0;
        uint32_t m_IndexBlockID = 0;
        size_t m_IndexOffsetInBlock = 0;
        uint32_t m_IndexBlockBase = 0;
        bool m_HasIndexWrite = false;
        uint8_t* m_IndexBuffer = nullptr;

        std::string_view CurrentTerm() const
        {
            return m_CurrentEntry ? std::string_view(m_CurrentEntry->LTE_Term, m_CurrentEntry->LTE_TermLength) : std::string_view();
        }

        bool AddIndex(size_t len)
        {
            constexpr size_t DATA_CAP = sizeof(IndexBlock::IB_Data);
            constexpr size_t CONT_HDR = sizeof(IndexBlockContinuationHeader);

            /*
            * Choose the target main-block position. The main segment stays
            * contiguous; if no complete posting pair fits in the current tail,
            * skip that tail and start at offset 0 in the next block.
            */
            const uint8_t* sourceBytes = m_IndexBuffer;
            uint32_t targetBlockID = m_IndexBlockID;
            size_t targetOffset = m_IndexOffsetInBlock;
            size_t splitLength = PostingSplitLength(sourceBytes, len, DATA_CAP - targetOffset);
            if (splitLength == 0) {
                ++targetBlockID;
                targetOffset = 0;
                splitLength = PostingSplitLength(sourceBytes, len, DATA_CAP);
                if (splitLength == 0) return false;
            }
            if (targetBlockID >= m_IndexBlockCount)
                return false;

            auto maxDocId = [](const uint8_t* data, size_t size) {
                size_t cursor = 0;
                uint64_t docId = 0;
                while (cursor < size) {
                    docId = 0;
                    uint8_t shift = 0;
                    while (true) {
                        const uint8_t byte = data[cursor++];
                        docId |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
                        if ((byte & 0x80u) == 0) break;
                        shift += 7;
                    }
                    while (data[cursor++] & 0x80u) {}
                }
                return docId;
            };

            struct ContinuationSegment {
                size_t Offset;
                size_t Length;
                uint64_t MaxDocID;
            };

            /*
            * Pre-shape continuation pages before copying. If capacity is not
            * enough, return false before committing any cursor or metadata state.
            */
            std::vector<ContinuationSegment> continuations;
            size_t sourceOffset = splitLength;
            while (sourceOffset < len) {
                const size_t remaining = len - sourceOffset;
                const size_t continuationLength = PostingSplitLength(sourceBytes + sourceOffset, remaining, DATA_CAP - CONT_HDR);
                if (continuationLength == 0) return false;
                continuations.push_back({ sourceOffset, continuationLength, maxDocId(sourceBytes + sourceOffset, continuationLength) });
                sourceOffset += continuationLength;
            }

            const uint64_t blocksNeeded = 1ull + static_cast<uint64_t>(continuations.size());
            if (blocksNeeded > m_IndexBlockCount - targetBlockID)
                return false;

            /*
            * Copy the main segment and page-shaped continuation blocks.
            * Continuation blocks must immediately follow the main block because
            * readers advance by IndexBlockID + 1, +2, ...
            */
            auto* targetBlock = reinterpret_cast<IndexBlock*>(m_BlockTable->m_IndexPool.BCP_Pages + static_cast<size_t>(targetBlockID) * sizeof(IndexBlock));
            std::memcpy(targetBlock->IB_Data + targetOffset, sourceBytes, splitLength);

            for (size_t i = 0; i < continuations.size(); ++i) {
                const auto& continuation = continuations[i];
                const uint32_t continuationBlockID = targetBlockID + 1 + static_cast<uint32_t>(i);
                targetBlock = reinterpret_cast<IndexBlock*>(m_BlockTable->m_IndexPool.BCP_Pages + static_cast<size_t>(continuationBlockID) * sizeof(IndexBlock));
                auto* header = reinterpret_cast<IndexBlockContinuationHeader*>(targetBlock->IB_Data);
                header->IBCH_MaxDocID = continuation.MaxDocID;
                header->IBCH_DataLength = static_cast<uint32_t>(continuation.Length);
                std::memcpy(targetBlock->IB_Data + CONT_HDR, sourceBytes + continuation.Offset, continuation.Length);
            }

            /*
            * Commit write cursor state only after the full scratch layout is
            * copied. If this function returned false earlier, copied bytes may
            * remain in memory, but no metadata points at them.
            */
            m_IndexBlockID = targetBlockID + static_cast<uint32_t>(continuations.size());
            m_IndexOffsetInBlock = continuations.empty() ? targetOffset + splitLength : DATA_CAP;
            m_PostingIndexBlockID = targetBlockID;
            m_PostingIndexOffset = static_cast<uint32_t>(targetOffset);
            m_PostingIndexLength = static_cast<uint32_t>(splitLength);
            m_PostingContinuationBlockCount = static_cast<uint32_t>(continuations.size());
            m_HasIndexWrite = true;
            return true;
        }

        void ReleaseCurrentLeaf()
        {
            if (m_CurrentLeaf && m_BlockTable) {
                m_BlockTable->ReleaseBlock(BlockKind::LeafTerm, m_LeafSlot, true);
                m_CurrentLeaf = nullptr;
                m_LeafSlot = UINT32_MAX;
            }
            m_CurrentEntry = nullptr;
        }

        void GoCurrent()
        {
            if (!m_BlockTable) return;

            while (m_LeafBlockID < m_LeafTermBlockCount) {
                if (!m_CurrentLeaf) {
                    m_CurrentLeaf = static_cast<const LeafTermBlock*>(m_BlockTable->GetBlock(BlockKind::LeafTerm, m_LeafBlockID, &m_LeafSlot, true));
                    if (!m_CurrentLeaf) return;
                }

                const uint8_t* blockBase = reinterpret_cast<const uint8_t*>(m_CurrentLeaf);
                const uint32_t entryCount = std::min<uint32_t>(m_CurrentLeaf->LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1], LEAF_TERM_DIRECTORY_COUNT - 1);
                if (m_EntryIndex < entryCount && m_EntryIndex < LEAF_TERM_DIRECTORY_COUNT - 1) {
                    m_CurrentEntry = reinterpret_cast<const LeafTermEntry*>(blockBase + m_CurrentLeaf->LTB_Directory[m_EntryIndex]);
                    return;
                }

                ReleaseCurrentLeaf();
                ++m_LeafBlockID;
                m_EntryIndex = 0;
            }
            ReleaseCurrentLeaf();
        }

        size_t PostingBytesSize() const
        {
            size_t bytesNeeded = m_CurrentEntry->LTE_IndexLength;
            for (uint32_t i = 0; i < m_CurrentEntry->LTE_ContinuationBlockCount; ++i) {
                uint32_t slot = UINT32_MAX;
                const auto* continuation = static_cast<const IndexBlock*>(m_BlockTable->GetBlock(BlockKind::Index, m_CurrentEntry->LTE_IndexBlockID + 1 + i, &slot, true));
                const auto* header = reinterpret_cast<const IndexBlockContinuationHeader*>(continuation->IB_Data);
                bytesNeeded += header->IBCH_DataLength;
                m_BlockTable->ReleaseBlock(BlockKind::Index, slot, true);
            }
            return bytesNeeded;
        }

        void ReadPostingBytesTo(uint8_t* bytes, size_t& bytesWritten) const
        {
            uint32_t slot = UINT32_MAX;
            const auto* block = static_cast<const IndexBlock*>(m_BlockTable->GetBlock(BlockKind::Index, m_CurrentEntry->LTE_IndexBlockID, &slot, true));
            std::memcpy(bytes + bytesWritten, block->IB_Data + m_CurrentEntry->LTE_IndexOffset, m_CurrentEntry->LTE_IndexLength);
            bytesWritten += m_CurrentEntry->LTE_IndexLength;
            m_BlockTable->ReleaseBlock(BlockKind::Index, slot, true);

            for (uint32_t i = 0; i < m_CurrentEntry->LTE_ContinuationBlockCount; ++i) {
                slot = UINT32_MAX;
                const auto* continuation = static_cast<const IndexBlock*>(m_BlockTable->GetBlock(BlockKind::Index, m_CurrentEntry->LTE_IndexBlockID + 1 + i, &slot, true));
                const auto* header = reinterpret_cast<const IndexBlockContinuationHeader*>(continuation->IB_Data);
                const uint8_t* begin = continuation->IB_Data + sizeof(IndexBlockContinuationHeader);
                std::memcpy(bytes + bytesWritten, begin, header->IBCH_DataLength);
                bytesWritten += header->IBCH_DataLength;
                m_BlockTable->ReleaseBlock(BlockKind::Index, slot, true);
            }
        }
    };

    static bool ReadVbPairEnd(const uint8_t* data, size_t size, size_t& offset)
    {
        auto readOne = [&]() -> bool {
            while (offset < size) {
                const uint8_t byte = data[offset++];
                if ((byte & 0x80u) == 0)
                    return true;
            }
            return false;
        };
        return readOne() && readOne();
    }

    static size_t PostingSplitLength(const uint8_t* data, size_t size, size_t capacity)
    {
        if (size <= capacity)
            return size;

        size_t byteOffset = 0;
        size_t lastPairEnd = 0;
        const size_t limit = std::min(size, capacity);
        while (byteOffset < limit) {
            const size_t pairStart = byteOffset;
            if (!ReadVbPairEnd(data, size, byteOffset) || byteOffset > limit) {
                byteOffset = pairStart;
                break;
            }
            lastPairEnd = byteOffset;
        }
        return lastPairEnd;
    }

    static bool AppendFile(FileAccess& output, const std::string& path)
    {
        FileAccess input(path.c_str());
        if (!input.Init()) return false;
        uint8_t buffer[PAGE_SIZE * 16];
        while (true) {
            const int bytes = input.GetData(buffer, static_cast<int>(sizeof(buffer)));
            if (bytes < 0) return false;
            if (bytes == 0) return true;
            if (!output.PutData(buffer, static_cast<uint64_t>(bytes))) return false;
        }
    }

    static bool ReadFileData(FileAccess& input, void* buffer, uint64_t bytes)
    {
        auto* cursor = static_cast<uint8_t*>(buffer);
        constexpr uint64_t READ_CHUNK_BYTES = 64ull * 1024ull * 1024ull;
        while (bytes > 0) {
            const int chunk = static_cast<int>(std::min<uint64_t>(bytes, READ_CHUNK_BYTES));
            if (input.GetData(cursor, chunk) != chunk)
                return false;
            cursor += chunk;
            bytes -= static_cast<uint64_t>(chunk);
        }
        return true;
    }

    uint32_t EstimateDocFreq(const shared_ptr<EvalNode>& node)
    {
        if (!node) return 0;
        if (node->GetType() == NodeType::Term) {
            auto* termNode = static_cast<TermNode*>(node.get());
            uint32_t indexBlockID = 0;
            uint32_t indexOffset = 0;
            uint32_t indexLength = 0;
            uint32_t docFreq = 0;
            uint32_t continuationBlocks = 0;
            if (m_BlockTable.FindTermData(termNode->stream_key.c_str(),
                                          &indexBlockID,
                                          &indexOffset,
                                          &indexLength,
                                          &docFreq,
                                          &continuationBlocks)) {
                return docFreq;
            }
            return 0;
        }

        if (node->GetType() == NodeType::Or) {
            auto* orNode = static_cast<OrNode*>(node.get());
            uint64_t total = 0;
            for (const auto& child : orNode->children)
                total += EstimateDocFreq(child);
            return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
        }

        return 0;
    }

    bool IsHighDfWeakAndGate(const shared_ptr<EvalNode>& node)
    {
        const uint64_t docCount = DocumentCount();
        if (docCount == 0) return false;
        const uint32_t docFreq = EstimateDocFreq(node);
        if (docFreq == 0) return false;

        const uint64_t threshold = std::max<uint64_t>(100000, docCount / 5); // 20% corpus
        return docFreq > threshold;
    }

    /*
    * Recursively convert EvalNode → ISR tree.
    * Mirrors ISRCreatorDocShard::CreateIsr():
    *   Load node  → CreateWordIsr()  → AdvancedIndexReader (leaf)
    *   And node   → CreateAndIsr()   → AndIndexReader
    *   Or node    → CreateOrIsr()    → OrIndexReader
    *   Not node   → CreateFilterIsr()→ NotIndexReader
    */
    shared_ptr<IndexReader> BuildIndexReader(const shared_ptr<EvalNode>& node)
    {
        if (!node) {
            auto empty = make_shared<AdvancedIndexReader>();
            return empty;
        }

        switch (node->GetType()) {

        case NodeType::Term: {
            /*
            * Leaf: create AdvancedIndexReader for this (term + stream) key.
            * If the term is not in the index the reader is immediately IsEnd.
            */
            auto* termNode = static_cast<TermNode*>(node.get());
            auto  reader   = make_shared<AdvancedIndexReader>();

            reader->Open(termNode->stream_key.c_str(),
                         &m_BlockTable,
                         this,
                         termNode->word_span);
            return reader;
        }

        case NodeType::And: {
            auto* andNode = static_cast<AndNode*>(node.get());
            std::vector<shared_ptr<IndexReader>> children;

            for (auto& child : andNode->children)
                children.push_back(BuildIndexReader(child));

            if (children.empty()) {
                auto empty = make_shared<AdvancedIndexReader>();
                return empty;
            }

            if (children.size() == 1)
                return children[0];

            return make_shared<AndIndexReader>(std::move(children));
        }

        case NodeType::Or: {
            auto* orNode = static_cast<OrNode*>(node.get());
            std::vector<shared_ptr<IndexReader>> children;

            for (auto& child : orNode->children)
                children.push_back(BuildIndexReader(child));

            if (children.empty()) {
                auto empty = make_shared<AdvancedIndexReader>();
                return empty;
            }

            if (children.size() == 1)
                return children[0];

            return make_shared<OrIndexReader>(std::move(children));
        }

        case NodeType::WeakAnd: {
            auto* weakAndNode = static_cast<WeakAndNode*>(node.get());
            std::vector<shared_ptr<IndexReader>> children;

            if (m_WeakAndBuildMode == WeakAndBuildMode::FlatPruned) {
                auto appendChild = [&](auto&& self, const shared_ptr<EvalNode>& child, bool allowHighDf) -> void {
                    if (!child)
                        return;
                    if (child->GetType() == NodeType::Or) {
                        auto* orNode = static_cast<OrNode*>(child.get());
                        for (auto& grandchild : orNode->children)
                            self(self, grandchild, allowHighDf);
                        return;
                    }

                    if (!allowHighDf && IsHighDfWeakAndGate(child))
                        return;

                    auto reader = BuildIndexReader(child);
                    if (reader && !reader->IsEnd())
                        children.push_back(std::move(reader));
                };

                for (auto& child : weakAndNode->children)
                    appendChild(appendChild, child, false);

                if (children.empty()) {
                    for (auto& child : weakAndNode->children)
                        appendChild(appendChild, child, true);
                }
            } else {
                const bool pruneEmpty = m_WeakAndBuildMode == WeakAndBuildMode::OrChildrenPruned;
                for (auto& child : weakAndNode->children) {
                    auto reader = BuildIndexReader(child);
                    if (pruneEmpty && (!reader || reader->IsEnd()))
                        continue;
                    children.push_back(std::move(reader));
                }
            }

            if (children.empty()) {
                auto empty = make_shared<AdvancedIndexReader>();
                return empty;
            }

            uint32_t minShouldMatch = 1;
            if (m_WeakAndBuildMode == WeakAndBuildMode::FlatPruned) {
                if (children.size() <= 2) minShouldMatch = 1;
                else if (children.size() <= 5) minShouldMatch = 2;
                else minShouldMatch = 3;
            } else {
                minShouldMatch = std::min<uint32_t>(
                    weakAndNode->min_should_match,
                    static_cast<uint32_t>(children.size()));
            }

            if (children.size() == 1)
                return children[0];

            return make_shared<WeakAndIndexReader>(std::move(children), minShouldMatch);
        }

        case NodeType::Not: {
            auto* notNode = static_cast<NotNode*>(node.get());
            auto  base    = BuildIndexReader(notNode->base);
            auto  excl    = BuildIndexReader(notNode->exclude);

            return make_shared<NotIndexReader>(base, excl);
        }

        case NodeType::Boost: {
            auto* boostNode = static_cast<BoostNode*>(node.get());
            auto base = BuildIndexReader(boostNode->base);
            if (!base || base->IsEnd())
                return base;

            auto boost = BuildIndexReader(boostNode->boost);
            if (!boost || boost->IsEnd())
                return base;

            return make_shared<BoostIndexReader>(base, boost, boostNode->boost_weight);
        }

        default: {
            auto empty = make_shared<AdvancedIndexReader>();
            return empty;
        }
        }
    }

    shared_ptr<IndexReader> BuildVectorIndexReader(const std::vector<float>& queryVector,
                                                   size_t efSearch = 200)
    {
        BuildVectorRuntime();
        if (queryVector.empty() || m_VectorIndex.Empty()) {
            auto empty = make_shared<AdvancedIndexReader>();
            return empty;
        }

        const size_t vectorTopK = std::max<size_t>(1, efSearch);
        return make_shared<VectorIndexReader>(
            m_VectorIndex.Search(queryVector, vectorTopK, VectorMetric::Cosine, efSearch));
    }
};

#endif
