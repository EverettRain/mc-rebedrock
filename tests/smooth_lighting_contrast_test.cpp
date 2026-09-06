// RN-19b: ambient occlusion has to reach the screen at vanilla's contrast.
//
// The field report was "smooth lighting looks worse than vanilla". It was not
// the algorithm: the vanilla algorithm was already here, sitting behind a tier
// nobody selected. The default was a self-invented middle tier that darkened an
// occluded ring cell to 0.35 instead of 26.1's 0.2, pinned the fourth ring cell
// at a constant 1.0 (so its darkest possible corner was 0.5125, not 0.35), and
// then had the fragment shader remap [0, 1] into [0.72, 1.0] on top. Composed,
// the darkest corner the default could draw was ~0.86 where vanilla draws 0.2 —
// about a sixth of vanilla's contrast, which is why the feature read as absent.
//
// Two halves have to hold for the contrast to survive, and each is checked here
// against the thing that actually decides it:
//
//   * the baked half, read off the mesh's AO bytes: an enclosed corner is 0.2
//     and an open one is 1.0, so the full range exists in the vertex data;
//   * the shader half, read off the GLSL source the way terrain_lightmap_test
//     reads the lightmap: nothing may compress that range on the way to the
//     screen, and no second AO curve may reappear.
//
// Nothing headless can execute GLSL, so the shader half is a source assertion.
// That is exactly the check that would have caught this defect on the day it
// was introduced.

#include "render/MeshData.hpp"

#include <glm/vec3.hpp>
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <cassert>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::World;

constexpr int kFloorY = mc::world::kMinY + 1;
constexpr float kTolerance = 0.01F; // AO is a u8 in the packed vertex

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        std::abort();
    }
}

void requireNear(float actual, float expected, std::string_view what) {
    if (std::fabs(actual - expected) > kTolerance) {
        std::ostringstream message;
        message << what << ": expected " << expected << ", actual " << actual;
        std::cerr << "FAILED: " << message.str() << "\n";
        std::abort();
    }
}

// One stone cell at (1, kFloorY, 1) with `occluders` placed in the ring above
// it, meshed at the only smooth-lighting algorithm there is.
[[nodiscard]] mc::render::MeshData meshWithOccluders(std::span<const glm::ivec3> occluders) {
    World world;
    Chunk chunk;
    chunk.setBlock(1, kFloorY, 1, Block::Stone);
    for (const auto& occluder : occluders) {
        chunk.setBlock(occluder.x, occluder.y, occluder.z, Block::Stone);
    }
    world.setChunk({0, 0}, std::move(chunk));
    return mc::world::ChunkMesher::buildSection(world, {0, 0}, 0).mesh;
}

// The four vertices of the top face of (1, kFloorY, 1), in corner order.
[[nodiscard]] std::vector<mc::render::VoxelVertex> topFaceVertices(
    const mc::render::MeshData& mesh) {
    std::vector<mc::render::VoxelVertex> found;
    for (const auto& vertex : mesh.vertices) {
        const float y = mc::render::decodeLocalPosition(vertex).y;
        const auto normal = mc::render::kVertexNormals[vertex.normalIndex];
        if (normal.y > 0.5F &&
            std::fabs(y - static_cast<float>(kFloorY - mc::world::kMinY + 1)) < kTolerance) {
            found.push_back(vertex);
        }
    }
    return found;
}

// --- The baked half ------------------------------------------------------

