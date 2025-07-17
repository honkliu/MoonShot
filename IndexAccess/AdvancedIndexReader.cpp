#include "AdvancedIndexReader.h"

#include <cstring>
#include <iostream>

using namespace std;
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

    if (m_EncodedData != nullptr) {
        auto& table = GetIndexBlockTable();
        m_IndexBlock = std::shared_ptr<IndexBlock>(table.GetIndexBlock(m_Word), [](IndexBlock*){});

        m_EncodedData = (uint8_t *)m_IndexBlock.get();
    }

    cout << "Go Next" << endl;
    m_Decoder.GoNext();
}
void 
AdvancedIndexReader::GoUntil(uint64_t target, uint64_t limit)
{
    m_Decoder.GoUntil(target);
}

bool 
AdvancedIndexReader::IsEnd()
{
    return m_Decoder.IsEnd();
}

void 
AdvancedIndexReader::Open(const char * word)
{
    m_Word = new char[std::strlen(word) + 1];

    std::strcpy(m_Word, word);

    auto& table = GetIndexBlockTable();
    IndexBlock* block = table.GetIndexBlock(word);
    m_IndexBlock = std::shared_ptr<IndexBlock>(block, [](IndexBlock*){});

    cout << "Opened posting: " << word << endl;
    if (m_IndexBlock) {
        m_Decoder.Open(m_IndexBlock.get(), 0);
    }
    GoNext();
}

void 
AdvancedIndexReader::Close()
{
    m_IndexBlock.reset();
    m_EncodedData = nullptr;
    m_Word = nullptr;
}

uint64_t AdvancedIndexReader::GetDocumentID()
{
    return m_Decoder.GetDocumentID();
}