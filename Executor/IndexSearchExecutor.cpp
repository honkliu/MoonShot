#include "IndexSearchExecutor.h"

#include "IndexContext.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

float g_StaticWeight = 1.0f;
float g_QualityWeight = 1.0f;
float g_AuthorityWeight = 0.5f;
float g_SpamPenalty = 2.0f;

float DocDataScore(const DocDataEntry& entry)
{
    // Fitted formula from the 2026-06-30 full-feature GPU sweep:
    // score = 1.8*weak + 0.1*dump_bigram + 1.0*static + 0.0*prior
    //       + 1.0*quality + 0.5*authority - 2.0*spam + 0.0*both + 0.0*bigram_only.
    // Scheme A folds stream-side terms into leaf span weights:
    // unigram span weight = 1.8; bigram span weight = 0.1 * dump_bigram_span_weight(2.0) = 0.2.
    return g_StaticWeight * entry.DDE_StaticRank
        + g_QualityWeight * entry.DDE_QualityScore
        + g_AuthorityWeight * entry.DDE_AuthorityScore
        - g_SpamPenalty * entry.DDE_SpamScore;
}

}

IndexSearchExecutor::IndexSearchExecutor(const IndexContext* context)
    : m_Context(context)
{}

void IndexSearchExecutor::SetFittedDocWeights(float staticWeight,
                                              float qualityWeight,
                                              float authorityWeight,
                                              float spamPenalty)
{
    g_StaticWeight = staticWeight;
    g_QualityWeight = qualityWeight;
    g_AuthorityWeight = authorityWeight;
    g_SpamPenalty = spamPenalty;
}

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
        float    score     = reader->GetScore(entry) + DocDataScore(*entry);

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
        float    score     = reader->GetScore(entry) + DocDataScore(*entry);

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
