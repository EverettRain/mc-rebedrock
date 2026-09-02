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

    // --- RN-8c-D: the five layers a block item's cube needs -------------------
    //
    // Before this, the dropped and held cube got only top/side/bottom, so every
    // DirectionalCube was a uniform side-texture box: a dropped piston had no
    // platform, a dropped observer no face. The front sits on the model's north
    // face and the back on its south one, because a block item is the block's own
    // model with no blockstate rotation.
    {
        // The layer table is filled by the renderer at startup; give the two
        // blocks under test known values so the routing is what is being checked,
        // not the atlas.
        setBlockTextureLayers(Block::Stone, {11.0F, 12.0F, 13.0F});
        setBlockDirectionalLayers(Block::Observer,
                                  {/*front*/ 21.0F, /*frontActive*/ 22.0F, /*back*/ 23.0F,
                                   /*backActive*/ 24.0F, /*top*/ 25.0F, /*bottom*/ 26.0F,
                                   /*side*/ 27.0F});
        setBlockTextureLayers(Block::Observer, {31.0F, 32.0F, 33.0F});

        // A plain cube has no front of its own: both fall back to side, which is
        // exactly what every item cube used to do for every block.
        const auto stone = cubeItemLayers(Block::Stone);
        assert(stone.top == 11.0F && stone.side == 12.0F && stone.bottom == 13.0F);
        assert(stone.front == 12.0F && stone.back == 12.0F);

        // A DirectionalCube answers its own six faces, and takes the UNLIT front:
        // a block item has no state, and vanilla's item model is the unlit one.
        const auto observer = cubeItemLayers(Block::Observer);
        assert(observer.top == 25.0F && observer.bottom == 26.0F && observer.side == 27.0F);
        assert(observer.front == 21.0F && observer.back == 23.0F);
        assert(observer.front != 22.0F);

        // The packing the shader decodes: `1 + front + back * stride`, with zero
        // reserved for "not supplied" so the block-breaking overlay and falling
        // blocks keep side on every side face.
        const float packed = packItemFrontBackLayers(observer.front, observer.back);
        assert(packed > 0.5F);
        const float value = packed - 1.0F;
        const float decodedFront = std::fmod(value, kItemLayerPackStride);
        const float decodedBack = std::floor(value / kItemLayerPackStride);
        assert(decodedFront == observer.front);
        assert(decodedBack == observer.back);

        // The encoding has to survive a float32 round trip for every layer the
        // atlas can hold, which is what bounds the stride: every integer up to
        // 2^24 is exact in a float32, and the widest packed value is
        // 1 + (stride-1) + (stride-1)*stride = stride^2. So 4096 is the largest
        // stride that is exact, and it sits exactly on the boundary rather than
        // over it.
        const float widest =
            packItemFrontBackLayers(kItemLayerPackStride - 1.0F, kItemLayerPackStride - 1.0F);
        assert(widest == kItemLayerPackStride * kItemLayerPackStride);
        assert(widest <= 16777216.0F);
        assert(std::fmod(widest - 1.0F, kItemLayerPackStride) == kItemLayerPackStride - 1.0F);
        assert(std::floor((widest - 1.0F) / kItemLayerPackStride) == kItemLayerPackStride - 1.0F);

        // The shader has to agree about the stride AND about which half of the
        // packed value is which. A headless test cannot run GLSL, so this checks
        // the source's shape: the two decode expressions, each bound to the right
        // face. It is a literal match on purpose — if the shader is reformatted
        // the test fails loudly and whoever reformatted it re-reads this, which is
        // the same contract the UV tables above are held to.
        const std::string source = readFile(kShaderDir / "item_entity.vert");
        std::ostringstream stride;
        stride << static_cast<int>(kItemLayerPackStride) << ".0";
        assert(source.find(stride.str()) != std::string::npos);
        assert(source.find("frontLayer = mod(packedFaces, " + stride.str() + ")") !=
               std::string::npos);
        assert(source.find("backLayer = floor(packedFaces / " + stride.str() + ")") !=
               std::string::npos);
        // And that the front lands on the model's north face (-Z, face 5), not on
        // its south one — the swap the old held-item hack had. Matched with the
        // whitespace stripped, so reformatting the shader does not fail this.
        std::string dense;
        for (const char character : source) {
            if (character != ' ' && character != '\n' && character != '\t' &&
                character != '\r') {
                dense.push_back(character);
            }
        }
        assert(dense.find("face==5?frontLayer") != std::string::npos);
        assert(dense.find("face==4?backLayer") != std::string::npos);
    }

    // --- RN-8c-D regression: exactly one face carries the front ---------------
    //
    // Reported from a real run: a dropped furnace showed its opening on three or
    // four faces and a dropped piston its platform on three. The cause was not the
    // packing but where the OTHER three layers came from — the dropped cube took
    // top/side/bottom from `textureLayers`, and the atlas baker deliberately puts
    // a DirectionalCube's FRONT layer in that triple's `side` slot so a
    // three-slot item cube is still recognisable (BlockAtlasBaker.cpp). Feeding
    // the real front in as well then painted it on +X, -X and -Z at once.
    //
    // This walks all six faces the way item_entity.vert selects them and counts.
    {
        setBlockDirectionalLayers(Block::Furnace,
                                  {/*front*/ 41.0F, /*frontActive*/ 42.0F, /*back*/ 43.0F,
                                   /*backActive*/ 44.0F, /*top*/ 45.0F, /*bottom*/ 46.0F,
                                   /*side*/ 47.0F});
        // What the baker leaves behind for a DirectionalCube. It used to put the
        // FRONT in this triple's middle slot, which is what made the dropped
        // furnace show its opening on three faces once the real front arrived as
        // well; the slot is the side now, and nothing that draws a per-face cube
        // reads it either way.
        setBlockTextureLayers(Block::Furnace, {45.0F, 47.0F, 46.0F});

        const auto faces = cubeItemLayers(Block::Furnace);
        constexpr std::array<Face, 6> kAll{Face::PositiveX, Face::NegativeX, Face::PositiveY,
                                           Face::NegativeY, Face::PositiveZ, Face::NegativeZ};
        int frontFaces = 0;
        int backFaces = 0;
        int sideFaces = 0;
        for (const Face face : kAll) {
            const float layer = cubeItemFaceLayer(faces, face);
            if (layer == 41.0F) ++frontFaces;
            if (layer == 43.0F) ++backFaces;
            if (layer == 47.0F) ++sideFaces;
        }
        assert(frontFaces == 1);
        assert(backFaces == 1);
        assert(sideFaces == 2);
        assert(cubeItemFaceLayer(faces, Face::NegativeZ) == 41.0F); // front on north
        assert(cubeItemFaceLayer(faces, Face::PositiveZ) == 43.0F); // back on south
        assert(cubeItemFaceLayer(faces, Face::PositiveY) == 45.0F);
        assert(cubeItemFaceLayer(faces, Face::NegativeY) == 46.0F);

        // A per-face cube takes every layer from cubeItemLayers, never from the
        // flat triple — the triple has no front or back to give, which is the
        // whole reason the dropped cube had none.
        assert(faces.front != faces.side);
        assert(faces.back != faces.side);
    }

    return 0;
}
