#ifndef ADVANCEDINDEXREADER_H__
#define ADVANCEDINDEXREADER_H__

#include <stdint.h>

#include <boost/shared_ptr.hpp>

#include "BlockTable.h"
#include "IndexReader.h"
inline uint64_t Decode(unsigned char *) {
    return 0;
}
class AdvancedIndexReader : IndexReader 
{
    public:
        AdvancedIndexReader();

        static IndexReader * GetReader(const uint8_t * p_token, uint32_t token_len, IndexBlockTable * p_table)
        virtual void GoNext();
        virtual void GoUntil(uint64_t target = 0, uint64_t limit = 0);
        virtual bool IsEnd();
        virtual uint64_t GetDocumentID();
    private:
        boost::shared_ptr<struct IndexBlock> m_IndexBlock;
        boost::shared_ptr<IndexBlockTable> m_IndexBlockTable;

        uint32_t m_BlockSeqNumber;
        unsigned char * m_EncodedData;

};

#endif