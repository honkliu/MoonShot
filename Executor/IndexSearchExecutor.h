#ifndef INDEXSEARCHEXECUTOR_H__
#define INDEXSEARCHEXECUTOR_H__

#include "EvalExpression.h"
#include "IndexReader.h"
#include "PostingStore.h"
#include "Bm25Scorer.h"
#include "SearchResult.h"

#include <memory>
#include <vector>
#include <algorithm>

/*
* Traverses an ISR tree in doc_id order, scores each matched document
* with BM25 + doc importance, and returns a top-K result list.
*/
class IndexSearchExecutor
{
public:
    explicit IndexSearchExecutor(std::shared_ptr<PostingStore> store)
        : store_(std::move(store))
    {}

    std::vector<SearchResult> Execute(std::shared_ptr<IndexReader> reader,
                                      int top_k = 10)
    {
        if (!reader || reader->IsEnd()) return {};

        Bm25Scorer scorer(store_->TotalDocs(), store_->AvgDocLen());
        std::vector<SearchResult> results;

        while (!reader->IsEnd()) {
            uint64_t doc_id  = reader->GetDocumentID();
            uint32_t doc_len = store_->GetDocLen(doc_id);
            float    score   = reader->GetBM25Score(scorer, doc_len)
                             + store_->GetDocImportance(doc_id);
            results.push_back({doc_id, score, ""});
            reader->GoNext();
        }

        SortAndTruncate(results, top_k);
        return results;
    }

    std::vector<SearchResult> Execute(IndexReader* reader, int top_k = 10)
    {
        return Execute(std::shared_ptr<IndexReader>(reader,
                       [](IndexReader*){}), top_k);
    }

    /*
    * Two-phase search: run phase1 first; if fewer than min_results
    * are found, run phase2 and merge the result sets.
    */
    std::vector<SearchResult> ExecutePhased(
            std::shared_ptr<IndexReader> phase1_reader,
            std::shared_ptr<IndexReader> phase2_reader,
            int top_k = 10,
            int min_results_phase1 = 3)
    {
        Bm25Scorer scorer(store_->TotalDocs(), store_->AvgDocLen());

        auto results = CollectResults(phase1_reader, scorer);

        if ((int)results.size() < min_results_phase1 && phase2_reader) {
            auto r2 = CollectResults(phase2_reader, scorer);
            MergeResults(results, r2);
        }

        SortAndTruncate(results, top_k);
        return results;
    }

    // kept for backward compat
    void Execute() {}
    void Execute(EvalTree* /*eval_tree*/) {}

private:
    std::shared_ptr<PostingStore> store_;

    std::vector<SearchResult> CollectResults(
            std::shared_ptr<IndexReader>& reader,
            const Bm25Scorer& scorer)
    {
        std::vector<SearchResult> out;
        if (!reader) return out;
        while (!reader->IsEnd()) {
            uint64_t doc_id  = reader->GetDocumentID();
            uint32_t doc_len = store_->GetDocLen(doc_id);
            float    score   = reader->GetBM25Score(scorer, doc_len)
                             + store_->GetDocImportance(doc_id);
            out.push_back({doc_id, score, ""});
            reader->GoNext();
        }
        return out;
    }

    static void SortAndTruncate(std::vector<SearchResult>& v, int top_k)
    {
        std::sort(v.begin(), v.end(),
            [](const SearchResult& a, const SearchResult& b){
                return a.score > b.score;
            });
        if (top_k > 0 && (int)v.size() > top_k)
            v.resize(static_cast<size_t>(top_k));
    }

    static void MergeResults(std::vector<SearchResult>& base,
                              const std::vector<SearchResult>& additional)
    {
        for (auto& r : additional) {
            bool dup = false;
            for (auto& b : base) {
                if (b.doc_id == r.doc_id) {
                    b.score = std::max(b.score, r.score);
                    dup = true;
                    break;
                }
            }
            if (!dup) base.push_back(r);
        }
    }
};

#endif
