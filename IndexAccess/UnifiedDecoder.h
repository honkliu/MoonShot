#ifndef UNIFIEDDECODER_H__
#define UNIFIEDDECODER_H__

#include <stdint.h>
#include <cstdint>

// Forward declaration to avoid circular dependency
struct IndexBlock;

class UnifiedDecoder {
public:
    UnifiedDecoder() : m_block(nullptr), m_current_ptr(nullptr), 
                       m_current_doc(0), m_current_tf(0), 
                       m_block_end(nullptr) {}

    // Open a new index block for decoding
    void Open(IndexBlock* block, uint64_t last_doc_id = 0) {
        m_block = block;
        m_current_doc = last_doc_id;
        m_current_ptr = reinterpret_cast<const uint8_t*>(block->IB_Data);
        m_block_end = m_current_ptr + sizeof(block->IB_Data);
    }

    // Check if at end of block
    bool IsEnd() const {
        return m_current_ptr >= m_block_end || *m_current_ptr == 0;
    }

    // Move to next document
    void GoNext() {
        if (IsEnd()) return;
        
        // Decode delta-compressed document ID
        uint64_t delta = 0;
        uint8_t shift = 0;
        while (true) {
            uint8_t byte = *m_current_ptr++;
            delta |= (byte & 0x7F) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
        }
        m_current_doc += delta;

        // Decode term frequency
        m_current_tf = 0;
        shift = 0;
        while (true) {
            uint8_t byte = *m_current_ptr++;
            m_current_tf |= (byte & 0x7F) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
        }
    }

    // Seek to document >= target
    void GoUntil(uint64_t target) {
        while (!IsEnd() && m_current_doc < target) {
            GoNext();
        }
    }

    // Getters
    uint64_t GetDocumentID() const { return m_current_doc; }
    uint32_t GetTermFrequency() const { return m_current_tf; }

private:
    IndexBlock* m_block;            // Current index block
    const uint8_t* m_current_ptr;   // Current read position
    const uint8_t* m_block_end;     // End of block marker
    uint64_t m_current_doc;         // Current document ID
    uint32_t m_current_tf;          // Current term frequency
};

#endif