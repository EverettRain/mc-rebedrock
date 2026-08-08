#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mc::animation {

// Runtime inputs for a Molang expression: `query.*` values describe the
// entity state driving the animation (anim_time, ground_speed, ...) while
// `variable.*` values are animation-authored scratch values. Missing lookups
// resolve to 0, matching Bedrock behaviour.
class MolangContext final {
  public:
    void setQuery(std::string name, float value) { queries_[std::move(name)] = value; }
    void setVariable(std::string name, float value) { variables_[std::move(name)] = value; }

    [[nodiscard]] float query(std::string_view name) const;
    [[nodiscard]] float variable(std::string_view name) const;

  private:
    std::unordered_map<std::string, float> queries_;
    std::unordered_map<std::string, float> variables_;
};

// A compiled Molang expression. Bedrock animation channels are frequently
// authored as Molang strings such as "math.cos(query.anim_time*38)*variable.x";
// compiling once and evaluating per frame keeps the hot path allocation-free.
//
// Supported grammar (a practical subset of Molang):
//   number literals, + - * / and unary minus, parentheses,
//   comparisons < <= > >= == !=, logical && ||, ternary a ? b : c,
//   query.<name>, variable.<name> (and the `q.` / `v.` aliases),
//   math.sin math.cos math.abs math.sqrt math.floor math.ceil math.round
//   math.mod math.min math.max math.clamp math.lerp math.pow math.exp
//   math.pi and the multi-statement ';' sequence (returns the last value).
// Angles for sin/cos are in degrees, exactly as Bedrock defines them.
class MolangExpression final {
  public:
    MolangExpression() = default;

    // Compiles `source`. Throws std::runtime_error on a syntax error.
    [[nodiscard]] static MolangExpression compile(std::string_view source);

    // Builds a constant expression (fast path for plain numeric channels).
    [[nodiscard]] static MolangExpression constant(float value);

    [[nodiscard]] float evaluate(const MolangContext& context) const;
    [[nodiscard]] bool isConstant() const { return constant_; }
    [[nodiscard]] float constantValue() const { return constantValue_; }

    // Exposed for the parser/evaluator translation unit; not part of the
    // public animation API.
    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

  private:
    bool constant_ = true;
    float constantValue_ = 0.0F;
    NodePtr root_;
};

} // namespace mc::animation
