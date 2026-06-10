#ifndef INDEXREADERIMPL_H__
#define INDEXREADERIMPL_H__

#include "IndexReader.h"
#include "Bm25Scorer.h"

#include <algorithm>
#include <cinttypes>
#include <memory>
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
        : children_(std::move(children))
    {
        AlignToPivot();
    }

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        printf("%*s[AND]\n", depth * 2, "");
        for (auto& c : children_) c->SetDebug(label, depth + 1);
    }

    bool IsEnd() override
    {
        if (children_.empty())
            return true;
        for (auto& c : children_)
            if (c->IsEnd()) return true;
        return false;
    }

    uint64_t GetDocumentID() override
    {
        return IsEnd() ? NO_MORE_DOCS : children_[0]->GetDocumentID();
    }

    uint32_t GetTermFreq() override
    {
        uint32_t total = 0;
        for (auto& c : children_)
            total += c->GetTermFreq();
        return total;
    }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) override
    {
        float total = 0.0f;
        for (auto& c : children_)
            total += c->GetBM25Score(scorer, docLength);
        return total;
    }

    void GoNext() override
    {
        if (IsEnd())
            return;

        uint64_t doc = GetDocumentID();

        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                c->GoNext();
        }

        AlignToPivot();
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        for (auto& c : children_)
            c->GoUntil(target, limit);

        AlignToPivot();
    }

    void Close() override { for (auto& c : children_) c->Close(); }
    void Open(const char*) override {}

private:
    std::vector<std::shared_ptr<IndexReader>> children_;

    void AlignToPivot()
    {
        while (true) {
            if (IsEnd())
                return;

            uint64_t pivot = 0;

            for (auto& c : children_) {
                if (c->IsEnd())
                    return;

                pivot = std::max(pivot, c->GetDocumentID());
            }

            bool aligned = true;

            for (auto& c : children_) {
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
                    printf("%*sAND match  doc %" PRIu64 "\n",
                           m_debugDepth * 2, "", pivot);
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
        : children_(std::move(children))
    {}

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        printf("%*s[OR]\n", depth * 2, "");
        for (auto& c : children_) c->SetDebug(label, depth + 1);
    }

    bool IsEnd() override
    {
        for (auto& c : children_) {
            if (!c->IsEnd())
                return false;
        }
        return true;
    }

    uint64_t GetDocumentID() override
    {
        uint64_t minDoc = NO_MORE_DOCS;

        for (auto& c : children_) {
            if (!c->IsEnd())
                minDoc = std::min(minDoc, c->GetDocumentID());
        }

        return minDoc;
    }

    uint32_t GetTermFreq() override
    {
        uint64_t doc   = GetDocumentID();
        uint32_t total = 0;

        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                total += c->GetTermFreq();
        }

        return total;
    }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) override
    {
        uint64_t doc   = GetDocumentID();
        float    total = 0.0f;

        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                total += c->GetBM25Score(scorer, docLength);
        }

        return total;
    }

    void GoNext() override
    {
        if (IsEnd())
            return;

        uint64_t doc = GetDocumentID();

        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                c->GoNext();
        }
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() < target)
                c->GoUntil(target, limit);
        }
    }

    void Close() override { for (auto& c : children_) c->Close(); }
    void Open(const char*) override {}

private:
    std::vector<std::shared_ptr<IndexReader>> children_;
};

/*
* NotIndexReader — base reader filtered by an exclusion reader.
*/
class NotIndexReader : public IndexReader {
public:
    NotIndexReader(std::shared_ptr<IndexReader> base,
                   std::shared_ptr<IndexReader> exclude)
        : base_(std::move(base))
        , exclude_(std::move(exclude))
    {
        SkipExcluded();
    }

    void SetDebug(const char* label, int depth = 0) override
    {
        IndexReader::SetDebug(label, depth);
        printf("%*s[NOT]\n", depth * 2, "");
        printf("%*s  + base:\n", depth * 2, "");
        base_->SetDebug(label, depth + 2);
        printf("%*s  - excl:\n", depth * 2, "");
        exclude_->SetDebug(label, depth + 2);
    }

    bool     IsEnd()         override { return base_->IsEnd(); }
    uint64_t GetDocumentID() override { return base_->GetDocumentID(); }
    uint32_t GetTermFreq()   override { return base_->GetTermFreq(); }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) override
    {
        return base_->GetBM25Score(scorer, docLength);
    }

    void GoNext() override
    {
        base_->GoNext();
        SkipExcluded();
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        base_->GoUntil(target, limit);
        SkipExcluded();
    }

    void Close() override { base_->Close(); exclude_->Close(); }
    void Open(const char*) override {}

private:
    std::shared_ptr<IndexReader> base_;
    std::shared_ptr<IndexReader> exclude_;

    void SkipExcluded()
    {
        while (!base_->IsEnd()) {
            uint64_t doc = base_->GetDocumentID();

            exclude_->GoUntil(doc);

            if (!exclude_->IsEnd() && exclude_->GetDocumentID() == doc) {
                if (m_debug)
                    printf("%*sNOT excluded  doc %" PRIu64 "\n",
                           m_debugDepth * 2, "", doc);
                base_->GoNext();
            } else {
                break;
            }
        }
    }
};

#endif

