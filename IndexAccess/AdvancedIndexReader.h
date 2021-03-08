#ifndef ADVANCEDINDEXREADER_H__
#define ADVANCEDINDEXREADER_H__

#include <stdint.h>

#include <boost/shared_ptr.hpp>

#include "BlockTable.h"
#include "IndexReader.h"

inline uint64_t Decode(unsigned char *) {
    return 0;
}

class IndexContext;

class AdvancedIndexReader : public IndexReader 
{
    public:
        AdvancedIndexReader();
        virtual void GoNext();
        virtual void GoUntil(uint64_t target = 0, uint64_t limit = 0);
        virtual bool IsEnd();
        virtual uint64_t GetDocumentID();
        virtual void Close();
    private:
        boost::shared_ptr<struct IndexBlock> m_IndexBlock;
        boost::shared_ptr<IndexContext> m_IndexContext;
        uint32_t m_BlockSeqNumber;
        unsigned char * m_EncodedData;

};

#endif