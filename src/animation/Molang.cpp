#include "animation/Molang.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mc::animation {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kDegToRad = kPi / 180.0F;

[[nodiscard]] bool toBool(float value) { return value != 0.0F; }

} // namespace

float MolangContext::query(std::string_view name) const {
    const auto it = queries_.find(std::string{name});
    return it != queries_.end() ? it->second : 0.0F;
}

float MolangContext::variable(std::string_view name) const {
    const auto it = variables_.find(std::string{name});
    return it != variables_.end() ? it->second : 0.0F;
}

struct MolangExpression::Node {
    enum class Kind {
        Number,
        Query,
        Variable,
        Neg,
        Not,
        Add,
        Sub,
        Mul,
        Div,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        Equal,
        NotEqual,
        And,
        Or,
        Ternary,
        Sequence,
        Call,
    };

    Kind kind = Kind::Number;
    float number = 0.0F;
    std::string name;                 // query/variable identifier or function name
    std::vector<NodePtr> children;    // operands / arguments
};

namespace {

float evaluateNode(const MolangExpression::Node& node, const MolangContext& context);

float callFunction(const MolangExpression::Node& node, const MolangContext& context) {
    const auto& args = node.children;
    const auto arg = [&](std::size_t index) -> float {
        return index < args.size() ? evaluateNode(*args[index], context) : 0.0F;
    };
    const std::string& fn = node.name;
    if (fn == "sin") return std::sin(arg(0) * kDegToRad);
    if (fn == "cos") return std::cos(arg(0) * kDegToRad);
    if (fn == "abs") return std::fabs(arg(0));
    if (fn == "sqrt") return std::sqrt(std::max(arg(0), 0.0F));
    if (fn == "floor") return std::floor(arg(0));
    if (fn == "ceil") return std::ceil(arg(0));
    if (fn == "round") return std::round(arg(0));
    if (fn == "trunc") return std::trunc(arg(0));
    if (fn == "exp") return std::exp(arg(0));
    if (fn == "ln") return std::log(std::max(arg(0), 1e-9F));
    if (fn == "pow") return std::pow(arg(0), arg(1));
    if (fn == "min") return std::min(arg(0), arg(1));
    if (fn == "max") return std::max(arg(0), arg(1));
    if (fn == "mod") {
        const float divisor = arg(1);
        return divisor != 0.0F ? std::fmod(arg(0), divisor) : 0.0F;
    }
    if (fn == "clamp") {
        const float value = arg(0);
        return std::min(std::max(value, arg(1)), arg(2));
    }
    if (fn == "lerp") {
        const float t = arg(2);
        return arg(0) + (arg(1) - arg(0)) * t;
    }
    if (fn == "lerprotate") {
        float start = arg(0);
        float end = arg(1);
        float delta = std::fmod(end - start, 360.0F);
        if (delta > 180.0F) delta -= 360.0F;
        if (delta < -180.0F) delta += 360.0F;
        return start + delta * arg(2);
    }
    throw std::runtime_error("Molang: unknown function math." + fn);
}

// True when the subtree contains no query/variable lookups, i.e. it always
// evaluates to the same value and can be folded to a constant at compile time.
bool isPure(const MolangExpression::Node& node) {
    using Kind = MolangExpression::Node::Kind;
    if (node.kind == Kind::Query || node.kind == Kind::Variable) {
        return false;
    }
    for (const auto& child : node.children) {
        if (!isPure(*child)) {
            return false;
        }
    }
    return true;
}

float evaluateNode(const MolangExpression::Node& node, const MolangContext& context) {
    using Kind = MolangExpression::Node::Kind;
    const auto child = [&](std::size_t index) -> float {
        return evaluateNode(*node.children[index], context);
    };
    switch (node.kind) {
        case Kind::Number: return node.number;
        case Kind::Query: return context.query(node.name);
        case Kind::Variable: return context.variable(node.name);
        case Kind::Neg: return -child(0);
        case Kind::Not: return toBool(child(0)) ? 0.0F : 1.0F;
        case Kind::Add: return child(0) + child(1);
        case Kind::Sub: return child(0) - child(1);
        case Kind::Mul: return child(0) * child(1);
        case Kind::Div: {
            const float divisor = child(1);
            return divisor != 0.0F ? child(0) / divisor : 0.0F;
        }
        case Kind::Less: return child(0) < child(1) ? 1.0F : 0.0F;
        case Kind::LessEqual: return child(0) <= child(1) ? 1.0F : 0.0F;
        case Kind::Greater: return child(0) > child(1) ? 1.0F : 0.0F;
        case Kind::GreaterEqual: return child(0) >= child(1) ? 1.0F : 0.0F;
        case Kind::Equal: return child(0) == child(1) ? 1.0F : 0.0F;
        case Kind::NotEqual: return child(0) != child(1) ? 1.0F : 0.0F;
        case Kind::And: return toBool(child(0)) && toBool(child(1)) ? 1.0F : 0.0F;
        case Kind::Or: return toBool(child(0)) || toBool(child(1)) ? 1.0F : 0.0F;
        case Kind::Ternary: return toBool(child(0)) ? child(1) : child(2);
        case Kind::Sequence: {
            float value = 0.0F;
            for (const auto& statement : node.children) {
                value = evaluateNode(*statement, context);
            }
            return value;
        }
        case Kind::Call: return callFunction(node, context);
    }
    return 0.0F;
}

// Recursive-descent parser turning Molang source into a shared expression tree.
class MolangParser final {
  public:
    explicit MolangParser(std::string_view source) : source_(source) {}

