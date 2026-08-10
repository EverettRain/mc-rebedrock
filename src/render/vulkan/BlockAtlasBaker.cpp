#include "render/vulkan/BlockAtlasBaker.hpp"

#include "render/vulkan/BlockAtlasLayout.hpp"

#include "assets/ImageData.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/SpawnEggItems.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/gen/Biome.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mc::render {

namespace {

void requireSameSize(const assets::ImageData& first, const assets::ImageData& other) {
    if (first.width != other.width || first.height != other.height) {
        throw std::runtime_error("Grass block texture layers must have identical dimensions");
    }
}

[[nodiscard]] std::vector<assets::ImageData> animatedSquareFrames(const assets::ImageData& image,
                                                                  int targetSize) {
    if (image.width <= 0 || image.height < image.width || image.height % image.width != 0) {
        throw std::runtime_error("Animated block texture has invalid frame dimensions");
    }
    const int frameCount = image.height / image.width;
    std::vector<assets::ImageData> frames;
    frames.reserve(static_cast<std::size_t>(frameCount));
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        assets::ImageData frame;
        frame.width = targetSize;
        frame.height = targetSize;
        frame.rgba.resize(static_cast<std::size_t>(targetSize * targetSize * 4));
        for (int y = 0; y < targetSize; ++y) {
            const int sourceY = frameIndex * image.width + y * image.width / targetSize;
            for (int x = 0; x < targetSize; ++x) {
                const int sourceX = x * image.width / targetSize;
                const std::size_t source =
                    static_cast<std::size_t>((sourceY * image.width + sourceX) * 4);
                const std::size_t destination = static_cast<std::size_t>((y * targetSize + x) * 4);
                std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4,
                            frame.rgba.begin() + static_cast<std::ptrdiff_t>(destination));
            }
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

[[nodiscard]] assets::ImageData resizedRegion(const assets::ImageData& image, int sourceX,
                                              int sourceY, int sourceWidth, int sourceHeight,
                                              int targetSize) {
    assets::ImageData result;
    result.width = targetSize;
    result.height = targetSize;
    result.rgba.resize(static_cast<std::size_t>(targetSize * targetSize * 4));
    for (int y = 0; y < targetSize; ++y) {
        for (int x = 0; x < targetSize; ++x) {
            const int sx = sourceX + x * sourceWidth / targetSize;
            const int sy = sourceY + y * sourceHeight / targetSize;
            const std::size_t source = static_cast<std::size_t>((sy * image.width + sx) * 4);
            const std::size_t target = static_cast<std::size_t>((y * targetSize + x) * 4);
            std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4,
                        result.rgba.begin() + static_cast<std::ptrdiff_t>(target));
        }
    }
    return result;
}

using PlayerSkinFaces = std::array<assets::ImageData, 6>;

[[nodiscard]] PlayerSkinFaces playerSkinCuboidFaces(const assets::ImageData& skin, int textureX,
                                                    int textureY, int width, int height, int depth,
                                                    int targetSize) {
    // Minecraft's cuboid unwrap is top/bottom in the first row, followed by
    // right/front/left/back.  Store faces in the exact order emitted by
    // item_entity.vert: +X, -X, +Y, -Y, +Z, -Z.
    return {
        resizedRegion(skin, textureX + depth + width, textureY + depth, depth, height, targetSize),
        resizedRegion(skin, textureX, textureY + depth, depth, height, targetSize),
        resizedRegion(skin, textureX + depth, textureY, width, depth, targetSize),
        resizedRegion(skin, textureX + depth + width, textureY, width, depth, targetSize),
        resizedRegion(skin, textureX + depth, textureY + depth, width, height, targetSize),
        resizedRegion(skin, textureX + depth * 2 + width, textureY + depth, width, height,
                      targetSize),
    };
}

void overlayScaled(assets::ImageData& destination, const assets::ImageData& source,
                   int destinationX, int destinationY, int destinationWidth,
                   int destinationHeight) {
    for (int y = 0; y < destinationHeight; ++y) {
        for (int x = 0; x < destinationWidth; ++x) {
            const int sourceX = x * source.width / destinationWidth;
            const int sourceY = y * source.height / destinationHeight;
            const std::size_t sourceIndex =
                static_cast<std::size_t>((sourceY * source.width + sourceX) * 4);
            const std::size_t destinationIndex = static_cast<std::size_t>(
                ((destinationY + y) * destination.width + destinationX + x) * 4);
            const float alpha = static_cast<float>(source.rgba[sourceIndex + 3U]) / 255.0F;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const float blended =
                    static_cast<float>(source.rgba[sourceIndex + channel]) * alpha +
                    static_cast<float>(destination.rgba[destinationIndex + channel]) *
                        (1.0F - alpha);
                destination.rgba[destinationIndex + channel] =
                    static_cast<std::uint8_t>(std::lround(blended));
            }
            destination.rgba[destinationIndex + 3U] = 255U;
        }
    }
}

