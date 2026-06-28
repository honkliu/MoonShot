#ifndef UNIFIEDDECODER_H__
#define UNIFIEDDECODER_H__

#include <stdint.h>
#include <cstdint>
#include <cstddef>

/*
* VarByte decoder for posting lists.
*
* OpenRaw(data, len) reads from a tightly packed VarByte buffer of exact length.
* End-of-data is detected by ptr >= end.
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
        : m_current_ptr(nullptr)
        , m_block_end(nullptr)
        , m_current_doc(0)
        , m_current_tf8(0)
        , m_has_current(false)
    {}

    /*
    * Open on an arbitrary VarByte byte buffer of exact length.
    */
    void OpenRaw(const uint8_t* data, size_t len)
    {
        m_current_doc = 0;
        m_current_tf8 = 0;
        m_current_ptr = data;
        m_block_end   = data ? data + len : data;
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
    * After this call, GetDocumentID() / GetTermFreq() return
    * the newly decoded values if IsEnd() is false.
    */
    void GoNext()
    {
        DecodeNext();
    }

    /*
    * Seek forward until the current doc_id >= target.
    */
    void GoUntil(uint64_t target)
    {
        if (m_has_current && m_current_doc >= target)
            return;

        do {
            DecodeNext();
        } while (m_has_current && m_current_doc < target);
    }

    uint64_t GetDocumentID()    const { return m_current_doc; }
    uint8_t GetTermFreq() const { return m_current_tf8; }

private:
    void DecodeNext()
    {
        if (!m_current_ptr || m_current_ptr >= m_block_end) {
            m_has_current = false;
            return;
        }

        m_current_doc = DecodeVarUInt64();
        if (m_current_ptr >= m_block_end) {
            m_has_current = false;
            return;
        }
        m_current_tf8 = *m_current_ptr++;

        m_has_current = true;
    }

    uint64_t DecodeVarUInt64()
    {
        uint8_t byte = *m_current_ptr++;
        if (!(byte & 0x80))
            return byte;

        uint64_t value = static_cast<uint64_t>(byte & 0x7F);
        uint8_t shift = 7;
        do {
            byte = *m_current_ptr++;
            value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            shift += 7;
        } while (byte & 0x80);
        return value;
    }

    const uint8_t* m_current_ptr;
    const uint8_t* m_block_end;
    uint64_t       m_current_doc;
    uint8_t        m_current_tf8;
    bool           m_has_current;
};

#endif
