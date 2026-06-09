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
*   "AUT"  — Anchor, URL, Title   (phase 1)
*   "AUTB" — + Body               (phase 2)
*   "T"    — Title only
*   "B"    — Body only
*
* Field-prefix syntax:
*   title:fox   → TermNode("foxT")
*   body:fox    → TermNode("foxB")
*   anchor:fox  → TermNode("foxA")
*
* Boolean syntax:
*   "fox quick"      → AND(fox, quick)
*   "fox OR quick"   → OR(fox, quick)
*   "fox NOT unsafe" → NOT(base=fox, exclude=unsafe)
*   "-unsafe fox"    → NOT(base=fox, exclude=unsafe)
*/
class IndexSearchCompiler {
public:
    IndexSearchCompiler()
        : tokenizer_(new SmartTokenizer())
    {}

    EvalTree* Compile(const char* query_string,
                      const char* stream_set = "AUT")
    {
        if (!query_string || !*query_string) return new EvalTree{};

        std::string q(query_string);
        std::vector<std::string> streams = ParseStreamSet(stream_set);

        auto root = ParseExpression(q, streams);
        auto tree = new EvalTree();
        tree->root = root;
        return tree;
    }

    template<typename T>
    Embeddings<T>* CompileToVector(const char* /*query_string*/)
    {
        return nullptr;
    }

private:
    std::unique_ptr<Tokenizer> tokenizer_;

    std::vector<std::string> ParseStreamSet(const char* ss)
    {
        std::vector<std::string> streams;
        for (const char* p = ss; *p; ++p) {
            switch (*p) {
                case 'A': streams.emplace_back("A"); break;
                case 'U': streams.emplace_back("U"); break;
                case 'T': streams.emplace_back("T"); break;
                case 'B': streams.emplace_back("B"); break;
                case 'M': streams.emplace_back("M"); break;
                default: break;
            }
        }
        if (streams.empty()) streams.emplace_back("T");
        return streams;
    }

    std::shared_ptr<EvalNode> MakeTermGroup(
            const std::string& term,
            const std::vector<std::string>& streams)
    {
        if (streams.size() == 1)
            return std::make_shared<TermNode>(term + streams[0]);

        auto or_node = std::make_shared<OrNode>();
        for (auto& s : streams)
            or_node->children.push_back(
                std::make_shared<TermNode>(term + s));
        return or_node;
    }

    /*
    * Return the single-stream abbreviation if a field prefix is detected,
    * and write the bare term into out_term. Returns "" if no prefix found.
    */
    std::string DetectFieldPrefix(const std::string& token,
                                  std::string& out_term)
    {
        static const struct { const char* prefix; const char* abbrev; } fields[] = {
            {"title:",  "T"},
            {"body:",   "B"},
            {"anchor:", "A"},
            {"url:",    "U"},
            {"site:",   "U"},
            {nullptr,  nullptr}
        };
        std::string low = token;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        for (auto* f = fields; f->prefix; ++f) {
            if (low.rfind(f->prefix, 0) == 0) {
                out_term = token.substr(strlen(f->prefix));
                std::transform(out_term.begin(), out_term.end(),
                               out_term.begin(), ::tolower);
                return f->abbrev;
            }
        }
        return "";
    }

    std::pair<std::shared_ptr<EvalNode>, bool>
    TokenToNode(const std::string& raw_token,
                const std::vector<std::string>& streams)
    {
        std::string token = raw_token;
        bool negated = false;

        if (!token.empty() && token[0] == '-') {
            negated = true;
            token = token.substr(1);
        }
        if (token.empty()) return {nullptr, false};

        std::string term_text;
        std::string field_abbrev = DetectFieldPrefix(token, term_text);
        if (!field_abbrev.empty() && !term_text.empty()) {
            auto node = std::make_shared<TermNode>(term_text + field_abbrev);
            return {node, negated};
        }

        auto tokens = tokenizer_->Tokenize(token.c_str());
        if (tokens.empty()) return {nullptr, false};

        if (tokens.size() == 1)
            return {MakeTermGroup(tokens[0], streams), negated};

        auto and_node = std::make_shared<AndNode>();
        for (auto& t : tokens)
            and_node->children.push_back(MakeTermGroup(t, streams));
        return {and_node, negated};
    }

    std::shared_ptr<EvalNode> ParseExpression(
            const std::string& query,
            const std::vector<std::string>& streams)
    {
        if (query.find(" OR ") != std::string::npos) {
            auto parts = SplitOn(query, " OR ");
            auto or_node = std::make_shared<OrNode>();
            for (auto& p : parts) {
                auto child = ParseExpression(Trim(p), streams);
                if (child) or_node->children.push_back(child);
            }
            if (or_node->children.empty()) return nullptr;
            if (or_node->children.size() == 1) return or_node->children[0];
            return or_node;
        }

        if (query.find(" NOT ") != std::string::npos) {
            auto parts = SplitOn(query, " NOT ");
            if (parts.size() >= 2) {
                auto base    = ParseExpression(Trim(parts[0]), streams);
                auto exclude = ParseExpression(Trim(parts[1]), streams);
                if (base && exclude) {
                    auto not_node = std::make_shared<NotNode>();
                    not_node->base    = base;
                    not_node->exclude = exclude;
                    return not_node;
                }
                if (base) return base;
            }
        }

        std::string cleaned = query;
        {
            size_t pos = 0;
            while ((pos = cleaned.find(" AND ", pos)) != std::string::npos)
                cleaned.replace(pos, 5, " ");
        }

        std::istringstream iss(cleaned);
        std::string tok;
        std::vector<std::shared_ptr<EvalNode>> positive_nodes;
        std::vector<std::shared_ptr<EvalNode>> negated_nodes;

        while (iss >> tok) {
            auto [node, negated] = TokenToNode(tok, streams);
            if (!node) continue;
            if (negated) negated_nodes.push_back(node);
            else         positive_nodes.push_back(node);
        }

        if (positive_nodes.empty()) return nullptr;

        std::shared_ptr<EvalNode> base;
        if (positive_nodes.size() == 1) {
            base = positive_nodes[0];
        } else {
            auto and_node = std::make_shared<AndNode>();
            and_node->children = std::move(positive_nodes);
            base = and_node;
        }

        for (auto& excl : negated_nodes) {
            auto not_node = std::make_shared<NotNode>();
            not_node->base    = base;
            not_node->exclude = excl;
            base = not_node;
        }

        return base;
    }

    static std::string Trim(const std::string& s)
    {
        size_t l = s.find_first_not_of(" \t");
        size_t r = s.find_last_not_of(" \t");
        if (l == std::string::npos) return "";
        return s.substr(l, r - l + 1);
    }
};

#endif
