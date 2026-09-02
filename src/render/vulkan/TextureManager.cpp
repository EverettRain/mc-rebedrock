#include "render/vulkan/TextureManager.hpp"

#include "render/vulkan/BlockAtlasBaker.hpp"
#include "render/vulkan/HudTypes.hpp"

#include "animation/SkeletalModel.hpp"
#include "assets/FontProviders.hpp"
#include "assets/ImageData.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/gen/Biome.hpp"

#include <glm/glm.hpp>
#include <miniz.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace mc::render {

namespace {

[[nodiscard]] assets::ImageData repeatTileToAtlas(const assets::ImageData& tile, int width,
                                                  int height, int repeats) {
    assets::ImageData atlas;
    atlas.width = width;
    atlas.height = height;
    atlas.rgba.resize(static_cast<std::size_t>(width * height * 4));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int sourceX = (x * tile.width * repeats / width) % tile.width;
            const int sourceY = (y * tile.height * repeats / height) % tile.height;
            const std::size_t sourceIndex =
                static_cast<std::size_t>((sourceY * tile.width + sourceX) * 4);
            const std::size_t destinationIndex = static_cast<std::size_t>((y * width + x) * 4);
            std::copy_n(tile.rgba.begin() + static_cast<std::ptrdiff_t>(sourceIndex), 4,
                        atlas.rgba.begin() + static_cast<std::ptrdiff_t>(destinationIndex));
        }
    }
    return atlas;
}

[[nodiscard]] assets::ImageData emptyRgbaAtlas(int width = 256, int height = 256) {
    assets::ImageData atlas;
    atlas.width = width;
    atlas.height = height;
    atlas.rgba.resize(static_cast<std::size_t>(width * height * 4));
    return atlas;
}

void blit(assets::ImageData& destination, const assets::ImageData& source, int destinationX,
          int destinationY) {
    for (int sourceY = 0; sourceY < source.height; ++sourceY) {
        const int targetY = destinationY + sourceY;
        if (targetY < 0 || targetY >= destination.height) {
            continue;
        }
        for (int sourceX = 0; sourceX < source.width; ++sourceX) {
            const int targetX = destinationX + sourceX;
            if (targetX < 0 || targetX >= destination.width) {
                continue;
            }
            const auto sourceOffset =
                static_cast<std::size_t>((sourceY * source.width + sourceX) * 4);
            const auto targetOffset =
                static_cast<std::size_t>((targetY * destination.width + targetX) * 4);
            std::copy_n(source.rgba.begin() + static_cast<std::ptrdiff_t>(sourceOffset), 4,
                        destination.rgba.begin() + static_cast<std::ptrdiff_t>(targetOffset));
        }
    }
}

struct UnihexPages final {
    std::vector<std::uint8_t> sizes = std::vector<std::uint8_t>(0x10000U);
    std::array<std::vector<std::uint8_t>, 256> alpha;
    std::vector<bool> seen = std::vector<bool>(0x10000U);
};

struct BitmapProviderLayer final {
    std::vector<std::uint8_t> alpha;
    std::vector<std::pair<char32_t, ui::FontGlyph>> glyphs;
};

[[nodiscard]] std::vector<std::uint8_t> alphaAtlas256(const assets::ImageData& image) {
    constexpr int kSize = 256;
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(kSize * kSize));
    for (int y = 0; y < kSize; ++y) {
        const int sourceY = std::min(y * image.height / kSize, image.height - 1);
        for (int x = 0; x < kSize; ++x) {
            const int sourceX = std::min(x * image.width / kSize, image.width - 1);
            alpha[static_cast<std::size_t>(y * kSize + x)] =
                image.rgba[static_cast<std::size_t>((sourceY * image.width + sourceX) * 4 + 3)];
        }
    }
    return alpha;
}

[[nodiscard]] BitmapProviderLayer
loadBitmapProvider(const assets::ResourceProvider& resources,
                   const assets::FontProviderDefinition& definition) {
    BitmapProviderLayer result;
    const auto image = assets::ImageData::loadRgbaOrMissing(resources, definition.file);
    const std::size_t rowCount = definition.chars.size();
    std::size_t columnCount = 0U;
    for (const auto& row : definition.chars) {
        columnCount = std::max(columnCount, row.size());
    }
    if (rowCount == 0U || columnCount == 0U || image.width % static_cast<int>(columnCount) != 0 ||
        image.height % static_cast<int>(rowCount) != 0) {
        throw std::runtime_error("Bitmap font grid does not match " + definition.file.toString());
    }
    result.alpha = alphaAtlas256(image);
    const int cellWidth = image.width / static_cast<int>(columnCount);
    const int cellHeight = image.height / static_cast<int>(rowCount);
    const float oversample =
        static_cast<float>(cellHeight) / static_cast<float>(std::max(definition.height, 1));
    for (std::size_t row = 0U; row < rowCount; ++row) {
        for (std::size_t column = 0U; column < definition.chars[row].size(); ++column) {
            const char32_t codepoint = definition.chars[row][column];
            if (codepoint == U'\0') {
                continue;
            }
            const int cellX = static_cast<int>(column) * cellWidth;
            const int cellY = static_cast<int>(row) * cellHeight;
            int left = cellWidth;
            int right = -1;
            for (int y = 0; y < cellHeight; ++y) {
                for (int x = 0; x < cellWidth; ++x) {
                    const auto alphaIndex =
                        static_cast<std::size_t>(((cellY + y) * image.width + cellX + x) * 4 + 3);
                    if (image.rgba[alphaIndex] != 0U) {
                        left = std::min(left, x);
                        right = std::max(right, x);
                    }
                }
            }
            if (right >= left) {
                ui::FontGlyph glyph;
                const float sourceWidth = static_cast<float>(right - left + 1);
                glyph.u = static_cast<float>(cellX + left) / static_cast<float>(image.width);
                glyph.v = static_cast<float>(cellY) / static_cast<float>(image.height);
                glyph.uvWidth = sourceWidth / static_cast<float>(image.width);
                glyph.uvHeight = static_cast<float>(cellHeight) / static_cast<float>(image.height);
                glyph.pixelWidth = sourceWidth / oversample;
                glyph.pixelHeight = static_cast<float>(definition.height);
                glyph.offsetY = 7.0F - static_cast<float>(definition.ascent);
                glyph.advance = std::ceil(glyph.pixelWidth) + 1.0F;
                glyph.visible = true;
                result.glyphs.emplace_back(codepoint, glyph);
            }
        }
    }
    return result;
}

