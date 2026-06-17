#ifndef UNIFIEDDECODER_H__
#define UNIFIEDDECODER_H__

#include <stdint.h>
#include <cstdint>
#include <cstddef>

// Forward declaration to avoid circular dependency
struct IndexBlock;

/*
* VarByte decoder for posting lists.
*
* Two open modes:
*
*   Open(IndexBlock*)
*       Block-based: reads from IndexBlock::IB_Data.
*       End-of-data detected by a zero sentinel byte.
*       Used by AdvancedIndexReader (disk + BlockTable path).
*
*   OpenRaw(data, len)
*       Raw-bytes: reads from a tightly packed VarByte buffer of exact length.
*       End-of-data detected by ptr >= end (no sentinel).
*       Used by TermIndexReader (in-memory PostingStore path).
*
* IsEnd() semantics
* -----------------
* IsEnd() returns true when the CURRENT position has no valid entry —
* i.e., the last GoNext() could not decode a new entry.
* This matches the executor pattern:
*
*   while (!reader->IsEnd()) {
*       use reader->GetDocumentID();
*       reader->GoNext();
*   }
*
* After construction the decoder is NOT yet on any entry.
* Call GoNext() once to position on the first entry before reading.
*/
class UnifiedDecoder {
public:
    UnifiedDecoder()
        : m_block(nullptr)
        , m_current_ptr(nullptr)
        , m_block_end(nullptr)
        , m_current_doc(0)
        , m_current_tf(0)
        , m_raw_mode(false)
        , m_has_current(false)
    {}

    /*
    * Open on a 4KB IndexBlock.
    */
    void Open(IndexBlock* block)
    {
        m_block       = block;
        m_current_doc = 0;
        m_current_tf  = 0;
        m_current_ptr = reinterpret_cast<const uint8_t*>(block->IB_Data);
        m_block_end   = m_current_ptr + sizeof(block->IB_Data);
        m_raw_mode    = false;
        m_has_current = false;
    }

    /*
    * Open on an arbitrary VarByte byte buffer of exact length.
    */
    void OpenRaw(const uint8_t* data, size_t len)
    {
        m_block       = nullptr;
        m_current_doc = 0;
        m_current_tf  = 0;
        m_current_ptr = data;
        m_block_end   = data ? data + len : data;
        m_raw_mode    = true;
        m_has_current = false;
    }

    /*
    * True when the current position has no valid (doc_id, tf) pair.
    * Becomes true after the last entry has been consumed.
    */
    bool IsEnd() const
    {
        return !m_has_current;
    }

    /*
    * Decode and advance to the next (doc_id, tf) pair.
    * After this call, GetDocumentID() / GetTermFrequency() return
    * the newly decoded values if IsEnd() is false.
    */
    void GoNext()
    {
        if (!HasMoreBytes()) {
            m_has_current = false;
            return;
        }

        uint64_t docID = 0;
        uint8_t  shift = 0;
        while (true) {
            if (m_current_ptr >= m_block_end) {
                m_has_current = false;
                return;
            }
            uint8_t byte = *m_current_ptr++;
            docID |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if (!(byte & 0x80))
                break;
            shift += 7;
        }
        m_current_doc = docID;

        m_current_tf = 0;
        shift        = 0;
        while (true) {
            if (m_current_ptr >= m_block_end) {
                m_has_current = false;
                return;
            }
            uint8_t byte = *m_current_ptr++;
            m_current_tf |= static_cast<uint32_t>(byte & 0x7F) << shift;
            if (!(byte & 0x80))
                break;
            shift += 7;
        }

        m_has_current = true;
    }

    /*
    * Seek forward until the current doc_id >= target.
    */
    void GoUntil(uint64_t target)
    {
        while (m_has_current && m_current_doc < target)
            GoNext();

        /*
        * If not yet on any entry, advance once to read the first one,
        * then continue seeking.
        */
        if (!m_has_current && HasMoreBytes()) {
            GoNext();
            while (m_has_current && m_current_doc < target)
                GoNext();
        }
    }

    uint64_t GetDocumentID()    const { return m_current_doc; }
    uint32_t GetTermFrequency() const { return m_current_tf;  }
    bool HasMore() const { return HasMoreBytes(); }

private:
    IndexBlock*    m_block;
    const uint8_t* m_current_ptr;
    const uint8_t* m_block_end;
    uint64_t       m_current_doc;
    uint32_t       m_current_tf;
    bool           m_raw_mode;
    bool           m_has_current;

    bool HasMoreBytes() const
    {
        if (!m_current_ptr || m_current_ptr >= m_block_end)
            return false;

        if (!m_raw_mode && *m_current_ptr == 0)
            return false;

        return true;
    }
};

#endif
