#ifndef INDEXREADER_H__
#define INDEXREADER_H__

#include <stdio.h>

class IndexReader
{
    public:
        IndexReader(const IndexReader&) = delete;

        virtual void GoNext() = 0;
        virtual void GoUntil(uint64_t target, uint64_t limit) = 0;
        virtual bool IsEnd() = 0;
        virtual uint64_t GetDocumentID() = 0;
        virtual void Close() = 0;
    protected:
        IndexReader() = default;
};

#endif