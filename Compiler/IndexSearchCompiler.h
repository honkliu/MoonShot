#ifndef INDEXSEARCHCOMPILER_H__
#define INDEXSEARCHCOMPILER_H__

#include "EvalExpression.h"
#include "Embeddings.h"
#include "Tokenizer.h"

#include <memory>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstring>

static inline std::vector<std::string> SplitOn(const std::string& s,
                                                const std::string& delim)
{
    std::vector<std::string> parts;
    size_t start = 0, pos;

    while ((pos = s.find(delim, start)) != std::string::npos) {
        parts.push_back(s.substr(start, pos - start));
        start = pos + delim.size();
    }

    parts.push_back(s.substr(start));
    return parts;
}

/*
* Compiles a query string into an EvalTree.
*
* Stream sets:
*   "AUT"   — Anchor, URL, Title   (phase 1)
*   "AUTB"  — + Body               (phase 2)
*   "T"     — Title only
*   "B"     — Body only
*
* Field-prefix syntax:
*   title:fox   → TermNode("foxT")
*   body:fox    → TermNode("foxB")
*
* Boolean syntax:
*   "fox quick"      → AND + bigram variant
*   "fox OR quick"   → OR
*   "fox NOT unsafe" → NOT
*   "-unsafe fox"    → NOT (minus prefix)
*
* Bigram support:
*   Multi-token queries automatically include a bigram alternative.
*   "good morning" → Or( And(good_morningT,...),        ← bigram arm
*                        And(Or(goodT,...), Or(morningT,...)) )  ← unigram arm
*   Documents matching bigrams score higher than unigram-only matches.
*/
class IndexSearchCompiler {
public:
    IndexSearchCompiler()
        : tokenizer_(new SmartTokenizer())
    {}

    EvalTree* Compile(const char* queryString,
                      const char* streamSet = "AUT")
    {
        if (!queryString || !*queryString)
            return new EvalTree{};

        std::string          query(queryString);
        std::vector<std::string> streams = ParseStreamSet(streamSet);

        auto root = ParseExpression(query, streams);

        auto tree  = new EvalTree();
        tree->root = root;
        return tree;
    }

    template<typename T>
    Embeddings<T>* CompileToVector(const char* /*queryString*/)
    {
        return nullptr;
    }

private:
    std::unique_ptr<Tokenizer> tokenizer_;

