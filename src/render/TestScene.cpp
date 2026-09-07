#include "render/TestScene.hpp"

#include "compat/VanillaMapping.hpp"
#include "world/StateSchema.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
//
// RN-17: this returns the parse instead of writing it into TestSceneOptions, because
// the legend of a structured scene needs the same spec parsed once per key. One
// parser, two callers — the alternative (a second, simpler reader for legend
// entries) is exactly how `oak_stairs[facing=north]` would come to mean two
// different things depending on which flag it was written after.
struct ParsedBlockSpec final {
    world::Block block = world::Block::Stone;
    world::BlockState state{world::Block::Stone};
    bool setsFacing = false;
    std::vector<std::string> properties;
};

[[nodiscard]] ParsedBlockSpec parseBlockSpec(std::string_view spec) {
    ParsedBlockSpec parsed;
    const auto open = spec.find('[');
    if (open == std::string_view::npos) {
        parsed.block = parseBlock(spec);
        parsed.state = world::BlockState{parsed.block};
        return parsed;
    }
    if (spec.back() != ']') {
        throw std::invalid_argument("Block state spec is missing its closing ']': " +
                                    std::string{spec});
    }
    parsed.block = parseBlock(spec.substr(0, open));
    parsed.state = world::BlockState{parsed.block};
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
        applySpecProperty(parsed.state, property, value);
        if (property == "facing") {
            parsed.setsFacing = true;
        }
        parsed.properties.emplace_back(std::string{property} + "-" + std::string{value});
        if (comma == std::string_view::npos) {
            break;
        }
        cursor = comma + 1U;
    }
    return parsed;
}

void applyBlockSpec(std::string_view spec, TestSceneOptions& options) {
    ParsedBlockSpec parsed = parseBlockSpec(spec);
    options.block = parsed.block;
    options.state = parsed.state;
    options.stateSetsFacing = parsed.setsFacing;
    options.stateSpec = std::move(parsed.properties);
}

// RN-17: one `--key <char>=<block spec>` entry, kept in the order it was given so the
// directory name is a function of the command line.
struct SceneLegendEntry final {
    char symbol = '\0';
    ParsedBlockSpec parsed;
};

// A legend symbol is one character out of [A-Za-z0-9_]. Restricted rather than
// "anything but the separators" because the symbol goes into a directory name,
// and a legend of `*` or `:` would produce a path that is legal on one of the two
// platforms this project ships on and not the other.
[[nodiscard]] bool isLegendSymbol(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
}

// FNV-1a, only ever used to keep an over-long directory name unique. Written out
// rather than reached for from <functional>: std::hash is allowed to differ
// between runs and between standard libraries, and a directory name that changes
// with the toolchain destroys every stored baseline.
[[nodiscard]] std::string shortHash(std::string_view text) {
    std::uint32_t hash = 2'166'136'261U;
    for (const char character : text) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16'777'619U;
    }
    std::string out(8U, '0');
    for (std::size_t i = 0; i < 8U; ++i) {
        out[7U - i] = "0123456789abcdef"[(hash >> (4U * i)) & 0xFU];
    }
    return out;
}

