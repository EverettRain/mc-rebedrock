#include "render/TestScene.hpp"

#include "compat/VanillaMapping.hpp"
#include "world/StateSchema.hpp"

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
    // 注册表同时接受 `rebedrock:stone`、vanilla 别名和裸名字三种写法
    if (const auto block = world::blockFromIdentifier(value); block.has_value()) {
        return *block;
    }
    throw std::invalid_argument("Unknown test-scene block: " + std::string{value});
}

// RN-15c: applies one `property=value` pair from the scene spec.
//
// The mapping itself is compat::mapVanillaState — the JC bridge's table, which
// already knows every property name this build's schema declares and (since
// RN-15) the six whose vanilla values are enum words. Nothing is parsed twice.
//
// What differs from the save path is the failure rule, and deliberately: a save
// SKIPS a property it does not understand, because a world from a newer build
// must still load. A command line must THROW, because the alternative is an
// automation quietly photographing a state nobody asked for and filing it as a
// baseline. `parseTestSceneArguments` already carries that rule for the block id.
void applySpecProperty(world::BlockState& state, std::string_view property,
                       std::string_view value) {
    const auto mapped = compat::mapVanillaState(property, value);
    if (!mapped.valid()) {
        // Distinguish the two ways it can fail, because they have different
        // fixes: a name this build has no property for, versus a value this
        // property does not take.
        if (world::statePropertyFromName(property) == world::StateProperty::Count &&
            compat::findOverride(property) == nullptr) {
            throw std::invalid_argument("Unknown block state property: " +
                                        std::string{property});
        }
        throw std::invalid_argument("Unknown value for block state property " +
                                    std::string{property} + ": " + std::string{value});
    }
    const auto& schema =
        world::kBlockRegistry[static_cast<std::size_t>(state.block())].states;
    if (!schema.has(mapped.property)) {
        throw std::invalid_argument(
            "Block " + world::blockDefinition(state.block()).identifier.toString() +
            " has no state property " + std::string{property});
    }
    // BlockStateTable clamps an out-of-range value to 0 rather than refusing it,
    // which on this path would render a different state than the one asked for
    // and say nothing. Catch it here instead.
    if (mapped.value >= schema.valueCount(mapped.property)) {
        throw std::invalid_argument("Value out of range for block state property " +
                                    std::string{property} + ": " + std::string{value});
    }
    state = compat::applyMappedState(state, mapped);
}

// Splits `oak_trapdoor[open=true,half=top]` into the block id and the pairs.
// A `[` with no `]`, an empty pair, or a pair with no `=` all throw: the spec is
// an exact instruction, not a best effort.
void parseBlockSpec(std::string_view spec, TestSceneOptions& options) {
    const auto open = spec.find('[');
    if (open == std::string_view::npos) {
        options.block = parseBlock(spec);
        options.state = world::BlockState{options.block};
        return;
    }
    if (spec.back() != ']') {
        throw std::invalid_argument("Block state spec is missing its closing ']': " +
                                    std::string{spec});
    }
    options.block = parseBlock(spec.substr(0, open));
    options.state = world::BlockState{options.block};
    const auto body = spec.substr(open + 1U, spec.size() - open - 2U);
    if (body.empty()) {
        throw std::invalid_argument("Block state spec has empty brackets: " + std::string{spec});
    }
    std::size_t cursor = 0;
    while (cursor <= body.size()) {
        const auto comma = body.find(',', cursor);
        const auto pair = body.substr(cursor, comma == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : comma - cursor);
        if (pair.empty()) {
            throw std::invalid_argument("Block state spec has an empty property: " +
                                        std::string{spec});
        }
        const auto equals = pair.find('=');
        if (equals == std::string_view::npos || equals == 0U || equals + 1U == pair.size()) {
            throw std::invalid_argument("Block state spec needs property=value, got: " +
                                        std::string{pair});
        }
        const auto property = pair.substr(0, equals);
        const auto value = pair.substr(equals + 1U);
        applySpecProperty(options.state, property, value);
        if (property == "facing") {
            options.stateSetsFacing = true;
        }
        options.stateSpec.emplace_back(std::string{property} + "-" + std::string{value});
        if (comma == std::string_view::npos) {
            break;
        }
        cursor = comma + 1U;
    }
}

[[nodiscard]] std::uint32_t parsePreviewSize(std::string_view value) {
    unsigned int size = 0U;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), size);
    if (error != std::errc{} || end != value.data() + value.size() || size < 64U ||
        size > 4096U) {
        throw std::invalid_argument("--preview-size must be an integer from 64 to 4096");
    }
    return size;
}

} // namespace

std::string previewDirectoryName(const TestSceneOptions& options) {
    std::string name = world::blockDefinition(options.block).identifier.toString();
    for (char& character : name) {
        if (character == ':') {
            character = '_';
        }
    }
    for (const std::string& property : options.stateSpec) {
        name += "__";
        name += property;
    }
    return name;
}

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
            parseBlockSpec(arguments[index], *result);
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
        } else if (arguments[index] == "--export-preview") {
            if (!result.has_value()) result = TestSceneOptions{};
            result->exportPreview = true;
        } else if (arguments[index] == "--preview-size") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--preview-size requires an integer from 64 to 4096");
            }
            if (!result.has_value()) result = TestSceneOptions{};
            result->previewSize = parsePreviewSize(arguments[index]);
        } else if (arguments[index] == "--preview-out") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--preview-out requires a directory");
            }
            if (!result.has_value()) result = TestSceneOptions{};
            result->previewRoot = std::filesystem::path{std::string{arguments[index]}};
        }
    }
    if (result.has_value() && !requestedScene) {
        throw std::invalid_argument("--stage requires --test-scene");
    }
    if (result.has_value() && !world::isRenderable(result->block)) {
        throw std::invalid_argument("The test scene requires a renderable block");
    }
    // The occlusion scene has no single block to photograph, so the two modes
    // cannot be combined — better to say so than to export eight pictures of a
    // stone platform.
    if (result.has_value() && result->exportPreview && result->occlusionScene) {
        throw std::invalid_argument("--export-preview cannot be combined with --occlusion-scene");
    }
    return result;
}

} // namespace mc::render