    std::vector<std::string> ParseStreamSet(const char* streamSet)
    {
        std::vector<std::string> streams;

        for (const char* p = streamSet; *p; ++p) {
            switch (*p) {
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

    /*
    * Build an Or/Term group for one term across all streams.
    * "fox" on "AUT" → OrNode(TermNode("foxA"), TermNode("foxU"), TermNode("foxT"))
    */
    std::shared_ptr<EvalNode> MakeTermGroup(
            const std::string&              term,
            const std::vector<std::string>& streams)
    {
        if (streams.size() == 1)
            return std::make_shared<TermNode>(term + streams[0]);

        auto orNode = std::make_shared<OrNode>();

        for (auto& stream : streams)
            orNode->children.push_back(std::make_shared<TermNode>(term + stream));

        return orNode;
    }

    /*
    * Build an AND of bigram term groups for adjacent token pairs.
    *
    * "good morning vietnam" →
    *   And( Or(good_morningA,...,good_morningT),
    *        Or(morning_vietnamA,...,morning_vietnamT) )
    *
    * Returns nullptr when fewer than 2 canonical terms are present.
    */
    std::shared_ptr<EvalNode> BuildBigramQuery(
            const std::vector<std::string>& terms,
            const std::vector<std::string>& streams)
    {
        if (terms.size() < 2)
            return nullptr;

        std::vector<std::shared_ptr<EvalNode>> bigramGroups;

        for (size_t i = 0; i + 1 < terms.size(); ++i) {
            if (terms[i].empty() || terms[i + 1].empty())
                continue;

            std::string bigramKey = terms[i] + "_" + terms[i + 1];
            bigramGroups.push_back(MakeTermGroup(bigramKey, streams));
        }

        if (bigramGroups.empty())
            return nullptr;

        if (bigramGroups.size() == 1)
            return bigramGroups[0];

        auto andNode      = std::make_shared<AndNode>();
        andNode->children = std::move(bigramGroups);
        return andNode;
    }

    /*
    * Extract canonical token strings from a raw query token for bigram use.
    * Field-prefixed tokens (title:foo) and negated tokens (-foo) are skipped —
    * bigrams do not span field boundaries or cross negation boundaries.
    */
    void ExtractCanonicalTerms(const std::string&       rawToken,
                               std::vector<std::string>& outTerms)
    {
        if (rawToken.empty() || rawToken[0] == '-')
            return;

        std::string termText;

        if (!DetectFieldPrefix(rawToken, termText).empty())
            return;

        auto tokens = tokenizer_->Tokenize(rawToken.c_str());

        for (auto& t : tokens)
            outTerms.push_back(t);
    }

    /*
    * Return the single-stream abbreviation if a field prefix is detected,
    * and write the bare term into outTerm. Returns "" if no prefix found.
    */
    std::string DetectFieldPrefix(const std::string& token,
                                  std::string&        outTerm)
    {
        static const struct { const char* prefix; const char* abbrev; } fields[] = {
            {"title:",  "T"},
            {"body:",   "B"},
            {"anchor:", "A"},
            {"url:",    "U"},
            {"site:",   "U"},
            {nullptr,  nullptr}
        };

        std::string lower = token;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        for (auto* f = fields; f->prefix; ++f) {
            if (lower.rfind(f->prefix, 0) == 0) {
                outTerm = token.substr(strlen(f->prefix));
                std::transform(outTerm.begin(), outTerm.end(),
                               outTerm.begin(), ::tolower);
                return f->abbrev;
            }
        }

        return "";
    }

    std::pair<std::shared_ptr<EvalNode>, bool>
    TokenToNode(const std::string&              rawToken,
                const std::vector<std::string>& streams)
    {
        std::string token   = rawToken;
        bool        negated = false;

        if (!token.empty() && token[0] == '-') {
            negated = true;
            token   = token.substr(1);
        }

        if (token.empty())
            return {nullptr, false};

        std::string termText;
        std::string fieldAbbrev = DetectFieldPrefix(token, termText);

        if (!fieldAbbrev.empty() && !termText.empty()) {
            auto node = std::make_shared<TermNode>(termText + fieldAbbrev);
            return {node, negated};
        }

        auto tokens = tokenizer_->Tokenize(token.c_str());

        if (tokens.empty())
            return {nullptr, false};

        if (tokens.size() == 1)
            return {MakeTermGroup(tokens[0], streams), negated};

        auto andNode = std::make_shared<AndNode>();
        for (auto& t : tokens)
            andNode->children.push_back(MakeTermGroup(t, streams));

        return {andNode, negated};
    }

    std::shared_ptr<EvalNode> ParseExpression(
            const std::string&              query,
            const std::vector<std::string>& streams)
    {
        if (query.find(" OR ") != std::string::npos) {
            auto parts  = SplitOn(query, " OR ");
            auto orNode = std::make_shared<OrNode>();

            for (auto& part : parts) {
                auto child = ParseExpression(Trim(part), streams);
                if (child)
                    orNode->children.push_back(child);
            }

            if (orNode->children.empty())
                return nullptr;

            if (orNode->children.size() == 1)
                return orNode->children[0];

            return orNode;
        }

        if (query.find(" NOT ") != std::string::npos) {
            auto parts = SplitOn(query, " NOT ");

            if (parts.size() >= 2) {
                auto baseExpr    = ParseExpression(Trim(parts[0]), streams);
                auto excludeExpr = ParseExpression(Trim(parts[1]), streams);

                if (baseExpr && excludeExpr) {
                    auto notNode     = std::make_shared<NotNode>();
                    notNode->base    = baseExpr;
                    notNode->exclude = excludeExpr;
                    return notNode;
                }

                if (baseExpr)
                    return baseExpr;
            }
        }

        std::string cleaned = query;
        {
            size_t pos = 0;
            while ((pos = cleaned.find(" AND ", pos)) != std::string::npos)
                cleaned.replace(pos, 5, " ");
        }

        std::istringstream               iss(cleaned);
        std::string                      tok;
        std::vector<std::shared_ptr<EvalNode>> freeNodes;
        std::vector<std::shared_ptr<EvalNode>> fieldNodes;
        std::vector<std::shared_ptr<EvalNode>> negatedNodes;
        std::vector<std::string>               canonicalTerms;

        while (iss >> tok) {
            auto [node, negated] = TokenToNode(tok, streams);

            if (!node)
                continue;

            if (negated) {
                negatedNodes.push_back(node);
            } else {
                std::string dummy;
                bool isField = !DetectFieldPrefix(tok, dummy).empty();

                if (isField)
                    fieldNodes.push_back(node);
                else
                    freeNodes.push_back(node);

                ExtractCanonicalTerms(tok, canonicalTerms);
            }
        }

        if (freeNodes.empty() && fieldNodes.empty())
            return nullptr;

        /*
        * Build the unigram AND from free (non-field) terms only.
        */
        std::shared_ptr<EvalNode> unigramBase;

        if (!freeNodes.empty()) {
            if (freeNodes.size() == 1) {
                unigramBase = freeNodes[0];
            } else {
                auto andNode      = std::make_shared<AndNode>();
                andNode->children = std::move(freeNodes);
                unigramBase       = andNode;
            }
        }

        /*
        * Combine with bigram arm when possible.
        */
        auto bigramQuery = BuildBigramQuery(canonicalTerms, streams);

        std::shared_ptr<EvalNode> base;

        if (unigramBase && bigramQuery) {
            auto orNode = std::make_shared<OrNode>();
            orNode->children.push_back(bigramQuery);
            orNode->children.push_back(unigramBase);
            base = orNode;
        } else if (unigramBase) {
            base = unigramBase;
        } else if (bigramQuery) {
            base = bigramQuery;
        }

        /*
        * AND field-constrained nodes (site:, title:, body:, ...) around the
        * entire expression so they apply to both the bigram and unigram arms.
        */
        for (auto& f : fieldNodes) {
            if (!base) {
                base = f;
            } else {
                auto andNode = std::make_shared<AndNode>();
                andNode->children.push_back(base);
                andNode->children.push_back(f);
                base = andNode;
            }
        }

        for (auto& excl : negatedNodes) {
            auto notNode     = std::make_shared<NotNode>();
            notNode->base    = base;
            notNode->exclude = excl;
            base             = notNode;
        }

        return base;
    }

    static std::string Trim(const std::string& s)
    {
        size_t l = s.find_first_not_of(" \t");
        size_t r = s.find_last_not_of(" \t");

        if (l == std::string::npos)
            return "";

        return s.substr(l, r - l + 1);
    }
};

#endif
