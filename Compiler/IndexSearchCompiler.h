#ifndef INDEXSEARCHCOMPILER_H__
#define INDEXSEARCHCOMPILER_H__

#include "EvalExpression.h"
#include "Tokenizer.h"

#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

/*
* Compiles a query string into an EvalTree.
*
* The query is tokenized exactly like a document — no special syntax.
* Adjacent tokens automatically generate a bigram arm so phrase matches
* score higher than scattered unigram matches.
*
*   "race car" → Or( race_carStreams,
*                    And(Or(raceStreams), Or(carStreams)) )
*/
class IndexSearchCompiler {
public:
    explicit IndexSearchCompiler(Tokenizer* tokenizer = nullptr)
        : m_Tokenizer(tokenizer)
        , m_Owned(tokenizer ? nullptr : new SmartTokenizer())
    {
        if (!m_Tokenizer)
            m_Tokenizer = m_Owned.get();
    }

    EvalTree* Compile(const char* queryString,
                      const char* streamSet = "AUT")
    {
        if (!queryString || !*queryString)
            return new EvalTree{};

        auto streams = ParseStreamSet(streamSet);
        auto root    = ParseExpression(queryString, streams);

        auto tree  = new EvalTree();
        tree->root = root;
        return tree;
    }

private:
    struct QueryTerm {
        std::string term;
        std::vector<std::string> streams;
    };

    Tokenizer*                 m_Tokenizer;
    std::unique_ptr<Tokenizer> m_Owned;

    std::vector<std::string> ParseStreamSet(const char* s)
    {
        std::vector<std::string> streams;

        for (; *s; ++s) {
            switch (*s) {
                case 'A': streams.emplace_back("A"); break;
                case 'U': streams.emplace_back("U"); break;
                case 'T': streams.emplace_back("T"); break;
                case 'B': streams.emplace_back("B"); break;
                case 'M': streams.emplace_back("M"); break;
                default:  break;
            }
        }

        if (streams.empty())
            streams.emplace_back("T");

        return streams;
    }

    std::vector<std::string> StreamsForField(const std::string& field,
                                             const std::vector<std::string>& fallback)
    {
        if (field == "title")  return {"T"};
        if (field == "body")   return {"B"};
        if (field == "url" || field == "site") return {"U"};
        if (field == "anchor") return {"A"};
        if (field == "meta")   return {"M"};
        return fallback;
    }

    static bool IsOrToken(const std::string& token)
    {
        return token == "or" || token == "OR" || token == "Or" || token == "oR";
    }

