#include "AdvancedIndexReader.h"
#include "IndexContext.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cmath>

void AdvancedIndexReader::Open(const char*      streamKey,
                                IndexBlockTable* blockTable,
                                const IndexContext* context)
{
    assert(streamKey);
    assert(blockTable);
    assert(context);

    delete[] m_Word;
    m_Word = new char[std::strlen(streamKey) + 1];
    std::strcpy(m_Word, streamKey);

    m_BlockTable      = blockTable;
    m_Context         = context;
    m_DocFreq         = 0;
    m_BlockSeqNumber  = 0;
    m_RemainingContinuationBlocks = 0;

    uint32_t indexOffset = 0, indexLength = 0;

    bool found = m_BlockTable->FindTermData(streamKey,
                                            &m_BlockSeqNumber, &indexOffset,
                                            &indexLength,      &m_DocFreq,
                                            &m_RemainingContinuationBlocks);
    if (found) {
        IndexBlock* block = reinterpret_cast<IndexBlock*>(
            m_BlockTable->GetBlock(BlockKind::Index, m_BlockSeqNumber));
        assert(block);
        m_Decoder.OpenRaw(block->IB_Data + indexOffset, indexLength);
    }

    GoNext();
}

void AdvancedIndexReader::GoNext()
{
    m_Decoder.GoNext();

    while (m_Decoder.IsEnd() && m_RemainingContinuationBlocks > 0) {
        IndexBlock* next = reinterpret_cast<IndexBlock*>(
            m_BlockTable->GetBlock(BlockKind::Index, m_BlockSeqNumber + 1));
        assert(next);
        ++m_BlockSeqNumber;
        const auto* header = reinterpret_cast<const IndexBlockContinuationHeader*>(next->IB_Data);
        m_Decoder.OpenRaw(next->IB_Data + sizeof(IndexBlockContinuationHeader), header->IBCH_DataLength);
        --m_RemainingContinuationBlocks;
        m_Decoder.GoNext();
    }
}

void AdvancedIndexReader::GoUntil(uint64_t target, uint64_t /*limit*/)
{
    while (true) {
        m_Decoder.GoUntil(target);
        if (!m_Decoder.IsEnd()) break;
        if (m_RemainingContinuationBlocks == 0) break;
        IndexBlock* next = reinterpret_cast<IndexBlock*>(
            m_BlockTable->GetBlock(BlockKind::Index, m_BlockSeqNumber + 1));
        assert(next);
        const auto* header = reinterpret_cast<const IndexBlockContinuationHeader*>(next->IB_Data);
        ++m_BlockSeqNumber;
        --m_RemainingContinuationBlocks;
        if (target > header->IBCH_MaxDocID)
            continue;
        m_Decoder.OpenRaw(next->IB_Data + sizeof(IndexBlockContinuationHeader), header->IBCH_DataLength);
        m_Decoder.GoNext();
        if (m_Decoder.IsEnd()) break;
    }
}

bool AdvancedIndexReader::IsEnd()          { return m_Decoder.IsEnd(); }
uint64_t AdvancedIndexReader::GetDocumentID() {
    return m_Decoder.GetDocumentID();
}
uint32_t AdvancedIndexReader::GetTermFreq() {
    return IsEnd() ? 0u : m_Decoder.GetTermFrequency();
}
float AdvancedIndexReader::GetScore(const DocDataEntry* entry) {
    assert(entry);
    assert(m_Context);
    const uint32_t docLength = entry->DDE_DocLength;
    assert(docLength > 0);

    const float tf = static_cast<float>(GetTermFreq());
    const float df = static_cast<float>(std::max(1u, m_DocFreq));

    const IndexFileHeader& header = m_Context->GetIndexFileHeader();
    const uint64_t documentCount = header.IFH_NumDocuments;
    const float averageDocLength = header.IFH_AvgDocLength;
    assert(documentCount > 0);
    assert(averageDocLength > 0.0f);
    const float totalDocs = static_cast<float>(documentCount);

    const float dl = static_cast<float>(std::max(1u, docLength));
    
    static constexpr float K1 = 1.2f;
    static constexpr float B = 0.75f;

    const float idf = std::max(0.0f,
        std::log((totalDocs - df + 0.5f) / (df + 0.5f) + 1.0f));
    const float tfNorm = tf * (K1 + 1.0f) /
        (tf + K1 * (1.0f - B + B * dl / averageDocLength));

    return idf * tfNorm;
}
void AdvancedIndexReader::Close() {
    delete[] m_Word;
    m_Word = nullptr;
}