[[nodiscard]] int hexDigit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

// unihex 字体是打在资源包*内部*的一个 zip，所以这里取出归档的字节流让 miniz 直接在内存上打开
// 否则一个 zip 资源包得先把整个字体归档解压到磁盘，只为再打开一次
void readUnihexArchive(std::span<const std::byte> archiveBytes,
                       const assets::FontProviderDefinition& definition,
                       const std::set<int>& requiredPages, UnihexPages& output) {
    mz_zip_archive zip{};
    if (mz_zip_reader_init_mem(&zip, archiveBytes.data(), archiveBytes.size(), 0) == MZ_FALSE) {
        throw std::runtime_error("Unable to open unihex archive: " + definition.file.toString());
    }
    const auto closeZip = [&] { mz_zip_reader_end(&zip); };
    try {
        const mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
        for (mz_uint fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
            mz_zip_archive_file_stat stat;
            if (mz_zip_reader_file_stat(&zip, fileIndex, &stat) == MZ_FALSE ||
                !std::string_view{stat.m_filename}.ends_with(".hex")) {
                continue;
            }
            size_t extractedSize = 0U;
            void* extracted = mz_zip_reader_extract_to_heap(&zip, fileIndex, &extractedSize, 0);
            if (extracted == nullptr) {
                continue;
            }
            const std::string_view contents{static_cast<const char*>(extracted), extractedSize};
            std::size_t lineStart = 0U;
            while (lineStart < contents.size()) {
                const auto lineEnd = contents.find('\n', lineStart);
                const std::string_view line = contents.substr(
                    lineStart, lineEnd == std::string_view::npos ? contents.size() - lineStart
                                                                 : lineEnd - lineStart);
                lineStart = lineEnd == std::string_view::npos ? contents.size() : lineEnd + 1U;
                const auto colon = line.find(':');
                if (colon == std::string_view::npos) {
                    continue;
                }
                std::uint32_t codepoint = 0U;
                const auto parsed =
                    std::from_chars(line.data(), line.data() + colon, codepoint, 16);
                const std::string_view bitmap = line.substr(colon + 1U);
                if (parsed.ec != std::errc{} || codepoint > 0xFFFFU ||
                    !requiredPages.contains(static_cast<int>(codepoint >> 8U)) ||
                    output.seen[codepoint] ||
                    (bitmap.size() != 32U && bitmap.size() != 64U && bitmap.size() != 96U &&
                     bitmap.size() != 128U)) {
                    continue;
                }
                const int sourceWidth = static_cast<int>(bitmap.size() / 4U);
                if (sourceWidth > 16) {
                    continue;
                }
                int left = sourceWidth;
                int right = -1;
                const int rowDigits = sourceWidth / 4;
                const int page = static_cast<int>(codepoint >> 8U);
                if (output.alpha[static_cast<std::size_t>(page)].empty()) {
                    output.alpha[static_cast<std::size_t>(page)].resize(256U * 256U);
                }
                auto& pagePixels = output.alpha[static_cast<std::size_t>(page)];
                const int cellX = static_cast<int>(codepoint & 0x0FU) * 16;
                const int cellY = static_cast<int>((codepoint >> 4U) & 0x0FU) * 16;
                for (int y = 0; y < 16; ++y) {
                    for (int x = 0; x < sourceWidth; ++x) {
                        const int digit =
                            hexDigit(bitmap[static_cast<std::size_t>(y * rowDigits + x / 4)]);
                        if (digit < 0 || (digit & (1 << (3 - x % 4))) == 0) {
                            continue;
                        }
                        left = std::min(left, x);
                        right = std::max(right, x);
                        pagePixels[static_cast<std::size_t>((cellY + y) * 256 + cellX + x)] = 0xFFU;
                    }
                }
                for (const auto& sizeOverride : definition.sizeOverrides) {
                    if (codepoint >= sizeOverride.from && codepoint <= sizeOverride.to) {
                        left = sizeOverride.left;
                        right = sizeOverride.right;
                        break;
                    }
                }
                if (right >= left) {
                    left = std::clamp(left, 0, 15);
                    right = std::clamp(right, left, 15);
                    output.sizes[codepoint] = static_cast<std::uint8_t>((left << 4) | right);
                }
                output.seen[codepoint] = true;
            }
            mz_free(extracted);
        }
        closeZip();
    } catch (...) {
        closeZip();
        throw;
    }
}