    std::vector<std::string> SplitRawItems(const std::string& query)
    {
        std::vector<std::string> items;
        std::string cur;
        for (char ch : query) {
            if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',') {
                if (!cur.empty()) {
                    items.push_back(cur);
                    cur.clear();
                }
            } else if (ch == '(' || ch == ')') {
                if (!cur.empty()) {
                    items.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) items.push_back(cur);
        return items;
    }

    void AddRawItem(const std::string& raw,
                    const std::vector<std::string>& defaultStreams,
                    std::vector<QueryTerm>& positive,
                    std::vector<QueryTerm>& negative)
    {
        if (raw.empty() || IsOrToken(raw)) return;

        bool exclude = raw[0] == '-' && raw.size() > 1;
        std::string item = exclude ? raw.substr(1) : raw;
        std::vector<std::string> streams = defaultStreams;

        size_t colon = item.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < item.size()) {
            std::string field = item.substr(0, colon);
            std::transform(field.begin(), field.end(), field.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            streams = StreamsForField(field, defaultStreams);
            item = item.substr(colon + 1);
        }

        auto tokens = m_Tokenizer->Tokenize(item.c_str());
        auto& target = exclude ? negative : positive;
        for (const auto& token : tokens)
            target.push_back({token, streams});
    }

    /* OR(termA, termU, termT, ...) across all streams for one token.
     * word_span: 1 = unigram, 2 = bigram (mirrors REF's AtomType/wordSpan). */
    std::shared_ptr<EvalNode> MakeTermGroup(
            const std::string&              term,
            const std::vector<std::string>& streams,
            uint32_t                        word_span = 1)
    {
        if (streams.size() == 1)
            return std::make_shared<TermNode>(term + streams[0], word_span);

        auto orNode = std::make_shared<OrNode>();
        for (auto& st : streams)
            orNode->children.push_back(
                std::make_shared<TermNode>(term + st, word_span));
        return orNode;
    }

    std::shared_ptr<EvalNode> MakeTermGroup(const QueryTerm& term,
                                            uint32_t word_span = 1)
    {
        return MakeTermGroup(term.term, term.streams, word_span);
    }

    /* AND of bigram groups for each adjacent token pair. */
    std::shared_ptr<EvalNode> BuildBigramQuery(
            const std::vector<QueryTerm>& terms)
    {
        if (terms.size() < 2)
            return nullptr;

        std::vector<std::shared_ptr<EvalNode>> groups;
        for (size_t i = 0; i + 1 < terms.size(); ++i) {
            if (terms[i].streams != terms[i + 1].streams)
                continue;

            groups.push_back(MakeTermGroup(
                terms[i].term + BIGRAM_SEP + terms[i + 1].term,
                terms[i].streams,
                /*word_span=*/2));
        }

        if (groups.empty()) return nullptr;

        if (groups.size() == 1) return groups[0];

        auto andNode      = std::make_shared<AndNode>();
        andNode->children = std::move(groups);
        return andNode;
    }

        std::shared_ptr<EvalNode> BuildImplicitExpression(
                const std::vector<QueryTerm>& tokens)
    {
            std::shared_ptr<EvalNode> unigramBase;
        if (tokens.size() == 1) {
                    unigramBase = MakeTermGroup(tokens[0]);
            } else {
                auto andNode = std::make_shared<AndNode>();
                for (auto& t : tokens)
                        andNode->children.push_back(MakeTermGroup(t));
                unigramBase = andNode;
        }

                auto bigram = BuildBigramQuery(tokens);
            if (!bigram) return unigramBase;

            auto orNode = std::make_shared<OrNode>();
            orNode->children.push_back(bigram);
            orNode->children.push_back(unigramBase);
            return orNode;
    }

        std::shared_ptr<EvalNode> BuildMinusExpression(
                const std::vector<QueryTerm>& positive,
                const std::vector<QueryTerm>& negative)
        {
                if (negative.empty())
                    return BuildImplicitExpression(positive);
                if (positive.empty())
                    return nullptr;

            auto notNode = std::make_shared<NotNode>();
                notNode->base = BuildImplicitExpression(positive);
                notNode->exclude = BuildImplicitExpression(negative);
            return notNode;
        }

    std::shared_ptr<EvalNode> ParseExpression(
            const std::string&              query,
            const std::vector<std::string>& streams)
    {
        std::vector<std::shared_ptr<EvalNode>> disjuncts;
        std::vector<QueryTerm> positive;
        std::vector<QueryTerm> negative;
        bool sawAnyTerm = false;

        for (const auto& raw : SplitRawItems(query)) {
            if (IsOrToken(raw)) {
                if (!positive.empty() || !negative.empty()) {
                    if (auto node = BuildMinusExpression(positive, negative))
                        disjuncts.push_back(node);
                    positive.clear();
                    negative.clear();
                }
            } else {
                AddRawItem(raw, streams, positive, negative);
                sawAnyTerm = true;
            }
        }

        if (!sawAnyTerm) return nullptr;

        if (disjuncts.empty())
            return BuildMinusExpression(positive, negative);

        if (!positive.empty() || !negative.empty()) {
            if (auto node = BuildMinusExpression(positive, negative))
                disjuncts.push_back(node);
        }

        if (disjuncts.size() == 1)
            return disjuncts[0];

        auto orNode = std::make_shared<OrNode>();
        orNode->children = std::move(disjuncts);
        return orNode;
    }
};

#endif
