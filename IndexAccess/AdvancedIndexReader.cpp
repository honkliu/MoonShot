#include "AdvancedIndexReader.h"
#include <cstring>

void AdvancedIndexReader::Open(const char*      streamKey,
                                IndexBlockTable* blockTable,
                                uint32_t         docFreq)
{
    delete[] m_Word;
    m_Word = new char[std::strlen(streamKey) + 1];
    std::strcpy(m_Word, streamKey);

    m_BlockTable      = blockTable;
    m_DocFreq         = docFreq;
    m_PageSkipOffset  = 0;
    m_InitialBlockSeq = 0;

    uint32_t block_seq = 0, offset = 0, data_len = 0, freq = 0;
    bool is_last = false;
    uint32_t pso = 0;

    bool found = m_BlockTable->FindTermData(streamKey,
                                            &block_seq, &offset,
                                            &data_len,  &freq,
                                            &is_last,   &pso);
    if (found) {
        IndexBlock* block = m_BlockTable->GetIndexBlock(block_seq, 1);
        if (block) {
            m_BlockSeqNumber  = block_seq;
            m_InitialBlockSeq = block_seq;
            m_PageSkipOffset  = pso;
            m_HasMore         = is_last && (block->IB_Header & IB_HEADER_HAS_MORE);
            m_IndexBlock      = std::shared_ptr<IndexBlock>(block, [](IndexBlock*){});
            m_Decoder.OpenRaw(block->IB_Data + offset, data_len, 0);
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
            m_HasMore    = (next->IB_Header & IB_HEADER_HAS_MORE) != 0;
            OpenContinuation(next, lastDoc);
        }
    }
    m_Decoder.GoNext();
}

void AdvancedIndexReader::GoUntil(uint64_t target, uint64_t /*limit*/)
{
    /* Fast path: use PageSkipList to jump directly to the right block. */
    if (m_PageSkipOffset > 0 && HasMoreBlocks()) {
        const uint64_t* skip = m_BlockTable->GetPageSkipPtr(m_PageSkipOffset);
        if (skip) {
            /* skip[i] = base_doc_id at the start of block (m_InitialBlockSeq + i) */
            uint32_t cur_idx = m_BlockSeqNumber - m_InitialBlockSeq;
            uint32_t tgt_idx = cur_idx;
            while (skip[tgt_idx + 1] != UINT64_MAX && skip[tgt_idx + 1] <= target)
                ++tgt_idx;

            if (tgt_idx > cur_idx) {
                /* Jump directly to the target block */
                uint32_t target_block = m_InitialBlockSeq + tgt_idx;
                IndexBlock* blk = m_BlockTable->GetIndexBlock(target_block, 1);
                if (blk) {
                    uint64_t base_loc = skip[tgt_idx];
                    m_BlockSeqNumber  = target_block;
                    m_IndexBlock      = std::shared_ptr<IndexBlock>(blk, [](IndexBlock*){});
                    m_HasMore         = (blk->IB_Header & IB_HEADER_HAS_MORE) != 0;
                    OpenContinuation(blk, base_loc);
                    m_Decoder.GoNext();
                    if (!m_Decoder.IsEnd()) {
                        m_Decoder.GoUntil(target);
                        if (!m_Decoder.IsEnd()) return;
                    }
                }
            }
        }
    }

    /* Sequential fallback */
    while (true) {
        m_Decoder.GoUntil(target);
        if (!m_Decoder.IsEnd()) break;
        if (!HasMoreBlocks())   break;
        IndexBlock* next = m_BlockTable->GetIndexBlock(m_BlockSeqNumber + 1, 1);
        if (!next) break;
        uint64_t lastDoc = m_Decoder.GetDocumentID();
        ++m_BlockSeqNumber;
        m_IndexBlock = std::shared_ptr<IndexBlock>(next, [](IndexBlock*){});
        m_HasMore    = (next->IB_Header & IB_HEADER_HAS_MORE) != 0;
        OpenContinuation(next, lastDoc);
        m_Decoder.GoNext();
        if (m_Decoder.IsEnd()) break;
    }
}

void AdvancedIndexReader::OpenContinuation(IndexBlock* blk, uint64_t lastDoc)
{
    /* New format: [CONT_MARKER:2][cont_len:2][VarByte:cont_len] */
    const uint8_t* d = blk->IB_Data;
    uint16_t marker = 0;
    std::memcpy(&marker, d, 2);
    if (marker == BLOCK_CONTINUATION_MARKER) {
        uint16_t cont_len = 0;
        std::memcpy(&cont_len, d + 2, 2);
        m_Decoder.OpenRaw(d + 4, cont_len, lastDoc);
    } else {
        /* Fallback: entire IB_Data is continuation (old format blocks) */
        m_Decoder.OpenRaw(d, sizeof(blk->IB_Data), lastDoc);
    }
}

bool AdvancedIndexReader::IsEnd()          { return m_Decoder.IsEnd(); }
uint64_t AdvancedIndexReader::GetDocumentID() {
    return m_Decoder.GetDocumentID();
}
uint32_t AdvancedIndexReader::GetTermFreq() {
    return IsEnd() ? 0u : m_Decoder.GetTermFrequency();
}
float AdvancedIndexReader::GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) {
    return scorer.Score(GetTermFreq(), docLength, m_DocFreq);
}
void AdvancedIndexReader::Close() {
    m_IndexBlock.reset();
    delete[] m_Word;
    m_Word = nullptr;
}
