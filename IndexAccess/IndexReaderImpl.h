#ifndef INDEXREADERIMPL_H__
#define INDEXREADERIMPL_H__

#include "IndexReader.h"
#include "Embeddings.h"

#include <cinttypes>
#include <algorithm>
#include <memory>
#include <print>
#include <string>
#include <vector>

static constexpr uint64_t NO_MORE_DOCS = UINT64_MAX;

/*
* Composite readers — And / Or / Not.
*
* Leaves are AdvancedIndexReader instances created by IndexContext.
* These composites know nothing about blocks or decoders; they operate
* purely through the IndexReader interface.
*
* BM25 score and term frequency are summed / aggregated from the leaves.
* This matches REF: ISRSimpleQuery2 / ISRContainer aggregate per-word
* scores from the underlying ISRWord objects.
*/

/*
* AndIndexReader — DAAT intersection (all children must match).
*/
class AndIndexReader : public IndexReader {
public:
    explicit AndIndexReader(std::vector<std::shared_ptr<IndexReader>> children)
        : m_Children(std::move(children))
    {
        AlignToPivot();
    }

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        std::println("{}[AND]", std::string(depth * 2, ' '));
        for (auto& c : m_Children) c->SetDebug(label, depth + 1);
    }

    bool IsEnd() override
    {
        if (m_Children.empty())
            return true;
        for (auto& c : m_Children)
            if (c->IsEnd()) return true;
        return false;
    }

    uint64_t GetDocumentID() override
    {
        return IsEnd() ? NO_MORE_DOCS : m_Children[0]->GetDocumentID();
    }

    uint32_t GetTermFreq() override
    {
        uint32_t total = 0;
        for (auto& c : m_Children)
            total += c->GetTermFreq();
        return total;
    }

    float GetScore(const DocDataEntry* entry) override
    {
        float total = 0.0f;
        for (auto& c : m_Children)
            total += c->GetScore(entry);
        return total;
    }

    uint8_t GetSourceMask() override
    {
        uint8_t sourceMask = 0;
        const uint64_t doc = GetDocumentID();
        for (auto& c : m_Children) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                sourceMask |= c->GetSourceMask();
        }
        return sourceMask;
    }

    void GoNext() override
    {
        if (IsEnd())
            return;

        uint64_t doc = GetDocumentID();

        for (auto& c : m_Children) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                c->GoNext();
        }

        AlignToPivot();
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        for (auto& c : m_Children)
            c->GoUntil(target, limit);

        AlignToPivot();
    }

    void Close() override { for (auto& c : m_Children) c->Close(); }

private:
    std::vector<std::shared_ptr<IndexReader>> m_Children;

    void AlignToPivot()
    {
        while (true) {
            if (IsEnd())
                return;

            uint64_t pivot = 0;

            for (auto& c : m_Children) {
                if (c->IsEnd())
                    return;

                pivot = std::max(pivot, c->GetDocumentID());
            }

            bool aligned = true;

            for (auto& c : m_Children) {
                if (c->GetDocumentID() == pivot)
                    continue;

                c->GoUntil(pivot);

                if (c->IsEnd())
                    return;

                if (c->GetDocumentID() != pivot) {
                    aligned = false;
                    break;
                }
            }

            if (aligned) {
                if (m_debug)
                    std::println("{}AND match  doc {}",
                                 std::string(m_debugDepth * 2, ' '), pivot);
                return;
            }
        }
    }
};

