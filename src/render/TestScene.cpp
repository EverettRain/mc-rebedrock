#include "render/TestScene.hpp"

#include <charconv>
#include <stdexcept>
#include <string>

namespace mc::render {
namespace {

[[nodiscard]] world::Block parseBlock(std::string_view value) {
    unsigned int numeric = 0U;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), numeric);
    if (error == std::errc{} && end == value.data() + value.size() &&
        numeric < static_cast<unsigned int>(world::Block::Count)) {
        return static_cast<world::Block>(numeric);
    }
    // The registry takes `rebedrock:stone`, the vanilla alias and the bare name.
    if (const auto block = world::blockFromIdentifier(value); block.has_value()) {
        return *block;
    }
    throw std::invalid_argument("Unknown test-scene block: " + std::string{value});
}

} // namespace

std::optional<TestSceneOptions> parseTestSceneArguments(
    std::span<const std::string_view> arguments) {
    std::optional<TestSceneOptions> result;
    bool requestedScene = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "--test-scene") {
            requestedScene = true;
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--test-scene requires a block id");
            }
            if (!result.has_value()) result = TestSceneOptions{};
            result->block = parseBlock(arguments[index]);
        } else if (arguments[index] == "--stage") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--stage requires an integer from 0 to 9");
            }
            int stage = -1;
            const auto value = arguments[index];
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), stage);
            if (error != std::errc{} || end != value.data() + value.size() ||
                stage < 0 || stage > 9) {
                throw std::invalid_argument("--stage must be an integer from 0 to 9");
            }
            if (!result.has_value()) result = TestSceneOptions{};
            result->stage = stage;
        } else if (arguments[index] == "--occlusion-scene") {
            if (!result.has_value()) result = TestSceneOptions{};
            result->occlusionScene = true;
        }
    }
    if (result.has_value() && !requestedScene) {
        throw std::invalid_argument("--stage requires --test-scene");
    }
    if (result.has_value() && !world::isRenderable(result->block)) {
        throw std::invalid_argument("The test scene requires a renderable block");
    }
    return result;
}

} // namespace mc::render
