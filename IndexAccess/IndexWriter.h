#ifndef INDEXWRITER_H__
#define INDEXWRITER_H__

#include <stdio.h>
#include <vector>
#include <string>
#include <cstdint>

/*
* IndexWriter — base interface for writing tokens into the index.
*
* An IndexWriter or IndexReader represents a table name.
* After it is initialised it points to a table or index name such that:
*   "T_word": Doc1, Doc2, Doc3
*   "A_word": Doc5, Doc7, Doc9
*   "B_word": Doc1, Doc2, Doc3, Doc4
*/
class IndexWriter
{
    public:
        IndexWriter(const IndexWriter&) = delete;

        /*
        * Index a list of tokens for documentId under the given stream.
        * stream: "Title", "Body", "Anchor", "URL", "Meta"
        *         (or single-char abbreviations T, B, A, U, M)
        */
        virtual void Write(const std::vector<std::string>& words,
                           uint64_t                    documentId,
                           const char*                 postingType) {}

        /*
        * Assign a pre-computed quality score to a document.
        * Higher score lifts the document during BM25 ranking.
        * Typical source: PageRank, domain authority, or a learned model.
        */
        virtual void SetDocImportance(uint64_t /*doc_id*/, float /*score*/) {}

        /* Attach a vector embedding for ANN/vector search. */
        virtual void SetDocVector(uint64_t /*doc_id*/, std::vector<float> /*vector*/) {}

    protected:
        IndexWriter() = default;
};

#endif