// The scene's cells, from the pattern and the legend. Layers are `;`-separated
// (the first is the bottom), rows within a layer are `/`-separated (the first is
// the north edge), and every character of a row is one cell along +x.
//
// NO whitespace is trimmed. A space is air, and trimming would make it
// impossible to leave a gap at the edge of a layer — which is exactly what the
// stair-on-grass scene needs.
void buildScene(TestSceneOptions& options, const std::vector<SceneLegendEntry>& legend) {
    std::vector<std::vector<std::string_view>> layers;
    std::string_view pattern{options.scenePattern};
    for (std::size_t cursor = 0; cursor <= pattern.size();) {
        const auto semicolon = pattern.find(';', cursor);
        const auto layer = pattern.substr(cursor, semicolon == std::string_view::npos
                                                      ? std::string_view::npos
                                                      : semicolon - cursor);
        std::vector<std::string_view> rows;
        for (std::size_t inner = 0; inner <= layer.size();) {
            const auto slash = layer.find('/', inner);
            rows.push_back(layer.substr(inner, slash == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : slash - inner));
            if (slash == std::string_view::npos) break;
            inner = slash + 1U;
        }
        layers.push_back(std::move(rows));
        if (semicolon == std::string_view::npos) break;
        cursor = semicolon + 1U;
    }

    const std::size_t rowCount = layers.front().size();
    const std::size_t columnCount = layers.front().front().size();
    if (columnCount == 0U) {
        throw std::invalid_argument("--scene has an empty row");
    }
    for (const auto& rows : layers) {
        if (rows.size() != rowCount) {
            throw std::invalid_argument("--scene layers must all have the same number of rows");
        }
        for (const std::string_view row : rows) {
            // A ragged pattern is the one mistake that would silently shift a
            // whole layer sideways instead of failing.
            if (row.size() != columnCount) {
                throw std::invalid_argument("--scene rows must all be the same length");
            }
        }
    }
    if (static_cast<int>(columnCount) > kMaxSceneExtent ||
        static_cast<int>(rowCount) > kMaxSceneExtent ||
        static_cast<int>(layers.size()) > kMaxSceneExtent) {
        throw std::invalid_argument("--scene is larger than the preview world allows");
    }

    std::vector<bool> used(legend.size(), false);
    for (std::size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
        for (std::size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
            const std::string_view row = layers[layerIndex][rowIndex];
            for (std::size_t columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
                const char symbol = row[columnIndex];
                if (symbol == ' ') {
                    continue; // air
                }
                std::size_t match = legend.size();
                for (std::size_t i = 0; i < legend.size(); ++i) {
                    if (legend[i].symbol == symbol) {
                        match = i;
                        break;
                    }
                }
                if (match == legend.size()) {
                    throw std::invalid_argument(std::string{"--scene uses '"} + symbol +
                                                "', which no --key defines");
                }
                used[match] = true;
                options.sceneCells.push_back(
                    SceneCell{glm::ivec3{static_cast<int>(columnIndex),
                                         static_cast<int>(layerIndex),
                                         static_cast<int>(rowIndex)},
                              legend[match].parsed.state});
            }
        }
    }
    if (options.sceneCells.empty()) {
        throw std::invalid_argument("--scene is entirely air");
    }
    for (std::size_t i = 0; i < legend.size(); ++i) {
        // An unused key is a typo in the pattern or in the key, and either way the
        // picture is not the one that was asked for.
        if (!used[i]) {
            throw std::invalid_argument(std::string{"--key '"} + legend[i].symbol +
                                        "' is never used by --scene");
        }
    }
    options.sceneSize = {static_cast<int>(columnCount), static_cast<int>(layers.size()),
                         static_cast<int>(rowCount)};
    // `block` still has to name something renderable: the shared checks below and
    // the renderer's own printout both read it. The first cell's block is the
    // honest answer for a scene, and every cell is checked in its own right.
    options.block = options.sceneCells.front().state.block();
    options.state = options.sceneCells.front().state;
    for (const SceneCell& cell : options.sceneCells) {
        if (!world::isRenderable(cell.state.block())) {
            throw std::invalid_argument(
                "--scene names a block that cannot be rendered: " +
                world::blockDefinition(cell.state.block()).identifier.toString());
        }
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

namespace {

// `rebedrock:oak_stairs` -> `rebedrock_oak_stairs`. See the header for why the
// namespace is kept rather than dropped.
[[nodiscard]] std::string pathSafeIdentifier(world::Block block) {
    std::string name = world::blockDefinition(block).identifier.toString();
    for (char& character : name) {
        if (character == ':') {
            character = '_';
        }
    }
    return name;
}

} // namespace

static std::string previewBaseDirectoryName(const TestSceneOptions& options) {
    if (!options.isScene()) {
        // Unchanged, deliberately: RN-15's stored baselines live under these
        // names and a rename would quietly invalidate every one of them.
        std::string name = pathSafeIdentifier(options.block);
        for (const std::string& property : options.stateSpec) {
            name += "__";
            name += property;
        }
        return name;
    }
    // RN-17: the pattern itself is in the name, not just its size. `sg/gs` and
    // `gs/sg` are two different pictures; a name built from the size and the
    // legend alone would file them in the same directory and the second run
    // would overwrite the first with no sign that anything had happened.
    std::string name = "scene__";
    for (const char character : options.scenePattern) {
        if (character == '/') {
            name += '-';
        } else if (character == ';') {
            name += '+';
        } else if (character == ' ') {
            name += '.';
        } else {
            name += character;
        }
    }
    for (const std::string& entry : options.sceneLegend) {
        name += "__";
        name += entry;
    }
    if (name.size() <= kMaxPreviewDirectoryName) {
        return name;
    }
    // Long names are cut, not rejected: a scene with six keys is a perfectly
    // reasonable thing to photograph, and a filesystem's 255-byte component
    // limit is not a reason to refuse it. The suffix is over the WHOLE name, so
    // two scenes that share a prefix still land in different directories.
    return name.substr(0, kMaxPreviewDirectoryName - 10U) + "__" + shortHash(name);
}

std::string previewDirectoryName(const TestSceneOptions& options) {
    const auto base = previewBaseDirectoryName(options);
    if (!options.sunShadows && !options.shadowEntities && !options.sunTick) return base;
    const auto name = base + "__sun-" + (options.sunShadows ? "on" : "off") +
        "-" + std::to_string(options.sunTick.value_or(6000U)) +
        (options.shadowEntities ? "-entities" : "");
    return name.size() <= kMaxPreviewDirectoryName ? name :
        name.substr(0, kMaxPreviewDirectoryName - 10U) + "__" + shortHash(name);
}

std::optional<TestSceneOptions> parseTestSceneArguments(
    std::span<const std::string_view> arguments) {
    std::optional<TestSceneOptions> result;
    bool requestedScene = false;
    // RN-17: the legend is collected first and the pattern resolved afterwards, so
    // `--key` may be written on either side of `--scene`. A command line whose
    // meaning depends on flag order is a command line someone will get wrong.
    bool requestedStructure = false;
    bool requestedSingle = false;
    bool requestedStage = false;
    std::vector<SceneLegendEntry> legend;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "--test-scene") {
            requestedScene = true;
            requestedSingle = true;
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--test-scene requires a block id");
            }
            if (!result.has_value()) result = TestSceneOptions{};
            applyBlockSpec(arguments[index], *result);
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
            requestedStage = true;
            result->stage = stage;
        } else if (arguments[index] == "--scene") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--scene requires a pattern");
            }
            if (!result.has_value()) result = TestSceneOptions{};
            if (requestedStructure) {
                throw std::invalid_argument("--scene may only be given once");
            }
            requestedStructure = true;
            requestedScene = true;
            result->scenePattern = std::string{arguments[index]};
            if (result->scenePattern.empty()) {
                throw std::invalid_argument("--scene requires a pattern");
            }
        } else if (arguments[index] == "--key") {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("--key requires <symbol>=<block spec>");
            }
            if (!result.has_value()) result = TestSceneOptions{};
            const auto entry = arguments[index];
            const auto equals = entry.find('=');
            if (equals != 1U || entry.size() < 3U || !isLegendSymbol(entry.front())) {
                throw std::invalid_argument(
                    "--key must be <one letter, digit or underscore>=<block spec>, got: " +
                    std::string{entry});
            }
            const char symbol = entry.front();
            for (const SceneLegendEntry& existing : legend) {
                if (existing.symbol == symbol) {
                    throw std::invalid_argument(std::string{"--key '"} + symbol +
                                                "' is defined twice");
                }
            }
            SceneLegendEntry parsed;
            parsed.symbol = symbol;
            parsed.parsed = parseBlockSpec(entry.substr(equals + 1U));
            std::string name{symbol};
            name += '-';
            name += pathSafeIdentifier(parsed.parsed.block);
            for (const std::string& property : parsed.parsed.properties) {
                name += '.';
                name += property;
            }
            result->sceneLegend.push_back(std::move(name));
            legend.push_back(std::move(parsed));
        } else if (arguments[index] == "--occlusion-scene") {
            if (!result.has_value()) result = TestSceneOptions{};
            result->occlusionScene = true;
        } else if (arguments[index] == "--sun-shadows" || arguments[index] == "--shadow-entities") {
            if (!result.has_value()) result = TestSceneOptions{};
            if (arguments[index] == "--sun-shadows") result->sunShadows = true;
            else result->shadowEntities = true;
        } else if (arguments[index] == "--sun-tick") {
            if (++index >= arguments.size()) throw std::invalid_argument("--sun-tick requires 0..23999");
            std::uint32_t tick = 0;
            const auto value = arguments[index];
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), tick);
            if (error != std::errc{} || end != value.data() + value.size() || tick >= 24000U)
                throw std::invalid_argument("--sun-tick requires 0..23999");
            if (!result.has_value()) result = TestSceneOptions{};
            result->sunTick = tick;
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
    if (result && (result->sunShadows || result->shadowEntities || result->sunTick) && !result->exportPreview)
        throw std::invalid_argument("shadow preview options require --export-preview (hidden window)");
    if (result && result->shadowEntities &&
        (!result->isScene() && !requestedStructure))
        throw std::invalid_argument("--shadow-entities requires --scene with an 8x8 floor");
    if (result.has_value() && !requestedScene) {
        throw std::invalid_argument("--stage requires --test-scene");
    }
    if (result.has_value()) {
        // RN-17: `--scene` and `--test-scene` are two answers to "what am I
        // photographing", and a run that was given both would silently use one.
        if (requestedStructure && requestedSingle) {
            throw std::invalid_argument("--scene cannot be combined with --test-scene");
        }
        if (requestedStructure && result->occlusionScene) {
            throw std::invalid_argument("--scene cannot be combined with --occlusion-scene");
        }
        if (!legend.empty() && !requestedStructure) {
            throw std::invalid_argument("--key requires --scene");
        }
        if (requestedStructure) {
            if (requestedStage) {
                // `--stage` spins a block through six orientations. Every cell of
                // a scene already names its own state, so a stage could only
                // fight them.
                throw std::invalid_argument("--stage cannot be combined with --scene");
            }
            buildScene(*result, legend);
        }
    }
    if (result && result->shadowEntities && (result->sceneSize.x != 8 || result->sceneSize.z != 8))
        throw std::invalid_argument("--shadow-entities requires an 8x8 scene footprint");
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
