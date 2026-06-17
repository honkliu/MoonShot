#include "IndexSearchExecutor.h"

#include "IndexContext.h"

#include <algorithm>

IndexSearchExecutor::IndexSearchExecutor(const IndexContext* context)
    : m_Context(context)
{}

std::vector<SearchResult> IndexSearchExecutor::Execute(std::shared_ptr<IndexReader> reader,
                                                       int topK)
{
    if (!reader || reader->IsEnd())
        return {};

    std::vector<SearchResult> results;

    while (!reader->IsEnd()) {
        uint64_t docId     = reader->GetDocumentID();
        const DocRecord* record = m_Context ? m_Context->GetDocRecord(docId) : nullptr;
        float    score     = reader->GetScore(record)
                           + (record ? DecodeFloat16(record->DR_StaticRankHalf) : 0.0f);

        results.push_back({docId, score, ""});
        reader->GoNext();
    }

    SortAndTruncate(results, topK);
    return results;
}

std::vector<SearchResult> IndexSearchExecutor::Execute(IndexReader* reader, int topK)
{
    return Execute(std::shared_ptr<IndexReader>(reader, [](IndexReader*){}), topK);
}

std::vector<SearchResult> IndexSearchExecutor::ExecutePhased(
        std::shared_ptr<IndexReader> phase1Reader,
        std::shared_ptr<IndexReader> phase2Reader,
        int topK,
        int minResultsForPhase1)
{
    auto results = CollectResults(phase1Reader);

    if ((int)results.size() < minResultsForPhase1 && phase2Reader) {
        auto phase2Results = CollectResults(phase2Reader);
        MergeResults(results, phase2Results);
    }

    SortAndTruncate(results, topK);
    return results;
}

std::vector<SearchResult> IndexSearchExecutor::CollectResults(
    std::shared_ptr<IndexReader>& reader)
{
    std::vector<SearchResult> results;

    if (!reader)
        return results;

    while (!reader->IsEnd()) {
        uint64_t docId     = reader->GetDocumentID();
        const DocRecord* record = m_Context ? m_Context->GetDocRecord(docId) : nullptr;
        float    score     = reader->GetScore(record)
                           + (record ? DecodeFloat16(record->DR_StaticRankHalf) : 0.0f);

        results.push_back({docId, score, ""});
        reader->GoNext();
    }

    return results;
}

void IndexSearchExecutor::SortAndTruncate(std::vector<SearchResult>& results, int topK)
{
    std::sort(results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b){
            return a.score > b.score;
        });

    if (topK > 0 && (int)results.size() > topK)
        results.resize(static_cast<size_t>(topK));
}

void IndexSearchExecutor::MergeResults(std::vector<SearchResult>&       base,
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
