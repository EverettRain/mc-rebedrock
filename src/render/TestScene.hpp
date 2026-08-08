#pragma once

#include "world/Block.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace mc::render {

struct TestSceneOptions final {
    world::Block block = world::Block::Stone;
    int stage = 0;
    // Renders a small controlled occlusion scene: a flat stone platform with a
    // buried cave and a surface opening, so the query results are predictable.
    bool occlusionScene = false;

    [[nodiscard]] bool operator==(const TestSceneOptions&) const = default;
};

// Supported form: --test-scene <numeric id|minecraft:id> [--stage <0..9>], plus
// --occlusion-scene for the controlled occlusion test scene.
// Returns nullopt when the test-scene switch is absent and throws for malformed
// requests so automation cannot silently render the wrong asset.
[[nodiscard]] std::optional<TestSceneOptions> parseTestSceneArguments(
    std::span<const std::string_view> arguments);

} // namespace mc::render
