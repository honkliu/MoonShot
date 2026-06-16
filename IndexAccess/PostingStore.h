#ifndef POSTINGSTORE_H__
#define POSTINGSTORE_H__

#include "Embeddings.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

/*
* One (docId, termFrequency) pair inside a posting list.
*/
struct IndexEntry {
    uint64_t IE_DocID;
    uint32_t IE_TermFrequency;
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
    std::vector<IndexEntry>      entries;
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
            vb_write(e.IE_DocID - prev, bytes);
            vb_write(e.IE_TermFrequency, bytes);
            prev = e.IE_DocID;
        }
    }
};

/*
* Per-document metadata needed for ranking.
*/
struct DocStats {
    uint32_t    doc_len    = 0;
    float       importance = 0.0f;
    std::string path;              // file path stored in DocData
};

/*
* Central in-memory inverted index.
* Owns all posting lists and per-document statistics.
*/
class PostingStore {
public:

    void AddPosting(const std::string& stream_key, uint64_t doc_id, uint32_t tf)
    {
        auto& pl = m_Postings[stream_key];
        auto  it = std::lower_bound(pl.entries.begin(), pl.entries.end(), doc_id,
            [](const IndexEntry& e, uint64_t d) { return e.IE_DocID < d; });

        if (it != pl.entries.end() && it->IE_DocID == doc_id) {
            it->IE_TermFrequency += tf;
        } else {
            pl.entries.insert(it, IndexEntry{doc_id, tf});
            ++m_PostingEntries;
        }

        /*
        * Byte cache is now stale — clear it so the next GetBytes() call
        * re-encodes the updated entries list.
        */
        pl.InvalidateBytes();
    }

    void AddDocTokens(uint64_t doc_id, uint32_t count)
    {
        m_DocStats[doc_id].doc_len += count;
        m_TotalTerms += count;
    }

    void SetDocImportance(uint64_t doc_id, float score)
    {
        m_DocStats[doc_id].importance = score;
    }

    void SetDocPath(uint64_t doc_id, const std::string& path)
    {
        m_DocStats[doc_id].path = path;
    }

    const std::string& GetDocPath(uint64_t doc_id) const
    {
        static const std::string empty;
        auto it = m_DocStats.find(doc_id);
        return it != m_DocStats.end() ? it->second.path : empty;
    }

    const PostingList* GetPostingList(const std::string& key) const
    {
        auto it = m_Postings.find(key);
        return it != m_Postings.end() ? &it->second : nullptr;
    }

    uint32_t GetDocLen(uint64_t doc_id) const
    {
        auto it = m_DocStats.find(doc_id);
        return it != m_DocStats.end() ? it->second.doc_len : 1u;
    }

    float GetDocImportance(uint64_t doc_id) const
    {
        auto it = m_DocStats.find(doc_id);
        return it != m_DocStats.end() ? it->second.importance : 0.0f;
    }

    uint64_t TotalDocs()  const { return m_DocStats.size(); }

    float AvgDocLen() const
    {
        if (m_DocStats.empty())
            return 1.0f;

        return static_cast<float>(m_TotalTerms) /
               static_cast<float>(m_DocStats.size());
    }

    uint32_t DocFreq(const std::string& key) const
    {
        const auto* pl = GetPostingList(key);
        return pl ? pl->doc_freq() : 0u;
    }

    bool HasDoc(uint64_t doc_id) const
    {
        return m_DocStats.find(doc_id) != m_DocStats.end();
    }

    const std::unordered_map<std::string, PostingList>& AllPostings() const
    {
        return m_Postings;
    }

    const std::unordered_map<uint64_t, DocStats>& AllDocStats() const
    {
        return m_DocStats;
    }

    bool SetDocVector(uint64_t doc_id, std::vector<float> vector)
    {
        if (!HasDoc(doc_id))
            m_DocStats[doc_id];
        return m_VectorIndex.Add(doc_id, std::move(vector));
    }

    const std::vector<float>* GetDocVector(uint64_t doc_id) const
    {
        return m_VectorIndex.Get(doc_id);
    }

    std::vector<VectorSearchResult> VectorSearch(const std::vector<float>& query,
                                                 size_t topK = 20,
                                                 VectorMetric metric = VectorMetric::Cosine,
                                                 size_t efSearch = 200) const
    {
        return m_VectorIndex.Search(query, topK, metric, efSearch);
    }

    const std::unordered_map<uint64_t, std::vector<float>>& AllDocVectors() const
    {
        return m_VectorIndex.AllVectors();
    }

    FreshDiskAnnVectorIndex& MutableVectorIndex() { return m_VectorIndex; }
    const FreshDiskAnnVectorIndex& GetVectorIndex() const { return m_VectorIndex; }

    size_t VectorDimension() const { return m_VectorIndex.Dimension(); }
    size_t VectorCount() const { return m_VectorIndex.Size(); }

    uint64_t TotalTerms() const { return m_TotalTerms; }

    uint64_t TotalPostingEntries() const { return m_PostingEntries; }

    size_t UniqueTermCount() const { return m_Postings.size(); }

private:
    std::unordered_map<std::string, PostingList> m_Postings;
    std::unordered_map<uint64_t, DocStats>       m_DocStats;
    FreshDiskAnnVectorIndex                      m_VectorIndex;
    uint64_t m_TotalTerms = 0;
    uint64_t m_PostingEntries = 0;
};

#endif
