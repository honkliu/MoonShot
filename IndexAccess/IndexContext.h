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
#include <string_view>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <utility>
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
        ClearPinnedIndexData();
        ClearWritePinnedIndexData();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
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
            writer->SetDocVector(docId, m_VectorIndex.GetModel()->Embed(embeddingTokens));
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
        if (m_Built) return;

        if (BuildRuntime(m_BlockTable, m_VectorIndex, m_IndexFileHeader, m_DocData, m_Built)) {
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
        Build();

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

    EvalTree* Compile(const char* queryString,
                      const char* streamSet = "AUT")
    {
        return m_Compiler.Compile(queryString, streamSet, m_VectorIndex.GetModel());
    }

    /* Compile query string and return an ISR tree ready for traversal. */
    shared_ptr<IndexReader> GetReader(const char* queryString,
                                      const char* streamSet = "AUT")
    {
        auto tree = std::unique_ptr<EvalTree>(Compile(queryString, streamSet));
        return GetReader(tree.get());
    }

    /* Open a reader for one specific stream key (e.g. "raceA", "carT"). */
    shared_ptr<IndexReader> GetStreamReader(const char* streamKey)
    {
        Build();

        auto reader = make_shared<AdvancedIndexReader>();
        reader->Open(streamKey, &m_BlockTable, this);
        return reader;
    }

    PostingStore* GetStore() { return m_Store.get(); }

    std::string GetDocPath(uint64_t docId) const
    {
        const auto* entry = GetDocDataEntry(docId);
        if (!entry || entry->DDE_PathLength == 0 || entry->DDE_PathLength > DOC_PATH_MAX)
            return {};
        return std::string(reinterpret_cast<const char*>(entry->DDE_Path), entry->DDE_PathLength);
    }

    const DocDataEntry* GetDocDataEntry(uint64_t docId) const
    {
        if (!m_DocData || docId >= m_IndexFileHeader.IFH_NumDocuments)
            return nullptr;

        const auto* entry = reinterpret_cast<const DocDataEntry*>(m_DocData + docId * DOC_REC_SIZE);
        return entry->DDE_DocID == docId ? entry : nullptr;
    }

    const IndexFileHeader& GetIndexFileHeader() const { return m_IndexFileHeader; }
    uint64_t DocumentCount() const { return m_IndexFileHeader.IFH_NumDocuments; }

    std::vector<float> CompileToVector(const char* queryString)
    {
        return m_Compiler.CompileToVector(queryString, m_VectorIndex.GetModel());
    }

    std::vector<VectorSearchResult> VectorSearch(const std::vector<float>& query,
                                                 size_t topK = 20,
                                                 VectorMetric metric = VectorMetric::Cosine,
                                                 size_t efSearch = 200)
    {
        EnsureVectorIndexBuilt();
        return m_VectorIndex.Search(query, topK, metric, efSearch);
    }

    size_t VectorCount() { EnsureVectorIndexBuilt(); return m_VectorIndex.Size(); }
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

        if (!BuildRuntime(m_WriteBlockTable, m_WriteVectorIndex, m_WriteIndexFileHeader, m_WriteDocData, m_WriteBuilt))
            return false;

        if (overwritingLoadedIndex) {
            ClearPinnedIndexData();
            m_BlockTable.SetBlockMemory(nullptr, nullptr);
            m_Executor = IndexSearchExecutor(this);
            m_Built = false;
            m_LoadedFromDisk = false;
        }

        if (!IndexSerializer::Save(m_WriteIndexFileHeader, m_WriteBlockTable, m_WriteDocData, path)) return false;

        if (!savingDeltaIndex)
            return true;

        auto delta = std::make_unique<IndexContext>("", "", false);
        delta->m_IndexPath = savePath;
        delta->m_BlockTable.HandOverBlockTable(m_WriteBlockTable);
        delta->m_VectorIndex = std::move(m_WriteVectorIndex);
        delta->m_IndexFileHeader = m_WriteIndexFileHeader;
        delta->m_DocData = m_WriteDocData;
        delta->m_Built = true;
        delta->m_LoadedFromDisk = true;
        delta->m_VectorBuilt = true;
        delta->m_Executor = IndexSearchExecutor(delta.get());

        m_WriteDocData = nullptr;
        m_WriteIndexFileHeader = {};
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
        const std::string tempPath = std::string(outputPath) + ".tmp";
        const std::string prefix(outputPath);
        const std::string headTempPath = prefix + ".head.tmp";
        const std::string leafTempPath = prefix + ".leaf.tmp";
        const std::string indexTempPath = prefix + ".blocks.tmp";
        const std::string docDataTempPath = prefix + ".docdata.tmp";
        std::remove(tempPath.c_str());
        std::remove(headTempPath.c_str());
        std::remove(leafTempPath.c_str());
        std::remove(indexTempPath.c_str());
        std::remove(docDataTempPath.c_str());

        auto cleanupTempFiles = [&]() {
            std::remove(tempPath.c_str());
            std::remove(headTempPath.c_str());
            std::remove(leafTempPath.c_str());
            std::remove(indexTempPath.c_str());
            std::remove(docDataTempPath.c_str());
        };

        IndexContext& delta = *m_DeltaContext;

        // Step 2: Write DocData to a temp stream by appending resident base
        // DocData first and resident delta DocData second. No old raw source
        // file is opened here.
        uint64_t mergedDocs = 0;
        float avgDocLength = 1.0f;
        {
            FileAccess docFile(docDataTempPath.c_str());
            if (!docFile.InitWrite()) { cleanupTempFiles(); return false; }

            const uint64_t baseDocCount = m_IndexFileHeader.IFH_NumDocuments;
            const uint64_t deltaFirstDocId = baseDocCount;
            const uint64_t deltaDocLimit = delta.m_IndexFileHeader.IFH_NumDocuments;
            if (deltaDocLimit < deltaFirstDocId) { cleanupTempFiles(); return false; }
            const uint64_t deltaDocCount = deltaDocLimit - deltaFirstDocId;
            mergedDocs = deltaFirstDocId + deltaDocCount;

            uint64_t totalDocLength = static_cast<uint64_t>(static_cast<double>(m_IndexFileHeader.IFH_AvgDocLength) * static_cast<double>(baseDocCount));

            const uint64_t baseDocBytes = baseDocCount * DOC_REC_SIZE;
            if (baseDocBytes > 0 && !docFile.PutData(m_DocData, baseDocBytes)) { cleanupTempFiles(); return false; }

            if (deltaDocCount > 0) {
                const uint64_t deltaDocBytes = deltaDocCount * DOC_REC_SIZE;
                const uint8_t* deltaDocData = delta.m_DocData + deltaFirstDocId * DOC_REC_SIZE;
                if (!docFile.PutData(deltaDocData, deltaDocBytes)) { cleanupTempFiles(); return false; }

                totalDocLength += static_cast<uint64_t>(static_cast<double>(delta.m_IndexFileHeader.IFH_AvgDocLength) * static_cast<double>(deltaDocLimit));
            }

            avgDocLength = mergedDocs ? static_cast<float>(totalDocLength) / static_cast<float>(mergedDocs) : 1.0f;
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
        // The merged posting bytes and LeafTermEntry records are first packed
        // into the merge-local block table, then dumped to temp streams after
        // the term walk is complete.
        uint64_t headEntryCount = 0;
        uint64_t totalTerms = 0;
        uint32_t indexBlockCount = 0;
        {
            auto headFile = std::make_unique<FileAccess>(headTempPath.c_str());
            auto indexFile = std::make_unique<FileAccess>(indexTempPath.c_str());
            auto leafFile = std::make_unique<FileAccess>(leafTempPath.c_str());
            if (!headFile->InitWrite() || !indexFile->InitWrite() || !leafFile->InitWrite()) { cleanupTempFiles(); return false; }

            LeafTermBlockView baseCursor(m_BlockTable);
            LeafTermBlockView deltaCursor(delta.m_BlockTable);
            constexpr uint32_t MERGED_LEAF_BLOCK_LIMIT = 100;
            constexpr uint32_t MERGED_INDEX_BLOCK_LIMIT = 200;
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

                HeadTermEntry heads[MERGED_LEAF_BLOCK_LIMIT]{};
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

                if (!headFile->PutData(heads, static_cast<uint64_t>(usedLeafBlocks) * sizeof(HeadTermEntry))) return false;
                if (!leafFile->PutData(mergedLeafBlocks, static_cast<uint64_t>(usedLeafBlocks) * sizeof(LeafTermBlock))) return false;
                headEntryCount += usedLeafBlocks;

                if (!indexFile->PutData(mergedIndexBlocks, static_cast<uint64_t>(usedIndexBlocks) * sizeof(IndexBlock))) return false;
                indexBlockCount += usedIndexBlocks;

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
                    } else {
                        if (!dumpBatch()) { cleanupTempFiles(); return false; }
                    }
                } else if (takeBase) {
                    if (mergedCursor.CanAddLeafTermEntry(baseCursor)
                        && mergedCursor.AddIndex(baseCursor)
                        && mergedCursor.AddLeafTermEntry(baseCursor)) {
                        advanceBase = true;
                    } else {
                        if (!dumpBatch()) { cleanupTempFiles(); return false; }
                    }
                } else if (takeDelta) {
                    if (mergedCursor.CanAddLeafTermEntry(deltaCursor)
                        && mergedCursor.AddIndex(deltaCursor)
                        && mergedCursor.AddLeafTermEntry(deltaCursor)) {
                        advanceDelta = true;
                    } else {
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

            if (!dumpBatch()) { cleanupTempFiles(); return false; }
            headFile.reset();
            indexFile.reset();
            leafFile.reset();
        }

        // Step 4: Assemble <output>.tmp in v12 physical order, patch the header,
        // release old file handles, then rename <output>.tmp to the final path.
        {
            FileAccess output(tempPath.c_str());
            if (!output.InitWrite()) { cleanupTempFiles(); return false; }

            IndexFileHeader header{};
            if (!output.PutData(&header, sizeof(header))) { cleanupTempFiles(); return false; }
            if (!AppendFile(output, headTempPath)) { cleanupTempFiles(); return false; }
            if (!AppendFile(output, leafTempPath)) { cleanupTempFiles(); return false; }
            if (!AppendFile(output, docDataTempPath)) { cleanupTempFiles(); return false; }
            if (!AppendFile(output, indexTempPath)) { cleanupTempFiles(); return false; }

            std::memcpy(header.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC));
            header.IFH_Version = INDEX_FORMAT_VERSION;
            header.IFH_NumDocuments = mergedDocs;
            header.IFH_NumTerms = totalTerms;
            header.IFH_AvgDocLength = avgDocLength;
            header.IFH_HeadTermEntryOffset = sizeof(IndexFileHeader);
            header.IFH_HeadTermEntryCount = headEntryCount;
            header.IFH_LeafTermBlockOffset = header.IFH_HeadTermEntryOffset + header.IFH_HeadTermEntryCount * sizeof(HeadTermEntry);
            header.IFH_LeafTermBlockCount = headEntryCount;
            header.IFH_DocDataOffset = header.IFH_LeafTermBlockOffset + header.IFH_LeafTermBlockCount * sizeof(LeafTermBlock);
            header.IFH_IndexBlockOffset = header.IFH_DocDataOffset + mergedDocs * DOC_REC_SIZE;
            header.IFH_IndexBlockCount = indexBlockCount;

            if (!output.SetPosition(0) || !output.PutData(&header, sizeof(header))) {
                cleanupTempFiles();
                return false;
            }
        }

        std::remove(headTempPath.c_str());
        std::remove(leafTempPath.c_str());
        std::remove(indexTempPath.c_str());
        std::remove(docDataTempPath.c_str());

        ResetLoadedRuntimeState();
        std::remove(outputPath);
        if (std::rename(tempPath.c_str(), outputPath) != 0) {
            cleanupTempFiles();
            return false;
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

        m_VectorIndex.SetDocData(m_DocData);
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
    std::unique_ptr<IndexContext> m_DeltaContext;

    IndexBlockTable              m_WriteBlockTable;
    FreshDiskAnnVectorIndex      m_WriteVectorIndex;
    IndexFileHeader              m_WriteIndexFileHeader{};
    uint8_t*                     m_WriteDocData = nullptr;
    bool                         m_WriteBuilt = false;

    static constexpr uint32_t INDEX_BLOCK_CACHE_SLOT_COUNT =
        static_cast<uint32_t>(INDEX_BLOCK_CACHE_BYTES / sizeof(IndexBlock));
    static constexpr uint32_t LEAF_TERM_BLOCK_CACHE_SLOT_COUNT =
        static_cast<uint32_t>(LEAF_TERM_CACHE_BYTES / sizeof(LeafTermBlock));

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
        m_VectorBuilt = false;
    }

    void ClearWritePinnedIndexData()
    {
        ClearDocDataOnly(m_WriteDocData, m_WriteVectorIndex);
        m_WriteBlockTable.SetBlockMemory(nullptr, nullptr);
        m_WriteIndexFileHeader = {};
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

    void EnsureVectorIndexBuilt()
    {
        if (m_VectorBuilt) return;
        m_VectorIndex.Clear();
        m_VectorIndex.SetDocData(m_DocData);
        if (m_DocData) {
            for (uint64_t docId = 0; docId < m_IndexFileHeader.IFH_NumDocuments; ++docId)
                m_VectorIndex.Add(docId);
        }
        m_VectorBuilt = true;
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
                           bool& built)
    {
        ClearDocDataOnly(docData, vectorIndex);
        blockTable.SetBlockMemory(nullptr, nullptr);
        header = {};
        built = false;
    }

    bool BuildRuntime(IndexBlockTable& blockTable,
                      FreshDiskAnnVectorIndex& vectorIndex,
                      IndexFileHeader& header,
                      uint8_t*& docData,
                      bool& built)
    {
        if (built) return true;

        if (!m_Store || m_Store->AllDocStats().empty()) {
            built = true;
            return true;
        }

        uint64_t maxDocId = 0;
        for (const auto& [docId, _] : m_Store->AllDocStats())
            maxDocId = std::max(maxDocId, docId);

        ClearDocDataOnly(docData, vectorIndex);
        docData = static_cast<uint8_t*>(PinnedMemAlloc((maxDocId + 1) * DOC_REC_SIZE));
        if (!docData) {
            ResetRuntimeState(blockTable, vectorIndex, header, docData, built);
            return false;
        }
        std::memset(docData, 0, static_cast<size_t>((maxDocId + 1) * DOC_REC_SIZE));
        for (uint64_t docId = 0; docId <= maxDocId; ++docId)
            reinterpret_cast<DocDataEntry*>(docData + docId * DOC_REC_SIZE)->DDE_DocID = UINT64_MAX;

        uint64_t totalLen = 0;
        for (const auto& [docId, stats] : m_Store->AllDocStats()) {
            auto* entry = reinterpret_cast<DocDataEntry*>(docData + docId * DOC_REC_SIZE);
            entry->DDE_DocID = docId;
            entry->DDE_StaticRank = stats.importance;
            entry->DDE_DocLength = stats.doc_len;
            entry->DDE_VectorDim = static_cast<uint16_t>(DOC_VECTOR_DIM);
            entry->DDE_VectorFormat = 1;
            std::memcpy(entry->DDE_VectorData, m_Store->GetDocVector(docId), DOC_VECTOR_DIM);
            entry->DDE_PathLength = static_cast<uint16_t>(std::min(stats.path.size(), DOC_PATH_MAX));
            if (entry->DDE_PathLength > 0)
                std::memcpy(entry->DDE_Path, stats.path.data(), entry->DDE_PathLength);
            totalLen += stats.doc_len;
        }

        const uint64_t totalDocs = maxDocId + 1;
        std::memcpy(header.IFH_Magic, INDEX_FILE_MAGIC, sizeof(INDEX_FILE_MAGIC));
        header.IFH_Version = INDEX_FORMAT_VERSION;
        header.IFH_NumDocuments = totalDocs;
        header.IFH_AvgDocLength = totalDocs ? static_cast<float>(totalLen) / static_cast<float>(totalDocs) : 1.0f;

        auto br = IndexSerializer::BuildBlocks(*m_Store);
        if (br.BBR_IndexBlocks.size() > UINT32_MAX || br.BBR_LeafTermBlocks.size() > UINT32_MAX || br.BBR_HeadTermEntries.size() > UINT32_MAX) {
            std::cerr << "Built index section count exceeds runtime limit\n";
            ResetRuntimeState(blockTable, vectorIndex, header, docData, built);
            return false;
        }

        const uint32_t indexBlockCount = static_cast<uint32_t>(br.BBR_IndexBlocks.size());
        const uint32_t leafTermBlockCount = static_cast<uint32_t>(br.BBR_LeafTermBlocks.size());
        const uint32_t headCount = static_cast<uint32_t>(br.BBR_HeadTermEntries.size());

        header.IFH_NumTerms = br.BBR_TotalTerms;
        header.IFH_HeadTermEntryOffset = sizeof(IndexFileHeader);
        header.IFH_HeadTermEntryCount = headCount;
        header.IFH_LeafTermBlockOffset = header.IFH_HeadTermEntryOffset + static_cast<uint64_t>(headCount) * sizeof(HeadTermEntry);
        header.IFH_LeafTermBlockCount = leafTermBlockCount;
        header.IFH_DocDataOffset = header.IFH_LeafTermBlockOffset + static_cast<uint64_t>(leafTermBlockCount) * sizeof(LeafTermBlock);
        header.IFH_IndexBlockOffset = header.IFH_DocDataOffset + header.IFH_NumDocuments * DOC_REC_SIZE;
        header.IFH_IndexBlockCount = indexBlockCount;

        auto* indexBlocks = blockTable.Init(BlockKind::Index, nullptr, 0, indexBlockCount, indexBlockCount);
        auto* leafTermBlocks = blockTable.Init(BlockKind::LeafTerm, nullptr, 0, leafTermBlockCount, leafTermBlockCount);
        if ((indexBlockCount > 0 && !indexBlocks) || (leafTermBlockCount > 0 && !leafTermBlocks)) {
            ResetRuntimeState(blockTable, vectorIndex, header, docData, built);
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

        vectorIndex.SetDocData(docData);
        for (uint64_t docId = 0; docId < header.IFH_NumDocuments; ++docId)
            vectorIndex.Add(docId);

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
            entry->LTE_IndexOffset = m_PostingIndexOffset;
            entry->LTE_IndexLength = m_PostingIndexLength;
            entry->LTE_ContinuationBlockCount = m_PostingContinuationBlockCount;
            m_LeafWriteOffset += entryBytes;
            ++m_LeafEntryCount;
            leaf->LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] = static_cast<uint16_t>(m_LeafEntryCount);
            m_PostingIndexLength = 0;
            m_PostingContinuationBlockCount = 0;
            return true;
        }

        bool HasWriteData() const
        {
            return UsedLeafBlockCount() > 0 || UsedIndexBlockCount() > 0;
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
                m_BlockTable->ReleaseBlock(BlockKind::LeafTerm, m_LeafSlot);
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
                    m_CurrentLeaf = static_cast<const LeafTermBlock*>(m_BlockTable->GetBlock(BlockKind::LeafTerm, m_LeafBlockID, &m_LeafSlot));
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
                const auto* continuation = static_cast<const IndexBlock*>(m_BlockTable->GetBlock(BlockKind::Index, m_CurrentEntry->LTE_IndexBlockID + 1 + i, &slot));
                const auto* header = reinterpret_cast<const IndexBlockContinuationHeader*>(continuation->IB_Data);
                bytesNeeded += header->IBCH_DataLength;
                m_BlockTable->ReleaseBlock(BlockKind::Index, slot);
            }
            return bytesNeeded;
        }

        void ReadPostingBytesTo(uint8_t* bytes, size_t& bytesWritten) const
        {
            uint32_t slot = UINT32_MAX;
            const auto* block = static_cast<const IndexBlock*>(m_BlockTable->GetBlock(BlockKind::Index, m_CurrentEntry->LTE_IndexBlockID, &slot));
            std::memcpy(bytes + bytesWritten, block->IB_Data + m_CurrentEntry->LTE_IndexOffset, m_CurrentEntry->LTE_IndexLength);
            bytesWritten += m_CurrentEntry->LTE_IndexLength;
            m_BlockTable->ReleaseBlock(BlockKind::Index, slot);

            for (uint32_t i = 0; i < m_CurrentEntry->LTE_ContinuationBlockCount; ++i) {
                slot = UINT32_MAX;
                const auto* continuation = static_cast<const IndexBlock*>(m_BlockTable->GetBlock(BlockKind::Index, m_CurrentEntry->LTE_IndexBlockID + 1 + i, &slot));
                const auto* header = reinterpret_cast<const IndexBlockContinuationHeader*>(continuation->IB_Data);
                const uint8_t* begin = continuation->IB_Data + sizeof(IndexBlockContinuationHeader);
                std::memcpy(bytes + bytesWritten, begin, header->IBCH_DataLength);
                bytesWritten += header->IBCH_DataLength;
                m_BlockTable->ReleaseBlock(BlockKind::Index, slot);
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
        EnsureVectorIndexBuilt();
        if (queryVector.empty() || m_VectorIndex.Empty()) {
            auto empty = make_shared<AdvancedIndexReader>();
            return empty;
        }

        return make_shared<VectorIndexReader>(
            m_VectorIndex.Search(queryVector, 0, VectorMetric::Cosine, efSearch));
    }
};

#endif
