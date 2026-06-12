#include "AdvancedIndexReader.h"

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

    uint32_t block_seq = 0, offset = 0, data_len = 0, freq = 0;
    bool is_last = false;
    bool found = m_BlockTable->FindTermData(streamKey,
                                            &block_seq, &offset,
                                            &data_len,  &freq,
                                            &is_last);
    if (found) {
        IndexBlock* block = m_BlockTable->GetIndexBlock(block_seq, 1);
        if (block) {
            m_BlockSeqNumber = block_seq;
            m_IndexBlock = std::shared_ptr<IndexBlock>(block, [](IndexBlock*){});
            // HAS_MORE on the block only means THIS term continues iff
            // it is the last entry in the block.
            m_HasMore = is_last && (block->IB_Header & IB_HEADER_HAS_MORE);
            m_Decoder.OpenRaw(block->IB_Data + offset, data_len,
                              /*last_doc_id=*/0);
        }
    }

    GoNext();
}

void AdvancedIndexReader::GoNext()
{
    if (m_Decoder.IsEnd() && HasMoreBlocks()) {
        IndexBlock* next = m_BlockTable->GetIndexBlock(m_BlockSeqNumber + 1, 1);

        if (next) {
            uint64_t lastDoc = m_Decoder.GetDocumentID();
            ++m_BlockSeqNumber;
            m_IndexBlock = std::shared_ptr<IndexBlock>(next, [](IndexBlock*){});
            m_HasMore = (next->IB_Header & IB_HEADER_HAS_MORE) != 0;

            // Continuation block: skip the 0xFFFF marker and read raw bytes.
            const uint8_t* data = next->IB_Data + 2;
            size_t         len  = sizeof(next->IB_Data) - 2;
            m_Decoder.OpenRaw(data, len, lastDoc);
        }
    }

    m_Decoder.GoNext();

    if (m_debug && !m_Decoder.IsEnd())
        std::println("{}{:<12}  -{}-",
                     std::string(m_debugDepth * 2, ' '),
                     m_Word ? m_Word : "?",
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
