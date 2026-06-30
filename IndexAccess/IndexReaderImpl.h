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
    {
        InitializeChildDocs();
        RefreshCurrentDoc();
    }

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        std::println("{}[OR]", std::string(depth * 2, ' '));
        for (auto& c : m_Children) c->SetDebug(label, depth + 1);
    }

    bool IsEnd() override
    {
        return m_CurrentDoc == NO_MORE_DOCS;
    }

    uint64_t GetDocumentID() override
    {
        return m_CurrentDoc;
    }

    uint32_t GetTermFreq() override
    {
        uint32_t total = 0;

        for (const size_t childIndex : m_MatchingChildren)
            total += m_Children[childIndex]->GetTermFreq();

        return total;
    }

    float GetScore(const DocDataEntry* entry) override
    {
        float    total = 0.0f;

        for (const size_t childIndex : m_MatchingChildren)
            total += m_Children[childIndex]->GetScore(entry);

        return total;
    }

    uint8_t GetSourceMask() override
    {
        uint8_t sourceMask = 0;

        for (const size_t childIndex : m_MatchingChildren)
            sourceMask |= m_Children[childIndex]->GetSourceMask();

        return sourceMask;
    }

    void GoNext() override
    {
        if (IsEnd())
            return;

        for (const size_t childIndex : m_MatchingChildren) {
            m_Children[childIndex]->GoNext();
            UpdateChildDoc(childIndex);
        }
        RefreshCurrentDoc();
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        for (size_t i = 0; i < m_Children.size(); ++i) {
            if (m_ChildDocs[i] < target) {
                m_Children[i]->GoUntil(target, limit);
                UpdateChildDoc(i);
            }
        }
        RefreshCurrentDoc();
    }

    void Close() override { for (auto& c : m_Children) c->Close(); }

private:
    std::vector<std::shared_ptr<IndexReader>> m_Children;
    std::vector<uint64_t> m_ChildDocs;
    std::vector<size_t> m_MatchingChildren;
    uint64_t m_CurrentDoc = NO_MORE_DOCS;

    void InitializeChildDocs()
    {
        m_ChildDocs.resize(m_Children.size());
        for (size_t i = 0; i < m_Children.size(); ++i)
            UpdateChildDoc(i);
    }

    void UpdateChildDoc(size_t childIndex)
    {
        auto& child = m_Children[childIndex];
        m_ChildDocs[childIndex] = (!child || child->IsEnd()) ? NO_MORE_DOCS : child->GetDocumentID();
    }

    void RefreshCurrentDoc()
    {
        m_CurrentDoc = NO_MORE_DOCS;
        for (const uint64_t childDoc : m_ChildDocs)
            m_CurrentDoc = std::min(m_CurrentDoc, childDoc);

        m_MatchingChildren.clear();
        if (m_CurrentDoc == NO_MORE_DOCS)
            return;

        for (size_t i = 0; i < m_ChildDocs.size(); ++i) {
            if (m_ChildDocs[i] == m_CurrentDoc)
                m_MatchingChildren.push_back(i);
        }
    }
};

/*
* WeakAndIndexReader — DAAT soft intersection.
* Returns a document when at least m_MinShouldMatch children match it.
*/
class WeakAndIndexReader : public IndexReader {
public:
    WeakAndIndexReader(std::vector<std::shared_ptr<IndexReader>> children,
                       uint32_t minShouldMatch)
        : m_Children(std::move(children))
        , m_MinShouldMatch(std::max<uint32_t>(1, minShouldMatch))
    {
        InitializeChildDocs();
        AlignToMatch();
    }

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        std::println("{}[WEAK-AND min={}]", std::string(depth * 2, ' '), m_MinShouldMatch);
        for (auto& c : m_Children) c->SetDebug(label, depth + 1);
    }

    bool IsEnd() override { return m_CurrentDoc == NO_MORE_DOCS; }

    uint64_t GetDocumentID() override { return m_CurrentDoc; }

    uint32_t GetTermFreq() override
    {
        uint32_t total = 0;
        for (const size_t childIndex : m_MatchingChildren)
            total += m_Children[childIndex]->GetTermFreq();
        return total;
    }

    float GetScore(const DocDataEntry* entry) override
    {
        float total = 0.0f;
        for (const size_t childIndex : m_MatchingChildren)
            total += m_Children[childIndex]->GetScore(entry);
        return total;
    }

    uint8_t GetSourceMask() override
    {
        uint8_t sourceMask = 0;
        for (const size_t childIndex : m_MatchingChildren)
            sourceMask |= m_Children[childIndex]->GetSourceMask();
        return sourceMask;
    }

    void GoNext() override
    {
        if (IsEnd()) return;
        for (const size_t childIndex : m_MatchingChildren) {
            m_Children[childIndex]->GoNext();
            UpdateChildDoc(childIndex);
        }
        AlignToMatch();
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        for (size_t i = 0; i < m_Children.size(); ++i) {
            if (m_ChildDocs[i] < target) {
                m_Children[i]->GoUntil(target, limit);
                UpdateChildDoc(i);
            }
        }
        AlignToMatch();
    }

    void Close() override { for (auto& c : m_Children) c->Close(); }

