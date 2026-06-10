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
*
* entries  — decoded form; used by the writer to accumulate postings.
* bytes    — VarByte-delta-encoded form; used by TermIndexReader via
*             UnifiedDecoder::OpenRaw().  Computed lazily on first call to
*             GetBytes() so encoding only runs once per posting list.
*/
struct PostingList {
    std::vector<PostingEntry>    entries;
    mutable std::vector<uint8_t> bytes;

    uint32_t doc_freq() const { return static_cast<uint32_t>(entries.size()); }

    /*
    * Return the VarByte-encoded representation, encoding lazily on first call.
    */
    const std::vector<uint8_t>& GetBytes() const
    {
        if (bytes.empty() && !entries.empty())
            Encode();
        return bytes;
    }

    /*
    * Invalidate the byte cache when entries are modified.
    */
    void InvalidateBytes()
    {
        bytes.clear();
    }

private:
    static void vb_write(uint64_t v, std::vector<uint8_t>& out)
    {
        while (v >= 0x80u) {
            out.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
            v >>= 7;
        }
        out.push_back(static_cast<uint8_t>(v));
    }

    void Encode() const
    {
        bytes.clear();
        bytes.reserve(entries.size() * 3);

        uint64_t prev = 0;
        for (const auto& e : entries) {
            vb_write(e.doc_id - prev, bytes);
            vb_write(e.tf,            bytes);
            prev = e.doc_id;
        }
    }
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
        auto  it = std::lower_bound(pl.entries.begin(), pl.entries.end(), doc_id,
            [](const PostingEntry& e, uint64_t d) { return e.doc_id < d; });

        if (it != pl.entries.end() && it->doc_id == doc_id) {
            it->tf += tf;
        } else {
            pl.entries.insert(it, PostingEntry{doc_id, tf});
        }

        /*
        * Byte cache is now stale — clear it so the next GetBytes() call
        * re-encodes the updated entries list.
        */
        pl.InvalidateBytes();
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
        if (doc_stats_.empty())
            return 1.0f;

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
