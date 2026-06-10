#ifndef INDEXREADERIMPL_H__
#define INDEXREADERIMPL_H__

#include "IndexReader.h"
#include "PostingStore.h"
#include "Bm25Scorer.h"

#include <algorithm>
#include <memory>
#include <vector>

/*
* Sentinel: returned by GetDocumentID() when the reader is exhausted.
*/
static constexpr uint64_t NO_MORE_DOCS = UINT64_MAX;

/*
* TermIndexReader — leaf reader; iterates one posting list for a single
* (term + stream) key, e.g. "rustT" or "safetyB".
*/
class TermIndexReader : public IndexReader {
public:
    explicit TermIndexReader(const PostingList* postingList)
        : postingList_(postingList)
        , position_(0)
        , docFrequency_(postingList ? postingList->doc_freq() : 0u)
    {}

    void GoNext() override
    {
        if (postingList_ && position_ < postingList_->entries.size())
            ++position_;
    }

    void GoUntil(uint64_t target, uint64_t /*limit*/ = NO_MORE_DOCS) override
    {
        if (!postingList_)
            return;

        const auto& entries = postingList_->entries;
        auto it = std::lower_bound(
            entries.begin() + position_, entries.end(), target,
            [](const PostingEntry& entry, uint64_t docId) {
                return entry.doc_id < docId;
            });

        position_ = static_cast<size_t>(it - entries.begin());
    }

    bool IsEnd() override
    {
        return !postingList_ || position_ >= postingList_->entries.size();
    }

    uint64_t GetDocumentID() override
    {
        return IsEnd() ? NO_MORE_DOCS : postingList_->entries[position_].doc_id;
    }

    uint32_t GetTermFreq() override
    {
        return IsEnd() ? 0u : postingList_->entries[position_].tf;
    }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) override
    {
        return scorer.Score(GetTermFreq(), docLength, docFrequency_);
    }

    void Close() override
    {
        position_ = postingList_ ? postingList_->entries.size() : 0;
    }

    void Open(const char* /*streamKey*/) override {}

private:
    const PostingList* postingList_;
    size_t             position_;
    uint32_t           docFrequency_;
};

/*
* AndIndexReader — Document-At-A-Time intersection.
* All children must reach the same doc_id before a match is declared.
*/
class AndIndexReader : public IndexReader {
public:
    explicit AndIndexReader(std::vector<std::shared_ptr<IndexReader>> childReaders)
        : childReaders_(std::move(childReaders))
    {
        AlignToPivot();
    }

    bool IsEnd() override
    {
        if (childReaders_.empty())
            return true;

        for (auto& child : childReaders_) {
            if (child->IsEnd())
                return true;
        }

        return false;
    }

    uint64_t GetDocumentID() override
    {
        return IsEnd() ? NO_MORE_DOCS : childReaders_[0]->GetDocumentID();
    }

    uint32_t GetTermFreq() override
    {
        if (IsEnd())
            return 0u;

        uint32_t total = 0;
        for (auto& child : childReaders_)
            total += child->GetTermFreq();

        return total;
    }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) override
    {
        if (IsEnd())
            return 0.0f;

        float total = 0.0f;
        for (auto& child : childReaders_)
            total += child->GetBM25Score(scorer, docLength);

        return total;
    }

    void GoNext() override
    {
        if (IsEnd())
            return;

        uint64_t currentDoc = GetDocumentID();

        for (auto& child : childReaders_) {
            if (!child->IsEnd() && child->GetDocumentID() == currentDoc)
                child->GoNext();
        }

        AlignToPivot();
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        for (auto& child : childReaders_)
            child->GoUntil(target, limit);

        AlignToPivot();
    }

    void Close() override
    {
        for (auto& child : childReaders_) child->Close();
    }

    void Open(const char* /*streamKey*/) override {}

