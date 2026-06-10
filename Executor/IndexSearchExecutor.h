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
* Traverses an IndexReader tree in ascending doc_id order, scores each
* matched document with BM25 + doc importance, and returns a top-K list.
*/
class IndexSearchExecutor
{
public:
    explicit IndexSearchExecutor(std::shared_ptr<PostingStore> store)
        : store_(std::move(store))
    {}

    std::vector<SearchResult> Execute(std::shared_ptr<IndexReader> reader,
                                      int topK = 10)
    {
        if (!reader || reader->IsEnd())
            return {};

        Bm25Scorer scorer(store_->TotalDocs(), store_->AvgDocLen());
        std::vector<SearchResult> results;

        while (!reader->IsEnd()) {
            uint64_t docId     = reader->GetDocumentID();
            uint32_t docLength = store_->GetDocLen(docId);
            float    score     = reader->GetBM25Score(scorer, docLength)
                               + store_->GetDocImportance(docId);

            results.push_back({docId, score, ""});
            reader->GoNext();
        }

        SortAndTruncate(results, topK);
        return results;
    }

    std::vector<SearchResult> Execute(IndexReader* reader, int topK = 10)
    {
        return Execute(std::shared_ptr<IndexReader>(reader,
                       [](IndexReader*){}), topK);
    }

    /*
    * Two-phase search: run phase1 first; if fewer than minResultsForPhase1
    * are found, run phase2 and merge the result sets.
    */
    std::vector<SearchResult> ExecutePhased(
            std::shared_ptr<IndexReader> phase1Reader,
            std::shared_ptr<IndexReader> phase2Reader,
            int topK                = 10,
            int minResultsForPhase1 = 3)
    {
        Bm25Scorer scorer(store_->TotalDocs(), store_->AvgDocLen());

        auto results = CollectResults(phase1Reader, scorer);

        if ((int)results.size() < minResultsForPhase1 && phase2Reader) {
            auto phase2Results = CollectResults(phase2Reader, scorer);
            MergeResults(results, phase2Results);
        }

        SortAndTruncate(results, topK);
        return results;
    }

    // Kept for backward compat.
    void Execute() {}
    void Execute(EvalTree* /*evalTree*/) {}

private:
    std::shared_ptr<PostingStore> store_;

    std::vector<SearchResult> CollectResults(
            std::shared_ptr<IndexReader>& reader,
            const Bm25Scorer&             scorer)
    {
        std::vector<SearchResult> results;

        if (!reader)
            return results;

        while (!reader->IsEnd()) {
            uint64_t docId     = reader->GetDocumentID();
            uint32_t docLength = store_->GetDocLen(docId);
            float    score     = reader->GetBM25Score(scorer, docLength)
                               + store_->GetDocImportance(docId);

            results.push_back({docId, score, ""});
            reader->GoNext();
        }

        return results;
    }

    static void SortAndTruncate(std::vector<SearchResult>& results, int topK)
    {
        std::sort(results.begin(), results.end(),
            [](const SearchResult& a, const SearchResult& b){
                return a.score > b.score;
            });

        if (topK > 0 && (int)results.size() > topK)
            results.resize(static_cast<size_t>(topK));
    }

    static void MergeResults(std::vector<SearchResult>&       base,
                              const std::vector<SearchResult>& additional)
    {
        for (auto& result : additional) {
            bool isDuplicate = false;

            for (auto& existing : base) {
                if (existing.doc_id == result.doc_id) {
                    existing.score = std::max(existing.score, result.score);
                    isDuplicate    = true;
                    break;
                }
            }

            if (!isDuplicate)
                base.push_back(result);
        }
    }
};

#endif
