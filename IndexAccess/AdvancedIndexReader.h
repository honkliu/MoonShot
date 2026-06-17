#ifndef ADVANCEDINDEXREADER_H__
#define ADVANCEDINDEXREADER_H__

#include <stdint.h>
#include <cinttypes>
#include <memory>
#include <string>

#include "BlockTable.h"
#include "IndexReader.h"
#include "UnifiedDecoder.h"

class IndexContext;

/*
* AdvancedIndexReader — the leaf IndexReader.
*
* Reads one (term + stream) posting list from IndexBlockTable using
* UnifiedDecoder, identical to REF's ISRWord backed by a PageManager.
*
* Lifecycle (mirrors ISRCreatorDocShard::CreateWordIsr):
*   reader->Open(streamKey, blockTable)
*       ← look up streamKey in blockTable
*       ← if found: open UnifiedDecoder on the first block, GoNext() once
*       ← if not found: reader stays at IsEnd
*
* Multi-block spanning:
*   GoNext() / GoUntil() use LTE_ContinuationBlockCount from the LeafTermEntry.
*   Posting bytes store absolute VBC (docID, TF) pairs, so continuation blocks
*   reopen without a base docID.
*
* Term frequency and BM25 score live HERE only.
* Composite readers (And/Or/Not) aggregate but do not own TF or BM25.
*/
class AdvancedIndexReader : public IndexReader
{
    public:
        AdvancedIndexReader() = default;

        ~AdvancedIndexReader()
        {
            delete[] m_Word;
        }

        /*
        * Primary open: called by IndexContext::BuildIndexReader for each
        * leaf TermNode in the EvalTree.
        * blockTable — the IndexContext-owned BlockTable.
        */
        void Open(const char*      streamKey,
              IndexBlockTable* blockTable,
              const IndexContext* context);

        void SetDebug(const char* label, int depth = 0) override
        {
            IndexReader::SetDebug(label, depth);
            const char* w = m_Word ? m_Word : "?";
            auto ind = std::string(depth * 2, ' ');
            if (!m_Decoder.IsEnd())
                std::println("{}[leaf] {:<12}  -{}-", ind, w, m_Decoder.GetDocumentID());
            else
                std::println("{}[leaf] {:<12}  (empty)", ind, w);
        }

        void     GoNext() override;
        void     GoUntil(uint64_t target, uint64_t limit = UINT64_MAX) override;
        bool     IsEnd() override;
        uint64_t GetDocumentID() override;

        /*
        * Term frequency for the current document.
        * Non-zero only when IsEnd() == false.
        */
        uint32_t GetTermFreq() override;

        /*
        * BM25 contribution of this term for the current document.
        * Uses the docFreq stored in the matching LeafTermEntry.
        */
        float GetScore(const DocDataEntry* entry) override;

        void Close() override;

    private:
        std::shared_ptr<IndexBlock> m_IndexBlock;
        char*                       m_Word            = nullptr;
        uint32_t                    m_BlockSeqNumber  = 0;
        uint32_t                    m_InitialBlockSeq = 0;   // first block of this term
        uint32_t                    m_DocFreq         = 0;
        uint32_t                    m_TotalContinuationBlocks = 0;
        uint32_t                    m_RemainingContinuationBlocks = 0;
        IndexBlockTable*            m_BlockTable      = nullptr;
        const IndexContext*         m_Context         = nullptr;
        UnifiedDecoder              m_Decoder;

        bool HasMoreBlocks() const { return m_RemainingContinuationBlocks > 0; }

        /* Open decoder on a continuation block, reading cont_len from the block header. */
        void OpenContinuation(IndexBlock* blk);
};

#endif
