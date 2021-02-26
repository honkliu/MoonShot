#include "AdvancedIndexReader.h"

AdvancedIndexReader::AdvancedIndexReader()
{

}

void AdvancedIndexReader::GoNext()
{
    uint64_t m_Decoder.GoNext(0);
    
    if (!m_Decoder.IsEnd()) {
        int page = pageReader->GetNextPage();
        m_Decoder.SetPage(page)

    }

    
}

void AdvancedIndexReader::GoUntil(uint64_t target, uint64_t limit)
{

}

bool AdvancedIndexReader::IsEnd()
{
    return true;
}

uint64_t AdvancedIndexReader::GetDocumentID()
{
    return 0;
}