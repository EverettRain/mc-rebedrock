// RN-13-3: dump a block's BAKED model as JSON, so the offline UV preview
// (tools/block_uv_preview.py) can draw exactly the quads the chunk mesher would.
//
// Why this exists. The trapdoor report that opened RN-13 — "the open leaf's
// texture faces the wrong way" — is the shape of defect that headless assertions
// structurally cannot see: every number involved (box, uv rect, U/V direction,
// placement, texture slot) checks out against the vanilla json and the model
// still looks wrong, because "looks wrong" is a claim about the picture. Each
// one of those has cost a round trip to a Mac. A picture that can be produced in
// this container ends that loop, and the only piece the Python side cannot
// reconstruct on its own is what OUR baker produced — hence this dumper.
//
// It prints the real `bake::bakedElementModel` output. It resolves no atlas and
// loads no texture: the sprite is named, and the preview tool reads the PNG out
// of whatever resource pack the user points it at. Mojang assets are never
// bundled, cached or copied into this repository — see
// docs/content-dev/REGULAR.md and the copyright rule.
//
//   ./mc_rebedrock_block_model_dump --block oak_trapdoor --state open=true \
//       --state facing=north
//
// Prints one JSON object with the resolved state and an array of quads.

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/ElementModelBaker.hpp"
#include "world/FaceBakery.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::bake::Facing;

