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

// 把动画纹理的源帧适配到图集为它预留的固定层数
// 图集布局和动画着色器写死了帧数，水 32 帧、岩浆 20 或 16 帧
// 帧数不同的资源包，或带显式 `.mcmeta` `frames` 顺序的，在这里被调和而不是崩掉
// 做法是先按元数据重排，再循环或截断到恰好填满预留层
// 整段动画的 `.mcmeta` frametime 由 bakeBlockAtlas() 另行转给着色器
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

// 按 item_entity.vert 的面序（+X、-X、+Y、-Y、+Z、-Z）从展开图裁出六个面
// `entityYFlip` 见 AtlasLayerFit.hpp 的 cuboidNetRects：生物/玩家经实体根节点的 Y 翻转绘制，
// 方块实体（箱子）没有，两者的上下面在展开图里正好互换
[[nodiscard]] PlayerSkinFaces cuboidFaces(const assets::ImageData& skin, int textureX, int textureY,
                                          int width, int height, int depth, int targetSize,
                                          bool entityYFlip) {
    const auto rects = cuboidNetRects(textureX, textureY, width, height, depth, entityYFlip);
    PlayerSkinFaces faces;
    for (std::size_t face = 0; face < faces.size(); ++face) {
        faces[face] = resizedRegion(skin, rects[face].x, rects[face].y, rects[face].width,
                                    rects[face].height, targetSize);
    }
    return faces;
}

