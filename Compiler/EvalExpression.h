#ifndef EVALEXPRESSION_H__
#define EVALEXPRESSION_H__

#include <string>
#include <vector>
#include <memory>
#include <cstddef>
#include <cstdint>

enum class NodeType { Term, And, Or, Not, WeakAnd, Boost };
enum class QueryCompileMode { Default, WeakAndBigram, WeakAndBigramBoost, WeakAndBigramBoostForDoc };
enum class WeakAndBuildMode { FlatPruned, OrChildren, OrChildrenPruned };

struct QueryCompileModeParameters {
    float QMP_UnigramWeight = 1.8f;
    float QMP_BigramWeight = 0.2f;
    float QMP_BigramBoostWeight = 0.0f;
    float QMP_StaticWeight = 1.0f;
    float QMP_PriorWeight = 0.0f;
    float QMP_QualityWeight = 1.0f;
    float QMP_AuthorityWeight = 0.5f;
    float QMP_SpamPenalty = 2.0f;
    float QMP_CosineWeight = 16.0f;
    float QMP_AnchorWeight = 1.0f;
    float QMP_UrlWeight = 1.0f;
    float QMP_TitleWeight = 1.0f;
    float QMP_BodyWeight = 1.0f;
};

inline constexpr QueryCompileModeParameters kWeakAndBigramParameters{
    1.8f, 0.2f, 0.0f,
    1.0f, 0.0f, 1.0f, 0.5f, 2.0f,
    16.0f,
    1.0f, 1.0f, 1.0f, 1.0f
};

inline constexpr QueryCompileModeParameters kWeakAndBigramBoostParameters{
    0.25f, 1.0f, 0.5f,
    0.25f, 4.0f, 0.25f, 0.0f, 4.0f,
    16.0f,
    1.0f, 1.0f, 1.0f, 1.0f
};

inline constexpr QueryCompileModeParameters kWeakAndBigramBoostForDocParameters{
    0.09f, 0.5f, 4.0f,
    0.25f, 2.0f, 1.0f, 0.1f, 1.0f,
    128.0f,
    1.0f, 1.0f, 1.0f, 1.0f
};

inline const QueryCompileModeParameters& GetQueryCompileModeParameters(QueryCompileMode mode)
{
    switch (mode) {
    case QueryCompileMode::WeakAndBigramBoost:
        return kWeakAndBigramBoostParameters;
    case QueryCompileMode::WeakAndBigramBoostForDoc:
        return kWeakAndBigramBoostForDocParameters;
    default:
        return kWeakAndBigramParameters;
    }
}

/*
* Bigram separator — mirrors REF's CreateBigramString().
* \x1F (ASCII Unit Separator) is never produced by ICU's word breaker,
* so it unambiguously marks a bigram key vs a unigram that happens to
* contain an underscore (e.g. a variable name "morning_call").
*
* REF equivalent: AtomType_Bigram with wordSpan = 2.
*/
static constexpr char BIGRAM_SEP = '\x1F';

struct EvalNode {
    virtual ~EvalNode() = default;
    virtual NodeType GetType() const = 0;
};

/*
* Leaf node — stream_key is term + stream abbreviation, e.g. "foxT".
* word_span mirrors REF's AtomType / wordSpan:
*   1 = unigram  (AtomType_Unigram, wordSpan = 1)
*   2 = bigram   (AtomType_Bigram,  wordSpan = 2)
*/
struct TermNode : EvalNode {
    std::string stream_key;
    uint32_t    word_span = 1;

    explicit TermNode(std::string key, uint32_t span = 1)
        : stream_key(std::move(key)), word_span(span) {}
    NodeType GetType() const override { return NodeType::Term; }
};

struct AndNode : EvalNode {
    std::vector<std::shared_ptr<EvalNode>> children;
    NodeType GetType() const override { return NodeType::And; }
};

struct OrNode : EvalNode {
    std::vector<std::shared_ptr<EvalNode>> children;
    NodeType GetType() const override { return NodeType::Or; }
};

struct WeakAndNode : EvalNode {
    std::vector<std::shared_ptr<EvalNode>> children;
    uint32_t min_should_match = 1;
    NodeType GetType() const override { return NodeType::WeakAnd; }
};

struct NotNode : EvalNode {
    std::shared_ptr<EvalNode> base;
    std::shared_ptr<EvalNode> exclude;
    NodeType GetType() const override { return NodeType::Not; }
};

struct BoostNode : EvalNode {
    std::shared_ptr<EvalNode> base;
    std::shared_ptr<EvalNode> boost;
    float boost_weight = 1.0f;
    NodeType GetType() const override { return NodeType::Boost; }
};

class EvalTree {
public:
    std::shared_ptr<EvalNode> root;
    std::vector<float> vector_query;
    size_t vector_ef_search = 200;

    bool HasTextQuery() const { return root != nullptr; }
    bool HasVectorQuery() const { return !vector_query.empty(); }
    bool IsEmpty() const { return !HasTextQuery() && !HasVectorQuery(); }
};

class EvalItem {};

#endif
