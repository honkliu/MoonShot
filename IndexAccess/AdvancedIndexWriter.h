#ifndef ADVANCEDINDEXWRITER_H__
#define ADVANCEDINDEXWRITER_H__

#include "IndexWriter.h"
#include "PostingStore.h"
#include "EvalExpression.h"  /* BIGRAM_SEP */

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
        : m_Store(std::move(store))
    {}

    void Write(const std::vector<std::string>& words,
               uint64_t                    documentId,
               const char*                 postingType) override
    {
        if (!m_Store || words.empty())
            return;

        const std::string abbrev = StreamAbbrev(postingType);

        std::unordered_map<std::string, uint32_t> termTf;
        termTf.reserve(words.size() * 2);
        for (const auto& word : words) {
            if (!word.empty())
                ++termTf[word];
        }
        const uint32_t uniqueUnigramCount = static_cast<uint32_t>(termTf.size());

        /*
        * Index bigrams: adjacent pairs joined by BIGRAM_SEP (\x1F).
        * Using \x1F (Unit Separator) instead of '_' mirrors REF's
        * CreateBigramString approach: the separator is never produced by
        * ICU's word breaker, so "morning\x1FcallT" (bigram) is unambiguous
        * from "morning_callT" (unigram token containing an underscore).
        */
        for (size_t i = 0; i + 1 < words.size(); ++i) {
            if (!words[i].empty() && !words[i + 1].empty()) {
                std::string bigram = words[i] + BIGRAM_SEP + words[i + 1];
                ++termTf[bigram];
            }
        }

        for (auto& [term, tf] : termTf)
            m_Store->AddPosting(term + abbrev, documentId, tf);

        m_Store->AddDocTokens(documentId,
                             static_cast<uint32_t>(words.size()));
        m_Store->AddStreamStats(documentId,
                    abbrev.empty() ? 'B' : abbrev[0],
                    static_cast<uint32_t>(words.size()),
                    uniqueUnigramCount);
    }

    void SetDocImportance(uint64_t doc_id, float score) override
    {
        m_Store->SetDocImportance(doc_id, score);
    }

    void SetDocVector(uint64_t doc_id, std::vector<float> vector) override
    {
        m_Store->SetDocVector(doc_id, std::move(vector));
    }

private:
    std::shared_ptr<PostingStore> m_Store;

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
