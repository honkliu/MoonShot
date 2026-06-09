#include "AdvancedIndexReader.h"

#include <cstring>
#include <iostream>

using namespace std;

void
AdvancedIndexReader::GoNext()
{
    /*
    * If we have more data in the current block, advance the decoder.
    * When the decoder reaches the block boundary, reload the next block.
    */
    if (m_Decoder.IsEnd() && m_IndexBlock) {
        auto& table = GetIndexBlockTable();
        IndexBlock* next = table.GetIndexBlock(
            m_BlockSeqNumber + 1, 1);
        if (next) {
            ++m_BlockSeqNumber;
            m_IndexBlock = shared_ptr<IndexBlock>(next, [](IndexBlock*){});
            m_Decoder.Open(m_IndexBlock.get(), m_Decoder.GetDocumentID());
        }
    }

    m_Decoder.GoNext();
}

void
AdvancedIndexReader::GoUntil(uint64_t target, uint64_t /*limit*/)
{
    m_Decoder.GoUntil(target);
}

bool
AdvancedIndexReader::IsEnd()
{
    return m_Decoder.IsEnd();
}

void
AdvancedIndexReader::Open(const char* word)
{
    /*
    * Free any previous allocation before claiming new storage.
    */
    delete[] m_Word;
    m_Word = new char[std::strlen(word) + 1];
    std::strcpy(m_Word, word);

    auto& table = GetIndexBlockTable();
    IndexBlock* block = table.GetIndexBlock(word);

    if (block) {
        m_IndexBlock = shared_ptr<IndexBlock>(block, [](IndexBlock*){});
        m_Decoder.Open(m_IndexBlock.get(), 0);
        m_EncodedData = reinterpret_cast<unsigned char*>(block->IB_Data);
    }

    GoNext();
}

void
AdvancedIndexReader::Close()
{
    m_IndexBlock.reset();
    m_EncodedData = nullptr;
    delete[] m_Word;
    m_Word = nullptr;
}

uint64_t AdvancedIndexReader::GetDocumentID()
{
    return m_Decoder.GetDocumentID();
}
