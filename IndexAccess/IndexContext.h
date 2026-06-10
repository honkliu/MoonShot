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

using std::shared_ptr;
using std::make_shared;
using std::string;

/*
* Central factory: owns the PostingStore and creates writers, readers,
* and executors on demand.
*
* Construction
*   IndexContext()                   — in-memory only
*   IndexContext("cfg", "idx.bin")   — auto-loads idx.bin if it exists
*
* Persistence
*   engine.SaveIndex()               — write to the path given at construction
*   engine.SaveIndex("other.bin")    — write to a different path
*/
class IndexContext
{
public:
    explicit IndexContext(const char* configFile  = "",
                         const char* indexFile    = "")
        : store_(make_shared<PostingStore>())
        , params_(make_shared<ConfigParameters>())
        , indexPath_(indexFile ? indexFile : "")
    {
        if (!indexPath_.empty())
            LoadIndex();
    }

    shared_ptr<IndexWriter> GetWriter()
    {
        return make_shared<AdvancedIndexWriter>(store_);
    }

    /*
    * Open a single-term IndexReader across the default AUT streams.
    */
    shared_ptr<IndexReader> GetReader(const char* term)
    {
        std::string termStr(term);
        std::vector<shared_ptr<IndexReader>> readers;

        for (const char* streamAbbrev : {"A", "U", "T"}) {
            const PostingList* postingList =
                store_->GetPostingList(termStr + streamAbbrev);

            if (postingList)
                readers.push_back(make_shared<TermIndexReader>(postingList));
        }

        if (readers.empty())
            return make_shared<TermIndexReader>(nullptr);

        if (readers.size() == 1)
            return readers[0];

        return make_shared<OrIndexReader>(std::move(readers));
    }

    /*
    * Build an IndexReader tree from a compiled EvalTree.
    */
    shared_ptr<IndexReader> GetReader(EvalTree* evalTree)
    {
        if (!evalTree || evalTree->IsEmpty())
            return make_shared<TermIndexReader>(nullptr);

        return BuildIndexReader(evalTree->root);
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
        if (indexPath_.empty()) return false;
        return IndexSerializer::Save(*store_, indexPath_.c_str());
    }

    bool SaveIndex(const char* path)
    {
        if (!path || !*path) return false;
        indexPath_ = path;
        return IndexSerializer::Save(*store_, path);
    }

    void LoadIndex()
    {
        if (!indexPath_.empty()) {
            store_ = make_shared<PostingStore>();
            IndexSerializer::Load(*store_, indexPath_.c_str());
        }
    }

    void LoadIndex(const char* path)
    {
        if (!path || !*path) return;
        indexPath_ = path;
        store_     = make_shared<PostingStore>();
        IndexSerializer::Load(*store_, path);
    }

private:
    shared_ptr<PostingStore>     store_;
    shared_ptr<ConfigParameters> params_;
    std::string                  indexPath_;

    /*
    * Recursively convert an EvalNode into its matching IndexReader type.
    */
    shared_ptr<IndexReader> BuildIndexReader(const shared_ptr<EvalNode>& node)
    {
        if (!node) return make_shared<TermIndexReader>(nullptr);

        switch (node->GetType()) {

        case NodeType::Term: {
            auto* termNode        = static_cast<TermNode*>(node.get());
            const PostingList* postingList =
                store_->GetPostingList(termNode->stream_key);
            return make_shared<TermIndexReader>(postingList);
        }

        case NodeType::And: {
            auto* andNode = static_cast<AndNode*>(node.get());
            std::vector<shared_ptr<IndexReader>> childReaders;

            for (auto& child : andNode->children) {
                auto childReader = BuildIndexReader(child);
                if (childReader) childReaders.push_back(childReader);
            }

            if (childReaders.empty())
                return make_shared<TermIndexReader>(nullptr);

            if (childReaders.size() == 1)
                return childReaders[0];

            return make_shared<AndIndexReader>(std::move(childReaders));
        }

        case NodeType::Or: {
            auto* orNode = static_cast<OrNode*>(node.get());
            std::vector<shared_ptr<IndexReader>> childReaders;

            for (auto& child : orNode->children) {
                auto childReader = BuildIndexReader(child);
                if (childReader) childReaders.push_back(childReader);
            }

            if (childReaders.empty())
                return make_shared<TermIndexReader>(nullptr);

            if (childReaders.size() == 1)
                return childReaders[0];

            return make_shared<OrIndexReader>(std::move(childReaders));
        }

        case NodeType::Not: {
            auto* notNode    = static_cast<NotNode*>(node.get());
            auto  baseReader = BuildIndexReader(notNode->base);
            auto  exclReader = BuildIndexReader(notNode->exclude);

            if (!baseReader)
                return make_shared<TermIndexReader>(nullptr);

            if (!exclReader)
                return baseReader;

            return make_shared<NotIndexReader>(baseReader, exclReader);
        }

        default:
            return make_shared<TermIndexReader>(nullptr);
        }
    }
};

#endif
