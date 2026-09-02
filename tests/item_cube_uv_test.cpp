#include "world/Block.hpp"
#include "world/CubeUv.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif

// RN-8c follow-up: a block ITEM is drawn as a cube too — as a dropped entity, as
// the held item and as the inventory icon — but those three cubes are generated
// in a vertex shader, so they carried their own copy of the pre-RN-8c UV
// convention and stayed a quarter turn out after the world was fixed.
//
// A shader cannot include the C++ header, so it carries the numbers as GLSL
// literals. This test is what keeps the two from drifting: it derives the table
// from the same FaceBakery rules the chunk mesher uses and checks it against the
// literals parsed out of the shader source.
namespace {

const std::filesystem::path kShaderDir{MC_REBEDROCK_SHADER_SRC_DIR};

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Every `vec2(a, b)` between the two markers, in source order. Comments inside
// the block are skipped because they never contain a vec2 literal.
[[nodiscard]] std::vector<glm::vec2> parseVec2Block(const std::string& source,
                                                    std::string_view beginMarker,
                                                    std::string_view endMarker) {
    const auto begin = source.find(beginMarker);
    const auto end = source.find(endMarker);
    assert(begin != std::string::npos && "marker missing: did the shader lose its table?");
    assert(end != std::string::npos && end > begin);
    const std::string block = source.substr(begin, end - begin);

    std::vector<glm::vec2> values;
    std::size_t cursor = 0;
    while (true) {
        const auto call = block.find("vec2(", cursor);
        if (call == std::string::npos) {
            break;
        }
        const auto close = block.find(')', call);
        assert(close != std::string::npos);
        const std::string arguments = block.substr(call + 5, close - call - 5);
        const auto comma = arguments.find(',');
        assert(comma != std::string::npos);
        values.push_back({std::stof(arguments.substr(0, comma)),
                          std::stof(arguments.substr(comma + 1))});
        cursor = close + 1;
    }
    return values;
}

[[nodiscard]] bool same(const glm::vec2& a, const glm::vec2& b) {
    return std::fabs(a.x - b.x) < 1.0e-4F && std::fabs(a.y - b.y) < 1.0e-4F;
}

} // namespace