[[nodiscard]] assets::ImageData stackedChestFace(const assets::ImageData& lid,
                                                 const assets::ImageData& base) {
    assets::ImageData result = base;
    for (int y = 0; y < result.height; ++y) {
        const int sourceRow = y * 15 / result.height;
        const bool lidRow = sourceRow < 5;
        const auto& source = lidRow ? lid : base;
        const int partRow = lidRow ? sourceRow : sourceRow - 5;
        const int partHeight = lidRow ? 5 : 10;
        const int sourceY = partRow * source.height / partHeight;
        for (int x = 0; x < result.width; ++x) {
            const std::size_t sourceIndex =
                static_cast<std::size_t>((sourceY * source.width + x) * 4);
            const std::size_t destinationIndex =
                static_cast<std::size_t>((y * result.width + x) * 4);
            std::copy_n(source.rgba.begin() + static_cast<std::ptrdiff_t>(sourceIndex), 4,
                        result.rgba.begin() + static_cast<std::ptrdiff_t>(destinationIndex));
        }
    }
    return result;
}

[[nodiscard]] std::uint8_t tintedChannel(std::uint8_t source, float tint) {
    const auto value = static_cast<int>(std::lround(static_cast<float>(source) * tint));
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// Spawn-egg icon (1.16.1): `spawn_egg.png` paints only part of the egg's
// silhouette while `spawn_egg_overlay.png` supplies the spots that fill the
// rest; the two are tinted with the species' SpawnEggColors and blended by the
// overlay's alpha into one 16×16 layer. Pixels survive whenever EITHER texture
// is opaque: skipping every pixel with a transparent base would drop the spots
// (they sit on transparent base pixels) and leave only the shell colour.
[[nodiscard]] assets::ImageData buildSpawnEggIcon(const std::filesystem::path& itemDir,
                                                  gameplay::entities::SpawnEggColors eggColors) {
    assets::ImageData egg = assets::ImageData::loadRgba(itemDir / "spawn_egg.png");
    const auto eggOverlay = assets::ImageData::loadRgba(itemDir / "spawn_egg_overlay.png");
    requireSameSize(egg, eggOverlay);
    const auto unpack = [](std::uint32_t rgb) {
        return std::array<int, 3>{static_cast<int>((rgb >> 16) & 0xFFU),
                                  static_cast<int>((rgb >> 8) & 0xFFU),
                                  static_cast<int>(rgb & 0xFFU)};
    };
    const std::array<int, 3> primary = unpack(eggColors.primary);
    const std::array<int, 3> secondary = unpack(eggColors.secondary);
    for (std::size_t p = 0; p + 3U < egg.rgba.size(); p += 4U) {
        const float baseAlpha = static_cast<float>(egg.rgba[p + 3U]) / 255.0F;
        const float overlayAlpha = static_cast<float>(eggOverlay.rgba[p + 3U]) / 255.0F;
        if (baseAlpha == 0.0F && overlayAlpha == 0.0F) {
            continue; // outside the egg shape entirely
        }
        std::array<std::uint8_t, 3> baseColor{};
        for (std::size_t c = 0; c < 3U; ++c) {
            if (baseAlpha > 0.0F) {
                baseColor[c] = static_cast<std::uint8_t>(egg.rgba[p + c] * primary[c] / 255);
            }
        }
        // Spot-only pixels would inherit the transparent base's alpha and
        // vanish, so the output is opaque wherever either source has ink.
        egg.rgba[p + 3U] = 255U;
        for (std::size_t c = 0; c < 3U; ++c) {
            const float spot = static_cast<float>(eggOverlay.rgba[p + c] * secondary[c] / 255);
            const float base = static_cast<float>(baseColor[c]);
            egg.rgba[p + c] =
                static_cast<std::uint8_t>(spot * overlayAlpha + base * (1.0F - overlayAlpha));
        }
    }
    return egg;
}

}  // namespace

