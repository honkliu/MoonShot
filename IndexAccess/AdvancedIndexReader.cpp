#include "AdvancedIndexReader.h"

#include <cinttypes>
#include <cstring>

void AdvancedIndexReader::Open(const char*      streamKey,
                                IndexBlockTable* blockTable,
                                uint32_t         docFreq)
{
    delete[] m_Word;
    m_Word = new char[std::strlen(streamKey) + 1];
    std::strcpy(m_Word, streamKey);

    m_BlockTable = blockTable;
    m_DocFreq    = docFreq;

    IndexBlock* block = m_BlockTable->GetIndexBlock(streamKey);

    if (block) {
        m_BlockSeqNumber = static_cast<uint32_t>(
            block->IB_Header & ~IB_HEADER_HAS_MORE);

        m_IndexBlock = std::shared_ptr<IndexBlock>(block, [](IndexBlock*){});
        m_Decoder.Open(m_IndexBlock.get(), 0);
    }

    GoNext();
}

void AdvancedIndexReader::GoNext()
{
    if (m_Decoder.IsEnd() && HasMoreBlocks()) {
        IndexBlock* next = m_BlockTable->GetIndexBlock(m_BlockSeqNumber + 1, 1);

        if (next) {
            ++m_BlockSeqNumber;
            m_IndexBlock = std::shared_ptr<IndexBlock>(next, [](IndexBlock*){});
            m_Decoder.Open(m_IndexBlock.get(), m_Decoder.GetDocumentID());
        }
    }

    m_Decoder.GoNext();

    if (m_debug && !m_Decoder.IsEnd())
        printf("%*s%-12s  -%" PRIu64 "-\n",
               m_debugDepth * 2, "", m_Word ? m_Word : "?",
               m_Decoder.GetDocumentID());
}

void AdvancedIndexReader::GoUntil(uint64_t target, uint64_t /*limit*/)
{
    while (true) {
        m_Decoder.GoUntil(target);

        if (!m_Decoder.IsEnd())
            break;

        if (!HasMoreBlocks())
            break;

        IndexBlock* next = m_BlockTable->GetIndexBlock(m_BlockSeqNumber + 1, 1);

        if (!next)
            break;

        uint64_t lastDoc = m_Decoder.GetDocumentID();
        ++m_BlockSeqNumber;
        m_IndexBlock = std::shared_ptr<IndexBlock>(next, [](IndexBlock*){});
        m_Decoder.Open(m_IndexBlock.get(), lastDoc);
        m_Decoder.GoNext();

        if (m_Decoder.IsEnd())
            break;
    }
}

bool AdvancedIndexReader::IsEnd()
{
    return m_Decoder.IsEnd();
}

uint64_t AdvancedIndexReader::GetDocumentID()
{
    return m_Decoder.GetDocumentID();
}

uint32_t AdvancedIndexReader::GetTermFreq()
{
    return IsEnd() ? 0u : m_Decoder.GetTermFrequency();
}

float AdvancedIndexReader::GetBM25Score(const Bm25Scorer& scorer,
                                         uint32_t          docLength)
{
    return scorer.Score(GetTermFreq(), docLength, m_DocFreq);
}

void AdvancedIndexReader::Close()
{
    m_IndexBlock.reset();
    delete[] m_Word;
    m_Word = nullptr;
}