int main() {
    using namespace mc::world;

    // The table itself, before anything reads a shader: a block item is the
    // block's own model with no blockstate rotation, so its six faces are the
    // world cube's at the identity FACING. Spot-checked against JE
    // defaultFaceUV by hand — the up face reads u = x, v = z; the down face
    // u = x, v = 1-z; a side face keeps what it always had.
    {
        const auto& up = kCubeItemFaceUv[static_cast<std::size_t>(Face::PositiveY)];
        assert(same(up[0], {0.0F, 0.0F}) && same(up[1], {0.0F, 1.0F}) &&
               same(up[2], {1.0F, 1.0F}) && same(up[3], {1.0F, 0.0F}));
        const auto& down = kCubeItemFaceUv[static_cast<std::size_t>(Face::NegativeY)];
        assert(same(down[0], {0.0F, 0.0F}) && same(down[1], {0.0F, 1.0F}) &&
               same(down[2], {1.0F, 1.0F}) && same(down[3], {1.0F, 0.0F}));
        const auto& east = kCubeItemFaceUv[static_cast<std::size_t>(Face::PositiveX)];
        assert(same(east[0], {0.0F, 1.0F}) && same(east[1], {1.0F, 1.0F}) &&
               same(east[2], {1.0F, 0.0F}) && same(east[3], {0.0F, 0.0F}));
    }

    // --- item_entity.vert: the dropped item and the held item ------------------
    // Its cube face order and corner order are the mesher's kFaces order, so the
    // array is kCubeItemFaceUv flattened.
    {
        const std::string source = readFile(kShaderDir / "item_entity.vert");
        const auto parsed = parseVec2Block(source, "---- kCubeItemFaceUv begin ----",
                                           "---- kCubeItemFaceUv end ----");
        assert(parsed.size() == 24);
        for (std::size_t face = 0; face < 6; ++face) {
            for (std::size_t corner = 0; corner < 4; ++corner) {
                assert(same(parsed[face * 4 + corner], kCubeItemFaceUv[face][corner]));
            }
        }
    }

    // --- hud.vert: the inventory icon ------------------------------------------
    // The icon is vanilla's `gui` item transform (rotation [30, 225, 0]), which
    // shows three faces: the up face as the top diamond, the model's north face
    // as the left parallelogram, its west face as the right one. Each of the 18
    // vertices is one cube corner, so its UV is that face's entry for the corner
    // standing there. The corner->kFaces-index mapping below is stated from the
    // projection, independently of what the shader says.
    {
        // Which of the face's four kFaces corners each shader vertex stands on.
        struct IconVertex final {
            Face face;
            glm::vec3 corner; // the model-space cube corner, 0..1
        };
        constexpr glm::vec3 a{1.0F, 1.0F, 1.0F}; // diamond top      (farthest)
        constexpr glm::vec3 b{0.0F, 1.0F, 1.0F}; // diamond right
        constexpr glm::vec3 c{0.0F, 1.0F, 0.0F}; // diamond bottom   (nearest)
        constexpr glm::vec3 d{1.0F, 1.0F, 0.0F}; // diamond left
        constexpr glm::vec3 e{0.0F, 0.0F, 0.0F}; // near bottom corner
        constexpr glm::vec3 f{1.0F, 0.0F, 0.0F}; // left face, bottom outer
        constexpr glm::vec3 g{0.0F, 0.0F, 1.0F}; // right face, bottom outer
        const std::array<IconVertex, 18> icon{{
            // top diamond: [top, right, bottom] then [top, bottom, left]
            {Face::PositiveY, a}, {Face::PositiveY, b}, {Face::PositiveY, c},
            {Face::PositiveY, a}, {Face::PositiveY, c}, {Face::PositiveY, d},
            // left parallelogram (north face): [tl, tr, br] then [tl, br, bl]
            {Face::NegativeZ, d}, {Face::NegativeZ, c}, {Face::NegativeZ, e},
            {Face::NegativeZ, d}, {Face::NegativeZ, e}, {Face::NegativeZ, f},
            // right parallelogram (west face): [tl, tr, br] then [tl, br, bl]
            {Face::NegativeX, c}, {Face::NegativeX, b}, {Face::NegativeX, g},
            {Face::NegativeX, c}, {Face::NegativeX, g}, {Face::NegativeX, e},
        }};

        const std::string source = readFile(kShaderDir / "hud.vert");
        const auto parsed = parseVec2Block(source, "---- kCubeItemFaceUv icon begin ----",
                                           "---- kCubeItemFaceUv icon end ----");
        // One table of 18 per cube model, in CubeUvModel order.
        assert(parsed.size() == 18 * kCubeUvModelCount);
        for (std::size_t model = 0; model < kCubeUvModelCount; ++model) {
            for (std::size_t v = 0; v < icon.size(); ++v) {
                const auto faceIndex = static_cast<std::size_t>(icon[v].face);
                // Find which of the face's four corners this vertex stands on.
                std::size_t cornerIndex = 4;
                for (std::size_t k = 0; k < 4; ++k) {
                    const glm::vec3 corner = kFaces[faceIndex].corners[k];
                    if (corner.x == icon[v].corner.x && corner.y == icon[v].corner.y &&
                        corner.z == icon[v].corner.z) {
                        cornerIndex = k;
                    }
                }
                assert(cornerIndex < 4 && "icon vertex is not a corner of its face");
                assert(same(parsed[model * 18 + v],
                            kCubeModelFaceUv[model][faceIndex][cornerIndex]));
            }
        }
    }

    // --- the declared models actually differ, so the indexing is not vacuous ---
    // The piston's west face carries "rotation": 270 and the observer's up face an
    // inverted rect; if either stopped reaching the table, the icon would silently
    // fall back to the plain cube's numbers.
    {
        const auto west = static_cast<std::size_t>(Face::NegativeX);
        const auto up = static_cast<std::size_t>(Face::PositiveY);
        const auto plain = static_cast<std::size_t>(CubeUvModel::Default);
        const auto piston = static_cast<std::size_t>(CubeUvModel::PistonTemplate);
        const auto observer = static_cast<std::size_t>(CubeUvModel::Observer);
        bool westDiffers = false;
        bool upDiffers = false;
        for (std::size_t c = 0; c < 4; ++c) {
            westDiffers |= !same(kCubeModelFaceUv[piston][west][c],
                                 kCubeModelFaceUv[plain][west][c]);
            upDiffers |= !same(kCubeModelFaceUv[observer][up][c],
                               kCubeModelFaceUv[plain][up][c]);
        }
        assert(westDiffers);
        assert(upDiffers);
        // The observer's up face is the plain cube's mirrored in V: its rect is
        // [0,16,16,0] against the plain [0,0,16,16].
        for (std::size_t c = 0; c < 4; ++c) {
            const glm::vec2 plainUv = kCubeModelFaceUv[plain][up][c];
            const glm::vec2 observerUv = kCubeModelFaceUv[observer][up][c];
            assert(same(observerUv, {plainUv.x, 1.0F - plainUv.y}));
        }
        // And the blocks that declare them are the ones expected to.
        assert(blockDefinition(Block::Piston).cubeUvModel == CubeUvModel::PistonTemplate);
        assert(blockDefinition(Block::StickyPiston).cubeUvModel == CubeUvModel::PistonTemplate);
        assert(blockDefinition(Block::Observer).cubeUvModel == CubeUvModel::Observer);
        assert(blockDefinition(Block::Stone).cubeUvModel == CubeUvModel::Default);
        assert(blockDefinition(Block::Furnace).cubeUvModel == CubeUvModel::Default);
    }

    return 0;
}
