#include "AdvancedIndexReader.h"

/* 
* Here need to define the relationship
*
* Index --> 1 PageManager (1 filePageManager) 
*
* Reader.GoNext() --> Page.GoNext() --> IO.GoNext()
*/
void 
AdvancedIndexReader::GoNext()
{

    //TODO: uint64_t m_Decoder.GoNext(0);

    if (m_EncodedData!= 0) {
        int IndexBlock = m_IndexBlockTable->GetIndexBlock();

        m_EncodedData = (u_int8_t *)m_IndexBlock;
    }

    m_Decoder.GoNext();

}
void 
AdvancedIndexReader::GoUntil(uint64_t target, uint64_t limit)
{

}

bool 
AdvancedIndexReader::IsEnd()
{
    return true;
}

void 
AdvancedIndexReader::Open(char * word)
{
    auto& table = GetIndexBlockTable();
    m_IndexBlock = table.get(word);

    m_Decoder.Open(m_IndexBlock[0], 0)
    GoNext();
}

void 
AdvancedIndexReader::Close()
{

}

uint64_t AdvancedIndexReader::GetDocumentID()
{
    return m_Decoder.GetDocumentID();
    return 0;
}