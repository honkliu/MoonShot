#include "AdvancedIndexReader.h"
#include "IndexContext.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cmath>

namespace {

float StreamWeight(char stream, const QueryCompileModeParameters& parameters)
{
    switch (stream) {
    case 'A': return parameters.QMP_AnchorWeight;
    case 'U': return parameters.QMP_UrlWeight;
    case 'T': return parameters.QMP_TitleWeight;
    case 'B': return parameters.QMP_BodyWeight;
    default: return 1.0f;
    }
}

}

void AdvancedIndexReader::Open(const char*      streamKey,
                                IndexBlockTable* blockTable,
                                const IndexContext* context,
                                uint32_t wordSpan)
{
    assert(streamKey);
    assert(blockTable);
    assert(context);

    if (m_BlockTable && m_BlockSlotNumber != UINT32_MAX)
        m_BlockTable->ReleaseBlock(BlockKind::Index, m_BlockSlotNumber);

    delete[] m_Word;
    m_Word = new char[std::strlen(streamKey) + 1];
    std::strcpy(m_Word, streamKey);
    m_Stream = m_Word[std::strlen(m_Word) - 1];
    m_SourceMask = ReaderSourceMaskForStream(m_Stream);

    m_BlockTable      = blockTable;
    m_Context         = context;
    m_DocFreq         = 0;
    m_WordSpan        = std::max(1u, wordSpan);
    m_SpanWeight      = m_Context->GetSpanWeight(m_WordSpan) * StreamWeight(m_Stream, m_Context->GetQueryParameters());
    m_Idf             = 0.0f;
    m_Bm25LengthBias  = 0.0f;
    m_Bm25LengthScale = 0.0f;
    m_BlockSeqNumber  = 0;
    m_BlockSlotNumber = UINT32_MAX;
    m_RemainingContinuationBlocks = 0;

    uint32_t indexOffset = 0, indexLength = 0;

    bool found = m_BlockTable->FindTermData(streamKey,
                                            &m_BlockSeqNumber, &indexOffset,
                                            &indexLength,      &m_DocFreq,
                                            &m_RemainingContinuationBlocks);
    if (found) {
        static constexpr float K1 = 1.2f;
        static constexpr float B = 0.75f;

        const IndexFileHeader& header = m_Context->GetIndexFileHeader();
        assert(header.IFH_NumDocuments > 0);
        assert(header.IFH_AvgDocLength > 0.0f);

        const float totalDocs = static_cast<float>(header.IFH_NumDocuments);
        const float df = static_cast<float>(std::max(1u, m_DocFreq));
        m_Idf = std::max(0.0f,
            std::log((totalDocs - df + 0.5f) / (df + 0.5f) + 1.0f));
        m_Bm25LengthBias = K1 * (1.0f - B);
        m_Bm25LengthScale = K1 * B / std::max(1.0f, m_Context->GetAverageStreamLength(m_Stream));

        IndexBlock* block = reinterpret_cast<IndexBlock*>(
            m_BlockTable->GetBlock(BlockKind::Index, m_BlockSeqNumber, &m_BlockSlotNumber));
        assert(block);
        m_Decoder.OpenRaw(block->IB_Data + indexOffset, indexLength);
    }

    GoNext();
}

void AdvancedIndexReader::GoNext()
{
    m_Decoder.GoNext();

    while (m_Decoder.IsEnd() && m_RemainingContinuationBlocks > 0) {
        m_BlockTable->ReleaseBlock(BlockKind::Index, m_BlockSlotNumber);
        IndexBlock* next = reinterpret_cast<IndexBlock*>(
            m_BlockTable->GetBlock(BlockKind::Index, m_BlockSeqNumber + 1, &m_BlockSlotNumber));
        assert(next);
        ++m_BlockSeqNumber;
        const auto* header = reinterpret_cast<const IndexBlockContinuationHeader*>(next->IB_Data);
        m_Decoder.OpenRaw(next->IB_Data + sizeof(IndexBlockContinuationHeader), header->IBCH_DataLength);
        --m_RemainingContinuationBlocks;
        m_Decoder.GoNext();
    }

    if (m_Decoder.IsEnd() && m_RemainingContinuationBlocks == 0) {
        m_BlockTable->ReleaseBlock(BlockKind::Index, m_BlockSlotNumber);
        m_BlockSlotNumber = UINT32_MAX;
    }
}

void AdvancedIndexReader::GoUntil(uint64_t target, uint64_t /*limit*/)
{
    while (true) {
        m_Decoder.GoUntil(target);
        if (!m_Decoder.IsEnd()) break;
        if (m_RemainingContinuationBlocks == 0) {
            m_BlockTable->ReleaseBlock(BlockKind::Index, m_BlockSlotNumber);
            m_BlockSlotNumber = UINT32_MAX;
            break;
        }
        m_BlockTable->ReleaseBlock(BlockKind::Index, m_BlockSlotNumber);
        IndexBlock* next = reinterpret_cast<IndexBlock*>(
            m_BlockTable->GetBlock(BlockKind::Index, m_BlockSeqNumber + 1, &m_BlockSlotNumber));
        assert(next);
        const auto* header = reinterpret_cast<const IndexBlockContinuationHeader*>(next->IB_Data);
        ++m_BlockSeqNumber;
        --m_RemainingContinuationBlocks;
        if (target > header->IBCH_MaxDocID) {
            m_BlockTable->ReleaseBlock(BlockKind::Index, m_BlockSlotNumber);
            m_BlockSlotNumber = UINT32_MAX;
            continue;
        }
        m_Decoder.OpenRaw(next->IB_Data + sizeof(IndexBlockContinuationHeader), header->IBCH_DataLength);
        m_Decoder.GoNext();
        if (m_Decoder.IsEnd()) {
            m_BlockTable->ReleaseBlock(BlockKind::Index, m_BlockSlotNumber);
            m_BlockSlotNumber = UINT32_MAX;
            break;
        }
    }
}

bool AdvancedIndexReader::IsEnd()          { return m_Decoder.IsEnd(); }
uint64_t AdvancedIndexReader::GetDocumentID() {
    return m_Decoder.GetDocumentID();
}
uint32_t AdvancedIndexReader::GetTermFreq() {
    return m_Decoder.GetTermFreq();
}
float AdvancedIndexReader::GetScore(const DocDataEntry* entry) {
    assert(entry);
    const uint32_t docLength = IndexContext::GetStreamLength(*entry, m_Stream);
    assert(docLength > 0);

    const float tf = static_cast<float>(m_Decoder.GetTermFreq());
    const float dl = static_cast<float>(std::max(1u, docLength));

    static constexpr float K1PlusOne = 2.2f;
    return m_SpanWeight * m_Idf * tf * K1PlusOne /
        (tf + m_Bm25LengthBias + m_Bm25LengthScale * dl);
}
void AdvancedIndexReader::Close() {
    if (m_BlockTable && m_BlockSlotNumber != UINT32_MAX)
        m_BlockTable->ReleaseBlock(BlockKind::Index, m_BlockSlotNumber);
    m_BlockSlotNumber = UINT32_MAX;
    delete[] m_Word;
    m_Word = nullptr;
}
