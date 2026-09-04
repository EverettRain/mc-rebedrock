#include "world/Block.hpp"
#include "world/CubeUv.hpp"
#include "world/ItemModel.hpp"

#include <array>
#include <cassert>
#include <cctype>
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

// Every `vec3(a, b, c)` between the two markers, in source order.
[[nodiscard]] std::vector<glm::vec3> parseVec3Block(const std::string& source,
                                                    std::string_view beginMarker,
                                                    std::string_view endMarker) {
    const auto begin = source.find(beginMarker);
    assert(begin != std::string::npos && "marker missing: did the shader lose its table?");
    const auto end = source.find(endMarker, begin);
    assert(end != std::string::npos && end > begin);
    const std::string block = source.substr(begin, end - begin);
    std::vector<glm::vec3> values;
    std::size_t cursor = 0;
    while (true) {
        const auto call = block.find("vec3(", cursor);
        if (call == std::string::npos) {
            break;
        }
        const auto close = block.find(')', call);
        assert(close != std::string::npos);
        std::string arguments = block.substr(call + 5, close - call - 5);
        std::array<float, 3> component{};
        std::size_t at = 0;
        for (float& value : component) {
            const auto comma = arguments.find(',', at);
            value = std::stof(arguments.substr(at, comma - at));
            at = comma == std::string::npos ? arguments.size() : comma + 1;
        }
        values.push_back({component[0], component[1], component[2]});
        cursor = close + 1;
    }
    return values;
}