[[nodiscard]] assets::ImageData singleChestGui(const assets::ImageData& generic) {
    assets::ImageData result = generic;
    std::ranges::fill(result.rgba, 0U);
    const auto copyRows = [&](int sourceY, int destinationY, int rowCount) {
        for (int row = 0; row < rowCount; ++row) {
            const auto source = generic.rgba.begin() +
                                static_cast<std::ptrdiff_t>(((sourceY + row) * generic.width) * 4);
            const auto destination =
                result.rgba.begin() +
                static_cast<std::ptrdiff_t>(((destinationY + row) * result.width) * 4);
            std::copy_n(source, static_cast<std::size_t>(generic.width * 4), destination);
        }
    };
    // 容器界面把上方箱子行与下方玩家背包区拼在一起；这里把三行的变体烘成一个图集层
    copyRows(0, 0, 71);
    copyRows(126, 71, 96);
    return result;
}

} // namespace

void TextureManager::createTextureArray(int anisotropy) {
    const auto pixels = bakeBlockAtlas(*resourceProvider_);
    fluidAnimationFrameTimes = pixels.fluidAnimationFrameTimes;
    blockAnimations = pixels.blockAnimations;
    const auto byteSize = static_cast<VkDeviceSize>(pixels.rgba.size());
    // 图集层数由按名解析的构建结果决定，含固定特殊区、方块纹理和物品图标
    // 因此从字节数反推，而不是写死成编译期常量
    const auto layerSize =
        static_cast<VkDeviceSize>(pixels.width) * static_cast<VkDeviceSize>(pixels.height) * 4U;
    if (byteSize % layerSize != 0U) {
        throw std::runtime_error("Block texture array data is not whole layers");
    }
    const std::uint32_t layerCount = static_cast<std::uint32_t>(byteSize / layerSize);
    textureImage =
        resources_->createImage(pixels.width, pixels.height, layerCount, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    // 地形着色器在顶点阶段就要读方块层（面朝向选层），因此目标阶段含 VERTEX
    resources_->uploadImageLayers(textureImage, pixels.rgba.data(), byteSize, pixels.width,
                                  pixels.height, layerCount,
                                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    textureView = resources_->createImageView(textureImage.image, VK_FORMAT_R8G8B8A8_UNORM,
                                              VK_IMAGE_ASPECT_COLOR_BIT, layerCount,
                                              VK_IMAGE_VIEW_TYPE_2D_ARRAY);

    createTextureSampler(anisotropy);
    std::cout << "Loaded block texture array: " << pixels.width << 'x' << pixels.height << " x "
              << layerCount << '\n';
}

void TextureManager::createTextureSampler(int anisotropy) {
    auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = anisotropySupported_ && anisotropy > 1 ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = samplerInfo.anisotropyEnable == VK_TRUE
                                    ? std::min(static_cast<float>(anisotropy), maxAnisotropy_)
                                    : 1.0F;
    samplerInfo.maxLod = 0.0F;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &textureSampler), "vkCreateSampler");
}

void TextureManager::recreateTextureSampler(int anisotropy) {
    if (textureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, textureSampler, nullptr);
        textureSampler = VK_NULL_HANDLE;
    }
    createTextureSampler(anisotropy);
}

