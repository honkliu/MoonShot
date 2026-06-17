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

#include <memory>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
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
            m_BlockTable.Reset(PostingBlockCacheSlots(0));
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

        RefreshDocDataFromBuildState();

        return docId;
    }

    /*
    * Build() — delegates to IndexSerializer::BuildBlocksForContext so the
    * packing logic (continuation fix, PageSkipList) is in one place.
    */
    void Build()
    {
        if (m_Built) return;

        RefreshDocDataFromBuildState();
        auto br = IndexSerializer::BuildBlocksForContext(*m_Store);

        size_t n = br.BBR_IndexBlocks.size();
        m_BlockTable.ResizeCache(PostingBlockCacheSlots(n));

        for (size_t i = 0; i < n; ++i) {
            m_BlockTable.InsertBlock(static_cast<uint32_t>(i), &br.BBR_IndexBlocks[i]);
        }

        m_BlockTable.SetPagedLeafTermBlocks(std::move(br.BBR_HeadTermEntries),
            std::move(br.BBR_LeafTermPages));
        m_BlockTable.SetPageSkipData(std::move(br.BBR_PageSkipList));

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

        EnsureBuilt();

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
        EnsureBuilt();
        auto reader = make_shared<AdvancedIndexReader>();
        reader->Open(streamKey, &m_BlockTable, this);
        return reader;
    }

    PostingStore* GetStore() { return m_Store.get(); }

    const DocDataEntry* GetDocDataEntry(uint64_t docId) const
    {
        if (!m_DocData || docId >= m_TotalDocs)
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

        EnsureBuilt();
        if (!IndexSerializer::Save(*m_Store, path)) return false;

        // Wire up FileBlockManager so cache misses reload from the .idx file.
        IndexLayoutInfo li;
        if (!IndexSerializer::LoadLayout(path, li))
            return true;  // save succeeded; FileBlockManager wiring is best-effort

        auto fm = make_shared<FileBlockManager>(sizeof(IndexBlock), li.ILI_BlocksOffset);
        if (fm->open(path))
            m_BlockTable.SetFileManager(std::move(fm));

        auto leafFm = make_shared<FileBlockManager>(sizeof(LeafTermPage), li.ILI_LeafBlocksOffset);
        if (leafFm->open(path))
            m_BlockTable.SetLeafTermFileManager(std::move(leafFm));
        m_BlockTable.ReserveLeafTermPageMap(static_cast<uint32_t>(std::min<uint64_t>(li.ILI_NumLeafBlocks, UINT32_MAX)));

        return true;
    }

    void LoadIndex()
    {
        if (m_IndexPath.empty()) return;

        ClearPinnedIndexData();

        IndexLayoutInfo li;
        if (!IndexSerializer::LoadLayout(m_IndexPath.c_str(), li))
        {
            std::cerr << "Failed to load index: " << m_IndexPath
                      << " (unsupported/corrupt format; rebuild with current moon.exe)\n";
            return;
        }

        m_IndexFileHeader = li.ILI_Header;
        m_IndexLayout = li;
        m_BlockTable.Reset(PostingBlockCacheSlots(li.ILI_NumBlocks));
        m_BlockTable.ReserveBlockMap(static_cast<uint32_t>(std::min<uint64_t>(li.ILI_NumBlocks, UINT32_MAX)));
        m_BlockTable.SetHeadTermEntries(std::move(li.ILI_HeadTermEntries), static_cast<uint32_t>(std::min<uint64_t>(li.ILI_NumLeafBlocks, UINT32_MAX)));
        m_BlockTable.SetPageSkipData(std::move(li.ILI_PageSkipData));

        ClearDocDataOnly();
        ClearLeafTermPagesOnly();
        ClearIndexBlocksOnly();

        const uint64_t docdata_bytes = li.ILI_Header.IFH_DocDataSize;

        if (!AllocatePinnedSection(docdata_bytes, m_DocDataBase, m_DocData, m_DocDataBytes))
        {
            std::cerr << "Failed to allocate pinned DocData memory for: " << m_IndexPath << "\n";
            ClearPinnedIndexData();
            return;
        }

        if (!IndexSerializer::ReadSections(m_IndexPath.c_str(),
                                           li.ILI_DocDataOffset, m_DocData, docdata_bytes,
                                           0, nullptr, 0,
                                           0, nullptr, 0))
        {
            std::cerr << "Failed to read DocData for: " << m_IndexPath << "\n";
            ClearPinnedIndexData();
            return;
        }

        m_DocDataPageCount        = m_DocDataBytes / PAGE_SIZE;
        m_TotalDocs               = std::min<uint64_t>(li.ILI_Header.IFH_NumDocuments, docdata_bytes / DOC_REC_SIZE);
        m_AvgDocLen               = li.ILI_Header.IFH_AvgDocLength > 0.0f ? li.ILI_Header.IFH_AvgDocLength : 1.0f;
        RefreshStoreFromLoadedDocData();

        auto fm = make_shared<FileBlockManager>(sizeof(IndexBlock), li.ILI_BlocksOffset);
        if (fm->open(m_IndexPath.c_str())) {
            m_BlockTable.SetFileManager(std::move(fm));
        }

        auto leafFm = make_shared<FileBlockManager>(sizeof(LeafTermPage), li.ILI_LeafBlocksOffset);
        if (leafFm->open(m_IndexPath.c_str())) {
            m_BlockTable.SetLeafTermFileManager(std::move(leafFm));
        }
        m_BlockTable.ReserveLeafTermPageMap(static_cast<uint32_t>(std::min<uint64_t>(li.ILI_NumLeafBlocks, UINT32_MAX)));

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
    IndexLayoutInfo              m_IndexLayout{};
    std::unique_ptr<uint8_t[]>   m_DocDataBase;
    uint8_t*                     m_DocData = nullptr;
    uint64_t                     m_DocDataBytes = 0;
    uint64_t                     m_DocDataPageCount = 0;
    std::unique_ptr<uint8_t[]>   m_LeafTermPagesBase;
    uint8_t*                     m_LeafTermPages = nullptr;
    uint64_t                     m_LeafTermPagesBytes = 0;
    uint64_t                     m_LeafTermPageCountPinned = 0;
    std::unique_ptr<uint8_t[]>   m_IndexBlocksBase;
    uint8_t*                     m_IndexBlocks = nullptr;
    uint64_t                     m_IndexBlocksBytes = 0;
    uint64_t                     m_IndexBlockCountPinned = 0;
    uint64_t                     m_TotalDocs = 0;
    float                        m_AvgDocLen = 1.0f;

    static uint32_t PostingBlockCacheSlots(uint64_t postingBlockCount)
    {
        static constexpr uint64_t CACHE_BYTES = 100ull * 1024ull * 1024ull;
        uint64_t maxSlots = std::max<uint64_t>(CACHE_BYTES / sizeof(IndexBlock), 1);
        uint64_t wanted = std::min<uint64_t>(postingBlockCount ? postingBlockCount : maxSlots, maxSlots);
        return static_cast<uint32_t>(std::max<uint64_t>(wanted, 1));
    }

    void EnsureBuilt()
    {
        if (!m_Built)
            Build();
    }

    void ClearDocDataOnly()
    {
        if (m_DocData)
            UnpinMemoryPages(m_DocData, static_cast<size_t>(m_DocDataBytes));
        m_DocDataBase.reset();
        m_DocData = nullptr;
        m_DocDataBytes = 0;
        m_DocDataPageCount = 0;
        m_TotalDocs = 0;
        m_AvgDocLen = 1.0f;
    }

    void ClearLeafTermPagesOnly()
    {
        if (m_LeafTermPages)
            UnpinMemoryPages(m_LeafTermPages, static_cast<size_t>(m_LeafTermPagesBytes));
        m_LeafTermPagesBase.reset();
        m_LeafTermPages = nullptr;
        m_LeafTermPagesBytes = 0;
        m_LeafTermPageCountPinned = 0;
    }

    void ClearIndexBlocksOnly()
    {
        if (m_IndexBlocks)
            UnpinMemoryPages(m_IndexBlocks, static_cast<size_t>(m_IndexBlocksBytes));
        m_IndexBlocksBase.reset();
        m_IndexBlocks = nullptr;
        m_IndexBlocksBytes = 0;
        m_IndexBlockCountPinned = 0;
    }

    void ClearPinnedIndexData()
    {
        ClearDocDataOnly();
        ClearLeafTermPagesOnly();
        ClearIndexBlocksOnly();
        m_IndexFileHeader = {};
        m_IndexLayout = {};
    }

    static uintptr_t AlignUp(uintptr_t value, uintptr_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static uint64_t PageAlignedBytes(uint64_t bytes)
    {
        return ((bytes + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    }

    bool AllocatePinnedSection(uint64_t logicalBytes,
                               std::unique_ptr<uint8_t[]>& baseOut,
                               uint8_t*& alignedOut,
                               uint64_t& alignedBytesOut)
    {
        baseOut.reset();
        alignedOut = nullptr;
        alignedBytesOut = 0;

        if (logicalBytes == 0)
            return true;

        const uint64_t pageBytes = PageAlignedBytes(logicalBytes);
        std::unique_ptr<uint8_t[]> sectionBase(new uint8_t[static_cast<size_t>(pageBytes + PAGE_SIZE - 1)]{});
        uint8_t* section = reinterpret_cast<uint8_t*>(
            AlignUp(reinterpret_cast<uintptr_t>(sectionBase.get()), PAGE_SIZE));
        if (!PinMemoryPages(section, static_cast<size_t>(pageBytes)))
            return false;

        baseOut = std::move(sectionBase);
        alignedOut = section;
        alignedBytesOut = pageBytes;
        return true;
    }

    void RefreshDocDataFromBuildState()
    {
        if (!m_Store || m_Store->AllDocStats().empty())
            return;

        uint64_t maxDocId = 0;
        for (const auto& [docId, _] : m_Store->AllDocStats())
            maxDocId = std::max(maxDocId, docId);

        ClearDocDataOnly();
        const uint64_t bytes = (maxDocId + 1) * DOC_REC_SIZE;
        static constexpr uintptr_t DOC_DATA_ALIGNMENT = 4096;
        const uint64_t pageBytes = PageAlignedBytes(bytes);
        m_DocDataPageCount = pageBytes / PAGE_SIZE;
        m_DocDataBase.reset(new uint8_t[static_cast<size_t>(pageBytes + DOC_DATA_ALIGNMENT - 1)]{});
        m_DocData = reinterpret_cast<uint8_t*>(
            AlignUp(reinterpret_cast<uintptr_t>(m_DocDataBase.get()), DOC_DATA_ALIGNMENT));
        m_DocDataBytes = pageBytes;

        uint64_t totalLen = 0;
        for (const auto& [docId, stats] : m_Store->AllDocStats()) {
            auto* entry = reinterpret_cast<DocDataEntry*>(m_DocData + docId * DOC_REC_SIZE);
            entry->DDE_DocID = docId;
            entry->DDE_StaticRankHalf = EncodeFloat16(stats.importance);
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
            entry->DDE_PathLength = EncodeDocPath(stats.path, entry->DDE_Path);
            totalLen += stats.doc_len;
        }

        PinMemoryPages(m_DocData, static_cast<size_t>(pageBytes));
        m_TotalDocs = maxDocId + 1;
        m_AvgDocLen = m_TotalDocs ? static_cast<float>(totalLen) / static_cast<float>(m_TotalDocs) : 1.0f;
        std::memcpy(m_IndexFileHeader.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC));
        m_IndexFileHeader.IFH_Version = INDEX_FORMAT_VERSION;
        m_IndexFileHeader.IFH_NumDocuments = m_TotalDocs;
        m_IndexFileHeader.IFH_AvgDocLength = m_AvgDocLen;
    }

    void RefreshStoreFromLoadedDocData()
    {
        m_Store = make_shared<PostingStore>();
        if (!m_DocData || m_TotalDocs == 0)
            return;

        for (uint64_t docId = 0; docId < m_TotalDocs; ++docId) {
            const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + docId * DOC_REC_SIZE);
            if (entry->DDE_DocID != docId)
                continue;

            m_Store->AddDocTokens(docId, entry->DDE_DocLength);
            m_Store->SetDocImportance(docId, DecodeFloat16(entry->DDE_StaticRankHalf));

            if (entry->DDE_VectorDim > 0
                && entry->DDE_VectorDim <= DOC_VECTOR_STORAGE_MAX_DIM
                && entry->DDE_VectorFormat == 1)
            {
                std::vector<float> vector(entry->DDE_VectorDim);
                for (size_t i = 0; i < entry->DDE_VectorDim; ++i)
                    vector[i] = static_cast<float>(entry->DDE_VectorData[i]) / 128.0f;
                m_Store->SetDocVector(docId, std::move(vector));
            }

            if (entry->DDE_PathLength > 0)
                m_Store->SetDocPath(docId, DecodeDocPath(*entry));
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