    MolangExpression::NodePtr parse() {
        MolangExpression::NodePtr node = parseSequence();
        skipSpaces();
        if (position_ != source_.size()) {
            fail("unexpected trailing characters");
        }
        return node;
    }

  private:
    using Node = MolangExpression::Node;
    using NodePtr = MolangExpression::NodePtr;

    static NodePtr make(Node::Kind kind, std::vector<NodePtr> children) {
        auto node = std::make_shared<Node>();
        node->kind = kind;
        node->children = std::move(children);
        return node;
    }

    NodePtr parseSequence() {
        std::vector<NodePtr> statements;
        statements.push_back(parseTernary());
        skipSpaces();
        while (peek() == ';') {
            ++position_;
            skipSpaces();
            if (position_ >= source_.size()) {
                break; // trailing ';'
            }
            statements.push_back(parseTernary());
            skipSpaces();
        }
        if (statements.size() == 1U) {
            return statements.front();
        }
        return make(Node::Kind::Sequence, std::move(statements));
    }

    NodePtr parseTernary() {
        NodePtr condition = parseOr();
        skipSpaces();
        if (peek() == '?') {
            ++position_;
            NodePtr whenTrue = parseTernary();
            skipSpaces();
            if (peek() == ':') {
                ++position_;
            } else {
                fail("expected ':' in ternary expression");
            }
            NodePtr whenFalse = parseTernary();
            return make(Node::Kind::Ternary, {condition, whenTrue, whenFalse});
        }
        return condition;
    }

    NodePtr parseOr() {
        NodePtr left = parseAnd();
        while (true) {
            skipSpaces();
            if (match("||")) {
                left = make(Node::Kind::Or, {left, parseAnd()});
            } else {
                return left;
            }
        }
    }

    NodePtr parseAnd() {
        NodePtr left = parseComparison();
        while (true) {
            skipSpaces();
            if (match("&&")) {
                left = make(Node::Kind::And, {left, parseComparison()});
            } else {
                return left;
            }
        }
    }

    NodePtr parseComparison() {
        NodePtr left = parseAdditive();
        while (true) {
            skipSpaces();
            if (match("<=")) {
                left = make(Node::Kind::LessEqual, {left, parseAdditive()});
            } else if (match(">=")) {
                left = make(Node::Kind::GreaterEqual, {left, parseAdditive()});
            } else if (match("==")) {
                left = make(Node::Kind::Equal, {left, parseAdditive()});
            } else if (match("!=")) {
                left = make(Node::Kind::NotEqual, {left, parseAdditive()});
            } else if (peek() == '<') {
                ++position_;
                left = make(Node::Kind::Less, {left, parseAdditive()});
            } else if (peek() == '>') {
                ++position_;
                left = make(Node::Kind::Greater, {left, parseAdditive()});
            } else {
                return left;
            }
        }
    }

    NodePtr parseAdditive() {
        NodePtr left = parseMultiplicative();
        while (true) {
            skipSpaces();
            const char c = peek();
            if (c == '+') {
                ++position_;
                left = make(Node::Kind::Add, {left, parseMultiplicative()});
            } else if (c == '-') {
                ++position_;
                left = make(Node::Kind::Sub, {left, parseMultiplicative()});
            } else {
                return left;
            }
        }
    }

    NodePtr parseMultiplicative() {
        NodePtr left = parseUnary();
        while (true) {
            skipSpaces();
            const char c = peek();
            if (c == '*') {
                ++position_;
                left = make(Node::Kind::Mul, {left, parseUnary()});
            } else if (c == '/') {
                ++position_;
                left = make(Node::Kind::Div, {left, parseUnary()});
            } else {
                return left;
            }
        }
    }