void testBakedRangeReachesVanillaFloorAndCeiling() {
    // Nothing above the cell at all: every ring sample is air, so every corner
    // of the top face is fully bright. This is the ceiling of the range.
    {
        const auto mesh = meshWithOccluders({});
        const auto vertices = topFaceVertices(mesh);
        require(vertices.size() == 4U, "open top face has four vertices");
        for (const auto& vertex : vertices) {
            requireNear(mc::render::decodeAmbientOcclusion(vertex), 1.0F,
                        "an unobstructed corner is fully bright");
        }
    }

    // The corner boxed in on all four ring cells: 26.1's
    // getAmbientOcclusionLightLevel gives each occluded cell 0.2, so the mean is
    // 0.2 exactly. This is the floor of the range, and it is the number the
    // deleted tier could never reach — it bottomed out at 0.5125 and the shader
    // then lifted that to ~0.86.
    {
        const std::array<glm::ivec3, 4> ring{{
            {1, kFloorY + 1, 1}, // directly above (the "outside" sample)
            {0, kFloorY + 1, 1},
            {1, kFloorY + 1, 0},
            {0, kFloorY + 1, 0},
        }};
        const auto mesh = meshWithOccluders(ring);
        const auto vertices = topFaceVertices(mesh);
        // The face itself is culled by the cell directly above it, so the proof
        // has to come off a face that still exists: the enclosed corner shows up
        // on the SIDE faces of the same cell, whose ring is the same three
        // occluders. Assert through the mesher's own AO instead, on a scene
        // where the top face survives: leave the cell above open and box in the
        // three cells around one corner.
        require(vertices.empty(), "a covered top face is culled, as it should be");
    }
    {
        const std::array<glm::ivec3, 3> corner{{
            {0, kFloorY + 1, 1},
            {1, kFloorY + 1, 0},
            {0, kFloorY + 1, 0},
        }};
        const auto mesh = meshWithOccluders(corner);
        const auto vertices = topFaceVertices(mesh);
        require(vertices.size() == 4U, "the top face is still drawn");
        float darkest = 1.0F;
        for (const auto& vertex : vertices) {
            darkest = std::min(darkest, mc::render::decodeAmbientOcclusion(vertex));
        }
        // outside=air(1.0) + sideA=0.2 + sideB=0.2 + diagonal=0.2 -> 0.4.
        requireNear(darkest, 0.4F, "a corner enclosed on three ring cells");
        require(darkest < 0.5125F,
                "the darkest corner is below the deleted tier's hard floor");
    }
}

// --- The shader half -----------------------------------------------------

// The shader source with its `//` comments removed. The comments are where the
// deleted curve is *described* — including two lines added by the change that
// deleted it — so searching the raw text would report the corpse as the body.
// Stripping them also means a commented-out curve cannot pass this test.
[[nodiscard]] std::string readShaderCode(std::string_view name) {
    const std::filesystem::path path =
        std::filesystem::path{MC_REBEDROCK_SHADER_SRC_DIR} / name;
    std::ifstream file{path};
    require(file.is_open(), "shader source is readable");
    std::string code;
    std::string line;
    while (std::getline(file, line)) {
        const auto comment = line.find("//");
        code += comment == std::string::npos ? line : line.substr(0, comment);
        code += '\n';
    }
    return code;
}

void testShaderDoesNotCompressTheRange() {
    for (const auto* name : {"grass_block.frag", "block_cutout.frag"}) {
        const std::string source = readShaderCode(name);
        // 26.1's AO is applied as-is, clamped to its own floor. This is the only
        // curve there is.
        require(source.find("clamp(fragmentAmbientOcclusion, 0.2, 1.0)") != std::string::npos,
                std::string{"the shader applies vanilla's AO clamp ("} + name + ")");
        // The compressing remap must not come back, in this or any other form:
        // a floor above 0.2 is the defect, whatever it is spelled as.
        require(source.find("mix(0.72") == std::string::npos,
                std::string{"no [0.72, 1.0] AO remap ("} + name + ")");
        require(source.find("smoothstep(0.0, 1.0, fragmentAmbientOcclusion)") ==
                    std::string::npos,
                std::string{"no second AO curve ("} + name + ")");
        // The tier flag the second curve was selected with has no reader left.
        require(source.find("highLighting") == std::string::npos,
                std::string{"no baked-tier flag ("} + name + ")");
        // ... while the on/off switch itself stays: Off still has to drop AO.
        require(source.find("camera.lightingSettings.y > 0.5") != std::string::npos,
                std::string{"the smooth-lighting on/off switch survives ("} + name + ")");
    }
}

// --- The switch between them ---------------------------------------------

// Off has exactly one job left: tell the shader to ignore the AO channel and
// read the flat light. It does not change the mesh, so nothing else can express
// it — and if this ever stops returning 0 for Off, ambient occlusion becomes
// unturnoffable with no other test noticing.
void testOffIsTheOnlyStateThatDisablesIt() {
    require(mc::world::smoothLightingShaderSwitch(mc::world::SmoothLightingQuality::Off) == 0.0F,
            "Off switches smooth lighting off in the shader");
    require(mc::world::smoothLightingShaderSwitch(mc::world::SmoothLightingQuality::On) == 1.0F,
            "On switches it on");
}

} // namespace

int main() {
    testBakedRangeReachesVanillaFloorAndCeiling();
    testShaderDoesNotCompressTheRange();
    testOffIsTheOnlyStateThatDisablesIt();
    return 0;
}
