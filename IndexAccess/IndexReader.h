#ifndef INDEXREADER_H__
#define INDEXREADER_H__

#include <cstdint>
#include <print>
#include <string>

#include "BlockTable.h"

static constexpr uint64_t READER_DOCID_SOURCE_SHIFT = 59;
static constexpr uint64_t READER_DOCID_VALUE_MASK = (1ull << READER_DOCID_SOURCE_SHIFT) - 1ull;
static constexpr uint8_t READER_SOURCE_ANCHOR = 1u << 0;
static constexpr uint8_t READER_SOURCE_URL = 1u << 1;
static constexpr uint8_t READER_SOURCE_TITLE = 1u << 2;
static constexpr uint8_t READER_SOURCE_BODY = 1u << 3;
static constexpr uint8_t READER_SOURCE_VECTOR = 1u << 4;

inline uint64_t ReaderDocumentIDValue(uint64_t docId)
{
    return docId & READER_DOCID_VALUE_MASK;
}

inline uint8_t ReaderDocumentIDSourceMask(uint64_t docId)
{
    return static_cast<uint8_t>(docId >> READER_DOCID_SOURCE_SHIFT);
}

inline uint64_t MakeReaderDocumentID(uint64_t docId, uint8_t sourceMask)
{
    return (static_cast<uint64_t>(sourceMask) << READER_DOCID_SOURCE_SHIFT)
        | (docId & READER_DOCID_VALUE_MASK);
}

inline uint8_t ReaderSourceMaskForStream(char stream)
{
    switch (stream) {
    case 'A': return READER_SOURCE_ANCHOR;
    case 'U': return READER_SOURCE_URL;
    case 'T': return READER_SOURCE_TITLE;
    case 'B': return READER_SOURCE_BODY;
    case 'V': return READER_SOURCE_VECTOR;
    default: return 0;
    }
}

class IndexReader
{
    public:
        IndexReader(const IndexReader&) = delete;

        virtual void GoNext() = 0;
        virtual void GoUntil(uint64_t target, uint64_t limit = UINT64_MAX) = 0;
        virtual bool IsEnd() = 0;
        virtual uint64_t GetDocumentID() = 0;

        virtual uint32_t GetTermFreq() { return 1u; }

        virtual float GetScore(const DocDataEntry* /*entry*/) { return 0.0f; }

        virtual uint8_t GetSourceMask() { return 0; }

        virtual void Close() = 0;

        /*
        * Enable debug tracing.  label appears in every printed line so the
        * caller can identify which reader produced each output.
        * Composite readers propagate the flag to all children.
        */
        virtual void SetDebug(const char* label, int depth = 0)
        {
            m_debug      = true;
            m_debugDepth = depth;
            if (label) m_debugLabel = label;
        }

    protected:
        IndexReader() = default;

        bool        m_debug      = false;
        int         m_debugDepth = 0;
        const char* m_debugLabel = "";
};

#endif
