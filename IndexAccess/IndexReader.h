#ifndef INDEXREADER_H__
#define INDEXREADER_H__

#include <stdio.h>
#include <cstdint>

class Bm25Scorer;

class IndexReader
{
    public:
        IndexReader(const IndexReader&) = delete;

        virtual void GoNext() = 0;
        virtual void GoUntil(uint64_t target, uint64_t limit = UINT64_MAX) = 0;
        virtual bool IsEnd() = 0;
        virtual uint64_t GetDocumentID() = 0;

        /*
        * Term frequency for the current document.
        */
        virtual uint32_t GetTermFreq() { return 1u; }

        /*
        * BM25 contribution of this node for the current document.
        * doc_len is the total token count for the current document.
        */
        virtual float GetBM25Score(const Bm25Scorer& /*scorer*/,
                                   uint32_t          /*doc_len*/) { return 0.0f; }

        virtual void Close() = 0;
        virtual void Open(const char* word) = 0;

    protected:
        IndexReader() = default;
};

#endif