[[nodiscard]] PlayerSkinFaces playerSkinCuboidFaces(const assets::ImageData& skin, int textureX,
                                                    int textureY, int width, int height, int depth,
                                                    int targetSize) {
    return cuboidFaces(skin, textureX, textureY, width, height, depth, targetSize,
                       /*entityYFlip=*/true);
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
    // 每张方块纹理都按名字经 provider 解析，叠加的资源包因此可以逐文件覆盖
    // 导入的包只想换泥土，就不必提供其余所有方块
    const auto blockTex = [&](std::string_view name) {
        return assets::ImageData::loadRgbaOrMissing(resources, assets::textures("block/" + std::string{name} + ".png"));
    };
    auto top = blockTex("grass_block_top");
    auto side = blockTex("grass_block_side");
    const auto overlay = blockTex("grass_block_side_overlay");
    const auto dirt = blockTex("dirt");
    auto grassPlant = blockTex("short_grass");
    auto oakLeaves = blockTex("oak_leaves");
    // 填充固定特殊区的动画帧与实体帧（物品图标不在这里，它们接在方块纹理之后）
    // 每项都适配到图集为其预留的层数，遵守纹理 `.mcmeta` 的帧顺序，帧数不符的资源包也不会崩
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
    // 从 4x4 的 experience_orb.png 里取左上角那个 16x16 格，缩放到图集瓦片尺寸
    // 允许缺失：没有这张实体纹理的资源包烘出棋盘格而不是中止
    const auto experienceOrbSheet = assets::ImageData::loadRgbaOrMissing(
        resources, assets::textures("entity/experience/experience_orb.png"), 64U, 64U);
    const auto experienceOrb =
        resizedRegion(experienceOrbSheet, 0, 0, 16, 16, top.width);
    // RN-9a：粒子精灵集。一集对应 vanilla 的 particles/<id>.json，名字表就是它的
    // textures 数组，顺序即层序。允许缺失：没有这些贴图的资源包烘出棋盘格而不是中止
    // ——它们不入库，只从用户自备资源包读
    std::vector<assets::ImageData> particleSprites;
    for (const auto& set : kParticleSpriteSets) {
        for (const std::string_view texture : set.textures) {
            const auto sheet = assets::ImageData::loadRgbaOrMissing(
                resources, assets::textures("particle/" + std::string{texture} + ".png"),
                16U, 16U);
            particleSprites.push_back(
                resizedRegion(sheet, 0, 0, sheet.width, sheet.height, top.width));
        }
    }
    // 箱子是方块实体：`ChestRenderer` 只按 FACING 偏航，没有生物那层根节点 Y 翻转，
    // 所以三个部件的上下面都取展开图里与玩家相反的那一块（entityYFlip=false）
    // 从前这里只给盖子手工 swap 了一次，底座没管，开盖后看到的箱口那一面
    // 显示的是木板——和箱子顶面同一块像素——而不是 (28,19) 那块带箱口的
    auto chestParts = std::array{
        cuboidFaces(chestTexture, 0, 19, 14, 10, 14, top.width, /*entityYFlip=*/false),
        cuboidFaces(chestTexture, 0, 0, 14, 5, 14, top.width, /*entityYFlip=*/false),
        cuboidFaces(chestTexture, 0, 0, 2, 4, 1, top.width, /*entityYFlip=*/false),
    };
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
    // 各群系树叶及其着色（云杉/白桦为固定色，其余取群系叶色），与树形生成用的是同一套
    constexpr std::array<float, 3> foliageTint{0.49F, 0.74F, 0.32F};
    constexpr std::array<float, 3> spruceTint{0x61 / 255.0F, 0x99 / 255.0F, 0x61 / 255.0F};
    constexpr std::array<float, 3> birchTint{0x80 / 255.0F, 0xA7 / 255.0F, 0x55 / 255.0F};
    std::array<assets::ImageData, 5> biomeLeafTextures{
        blockTex("spruce_leaves"), blockTex("birch_leaves"),    blockTex("jungle_leaves"),
        blockTex("acacia_leaves"), blockTex("dark_oak_leaves"),
    };
    const std::array<std::array<float, 3>, 5> biomeLeafTints{spruceTint, birchTint, foliageTint,
                                                             foliageTint, foliageTint};
    // 地形用的是未着色叶片底图，逐方块的群系着色由网格化器在顶点上做
    // 上面那批已着色的留给物品和 GUI
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
    // 水/岩浆的帧数由上面的 fitAnimationFrames 调和，帧数不符的资源包不会中止烘焙
    // 太阳只有一帧静止图，多于一帧说明拿错了纹理，这仍然是硬错误
    if (sunFrames.empty()) {
        throw std::runtime_error("Minecraft sun texture must contain at least one square frame");
    }
    // 未着色的草方块家族，供下面的逐群系配色变体使用
    const auto grassTopRaw = top;
    const auto grassSideBase = side;
    const auto grassOverlay = overlay;
    const auto grassPlantRaw = grassPlant;
    const auto leavesRaw = oakLeaves;
    // 叶片与草在进入图集之前先按 vanilla 配色着色
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
    // 水的帧只定不透明度，颜色留给顶点上的群系水色
    // 从前这里按一个固定的蓝色着色，于是沼泽的浑绿与寒冷海洋的深蓝根本不存在；
    // 那个常量恰好接近 vanilla 的默认水色 4159204，所以多数群系看起来没差，
    // 唯独真正有自己水色的那几个群系一直是错的
    const auto setWaterAlpha = [&](std::vector<assets::ImageData>& frames) {
        for (auto& frame : frames) {
            for (std::size_t index = 0; index + 3U < frame.rgba.size(); index += 4U) {
                frame.rgba[index + 3U] = 155U;
            }
        }
    };
    setWaterAlpha(waterStillFrames);
    setWaterAlpha(waterFlowFrames);

    // ---- 固定特殊区，顺序确定 ----
    std::vector<assets::ImageData> layers;
    // 烘焙期被乘过颜色的层号。地形只能取未着色的层（颜色走顶点 tint），
    // 这张表让 biome_tint_layers 测试能把「谁被预着色」与「谁吃顶点 tint」对起来。
    std::vector<std::uint32_t> preTintedLayers;
    const auto markPreTinted = [&preTintedLayers](std::size_t layer) {
        preTintedLayers.push_back(static_cast<std::uint32_t>(layer));
    };
    const auto append = [&](const assets::ImageData& image) {
        layers.push_back(conformToAtlasLayer(top, image, "fixed-section layer"));
    };
    // 每段开始之前核对写入位置正好落在 BlockAtlasLayout 声明的起始层号上
    // 从前只在最末尾校验一次总数，某段少一层、另一段多一层就会互相抵消：
    // 总数对得上，采样侧却整段错位（箱子去读破坏阶段的贴图），而且没有任何征兆
    // 现在每段起点都被钉住，错位在启动时当场报出是哪一段偏了、偏了多少
    const auto beginSegment = [&](float firstLayer, const char* segment) {
        if (static_cast<float>(layers.size()) != firstLayer) {
            throw std::runtime_error(
                std::string{"Block atlas fixed section drifted before \""} + segment +
                "\": at layer " + std::to_string(layers.size()) + ", BlockAtlasLayout expects " +
                std::to_string(static_cast<int>(firstLayer)));
        }
    };

    beginSegment(static_cast<float>(kWaterStillLayer), "water_still");
    for (const auto& frame : waterStillFrames)
        append(frame); // 0..31
    beginSegment(static_cast<float>(kWaterFlowLayer), "water_flow");
    for (const auto& frame : waterFlowFrames)
        append(frame); // 32..63
    beginSegment(static_cast<float>(kLavaStillLayer), "lava_still");
    for (const auto& frame : lavaStillFrames)
        append(frame); // 64..83
    beginSegment(static_cast<float>(kLavaFlowLayer), "lava_flow");
    for (const auto& frame : lavaFlowFrames)
        append(frame); // 84..99
    beginSegment(kPlayerHeadFirstLayer, "player skin parts");
    for (const auto& part : playerParts) {
        for (const auto& face : part)
            append(face); // 100..135
    }
    beginSegment(kDestroyStageFirstLayer, "destroy stages");
    for (const auto& stage : destroyStages)
        append(stage); // 136..145
    beginSegment(kChestBaseFirstLayer, "chest parts");
    for (const auto& part : chestParts) {
        for (const auto& face : part)
            append(face); // 146..163
    }
    beginSegment(kChestItemTopLayer, "chest item faces");
    for (const auto& texture : chestItemTextures)
        append(texture);    // 164..166
    beginSegment(kMoonPhaseFirstLayer, "moon phases");
    for (const auto& tile : moonPhaseTiles)
        append(tile);            // 167..174
    beginSegment(kSunLayer, "sun");
    append(sunFrames.front());   // 175
    beginSegment(kExperienceOrbLayer, "experience orb");
    append(experienceOrb);       // 176
    beginSegment(kParticleSpriteFirstLayer, "particle sprites");
    for (const auto& sprite : particleSprites)
        append(sprite);          // 177..202

    // ---- 动态方块纹理，按方块注册表的名字解析 ----
    // 合成出来的图层按名字登记，复用它们的方块因此都指向同一层
    // 比如 grass_block_side、dirt 和已着色的叶片
    beginSegment(static_cast<float>(kFirstBlockTextureLayer), "dynamic block textures");
    std::unordered_map<std::string, float> layerByName;
    // 非流体动画方块纹理，`assign` 每烘一条多帧动画条就累积一项
    // 最后统一转给 output.blockAnimations
    std::vector<BlockTextureAnimation> blockAnimations;
    const auto assign = [&](const char* name) -> float {
        const auto existing = layerByName.find(name);
        if (existing != layerByName.end()) {
            return existing->second;
        }
        // 作物各生长阶段从第 0 阶层号起连续排列，网格化器直接用 stage0 + age 取层
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
        // 耕地的湿润变体紧跟在干燥面之后
        if (view == "farmland") {
            const float first = static_cast<float>(layers.size());
            for (const char* file : {"farmland", "farmland_moist"}) {
                layers.push_back(conformToAtlasLayer(top, blockTex(file), file));
            }
            layerByName.emplace(name, first);
            return first;
        }
        const auto image = blockTex(name);
        if (image.width > 0 && image.height > image.width && image.height % image.width == 0) {
            // 多帧的方块动画条（岩浆块、海晶石等）：把每一帧连续烘入并记下动画信息，着色器据此轮播
            // 若有 .mcmeta 帧顺序则遵守，与流体一致
            auto frames = animatedSquareFrames(image, top.width);
            const auto animation = assets::TextureAnimation::load(
                resources, assets::textures("block/" + std::string{name} + ".png"));
            std::vector<assets::ImageData> ordered;
            if (animation.has_value() && !animation->frames.empty()) {
                for (const auto& frame : animation->frames) {
                    if (frame.index >= 0 &&
                        static_cast<std::size_t>(frame.index) < frames.size()) {
                        ordered.push_back(frames[static_cast<std::size_t>(frame.index)]);
                    }
                }
            }
            if (ordered.empty()) {
                ordered = std::move(frames);
            }
            const float base = static_cast<float>(layers.size());
            for (const auto& frame : ordered) {
                layers.push_back(conformToAtlasLayer(top, frame, name));
            }
            blockAnimations.push_back(
                {base, static_cast<std::uint32_t>(ordered.size()),
                 animation.has_value() ? static_cast<float>(animation->frametime) : 1.0F});
            layerByName.emplace(name, base);
            return base;
        }
        const float index = static_cast<float>(layers.size());
        layers.push_back(conformToAtlasLayer(top, image, name));
        layerByName.emplace(name, index);
        return index;
    };
    // 合成图层先登记，复用方能共享同一层
    markPreTinted(layers.size());  // 物品/GUI 副本，烘死了 foliage 色
    layerByName.emplace("grass_block_top", static_cast<float>(layers.size()));
    layers.push_back(top);
    markPreTinted(layers.size());  // 物品/GUI 副本，烘死了 foliage 色
    layerByName.emplace("grass_block_side", static_cast<float>(layers.size()));
    layers.push_back(side);
    layerByName.emplace("dirt", static_cast<float>(layers.size()));
    layers.push_back(dirt);
    markPreTinted(layers.size());  // 物品/GUI 副本，烘死了 foliage 色
    layerByName.emplace("short_grass", static_cast<float>(layers.size()));
    layers.push_back(grassPlant);
    markPreTinted(layers.size());  // 物品/GUI 副本，烘死了 foliage 色
    layerByName.emplace("oak_leaves", static_cast<float>(layers.size()));
    layers.push_back(oakLeaves);
    const std::array<const char*, 5> biomeLeafNames{
        "spruce_leaves", "birch_leaves", "jungle_leaves", "acacia_leaves", "dark_oak_leaves"};
    for (std::size_t leaf = 0; leaf < biomeLeafNames.size(); ++leaf) {
        markPreTinted(layers.size());  // 同上：物品/GUI 用的已着色副本
        layerByName.emplace(biomeLeafNames[leaf], static_cast<float>(layers.size()));
        layers.push_back(biomeLeafTextures[leaf]);
    }

    // ---- 逐群系的草/叶配色（对应 vanilla BiomeColors）----
    // vanilla 的草与叶配色图是按温度和湿度索引的 256x256 查找表，每个群系各取各的
    // 网格化器逐方块做双线性混合，与 BlockGetter 的取色一致
    // 群系边界因此是平滑的颜色渐变而不是硬切换
    // 沼泽与黑森林有各自的特例，见下
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
    // 地形读的是未着色的草方块家族与橡木系树叶，颜色由网格化器逐列解析后写进顶点
    // 图集里另有一份已着色的副本，给没有群系可问的物品与 GUI 用
    //
    // 草方块侧面存两层：泥土底图，以及画在它上面的草形状 overlay
    // vanilla 的 grass_block 模型正是为此把侧面画两遍——预先合成好的单张纹理一旦着色，
    // 连泥土也会跟着变绿
    //
    // 云杉与白桦树叶保留固定色层：它们在每个群系里都是同一个常量色
    const float terrainGrassTop = static_cast<float>(layers.size());
    layers.push_back(grassTopRaw);
    layerByName.emplace("grass_block_top:terrain", terrainGrassTop);
    const float terrainGrassPlant = static_cast<float>(layers.size());
    layers.push_back(grassPlantRaw);
    layerByName.emplace("grass:terrain", terrainGrassPlant);
    const float terrainGrassSide = static_cast<float>(layers.size());
    layers.push_back(grassSideBase);
    layerByName.emplace("grass_block_side:terrain", terrainGrassSide);
    const float terrainGrassOverlay = static_cast<float>(layers.size());
    layers.push_back(grassOverlay);
    layerByName.emplace("grass_block_side_overlay:terrain", terrainGrassOverlay);
    world::gen::setTerrainGrassLayers(terrainGrassTop, terrainGrassPlant, terrainGrassSide,
                                      terrainGrassOverlay);
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
        if (leafFixedTints[leaf] != 0U) {
            // 云杉/白桦是 vanilla 自己的常量色（BlockColors 的 constant(...)），
            // 烘死是对的——它们在 tintKindFor 里也确实拿 TintKind::None。
            markPreTinted(layers.size());
        }
        layers.push_back(pixels);
        layerByName.emplace(std::string("leaves:terrain:") + leafNames[leaf], layer);
        world::gen::setTerrainLeafLayer(leafBlocks[leaf], layer);
    }
    // 群系配色解析成颜色值，不再烘成图集层
    //
    // 从前这里给每个群系烘 3 个草层加 4 个叶层——25 个群系就是 175 层，26.1 的 66 个群系
    // 会是 462 层，数据包再加还要涨；而离散的层既混不出边界渐变，也没法给水上色
    // 现在每个群系解析出四个颜色，网格化器按 vanilla 的 5x5 方块窗口逐列取平均
    for (int biomeIndex = 0; biomeIndex < static_cast<int>(world::gen::Biome::Count);
         ++biomeIndex) {
        const auto biome = static_cast<world::gen::Biome>(biomeIndex);
        const auto& definition = world::gen::biomeDefinition(biome);
        // 有 override 就用 override，否则按 (temperature, downfall) 查配色图
        // grassColorModifier 不在这里应用：沼泽那档取决于方块坐标，属于网格化器
        world::gen::BiomeSurfaceColors colors;
        colors.grass = definition.grassColorOverride != 0U
            ? definition.grassColorOverride
            : colormapColor(grassColormap, definition.temperature, definition.downfall);
        colors.foliage = definition.foliageColorOverride != 0U
            ? definition.foliageColorOverride
            : colormapColor(foliageColormap, definition.temperature, definition.downfall);
        colors.dryFoliage = definition.dryFoliageColorOverride != 0U
            ? definition.dryFoliageColorOverride
            : colors.foliage;
        colors.water = definition.waterColor;
        world::gen::setBiomeSurfaceColors(biome, colors);
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
            // 掉落的箱子物品画的是烘好的箱子物品面
            world::setBlockTextureLayers(block, {static_cast<float>(kChestItemTopLayer),
                                                 static_cast<float>(kChestItemSideLayer),
                                                 static_cast<float>(kChestItemSideLayer)});
            continue;
        }
        if (definition.model == world::BlockModel::DirectionalCube) {
            // 解析六个具名面；方块没有通电变体时，backActive 回落到 back
            world::DirectionalTextureLayers dl;
            dl.front = assign(definition.directional.front);
            dl.frontActive = definition.directional.frontActive
                ? assign(definition.directional.frontActive)
                : dl.front;
            dl.back = assign(definition.directional.back);
            dl.backActive = definition.directional.backActive
                ? assign(definition.directional.backActive)
                : dl.back;
            dl.top = assign(definition.directional.top);
            dl.bottom = assign(definition.directional.bottom);
            dl.side = assign(definition.directional.side);
            world::setBlockDirectionalLayers(block, dl);
            // 不 continue：扁平三元组照常由下面的通用分支从 `.texture()` 解析
            // 这里曾经再填一次扁平三元组，还刻意把**正面**放进 `side` 槽——当时物品立方体只读
            // 顶/侧/底三个槽，塞正面进去才认得出是熔炉。那是个陷阱：RN-8c-D 给掉落物补上真正的
            // 正面之后，侧面仍从这里读，于是熔炉有三个面是炉口。
            // 现在每个 DirectionalCube 都自己声明 `.texture()`（活塞声明的正是它的 inventory
            // 模型三张图），三元组走下面的通用解析，没有特例。
        }
        if (definition.model == world::BlockModel::ElementModel ||
            definition.model == world::BlockModel::RedstoneWire) {
            // 解析网格化器在逐元素（或红石线）转写时要读的逐方块纹理槽
            // 方块自身的 `.texture()` 仍保留（在下面解析），供扁平的 HUD/物品图标和掉落物使用
            std::array<float, world::kMaxModelTextureSlots> slots{};
            for (std::size_t i = 0; i < slots.size(); ++i) {
                slots[i] = definition.modelTextures[i] ? assign(definition.modelTextures[i]) : 0.0F;
            }
            world::setBlockModelSlotLayers(block, slots);
        }
        world::BlockTextureLayers resolved;
        resolved.top = assign(definition.textures.top);
        resolved.side = assign(definition.textures.side);
        resolved.bottom = assign(definition.textures.bottom);
        world::setBlockTextureLayers(block, resolved);
    }

    // RN-8c-D: 物品立方体把前/后面的层号与 UV 模型打包进一个 float 传给着色器
    // （world::packItemCubeFaces），层号必须小于 kItemLayerPackStride 才能保证精确
    // 这里在图集建好后硬校验一次：与其让层号在着色器里静默串位，不如启动就炸
    if (static_cast<float>(layers.size()) >= world::kItemLayerPackStride) {
        throw std::runtime_error(
            "block atlas has more layers than the item cube face packing can carry");
    }

    // 熄灭的红石火把贴图：网格化器在 LIT=false 时换上的第二张纹理（方块自己的侧面纹理是点亮态那张）
    world::setRedstoneTorchOffLayer(assign("redstone_torch_off"));

    TextureArrayPixels output;
    output.width = static_cast<std::uint32_t>(top.width);
    output.height = static_cast<std::uint32_t>(top.height);
    output.fluidAnimationFrameTimes = fluidAnimationFrameTimes;
    output.blockAnimations = std::move(blockAnimations);
    std::ranges::sort(preTintedLayers);
    output.preTintedLayers = std::move(preTintedLayers);
    for (const auto& layer : layers) {
        output.rgba.insert(output.rgba.end(), layer.rgba.begin(), layer.rgba.end());
    }
    const std::uint32_t baseLayerCount = static_cast<std::uint32_t>(layers.size());
    // 物品图标按注册顺序逐个追加一层，各自经 provider 解析
    // 资源包因此能逐文件覆盖物品美术
    std::uint32_t itemIndex = 0U;
    const auto appendItemIcon = [&](const gameplay::Item* item) {
        assets::ImageData icon;
        // 26.1 的每个刷怪蛋和其它物品一样只有一张成品贴图，不再是共享外壳加叠加层着色的合成方式
        icon = assets::ImageData::loadRgbaOrMissing(resources, assets::textures("item/" + std::string{item->textureName} + ".png"),
            top.width, top.height);
        // 皮革护甲是两层贴图，灰度底图按皮革颜色着色，默认色为 0xA06540
        // 上面再叠一张不着色的全彩 `_overlay`，画的是扣件与镶边
        // 少了着色，底图会白得像铁；少了叠加层，镶边就没了
        // 只有皮革护甲可染色，所以只有它走这条路径
        if (item->armorMaterial == gameplay::ArmorMaterialId::Leather) {
            constexpr std::uint32_t kDefaultLeather = 0xA06540U;
            const auto tintChannel = [](std::uint8_t value, std::uint32_t channel) {
                return static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) * channel / 255U);
            };
            for (std::size_t p = 0; p + 3U < icon.rgba.size(); p += 4U) {
                icon.rgba[p + 0U] = tintChannel(icon.rgba[p + 0U], (kDefaultLeather >> 16) & 0xFFU);
                icon.rgba[p + 1U] = tintChannel(icon.rgba[p + 1U], (kDefaultLeather >> 8) & 0xFFU);
                icon.rgba[p + 2U] = tintChannel(icon.rgba[p + 2U], kDefaultLeather & 0xFFU);
            }
            const auto overlayLocation =
                assets::textures("item/" + std::string{item->textureName} + "_overlay.png");
            if (resources.exists(overlayLocation)) {
                const auto overlay =
                    assets::ImageData::loadRgbaOrMissing(resources, overlayLocation, icon.width,
                                                         icon.height);
                if (overlay.width == icon.width && overlay.height == icon.height &&
                    overlay.rgba.size() == icon.rgba.size()) {
                    // 普通的 source-over 合成，叠加层的镶边压在已着色的底图上
                    // 由它自己的 alpha 决定哪里显现
                    for (std::size_t p = 0; p + 3U < icon.rgba.size(); p += 4U) {
                        const std::uint32_t a = overlay.rgba[p + 3U];
                        if (a == 0U) continue;
                        for (std::size_t c = 0; c < 3U; ++c) {
                            icon.rgba[p + c] = static_cast<std::uint8_t>(
                                (static_cast<std::uint32_t>(overlay.rgba[p + c]) * a +
                                 static_cast<std::uint32_t>(icon.rgba[p + c]) * (255U - a)) /
                                255U);
                        }
                        icon.rgba[p + 3U] = static_cast<std::uint8_t>(
                            std::max<std::uint32_t>(icon.rgba[p + 3U], a));
                    }
                }
            }
        }
        const auto fitted = conformToAtlasLayer(top, icon, item->textureName);
        output.rgba.insert(output.rgba.end(), fitted.rgba.begin(), fitted.rgba.end());
        gameplay::setItemTextureLayer(item, static_cast<float>(baseLayerCount + itemIndex));
        ++itemIndex;
    };
    for (const gameplay::Item* item : gameplay::kItemRegistry)
        appendItemIcon(item);
    for (const gameplay::Item* item : gameplay::kSpawnEggItems)
        appendItemIcon(item);
    // 岩浆的动画帧在固定区（64..99）；物品图标之后再无内容，整个图集就是上面这些层
    return output;
}

} // namespace mc::render