// vanilla 的降水纹理是 64x256
// 它单独用一张 2D 图像，而不是塞进方形的方块纹理数组
// 缩放进图集层会把竖直方向四个纹素压成一个，细密的雨丝会退回成早期那种粗大的水片
void TextureManager::createRainTexture() {
    const auto pixels = assets::ImageData::loadRgba(*resourceProvider_, assets::textures("environment/rain.png"));
    const auto width = static_cast<std::uint32_t>(pixels.width);
    const auto height = static_cast<std::uint32_t>(pixels.height);
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(pixels.rgba.size());
    rainTextureImage =
        resources_->createImage(width, height, 1U, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->uploadImageLayers(rainTextureImage, pixels.rgba.data(), byteSize, width, height, 1U,
                                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // 单张 2D 图像，不是数组层：雨幕是 64x256 的竖直贴图，见函数头的说明
    rainTextureView = resources_->createImageView(rainTextureImage.image, VK_FORMAT_R8G8B8A8_UNORM,
                                                  VK_IMAGE_ASPECT_COLOR_BIT);
    std::cout << "Loaded vanilla rain texture: " << pixels.width << 'x' << pixels.height << '\n';
}

// 所有被采样的颜色纹理都是 **UNORM**，不是 SRGB。
//
// vanilla 从不做伽马转换：纹素采到什么值就拿什么值乘顶点色、乘亮度、混雾，
// 帧缓冲里存的也正是这些值。本项目的世界着色器逐条转写的就是那套算式，里面的
// 每个系数（面明暗 0.8/0.68/0.5、雾色 0x050533、方块光暖色、生物群系色、羊毛
// 染料色）都是 vanilla 的 sRGB 编码值。用 SRGB 格式的图像意味着采样器先把纹素
// 解码成线性，于是"编码值 × 线性值"——算式没错，两边却不在一个空间：黑羊会渲染
// 成 #5F5F65 而不是 #1D1D21，方块底面是 0xBC 而不是 0x80。
// 采样器返回原始字节，那套转写才成立。
void TextureManager::createGuiTexture() {
    // 26.1 把 HUD 与控件美术拆成一个个具名的 gui/sprites 文件
    // 启动时把本渲染器用到的那一小撮打进兼容层
    // 绘制代码仍用紧凑的像素矩形，而每个源文件仍能按标准资源包的名字被单独覆盖
    const auto tex = [&](std::string_view sub) {
        return assets::ImageData::loadRgbaOrMissing(*resourceProvider_, assets::textures(sub));
    };
    const auto guiTex = [&](std::string_view sub) { return tex("gui/" + std::string{sub}); };
    const auto sprite = [&](std::string_view name) {
        return guiTex("sprites/" + std::string{name} + ".png");
    };

    auto widgets = emptyRgbaAtlas();
    // 按钮和滑条轨道的尺寸由运行期决定，它们的 26.1 `gui.scaling` 附属文件必须一起带进图集
    // 记下每个精灵的落位和解析后的元数据，HUD 才能做九宫格而不是整张位图拉伸
    // 没有附属文件或读不出来的精灵沿用纯拉伸的默认行为
    const auto blitWidget = [&](assets::ImageData& atlas, GuiWidgetSprite id,
                                std::string_view name, int x, int y) {
        const std::string path = "gui/sprites/" + std::string{name} + ".png";
        const auto image = tex(path);
        blit(atlas, image, x, y);
        auto scaling =
            assets::GuiSpriteScaling::load(*resourceProvider_, assets::textures(path));
        // 附属文件可以不写参考尺寸，这时边框就以美术本身的尺寸为基准
        if (scaling.width <= 0) {
            scaling.width = image.width;
        }
        if (scaling.height <= 0) {
            scaling.height = image.height;
        }
        guiWidgetSprites[static_cast<std::size_t>(id)] = GuiAtlasSprite{
            ui::UiRect{static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(image.width), static_cast<float>(image.height)},
            scaling,
        };
    };
    blit(widgets, sprite("hud/hotbar"), 0, 0);
    blit(widgets, sprite("hud/hotbar_selection"), 0, 22);
    blitWidget(widgets, GuiWidgetSprite::ButtonDisabled, "widget/button_disabled", 0, 46);
    blitWidget(widgets, GuiWidgetSprite::Button, "widget/button", 0, 66);
    blitWidget(widgets, GuiWidgetSprite::ButtonHighlighted, "widget/button_highlighted", 0, 86);
    blitWidget(widgets, GuiWidgetSprite::Slider, "widget/slider", 0, 106);
    blit(widgets, sprite("widget/slider_highlighted"), 0, 126);
    blitWidget(widgets, GuiWidgetSprite::SliderHandle, "widget/slider_handle", 0, 146);
    blitWidget(widgets, GuiWidgetSprite::SliderHandleHighlighted,
               "widget/slider_handle_highlighted", 0, 166);

    auto hud = emptyRgbaAtlas();
    blit(hud, sprite("hud/crosshair"), 0, 0);
    blit(hud, sprite("hud/heart/container"), 16, 0);
    blit(hud, sprite("hud/heart/container_blinking"), 25, 0);
    blit(hud, sprite("hud/heart/full"), 52, 0);
    blit(hud, sprite("hud/heart/half"), 61, 0);
    blit(hud, sprite("hud/armor_empty"), 16, 9);
    blit(hud, sprite("hud/armor_half"), 25, 9);
    blit(hud, sprite("hud/armor_full"), 34, 9);
    blit(hud, sprite("hud/air"), 16, 18);
    blit(hud, sprite("hud/air_empty"), 25, 18);
    blit(hud, sprite("hud/food_empty"), 16, 27);
    blit(hud, sprite("hud/food_full"), 52, 27);
    blit(hud, sprite("hud/food_half"), 61, 27);
    blit(hud, sprite("hud/experience_bar_background"), 0, 64);
    blit(hud, sprite("hud/experience_bar_progress"), 0, 69);

    auto tabs = emptyRgbaAtlas();
    // 上排七个页签（建筑方块…战斗）与下排四个页签（食物…背包）
    // 26.1 提供 tab_top_1..7 与 tab_bottom_1..7
    // HUD 按 x = 列号 * 28 采样，与 drawCreativeInventory 的 UV 计算一致
    // 上排未选中与选中分别取 y 0 和 32，下排取 y 64 和 96
    for (int tab = 0; tab < 7; ++tab) {
        const std::string suffix = std::to_string(tab + 1);
        blit(tabs, sprite("container/creative_inventory/tab_top_unselected_" + suffix),
             tab * 28 + 1, 0);
        blit(tabs, sprite("container/creative_inventory/tab_top_selected_" + suffix), tab * 28 + 1,
             32);
    }
    for (int tab = 0; tab < 4; ++tab) {
        const std::string suffix = std::to_string(tab + 1);
        blit(tabs, sprite("container/creative_inventory/tab_bottom_unselected_" + suffix),
             tab * 28 + 1, 64);
        blit(tabs, sprite("container/creative_inventory/tab_bottom_selected_" + suffix),
             tab * 28 + 1, 96);
    }
    blit(tabs, sprite("container/creative_inventory/scroller"), 232, 0);
    blit(tabs, sprite("container/creative_inventory/scroller_disabled"), 244, 0);

    // I-2: 26.1 的提示框底衬是两张九宫格精灵，不是一块纯色矩形——
    // tooltip/background 是 0xF0100010 的填充，tooltip/frame 是一圈 1px 的竖直
    // 渐变边框（#5000FF → #28007F，alpha 0x50）。两张都是 100x100，并排放进
    // 同一层；带着各自的 gui.scaling（背景 border 9，边框 border 10 且
    // stretch_inner）进来，HudRenderer 才能按内容尺寸切片而不是整张拉伸。
    auto tooltipGui = emptyRgbaAtlas();
    blitWidget(tooltipGui, GuiWidgetSprite::TooltipBackground, "tooltip/background", 0, 0);
    blitWidget(tooltipGui, GuiWidgetSprite::TooltipFrame, "tooltip/frame", 100, 0);

    const auto underwater = repeatTileToAtlas(tex("misc/underwater.png"), 256, 256, 4);
    const auto menuBackground = repeatTileToAtlas(guiTex("menu_background.png"), 256, 256, 16);
    const auto menuListBackground =
        repeatTileToAtlas(guiTex("menu_list_background.png"), 256, 256, 16);
    const auto chestGui = singleChestGui(guiTex("container/generic_54.png"));
    auto furnaceGui = guiTex("container/furnace.png");
    blit(furnaceGui, sprite("container/furnace/lit_progress"), 176, 0);
    blit(furnaceGui, sprite("container/furnace/burn_progress"), 176, 14);
    // ENCH-2: the enchanting table's screen. Its 176x166 panel already fills the
    // top-left of the 256x256 file, so the six level numerals (16x16 each) and
    // the three option-bar states (108x19 each) are packed into the space the
    // panel leaves — the numerals in the strip to its right, the bars below it.
    // Every one of them is read from the pack (gui/container/enchanting_table.png
    // + gui/sprites/container/enchanting_table/*), never drawn by hand, so a
    // resource pack that restyles the table restyles this screen.
    auto enchantingGui = guiTex("container/enchanting_table.png");
    for (int level = 0; level < 3; ++level) {
        const std::string suffix = std::to_string(level + 1);
        blit(enchantingGui, sprite("container/enchanting_table/level_" + suffix),
             kEnchantingLevelSpriteX + level * 16, kEnchantingLevelSpriteY);
        blit(enchantingGui, sprite("container/enchanting_table/level_" + suffix + "_disabled"),
             kEnchantingLevelSpriteX + level * 16, kEnchantingLevelSpriteY + 16);
    }
    blit(enchantingGui, sprite("container/enchanting_table/enchantment_slot"), 0,
         kEnchantingBarSpriteY);
    blit(enchantingGui, sprite("container/enchanting_table/enchantment_slot_disabled"), 0,
         kEnchantingBarSpriteY + 20);
    blit(enchantingGui, sprite("container/enchanting_table/enchantment_slot_highlighted"), 0,
         kEnchantingBarSpriteY + 40);
    // ENCH-3: the anvil's screen, packed the same way.
    auto anvilGui = guiTex("container/anvil.png");
    blit(anvilGui, sprite("container/anvil/text_field"), 0, kAnvilTextFieldSpriteY);
    blit(anvilGui, sprite("container/anvil/text_field_disabled"), 0, kAnvilTextFieldSpriteY + 17);
    blit(anvilGui, sprite("container/anvil/error"), kAnvilErrorSpriteX, kAnvilErrorSpriteY);
    // Screen.renderBackground 会在每个游戏内界面上铺一层竖直渐变
    // 顶部为 rgba(0x10,0x10,0x10,0xC0)，底部为 rgba(0x10,0x10,0x10,0xD0)
    // 把它烘成一个 256x256 层，各界面用一次精灵绘制就能拿到与 vanilla 完全一致的底衬
    assets::ImageData screenDimGradient;
    screenDimGradient.width = 256;
    screenDimGradient.height = 256;
    screenDimGradient.rgba.resize(256U * 256U * 4U);
    for (std::uint32_t gradientY = 0U; gradientY < 256U; ++gradientY) {
        const std::uint8_t gradientAlpha =
            static_cast<std::uint8_t>(0xC0U + (0xD0U - 0xC0U) * gradientY / 255U);
        for (std::uint32_t gradientX = 0U; gradientX < 256U; ++gradientX) {
            const std::size_t offset = static_cast<std::size_t>(gradientY * 256U + gradientX) * 4U;
            screenDimGradient.rgba[offset + 0] = 0x10U;
            screenDimGradient.rgba[offset + 1] = 0x10U;
            screenDimGradient.rgba[offset + 2] = 0x10U;
            screenDimGradient.rgba[offset + 3] = gradientAlpha;
        }
    }
    const std::array images{
        widgets,
        hud,
        guiTex("container/inventory.png"),
        guiTex("container/creative_inventory/tab_items.png"),
        tabs,
        guiTex("container/creative_inventory/tab_inventory.png"),
        underwater,
        guiTex("container/crafting_table.png"),
        furnaceGui,
        menuBackground,
        chestGui,
        tex("misc/vignette.png"),
        screenDimGradient,
        menuListBackground,
        enchantingGui,
        anvilGui,
        tooltipGui,
    };
    constexpr std::uint32_t kGuiLayerCount = 17U;
    // 层号是写死在 HudTypes.hpp 里的常量（kTooltipGuiLayer 等），而层内容是上面
    // 这个数组的顺序。加一层却漏改这个数，上传就会按错误的层数切分整块像素，
    // 于是每一层都错位——编译期钉住它。
    static_assert(images.size() == kGuiLayerCount, "GUI atlas layer count must match the images");
    const int width = images.front().width;
    const int height = images.front().height;
    for (const auto& image : images) {
        if (image.width != width || image.height != height) {
            throw std::runtime_error("Minecraft GUI textures must share one size");
        }
    }
    std::vector<std::uint8_t> pixels;
    const std::size_t layerBytes = images.front().rgba.size();
    pixels.reserve(layerBytes * images.size());
    for (const auto& image : images) {
        pixels.insert(pixels.end(), image.rgba.begin(), image.rgba.end());
    }
    const auto byteSize = static_cast<VkDeviceSize>(pixels.size());
    guiTextureImage = resources_->createImage(
        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), kGuiLayerCount,
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->uploadImageLayers(guiTextureImage, pixels.data(), byteSize,
                                  static_cast<std::uint32_t>(width),
                                  static_cast<std::uint32_t>(height), kGuiLayerCount,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    guiTextureView = resources_->createImageView(guiTextureImage.image, VK_FORMAT_R8G8B8A8_UNORM,
                                                 VK_IMAGE_ASPECT_COLOR_BIT, kGuiLayerCount,
                                                 VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    std::cout << "Loaded Minecraft GUI texture array: " << width << 'x' << height << " x "
              << kGuiLayerCount << '\n';
}

// 标题全景面是 1024x1024 的实拍图，因此单独用一个原生分辨率的数组，而不是挤进 256px 的 GUI 数组
// 每个全景面一层
void TextureManager::createPanoramaTexture() {
    std::array<assets::ImageData, kPanoramaFaces> faces{};
    for (std::size_t index = 0; index < kPanoramaFaces; ++index) {
        // 全景面逐文件解析，资源包只覆盖了一部分甚至没覆盖时，其余仍回落到内置资源
        // 这样整个 title/background 目录不会被资源包整体遮蔽
        faces[index] = assets::ImageData::loadRgbaOrMissing(*resourceProvider_,
            assets::textures("gui/title/background/panorama_" + std::to_string(index) + ".png"));
    }
    const int width = faces.front().width;
    const int height = faces.front().height;
    for (const auto& face : faces) {
        if (face.width != width || face.height != height) {
            throw std::runtime_error("Minecraft panorama faces must share one size");
        }
    }
    std::vector<std::uint8_t> pixels;
    const std::size_t layerBytes = faces.front().rgba.size();
    pixels.reserve(layerBytes * kPanoramaFaces);
    for (const auto& face : faces) {
        pixels.insert(pixels.end(), face.rgba.begin(), face.rgba.end());
    }
    const auto byteSize = static_cast<VkDeviceSize>(pixels.size());
    panoramaTextureImage = resources_->createImage(
        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
        static_cast<std::uint32_t>(kPanoramaFaces), VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->uploadImageLayers(panoramaTextureImage, pixels.data(), byteSize,
                                  static_cast<std::uint32_t>(width),
                                  static_cast<std::uint32_t>(height),
                                  static_cast<std::uint32_t>(kPanoramaFaces),
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    panoramaTextureView = resources_->createImageView(
        panoramaTextureImage.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
        static_cast<std::uint32_t>(kPanoramaFaces), VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    std::cout << "Loaded Minecraft title panorama: " << width << 'x' << height << " x "
              << kPanoramaFaces << " faces\n";
}

void TextureManager::createPanoramaSampler() {
    auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    // 全景面是放大的实拍图，需要线性过滤，而不是像素画用的最近邻采样器
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.0F;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &panoramaSampler),
            "vkCreateSampler(panorama)");
}


// 把每个已提供的物种模型与皮肤装进专用 2D 数组纹理（binding 4），按 speciesModels 顺序一物种一层
// 新增生物只需注册一个带渲染描述的 EntityType
// 缺 .png 时回落到程序化皮肤，它走的是着色器采样时同一套 box-UV 映射
void TextureManager::createEntityTextureArray(
    std::vector<gameplay::entities::SpeciesRenderModel>& speciesModels) {
    const auto resourceRoot = resourceProvider_->resourceRoot();
    speciesModels = gameplay::entities::buildSpeciesModels(
        resourceRoot, gameplay::entities::entityTypeRegistry());

    std::uint32_t atlasWidth = 64U;
    std::uint32_t atlasHeight = 64U;
    const auto declaredSize = [&](const animation::SkeletalModel& model) {
        return gameplay::entities::entityTextureSize(
            model, {static_cast<float>(atlasWidth), static_cast<float>(atlasHeight)});
    };
    for (const auto& species : speciesModels) {
        if (!species.loaded) {
            continue;
        }
        const glm::vec2 declared = declaredSize(species.model.model);
        atlasWidth = std::max(atlasWidth, static_cast<std::uint32_t>(declared.x));
        atlasHeight = std::max(atlasHeight, static_cast<std::uint32_t>(declared.y));
    }
    entityTextureWidth = atlasWidth;
    entityTextureHeight = atlasHeight;
    // 分配纹理数组层：每物种一层，声明了第二张皮肤的（羊毛）再多一层
    // 逐骨骼的层号也在这里预先算好，绘制循环因此不必反复比对骨骼名
    // "wool" 前缀的骨骼落到第二层，其余落到身体层
    std::uint32_t nextLayer = 0U;
    for (auto& species : speciesModels) {
        species.textureLayer = static_cast<float>(nextLayer++);
        species.secondaryTextureLayer = -1.0F;
        if (species.loaded && !species.type->render().secondaryTexturePath.empty()) {
            species.secondaryTextureLayer = static_cast<float>(nextLayer++);
        }
        const auto& bones = species.model.model.bones();
        species.boneTextureLayer.assign(bones.size(), species.textureLayer);
        if (species.secondaryTextureLayer >= 0.0F) {
            for (std::size_t b = 0; b < bones.size(); ++b) {
                if (bones[b].name.rfind("wool", 0U) == 0U) {
                    species.boneTextureLayer[b] = species.secondaryTextureLayer;
                }
            }
        }
    }
    const std::uint32_t layerCount = nextLayer;
    std::vector<std::uint8_t> atlas(
        static_cast<std::size_t>(atlasWidth) * atlasHeight * 4U * layerCount, 0U);
    // 把一张皮肤采样进指定数组层，皮肤经资源包栈从 `texturePath` 加载
    // 同时把声明的皮肤尺寸放大到共享图集的尺寸
    const auto blitLayer = [&](std::uint32_t layer, std::string_view texturePath,
                               const animation::SkeletalModel& model) {
        const auto skin = gameplay::entities::buildSpeciesSkin(
            *resourceProvider_, model, texturePath,
            {static_cast<float>(atlasWidth), static_cast<float>(atlasHeight)});
        const glm::vec2 declared = declaredSize(model);
        const std::uint32_t skinWidth = static_cast<std::uint32_t>(declared.x);
        const std::uint32_t skinHeight = static_cast<std::uint32_t>(declared.y);
        for (std::uint32_t layerY = 0; layerY < atlasHeight; ++layerY) {
            const std::uint32_t srcY = std::min(skinHeight - 1U, layerY * skinHeight / atlasHeight);
            for (std::uint32_t layerX = 0; layerX < atlasWidth; ++layerX) {
                const std::uint32_t srcX =
                    std::min(skinWidth - 1U, layerX * skinWidth / atlasWidth);
                const std::size_t src = (static_cast<std::size_t>(srcY) * skinWidth + srcX) * 4U;
                const std::size_t dst =
                    (static_cast<std::size_t>(layer) * atlasWidth * atlasHeight +
                     static_cast<std::size_t>(layerY) * atlasWidth + layerX) *
                    4U;
                std::memcpy(&atlas[dst], &skin[src], 4U);
            }
        }
    };
    for (const auto& species : speciesModels) {
        if (!species.loaded) {
            continue;
        }
        blitLayer(static_cast<std::uint32_t>(species.textureLayer),
                  species.type->render().texturePath, species.model.model);
        if (species.secondaryTextureLayer >= 0.0F) {
            blitLayer(static_cast<std::uint32_t>(species.secondaryTextureLayer),
                      species.type->render().secondaryTexturePath, species.model.model);
        }
    }

    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(atlas.size());
    entityTextureImage =
        resources_->createImage(atlasWidth, atlasHeight, layerCount, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    // 骨骼的层号在顶点阶段选层，因此目标阶段含 VERTEX
    resources_->uploadImageLayers(entityTextureImage, atlas.data(), byteSize, atlasWidth,
                                  atlasHeight, layerCount,
                                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    entityTextureView = resources_->createImageView(entityTextureImage.image,
                                                    VK_FORMAT_R8G8B8A8_UNORM,
                                                    VK_IMAGE_ASPECT_COLOR_BIT, layerCount,
                                                    VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    std::cout << "Loaded entity texture atlas: " << atlasWidth << 'x' << atlasHeight << " x "
              << layerCount << '\n';
}

// 字体放在一个 256x256 的 R8 纹理数组里
// 第 0 层是位图 ASCII provider，其后是其它位图 provider
// 再往后是从 26.1 的 unihex zip 光栅化出来的各页
void TextureManager::createFontTexture(ui::BitmapFontMetrics& fontMetrics, ui::TextFont& textFont,
                                       const std::set<int>& requiredPages, bool forceUnicode) {
    // 字体页逐文件解析，不完整的资源包会回落到内置的 ascii 表与 unicode 页
    // 这样它们不会因为 font/ 目录被整体遮蔽而丢失
    const auto ascii = assets::ImageData::loadRgbaOrMissing(*resourceProvider_, assets::textures("font/ascii.png"));
    fontMetrics = ui::BitmapFontMetrics::fromRgba(ascii.rgba, ascii.width, ascii.height);
    textFont.setAsciiMetrics(fontMetrics);
    textFont.clearUnicodePages();
    textFont.clearBitmapGlyphs();
    textFont.clearSpaceAdvances();
    textFont.setForceUnicode(forceUnicode);

    const auto providers = assets::loadFontProviders(
        *resourceProvider_, forceUnicode ? "minecraft:uniform" : "minecraft:default", forceUnicode,
        false);
    UnihexPages unihex;
    std::vector<BitmapProviderLayer> bitmapLayers;
    for (const auto& provider : providers) {
        if (provider.kind == assets::FontProviderKind::Space) {
            for (const auto& [codepoint, advance] : provider.advances) {
                textFont.setSpaceAdvance(codepoint, advance);
            }
        } else if (provider.kind == assets::FontProviderKind::Bitmap &&
                   provider.file != assets::textures("font/ascii.png")) {
            bitmapLayers.push_back(loadBitmapProvider(*resourceProvider_, provider));
        } else if (provider.kind == assets::FontProviderKind::Unihex) {
            readUnihexArchive(resourceProvider_->readBytes(provider.file), provider,
                              requiredPages, unihex);
        }
    }

    // ENCH-2: the Standard Galactic Alphabet page (`minecraft:alt` ->
    // font/ascii_sga.png). It is not part of the default stack — nothing types
    // galactic — so it is resolved as its own font id and its glyphs are moved
    // into the Private Use Area (ui::galacticCodepoint) before they join the
    // shared map, where they would otherwise collide with the Latin letters they
    // are drawn for. Read from the pack like every other font page: a resource
    // pack that restyles ascii_sga.png restyles the enchanting preview.
    for (const auto& provider :
         assets::loadFontProviders(*resourceProvider_, "minecraft:alt", false, false)) {
        if (provider.kind != assets::FontProviderKind::Bitmap) {
            continue;
        }
        auto galactic = loadBitmapProvider(*resourceProvider_, provider);
        for (auto& [codepoint, glyph] : galactic.glyphs) {
            codepoint = ui::galacticCodepoint(codepoint);
        }
        bitmapLayers.push_back(std::move(galactic));
    }

    constexpr std::uint32_t kFontPageSize = 256U;
    constexpr std::size_t kFontLayerBytes = static_cast<std::size_t>(kFontPageSize) * kFontPageSize;
    std::vector<std::uint8_t> pixels;
    pixels.resize(kFontLayerBytes);
    // 对 128x128 的表做最近邻放大，归一化 UV 和屏幕像素都保持不变
    for (std::uint32_t y = 0; y < kFontPageSize; ++y) {
        for (std::uint32_t x = 0; x < kFontPageSize; ++x) {
            const auto sourceX =
                std::min(x * static_cast<std::uint32_t>(ascii.width) / kFontPageSize,
                         static_cast<std::uint32_t>(ascii.width) - 1U);
            const auto sourceY =
                std::min(y * static_cast<std::uint32_t>(ascii.height) / kFontPageSize,
                         static_cast<std::uint32_t>(ascii.height) - 1U);
            pixels[y * kFontPageSize + x] = ascii.rgba[(static_cast<std::size_t>(sourceY) *
                                                            static_cast<std::size_t>(ascii.width) +
                                                        sourceX) *
                                                           4U +
                                                       3U];
        }
    }

    textFont.setUnicodeSizes(std::move(unihex.sizes));
    std::uint32_t layerCount = 1U;
    for (auto& bitmap : bitmapLayers) {
        for (auto& [codepoint, glyph] : bitmap.glyphs) {
            glyph.layer = static_cast<float>(layerCount);
            textFont.addBitmapGlyph(codepoint, glyph);
        }
        pixels.insert(pixels.end(), bitmap.alpha.begin(), bitmap.alpha.end());
        ++layerCount;
    }
    for (const int page : requiredPages) {
        if (page < 0 || page >= 256 || unihex.alpha[static_cast<std::size_t>(page)].empty()) {
            continue;
        }
        const auto& pagePixels = unihex.alpha[static_cast<std::size_t>(page)];
        pixels.insert(pixels.end(), pagePixels.begin(), pagePixels.end());
        textFont.setUnicodePageLayer(page, static_cast<int>(layerCount));
        ++layerCount;
    }

    const auto byteSize = static_cast<VkDeviceSize>(pixels.size());
    fontTextureImage =
        resources_->createImage(kFontPageSize, kFontPageSize, layerCount, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->uploadImageLayers(fontTextureImage, pixels.data(), byteSize, kFontPageSize,
                                  kFontPageSize, layerCount,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    fontTextureView = resources_->createImageView(fontTextureImage.image, VK_FORMAT_R8_UNORM,
                                                  VK_IMAGE_ASPECT_COLOR_BIT, layerCount,
                                                  VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    std::cout << "Loaded Minecraft font array: " << kFontPageSize << 'x' << kFontPageSize << " x "
              << layerCount << " (" << (1U + bitmapLayers.size()) << " bitmap layers + "
              << (layerCount - 1U - static_cast<std::uint32_t>(bitmapLayers.size()))
              << " unihex pages)\n";
}

void TextureManager::recreateFontTexture(ui::BitmapFontMetrics& fontMetrics, ui::TextFont& textFont,
                                         const std::set<int>& requiredPages, bool forceUnicode) {
    if (fontTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, fontTextureView, nullptr);
        fontTextureView = VK_NULL_HANDLE;
    }
    resources_->destroyImage(fontTextureImage);
    createFontTexture(fontMetrics, textFont, requiredPages, forceUnicode);
}

void TextureManager::destroy(bool allocatorAlive) noexcept {
    if (rainTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, rainTextureView, nullptr);
        rainTextureView = VK_NULL_HANDLE;
    }
    if (guiTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, guiTextureView, nullptr);
        guiTextureView = VK_NULL_HANDLE;
    }
    if (panoramaTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, panoramaTextureView, nullptr);
        panoramaTextureView = VK_NULL_HANDLE;
    }
    if (panoramaSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, panoramaSampler, nullptr);
        panoramaSampler = VK_NULL_HANDLE;
    }
    if (fontTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, fontTextureView, nullptr);
        fontTextureView = VK_NULL_HANDLE;
    }
    if (entityTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, entityTextureView, nullptr);
        entityTextureView = VK_NULL_HANDLE;
    }
    if (textureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, textureSampler, nullptr);
        textureSampler = VK_NULL_HANDLE;
    }
    if (textureView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, textureView, nullptr);
        textureView = VK_NULL_HANDLE;
    }
    if (allocatorAlive) {
        resources_->destroyImage(rainTextureImage);
        resources_->destroyImage(guiTextureImage);
        resources_->destroyImage(panoramaTextureImage);
        resources_->destroyImage(fontTextureImage);
        resources_->destroyImage(entityTextureImage);
        resources_->destroyImage(textureImage);
    }
}

std::size_t TextureManager::residentImageBytes() const {
    const AllocatedImage* const images[] = {
        &textureImage,      &entityTextureImage, &guiTextureImage,  &fontTextureImage,
        &rainTextureImage,  &panoramaTextureImage};
    std::size_t bytes = 0;
    for (const auto* image : images) {
        if (image->allocation != VK_NULL_HANDLE) {
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(allocator_, image->allocation, &info);
            bytes += info.size;
        }
    }
    return bytes;
}

} // namespace mc::render
