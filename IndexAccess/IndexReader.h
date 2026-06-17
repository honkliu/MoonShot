#ifndef INDEXREADER_H__
#define INDEXREADER_H__

#include <cstdint>
#include <print>
#include <string>

#include "BlockTable.h"

class IndexReader
{
    public:
        IndexReader(const IndexReader&) = delete;

        virtual void GoNext() = 0;
        virtual void GoUntil(uint64_t target, uint64_t limit = UINT64_MAX) = 0;
        virtual bool IsEnd() = 0;
        virtual uint64_t GetDocumentID() = 0;

        virtual uint32_t GetTermFreq() { return 1u; }

        virtual float GetScore(const DocRecord* /*record*/) { return 0.0f; }

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