// Every integer between the two markers, in source order.
[[nodiscard]] std::vector<int> parseIntBlock(const std::string& source,
                                             std::string_view beginMarker,
                                             std::string_view endMarker) {
    const auto begin = source.find(beginMarker);
    assert(begin != std::string::npos);
    const auto end = source.find(endMarker, begin);
    assert(end != std::string::npos && end > begin);
    const std::string block = source.substr(begin, end - begin);
    std::vector<int> values;
    for (std::size_t i = 0; i < block.size();) {
        if (std::isdigit(static_cast<unsigned char>(block[i])) == 0) {
            ++i;
            continue;
        }
        // Skip a number that is part of an identifier (int[24], vec3, ...).
        if (i > 0 && (std::isalnum(static_cast<unsigned char>(block[i - 1])) != 0 ||
                      block[i - 1] == '_' || block[i - 1] == '[')) {
            while (i < block.size() && std::isalnum(static_cast<unsigned char>(block[i])) != 0) {
                ++i;
            }
            continue;
        }
        std::size_t start = i;
        while (i < block.size() && std::isdigit(static_cast<unsigned char>(block[i])) != 0) {
            ++i;
        }
        values.push_back(std::stoi(block.substr(start, i - start)));
    }
    return values;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
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

    // --- The rule the two shaders now compute UVs with -------------------------
    // RN-14 deleted three literal 24-entry UV tables from item_entity.vert and
    // three 18-entry ones from hud.vert. They existed because a shader cannot
    // include this header, so a block item's UVs had to be spelled out — and only
    // a CUBE's could be spelled out, which is a large part of why every shaped
    // block's item was drawn as a cube. Both shaders now take the model json's uv
    // rect per draw and sample it by JE's CuboidFace.UVs rule.
    //
    // So the first thing to pin is that the rule reproduces what the tables held:
    // the plain cube is the whole-sprite rect at quadrant 0.
    {
        for (std::size_t f = 0; f < kFaces.size(); ++f) {
            const auto corners = faceUvCorners(kWholeSpriteRect, kFaces[f].face, 0);
            for (std::size_t c = 0; c < 4; ++c) {
                assert(same(corners[c],
                            kCubeModelFaceUv[static_cast<std::size_t>(CubeUvModel::Default)][f][c]));
            }
        }
        // And that a declared rect and a declared rotation still reach it: the
        // piston's west face carries "rotation": 270, the observer's up face an
        // inverted rect.
        const auto pistonWest =
            faceUvCorners(kWholeSpriteRect, Face::NegativeX, bake::kQuadrant270);
        for (std::size_t c = 0; c < 4; ++c) {
            assert(same(pistonWest[c],
                        kCubeModelFaceUv[static_cast<std::size_t>(CubeUvModel::PistonTemplate)]
                                        [static_cast<std::size_t>(Face::NegativeX)][c]));
        }
    }

    // --- item_entity.vert: the dropped item and the held item ------------------
    // What it carries now is the bridge between its own corner order and JE's
    // FaceInfo vertex order, which is what turns a uv rect into four corner UVs.
    {
        const std::string source = readFile(kShaderDir / "item_entity.vert");
        const auto parsed = parseIntBlock(source, "---- kFaceInfoCorner begin ----",
                                          "---- kFaceInfoCorner end ----");
        assert(parsed.size() == 24);
        for (std::size_t face = 0; face < 6; ++face) {
            for (std::size_t corner = 0; corner < 4; ++corner) {
                assert(parsed[face * 4 + corner] ==
                       static_cast<int>(kFaceInfoCorner[face][corner]));
            }
        }
        // The rule itself, both halves of it. JE's getVertexU keeps minU on
        // indices 0 and 1, getVertexV keeps minV on 0 and 3, and the quadrant is
        // a cyclic shift of the index.
        assert(contains(source, "int index = (faceInfoIndex + quadrant) & 3;"));
        assert(contains(source, "(index != 0 && index != 1) ? rect.z : rect.x"));
        assert(contains(source, "(index != 0 && index != 3) ? rect.w : rect.y"));
        // The block-item modes, and the fields they read the rect out of.
        // The two are named now instead of being a threshold that covers "10 and
        // up"; `hud_push_constant_test` owns the membership assertion, this one
        // only needs the rect to still come from the same four components.
        assert(contains(source, "isItemMode(kItemModeBlockItemDropped) || "
                                "isItemMode(kItemModeBlockItemHeld)"));
        assert(contains(source, "bool blockItemHeld = isItemMode(kItemModeBlockItemHeld);"));
        assert(contains(source,
                        "vec4(item.data.y, item.data.z, item.data.w, item.positionSize.w)"));
        // The slab's hard-coded half-height V crop is gone: a slab is a box of a
        // model now, and block/slab.json's own rect says the same thing.
        assert(!contains(source, "cubeUv.y = 0.5 + cubeUv.y * 0.5"));
    }

    // --- hud.vert: the inventory icon ------------------------------------------
    // The icon used to be eighteen pre-projected SCREEN positions, which is a
    // unit cube and nothing else. It is now eighteen cube CORNERS plus a named
    // projection, so any box goes through it.
    {
        const std::string source = readFile(kShaderDir / "hud.vert");
        const auto parsed = parseVec3Block(source, "const vec3 iconCorners[18]", ");");
        assert(parsed.size() == kIconCubeCorners.size());
        for (std::size_t v = 0; v < parsed.size(); ++v) {
            assert(parsed[v].x == kIconCubeCorners[v].x && parsed[v].y == kIconCubeCorners[v].y &&
                   parsed[v].z == kIconCubeCorners[v].z);
        }
        // Every corner must be a corner of the face its group draws, or the
        // eighteen vertices have stopped describing three faces of a box.
        for (std::size_t v = 0; v < parsed.size(); ++v) {
            const Face face = kIconFaces[v / 6];
            const auto faceIndex = static_cast<std::size_t>(face);
            bool found = false;
            for (std::size_t k = 0; k < 4; ++k) {
                const glm::vec3 corner = kFaces[faceIndex].corners[k];
                found = found || (corner.x == parsed[v].x && corner.y == parsed[v].y &&
                                  corner.z == parsed[v].z);
            }
            assert(found && "icon vertex is not a corner of its face");
        }
        // The projection and the depth, term for term against mc::world.
        assert(contains(source, "vec2(0.5 + 0.44 * (p.z - p.x),"));
        assert(contains(source, "0.46 - 0.21 * (p.x + p.z) + 0.48 * (1.0 - p.y))"));
        assert(contains(source, "return (p.x - p.y + p.z + 1.0) / 3.0;"));
        assert(contains(source, "const int iconQuadCorner[6] = int[](0, 1, 2, 0, 2, 3);"));
        // The box arrives per draw; a fixed unit cube would put every shaped
        // block back to being a cube.
        //
        // It arrives in iconBoxMin/iconBoxMax, not in color/uvRect. Reading it
        // out of `color` is what made every icon a black diamond — hud.frag went
        // on multiplying that same field in as a tint. See
        // hud_push_constant_test, which holds all three declarations of the block
        // together so the next such move cannot be told to one consumer only.
        assert(contains(source, "vec3 p = mix(hud.iconBoxMin.xyz, hud.iconBoxMax.xyz, unit);"));
        assert(!contains(source, "hud.color.xyz"));
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

        // RN-14: there is no packing left to decode. A block item draw is one
        // FACE of one box, so it carries one layer — the one the model json's
        // `#top`/`#side`/`#bottom` reference names — and the front/back pair that
        // used to travel packed alongside the cube UV model in `data.y` is gone
        // with it. What remains to pin is the ROUTING: which face asks for which
        // of the five layers.
        const CubeUvModel observerUv = cubeItemUvModel(Block::Observer);
        assert(observerUv == CubeUvModel::Observer); // its item IS the block model
        {
            const auto& box = kItemModelBoxes[static_cast<std::size_t>(observerUv)];
            int fronts = 0;
            int backs = 0;
            int sides = 0;
            for (std::size_t f = 0; f < bake::kFacingCount; ++f) {
                assert(box.face[f].present);
                fronts += box.face[f].slot == ItemLayerSlot::Front ? 1 : 0;
                backs += box.face[f].slot == ItemLayerSlot::Back ? 1 : 0;
                sides += box.face[f].slot == ItemLayerSlot::Side ? 1 : 0;
            }
            // Exactly one face carries the front and one the back — the RN-8c-D
            // regression ("a dropped furnace showed its opening on three faces"),
            // now a property of the item model table rather than of a packing.
            assert(fronts == 1 && backs == 1 && sides == 2);
            assert(box.face[static_cast<std::size_t>(bake::Facing::North)].slot ==
                   ItemLayerSlot::Front);
            assert(box.face[static_cast<std::size_t>(bake::Facing::South)].slot ==
                   ItemLayerSlot::Back);
            assert(itemFaceLayer(observer, box.face[static_cast<std::size_t>(bake::Facing::North)]
                                               .slot) == observer.front);
        }
        // And the shader takes that one layer for a block item rather than
        // re-deriving a face from a triple. A headless test cannot run GLSL, so
        // this is a literal match on the source, the same contract the tables
        // above are held to.
        {
            const std::string source = readFile(kShaderDir / "item_entity.vert");
            std::string dense;
            for (const char character : source) {
                if (character != ' ' && character != '\n' && character != '\t' &&
                    character != '\r') {
                    dense.push_back(character);
                }
            }
            assert(dense.find("blockItemBox?item.textureLayersRotation.x") != std::string::npos);
            // The packing is gone, not merely unused.
            assert(source.find("packedFaces") == std::string::npos);
            assert(source.find("itemUvModel") == std::string::npos);
        }
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

    // --- RN-8c-D: a block's ITEM model is not always the block's model ---------
    //
    // Second real-run report: the dropped piston still showed its platform on a
    // side while the piston_side frame around it pointed up, and the dropped
    // observer's top arrow ran opposite to the placed one.
    //
    // Both come from the same place. vanilla keeps a block item's model in
    // assets/minecraft/items/<block>.json, and it is NOT always the block's own:
    //   items/piston.json   -> block/piston_inventory, a plain cube_bottom_top
    //                          with piston_top on TOP and no face rotations
    //   items/observer.json -> block/observer, the block's own model, whose up
    //                          face declares an inverted uv rect
    // The item cube was drawing every block as if the second case applied and
    // with the plain UV table, so the piston got its platform on the north face
    // (its BLOCK model's front) and the observer lost its inverted top.
    {
        setBlockTextureLayers(Block::Piston, {51.0F, 52.0F, 53.0F});
        setBlockDirectionalLayers(Block::Piston,
                                  {/*front*/ 51.0F, /*frontActive*/ 51.0F, /*back*/ 53.0F,
                                   /*backActive*/ 53.0F, /*top*/ 52.0F, /*bottom*/ 52.0F,
                                   /*side*/ 52.0F});

        // The piston's item is a plain cube: platform on TOP, side on all four
        // sides, no front or back of its own — and, because a cube_bottom_top
        // declares no face rotation, the Default UV table however much the
        // piston's own model rotates.
        assert(blockDefinition(Block::Piston).cubeItemModel == CubeItemModel::PlainCube);
        assert(blockDefinition(Block::StickyPiston).cubeItemModel == CubeItemModel::PlainCube);
        assert(!cubeItemUsesBlockModel(Block::Piston));
        assert(blockDefinition(Block::Piston).cubeUvModel == CubeUvModel::PistonTemplate);
        assert(cubeItemUvModel(Block::Piston) == CubeUvModel::Default);

        const auto piston = cubeItemLayers(Block::Piston);
        assert(cubeItemFaceLayer(piston, Face::PositiveY) == 51.0F); // the platform, on top
        assert(cubeItemFaceLayer(piston, Face::NegativeY) == 53.0F);
        for (const Face side : {Face::PositiveX, Face::NegativeX, Face::PositiveZ,
                                Face::NegativeZ}) {
            assert(cubeItemFaceLayer(piston, side) == 52.0F);
        }

        // The observer's item IS the block model, so it keeps both its own faces
        // and its own inverted top rect.
        assert(blockDefinition(Block::Observer).cubeItemModel == CubeItemModel::BlockModel);
        assert(cubeItemUsesBlockModel(Block::Observer));
        assert(cubeItemUvModel(Block::Observer) == CubeUvModel::Observer);
        assert(cubeItemUvModel(Block::Furnace) == CubeUvModel::Default);
        assert(cubeItemUsesBlockModel(Block::Furnace));

        // A plain cube is a plain cube either way.
        assert(!cubeItemUsesBlockModel(Block::Stone));
        assert(cubeItemUvModel(Block::Stone) == CubeUvModel::Default);
    }

    return 0;
}
