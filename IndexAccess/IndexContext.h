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
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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
                         const char* indexFile   = "")
        : m_Store(make_shared<PostingStore>())
        , m_Params(make_shared<ConfigParameters>())
        , m_BlockTable(512)
        , m_Compiler(&m_Tokenizer)
        , m_Executor(this)
        , m_EmbeddingModel(make_shared<TFIDFSemanticEmbedding>(128))
        , m_IndexPath(indexFile ? indexFile : "")
        , m_Built(false)
        , m_LoadedFromDisk(false)
    {
        if (!m_IndexPath.empty())
            LoadIndex();
    }

    ~IndexContext()
    {
        ClearPinnedIndexData();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
        if (m_LoadedFromDisk) {
            m_Store = make_shared<PostingStore>();
            ClearPinnedIndexData();
            m_BlockTable.SetBlockMemory(nullptr, nullptr, 0, 0);
            m_Executor = IndexSearchExecutor(this);
            m_Built = false;
            m_LoadedFromDisk = false;
        }
        return make_shared<AdvancedIndexWriter>(m_Store);
    }

    uint64_t AllocateDocumentID() const
    {
        if (m_Store->AllDocStats().empty())
            return 0;

        uint64_t maxId = 0;
        for (const auto& [docId, _] : m_Store->AllDocStats())
            maxId = std::max(maxId, docId);
        return maxId + 1;
    }

    uint64_t AddDocument(const Document& doc)
    {
        const uint64_t docId = doc.doc_id == UINT64_MAX ? AllocateDocumentID() : doc.doc_id;
        auto writer = GetWriter();

        auto titleTokens = m_Tokenizer.Tokenize(doc.title.c_str());
        const std::string url = doc.url.empty() ? doc.path : doc.url;
        auto urlTokens = m_Tokenizer.Tokenize(url.c_str());
        auto anchorTokens = m_Tokenizer.Tokenize(doc.anchor.c_str());
        auto bodyTokens = m_Tokenizer.Tokenize(doc.body.c_str());
        auto metaTokens = m_Tokenizer.Tokenize(doc.meta.c_str());

        std::vector<std::string> embeddingTokens;
        embeddingTokens.reserve(titleTokens.size() + urlTokens.size() + anchorTokens.size() + bodyTokens.size() + metaTokens.size());
        embeddingTokens.insert(embeddingTokens.end(), titleTokens.begin(), titleTokens.end());
        embeddingTokens.insert(embeddingTokens.end(), urlTokens.begin(), urlTokens.end());
        embeddingTokens.insert(embeddingTokens.end(), anchorTokens.begin(), anchorTokens.end());
        embeddingTokens.insert(embeddingTokens.end(), bodyTokens.begin(), bodyTokens.end());
        embeddingTokens.insert(embeddingTokens.end(), metaTokens.begin(), metaTokens.end());

        writer->Write(titleTokens, docId, "Title");
        writer->Write(urlTokens, docId, "URL");
        writer->Write(anchorTokens, docId, "Anchor");
        writer->Write(bodyTokens, docId, "Body");
        writer->Write(metaTokens, docId, "Meta");
        writer->SetDocImportance(docId, doc.importance);
        if (!embeddingTokens.empty())
            writer->SetDocVector(docId, m_EmbeddingModel->Embed(embeddingTokens));
        if (!doc.path.empty())
            m_Store->SetDocPath(docId, doc.path);

        if (!RefreshDocDataFromBuildState())
            return UINT64_MAX;

        return docId;
    }

    /*
    * Build() — delegates to IndexSerializer::BuildBlocksForContext so the
    * block packing logic is in one place.
    */
    void Build()
    {
        if (m_Built) return;

        if (!RefreshDocDataFromBuildState())
            return;
        auto br = IndexSerializer::BuildBlocksForContext(*m_Store);

        size_t n = br.BBR_IndexBlocks.size();
        if (n > UINT32_MAX || br.BBR_LeafTermBlocks.size() > UINT32_MAX || br.BBR_HeadTermEntries.size() > UINT32_MAX) {
            std::cerr << "Built index section count exceeds runtime limit\n";
            ResetLoadedRuntimeState();
            return;
        }
        ClearBlockPageDataOnly();

        const uint32_t indexBlockCount = static_cast<uint32_t>(n);
        const uint32_t leafTermBlockCount = static_cast<uint32_t>(br.BBR_LeafTermBlocks.size());
        const uint32_t headCount = static_cast<uint32_t>(br.BBR_HeadTermEntries.size());

        if (indexBlockCount > 0) {
            m_IndexBlocks = static_cast<IndexBlock*>(
                PinnedMemAlloc(static_cast<uint64_t>(indexBlockCount) * sizeof(IndexBlock)));
            if (!m_IndexBlocks) {
                std::cerr << "Failed to allocate pinned index block memory\n";
                ResetLoadedRuntimeState();
                return;
            }
        }

        if (leafTermBlockCount > 0) {
            m_LeafTermBlocks = static_cast<LeafTermBlock*>(
                PinnedMemAlloc(static_cast<uint64_t>(leafTermBlockCount) * sizeof(LeafTermBlock)));
            if (!m_LeafTermBlocks) {
                std::cerr << "Failed to allocate pinned leaf term block memory\n";
                ResetLoadedRuntimeState();
                return;
            }
        }

        if (indexBlockCount > 0)
            std::memcpy(m_IndexBlocks, br.BBR_IndexBlocks.data(), static_cast<size_t>(indexBlockCount) * sizeof(IndexBlock));
        if (leafTermBlockCount > 0)
            std::memcpy(m_LeafTermBlocks, br.BBR_LeafTermBlocks.data(), static_cast<size_t>(leafTermBlockCount) * sizeof(LeafTermBlock));

        std::unique_ptr<HeadTermEntry[]> headTermEntries;
        if (headCount > 0) {
            headTermEntries.reset(new HeadTermEntry[headCount]);
            std::memcpy(headTermEntries.get(), br.BBR_HeadTermEntries.data(), static_cast<size_t>(headCount) * sizeof(HeadTermEntry));
        }
        m_BlockTable.SetBlockMemory(m_IndexBlocks, m_LeafTermBlocks, indexBlockCount, leafTermBlockCount);
        m_BlockTable.SetHeadTermEntries(std::move(headTermEntries), headCount);

        m_Built = true;
        m_LoadedFromDisk = false;
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

        if (evalTree->HasTextQuery() && evalTree->HasVectorQuery()) {
            std::vector<shared_ptr<IndexReader>> children;
            children.push_back(BuildIndexReader(evalTree->root));
            children.push_back(BuildVectorIndexReader(evalTree->vector_query, evalTree->vector_ef_search));
            return make_shared<OrIndexReader>(std::move(children));
        }

        if (evalTree->HasVectorQuery())
            return BuildVectorIndexReader(evalTree->vector_query, evalTree->vector_ef_search);

        return BuildIndexReader(evalTree->root);
    }

    Tokenizer*           GetTokenizer() { return &m_Tokenizer; }
    IndexSearchCompiler* GetCompiler()  { return &m_Compiler; }
    IndexSearchExecutor* GetExecutor()  { return new IndexSearchExecutor(this); }

    /* Compile query string and return an ISR tree ready for traversal. */
    shared_ptr<IndexReader> GetReader(const char* queryString,
                                      const char* streamSet = "AUT")
    {
        auto tree = std::unique_ptr<EvalTree>(
                        m_Compiler.Compile(queryString, streamSet, m_EmbeddingModel.get()));
        return GetReader(tree.get());
    }

    /* Open a reader for one specific stream key (e.g. "raceA", "carT"). */
    shared_ptr<IndexReader> GetStreamReader(const char* streamKey)
    {
        auto reader = make_shared<AdvancedIndexReader>();
        reader->Open(streamKey, &m_BlockTable, this);
        return reader;
    }

    PostingStore* GetStore() { return m_Store.get(); }

    const DocDataEntry* GetDocDataEntry(uint64_t docId) const
    {
        if (!m_DocData || docId >= m_IndexFileHeader.IFH_NumDocuments)
            return nullptr;

        const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + docId * DOC_REC_SIZE);
        return entry->DDE_DocID == docId ? entry : nullptr;
    }

    const IndexFileHeader& GetIndexFileHeader() const { return m_IndexFileHeader; }

    std::vector<float> CompileToVector(const char* queryString)
    {
        return m_Compiler.CompileToVector(queryString, m_EmbeddingModel.get());
    }

    std::vector<VectorSearchResult> VectorSearch(const std::vector<float>& query,
                                                 size_t topK = 20,
                                                 VectorMetric metric = VectorMetric::Cosine,
                                                 size_t efSearch = 200)
    {
        return m_Store->VectorSearch(query, topK, metric, efSearch);
    }

    bool SaveIndex()
    {
        if (m_IndexPath.empty()) return false;
        return SaveIndex(m_IndexPath.c_str());
    }

    bool SaveIndex(const char* path)
    {
        if (!path || !*path) return false;
        m_IndexPath = path;

        if (!IndexSerializer::Save(*m_Store, path)) return false;

        IndexFileHeader header{};
        FileAccess indexFile(path);
        if (!indexFile.Init()
            || indexFile.GetData(&header, static_cast<int>(sizeof(header))) != static_cast<int>(sizeof(header))
            || std::memcmp(header.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC)) != 0
            || header.IFH_Version != INDEX_FORMAT_VERSION)
        {
            return true;  // save succeeded; FileBlockManager wiring is best-effort
        }

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
            || m_IndexFileHeader.IFH_IndexBlockCount > UINT32_MAX)
        {
            std::cerr << "Index section count exceeds runtime limit in: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        ClearBlockPageDataOnly();

        const uint32_t indexBlockCount = static_cast<uint32_t>(m_IndexFileHeader.IFH_IndexBlockCount);
        const uint32_t leafTermBlockCount = static_cast<uint32_t>(m_IndexFileHeader.IFH_LeafTermBlockCount);
        const uint32_t indexBlockLoadCount = std::min(indexBlockCount, INDEX_BLOCK_CACHE_SLOT_COUNT);
        const uint32_t leafTermBlockLoadCount = std::min(leafTermBlockCount, LEAF_TERM_BLOCK_CACHE_SLOT_COUNT);

        m_IndexBlocks = static_cast<IndexBlock*>(
            PinnedMemAlloc(static_cast<uint64_t>(INDEX_BLOCK_CACHE_SLOT_COUNT) * sizeof(IndexBlock)));
        m_LeafTermBlocks = static_cast<LeafTermBlock*>(
            PinnedMemAlloc(static_cast<uint64_t>(LEAF_TERM_BLOCK_CACHE_SLOT_COUNT) * sizeof(LeafTermBlock)));
        if (!m_IndexBlocks || !m_LeafTermBlocks) {
            std::cerr << "Failed to allocate pinned index/leaf runtime cache memory for: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        const uint64_t indexBlockBytes = static_cast<uint64_t>(indexBlockLoadCount) * sizeof(IndexBlock);
        if (indexBlockBytes > 0
            && (indexBlockBytes > static_cast<uint64_t>(std::numeric_limits<int>::max())
                || !indexFile.SetPosition(m_IndexFileHeader.IFH_IndexBlockOffset)
                || indexFile.GetData(m_IndexBlocks, static_cast<int>(indexBlockBytes)) != static_cast<int>(indexBlockBytes)))
        {
            std::cerr << "Failed to read IndexBlocks for: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        const uint64_t leafTermBlockBytes = static_cast<uint64_t>(leafTermBlockLoadCount) * sizeof(LeafTermBlock);
        if (leafTermBlockBytes > 0
            && (leafTermBlockBytes > static_cast<uint64_t>(std::numeric_limits<int>::max())
                || !indexFile.SetPosition(m_IndexFileHeader.IFH_LeafTermBlockOffset)
                || indexFile.GetData(m_LeafTermBlocks, static_cast<int>(leafTermBlockBytes)) != static_cast<int>(leafTermBlockBytes)))
        {
            std::cerr << "Failed to read LeafTermBlocks for: " << m_IndexPath << "\n";
            ResetLoadedRuntimeState();
            return;
        }

        m_BlockTable.SetBlockMemory(m_IndexBlocks, m_LeafTermBlocks, indexBlockLoadCount, leafTermBlockLoadCount);
        m_BlockTable.SetHeadTermEntries(std::move(headTermEntries), static_cast<uint32_t>(m_IndexFileHeader.IFH_HeadTermEntryCount));

        ClearDocDataOnly();

        if (m_IndexFileHeader.IFH_NumDocuments > UINT64_MAX / DOC_REC_SIZE)
        {
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

            if (docdata_bytes > static_cast<uint64_t>(std::numeric_limits<int>::max())
                || !indexFile.SetPosition(m_IndexFileHeader.IFH_DocDataOffset)
                || indexFile.GetData(m_DocData, static_cast<int>(docdata_bytes)) != static_cast<int>(docdata_bytes))
            {
                std::cerr << "Failed to read DocData for: " << m_IndexPath << "\n";
                ResetLoadedRuntimeState();
                return;
            }
        }

        RefreshStoreFromLoadedDocData();

        m_Executor = IndexSearchExecutor(this);
        m_Built    = true;
        m_LoadedFromDisk = true;
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
    SmartTokenizer               m_Tokenizer;
    IndexSearchCompiler          m_Compiler;
    IndexSearchExecutor          m_Executor;
    shared_ptr<IEmbeddingModel>  m_EmbeddingModel;
    std::string                  m_IndexPath;
    bool                         m_Built;
    bool                         m_LoadedFromDisk;
    IndexFileHeader              m_IndexFileHeader{};
    uint8_t*                     m_DocData = nullptr;
    IndexBlock*                  m_IndexBlocks = nullptr;
    LeafTermBlock*               m_LeafTermBlocks = nullptr;

    static constexpr uint32_t INDEX_BLOCK_CACHE_SLOT_COUNT =
        static_cast<uint32_t>(INDEX_BLOCK_CACHE_BYTES / sizeof(IndexBlock));
    static constexpr uint32_t LEAF_TERM_BLOCK_CACHE_SLOT_COUNT =
        static_cast<uint32_t>(LEAF_TERM_CACHE_BYTES / sizeof(LeafTermBlock));

    void ClearDocDataOnly()
    {
        if (m_DocData)
            PinnedMemFree(m_DocData);
        m_DocData = nullptr;
    }

    void ClearPinnedIndexData()
    {
        ClearDocDataOnly();
        ClearBlockPageDataOnly();
        m_IndexFileHeader = {};
    }

    void ClearBlockPageDataOnly()
    {
        if (m_IndexBlocks)
            PinnedMemFree(m_IndexBlocks);
        m_IndexBlocks = nullptr;

        if (m_LeafTermBlocks)
            PinnedMemFree(m_LeafTermBlocks);
        m_LeafTermBlocks = nullptr;
    }

    void ResetLoadedRuntimeState()
    {
        ClearPinnedIndexData();
        m_BlockTable.SetBlockMemory(nullptr, nullptr, 0, 0);
        m_Executor = IndexSearchExecutor(this);
        m_Built = false;
        m_LoadedFromDisk = false;
    }

    bool RefreshDocDataFromBuildState()
    {
        if (!m_Store || m_Store->AllDocStats().empty())
            return true;

        uint64_t maxDocId = 0;
        for (const auto& [docId, _] : m_Store->AllDocStats())
            maxDocId = std::max(maxDocId, docId);

        ClearDocDataOnly();
        const uint64_t bytes = (maxDocId + 1) * DOC_REC_SIZE;
        m_DocData = static_cast<uint8_t*>(PinnedMemAlloc(bytes));
        if (!m_DocData) {
            std::cerr << "Failed to allocate pinned DocData memory\n";
            ClearDocDataOnly();
            return false;
        }

        uint64_t totalLen = 0;
        for (const auto& [docId, stats] : m_Store->AllDocStats()) {
            auto* entry = reinterpret_cast<DocDataEntry*>(m_DocData + docId * DOC_REC_SIZE);
            entry->DDE_DocID = docId;
            entry->DDE_StaticRank = stats.importance;
            entry->DDE_DocLength = stats.doc_len;
            if (const auto* vector = m_Store->GetDocVector(docId);
                vector && !vector->empty() && vector->size() <= DOC_VECTOR_STORAGE_MAX_DIM) {
                entry->DDE_VectorDim = static_cast<uint16_t>(vector->size());
                entry->DDE_VectorFormat = 1;
                for (size_t i = 0; i < vector->size(); ++i) {
                    // Quantize float32 to int8: clamp to [-128, 127]
                    const float val = (*vector)[i];
                    const float clipped = std::max(-128.0f, std::min(127.0f, val * 128.0f));
                    entry->DDE_VectorData[i] = static_cast<int8_t>(std::round(clipped));
                }
            }
            entry->DDE_PathLength = static_cast<uint16_t>(std::min(stats.path.size(), DOC_PATH_MAX));
            if (entry->DDE_PathLength > 0)
                std::memcpy(entry->DDE_Path, stats.path.data(), entry->DDE_PathLength);
            totalLen += stats.doc_len;
        }

        const uint64_t totalDocs = maxDocId + 1;
        const float avgDocLen = totalDocs ? static_cast<float>(totalLen) / static_cast<float>(totalDocs) : 1.0f;
        std::memcpy(m_IndexFileHeader.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC));
        m_IndexFileHeader.IFH_Version = INDEX_FORMAT_VERSION;
        m_IndexFileHeader.IFH_NumDocuments = totalDocs;
        m_IndexFileHeader.IFH_AvgDocLength = avgDocLen;
        return true;
    }

    void RefreshStoreFromLoadedDocData()
    {
        m_Store = make_shared<PostingStore>();
        if (!m_DocData || m_IndexFileHeader.IFH_NumDocuments == 0)
            return;

        for (uint64_t docId = 0; docId < m_IndexFileHeader.IFH_NumDocuments; ++docId) {
            const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + docId * DOC_REC_SIZE);
            if (entry->DDE_DocID != docId)
                continue;

            m_Store->AddDocTokens(docId, entry->DDE_DocLength);
            m_Store->SetDocImportance(docId, entry->DDE_StaticRank);

            if (entry->DDE_VectorDim > 0
                && entry->DDE_VectorDim <= DOC_VECTOR_STORAGE_MAX_DIM
                && entry->DDE_VectorFormat == 1)
            {
                std::vector<float> vector(entry->DDE_VectorDim);
                for (size_t i = 0; i < entry->DDE_VectorDim; ++i)
                    vector[i] = static_cast<float>(entry->DDE_VectorData[i]) / 128.0f;
                m_Store->SetDocVector(docId, std::move(vector));
            }

            if (entry->DDE_PathLength > 0 && entry->DDE_PathLength <= DOC_PATH_MAX)
                m_Store->SetDocPath(docId, std::string(reinterpret_cast<const char*>(entry->DDE_Path), entry->DDE_PathLength));
        }
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
                         this);
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

        case NodeType::Not: {
            auto* notNode = static_cast<NotNode*>(node.get());
            auto  base    = BuildIndexReader(notNode->base);
            auto  excl    = BuildIndexReader(notNode->exclude);

            return make_shared<NotIndexReader>(base, excl);
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
        if (queryVector.empty() || m_Store->VectorCount() == 0) {
            auto empty = make_shared<AdvancedIndexReader>();
            return empty;
        }

        return make_shared<VectorIndexReader>(
            m_Store->VectorSearch(queryVector, 0, VectorMetric::Cosine, efSearch));
    }
};

#endif
