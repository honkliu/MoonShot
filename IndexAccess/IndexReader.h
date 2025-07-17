#ifndef INDEXREADER_H__
#define INDEXREADER_H__

#include <stdio.h>
#include <cstdint>

class IndexReader
{
    public:
        IndexReader(const IndexReader&) = delete;

        virtual void GoNext() = 0;
        virtual void GoUntil(uint64_t target, uint64_t limit) = 0;
        virtual bool IsEnd() = 0;
        virtual uint64_t GetDocumentID() = 0;
        virtual void Close() = 0;
        virtual void Open(const char* word) = 0;
    protected:
        IndexReader() = default;
};

#endif