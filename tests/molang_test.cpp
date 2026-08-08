#include "animation/Molang.hpp"

#include <cassert>
#include <cmath>

using namespace mc::animation;

int main() {
    MolangContext context;
    context.setQuery("anim_time", 0.5F);
    context.setVariable("x", 2.0F);

    // Pure expressions fold to a constant.
    const auto folded = MolangExpression::compile("1 + 2 * 3");
    assert(folded.isConstant());
    assert(std::abs(folded.evaluate(context) - 7.0F) < 1e-5F);

    // Trigonometry is in degrees, matching Bedrock.
    assert(std::abs(MolangExpression::compile("math.sin(90)").evaluate(context) - 1.0F) < 1e-5F);
    assert(std::abs(MolangExpression::compile("math.cos(0)").evaluate(context) - 1.0F) < 1e-5F);

    // Query and variable lookups (with q./v. aliases).
    assert(std::abs(MolangExpression::compile("query.anim_time + variable.x").evaluate(context) -
                    2.5F) < 1e-5F);
    assert(std::abs(MolangExpression::compile("q.anim_time * v.x").evaluate(context) - 1.0F) <
           1e-5F);

    // Control flow and math helpers.
    assert(std::abs(MolangExpression::compile("query.anim_time > 0.25 ? 10 : -10").evaluate(context) -
                    10.0F) < 1e-5F);
    assert(std::abs(MolangExpression::compile("math.clamp(5, 0, 3)").evaluate(context) - 3.0F) <
           1e-5F);
    assert(std::abs(MolangExpression::compile("math.lerp(0, 10, 0.5)").evaluate(context) - 5.0F) <
           1e-5F);
    assert(std::abs(MolangExpression::compile("math.mod(7, 3)").evaluate(context) - 1.0F) < 1e-5F);
    assert(std::abs(MolangExpression::compile("1; 2; 3").evaluate(context) - 3.0F) < 1e-5F);
    assert(std::abs(MolangExpression::compile("math.pi").evaluate(context) - 3.14159265F) < 1e-4F);

    // A missing query resolves to 0 rather than failing.
    assert(std::abs(MolangExpression::compile("query.missing").evaluate(context)) < 1e-6F);

    bool threw = false;
    try {
        (void)MolangExpression::compile("1 +").evaluate(context);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
    return 0;
}
