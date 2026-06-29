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
                                      int topK = 10);

        std::vector<SearchResult> ExecuteBounded(std::shared_ptr<IndexReader> reader,
                                                                                         int topK,
                                                                                         uint64_t maxVisitedDocs);

    std::vector<SearchResult> Execute(IndexReader* reader, int topK = 10);

    std::vector<SearchResult> ExecutePhased(
            std::shared_ptr<IndexReader> phase1Reader,
            std::shared_ptr<IndexReader> phase2Reader,
            int topK                = 10,
            int minResultsForPhase1 = 3);

    void Execute() {}
    void Execute(EvalTree* /*evalTree*/) {}

private:
    const IndexContext*          m_Context = nullptr;

    std::vector<SearchResult> CollectResults(
            std::shared_ptr<IndexReader>& reader);

    static void SortAndTruncate(std::vector<SearchResult>& results, int topK);

    static void MergeResults(std::vector<SearchResult>&       base,
                             const std::vector<SearchResult>& additional);
};

#endif
