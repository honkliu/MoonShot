#ifndef ADVANCEDINDEXREADER_H__
#define ADVANCEDINDEXREADER_H__

#include "IndexReader.h"
class AdvancedIndexReader : IndexReader 
{
    public:
        AdvancedIndexReader();
        virtual void GoNext();
        virtual void GoUntil(uint64_t target, uint64_t limit);
        virtual bool IsEnd();
        virtual uint64_t GetDocumentID();
};

#endif