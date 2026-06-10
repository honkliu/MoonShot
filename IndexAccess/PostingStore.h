#ifndef POSTINGSTORE_H__
#define POSTINGSTORE_H__

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

/*
* One (docId, termFrequency) pair inside a posting list.
*/
struct PostingEntry {
    uint64_t doc_id;
    uint32_t tf;
};

/*
* Sorted array of entries for one (term+stream) key, e.g. "foxT".
*/
struct PostingList {
    std::vector<PostingEntry> entries;

    uint32_t doc_freq() const { return static_cast<uint32_t>(entries.size()); }
};

/*
* Per-document metadata needed for ranking.
*/
struct DocStats {
    uint32_t doc_len    = 0;
    float    importance = 0.0f;
};

/*
* Central in-memory inverted index.
* Owns all posting lists and per-document statistics.
*/
class PostingStore {
public:

    void AddPosting(const std::string& stream_key, uint64_t doc_id, uint32_t tf)
    {
        auto& pl = postings_[stream_key];
        auto it = std::lower_bound(pl.entries.begin(), pl.entries.end(), doc_id,
            [](const PostingEntry& e, uint64_t d) { return e.doc_id < d; });
        if (it != pl.entries.end() && it->doc_id == doc_id) {
            it->tf += tf;
        } else {
            pl.entries.insert(it, PostingEntry{doc_id, tf});
        }
    }

    void AddDocTokens(uint64_t doc_id, uint32_t count)
    {
        doc_stats_[doc_id].doc_len += count;
        total_terms_ += count;
    }

    void SetDocImportance(uint64_t doc_id, float score)
    {
        doc_stats_[doc_id].importance = score;
    }

    const PostingList* GetPostingList(const std::string& key) const
    {
        auto it = postings_.find(key);
        return it != postings_.end() ? &it->second : nullptr;
    }

    uint32_t GetDocLen(uint64_t doc_id) const
    {
        auto it = doc_stats_.find(doc_id);
        return it != doc_stats_.end() ? it->second.doc_len : 1u;
    }

    float GetDocImportance(uint64_t doc_id) const
    {
        auto it = doc_stats_.find(doc_id);
        return it != doc_stats_.end() ? it->second.importance : 0.0f;
    }

    uint64_t TotalDocs()  const { return doc_stats_.size(); }

    float AvgDocLen() const
    {
        if (doc_stats_.empty()) return 1.0f;
        return static_cast<float>(total_terms_) /
               static_cast<float>(doc_stats_.size());
    }

    uint32_t DocFreq(const std::string& key) const
    {
        const auto* pl = GetPostingList(key);
        return pl ? pl->doc_freq() : 0u;
    }

    bool HasDoc(uint64_t doc_id) const
    {
        return doc_stats_.find(doc_id) != doc_stats_.end();
    }

    const std::unordered_map<std::string, PostingList>& AllPostings() const
    {
        return postings_;
    }

    const std::unordered_map<uint64_t, DocStats>& AllDocStats() const
    {
        return doc_stats_;
    }

    uint64_t TotalTerms() const { return total_terms_; }

private:
    std::unordered_map<std::string, PostingList> postings_;
    std::unordered_map<uint64_t, DocStats>       doc_stats_;
    uint64_t total_terms_ = 0;
};

#endif
