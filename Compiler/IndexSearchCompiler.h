#ifndef INDEXSEARCHCOMPILER_H__
#define INDEXSEARCHCOMPILER_H__

#include "EvalExpression.h"
#include "Embeddings.h"
#include "Tokenizer.h"

#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <set>

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
                      const char* streamSet = "AUT",
                      IEmbeddingModel* embeddingModel = nullptr,
                      QueryCompileMode mode = QueryCompileMode::Default)
    {
        if (!queryString || !*queryString)
            return new EvalTree{};

        auto streams = ParseStreamSet(streamSet);
        auto root    = streams.empty() ? nullptr : ParseExpression(queryString, streams, mode);

        auto tree  = new EvalTree();
        tree->root = root;
        if (HasVectorStream(streamSet)) {
            if (embeddingModel)
                tree->vector_query = CompileToVector(queryString, embeddingModel);
        }
        return tree;
    }

    std::vector<float> CompileToVector(const char* queryString,
                                       IEmbeddingModel* model)
    {
        if (!queryString || !*queryString)
            return std::vector<float>(model->GetDimension(), 0.0f);
        return model->Embed(m_Tokenizer->Tokenize(queryString));
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
        bool sawVectorOnlyCandidate = false;

        for (; *s; ++s) {
            switch (*s) {
                case 'A': streams.emplace_back("A"); break;
                case 'U': streams.emplace_back("U"); break;
                case 'T': streams.emplace_back("T"); break;
                case 'B': streams.emplace_back("B"); break;
                case 'M': streams.emplace_back("M"); break;
                case 'V': case 'v': sawVectorOnlyCandidate = true; break;
                default:  break;
            }
        }

        if (streams.empty() && !sawVectorOnlyCandidate)
            streams.emplace_back("T");

        return streams;
    }

    static bool HasVectorStream(const char* s)
    {
        if (!s) return false;
        for (; *s; ++s)
            if (*s == 'V' || *s == 'v') return true;
        return false;
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

    static bool IsNotToken(const std::string& token)
    {
        return token == "not" || token == "NOT" || token == "Not" || token == "nOT" || token == "-";
    }

    static std::string QueryTermKey(const QueryTerm& term)
    {
        std::string key = term.term;
        key.push_back('\x1e');
        for (const auto& stream : term.streams) {
            key += stream;
            key.push_back(',');
        }
        return key;
    }

    static std::vector<QueryTerm> FilterWeakAndTerms(const std::vector<QueryTerm>& tokens)
    {
        std::vector<QueryTerm> filtered;
        std::set<std::string> seen;
        for (const auto& token : tokens) {
            if (token.term.size() <= 1)
                continue;
            if (seen.insert(QueryTermKey(token)).second)
                filtered.push_back(token);
        }

        if (!filtered.empty())
            return filtered;

        for (const auto& token : tokens) {
            if (token.term.empty())
                continue;
            if (seen.insert(QueryTermKey(token)).second)
                filtered.push_back(token);
        }
        return filtered;
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
                    std::vector<QueryTerm>& negative,
                    bool forceExclude = false)
    {
        if (raw.empty() || IsOrToken(raw) || IsNotToken(raw)) return;

        bool hasMinusPrefix = raw[0] == '-' && raw.size() > 1;
        bool exclude = hasMinusPrefix || forceExclude;
        std::string item = hasMinusPrefix ? raw.substr(1) : raw;
        if (item.empty()) return;
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

    static uint32_t MinShouldMatch(size_t termCount)
    {
        if (termCount <= 2) return 1;
        if (termCount <= 5) return 2;
        return 3;
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

    std::shared_ptr<EvalNode> BuildAnyBigramQuery(
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

            auto orNode = std::make_shared<OrNode>();
            orNode->children = std::move(groups);
            return orNode;
    }

    std::shared_ptr<EvalNode> BuildWeakAndBaseExpression(
            const std::vector<QueryTerm>& terms)
    {
        if (terms.empty())
            return nullptr;
        if (terms.size() == 1)
            return MakeTermGroup(terms[0]);

        std::vector<std::shared_ptr<EvalNode>> groups;
        groups.reserve(terms.size() * 2);
        for (const auto& token : terms)
            groups.push_back(MakeTermGroup(token));

        std::shared_ptr<EvalNode> base;
        if (groups.size() == 1) {
            base = groups[0];
        } else {
            auto weakAndNode = std::make_shared<WeakAndNode>();
            weakAndNode->children = std::move(groups);
            weakAndNode->min_should_match = std::min<uint32_t>(
                MinShouldMatch(terms.size()),
                static_cast<uint32_t>(weakAndNode->children.size()));
            base = weakAndNode;
        }

        return base;
    }

    std::shared_ptr<EvalNode> BuildWeakAndBigramExpression(
            const std::vector<QueryTerm>& tokens)
    {
        auto terms = FilterWeakAndTerms(tokens);
        auto base = BuildWeakAndBaseExpression(terms);
        auto bigram = BuildAnyBigramQuery(terms);
        if (!base) return bigram;
        if (!bigram) return base;

        auto candidateOr = std::make_shared<OrNode>();
        candidateOr->children.push_back(std::move(base));
        candidateOr->children.push_back(std::move(bigram));
        return candidateOr;
    }

    std::shared_ptr<EvalNode> BuildImplicitExpression(
            const std::vector<QueryTerm>& tokens,
            QueryCompileMode mode = QueryCompileMode::Default)
    {
        if (mode == QueryCompileMode::WeakAndBigram)
            return BuildWeakAndBigramExpression(tokens);

        if (tokens.empty())
            return nullptr;

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
            const std::vector<QueryTerm>& negative,
            QueryCompileMode mode = QueryCompileMode::Default)
    {
        if (negative.empty())
            return BuildImplicitExpression(positive, mode);
        if (positive.empty())
            return nullptr;

        auto notNode = std::make_shared<NotNode>();
        notNode->base = BuildImplicitExpression(positive, mode);
        notNode->exclude = BuildImplicitExpression(negative, mode);
        return notNode;
    }

    std::shared_ptr<EvalNode> ParseExpression(
            const std::string&              query,
            const std::vector<std::string>& streams,
            QueryCompileMode                mode = QueryCompileMode::Default)
    {
        std::vector<std::shared_ptr<EvalNode>> disjuncts;
        std::vector<QueryTerm> positive;
        std::vector<QueryTerm> negative;
        bool sawAnyTerm = false;
        bool nextItemIsNegative = false;

        for (const auto& raw : SplitRawItems(query)) {
            if (IsOrToken(raw)) {
                if (!positive.empty() || !negative.empty()) {
                    if (auto node = BuildMinusExpression(positive, negative, mode))
                        disjuncts.push_back(node);
                    positive.clear();
                    negative.clear();
                }
                nextItemIsNegative = false;
            } else if (IsNotToken(raw)) {
                nextItemIsNegative = true;
            } else {
                AddRawItem(raw, streams, positive, negative, nextItemIsNegative);
                nextItemIsNegative = false;
                sawAnyTerm = true;
            }
        }

        if (!sawAnyTerm) return nullptr;

        if (disjuncts.empty())
            return BuildMinusExpression(positive, negative, mode);

        if (!positive.empty() || !negative.empty()) {
            if (auto node = BuildMinusExpression(positive, negative, mode))
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
