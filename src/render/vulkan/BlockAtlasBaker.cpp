#include "render/vulkan/BlockAtlasBaker.hpp"

#include "render/vulkan/AtlasLayerFit.hpp"
#include "render/vulkan/BlockAtlasLayout.hpp"

#include "assets/ImageData.hpp"
#include "assets/TextureAnimation.hpp"
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
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::render {

namespace {

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

// Fits an animated texture's source frames to the fixed number of atlas layers
// reserved for it. The atlas layout and the animation shader are hardwired to a
// frame count (32 water, 20/16 lava), so a pack whose texture has a different
// count — or an explicit `.mcmeta` `frames` order — is reconciled here rather
// than crashing: the frames are reordered per the metadata, then cycled or
// truncated to fill exactly the reserved layers. The animation-wide `.mcmeta`
// frametime is forwarded separately to the shader by bakeBlockAtlas().
[[nodiscard]] std::vector<assets::ImageData>
fitAnimationFrames(std::vector<assets::ImageData> source,
                   const std::optional<assets::TextureAnimation>& animation,
                   std::uint32_t reserved, std::string_view name) {
    std::vector<assets::ImageData> ordered;
    if (animation.has_value() && !animation->frames.empty()) {
        for (const auto& frame : animation->frames) {
            if (frame.index >= 0 && static_cast<std::size_t>(frame.index) < source.size()) {
                ordered.push_back(source[static_cast<std::size_t>(frame.index)]);
            }
        }
    }
    if (ordered.empty()) {
        ordered = std::move(source);
    }
    if (ordered.empty()) {
        throw std::runtime_error("Animated texture " + std::string{name} + " has no frames");
    }
    if (ordered.size() != reserved) {
        std::cerr << "[texture-animation] " << name << " provides " << ordered.size()
                  << " frame(s); the atlas reserves " << reserved
                  << " — cycling/truncating to fit.\n";
    }
    std::vector<assets::ImageData> fitted;
    fitted.reserve(reserved);
    for (std::uint32_t index = 0; index < reserved; ++index) {
        fitted.push_back(ordered[index % ordered.size()]);
    }
    return fitted;
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

} // namespace

TextureArrayPixels bakeBlockAtlas(const assets::ResourceProvider& resources) {
    // Every block texture is resolved by name through the provider, so a layered
    // pack overrides them one file at a time (an imported pack can replace just
    // dirt without shipping every other block).
    const auto blockTex = [&](std::string_view name) {
        return assets::ImageData::loadRgbaOrMissing(resources, assets::textures("block/" + std::string{name} + ".png"));
    };
    auto top = blockTex("grass_block_top");
    auto side = blockTex("grass_block_side");
    const auto overlay = blockTex("grass_block_side_overlay");
    const auto dirt = blockTex("dirt");
    auto grassPlant = blockTex("short_grass");
    auto oakLeaves = blockTex("oak_leaves");
    // The animated and entity frames that fill the fixed special section. Item
    // icons no longer live here; they append after the block textures. Each is
    // fitted to the layers the atlas reserves for it, honouring the texture's
    // `.mcmeta` frame order and never crashing on an off-count pack.
    std::array<float, 4> fluidAnimationFrameTimes{1.0F, 1.0F, 1.0F, 1.0F};
    const auto animatedFrames = [&](std::string_view name, std::uint32_t reserved,
                                    std::size_t animationIndex) {
        const auto location = assets::textures("block/" + std::string{name} + ".png");
        const auto animation = assets::TextureAnimation::load(resources, location);
        if (animation.has_value()) {
            fluidAnimationFrameTimes[animationIndex] =
                static_cast<float>(animation->frametime);
        }
        return fitAnimationFrames(animatedSquareFrames(blockTex(name), top.width),
                                  animation, reserved, name);
    };
    auto waterStillFrames = animatedFrames("water_still", kWaterAnimationFrameCount, 0U);
    auto waterFlowFrames = animatedFrames("water_flow", kWaterAnimationFrameCount, 1U);
    auto lavaStillFrames = animatedFrames("lava_still", kLavaStillFrameCount, 2U);
    auto lavaFlowFrames = animatedFrames("lava_flow", kLavaFlowFrameCount, 3U);
    auto sunFrames = animatedSquareFrames(assets::ImageData::loadRgba(resources,
                                              assets::textures("environment/celestial/sun.png")),
                                          top.width);
    constexpr std::array<std::string_view, 8> kMoonPhaseNames{
        "full_moon", "waning_gibbous",  "third_quarter", "waning_crescent",
        "new_moon",  "waxing_crescent", "first_quarter", "waxing_gibbous",
    };
    std::array<assets::ImageData, kMoonPhaseNames.size()> moonPhaseTiles;
    for (std::size_t phase = 0; phase < kMoonPhaseNames.size(); ++phase) {
        const auto moon = assets::ImageData::loadRgba(resources, assets::textures(
            "environment/celestial/moon/" + std::string{kMoonPhaseNames[phase]} + ".png"));
        moonPhaseTiles[phase] = resizedRegion(moon, 0, 0, moon.width, moon.height, top.width);
    }
    const auto playerSkin = assets::ImageData::loadRgba(resources, assets::textures("entity/player/wide/steve.png"));
    const std::array playerParts{
        playerSkinCuboidFaces(playerSkin, 0, 0, 8, 8, 8, top.width),
        playerSkinCuboidFaces(playerSkin, 16, 16, 8, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 40, 16, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 32, 48, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 0, 16, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 16, 48, 4, 12, 4, top.width),
    };
    const auto chestTexture =
        assets::ImageData::loadRgba(resources, assets::textures("entity/chest/normal.png"));
    const auto furnaceFront = blockTex("furnace_front");
    const auto furnaceFrontOn = blockTex("furnace_front_on");
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
        destroyStages[stage] = blockTex("destroy_stage_" + std::to_string(stage));
    }
    // The biome leaves and their tints (spruce/birch fixed, the rest the biome
    // foliage colour), the same set the tree shapes grow.
    constexpr std::array<float, 3> foliageTint{0.49F, 0.74F, 0.32F};
    constexpr std::array<float, 3> spruceTint{0x61 / 255.0F, 0x99 / 255.0F, 0x61 / 255.0F};
    constexpr std::array<float, 3> birchTint{0x80 / 255.0F, 0xA7 / 255.0F, 0x55 / 255.0F};
    std::array<assets::ImageData, 5> biomeLeafTextures{
        blockTex("spruce_leaves"), blockTex("birch_leaves"),    blockTex("jungle_leaves"),
        blockTex("acacia_leaves"), blockTex("dark_oak_leaves"),
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
    // water/lava frame counts are now reconciled by fitAnimationFrames above, so
    // an off-count pack no longer aborts the bake. The sun is a single still
    // frame; more than one means the wrong texture, which is still a hard error.
    if (sunFrames.empty()) {
        throw std::runtime_error("Minecraft sun texture must contain at least one square frame");
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
        layers.push_back(conformToAtlasLayer(top, image, "fixed-section layer"));
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
                layers.push_back(conformToAtlasLayer(top, blockTex(image), image));
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
                layers.push_back(conformToAtlasLayer(top, blockTex(file), file));
            }
            layerByName.emplace(name, first);
            return first;
        }
        const float index = static_cast<float>(layers.size());
        layers.push_back(conformBlockLayer(top, blockTex(name), name));
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
    layerByName.emplace("short_grass", static_cast<float>(layers.size()));
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
        return assets::ImageData::loadRgba(resources, assets::textures(std::string{"colormap/"} + name));
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
    output.fluidAnimationFrameTimes = fluidAnimationFrameTimes;
    for (const auto& layer : layers) {
        output.rgba.insert(output.rgba.end(), layer.rgba.begin(), layer.rgba.end());
    }
    const std::uint32_t baseLayerCount = static_cast<std::uint32_t>(layers.size());
    // Item icons: one appended layer per registered item, in registry order.
    // Each resolves through the provider so a pack overrides item art per file.
    std::uint32_t itemIndex = 0U;
    const auto appendItemIcon = [&](const gameplay::Item* item) {
        assets::ImageData icon;
        // 26.1 ships one final sprite per spawn egg just like every other item;
        // the legacy shared shell/overlay tint composite no longer exists.
        icon = assets::ImageData::loadRgbaOrMissing(resources, assets::textures("item/" + std::string{item->textureName} + ".png"),
            top.width, top.height);
        const auto fitted = conformToAtlasLayer(top, icon, item->textureName);
        output.rgba.insert(output.rgba.end(), fitted.rgba.begin(), fitted.rgba.end());
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