/*
* OrIndexReader — DAAT union (at least one child must match).
*/
class OrIndexReader : public IndexReader {
public:
    explicit OrIndexReader(std::vector<std::shared_ptr<IndexReader>> children)
        : m_Children(std::move(children))
    {}

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        std::println("{}[OR]", std::string(depth * 2, ' '));
        for (auto& c : m_Children) c->SetDebug(label, depth + 1);
    }

    bool IsEnd() override
    {
        for (auto& c : m_Children) {
            if (!c->IsEnd())
                return false;
        }
        return true;
    }

    uint64_t GetDocumentID() override
    {
        uint64_t minDoc = NO_MORE_DOCS;

        for (auto& c : m_Children) {
            if (!c->IsEnd())
                minDoc = std::min(minDoc, c->GetDocumentID());
        }

        return minDoc;
    }

    uint32_t GetTermFreq() override
    {
        uint64_t doc   = GetDocumentID();
        uint32_t total = 0;

        for (auto& c : m_Children) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                total += c->GetTermFreq();
        }

        return total;
    }

    float GetScore(const DocDataEntry* entry) override
    {
        uint64_t doc   = GetDocumentID();
        float    total = 0.0f;

        for (auto& c : m_Children) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                total += c->GetScore(entry);
        }

        return total;
    }

    uint8_t GetSourceMask() override
    {
        uint64_t doc = GetDocumentID();
        uint8_t sourceMask = 0;

        for (auto& c : m_Children) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                sourceMask |= c->GetSourceMask();
        }

        return sourceMask;
    }

    void GoNext() override
    {
        if (IsEnd())
            return;

        uint64_t doc = GetDocumentID();

        for (auto& c : m_Children) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                c->GoNext();
        }
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        for (auto& c : m_Children) {
            if (!c->IsEnd() && c->GetDocumentID() < target)
                c->GoUntil(target, limit);
        }
    }

    void Close() override { for (auto& c : m_Children) c->Close(); }

private:
    std::vector<std::shared_ptr<IndexReader>> m_Children;
};

/*
* NotIndexReader — base reader filtered by an exclusion reader.
*/
class NotIndexReader : public IndexReader {
public:
    NotIndexReader(std::shared_ptr<IndexReader> base,
                   std::shared_ptr<IndexReader> exclude)
        : m_Base(std::move(base))
        , m_Exclude(std::move(exclude))
    {
        SkipExcluded();
    }

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        std::println("{}[NOT]", std::string(depth * 2, ' '));
        std::println("{}  + base:", std::string(depth * 2, ' '));
        m_Base->SetDebug(label, depth + 2);
        std::println("{}  - excl:", std::string(depth * 2, ' '));
        m_Exclude->SetDebug(label, depth + 2);
    }

    bool     IsEnd()         override { return m_Base->IsEnd(); }
    uint64_t GetDocumentID() override { return m_Base->GetDocumentID(); }
    uint32_t GetTermFreq()   override { return m_Base->GetTermFreq(); }

    float GetScore(const DocDataEntry* entry) override
    {
        return m_Base->GetScore(entry);
    }

    uint8_t GetSourceMask() override { return m_Base->GetSourceMask(); }

    void GoNext() override
    {
        m_Base->GoNext();
        SkipExcluded();
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        m_Base->GoUntil(target, limit);
        SkipExcluded();
    }

    void Close() override { m_Base->Close(); m_Exclude->Close(); }

private:
    std::shared_ptr<IndexReader> m_Base;
    std::shared_ptr<IndexReader> m_Exclude;

    void SkipExcluded()
    {
        while (!m_Base->IsEnd()) {
            uint64_t doc = m_Base->GetDocumentID();

            m_Exclude->GoUntil(doc);

            if (!m_Exclude->IsEnd() && m_Exclude->GetDocumentID() == doc) {
                if (m_debug)
                    std::println("{}NOT excluded  doc {}",
                                 std::string(m_debugDepth * 2, ' '), doc);
                m_Base->GoNext();
            } else {
                break;
            }
        }
    }
};

class VectorIndexReader : public IndexReader {
public:
    explicit VectorIndexReader(std::vector<VectorSearchResult> results)
        : m_Results(std::move(results))
    {
        std::sort(m_Results.begin(), m_Results.end(), [](const auto& a, const auto& b) {
            return a.doc_id < b.doc_id;
        });
    }

    bool IsEnd() override { return m_Pos >= m_Results.size(); }

    uint64_t GetDocumentID() override
    {
        return IsEnd() ? NO_MORE_DOCS : m_Results[m_Pos].doc_id;
    }

    float GetScore(const DocDataEntry*) override
    {
        return IsEnd() ? 0.0f : m_Results[m_Pos].score;
    }

    uint8_t GetSourceMask() override { return IsEnd() ? 0 : READER_SOURCE_VECTOR; }

    void GoNext() override
    {
        if (!IsEnd()) ++m_Pos;
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        while (!IsEnd() && GetDocumentID() < target && GetDocumentID() < limit)
            ++m_Pos;
    }

    void Close() override { m_Pos = m_Results.size(); }

private:
    std::vector<VectorSearchResult> m_Results;
    size_t m_Pos = 0;
};

#endif

