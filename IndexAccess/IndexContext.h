#ifndef INDDEXCONTEXT_H__
#define INDDEXCONTEXT_H__

#include "AdvancedIndexReader.h"
#include "AdvancedIndexWriter.h"
#include "IndexReaderImpl.h"
#include "PostingStore.h"
#include "IndexSerializer.h"
#include "EvalExpression.h"
#include "IndexSearchExecutor.h"
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
        : store_(make_shared<PostingStore>())
        , params_(make_shared<ConfigParameters>())
        , blockTable_(512)
        , indexPath_(indexFile ? indexFile : "")
        , built_(false)
    {
        if (!indexPath_.empty())
            LoadIndex();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
        return make_shared<AdvancedIndexWriter>(store_);
    }

    /*
    * Build() — converts PostingStore entries into IndexBlock pages.
    * Called lazily on first GetReader().
    */
    void Build()
    {
        if (built_)
            return;

        const size_t blockCapacity = sizeof(IndexBlock::IB_Data) - 1u;
        uint32_t     blockSeq      = 0;

        for (const auto& [streamKey, postingList] : store_->AllPostings()) {
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

                blockTable_.InsertBlock(blockSeq,
                                        bytes.data() + offset,
                                        len,
                                        hasMore);
                ++blockSeq;
            }

            blockTable_.AddTermMapping(streamKey, firstSeq);
        }

        built_ = true;
    }

    /*
    * Open a single-term reader across the default AUT streams.
    * Returns OrIndexReader over the non-empty stream readers.
    */
    shared_ptr<IndexReader> GetReader(const char* term)
    {
        EnsureBuilt();

        std::string termStr(term);
        std::vector<shared_ptr<IndexReader>> readers;

        for (const char* abbrev : {"A", "U", "T"}) {
            std::string streamKey = termStr + abbrev;
            auto reader = make_shared<AdvancedIndexReader>();
            reader->Open(streamKey.c_str(), &blockTable_,
                         store_->DocFreq(streamKey));

            if (!reader->IsEnd())
                readers.push_back(reader);
        }

        if (readers.empty()) {
            auto empty = make_shared<AdvancedIndexReader>();
            return empty;
        }

        if (readers.size() == 1)
            return readers[0];

        return make_shared<OrIndexReader>(std::move(readers));
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

    /* Open a reader for one specific stream key (e.g. "raceA", "carT"). */
    shared_ptr<IndexReader> GetStreamReader(const char* streamKey)
    {
        EnsureBuilt();
        auto reader = make_shared<AdvancedIndexReader>();
        reader->Open(streamKey, &blockTable_, store_->DocFreq(streamKey));
        return reader;
    }

    template<typename T>
    shared_ptr<IndexReader> GetReader(Embeddings<T>* /*embedding*/)
    {
        return nullptr;
    }

    IndexSearchExecutor* GetExecutor()
    {
        return new IndexSearchExecutor(store_);
    }

    PostingStore* GetStore() { return store_.get(); }

    bool SaveIndex()
    {
        if (indexPath_.empty())
            return false;

        return IndexSerializer::Save(*store_, indexPath_.c_str());
    }

    bool SaveIndex(const char* path)
    {
        if (!path || !*path)
            return false;

        indexPath_ = path;
        return IndexSerializer::Save(*store_, path);
    }

    void LoadIndex()
    {
        if (!indexPath_.empty()) {
            store_ = make_shared<PostingStore>();
            IndexSerializer::Load(*store_, indexPath_.c_str());
            built_ = false;
        }
    }

    void LoadIndex(const char* path)
    {
        if (!path || !*path)
            return;

        indexPath_ = path;
        store_     = make_shared<PostingStore>();
        IndexSerializer::Load(*store_, path);
        built_ = false;
    }

private:
    shared_ptr<PostingStore>     store_;
    shared_ptr<ConfigParameters> params_;
    IndexBlockTable              blockTable_;
    std::string                  indexPath_;
    bool                         built_;

    void EnsureBuilt()
    {
        if (!built_)
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
                         &blockTable_,
                         store_->DocFreq(termNode->stream_key));
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
