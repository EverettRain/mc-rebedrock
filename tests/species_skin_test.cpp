#include "animation/SkeletalModel.hpp"
#include "assets/ResourceProvider.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"

// 实现体在 src/assets/StbImageImplementation.cpp（RN-15b 起共用），这里只要声明
#include "stb_image_write.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// ReBedrock ships no Mojang assets. Entity skins therefore have exactly two
// sources — the standard resource pack, or the procedural placeholder — and
// this pins that there is no third.
//
// There used to be one: a bundled PNG under `resources/entity/<path>`, whose
// only inhabitant was converted from Mojang's zombie skin. A release built from
// that tree carried derived vanilla art while claiming not to. The guard below
// plants a decoy exactly where that fallback used to look; if anyone
// reintroduces it, the decoy's pixels show up in the skin and this fails.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"species_skin_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

// A magenta no pack and no generator would ever produce, so finding it in the
// output means it came from a file that should not have been read.
constexpr std::uint8_t kDecoyR = 251U;
constexpr std::uint8_t kDecoyG = 3U;
constexpr std::uint8_t kDecoyB = 247U;

void writeSolidPng(const std::filesystem::path& path, int width, int height, std::uint8_t red,
                   std::uint8_t green, std::uint8_t blue) {
    std::filesystem::create_directories(path.parent_path());
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t i = 0; i < pixels.size(); i += 4U) {
        pixels[i] = red;
        pixels[i + 1] = green;
        pixels[i + 2] = blue;
        pixels[i + 3] = 255U;
    }
    REQUIRE(stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), width * 4) != 0);
}

// A model with a declared 64x64 texture space and one cube, which is all
// buildSpeciesSkin needs to size the skin and paint face rects.
[[nodiscard]] mc::animation::SkeletalModel testModel() {
    const std::string json = R"({
        "format_version": "1.12.0",
        "minecraft:geometry": [{
            "description": {
                "identifier": "geometry.test",
                "texture_width": 64, "texture_height": 64,
                "visible_bounds_width": 2, "visible_bounds_height": 2
            },
            "bones": [{
                "name": "body",
                "pivot": [0, 0, 0],
                "cubes": [{"origin": [-4, 0, -2], "size": [8, 12, 4], "uv": [16, 16]}]
            }]
        }]
    })";
    return mc::animation::SkeletalModel::parse(json);
}

[[nodiscard]] bool containsColor(const std::vector<std::uint8_t>& rgba, std::uint8_t red,
                                 std::uint8_t green, std::uint8_t blue) {
    for (std::size_t i = 0; i + 3U < rgba.size(); i += 4U) {
        if (rgba[i] == red && rgba[i + 1] == green && rgba[i + 2] == blue && rgba[i + 3] == 255U) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace mc;

    const fs::path root =
        fs::temp_directory_path() / ("species_skin_test_" + std::to_string(std::rand()));
    fs::remove_all(root);
    const fs::path packRoot = root / "pack";
    const fs::path bundledRoot = root / "resources";
    fs::create_directories(packRoot);
    fs::create_directories(bundledRoot);

    const auto model = testModel();
    REQUIRE(model.textureWidth() == 64);
    const std::string texturePath = "entity/zombie/zombie.png";

    // Plant the decoy where the removed bundled fallback used to look. The
    // provider is rooted at `bundledRoot`, so a reintroduced
    // `resourceRoot / texturePath` read would find this file.
    writeSolidPng(bundledRoot / texturePath, 64, 64, kDecoyR, kDecoyG, kDecoyB);

    // --- No pack ships the skin: the result is the procedural placeholder, and
    // the decoy is nowhere in it. ---
    {
        assets::StandardPackResourceProvider emptyPack{packRoot};
        assets::DirectoryResourceProvider bundled{bundledRoot};
        const assets::LayeredResourceProvider resources{bundled, {&emptyPack}};

        const auto skin = gameplay::entities::buildSpeciesSkin(resources, model, texturePath, {64.0F, 64.0F});
        REQUIRE(skin.size() == 64U * 64U * 4U);
        REQUIRE(!containsColor(skin, kDecoyR, kDecoyG, kDecoyB));
        // The placeholder actually paints: a model with a cube must produce
        // opaque texels, not an empty buffer that would render invisible.
        bool anyOpaque = false;
        for (std::size_t i = 3U; i < skin.size(); i += 4U) {
            anyOpaque = anyOpaque || skin[i] == 255U;
        }
        REQUIRE(anyOpaque);
    }

    // --- A pack that does ship the skin is used, which is the only supported
    // way to get real entity art into the game. ---
    {
        constexpr std::uint8_t packR = 12U;
        constexpr std::uint8_t packG = 200U;
        constexpr std::uint8_t packB = 64U;
        writeSolidPng(packRoot / "assets" / "minecraft" / "textures" / texturePath, 64, 64, packR,
                      packG, packB);
        assets::StandardPackResourceProvider pack{packRoot};
        assets::DirectoryResourceProvider bundled{bundledRoot};
        const assets::LayeredResourceProvider resources{bundled, {&pack}};

        const auto skin = gameplay::entities::buildSpeciesSkin(resources, model, texturePath, {64.0F, 64.0F});
        REQUIRE(containsColor(skin, packR, packG, packB));
        REQUIRE(!containsColor(skin, kDecoyR, kDecoyG, kDecoyB));
    }

    fs::remove_all(root);
    return 0;
}
