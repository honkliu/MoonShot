#ifndef INDEXSEARCHCOMPILER_H__
#define INDEXSEARCHCOMPILER_H__

#include "EvalExpression.h"
#include "Tokenizer.h"

#include <memory>
#include <string>
#include <vector>
#include <algorithm>

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

    /* OR(termA, termU, termT, ...) across all streams for one token. */
    std::shared_ptr<EvalNode> MakeTermGroup(
            const std::string&              term,
            const std::vector<std::string>& streams)
    {
        if (streams.size() == 1)
            return std::make_shared<TermNode>(term + streams[0]);

        auto orNode = std::make_shared<OrNode>();
        for (auto& st : streams)
            orNode->children.push_back(std::make_shared<TermNode>(term + st));
        return orNode;
    }

    /* AND of bigram groups for each adjacent token pair. */
    std::shared_ptr<EvalNode> BuildBigramQuery(
            const std::vector<std::string>& terms,
            const std::vector<std::string>& streams)
    {
        if (terms.size() < 2)
            return nullptr;

        std::vector<std::shared_ptr<EvalNode>> groups;
        for (size_t i = 0; i + 1 < terms.size(); ++i)
            groups.push_back(
                MakeTermGroup(terms[i] + "_" + terms[i+1], streams));

        if (groups.size() == 1) return groups[0];

        auto andNode      = std::make_shared<AndNode>();
        andNode->children = std::move(groups);
        return andNode;
    }

    std::shared_ptr<EvalNode> ParseExpression(
            const std::string&              query,
            const std::vector<std::string>& streams)
    {
        auto tokens = m_Tokenizer->Tokenize(query.c_str());
        if (tokens.empty()) return nullptr;

        /* unigram AND */
        std::shared_ptr<EvalNode> unigramBase;
        if (tokens.size() == 1) {
            unigramBase = MakeTermGroup(tokens[0], streams);
        } else {
            auto andNode = std::make_shared<AndNode>();
            for (auto& t : tokens)
                andNode->children.push_back(MakeTermGroup(t, streams));
            unigramBase = andNode;
        }

        /* OR(bigram arm, unigram arm) — bigram scores higher */
        auto bigram = BuildBigramQuery(tokens, streams);
        if (!bigram) return unigramBase;

        auto orNode = std::make_shared<OrNode>();
        orNode->children.push_back(bigram);
        orNode->children.push_back(unigramBase);
        return orNode;
    }
};

#endif