    NodePtr parseUnary() {
        skipSpaces();
        const char c = peek();
        if (c == '-') {
            ++position_;
            return make(Node::Kind::Neg, {parseUnary()});
        }
        if (c == '+') {
            ++position_;
            return parseUnary();
        }
        if (c == '!') {
            ++position_;
            return make(Node::Kind::Not, {parseUnary()});
        }
        return parsePrimary();
    }

    NodePtr parsePrimary() {
        skipSpaces();
        const char c = peek();
        if (c == '(') {
            ++position_;
            NodePtr inner = parseTernary();
            skipSpaces();
            if (peek() != ')') {
                fail("expected ')'");
            }
            ++position_;
            return inner;
        }
        if ((c >= '0' && c <= '9') || c == '.') {
            return parseNumber();
        }
        if (isIdentStart(c)) {
            return parseIdentifier();
        }
        fail("unexpected character in expression");
        return nullptr; // unreachable
    }

    NodePtr parseNumber() {
        const std::size_t start = position_;
        while (position_ < source_.size()) {
            const char c = source_[position_];
            const bool numeric = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                                 ((c == '+' || c == '-') && position_ > start &&
                                  (source_[position_ - 1U] == 'e' || source_[position_ - 1U] == 'E'));
            if (!numeric) {
                break;
            }
            ++position_;
        }
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Number;
        node->number = std::stof(std::string{source_.substr(start, position_ - start)});
        return node;
    }

    NodePtr parseIdentifier() {
        std::string identifier = readIdentifier();
        // Namespace: query./q., variable./v., math., temp., geometry., texture.
        std::string domain = identifier;
        std::string member;
        if (peek() == '.') {
            ++position_;
            member = readIdentifier();
        }

        if (domain == "math") {
            return parseMathCall(member);
        }
        if (domain == "query" || domain == "q") {
            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Query;
            node->name = member;
            return node;
        }
        if (domain == "variable" || domain == "v" || domain == "temp" || domain == "t") {
            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Variable;
            node->name = member;
            return node;
        }
        // A bare identifier such as `pi` or `true`/`false`.
        if (domain == "true") return numberNode(1.0F);
        if (domain == "false") return numberNode(0.0F);
        // Treat any other bare token as a variable so authored files degrade
        // gracefully rather than aborting the whole load.
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Variable;
        node->name = member.empty() ? domain : member;
        return node;
    }

    NodePtr parseMathCall(const std::string& member) {
        if (member == "pi") {
            return numberNode(kPi);
        }
        skipSpaces();
        std::vector<NodePtr> args;
        if (peek() == '(') {
            ++position_;
            skipSpaces();
            if (peek() != ')') {
                args.push_back(parseTernary());
                skipSpaces();
                while (peek() == ',') {
                    ++position_;
                    args.push_back(parseTernary());
                    skipSpaces();
                }
            }
            if (peek() != ')') {
                fail("expected ')' after math arguments");
            }
            ++position_;
        }
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Call;
        node->name = member;
        node->children = std::move(args);
        return node;
    }

    static NodePtr numberNode(float value) {
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Number;
        node->number = value;
        return node;
    }

    std::string readIdentifier() {
        const std::size_t start = position_;
        while (position_ < source_.size() && isIdentPart(source_[position_])) {
            ++position_;
        }
        return std::string{source_.substr(start, position_ - start)};
    }

    static bool isIdentStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool isIdentPart(char c) {
        return isIdentStart(c) || (c >= '0' && c <= '9');
    }

    void skipSpaces() {
        while (position_ < source_.size()) {
            const char c = source_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] char peek() const {
        return position_ < source_.size() ? source_[position_] : '\0';
    }

    bool match(std::string_view literal) {
        if (source_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("Molang parse error at offset " + std::to_string(position_) +
                                 ": " + message + " in \"" + std::string{source_} + "\"");
    }

    std::string_view source_;
    std::size_t position_ = 0U;
};

} // namespace

MolangExpression MolangExpression::compile(std::string_view source) {
    MolangExpression expression;
    MolangParser parser{source};
    expression.root_ = parser.parse();
    // Constant-fold expressions with no query/variable inputs so the per-frame
    // evaluation of plain numeric channels is a single load.
    if (expression.root_ && isPure(*expression.root_)) {
        expression.constant_ = true;
        expression.constantValue_ = evaluateNode(*expression.root_, MolangContext{});
        expression.root_ = nullptr;
    } else {
        expression.constant_ = false;
    }
    return expression;
}

MolangExpression MolangExpression::constant(float value) {
    MolangExpression expression;
    expression.constant_ = true;
    expression.constantValue_ = value;
    return expression;
}

float MolangExpression::evaluate(const MolangContext& context) const {
    if (constant_) {
        return constantValue_;
    }
    return evaluateNode(*root_, context);
}

} // namespace mc::animation