TextureArrayPixels bakeBlockAtlas(const std::filesystem::path& root) {
    auto top = assets::ImageData::loadRgba(root / "grass_block_top.png");
    auto side = assets::ImageData::loadRgba(root / "grass_block_side.png");
    const auto overlay = assets::ImageData::loadRgba(root / "grass_block_side_overlay.png");
    const auto dirt = assets::ImageData::loadRgba(root / "dirt.png");
    auto grassPlant = assets::ImageData::loadRgba(root / "grass.png");
    auto oakLeaves = assets::ImageData::loadRgba(root / "oak_leaves.png");
    // The animated and entity frames that fill the fixed special section. Item
    // icons no longer live here; they append after the block textures.
    auto waterStillFrames =
        animatedSquareFrames(assets::ImageData::loadRgba(root / "water_still.png"), top.width);
    auto waterFlowFrames =
        animatedSquareFrames(assets::ImageData::loadRgba(root / "water_flow.png"), top.width);
    auto lavaStillFrames =
        animatedSquareFrames(assets::ImageData::loadRgba(root / "lava_still.png"), top.width);
    auto lavaFlowFrames =
        animatedSquareFrames(assets::ImageData::loadRgba(root / "lava_flow.png"), top.width);
    auto sunFrames = animatedSquareFrames(
        assets::ImageData::loadRgba(root.parent_path() / "environment" / "sun.png"), top.width);
    const auto moonPhasesImage =
        assets::ImageData::loadRgba(root.parent_path() / "environment" / "moon_phases.png");
    std::array<assets::ImageData, 8> moonPhaseTiles;
    for (int phase = 0; phase < 8; ++phase) {
        const int column = phase % 4;
        const int row = phase / 4;
        moonPhaseTiles[static_cast<std::size_t>(phase)] = resizedRegion(
            moonPhasesImage, column * moonPhasesImage.width / 4, row * moonPhasesImage.height / 2,
            moonPhasesImage.width / 4, moonPhasesImage.height / 2, top.width);
    }
    const auto playerSkin =
        assets::ImageData::loadRgba(root.parent_path() / "entity" / "steve.png");
    const std::array playerParts{
        playerSkinCuboidFaces(playerSkin, 0, 0, 8, 8, 8, top.width),
        playerSkinCuboidFaces(playerSkin, 16, 16, 8, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 40, 16, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 32, 48, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 0, 16, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 16, 48, 4, 12, 4, top.width),
    };
    const auto chestTexture =
        assets::ImageData::loadRgba(root.parent_path() / "entity" / "chest" / "normal.png");
    const auto furnaceFront = assets::ImageData::loadRgba(root / "furnace_front.png");
    const auto furnaceFrontOn = assets::ImageData::loadRgba(root / "furnace_front_on.png");
    auto chestParts = std::array{
        playerSkinCuboidFaces(chestTexture, 0, 19, 14, 10, 14, top.width),
        playerSkinCuboidFaces(chestTexture, 0, 0, 14, 5, 14, top.width),
        playerSkinCuboidFaces(chestTexture, 0, 0, 2, 4, 1, top.width),
    };
    std::swap(chestParts[1][2], chestParts[1][3]);
    const auto latchUpper = resizedRegion(chestTexture, 1, 1, 2, 2, top.width);
    const auto latchLower = resizedRegion(chestTexture, 1, 3, 2, 2, top.width);
    overlayScaled(chestParts[1][4], latchUpper, 7, 10, 2, 6);
    overlayScaled(chestParts[0][4], latchLower, 7, 0, 2, 3);
    const std::array chestItemTextures{
        chestParts[1][2],
        stackedChestFace(chestParts[1][4], chestParts[0][4]),
        stackedChestFace(chestParts[1][0], chestParts[0][0]),
    };
    std::array<assets::ImageData, 10> destroyStages;
    for (std::size_t stage = 0; stage < destroyStages.size(); ++stage) {
        destroyStages[stage] =
            assets::ImageData::loadRgba(root / ("destroy_stage_" + std::to_string(stage) + ".png"));
    }
    // The biome leaves and their tints (spruce/birch fixed, the rest the biome
    // foliage colour), the same set the tree shapes grow.
    constexpr std::array<float, 3> foliageTint{0.49F, 0.74F, 0.32F};
    constexpr std::array<float, 3> spruceTint{0x61 / 255.0F, 0x99 / 255.0F, 0x61 / 255.0F};
    constexpr std::array<float, 3> birchTint{0x80 / 255.0F, 0xA7 / 255.0F, 0x55 / 255.0F};
    std::array<assets::ImageData, 5> biomeLeafTextures{
        assets::ImageData::loadRgba(root / "spruce_leaves.png"),
        assets::ImageData::loadRgba(root / "birch_leaves.png"),
        assets::ImageData::loadRgba(root / "jungle_leaves.png"),
        assets::ImageData::loadRgba(root / "acacia_leaves.png"),
        assets::ImageData::loadRgba(root / "dark_oak_leaves.png"),
    };
    const std::array<std::array<float, 3>, 5> biomeLeafTints{spruceTint, birchTint, foliageTint,
                                                             foliageTint, foliageTint};
    // Untinted leaf bases for the terrain (per-block biome tint happens per
    // vertex in the mesher), while the tinted ones above stay for items/GUI.
    const auto biomeLeafTexturesRaw = biomeLeafTextures;
    for (std::size_t leaf = 0; leaf < biomeLeafTextures.size(); ++leaf) {
        auto& pixels = biomeLeafTextures[leaf].rgba;
        for (std::size_t index = 0; index + 3U < pixels.size(); index += 4U) {
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                pixels[index + channel] =
                    tintedChannel(pixels[index + channel], biomeLeafTints[leaf][channel]);
            }
        }
    }
    if (waterStillFrames.size() != kWaterAnimationFrameCount ||
        waterFlowFrames.size() != kWaterAnimationFrameCount) {
        throw std::runtime_error("Minecraft water textures must contain 32 animation frames");
    }
    if (lavaStillFrames.size() != kLavaStillFrameCount ||
        lavaFlowFrames.size() != kLavaFlowFrameCount) {
        throw std::runtime_error("Minecraft lava textures must contain 20/16 animation frames");
    }
    if (sunFrames.size() != 1U) {
        throw std::runtime_error("Minecraft sun texture must contain one square frame");
    }
    // Untinted grass family, kept for the per-biome colour variants below.
    const auto grassTopRaw = top;
    const auto grassSideBase = side;
    const auto grassOverlay = overlay;
    const auto grassPlantRaw = grassPlant;
    const auto leavesRaw = oakLeaves;
    // Tint the foliage and grass the vanilla colours before they enter the atlas.
    const auto tintInPlace = [](assets::ImageData& image, const std::array<float, 3>& tint) {
        for (std::size_t index = 0; index + 3U < image.rgba.size(); index += 4U) {
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                image.rgba[index + channel] =
                    tintedChannel(image.rgba[index + channel], tint[channel]);
            }
        }
    };
    tintInPlace(top, foliageTint);
    for (std::size_t index = 0; index + 3U < side.rgba.size(); index += 4U) {
        const float alpha = static_cast<float>(overlay.rgba[index + 3U]) / 255.0F;
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            const auto overlayColor =
                tintedChannel(overlay.rgba[index + channel], foliageTint[channel]);
            const float blended = static_cast<float>(side.rgba[index + channel]) * (1.0F - alpha) +
                                  static_cast<float>(overlayColor) * alpha;
            side.rgba[index + channel] = static_cast<std::uint8_t>(
                std::clamp(static_cast<int>(std::lround(blended)), 0, 255));
        }
        side.rgba[index + 3U] = 255U;
    }
    tintInPlace(grassPlant, foliageTint);
    tintInPlace(oakLeaves, foliageTint);
    const auto tintWaterFrames = [&](std::vector<assets::ImageData>& frames) {
        constexpr std::array<float, 3> waterTint{0.25F, 0.48F, 0.92F};
        for (auto& frame : frames) {
            for (std::size_t index = 0; index + 3U < frame.rgba.size(); index += 4U) {
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    frame.rgba[index + channel] =
                        tintedChannel(frame.rgba[index + channel], waterTint[channel]);
                }
                frame.rgba[index + 3U] = 155U;
            }
        }
    };
    tintWaterFrames(waterStillFrames);
    tintWaterFrames(waterFlowFrames);

    // ---- Fixed special section, in a deterministic order ----
    std::vector<assets::ImageData> layers;
    const auto append = [&](const assets::ImageData& image) {
        requireSameSize(top, image);
        layers.push_back(image);
    };
    for (const auto& frame : waterStillFrames)
        append(frame); // 0..31
    for (const auto& frame : waterFlowFrames)
        append(frame); // 32..63
    for (const auto& frame : lavaStillFrames)
        append(frame); // 64..83
    for (const auto& frame : lavaFlowFrames)
        append(frame); // 84..99
    for (const auto& part : playerParts) {
        for (const auto& face : part)
            append(face); // 100..135
    }
    for (const auto& stage : destroyStages)
        append(stage); // 136..145
    for (const auto& part : chestParts) {
        for (const auto& face : part)
            append(face); // 146..163
    }
    for (const auto& texture : chestItemTextures)
        append(texture);    // 164..166
    append(furnaceFront);   // 167
    append(furnaceFrontOn); // 168
    for (const auto& tile : moonPhaseTiles)
        append(tile);          // 169..176
    append(sunFrames.front()); // 177

    // ---- Dynamic block textures, name-driven from the block registry ----
    // Baked composites register by name so every block that reuses them finds
    // the same layer (grass_block_side, dirt, the tinted leaves, ...).
    if (layers.size() != kFirstBlockTextureLayer) {
        throw std::runtime_error("Fixed texture section does not match kFirstBlockTextureLayer");
    }
    std::unordered_map<std::string, float> layerByName;
    const auto assign = [&](const char* name) -> float {
        const auto existing = layerByName.find(name);
        if (existing != layerByName.end()) {
            return existing->second;
        }
        // The crop stages are a contiguous run from their stage-0 layer, so the
        // mesher can read stage0 + age.
        const std::string_view view{name};
        auto stageFor = [&](std::string_view prefix, int count) -> float {
            if (!view.starts_with(prefix)) {
                return -1.0F;
            }
            const float first = static_cast<float>(layers.size());
            for (int stage = 0; stage < count; ++stage) {
                const std::string file{prefix};
                const std::string image = file.substr(0, file.size() - 1) + std::to_string(stage);
                assets::ImageData pixels = assets::ImageData::loadRgba(root / (image + ".png"));
                requireSameSize(top, pixels);
                layers.push_back(std::move(pixels));
            }
            return first;
        };
        if (const float wheat = stageFor("wheat_stage0", 8); wheat >= 0.0F) {
            layerByName.emplace(name, wheat);
            return wheat;
        }
        if (const float carrot = stageFor("carrots_stage0", 4); carrot >= 0.0F) {
            layerByName.emplace(name, carrot);
            return carrot;
        }
        if (const float potato = stageFor("potatoes_stage0", 4); potato >= 0.0F) {
            layerByName.emplace(name, potato);
            return potato;
        }
        // Farmland's moist variant sits right after its dry face.
        if (view == "farmland") {
            const float first = static_cast<float>(layers.size());
            for (const char* file : {"farmland", "farmland_moist"}) {
                assets::ImageData pixels =
                    assets::ImageData::loadRgba(root / (std::string(file) + ".png"));
                requireSameSize(top, pixels);
                layers.push_back(std::move(pixels));
            }
            layerByName.emplace(name, first);
            return first;
        }
        assets::ImageData pixels = assets::ImageData::loadRgba(root / (std::string(name) + ".png"));
        requireSameSize(top, pixels);
        const float index = static_cast<float>(layers.size());
        layers.push_back(std::move(pixels));
        layerByName.emplace(name, index);
        return index;
    };
    // The baked composites register first so reuses share their layer.
    layerByName.emplace("grass_block_top", static_cast<float>(layers.size()));
    layers.push_back(top);
    layerByName.emplace("grass_block_side", static_cast<float>(layers.size()));
    layers.push_back(side);
    layerByName.emplace("dirt", static_cast<float>(layers.size()));
    layers.push_back(dirt);
    layerByName.emplace("grass", static_cast<float>(layers.size()));
    layers.push_back(grassPlant);
    layerByName.emplace("oak_leaves", static_cast<float>(layers.size()));
    layers.push_back(oakLeaves);
    const std::array<const char*, 5> biomeLeafNames{
        "spruce_leaves", "birch_leaves", "jungle_leaves", "acacia_leaves", "dark_oak_leaves"};
    for (std::size_t leaf = 0; leaf < biomeLeafNames.size(); ++leaf) {
        layerByName.emplace(biomeLeafNames[leaf], static_cast<float>(layers.size()));
        layers.push_back(biomeLeafTextures[leaf]);
    }

    // ---- Per-biome grass/foliage colours (1.16.1 BiomeColors) ----
    // The vanilla grass and foliage colour maps are 256x256 lookups indexed by
    // temperature and rainfall. Each biome's grass and foliage colour comes
    // from its own map; the mesher blends them bilinearly per block (the way
    // 1.16.1's BlockView.getColor does) so a biome boundary reads as a smooth
    // colour gradient instead of a hard switch. Swamp and dark forest carry
    // their 1.16.1 overrides below.
    const auto loadColormap = [&](const char* name) {
        return assets::ImageData::loadRgba(root.parent_path() / "colormap" / name);
    };
    const auto grassColormap = loadColormap("grass.png");
    const auto foliageColormap = loadColormap("foliage.png");
    const auto colormapColor = [](const assets::ImageData& colormap, float temperature,
                                  float downfall) -> std::uint32_t {
        const float clampedTemperature = std::clamp(temperature, 0.0F, 1.0F);
        const float humidity = std::clamp(downfall, 0.0F, 1.0F) * clampedTemperature;
        const int xIndex = static_cast<int>((1.0 - clampedTemperature) * 255.0);
        const int yIndex = static_cast<int>((1.0 - humidity) * 255.0);
        const std::size_t pixel = static_cast<std::size_t>(yIndex * 256 + xIndex) * 4U;
        if (pixel + 3U >= colormap.rgba.size()) {
            return 0x00FF00U;
        }
        return (static_cast<std::uint32_t>(colormap.rgba[pixel]) << 16U) |
               (static_cast<std::uint32_t>(colormap.rgba[pixel + 1U]) << 8U) |
               static_cast<std::uint32_t>(colormap.rgba[pixel + 2U]);
    };
    const auto colorTint = [](std::uint32_t color) -> std::array<float, 3> {
        return {
            static_cast<float>((color >> 16U) & 0xFFU) / 255.0F,
            static_cast<float>((color >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>(color & 0xFFU) / 255.0F,
        };
    };
    // The terrain grass family and oak-family leaves render the UNTINTED
    // textures and take their colour from the fragment shader's biome-colour
    // lookup (a linear-filtered texture sample, so the biome boundary blends as
    // a smooth per-pixel gradient). The grass SIDE keeps its baked per-biome
    // layer so the dirt under a cliff stays dirt, and spruce/birch leaves keep
    // their fixed 1.16.1 tones.
    const float terrainGrassTop = static_cast<float>(layers.size());
    layers.push_back(grassTopRaw);
    layerByName.emplace("grass_block_top:terrain", terrainGrassTop);
    const float terrainGrassPlant = static_cast<float>(layers.size());
    layers.push_back(grassPlantRaw);
    layerByName.emplace("grass:terrain", terrainGrassPlant);
    world::gen::setTerrainGrassLayers(terrainGrassTop, terrainGrassPlant);
    const std::array<world::Block, 6> leafBlocks{
        world::Block::OakLeaves,    world::Block::SpruceLeaves, world::Block::BirchLeaves,
        world::Block::JungleLeaves, world::Block::AcaciaLeaves, world::Block::DarkOakLeaves,
    };
    const std::array<const assets::ImageData*, 6> leafTextures{
        &leavesRaw,
        &biomeLeafTexturesRaw[0],
        &biomeLeafTexturesRaw[1],
        &biomeLeafTexturesRaw[2],
        &biomeLeafTexturesRaw[3],
        &biomeLeafTexturesRaw[4],
    };
    const std::array<const char*, 6> leafNames{"oak",    "spruce", "birch",
                                               "jungle", "acacia", "dark_oak"};
    const std::array<std::uint32_t, 6> leafFixedTints{0U, 0x619961U, 0x80A755U, 0U, 0U, 0U};
    for (std::size_t leaf = 0; leaf < leafBlocks.size(); ++leaf) {
        auto pixels = *leafTextures[leaf];
        if (leafFixedTints[leaf] != 0U) {
            const auto tint = colorTint(leafFixedTints[leaf]);
            for (std::size_t index = 0; index + 3U < pixels.rgba.size(); index += 4U) {
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    pixels.rgba[index + channel] =
                        tintedChannel(pixels.rgba[index + channel], tint[channel]);
                }
            }
        }
        const float layer = static_cast<float>(layers.size());
        layers.push_back(pixels);
        layerByName.emplace(std::string("leaves:terrain:") + leafNames[leaf], layer);
        world::gen::setTerrainLeafLayer(leafBlocks[leaf], layer);
    }
    // Baked per-biome grass family: the top/side/plant are tinted with the
    // biome's grass colour at atlas build time, so the rendered colour never
    // depends on per-vertex data reaching the fragment shader.
    const auto buildBiomeGrass = [&](std::string_view suffix, std::uint32_t color) {
        auto biomeTop = grassTopRaw;
        auto biomeSide = grassSideBase;
        auto biomePlant = grassPlantRaw;
        const auto tint = colorTint(color);
        for (std::size_t index = 0; index + 3U < biomeTop.rgba.size(); index += 4U) {
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                biomeTop.rgba[index + channel] =
                    tintedChannel(biomeTop.rgba[index + channel], tint[channel]);
                biomePlant.rgba[index + channel] =
                    tintedChannel(biomePlant.rgba[index + channel], tint[channel]);
            }
        }
        for (std::size_t index = 0; index + 3U < biomeSide.rgba.size(); index += 4U) {
            const float alpha = static_cast<float>(grassOverlay.rgba[index + 3U]) / 255.0F;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const auto overlayColor =
                    tintedChannel(grassOverlay.rgba[index + channel], tint[channel]);
                const float blended =
                    static_cast<float>(grassSideBase.rgba[index + channel]) * (1.0F - alpha) +
                    static_cast<float>(overlayColor) * alpha;
                biomeSide.rgba[index + channel] = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(std::lround(blended)), 0, 255));
            }
            biomeSide.rgba[index + 3U] = 255U;
        }
        const std::string prefix{suffix};
        const float topLayer = static_cast<float>(layers.size());
        layers.push_back(biomeTop);
        layerByName.emplace("grass_block_top:" + prefix, topLayer);
        const float sideLayer = static_cast<float>(layers.size());
        layers.push_back(biomeSide);
        layerByName.emplace("grass_block_side:" + prefix, sideLayer);
        const float plantLayer = static_cast<float>(layers.size());
        layers.push_back(biomePlant);
        layerByName.emplace("grass:" + prefix, plantLayer);
        return world::BlockTextureLayers{topLayer, sideLayer, plantLayer};
    };
    // Baked per-biome foliage layer for the oak family.
    const auto buildLeafLayer = [&](std::string_view suffix, const assets::ImageData& texture,
                                    std::uint32_t color) {
        auto pixels = texture;
        const auto tint = colorTint(color);
        for (std::size_t index = 0; index + 3U < pixels.rgba.size(); index += 4U) {
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                pixels.rgba[index + channel] =
                    tintedChannel(pixels.rgba[index + channel], tint[channel]);
            }
        }
        const float layer = static_cast<float>(layers.size());
        layers.push_back(pixels);
        layerByName.emplace(std::string("leaves:") + std::string{suffix}, layer);
        return layer;
    };
    for (int biomeIndex = 0; biomeIndex < static_cast<int>(world::gen::Biome::Count);
         ++biomeIndex) {
        const auto biome = static_cast<world::gen::Biome>(biomeIndex);
        const auto& definition = world::gen::biomeDefinition(biome);
        std::uint32_t grassColor =
            colormapColor(grassColormap, definition.temperature, definition.downfall);
        if (biome == world::gen::Biome::DarkForest) {
            // DarkForestBiome#getGrassColorAt darkens the colormap colour.
            grassColor = ((grassColor & 0xFEFEFEU) + 0x28340AU) >> 1U;
        }
        std::uint32_t foliageColor =
            colormapColor(foliageColormap, definition.temperature, definition.downfall);
        if (biome == world::gen::Biome::Swamp) {
            // SwampBiome#getFoliageColor is the fixed 0x6A7039.
            foliageColor = 0x6A7039U;
        }
        if (biome == world::gen::Biome::Swamp) {
            // SwampBiome#getGrassColorAt picks 0x6A7039 or 0x4C763C by noise;
            // the mesher chooses the per-block tone from FOLIAGE_NOISE.
            world::gen::setBiomeGrassLayers(biome, buildBiomeGrass("swamp", 0x6A7039U));
            world::gen::setSwampDarkGrassLayers(buildBiomeGrass("swamp_dark", 0x4C763CU));
        } else {
            world::gen::setBiomeGrassLayers(biome,
                                            buildBiomeGrass(definition.identifier, grassColor));
        }
        // Baked per-biome oak-family foliage; spruce/birch keep the fixed terrain
        // layers built above.
        const std::string prefix{definition.identifier};
        world::gen::setBiomeFoliageLayer(biome, world::Block::OakLeaves,
                                         buildLeafLayer(prefix + ":oak", leavesRaw, foliageColor));
        world::gen::setBiomeFoliageLayer(
            biome, world::Block::JungleLeaves,
            buildLeafLayer(prefix + ":jungle", biomeLeafTexturesRaw[2], foliageColor));
        world::gen::setBiomeFoliageLayer(
            biome, world::Block::AcaciaLeaves,
            buildLeafLayer(prefix + ":acacia", biomeLeafTexturesRaw[3], foliageColor));
        world::gen::setBiomeFoliageLayer(
            biome, world::Block::DarkOakLeaves,
            buildLeafLayer(prefix + ":dark_oak", biomeLeafTexturesRaw[4], foliageColor));
    }

    for (const auto& definition : world::kBlockRegistry) {
        const auto block = definition.block;
        if (block == world::Block::Air) {
            continue;
        }
        if (block == world::Block::Water) {
            world::setBlockTextureLayers(block, {static_cast<float>(kWaterStillLayer),
                                                 static_cast<float>(kWaterFlowLayer),
                                                 static_cast<float>(kWaterFlowLayer)});
            continue;
        }
        if (block == world::Block::Lava) {
            world::setBlockTextureLayers(block, {static_cast<float>(kLavaStillLayer),
                                                 static_cast<float>(kLavaFlowLayer),
                                                 static_cast<float>(kLavaFlowLayer)});
            continue;
        }
        if (block == world::Block::Chest) {
            // The dropped chest item draws the baked chest-item faces.
            world::setBlockTextureLayers(block, {static_cast<float>(kChestItemTopLayer),
                                                 static_cast<float>(kChestItemSideLayer),
                                                 static_cast<float>(kChestItemSideLayer)});
            continue;
        }
        world::BlockTextureLayers resolved;
        resolved.top = assign(definition.textures.top);
        resolved.side = assign(definition.textures.side);
        resolved.bottom = assign(definition.textures.bottom);
        world::setBlockTextureLayers(block, resolved);
    }

    TextureArrayPixels output;
    output.width = static_cast<std::uint32_t>(top.width);
    output.height = static_cast<std::uint32_t>(top.height);
    for (const auto& layer : layers) {
        output.rgba.insert(output.rgba.end(), layer.rgba.begin(), layer.rgba.end());
    }
    const std::uint32_t baseLayerCount = static_cast<std::uint32_t>(layers.size());
    // Item icons: one appended layer per registered item, in registry order.
    const auto itemDir = root.parent_path() / "item";
    std::uint32_t itemIndex = 0U;
    const auto appendItemIcon = [&](const gameplay::Item* item) {
        assets::ImageData icon;
        if (const auto* spawnEgg = gameplay::asSpawnEgg(item)) {
            icon = buildSpawnEggIcon(itemDir, spawnEgg->entityType().spawnEgg());
        } else {
            icon = assets::ImageData::loadRgba(itemDir / (std::string{item->textureName} + ".png"));
        }
        requireSameSize(top, icon);
        output.rgba.insert(output.rgba.end(), icon.rgba.begin(), icon.rgba.end());
        gameplay::setItemTextureLayer(item, static_cast<float>(baseLayerCount + itemIndex));
        ++itemIndex;
    };
    for (const gameplay::Item* item : gameplay::kItemRegistry)
        appendItemIcon(item);
    for (const gameplay::Item* item : gameplay::kSpawnEggItems)
        appendItemIcon(item);
    // Lava's animation frames live in the fixed section (64..99); nothing trails
    // the item icons, so the whole atlas is exactly the layers above.
    return output;
}

} // namespace mc::render
