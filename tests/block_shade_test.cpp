// RN-13-1: the model json's per-element `"shade"`, from the baked quad all the
// way to the shader that has to act on it.
//
// RN-10a put the flag on the element and RN-10d set it on every lit diode torch
// and every glow billboard, but nothing consumed it: `block_cutout.frag` ran
// `cardinalShade(normal)` on every fragment, so the six billboards around a lit
// torch — which point six different ways — came out at four different
// brightnesses and the glow read as no glow at all. That is checklist item #6.
//
// Nothing here can see a pixel. What it can pin is the two ends of the wire:
//   * the mesh vertex carries the element's shade bit (and no longer carries the
//     fabricated `glow` light floor RN-13 removed with it), and
//   * the shaders actually read that byte and branch on it.
// The second half is a source-text lockstep in the shape item_cube_uv_test
// already uses, and it is the half that was missing before: a vertex attribute
// nobody samples looks exactly like a vertex attribute that works.

#include "render/MeshData.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/ElementModelBaker.hpp"
#include "world/WorldConstants.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif

namespace {

[[nodiscard]] std::string readShader(std::string_view name) {
    const std::filesystem::path path = std::filesystem::path{MC_REBEDROCK_SHADER_SRC_DIR} / name;
    std::ifstream stream{path};
    if (!stream) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Meshes one cell holding `state` at (3, kMinY+1, 4) of an otherwise empty
// chunk, so nothing culls and the mesh is exactly the block's own quads.
[[nodiscard]] mc::render::MeshData meshOne(mc::world::BlockState state) {
    mc::world::Chunk chunk;
    chunk.setState(3, mc::world::kMinY + 1, 4, state);
    return mc::world::ChunkMesher::build(chunk);
}

} // namespace

int main() {
    using mc::world::Block;
    using mc::world::BlockOrientation;
    using mc::world::BlockState;

    // --- The mesh end: every vertex of quad i carries quad i's shade bit. The
    //     mesher emits baked quads in order, four vertices each, which is the
    //     same pairing chunk_mesher_test's position check relies on. ---
    const std::array<BlockState, 4> cases{{
        BlockState{Block::Repeater, BlockOrientation::East}.withRepeaterDelay(2).withPowered(true),
        BlockState{Block::Repeater, BlockOrientation::East}.withRepeaterDelay(2),
        BlockState{Block::Comparator, BlockOrientation::North}.withPowered(true),
        BlockState{Block::Comparator, BlockOrientation::North}.withComparatorSubtract(true),
    }};
    for (const BlockState& state : cases) {
        const auto mesh = meshOne(state);
        const auto quads = mc::world::bake::bakeElementModel(state.block(), state);
        assert(mesh.vertices.size() == quads.size() * 4U);
        for (std::size_t q = 0; q < quads.size(); ++q) {
            for (std::size_t corner = 0; corner < 4U; ++corner) {
                assert(mc::render::decodeShade(mesh.vertices[q * 4U + corner]) == quads[q].shade);
            }
        }
    }

    // --- A powered repeater is the discriminating case: it has BOTH kinds of
    //     vertex. An all-true mesh means the bit never left the baker; an
    //     all-false one means it was inverted, and either would satisfy a check
    //     that only counted one kind. ---
    {
        const auto mesh = meshOne(BlockState{Block::Repeater, BlockOrientation::East}
                                      .withRepeaterDelay(1)
                                      .withPowered(true));
        int shaded = 0;
        int unshaded = 0;
        for (const auto& vertex : mesh.vertices) {
            (mc::render::decodeShade(vertex) ? shaded : unshaded)++;
        }
        // The slab base is shaded (six faces); the two lit torches and their two
        // six-billboard haloes are not.
        assert(shaded == 6 * 4);
        assert(unshaded > 0);
        assert(shaded + unshaded == static_cast<int>(mesh.vertices.size()));
    }

    // --- An unlit diode has no `"shade"` anywhere in its model (grep
    //     repeater_1tick.json / comparator.json: zero hits), so every vertex is
    //     shaded. ---
    {
        const auto mesh = meshOne(BlockState{Block::Repeater, BlockOrientation::South});
        assert(!mesh.vertices.empty());
        for (const auto& vertex : mesh.vertices) {
            assert(mc::render::decodeShade(vertex));
        }
    }

    // --- RN-13 deleted the `glow` field, which floored a lit diode torch's
    //     vertex block light at 0.5. Neither Blocks.REPEATER (Blocks.java:2089)
    //     nor Blocks.COMPARATOR (:2762) declares a lightLevel, and their models
    //     declare no `light_emission`, so a lit repeater emits nothing: in an
    //     unlit chunk every one of its vertices reads block light 0, torches and
    //     haloes included. ---
    {
        const auto mesh = meshOne(BlockState{Block::Comparator, BlockOrientation::North}
                                      .withPowered(true)
                                      .withComparatorSubtract(true));
        assert(!mesh.vertices.empty());
        for (const auto& vertex : mesh.vertices) {
            assert(mc::render::decodeBlockLight(vertex) == 0.0F);
        }
    }

    // --- The vertex layout: the shade byte is the fourth of the tint attribute,
    //     which is what lets the shader read it as `inTint.w` without a new
    //     VkVertexInputAttributeDescription. A reshuffle that moved it out of
    //     that slot would silently feed the shader a light channel. ---
    {
        using mc::render::VoxelVertex;
        static_assert(sizeof(VoxelVertex) == 24);
        static_assert(offsetof(VoxelVertex, shade) == offsetof(VoxelVertex, tintR) + 3);
        const auto shaded = mc::render::packVertex({}, {0.0F, 1.0F, 0.0F}, {}, 0.0F, 1.0F, 0.0F,
                                                   0.0F, 0.0F, 0.0F, 0.0F);
        assert(mc::render::decodeShade(shaded) && "packVertex defaults to shaded");
        const auto unshaded = mc::render::packVertex({}, {0.0F, 1.0F, 0.0F}, {}, 0.0F, 1.0F, 0.0F,
                                                     0.0F, 0.0F, 0.0F, 0.0F, 255U, 255U, 255U, 0U,
                                                     /*shade=*/false);
        assert(!mc::render::decodeShade(unshaded));
    }

    // --- The shader end. A vertex attribute nobody samples is indistinguishable
    //     from one that works, so these check that the bit is read out of
    //     inTint.w, passed along location 12, and actually branches the cardinal
    //     falloff in BOTH fragment shaders that share grass_block.vert. ---
    {
        const std::string vertexSource = readShader("grass_block.vert");
        assert(contains(vertexSource, "layout(location = 12) flat out float fragmentShade;"));
        assert(contains(vertexSource, "fragmentShade = float(inTint.w) / 255.0;"));

        for (const std::string_view name : {"block_cutout.frag", "grass_block.frag"}) {
            const std::string fragmentSource = readShader(name);
            assert(contains(fragmentSource, "layout(location = 12) flat in float fragmentShade;"));
            // The branch itself: an unshaded face takes 1.0, a shaded one takes
            // CardinalLighting. Both halves are asserted, so deleting either the
            // branch or the call is caught.
            assert(contains(fragmentSource,
                            "fragmentShade < 0.5 ? 1.0 : cardinalShade(normal)"));
        }
    }

    return 0;
}
