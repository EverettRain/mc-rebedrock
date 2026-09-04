// A face that takes a per-vertex biome tint must sample an UNTINTED atlas layer.
//
// The atlas bake also produces *tinted* copies of the same textures — grass,
// short grass, oak leaves and the five biome leaves — because items and the GUI
// have no biome to ask and need a colour baked in. Terrain must not read those.
// A face that multiplies a pre-tinted layer by the biome colour tints it twice,
// which squares the colour: plains grass at 0x91BD59 becomes roughly 0x53886F
// worth of green, and the ground reads as "everything got darker" with no single
// line of code looking wrong.
//
// This is a live trap, not a hypothetical. Mangrove leaves are correct today
// only by coincidence: `tintKindFor` gives them Foliage (matching 26.1's
// BlockColors, which registers `foliage()` for MANGROVE_LEAVES alongside vine
// and the oak family), the bake never registers a terrain leaf layer for them,
// and the roster layer they fall back to happens to be untinted. Add mangrove to
// the baker's `biomeLeafNames` — the list that pre-tints — and it double-tints
// with nothing to catch it. So the two halves are joined here instead of trusted
// separately: the baker publishes which layers it tinted, the mesher publishes
// which faces take a vertex tint, and the two sets must not intersect.

#include "assets/ImageData.hpp"
#include "assets/ResourceProvider.hpp"
#include "render/vulkan/BlockAtlasBaker.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/ChunkMesher.hpp"
#include "world/gen/Biome.hpp"

// 实现体在 src/assets/StbImageImplementation.cpp（RN-15b 起共用），这里只要声明
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error{"biome_tint_layers_test line " + std::to_string(line) + ": " +
                                 message};
    }
}

#define REQUIRE(condition, message) require(condition, message, __LINE__)

using mc::world::Block;
using mc::world::BiomeTintKind;
using mc::world::Face;

// Encodes a solid mid-grey RGBA image as PNG bytes. The guard compares layer
// *indices*, never pixels, so the content is irrelevant — the bake just has to
// complete. Mid-grey (not black) keeps any future pixel assertion meaningful.
[[nodiscard]] std::vector<std::byte> greyPng(int width, int height) {
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(height) * 4U, 0x80);
    for (std::size_t i = 3; i < rgba.size(); i += 4U) {
        rgba[i] = 0xFF;
    }
    std::vector<std::byte> out;
    const auto append = [](void* context, void* data, int size) {
        auto* sink = static_cast<std::vector<std::byte>*>(context);
        const auto* bytes = static_cast<const std::byte*>(data);
        sink->insert(sink->end(), bytes, bytes + size);
    };
    const int ok = stbi_write_png_to_func(append, &out, width, height, 4, rgba.data(), width * 4);
    require(ok != 0, "stbi_write_png_to_func failed", __LINE__);
    return out;
}

// Serves a generated PNG for any texture the bake asks for, so the bake runs
// without a resource pack. Sizes follow the few shapes the baker requires: the
// colour maps are indexed as 256x256 lookup tables, entity sheets are 64x64, and
// everything else is a 16x16 block sprite. Non-PNG requests (the `.mcmeta`
// animation sidecars) are absent, so no texture animates.
class SyntheticPack final : public mc::assets::ResourceProvider {
  public:
    [[nodiscard]] std::filesystem::path locate(const mc::assets::ResourceLocation&) const override {
        return {};
    }

    [[nodiscard]] bool exists(const mc::assets::ResourceLocation& location) const override {
        return location.path.ends_with(".png");
    }

    [[nodiscard]] std::vector<std::byte> readBytes(
        const mc::assets::ResourceLocation& location) const override {
        if (!exists(location)) {
            return {};
        }
        const std::string_view path{location.path};
        if (path.starts_with("textures/colormap/")) {
            return greyPng(256, 256);
        }
        if (path.starts_with("textures/entity/")) {
            return greyPng(64, 64);
        }
        return greyPng(16, 16);
    }

    [[nodiscard]] std::vector<mc::assets::ResourceLocation> list(
        std::string_view, std::string_view, mc::assets::PackType) const override {
        return {};
    }

    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
};

[[nodiscard]] std::string_view tintKindName(BiomeTintKind kind) {
    switch (kind) {
    case BiomeTintKind::Grass:
        return "Grass";
    case BiomeTintKind::Foliage:
        return "Foliage";
    case BiomeTintKind::Water:
        return "Water";
    case BiomeTintKind::None:
        break;
    }
    return "None";
}