private:
    std::vector<std::shared_ptr<IndexReader>> m_Children;
    std::vector<uint64_t> m_ChildDocs;
    std::vector<size_t> m_MatchingChildren;
    uint32_t m_MinShouldMatch = 1;
    uint64_t m_CurrentDoc = NO_MORE_DOCS;

    void InitializeChildDocs()
    {
        m_ChildDocs.resize(m_Children.size());
        for (size_t i = 0; i < m_Children.size(); ++i)
            UpdateChildDoc(i);
    }

    void UpdateChildDoc(size_t childIndex)
    {
        auto& child = m_Children[childIndex];
        m_ChildDocs[childIndex] = (!child || child->IsEnd()) ? NO_MORE_DOCS : child->GetDocumentID();
    }

    void AlignToMatch()
    {
        while (true) {
            uint64_t doc = NO_MORE_DOCS;
            for (const uint64_t childDoc : m_ChildDocs)
                doc = std::min(doc, childDoc);

            if (doc == NO_MORE_DOCS) {
                m_CurrentDoc = NO_MORE_DOCS;
                m_MatchingChildren.clear();
                return;
            }

            m_MatchingChildren.clear();
            for (size_t i = 0; i < m_ChildDocs.size(); ++i) {
                if (m_ChildDocs[i] == doc)
                    m_MatchingChildren.push_back(i);
            }

            if (m_MatchingChildren.size() >= m_MinShouldMatch) {
                m_CurrentDoc = doc;
                if (m_debug)
                    std::println("{}WEAK-AND match doc {} children={}",
                                 std::string(m_debugDepth * 2, ' '), doc, m_MatchingChildren.size());
                return;
            }

            for (const size_t childIndex : m_MatchingChildren) {
                m_Children[childIndex]->GoNext();
                UpdateChildDoc(childIndex);
            }
        }
    }
};

class BoostIndexReader : public IndexReader {
public:
    BoostIndexReader(std::shared_ptr<IndexReader> base,
                     std::shared_ptr<IndexReader> boost,
                     float boostWeight)
        : m_Base(std::move(base))
        , m_Boost(std::move(boost))
        , m_BoostWeight(boostWeight)
    {
        m_CurrentDoc = (!m_Base || m_Base->IsEnd()) ? NO_MORE_DOCS : m_Base->GetDocumentID();
        m_BoostDoc = (!m_Boost || m_Boost->IsEnd()) ? NO_MORE_DOCS : m_Boost->GetDocumentID();
    }

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        std::println("{}[BOOST weight={}]", std::string(depth * 2, ' '), m_BoostWeight);
        if (m_Base) m_Base->SetDebug(label, depth + 1);
        if (m_Boost) m_Boost->SetDebug(label, depth + 1);
    }

    bool IsEnd() override { return m_CurrentDoc == NO_MORE_DOCS; }

    uint64_t GetDocumentID() override { return m_CurrentDoc; }

    uint32_t GetTermFreq() override { return IsEnd() ? 0 : m_Base->GetTermFreq(); }

    float GetScore(const DocDataEntry* entry) override
    {
        if (IsEnd()) return 0.0f;

        float total = m_Base->GetScore(entry);
        if (BoostMatchesBase())
            total += m_BoostWeight;
        return total;
    }

    uint8_t GetSourceMask() override
    {
        if (IsEnd()) return 0;

        uint8_t sourceMask = m_Base->GetSourceMask();
        if (BoostMatchesBase())
            sourceMask |= m_Boost->GetSourceMask();
        return sourceMask;
    }

    void GoNext() override
    {
        if (!IsEnd()) {
            m_Base->GoNext();
            m_CurrentDoc = (!m_Base || m_Base->IsEnd()) ? NO_MORE_DOCS : m_Base->GetDocumentID();
        }
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        if (m_Base) {
            m_Base->GoUntil(target, limit);
            m_CurrentDoc = (!m_Base || m_Base->IsEnd()) ? NO_MORE_DOCS : m_Base->GetDocumentID();
        }
    }

    void Close() override
    {
        if (m_Base) m_Base->Close();
        if (m_Boost) m_Boost->Close();
        m_CurrentDoc = NO_MORE_DOCS;
        m_BoostDoc = NO_MORE_DOCS;
    }

private:
    std::shared_ptr<IndexReader> m_Base;
    std::shared_ptr<IndexReader> m_Boost;
    float m_BoostWeight = 1.0f;
    uint64_t m_CurrentDoc = NO_MORE_DOCS;
    uint64_t m_BoostDoc = NO_MORE_DOCS;

    bool BoostMatchesBase()
    {
        if (!m_Boost || m_BoostDoc == NO_MORE_DOCS || IsEnd())
            return false;

        if (m_BoostDoc < m_CurrentDoc) {
            m_Boost->GoUntil(m_CurrentDoc);
            m_BoostDoc = (!m_Boost || m_Boost->IsEnd()) ? NO_MORE_DOCS : m_Boost->GetDocumentID();
        }

        return m_BoostDoc == m_CurrentDoc;
    }
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

