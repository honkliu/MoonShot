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
    {
        if (!m_IndexPath.empty())
            LoadIndex();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
        return make_shared<AdvancedIndexWriter>(m_Store);
    }

    /*
    * Build() — converts PostingStore entries into IndexBlock pages.
    * Called lazily on first GetReader().
    */
    void Build()
    {
        if (m_Built)
            return;

        const size_t blockCapacity = sizeof(IndexBlock::IB_Data) - 1u;
        uint32_t     blockSeq      = 0;

        for (const auto& [streamKey, postingList] : m_Store->AllPostings()) {
            const auto& bytes = postingList.GetBytes();

            if (bytes.empty())
                continue;

            size_t dataLen   = bytes.size();
            size_t numBlocks = (dataLen + blockCapacity - 1) / blockCapacity;
            if (numBlocks == 0) numBlocks = 1;

            uint32_t firstSeq = blockSeq;

            for (size_t blk = 0; blk < numBlocks; ++blk) {
                size_t offset  = blk * blockCapacity;
                size_t len     = std::min(dataLen - offset, blockCapacity);
                bool   hasMore = (blk + 1 < numBlocks);

                m_BlockTable.InsertBlock(blockSeq,
                                        bytes.data() + offset,
                                        len,
                                        hasMore);
                ++blockSeq;
            }

            m_BlockTable.AddTermMapping(streamKey, firstSeq);
        }

        m_Built = true;
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
    IndexSearchExecutor* GetExecutor()  { return &m_Executor; }

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
        if (m_IndexPath.empty())
            return false;

        return IndexSerializer::Save(*m_Store, m_IndexPath.c_str());
    }

    bool SaveIndex(const char* path)
    {
        if (!path || !*path)
            return false;

        m_IndexPath = path;
        return IndexSerializer::Save(*m_Store, path);
    }

    void LoadIndex()
    {
        if (!m_IndexPath.empty()) {
            m_Store = make_shared<PostingStore>();
            IndexSerializer::Load(*m_Store, m_IndexPath.c_str());
            m_Executor = IndexSearchExecutor(m_Store);
            m_Built = false;
        }
    }

    void LoadIndex(const char* path)
    {
        if (!path || !*path)
            return;

        m_IndexPath = path;
        m_Store     = make_shared<PostingStore>();
        IndexSerializer::Load(*m_Store, path);
        m_Executor = IndexSearchExecutor(m_Store);
        m_Built = false;
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
