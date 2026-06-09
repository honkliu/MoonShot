#ifndef EVALEXPRESSION_H__
#define EVALEXPRESSION_H__

#include <string>
#include <vector>
#include <memory>

enum class NodeType { Term, And, Or, Not };

struct EvalNode {
    virtual ~EvalNode() = default;
    virtual NodeType GetType() const = 0;
};

/*
* Leaf node — stream_key is term + stream abbreviation, e.g. "foxT".
*/
struct TermNode : EvalNode {
    std::string stream_key;

    explicit TermNode(std::string key) : stream_key(std::move(key)) {}
    NodeType GetType() const override { return NodeType::Term; }
};

/*
* All children must match.
*/
struct AndNode : EvalNode {
    std::vector<std::shared_ptr<EvalNode>> children;
    NodeType GetType() const override { return NodeType::And; }
};

/*
* At least one child must match.
*/
struct OrNode : EvalNode {
    std::vector<std::shared_ptr<EvalNode>> children;
    NodeType GetType() const override { return NodeType::Or; }
};

/*
* base must match; exclude must NOT match.
*/
struct NotNode : EvalNode {
    std::shared_ptr<EvalNode> base;
    std::shared_ptr<EvalNode> exclude;
    NodeType GetType() const override { return NodeType::Not; }
};

/*
* Wrapper returned by IndexSearchCompiler::Compile().
*/
class EvalTree {
public:
    std::shared_ptr<EvalNode> root;
    bool IsEmpty() const { return root == nullptr; }
};

class EvalItem {};

#endif
