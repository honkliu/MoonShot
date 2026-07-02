#include "IndexSearchExecutor.h"

#include "IndexContext.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

QueryCompileModeParameters g_ScoringParameters = kWeakAndBigramParameters;

float DocDataPrior(const DocDataEntry& entry)
{
    const float docLength = static_cast<float>(std::max<uint32_t>(1, entry.DDE_DocLength));
    static constexpr float TargetLogLength = 6.0f;
    static constexpr float Width = 4.0f;
    static constexpr float Weight = 0.15f;
    const float distance = std::abs(std::log2(docLength) - TargetLogLength);
    const float lengthQuality = std::max(0.0f, 1.0f - distance / Width);
    return Weight * lengthQuality
        + 0.10f * entry.DDE_QualityScore
        + 0.05f * entry.DDE_AuthorityScore
        - 0.10f * entry.DDE_SpamScore;
}

float DocDataScore(const DocDataEntry& entry)
{
    // Fitted formula from the 2026-06-30 full-feature GPU sweep:
    // score = 1.8*weak + 0.1*dump_bigram + 1.0*static + 0.0*prior
    //       + 1.0*quality + 0.5*authority - 2.0*spam + 0.0*both + 0.0*bigram_only.
    // Scheme A folds stream-side terms into leaf span weights:
    // unigram span weight = 1.8; bigram span weight = 0.1 * dump_bigram_span_weight(2.0) = 0.2.
    return g_ScoringParameters.QMP_StaticWeight * entry.DDE_StaticRank
        + g_ScoringParameters.QMP_PriorWeight * DocDataPrior(entry)
        + g_ScoringParameters.QMP_QualityWeight * entry.DDE_QualityScore
        + g_ScoringParameters.QMP_AuthorityWeight * entry.DDE_AuthorityScore
        - g_ScoringParameters.QMP_SpamPenalty * entry.DDE_SpamScore;
}

float VectorScoreFeature(const DocDataEntry& entry, const std::vector<float>* query)
{
    if (!query || query->size() != DOC_VECTOR_DIM || entry.DDE_VectorFlags == 0 || entry.DDE_VectorDim != DOC_VECTOR_DIM)
        return 0.0f;

    float dot = 0.0f;
    float nq = 0.0f;
    float nd = 0.0f;
    for (size_t i = 0; i < DOC_VECTOR_DIM; ++i) {
        const float q = (*query)[i];
        const float d = static_cast<float>(entry.DDE_VectorData[i]) / 128.0f;
        dot += q * d;
        nq += q * q;
        nd += d * d;
    }
    if (nq <= 0.0f || nd <= 0.0f)
        return 0.0f;

    const float cosine = dot / (std::sqrt(nq) * std::sqrt(nd));
    return g_ScoringParameters.QMP_CosineWeight * cosine;
}

}

IndexSearchExecutor::IndexSearchExecutor(const IndexContext* context)
    : m_Context(context)
{}

void IndexSearchExecutor::SetScoringParameters(const QueryCompileModeParameters& parameters)
{
    g_ScoringParameters = parameters;
}

std::vector<SearchResult> IndexSearchExecutor::Execute(std::shared_ptr<IndexReader> reader,
                                                       int topK,
                                                       const std::vector<float>* vectorQuery)
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

    float score = 0.0;

    while (!reader->IsEnd()) {
        uint64_t docId     = reader->GetDocumentID();

        const DocDataEntry* entry = m_Context->GetDocDataEntry(docId);

        score = reader->GetScore(entry) + DocDataScore(*entry) + VectorScoreFeature(*entry, vectorQuery);

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
        uint64_t maxVisitedDocs,
        const std::vector<float>* vectorQuery)
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
        float    score     = reader->GetScore(entry) + DocDataScore(*entry) + VectorScoreFeature(*entry, vectorQuery);

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

std::vector<SearchResult> IndexSearchExecutor::Execute(IndexReader* reader,
                                                       int topK,
                                                       const std::vector<float>* vectorQuery)
{
    return Execute(std::shared_ptr<IndexReader>(reader, [](IndexReader*){}), topK, vectorQuery);
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
