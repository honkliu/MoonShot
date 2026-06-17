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
#include <iostream>
#include <vector>

#include <cstdio>

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
        , m_IndexPath(indexFile ? indexFile : "")
        , m_Built(false)
        , m_LoadedFromDisk(false)
    {
        if (!m_IndexPath.empty())
            LoadIndex();
    }

    ~IndexContext()
    {
        ClearDocData();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
        if (m_LoadedFromDisk) {
            m_Store = make_shared<PostingStore>();
            ClearDocData();
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
            writer->SetDocVector(docId, BuildHashedEmbedding(embeddingTokens));
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
                        m_Compiler.Compile(queryString, streamSet));
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

    const DocRecord* GetDocRecord(uint64_t docId) const
    {
        if (!m_DocData || docId >= m_TotalDocs)
            return nullptr;

        const auto* record = reinterpret_cast<const DocRecord*>(m_DocData + docId * DOC_REC_SIZE);
        return record->DR_DocID == docId ? record : nullptr;
    }

    const IndexFileHeader& GetIndexFileHeader() const { return m_IndexFileHeader; }

    std::vector<float> CompileToVector(const char* queryString, size_t dim = 128)
    {
        return m_Compiler.CompileToVector(queryString, dim);
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
        uint64_t blocks_offset = 0;
        PostingStore tmp;
        IndexSerializer::Load(tmp, path, nullptr, nullptr, &blocks_offset);

        auto fm = make_shared<FileBlockManager>(sizeof(IndexBlock), blocks_offset);
        if (fm->open(path))
            m_BlockTable.SetFileManager(std::move(fm));

        uint64_t leaf_offset = 0, leaf_pages = 0;
        PostingStore tmpLeaf;
        IndexSerializer::Load(tmpLeaf, path, nullptr, nullptr, nullptr, nullptr, nullptr, &leaf_offset, &leaf_pages);
        auto leafFm = make_shared<FileBlockManager>(sizeof(LeafTermPage), leaf_offset);
        if (leafFm->open(path))
            m_BlockTable.SetLeafTermFileManager(std::move(leafFm));
        m_BlockTable.ReserveLeafTermPageMap(static_cast<uint32_t>(std::min<uint64_t>(leaf_pages, UINT32_MAX)));

        return true;
    }

    void LoadIndex()
    {
        if (m_IndexPath.empty()) return;

        m_Store = make_shared<PostingStore>();
        ClearDocData();
        std::vector<HeadTermEntry>               headTermEntries;
        std::vector<uint64_t>                    pageskip;
        uint64_t blocks_offset = 0;
        uint64_t num_blocks = 0;
        uint64_t leaf_blocks_offset = 0;
        uint64_t num_leaf_blocks = 0;
        uint64_t docdata_offset = 0;
        uint64_t docdata_size = 0;
        uint64_t num_documents = 0;
        IndexFileHeader header{};

        if (!IndexSerializer::Load(*m_Store, m_IndexPath.c_str(),
                       &headTermEntries, nullptr,
                       &blocks_offset, &pageskip, &num_blocks,
                   &leaf_blocks_offset, &num_leaf_blocks,
                   &docdata_offset, &docdata_size, &num_documents,
                   &header))
        {
            std::cerr << "Failed to load index: " << m_IndexPath
                      << " (unsupported/corrupt format; rebuild with current moon.exe)\n";
            return;
        }

        m_BlockTable.Reset(PostingBlockCacheSlots(num_blocks));
        m_BlockTable.ReserveBlockMap(static_cast<uint32_t>(std::min<uint64_t>(num_blocks, UINT32_MAX)));
        m_BlockTable.SetHeadTermEntries(std::move(headTermEntries), static_cast<uint32_t>(std::min<uint64_t>(num_leaf_blocks, UINT32_MAX)));
        m_BlockTable.SetPageSkipData(std::move(pageskip));

        auto fm = make_shared<FileBlockManager>(sizeof(IndexBlock), blocks_offset);
        if (fm->open(m_IndexPath.c_str()))
            m_BlockTable.SetFileManager(std::move(fm));

        auto leafFm = make_shared<FileBlockManager>(sizeof(LeafTermPage), leaf_blocks_offset);
        if (leafFm->open(m_IndexPath.c_str()))
            m_BlockTable.SetLeafTermFileManager(std::move(leafFm));
        m_BlockTable.ReserveLeafTermPageMap(static_cast<uint32_t>(std::min<uint64_t>(num_leaf_blocks, UINT32_MAX)));

        LoadPinnedDocData(header);
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
    std::string                  m_IndexPath;
    bool                         m_Built;
    bool                         m_LoadedFromDisk;
    IndexFileHeader              m_IndexFileHeader{};
    std::unique_ptr<uint8_t[]>   m_DocDataBase;
    uint8_t*                     m_DocData = nullptr;
    uint64_t                     m_DocDataBytes = 0;
    uint64_t                     m_DocDataPageCount = 0;
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

    void ClearDocData()
    {
        if (m_DocData)
            UnpinMemoryPages(m_DocData, static_cast<size_t>(m_DocDataBytes));
        m_DocDataBase.reset();
        m_DocData = nullptr;
        m_DocDataBytes = 0;
        m_DocDataPageCount = 0;
        m_TotalDocs = 0;
        m_AvgDocLen = 1.0f;
        m_IndexFileHeader = {};
    }

    static uintptr_t AlignUp(uintptr_t value, uintptr_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static uint64_t PageAlignedBytes(uint64_t bytes)
    {
        return ((bytes + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    }

    static bool SeekFile(FILE* file, uint64_t offset)
    {
#if defined(_WIN32)
        return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
        return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
#endif
    }

    void LoadPinnedDocData(const IndexFileHeader& header)
    {
        ClearDocData();

        const uint64_t docDataBytes = header.IFH_DocDataSize;
        const uint64_t docCount = header.IFH_NumDocuments;
        if (docDataBytes == 0 || docCount == 0)
            return;
        if (docDataBytes % DOC_REC_SIZE != 0)
            return;

        const uint64_t docRecordCount = docDataBytes / DOC_REC_SIZE;
        const uint64_t pageBytes = PageAlignedBytes(docDataBytes);

        std::unique_ptr<uint8_t[]> docDataBase(new uint8_t[static_cast<size_t>(pageBytes + PAGE_SIZE - 1)]{});
        uint8_t* docData = reinterpret_cast<uint8_t*>(
            AlignUp(reinterpret_cast<uintptr_t>(docDataBase.get()), PAGE_SIZE));

        std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(m_IndexPath.c_str(), "rb"), &std::fclose);
        if (!file)
            return;
        if (!SeekFile(file.get(), header.IFH_DocDataOffset))
            return;

        const size_t bytesRead = std::fread(docData, 1, static_cast<size_t>(docDataBytes), file.get());
        if (bytesRead != docDataBytes)
            return;

        m_DocDataBase = std::move(docDataBase);
        m_DocData = docData;
        m_DocDataBytes = pageBytes;
        m_DocDataPageCount = pageBytes / PAGE_SIZE;
        m_TotalDocs = std::min<uint64_t>(docCount, docRecordCount);
        m_AvgDocLen = header.IFH_AvgDocLength > 0.0f ? header.IFH_AvgDocLength : 1.0f;
        m_IndexFileHeader = header;

        PinMemoryPages(m_DocData, static_cast<size_t>(pageBytes));
    }

    void RefreshDocDataFromBuildState()
    {
        if (!m_Store || m_Store->AllDocStats().empty())
            return;

        uint64_t maxDocId = 0;
        for (const auto& [docId, _] : m_Store->AllDocStats())
            maxDocId = std::max(maxDocId, docId);

        ClearDocData();
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
            auto* rec = reinterpret_cast<DocRecord*>(m_DocData + docId * DOC_REC_SIZE);
            rec->DR_DocID = docId;
            rec->DR_StaticRankHalf = EncodeFloat16(stats.importance);
            rec->DR_DocLength = stats.doc_len;
            if (const auto* vector = m_Store->GetDocVector(docId); vector && vector->size() == DOC_VECTOR_DIM) {
                rec->DR_VectorDim = static_cast<uint16_t>(DOC_VECTOR_DIM);
                rec->DR_VectorFormat = 1;
                for (size_t i = 0; i < DOC_VECTOR_DIM; ++i)
                    rec->DR_VectorHalf[i] = EncodeFloat16((*vector)[i]);
            }
            rec->DR_PathLength = EncodeDocPath(stats.path, rec->DR_Path);
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
