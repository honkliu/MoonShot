#ifndef ADVANCEDINDEXREADER_H__
#define ADVANCEDINDEXREADER_H__

#include <stdint.h>
#include <cinttypes>
#include <string>
#include <memory>

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
*   GoNext() / GoUntil() check IB_HEADER_HAS_MORE bit.
*   If set, the posting list continues in block_seq + 1.
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
            if (!m_Decoder.IsEnd())
                printf("%*s[leaf] %-12s  -%" PRIu64 "-\n",
                       depth * 2, "", w, m_Decoder.GetDocumentID());
            else
                printf("%*s[leaf] %-12s  (empty)\n", depth * 2, "", w);
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
        char*                       m_Word           = nullptr;
        uint32_t                    m_BlockSeqNumber = 0;
        uint32_t                    m_DocFreq        = 0;
        IndexBlockTable*            m_BlockTable     = nullptr; // set by IndexContext, always non-null after Open
        UnifiedDecoder              m_Decoder;

        bool HasMoreBlocks() const
        {
            return m_IndexBlock
                && (m_IndexBlock->IB_Header & IB_HEADER_HAS_MORE) != 0;
        }
};

#endif
