#ifndef ISRIMPL_H__
#define ISRIMPL_H__

#include "IndexReader.h"
#include "PostingStore.h"
#include "Bm25Scorer.h"

#include <algorithm>
#include <memory>
#include <vector>

static constexpr uint64_t ISR_END = UINT64_MAX;

/*
* TermIsr — iterates over one posting list for a single (term + stream) key.
*/
class TermIsr : public IndexReader {
public:
    explicit TermIsr(const PostingList* list)
        : list_(list), pos_(0)
        , doc_freq_(list ? list->doc_freq() : 0u)
    {}

    void GoNext() override
    {
        if (list_ && pos_ < list_->entries.size()) ++pos_;
    }

    void GoUntil(uint64_t target, uint64_t /*limit*/ = ISR_END) override
    {
        if (!list_) return;
        const auto& e = list_->entries;
        auto it = std::lower_bound(e.begin() + pos_, e.end(), target,
            [](const PostingEntry& pe, uint64_t t){ return pe.doc_id < t; });
        pos_ = static_cast<size_t>(it - e.begin());
    }

    bool IsEnd() override
    {
        return !list_ || pos_ >= list_->entries.size();
    }

    uint64_t GetDocumentID() override
    {
        return IsEnd() ? ISR_END : list_->entries[pos_].doc_id;
    }

    uint32_t GetTermFreq() override
    {
        return IsEnd() ? 0u : list_->entries[pos_].tf;
    }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t doc_len) override
    {
        return scorer.Score(GetTermFreq(), doc_len, doc_freq_);
    }

    void Close() override { pos_ = list_ ? list_->entries.size() : 0; }
    void Open(const char* /*word*/) override {}

private:
    const PostingList* list_;
    size_t             pos_;
    uint32_t           doc_freq_;
};

/*
* AndIsr — DAAT intersection; all children must be at the same doc_id.
*/
class AndIsr : public IndexReader {
public:
    explicit AndIsr(std::vector<std::shared_ptr<IndexReader>> children)
        : children_(std::move(children))
    {
        Align();
    }

    bool IsEnd() override
    {
        if (children_.empty()) return true;
        for (auto& c : children_) if (c->IsEnd()) return true;
        return false;
    }

    uint64_t GetDocumentID() override
    {
        return IsEnd() ? ISR_END : children_[0]->GetDocumentID();
    }

    uint32_t GetTermFreq() override
    {
        if (IsEnd()) return 0u;
        uint32_t total = 0;
        for (auto& c : children_) total += c->GetTermFreq();
        return total;
    }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t doc_len) override
    {
        if (IsEnd()) return 0.0f;
        float total = 0.0f;
        for (auto& c : children_) total += c->GetBM25Score(scorer, doc_len);
        return total;
    }

    void GoNext() override
    {
        if (IsEnd()) return;
        uint64_t doc = GetDocumentID();
        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                c->GoNext();
        }
        Align();
    }

    void GoUntil(uint64_t target, uint64_t limit = ISR_END) override
    {
        for (auto& c : children_) c->GoUntil(target, limit);
        Align();
    }

    void Close() override { for (auto& c : children_) c->Close(); }
    void Open(const char* /*word*/) override {}

private:
    std::vector<std::shared_ptr<IndexReader>> children_;

    void Align()
    {
        /*
        * Find the global max doc_id; seek lagging children to it.
        * If a child lands past max_doc (that doc is absent from its list),
        * restart with the new max rather than returning early.
        */
        while (true) {
            if (IsEnd()) return;

            uint64_t max_doc = 0;
            for (auto& c : children_) {
                if (c->IsEnd()) return;
                max_doc = std::max(max_doc, c->GetDocumentID());
            }

            bool all_aligned = true;
            for (auto& c : children_) {
                if (c->GetDocumentID() == max_doc) continue;
                c->GoUntil(max_doc);
                if (c->IsEnd()) return;
                if (c->GetDocumentID() != max_doc) {
                    all_aligned = false;
                    break;
                }
            }
            if (all_aligned) return;
        }
    }
};

/*
* OrIsr — DAAT union; current doc is the minimum across all children.
*/
class OrIsr : public IndexReader {
public:
    explicit OrIsr(std::vector<std::shared_ptr<IndexReader>> children)
        : children_(std::move(children))
    {}

    bool IsEnd() override
    {
        for (auto& c : children_) if (!c->IsEnd()) return false;
        return true;
    }

    uint64_t GetDocumentID() override
    {
        uint64_t min_doc = ISR_END;
        for (auto& c : children_) {
            if (!c->IsEnd())
                min_doc = std::min(min_doc, c->GetDocumentID());
        }
        return min_doc;
    }

    uint32_t GetTermFreq() override
    {
        uint64_t doc = GetDocumentID();
        uint32_t total = 0;
        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                total += c->GetTermFreq();
        }
        return total;
    }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t doc_len) override
    {
        uint64_t doc = GetDocumentID();
        float total = 0.0f;
        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                total += c->GetBM25Score(scorer, doc_len);
        }
        return total;
    }

    void GoNext() override
    {
        if (IsEnd()) return;
        uint64_t doc = GetDocumentID();
        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() == doc)
                c->GoNext();
        }
    }

    void GoUntil(uint64_t target, uint64_t limit = ISR_END) override
    {
        for (auto& c : children_) {
            if (!c->IsEnd() && c->GetDocumentID() < target)
                c->GoUntil(target, limit);
        }
    }

    void Close() override { for (auto& c : children_) c->Close(); }
    void Open(const char* /*word*/) override {}

private:
    std::vector<std::shared_ptr<IndexReader>> children_;
};

/*
* NotIsr — returns docs from base that are absent from exclude.
*/
class NotIsr : public IndexReader {
public:
    NotIsr(std::shared_ptr<IndexReader> base,
           std::shared_ptr<IndexReader> exclude)
        : base_(std::move(base)), exclude_(std::move(exclude))
    {
        SkipExcluded();
    }

    bool     IsEnd()         override { return base_->IsEnd(); }
    uint64_t GetDocumentID() override { return base_->GetDocumentID(); }
    uint32_t GetTermFreq()   override { return base_->GetTermFreq(); }

    float GetBM25Score(const Bm25Scorer& scorer, uint32_t doc_len) override
    {
        return base_->GetBM25Score(scorer, doc_len);
    }

    void GoNext() override
    {
        base_->GoNext();
        SkipExcluded();
    }

    void GoUntil(uint64_t target, uint64_t limit = ISR_END) override
    {
        base_->GoUntil(target, limit);
        SkipExcluded();
    }

    void Close() override { base_->Close(); exclude_->Close(); }
    void Open(const char* /*word*/) override {}

private:
    std::shared_ptr<IndexReader> base_;
    std::shared_ptr<IndexReader> exclude_;

    void SkipExcluded()
    {
        while (!base_->IsEnd()) {
            uint64_t doc = base_->GetDocumentID();
            exclude_->GoUntil(doc);
            if (!exclude_->IsEnd() && exclude_->GetDocumentID() == doc)
                base_->GoNext();
            else
                break;
        }
    }
};

#endif
