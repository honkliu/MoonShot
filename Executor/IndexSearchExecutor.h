#ifndef INDEXSEARCHEXECUTOR_H__
#define INDEXSEARCHEXECUTOR_H__

#include "EvalExpression.h"
#include "IndexReader.h"
#include "SearchResult.h"

#include <memory>
#include <vector>

class IndexContext;

/*
* Traverses an IndexReader tree in ascending doc_id order, scores each matched
* document, and returns a top-K list. Runtime document statistics come directly
* from IndexContext (header + pinned DocData).
*/
class IndexSearchExecutor
{
public:
    explicit IndexSearchExecutor(const IndexContext* context);

    std::vector<SearchResult> Execute(std::shared_ptr<IndexReader> reader,
                           int topK = 10,
                           const std::vector<float>* vectorQuery = nullptr);

    std::vector<SearchResult> ExecuteBounded(std::shared_ptr<IndexReader> reader,
                                             int topK,
                               uint64_t maxVisitedDocs,
                               const std::vector<float>* vectorQuery = nullptr);

    std::vector<SearchResult> Execute(IndexReader* reader,
                           int topK = 10,
                           const std::vector<float>* vectorQuery = nullptr);

    static void SetScoringParameters(const QueryCompileModeParameters& parameters);

private:
    const IndexContext*          m_Context = nullptr;

    static void SortAndTruncate(std::vector<SearchResult>& results, int topK);
};

#endif
