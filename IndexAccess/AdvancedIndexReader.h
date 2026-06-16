#ifndef ADVANCEDINDEXREADER_H__
#define ADVANCEDINDEXREADER_H__

#include <stdint.h>
#include <cinttypes>
#include <memory>
#include <string>

#include "BlockTable.h"
#include "IndexReader.h"
#include "UnifiedDecoder.h"
#include "Bm25Scorer.h"

class IndexContext;

/*
* AdvancedIndexReader — the leaf IndexReader.
*
* Reads one (term + stream) posting list from IndexBlockTable using
* UnifiedDecoder, identical to REF's ISRWord backed by a PageManager.
*
* Lifecycle (mirrors ISRCreatorDocShard::CreateWordIsr):
*   reader->Open(streamKey, blockTable, docFreq)
*       ← look up streamKey in blockTable
*       ← if found: open UnifiedDecoder on the first block, GoNext() once
*       ← if not found: reader stays at IsEnd
*
* Multi-block spanning:
*   GoNext() / GoUntil() use LTE_ContinuationBlockCount from the LeafTermEntry.
*   This allows the tail of a continuation block to host unrelated term starts.
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
        * docFreq    — document frequency for BM25 IDF computation.
        */
        void Open(const char*     streamKey,
                  IndexBlockTable* blockTable,
                  uint32_t         docFreq);

        void Open(const char* word) override {} /* not used — Open(key,table,freq) required */

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
        * Uses the docFreq supplied at Open() time.
        */
        float GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) override;

        void Close() override;

    private:
        std::shared_ptr<IndexBlock> m_IndexBlock;
        char*                       m_Word            = nullptr;
        uint32_t                    m_BlockSeqNumber  = 0;
        uint32_t                    m_InitialBlockSeq = 0;   // first block of this term
        uint32_t                    m_DocFreq         = 0;
        uint32_t                    m_PageSkipOffset  = 0;   // 0 = no skip list
        uint32_t                    m_TotalContinuationBlocks = 0;
        uint32_t                    m_RemainingContinuationBlocks = 0;
        IndexBlockTable*            m_BlockTable      = nullptr;
        UnifiedDecoder              m_Decoder;

        bool HasMoreBlocks() const { return m_RemainingContinuationBlocks > 0; }

        /* Open decoder on a continuation block, reading cont_len from the block header. */
        void OpenContinuation(IndexBlock* blk, uint64_t lastDoc);
};

#endif
