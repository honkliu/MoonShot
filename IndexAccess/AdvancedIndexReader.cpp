#include "AdvancedIndexReader.h"

AdvancedIndexReader::AdvancedIndexReader()
{

}

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
        //int IndexBlock = m_IndexBlockTable->GetIndexBlock();

        //m_EncodedData = (u_int8_t *)m_IndexBlock;
    }

    //TODO if (Decode(m_EncodedData))
    // }
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
AdvancedIndexReader::Close()
{

}

uint64_t AdvancedIndexReader::GetDocumentID()
{
    return 0;
}