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
    {
        if (!m_IndexPath.empty())
            LoadIndex();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
        return make_shared<AdvancedIndexWriter>(m_Store);
    }

    /*
    * Build() — packs PostingStore entries into multi-term IndexBlocks,
    * sorted alphabetically, and populates the SubIndex.
    * Called lazily on first GetReader() for in-memory indexes.
    * Not called when the index was loaded from disk (m_Built = true).
    */
    void Build()
    {
        if (m_Built)
            return;

        std::vector<std::pair<std::string, const PostingList*>> sorted;
        sorted.reserve(m_Store->AllPostings().size());
        for (const auto& [k, pl] : m_Store->AllPostings())
            sorted.push_back({k, &pl});
        std::sort(sorted.begin(), sorted.end());

        constexpr size_t   DATA_CAP = sizeof(IndexBlock::IB_Data) - 1u;
        constexpr uint16_t CONT     = BLOCK_CONTINUATION_MARKER;

        IndexBlock cur = {};
        uint8_t*   wptr  = cur.IB_Data;
        uint8_t*   wend  = cur.IB_Data + DATA_CAP;
        uint32_t   seq   = 0;
        bool       fresh = true;

        auto flush = [&](bool has_more) {
            cur.IB_Header = static_cast<uint64_t>(seq);
            if (has_more) cur.IB_Header |= IB_HEADER_HAS_MORE;
            if (!has_more && wptr + 2 <= wend + 1) {
                *wptr++ = 0; *wptr = 0;
            }
            m_BlockTable.InsertBlock(seq, &cur);
            ++seq;
            cur   = {};
            wptr  = cur.IB_Data;
            fresh = true;
        };

        for (const auto& [term, pl] : sorted) {
            const auto& bytes = pl->GetBytes();
            if (bytes.empty()) continue;

            uint16_t kl       = static_cast<uint16_t>(term.size());
            uint32_t freq     = pl->doc_freq();
            size_t   hdr_size = 2u + kl + 4u + 4u;

            if (static_cast<size_t>(wend - wptr) < hdr_size + 1u)
                flush(false);

            if (fresh) {
                m_BlockTable.AddSubIndexEntry(term, seq);
                fresh = false;
            }

            const uint8_t* src       = bytes.data();
            size_t         remaining = bytes.size();
            size_t         data_space = static_cast<size_t>(wend - wptr) - hdr_size;
            size_t         data_here  = std::min(remaining, data_space);
            bool           has_more   = (data_here < remaining);

            std::memcpy(wptr, &kl,   2); wptr += 2;
            std::memcpy(wptr, term.c_str(), kl); wptr += kl;
            std::memcpy(wptr, &freq, 4); wptr += 4;
            uint32_t dl = static_cast<uint32_t>(data_here);
            std::memcpy(wptr, &dl,   4); wptr += 4;
            std::memcpy(wptr, src,   data_here); wptr += data_here;
            src       += data_here;
            remaining -= data_here;

            if (has_more) {
                flush(true);
                while (remaining > 0) {
                    std::memcpy(wptr, &CONT, 2); wptr += 2;
                    size_t cont_here = std::min(remaining,
                                                static_cast<size_t>(wend - wptr));
                    bool   more_cont = (cont_here < remaining);
                    std::memcpy(wptr, src, cont_here);
                    wptr      += cont_here;
                    src       += cont_here;
                    remaining -= cont_here;
                    flush(more_cont);
                }
            }
        }
        if (!fresh || wptr != cur.IB_Data)
            flush(false);

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
        std::vector<SubIndexEntry> dummy_si;
        uint64_t blocks_offset = 0;
        PostingStore tmp;
        IndexSerializer::Load(tmp, path, &dummy_si, &blocks_offset);

        auto fm = make_shared<FileBlockManager>(sizeof(IndexBlock), blocks_offset);
        if (fm->open(path))
            m_BlockTable.SetFileManager(std::move(fm));

        return true;
    }

    void LoadIndex()
    {
        if (m_IndexPath.empty()) return;

        m_Store = make_shared<PostingStore>();
        std::vector<SubIndexEntry> subindex;
        uint64_t blocks_offset = 0;

        if (!IndexSerializer::Load(*m_Store, m_IndexPath.c_str(),
                                   &subindex, &blocks_offset))
            return;

        m_BlockTable.SetSubIndex(std::move(subindex));

        auto fm = make_shared<FileBlockManager>(sizeof(IndexBlock), blocks_offset);
        if (fm->open(m_IndexPath.c_str()))
            m_BlockTable.SetFileManager(std::move(fm));

        m_Executor = IndexSearchExecutor(m_Store);
        m_Built    = true;
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
