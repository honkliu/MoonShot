#ifndef ADVANCEDINDEXREADER_H__
#define ADVANCEDINDEXREADER_H__

#include <stdint.h>
#include <string>
#include <memory>

#include "BlockTable.h"
#include "IndexReader.h"
#include "UnifiedDecoder.h"

inline uint64_t Decode(unsigned char *) {
    return 0;
}

class IndexContext;

/*
* Concrete IndexReader backed by the block-based on-disk index.
* Uses UnifiedDecoder to walk the VarByte-compressed posting list
* stored inside an IndexBlock.
*/
class AdvancedIndexReader : public IndexReader
{
    public:
        AdvancedIndexReader() = default;

        ~AdvancedIndexReader()
        {
            delete[] m_Word;
        }

        virtual void GoNext();
        virtual void GoUntil(uint64_t target = 0, uint64_t limit = 0);
        virtual bool IsEnd();
        virtual uint64_t GetDocumentID();
        virtual void Close();
        virtual void Open(const char* word);

    private:
        std::shared_ptr<struct IndexBlock> m_IndexBlock;
        std::shared_ptr<IndexContext>      m_IndexContext;
        char *                             m_Word          = nullptr;
        uint32_t                           m_BlockSeqNumber = 0;
        unsigned char *                    m_EncodedData   = nullptr;
        UnifiedDecoder                     m_Decoder;
};

#endif