// The join: no face that takes a vertex tint may sample a pre-tinted layer.
void checkNoBlockIsTintedTwice() {
    const SyntheticPack pack;
    const auto atlas = mc::render::bakeBlockAtlas(pack);
    const std::set<std::uint32_t> preTinted{atlas.preTintedLayers.begin(),
                                            atlas.preTintedLayers.end()};
    REQUIRE(!preTinted.empty(),
            "the bake reported no pre-tinted layers at all — the item/GUI copies of grass and "
            "leaves are tinted, so an empty set means the bookkeeping stopped being maintained "
            "and this guard has quietly stopped guarding anything");

    constexpr std::array kFaces{Face::PositiveX, Face::NegativeX, Face::PositiveY,
                                Face::NegativeY, Face::PositiveZ, Face::NegativeZ};
    int tintedFaces = 0;
    for (int index = 0; index < static_cast<int>(Block::Count); ++index) {
        const auto block = static_cast<Block>(index);
        if (block == Block::Air) {
            continue;
        }
        for (const auto face : kFaces) {
            const auto kind = mc::world::biomeTintKind(block, face);
            if (kind == BiomeTintKind::None) {
                continue;
            }
            ++tintedFaces;
            const auto layer = static_cast<std::uint32_t>(mc::world::terrainAtlasLayer(block, face));
            REQUIRE(!preTinted.contains(layer),
                    std::string{"block "} + std::string{mc::world::blockDefinition(block).displayName} +
                        " takes the " + std::string{tintKindName(kind)} +
                        " biome tint but samples atlas layer " + std::to_string(layer) +
                        ", which the bake already multiplied by a colour. That tints it twice and "
                        "reads as the whole surface being too dark. Give it an UNTINTED terrain "
                        "layer (see setTerrainGrassLayers / setTerrainLeafLayer), or drop its "
                        "vertex tint if vanilla bakes a constant for it the way spruce and birch "
                        "leaves do.");
        }
    }
    REQUIRE(tintedFaces > 0,
            "no face reports a biome tint at all — tintKindFor has been emptied out, and every "
            "grass block, leaf and plant is rendering as its raw greyscale texture");
}

// The other direction: the blocks 26.1's BlockColors registers a biome resolver
// for must actually get one here. This is the check that would have caught the
// plants rendering white (fixed in d3d96b8) before a Mac ever saw it.
void checkVanillaTintedBlocksAreCovered() {
    struct Expectation final {
        Block block;
        BiomeTintKind kind;
        Face face;
    };
    // Straight from BlockColors.createDefault() in 26.1, restricted to the
    // blocks this roster actually has.
    constexpr std::array kExpected{
        Expectation{Block::Grass, BiomeTintKind::Grass, Face::PositiveY},
        Expectation{Block::GrassPlant, BiomeTintKind::Grass, Face::PositiveX},
        Expectation{Block::TallGrass, BiomeTintKind::Grass, Face::PositiveX},
        Expectation{Block::Fern, BiomeTintKind::Grass, Face::PositiveX},
        Expectation{Block::LargeFern, BiomeTintKind::Grass, Face::PositiveX},
        Expectation{Block::SugarCane, BiomeTintKind::Grass, Face::PositiveX},
        Expectation{Block::OakLeaves, BiomeTintKind::Foliage, Face::PositiveY},
        Expectation{Block::JungleLeaves, BiomeTintKind::Foliage, Face::PositiveY},
        Expectation{Block::AcaciaLeaves, BiomeTintKind::Foliage, Face::PositiveY},
        Expectation{Block::DarkOakLeaves, BiomeTintKind::Foliage, Face::PositiveY},
        Expectation{Block::MangroveLeaves, BiomeTintKind::Foliage, Face::PositiveY},
        Expectation{Block::Water, BiomeTintKind::Water, Face::PositiveY},
        // constant(-10380959) / constant(-8345771): baked, not a biome lookup.
        Expectation{Block::SpruceLeaves, BiomeTintKind::None, Face::PositiveY},
        Expectation{Block::BirchLeaves, BiomeTintKind::None, Face::PositiveY},
        // The grass block's sides carry their tint on the overlay quad, so the
        // base face itself is untinted — tinting it would turn the dirt green.
        Expectation{Block::Grass, BiomeTintKind::None, Face::PositiveX},
    };
    for (const auto& expected : kExpected) {
        const auto actual = mc::world::biomeTintKind(expected.block, expected.face);
        REQUIRE(actual == expected.kind,
                std::string{"block "} + std::string{mc::world::blockDefinition(expected.block).displayName} +
                    " should read the " + std::string{tintKindName(expected.kind)} +
                    " resolver (26.1 BlockColors.createDefault) but reads " +
                    std::string{tintKindName(actual)});
    }
}

} // namespace

int main() {
    checkVanillaTintedBlocksAreCovered();
    checkNoBlockIsTintedTwice();
    return 0;
}