[[nodiscard]] std::optional<Block> blockNamed(std::string_view name) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(Block::Count); ++i) {
        const auto block = static_cast<Block>(i);
        const auto& definition = mc::world::blockDefinition(block);
        if (definition.identifier.matches(name) || definition.vanilla.matches(name)) {
            return block;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<BlockOrientation> orientationNamed(std::string_view name) {
    if (name == "north") return BlockOrientation::North;
    if (name == "east") return BlockOrientation::East;
    if (name == "south") return BlockOrientation::South;
    if (name == "west") return BlockOrientation::West;
    if (name == "up") return BlockOrientation::Up;
    if (name == "down") return BlockOrientation::Down;
    return std::nullopt;
}

[[nodiscard]] const char* orientationName(BlockOrientation orientation) {
    switch (orientation) {
    case BlockOrientation::North: return "north";
    case BlockOrientation::East: return "east";
    case BlockOrientation::South: return "south";
    case BlockOrientation::West: return "west";
    case BlockOrientation::Up: return "up";
    case BlockOrientation::Down: return "down";
    }
    return "north";
}

[[nodiscard]] const char* facingName(Facing facing) {
    switch (facing) {
    case Facing::Down: return "down";
    case Facing::Up: return "up";
    case Facing::North: return "north";
    case Facing::South: return "south";
    case Facing::West: return "west";
    case Facing::East: return "east";
    }
    return "up";
}

// Which vanilla sprite a baked quad shows.
//
// This MIRRORS ChunkMesher's two rules — `modelSlotLayer` for an ElementModel
// block, `textureLayerOf` for the door/trapdoor/fence-gate families that draw
// from the baked store — because those return atlas LAYER numbers, which only
// exist once an atlas has been baked from a resource pack, and this tool
// deliberately does not open one. A drift between this and the mesher shows up
// as the wrong picture in the preview, which is a loud failure rather than a
// silent one; it is a diagnostic, not a second source of truth.
[[nodiscard]] const char* spriteName(Block block, BlockState state, Facing facing,
                                     std::uint8_t slot) {
    const auto& definition = mc::world::blockDefinition(block);
    if (definition.model == mc::world::BlockModel::ElementModel) {
        const char* name = slot < definition.modelTextures.size()
                               ? definition.modelTextures[slot]
                               : nullptr;
        return name != nullptr ? name : "";
    }
    if (definition.model == mc::world::BlockModel::Door) {
        // A door's sprite is per HALF, never per face.
        const char* name = state.isDoorUpperHalf() ? definition.textures.top
                                                   : definition.textures.side;
        return name != nullptr ? name : "";
    }
    const char* name = facing == Facing::Up      ? definition.textures.top
                       : facing == Facing::Down  ? definition.textures.bottom
                                                 : definition.textures.side;
    return name != nullptr ? name : "";
}

void usage() {
    std::fprintf(stderr,
                 "usage: mc_rebedrock_block_model_dump --block <name> "
                 "[--state key=value]...\n"
                 "  states: facing=<direction> open=<bool> half=<top|bottom> "
                 "powered=<bool>\n"
                 "          hinge=<left|right> in_wall=<bool> locked=<bool> "
                 "delay=<1..4>\n"
                 "          mode=<compare|subtract>\n");
}

[[nodiscard]] bool booleanValue(std::string_view text) {
    return text == "true" || text == "1" || text == "yes";
}

} // namespace

int main(int argc, char** argv) {
    std::string blockName;
    std::vector<std::pair<std::string, std::string>> states;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--block" && i + 1 < argc) {
            blockName = argv[++i];
        } else if (argument == "--state" && i + 1 < argc) {
            const std::string entry{argv[++i]};
            const auto equals = entry.find('=');
            if (equals == std::string::npos) {
                usage();
                return 1;
            }
            states.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
        } else {
            usage();
            return 1;
        }
    }
    if (blockName.empty()) {
        usage();
        return 1;
    }
    const auto block = blockNamed(blockName);
    if (!block.has_value()) {
        std::fprintf(stderr, "no such block: %s\n", blockName.c_str());
        return 1;
    }

    BlockState state{*block};
    for (const auto& [key, value] : states) {
        if (key == "facing") {
            const auto orientation = orientationNamed(value);
            if (!orientation.has_value()) {
                std::fprintf(stderr, "no such facing: %s\n", value.c_str());
                return 1;
            }
            // `with(orientation)` rather than a fresh BlockState: rebuilding
            // would silently discard every property already applied, so
            // `--state open=true --state facing=north` would lose the open.
            state = state.with(*orientation);
        } else if (key == "open") {
            state = state.withOpen(booleanValue(value));
        } else if (key == "half") {
            state = state.withDoorUpperHalf(value == "top" || value == "upper");
        } else if (key == "powered") {
            state = state.withPowered(booleanValue(value));
        } else if (key == "hinge") {
            state = state.withHinge(value == "right" ? mc::world::DoorHinge::Right
                                                     : mc::world::DoorHinge::Left);
        } else if (key == "in_wall") {
            state = state.withInWall(booleanValue(value));
        } else if (key == "locked") {
            state = state.withRepeaterLocked(booleanValue(value));
        } else if (key == "delay") {
            state = state.withRepeaterDelay(std::atoi(value.c_str()));
        } else if (key == "mode") {
            state = state.withComparatorSubtract(value == "subtract");
        } else {
            std::fprintf(stderr, "no such state property: %s\n", key.c_str());
            return 1;
        }
    }

    const auto quads = mc::world::bake::bakedElementModel(*block, state);
    const auto& definition = mc::world::blockDefinition(*block);

    std::printf("{\n");
    std::printf("  \"block\": \"%s\",\n", definition.identifier.toString().c_str());
    std::printf("  \"vanilla\": \"%s\",\n",
                definition.vanilla.empty() ? "" : definition.vanilla.toString().c_str());
    std::printf("  \"facing\": \"%s\",\n", orientationName(state.orientation()));
    std::printf("  \"open\": %s,\n", state.open() ? "true" : "false");
    std::printf("  \"half\": \"%s\",\n", state.isDoorUpperHalf() ? "top" : "bottom");
    std::printf("  \"quads\": [\n");
    for (std::size_t q = 0; q < quads.size(); ++q) {
        const auto& baked = quads[q];
        const auto facing = baked.quad.facing;
        std::printf("    {\"facing\": \"%s\", \"cull\": %s, \"shade\": %s, \"slot\": %u, "
                    "\"sprite\": \"%s\",\n",
                    facingName(facing),
                    baked.quad.cull == mc::world::bake::kNoCull
                        ? "null"
                        : (std::string{"\""} + facingName(static_cast<Facing>(baked.quad.cull)) +
                           "\"")
                              .c_str(),
                    baked.shade ? "true" : "false", static_cast<unsigned>(baked.quad.slot),
                    spriteName(*block, state, facing, baked.quad.slot));
        std::printf("     \"position\": [");
        for (std::size_t i = 0; i < 4; ++i) {
            std::printf("%s[%.6f, %.6f, %.6f]", i == 0 ? "" : ", ", baked.quad.position[i].x,
                        baked.quad.position[i].y, baked.quad.position[i].z);
        }
        std::printf("],\n     \"uv\": [");
        for (std::size_t i = 0; i < 4; ++i) {
            std::printf("%s[%.6f, %.6f]", i == 0 ? "" : ", ", baked.quad.uv[i].x,
                        baked.quad.uv[i].y);
        }
        std::printf("]}%s\n", q + 1 == quads.size() ? "" : ",");
    }
    std::printf("  ]\n}\n");
    return 0;
}
