#include "IndexSearchExecutor.h"

#include "IndexContext.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

float DocDataPrior(const DocDataEntry& entry)
{
    const float docLength = static_cast<float>(std::max<uint32_t>(1, entry.DDE_DocLength));
    static constexpr float TargetLogLength = 6.0f; // about 64 tokens
    static constexpr float Width = 4.0f;
    static constexpr float Weight = 0.15f;
    const float distance = std::abs(std::log2(docLength) - TargetLogLength);
    const float lengthQuality = std::max(0.0f, 1.0f - distance / Width);
    return Weight * lengthQuality
        + 0.10f * entry.DDE_QualityScore
        + 0.05f * entry.DDE_AuthorityScore
        - 0.10f * entry.DDE_SpamScore;
}

}

IndexSearchExecutor::IndexSearchExecutor(const IndexContext* context)
    : m_Context(context)
{}

std::vector<SearchResult> IndexSearchExecutor::Execute(std::shared_ptr<IndexReader> reader,
                                                       int topK)
{
    if (!reader || reader->IsEnd())
        return {};

    std::vector<SearchResult> results;
    const bool boundedTopK = topK > 0;
    const size_t heapLimit = boundedTopK ? static_cast<size_t>(topK) : 0;
    if (boundedTopK)
        results.reserve(heapLimit);

    auto worseScoreFirst = [](const SearchResult& a, const SearchResult& b) {
        return a.score > b.score;
    };

    while (!reader->IsEnd()) {
        uint64_t docId     = reader->GetDocumentID();
        assert(m_Context);
        const DocDataEntry* entry = m_Context->GetDocDataEntry(docId);
        assert(entry);
        float    score     = reader->GetScore(entry)
                   + entry->DDE_StaticRank
                   + DocDataPrior(*entry);

        SearchResult result{MakeReaderDocumentID(docId, reader->GetSourceMask()), score, ""};
        if (!boundedTopK) {
            results.push_back(std::move(result));
        } else if (results.size() < heapLimit) {
            results.push_back(std::move(result));
            std::push_heap(results.begin(), results.end(), worseScoreFirst);
        } else if (score > results.front().score) {
            std::pop_heap(results.begin(), results.end(), worseScoreFirst);
            results.back() = std::move(result);
            std::push_heap(results.begin(), results.end(), worseScoreFirst);
        }
        reader->GoNext();
    }

    SortAndTruncate(results, topK);
    return results;
}

std::vector<SearchResult> IndexSearchExecutor::ExecuteBounded(
        std::shared_ptr<IndexReader> reader,
        int topK,
        uint64_t maxVisitedDocs)
{
    if (!reader || reader->IsEnd() || maxVisitedDocs == 0)
        return {};

    std::vector<SearchResult> results;
    const bool boundedTopK = topK > 0;
    const size_t heapLimit = boundedTopK ? static_cast<size_t>(topK) : 0;
    if (boundedTopK)
        results.reserve(heapLimit);

    auto worseScoreFirst = [](const SearchResult& a, const SearchResult& b) {
        return a.score > b.score;
    };

    uint64_t visited = 0;
    while (!reader->IsEnd() && visited < maxVisitedDocs) {
        uint64_t docId     = reader->GetDocumentID();
        assert(m_Context);
        const DocDataEntry* entry = m_Context->GetDocDataEntry(docId);
        assert(entry);
        float    score     = reader->GetScore(entry)
                   + entry->DDE_StaticRank
                   + DocDataPrior(*entry);

        SearchResult result{MakeReaderDocumentID(docId, reader->GetSourceMask()), score, ""};
        if (!boundedTopK) {
            results.push_back(std::move(result));
        } else if (results.size() < heapLimit) {
            results.push_back(std::move(result));
            std::push_heap(results.begin(), results.end(), worseScoreFirst);
        } else if (score > results.front().score) {
            std::pop_heap(results.begin(), results.end(), worseScoreFirst);
            results.back() = std::move(result);
            std::push_heap(results.begin(), results.end(), worseScoreFirst);
        }
        ++visited;
        reader->GoNext();
    }

    SortAndTruncate(results, topK);
    return results;
}

std::vector<SearchResult> IndexSearchExecutor::Execute(IndexReader* reader, int topK)
{
    return Execute(std::shared_ptr<IndexReader>(reader, [](IndexReader*){}), topK);
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
