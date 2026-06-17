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
    m_InitialBlockSeq = 0;
    m_TotalContinuationBlocks = 0;
    m_RemainingContinuationBlocks = 0;

    uint32_t indexBlockID = 0, indexOffset = 0, indexLength = 0, freq = 0;
    uint32_t continuationBlockCount = 0;

    bool found = m_BlockTable->FindTermData(streamKey,
                                            &indexBlockID, &indexOffset,
                                            &indexLength,  &freq,
                                            &continuationBlockCount);
    if (found) {
        /* Use doc_freq from LeafTermEntry — correct after Load(), unlike PostingStore */
        m_DocFreq = freq;

        IndexBlock* block = m_BlockTable->GetIndexBlock(indexBlockID, 1);
        assert(block);
        m_BlockSeqNumber  = indexBlockID;
        m_InitialBlockSeq = indexBlockID;
        m_TotalContinuationBlocks = continuationBlockCount;
        m_RemainingContinuationBlocks = continuationBlockCount;
        m_IndexBlock      = std::shared_ptr<IndexBlock>(block, [](IndexBlock*){});
        m_Decoder.OpenRaw(block->IB_Data + indexOffset, indexLength);
    }

    GoNext();
}

void AdvancedIndexReader::GoNext()
{
    if (!m_Decoder.IsEnd())
        m_Decoder.GoNext();

    if (m_Decoder.IsEnd() && m_Decoder.HasMore())
        m_Decoder.GoNext();

    while (m_Decoder.IsEnd() && HasMoreBlocks()) {
        IndexBlock* next = m_BlockTable->GetIndexBlock(m_BlockSeqNumber + 1, 1);
        assert(next);
        ++m_BlockSeqNumber;
        m_IndexBlock = std::shared_ptr<IndexBlock>(next, [](IndexBlock*){});
        OpenContinuation(next);
        --m_RemainingContinuationBlocks;
        m_Decoder.GoNext();
    }
}

void AdvancedIndexReader::GoUntil(uint64_t target, uint64_t /*limit*/)
{
    while (true) {
        m_Decoder.GoUntil(target);
        if (!m_Decoder.IsEnd()) break;
        if (!HasMoreBlocks())   break;
        IndexBlock* next = m_BlockTable->GetIndexBlock(m_BlockSeqNumber + 1, 1);
        assert(next);
        ++m_BlockSeqNumber;
        m_IndexBlock = std::shared_ptr<IndexBlock>(next, [](IndexBlock*){});
        OpenContinuation(next);
        --m_RemainingContinuationBlocks;
        m_Decoder.GoNext();
        if (m_Decoder.IsEnd()) break;
    }
}

void AdvancedIndexReader::OpenContinuation(IndexBlock* blk)
{
    /* New format: [CONT_MARKER:2][cont_len:2][VarByte:cont_len] */
    const uint8_t* d = blk->IB_Data;
    uint16_t marker = 0;
    std::memcpy(&marker, d, 2);
    assert(marker == BLOCK_CONTINUATION_MARKER);
    uint16_t cont_len = 0;
    std::memcpy(&cont_len, d + 2, 2);
    m_Decoder.OpenRaw(d + 4, cont_len);
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
    m_IndexBlock.reset();
    delete[] m_Word;
    m_Word = nullptr;
}
