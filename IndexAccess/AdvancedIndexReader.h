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

class AdvancedIndexReader : public IndexReader 
{
    public:
        AdvancedIndexReader() = default;
        virtual ~AdvancedIndexReader() = default;

        virtual void GoNext();
        virtual void GoUntil(uint64_t target = 0, uint64_t limit = 0);
        virtual bool IsEnd();
        virtual uint64_t GetDocumentID();
        virtual void Close();
        virtual void Open(const char* word);  // Add overload for word parameter

    private:
        std::shared_ptr<struct IndexBlock> m_IndexBlock;
        std::shared_ptr<IndexContext> m_IndexContext;
        char * m_Word; 
        uint32_t m_BlockSeqNumber;
        unsigned char * m_EncodedData;
        UnifiedDecoder m_Decoder;

};

// class AdvancedIndexEBWriter : public IndexWriter {
//     public:
//         AdvancedIndexEBWriter() = default;

//         virtual void Open();
//         virtual void Open(const std::string& word);
//         virtual void Write(const std::string& data);
//         virtual void Close();   
//     private:
//         std::shared_ptr<IndexBlock> m_IndexBlock;   
//         std::shared_ptr<IndexContext> m_IndexContext;
//         std::string m_Word;
//         uint32_t m_BlockSeqNumber;
// };

#endif