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

#include <cstdio>

using std::shared_ptr;
using std::make_shared;
using std::string;

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
        , m_Executor(m_Store)
        , m_IndexPath(indexFile ? indexFile : "")
        , m_Built(false)
        , m_LoadedFromDisk(false)
    {
        if (!m_IndexPath.empty())
            LoadIndex();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
        if (m_LoadedFromDisk) {
            m_Store = make_shared<PostingStore>();
            m_BlockTable.Reset(PostingBlockCacheSlots(0));
            m_Executor = IndexSearchExecutor(m_Store);
            m_Built = false;
            m_LoadedFromDisk = false;
        }
        return make_shared<AdvancedIndexWriter>(m_Store);
    }

    /*
    * Build() — delegates to IndexSerializer::BuildBlocksForContext so the
    * packing logic (continuation fix, PageSkipList) is in one place.
    */
    void Build()
    {
        if (m_Built) return;

        auto br = IndexSerializer::BuildBlocksForContext(*m_Store);

        size_t n = br.blocks.size();
        m_BlockTable.ResizeCache(PostingBlockCacheSlots(n));

        for (size_t i = 0; i < n; ++i)
            m_BlockTable.InsertBlock(static_cast<uint32_t>(i), &br.blocks[i]);

        m_BlockTable.SetTermHeaderTable(std::move(br.term_directory),
                        std::move(br.term_header_blocks));
        m_BlockTable.SetPageSkipData(std::move(br.pageskip));

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
        return BuildIndexReader(evalTree->root);
    }

    Tokenizer*           GetTokenizer() { return &m_Tokenizer; }
    IndexSearchCompiler* GetCompiler()  { return &m_Compiler; }
    IndexSearchExecutor* GetExecutor()  { return new IndexSearchExecutor(m_Store); }

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
        reader->Open(streamKey, &m_BlockTable, m_Store->DocFreq(streamKey));
        return reader;
    }

    PostingStore* GetStore() { return m_Store.get(); }

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

        return true;
    }

    void LoadIndex()
    {
        if (m_IndexPath.empty()) return;

        m_Store = make_shared<PostingStore>();
        std::vector<TermDirectoryEntry>          term_directory;
        std::vector<TermHeaderBlock>             term_header_blocks;
        std::vector<uint64_t>                    pageskip;
        uint64_t blocks_offset = 0;
        uint64_t num_blocks = 0;

        if (!IndexSerializer::Load(*m_Store, m_IndexPath.c_str(),
                                   &term_directory, &term_header_blocks,
                                   &blocks_offset, &pageskip, &num_blocks))
            return;

        m_BlockTable.Reset(PostingBlockCacheSlots(num_blocks));
        m_BlockTable.ReserveBlockMap(static_cast<uint32_t>(std::min<uint64_t>(num_blocks, UINT32_MAX)));
        m_BlockTable.SetTermHeaderTable(std::move(term_directory),
                                        std::move(term_header_blocks));
        m_BlockTable.SetPageSkipData(std::move(pageskip));

        auto fm = make_shared<FileBlockManager>(sizeof(IndexBlock), blocks_offset);
        if (fm->open(m_IndexPath.c_str()))
            m_BlockTable.SetFileManager(std::move(fm));

        m_Executor = IndexSearchExecutor(m_Store);
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

    static uint32_t PostingBlockCacheSlots(uint64_t postingBlockCount)
    {
        static constexpr uint64_t MIN_CACHE_BYTES = 64ull * 1024ull * 1024ull;
        static constexpr uint64_t MAX_CACHE_BYTES = 1024ull * 1024ull * 1024ull;
        uint64_t minSlots = MIN_CACHE_BYTES / sizeof(IndexBlock);
        uint64_t maxSlots = MAX_CACHE_BYTES / sizeof(IndexBlock);
        uint64_t wanted = std::max<uint64_t>(postingBlockCount, minSlots);
        wanted = std::min<uint64_t>(wanted, maxSlots);
        return static_cast<uint32_t>(std::max<uint64_t>(wanted, 1));
    }

    void EnsureBuilt()
    {
        if (!m_Built)
            Build();
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
                         m_Store->DocFreq(termNode->stream_key));
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
};

#endif
