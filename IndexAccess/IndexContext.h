#ifndef INDDEXCONTEXT_H__
#define INDDEXCONTEXT_H__

#include "AdvancedIndexReader.h"
#include "AdvancedIndexWriter.h"
#include "IsrImpl.h"
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
* Central factory; owns the PostingStore and creates all pipeline objects.
*
* Construction
* ─────────────
*   IndexContext()                  — in-memory only
*   IndexContext("cfg", "idx.bin")  — if idx.bin exists, loads it on open
*
* Persistence
* ───────────
*   engine.SaveIndex()              — write index to the path given at construction
*   engine.SaveIndex("other.bin")   — write to a different path
*   IndexContext::LoadIndex("x.bin") already called in constructor if path given
*/
class IndexContext
{
public:
    explicit IndexContext(const char* config_file = "",
                         const char* index_file   = "")
        : store_(make_shared<PostingStore>())
        , params_(make_shared<ConfigParameters>())
        , index_path_(index_file ? index_file : "")
    {
        if (!index_path_.empty()) {
            LoadIndex();
        }
    }

    /* ---- factory methods ------------------------------------------------ */

    shared_ptr<IndexWriter> GetWriter()
    {
        return make_shared<AdvancedIndexWriter>(store_);
    }

    /*
    * Open a single-term ISR across the default AUT streams.
    */
    shared_ptr<IndexReader> GetReader(const char* term)
    {
        std::string s(term);
        std::vector<shared_ptr<IndexReader>> isrs;
        for (const char* abbrev : {"A", "U", "T"}) {
            auto* pl = store_->GetPostingList(s + abbrev);
            if (pl) isrs.push_back(make_shared<TermIsr>(pl));
        }
        if (isrs.empty()) return make_shared<TermIsr>(nullptr);
        if (isrs.size() == 1) return isrs[0];
        return make_shared<OrIsr>(std::move(isrs));
    }

    /*
    * Build an ISR tree from a compiled EvalTree.
    */
    shared_ptr<IndexReader> GetReader(EvalTree* eval_tree)
    {
        if (!eval_tree || eval_tree->IsEmpty())
            return make_shared<TermIsr>(nullptr);
        return BuildIsr(eval_tree->root);
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

    /* ---- persistence ---------------------------------------------------- */

    /*
    * Write the index to disk.
    * Uses the path given at construction; returns false if no path set.
    */
    bool SaveIndex()
    {
        if (index_path_.empty()) return false;
        return IndexSerializer::Save(*store_, index_path_.c_str());
    }

    /*
    * Write the index to an explicit path and update the default path.
    */
    bool SaveIndex(const char* path)
    {
        if (!path || !*path) return false;
        index_path_ = path;
        return IndexSerializer::Save(*store_, path);
    }

    /*
    * Load an index from disk, replacing the current in-memory state.
    */
    void LoadIndex()
    {
        if (!index_path_.empty()) {
            store_ = make_shared<PostingStore>();
            IndexSerializer::Load(*store_, index_path_.c_str());
        }
    }

    void LoadIndex(const char* path)
    {
        if (!path || !*path) return;
        index_path_ = path;
        store_ = make_shared<PostingStore>();
        IndexSerializer::Load(*store_, path);
    }

private:
    shared_ptr<PostingStore>      store_;
    shared_ptr<ConfigParameters>  params_;
    std::string                   index_path_;

    /*
    * Recursively convert an EvalNode to the matching ISR type.
    */
    shared_ptr<IndexReader> BuildIsr(const shared_ptr<EvalNode>& node)
    {
        if (!node) return make_shared<TermIsr>(nullptr);

        switch (node->GetType()) {

        case NodeType::Term: {
            auto* tn = static_cast<TermNode*>(node.get());
            auto* pl = store_->GetPostingList(tn->stream_key);
            return make_shared<TermIsr>(pl);
        }

        case NodeType::And: {
            auto* an = static_cast<AndNode*>(node.get());
            std::vector<shared_ptr<IndexReader>> kids;
            for (auto& c : an->children) {
                auto kid = BuildIsr(c);
                if (kid) kids.push_back(kid);
            }
            if (kids.empty())   return make_shared<TermIsr>(nullptr);
            if (kids.size()==1) return kids[0];
            return make_shared<AndIsr>(std::move(kids));
        }

        case NodeType::Or: {
            auto* on = static_cast<OrNode*>(node.get());
            std::vector<shared_ptr<IndexReader>> kids;
            for (auto& c : on->children) {
                auto kid = BuildIsr(c);
                if (kid) kids.push_back(kid);
            }
            if (kids.empty())   return make_shared<TermIsr>(nullptr);
            if (kids.size()==1) return kids[0];
            return make_shared<OrIsr>(std::move(kids));
        }

        case NodeType::Not: {
            auto* nn = static_cast<NotNode*>(node.get());
            auto base    = BuildIsr(nn->base);
            auto exclude = BuildIsr(nn->exclude);
            if (!base)    return make_shared<TermIsr>(nullptr);
            if (!exclude) return base;
            return make_shared<NotIsr>(base, exclude);
        }

        default:
            return make_shared<TermIsr>(nullptr);
        }
    }
};

#endif