private:
    std::vector<std::shared_ptr<IndexReader>> childReaders_;

    /*
    * Seek all lagging children to the pivot (maximum current doc_id).
    * If a child lands past the pivot, restart with the new maximum.
    * Repeats until all children agree on the same doc_id or one is exhausted.
    */
    void AlignToPivot()
    {
        while (true) {
            if (IsEnd())
                return;

            uint64_t pivotDocId = 0;

            for (auto& child : childReaders_) {
                if (child->IsEnd())
                    return;

                pivotDocId = std::max(pivotDocId, child->GetDocumentID());
            }

            bool pivotFound = true;

            for (auto& child : childReaders_) {
                if (child->GetDocumentID() == pivotDocId)
                    continue;

                child->GoUntil(pivotDocId);

                if (child->IsEnd())
                    return;

                if (child->GetDocumentID() != pivotDocId) {
                    pivotFound = false;
                    break;
                }
            }

            if (pivotFound)
                return;
        }
    }
};

/*
* OrIndexReader — Document-At-A-Time union.
* Current document is the minimum doc_id across all non-exhausted children.
*/
class OrIndexReader : public IndexReader {
public:
    explicit OrIndexReader(std::vector<std::shared_ptr<IndexReader>> childReaders)
        : childReaders_(std::move(childReaders))
    {}

    bool IsEnd() override
    {
        for (auto& child : childReaders_) {
            if (!child->IsEnd())
                return false;
        }

        return true;
    }

    uint64_t GetDocumentID() override
    {
        uint64_t minDocId = NO_MORE_DOCS;

        for (auto& child : childReaders_) {
            if (!child->IsEnd())
                minDocId = std::min(minDocId, child->GetDocumentID());
        }

        return minDocId;
    }

    uint32_t GetTermFreq() override
    {
        uint64_t currentDoc = GetDocumentID();
        uint32_t total      = 0;

        for (auto& child : childReaders_) {
            if (!child->IsEnd() && child->GetDocumentID() == currentDoc)
                total += child->GetTermFreq();
        }

        return total;
    }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) override
    {
        uint64_t currentDoc = GetDocumentID();
        float    total      = 0.0f;

        for (auto& child : childReaders_) {
            if (!child->IsEnd() && child->GetDocumentID() == currentDoc)
                total += child->GetBM25Score(scorer, docLength);
        }

        return total;
    }

    void GoNext() override
    {
        if (IsEnd())
            return;

        uint64_t currentDoc = GetDocumentID();

        for (auto& child : childReaders_) {
            if (!child->IsEnd() && child->GetDocumentID() == currentDoc)
                child->GoNext();
        }
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        for (auto& child : childReaders_) {
            if (!child->IsEnd() && child->GetDocumentID() < target)
                child->GoUntil(target, limit);
        }
    }

    void Close() override
    {
        for (auto& child : childReaders_) child->Close();
    }

    void Open(const char* /*streamKey*/) override {}

private:
    std::vector<std::shared_ptr<IndexReader>> childReaders_;
};

/*
* NotIndexReader — returns documents from the base reader that are
* absent from the exclusion reader.
*/
class NotIndexReader : public IndexReader {
public:
    NotIndexReader(std::shared_ptr<IndexReader> baseReader,
                   std::shared_ptr<IndexReader> exclusionReader)
        : baseReader_(std::move(baseReader))
        , exclusionReader_(std::move(exclusionReader))
    {
        SkipExcluded();
    }

    bool     IsEnd()         override { return baseReader_->IsEnd(); }
    uint64_t GetDocumentID() override { return baseReader_->GetDocumentID(); }
    uint32_t GetTermFreq()   override { return baseReader_->GetTermFreq(); }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t docLength) override
    {
        return baseReader_->GetBM25Score(scorer, docLength);
    }

    void GoNext() override
    {
        baseReader_->GoNext();
        SkipExcluded();
    }

    void GoUntil(uint64_t target, uint64_t limit = NO_MORE_DOCS) override
    {
        baseReader_->GoUntil(target, limit);
        SkipExcluded();
    }

    void Close() override
    {
        baseReader_->Close();
        exclusionReader_->Close();
    }

    void Open(const char* /*streamKey*/) override {}

private:
    std::shared_ptr<IndexReader> baseReader_;
    std::shared_ptr<IndexReader> exclusionReader_;

    void SkipExcluded()
    {
        while (!baseReader_->IsEnd()) {
            uint64_t currentDoc = baseReader_->GetDocumentID();

            exclusionReader_->GoUntil(currentDoc);

            if (!exclusionReader_->IsEnd() &&
                exclusionReader_->GetDocumentID() == currentDoc)
            {
                baseReader_->GoNext();
            }
            else
            {
                break;
            }
        }
    }
};

#endif
