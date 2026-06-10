#ifndef ADVANCEDINDEXWRITER_H__
#define ADVANCEDINDEXWRITER_H__

#include "IndexWriter.h"
#include "PostingStore.h"

#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>

/*
* Concrete writer that populates a PostingStore.
*
* Write() maps the stream name to its abbreviation (T, B, A, U, M),
* counts term frequencies, and appends entries to the shared store.
*/
class AdvancedIndexWriter : public IndexWriter {
public:
    explicit AdvancedIndexWriter(std::shared_ptr<PostingStore> store)
        : store_(std::move(store))
    {}

    void Write(std::vector<std::string>&& words,
               uint64_t                    documentId,
               const char*                 postingType) override
    {
        if (!store_ || words.empty())
            return;

        const std::string abbrev = StreamAbbrev(postingType);

        std::unordered_map<std::string, uint32_t> tf_map;
        for (auto& word : words) {
            if (!word.empty())
                ++tf_map[word];
        }

        for (auto& [term, tf] : tf_map)
            store_->AddPosting(term + abbrev, documentId, tf);

        store_->AddDocTokens(documentId,
                             static_cast<uint32_t>(words.size()));
    }

    void SetDocImportance(uint64_t doc_id, float score) override
    {
        store_->SetDocImportance(doc_id, score);
    }

private:
    std::shared_ptr<PostingStore> store_;

    /*
    * Map a stream name or single-char abbreviation to the canonical letter.
    */
    static std::string StreamAbbrev(const char* stream)
    {
        if (!stream || !*stream)
            return "B";

        switch (stream[0]) {
            case 'A': case 'a': return "A";
            case 'U': case 'u': return "U";
            case 'T': case 't': return "T";
            case 'B': case 'b': return "B";
            case 'M': case 'm': return "M";
            default: break;
        }

        std::string lower(stream);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "title")  return "T";
        if (lower == "body")   return "B";
        if (lower == "anchor") return "A";
        if (lower == "url")    return "U";
        if (lower == "meta")   return "M";

        return "B";
    }
};

#endif
