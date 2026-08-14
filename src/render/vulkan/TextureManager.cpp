#include "render/vulkan/TextureManager.hpp"

#include "render/vulkan/BlockAtlasBaker.hpp"

#include "animation/SkeletalModel.hpp"
#include "assets/FontProviders.hpp"
#include "assets/ImageData.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/LayeredBiomeSource.hpp"

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

// The six 1.16.1 title-screen panorama faces, one array layer each.
constexpr std::size_t kPanoramaFaces = 6U;

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

// The unihex font ships as a zip *inside* the pack, so this reads the archive's
// bytes and opens miniz over memory — a zipped pack would otherwise have to
// extract the whole font archive to disk just to be opened again.
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
    // GenericContainerScreen stitches the upper chest rows to the lower
    // player-inventory region. Bake the three-row variant into one atlas layer.
    copyRows(0, 0, 71);
    copyRows(126, 71, 96);
    return result;
}

} // namespace

void TextureManager::createTextureArray(int anisotropy) {
    const auto pixels = bakeBlockAtlas(*resourceProvider_);
    fluidAnimationFrameTimes = pixels.fluidAnimationFrameTimes;
    const auto byteSize = static_cast<VkDeviceSize>(pixels.rgba.size());
    // The atlas layer count is whatever the name-driven build produced (fixed
    // special section + block textures + item icons), so it is derived from the
    // bytes rather than a compile-time constant.
    const auto layerSize =
        static_cast<VkDeviceSize>(pixels.width) * static_cast<VkDeviceSize>(pixels.height) * 4U;
    if (byteSize % layerSize != 0U) {
        throw std::runtime_error("Block texture array data is not whole layers");
    }
    const std::uint32_t layerCount = static_cast<std::uint32_t>(byteSize / layerSize);
    auto staging = resources_->createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, pixels.rgba.data(), pixels.rgba.size());
    checkVk(vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(texture staging)");
    textureImage =
        resources_->createImage(pixels.width, pixels.height, layerCount, VK_FORMAT_R8G8B8A8_SRGB,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->transitionTextureImage(
        textureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    std::vector<VkBufferImageCopy> regions(layerCount);
    for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
        regions[layer].bufferOffset = layerSize * layer;
        regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[layer].imageSubresource.mipLevel = 0;
        regions[layer].imageSubresource.baseArrayLayer = layer;
        regions[layer].imageSubresource.layerCount = 1;
        regions[layer].imageExtent = {pixels.width, pixels.height, 1};
    }
    const auto commandBuffer = resources_->beginSingleUseCommands();
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, textureImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions.size()), regions.data());
    resources_->endSingleUseCommands(commandBuffer);
    resources_->transitionTextureImage(
        textureImage, layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    resources_->destroyBuffer(staging);

    auto viewInfo = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    viewInfo.image = textureImage.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = layerCount;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &textureView),
            "vkCreateImageView(texture)");

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

// The vanilla precipitation texture is 64x256. Keep it in a dedicated 2D image
// instead of the square block-texture array: resizing it into an atlas layer
// would crush four vertical texels into one and turn its fine rain streaks back
// into the broad water cards this renderer used previously.
void TextureManager::createRainTexture() {
    const auto pixels = assets::ImageData::loadRgba(*resourceProvider_, assets::textures("environment/rain.png"));
    const auto width = static_cast<std::uint32_t>(pixels.width);
    const auto height = static_cast<std::uint32_t>(pixels.height);
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(pixels.rgba.size());
    auto staging = resources_->createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, pixels.rgba.data(), pixels.rgba.size());
    checkVk(vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(rain texture)");

    rainTextureImage =
        resources_->createImage(width, height, 1U, VK_FORMAT_R8G8B8A8_SRGB,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->transitionTextureImage(
        rainTextureImage, 1U, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1U;
    region.imageExtent = {width, height, 1U};
    const auto commandBuffer = resources_->beginSingleUseCommands();
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, rainTextureImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);
    resources_->endSingleUseCommands(commandBuffer);
    resources_->transitionTextureImage(
        rainTextureImage, 1U, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    resources_->destroyBuffer(staging);
    rainTextureView = resources_->createImageView(rainTextureImage.image, VK_FORMAT_R8G8B8A8_SRGB,
                                                  VK_IMAGE_ASPECT_COLOR_BIT);
    std::cout << "Loaded vanilla rain texture: " << pixels.width << 'x' << pixels.height << '\n';
}

void TextureManager::createGuiTexture() {
    // 26.1 exposes HUD and widget art as named gui/sprites files. Pack the small
    // set this renderer uses into compatibility layers at startup; draw code
    // retains compact pixel rectangles while every source remains independently
    // overrideable by its standard resource-pack name.
    const auto tex = [&](std::string_view sub) {
        return assets::ImageData::loadRgbaOrMissing(*resourceProvider_, assets::textures(sub));
    };
    const auto guiTex = [&](std::string_view sub) { return tex("gui/" + std::string{sub}); };
    const auto sprite = [&](std::string_view name) {
        return guiTex("sprites/" + std::string{name} + ".png");
    };

    auto widgets = emptyRgbaAtlas();
    // Buttons and slider tracks are the elements drawn at runtime-decided
    // sizes, so their 26.1 `gui.scaling` sidecar has to survive the trip into
    // the atlas: record where each landed alongside its parsed metadata and the
    // HUD can nine-slice it instead of stretching the whole bitmap. A sprite
    // whose sidecar is absent or unreadable keeps the plain-stretch default,
    // which is exactly the behaviour these draws had before.
    const auto blitWidget = [&](GuiWidgetSprite id, std::string_view name, int x, int y) {
        const std::string path = "gui/sprites/" + std::string{name} + ".png";
        const auto image = tex(path);
        blit(widgets, image, x, y);
        auto scaling =
            assets::GuiSpriteScaling::load(*resourceProvider_, assets::textures(path));
        // A sidecar may omit the reference size; the art's own size is then what
        // the borders are measured against.
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
    blitWidget(GuiWidgetSprite::ButtonDisabled, "widget/button_disabled", 0, 46);
    blitWidget(GuiWidgetSprite::Button, "widget/button", 0, 66);
    blitWidget(GuiWidgetSprite::ButtonHighlighted, "widget/button_highlighted", 0, 86);
    blitWidget(GuiWidgetSprite::Slider, "widget/slider", 0, 106);
    blit(widgets, sprite("widget/slider_highlighted"), 0, 126);
    blitWidget(GuiWidgetSprite::SliderHandle, "widget/slider_handle", 0, 146);
    blitWidget(GuiWidgetSprite::SliderHandleHighlighted, "widget/slider_handle_highlighted", 0,
               166);

    auto hud = emptyRgbaAtlas();
    blit(hud, sprite("hud/crosshair"), 0, 0);
    blit(hud, sprite("hud/heart/container"), 16, 0);
    blit(hud, sprite("hud/heart/container_blinking"), 25, 0);
    blit(hud, sprite("hud/heart/full"), 52, 0);
    blit(hud, sprite("hud/heart/half"), 61, 0);
    blit(hud, sprite("hud/air"), 16, 18);
    blit(hud, sprite("hud/air_empty"), 25, 18);
    blit(hud, sprite("hud/food_empty"), 16, 27);
    blit(hud, sprite("hud/food_full"), 52, 27);
    blit(hud, sprite("hud/food_half"), 61, 27);
    blit(hud, sprite("hud/experience_bar_background"), 0, 64);
    blit(hud, sprite("hud/experience_bar_progress"), 0, 69);

    auto tabs = emptyRgbaAtlas();
    for (int tab = 0; tab < 6; ++tab) {
        const std::string suffix = std::to_string(tab + 1);
        blit(tabs, sprite("container/creative_inventory/tab_top_unselected_" + suffix),
             tab * 28 + 1, 0);
        blit(tabs, sprite("container/creative_inventory/tab_top_selected_" + suffix), tab * 28 + 1,
             32);
    }
    for (int tab = 0; tab < 2; ++tab) {
        const std::string suffix = std::to_string(tab + 6);
        blit(tabs, sprite("container/creative_inventory/tab_bottom_unselected_" + suffix),
             tab * 28 + 1, 64);
        blit(tabs, sprite("container/creative_inventory/tab_bottom_selected_" + suffix),
             tab * 28 + 1, 96);
    }
    blit(tabs, sprite("container/creative_inventory/scroller"), 232, 0);
    blit(tabs, sprite("container/creative_inventory/scroller_disabled"), 244, 0);

    const auto underwater = repeatTileToAtlas(tex("misc/underwater.png"), 256, 256, 4);
    const auto menuBackground = repeatTileToAtlas(guiTex("menu_background.png"), 256, 256, 16);
    const auto menuListBackground =
        repeatTileToAtlas(guiTex("menu_list_background.png"), 256, 256, 16);
    const auto chestGui = singleChestGui(guiTex("container/generic_54.png"));
    auto furnaceGui = guiTex("container/furnace.png");
    blit(furnaceGui, sprite("container/furnace/lit_progress"), 176, 0);
    blit(furnaceGui, sprite("container/furnace/burn_progress"), 176, 14);
    // 1.16.1's Screen.renderBackground paints a vertical gradient from
    // rgba(0x10,0x10,0x10,0xC0) at the top to rgba(0x10,0x10,0x10,0xD0) at the
    // bottom over every in-game screen. Bake that into a 256x256 layer so each
    // screen draws the exact vanilla backdrop with one sprite.
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
    };
    constexpr std::uint32_t kGuiLayerCount = 14U;
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
    auto staging = resources_->createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, pixels.data(), pixels.size());
    checkVk(vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(gui staging)");
    guiTextureImage = resources_->createImage(
        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), kGuiLayerCount,
        VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->transitionTextureImage(
        guiTextureImage, kGuiLayerCount, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    std::array<VkBufferImageCopy, kGuiLayerCount> regions{};
    for (std::uint32_t layer = 0; layer < kGuiLayerCount; ++layer) {
        regions[layer].bufferOffset = static_cast<VkDeviceSize>(layerBytes) * layer;
        regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[layer].imageSubresource.baseArrayLayer = layer;
        regions[layer].imageSubresource.layerCount = 1;
        regions[layer].imageExtent = {
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            1,
        };
    }
    const auto commandBuffer = resources_->beginSingleUseCommands();
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, guiTextureImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions.size()), regions.data());
    resources_->endSingleUseCommands(commandBuffer);
    resources_->transitionTextureImage(
        guiTextureImage, kGuiLayerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    resources_->destroyBuffer(staging);

    auto viewInfo = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    viewInfo.image = guiTextureImage.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = kGuiLayerCount;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &guiTextureView),
            "vkCreateImageView(gui)");
    std::cout << "Loaded Minecraft GUI texture array: " << width << 'x' << height << " x "
              << kGuiLayerCount << '\n';
}

// The title-screen panorama faces are 1024x1024 photographs, so they get their
// own array at native resolution instead of a layer in the 256px GUI array. One
// layer per 1.16.1 panorama face.
void TextureManager::createPanoramaTexture() {
    std::array<assets::ImageData, kPanoramaFaces> faces{};
    for (std::size_t index = 0; index < kPanoramaFaces; ++index) {
        // Resolved per file so a pack overriding some panorama faces (or none)
        // still falls back to the bundled ones rather than the whole title/
        // background folder being shadowed by the pack.
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
    auto staging = resources_->createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, pixels.data(), pixels.size());
    checkVk(vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(panorama staging)");
    panoramaTextureImage = resources_->createImage(
        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
        static_cast<std::uint32_t>(kPanoramaFaces), VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->transitionTextureImage(
        panoramaTextureImage, static_cast<std::uint32_t>(kPanoramaFaces), VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    std::array<VkBufferImageCopy, kPanoramaFaces> regions{};
    for (std::size_t layer = 0; layer < kPanoramaFaces; ++layer) {
        regions[layer].bufferOffset = static_cast<VkDeviceSize>(layerBytes) * layer;
        regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[layer].imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(layer);
        regions[layer].imageSubresource.layerCount = 1;
        regions[layer].imageExtent = {static_cast<std::uint32_t>(width),
                                      static_cast<std::uint32_t>(height), 1};
    }
    const auto commandBuffer = resources_->beginSingleUseCommands();
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, panoramaTextureImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions.size()), regions.data());
    resources_->endSingleUseCommands(commandBuffer);
    resources_->transitionTextureImage(
        panoramaTextureImage, static_cast<std::uint32_t>(kPanoramaFaces),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    resources_->destroyBuffer(staging);
    auto viewInfo = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    viewInfo.image = panoramaTextureImage.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = static_cast<std::uint32_t>(kPanoramaFaces);
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &panoramaTextureView),
            "vkCreateImageView(panorama)");
    std::cout << "Loaded Minecraft title panorama: " << width << 'x' << height << " x "
              << kPanoramaFaces << " faces\n";
}

void TextureManager::createPanoramaSampler() {
    auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    // Panorama faces are upscaled photographs, so they need linear filtering
    // instead of the nearest-neighbour pixel-art sampler.
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

void TextureManager::createBiomeTextureResources() {
    const std::uint32_t size = static_cast<std::uint32_t>(kBiomeTextureSize);
    biomeGrassImage =
        resources_->createImage(size, size, 1, VK_FORMAT_R8G8B8A8_SRGB,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    biomeFoliageImage =
        resources_->createImage(size, size, 1, VK_FORMAT_R8G8B8A8_SRGB,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    // Linear filtering turns the per-cell biome colours into a smooth,
    // hardware-interpolated per-pixel gradient across biome boundaries.
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0F;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &biomeSampler),
            "vkCreateSampler(biome)");
    biomeGrassView = resources_->createImageView(biomeGrassImage.image, VK_FORMAT_R8G8B8A8_SRGB,
                                                 VK_IMAGE_ASPECT_COLOR_BIT);
    biomeFoliageView = resources_->createImageView(biomeFoliageImage.image, VK_FORMAT_R8G8B8A8_SRGB,
                                                   VK_IMAGE_ASPECT_COLOR_BIT);
}

// Regenerates the biome colour lookup textures for a world seed: each texel
// holds the grass/foliage colour (from the vanilla colour maps, with the
// swamp/dark-forest overrides) of the biome at that 4-block cell. The fragment
// shader samples them with linear filtering, so adjacent cells blend into a
// per-pixel gradient — the GPU-side stand-in for 1.16.1's per-vertex
// BiomeColors.
void TextureManager::updateBiomeColorTextures(std::uint64_t seed) {
    constexpr int size = kBiomeTextureSize;
    constexpr int span = kBiomeTextureBlockSpan;
    constexpr int halfBlocks = size * span / 2;
    const auto grassColormap = assets::ImageData::loadRgba(*resourceProvider_, assets::textures("colormap/grass.png"));
    const auto foliageColormap = assets::ImageData::loadRgba(*resourceProvider_, assets::textures("colormap/foliage.png"));
    const auto colorAt = [](const assets::ImageData& colormap, float temperature,
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
    world::gen::LayeredBiomeSource biomes{seed};
    std::vector<std::uint8_t> grassPixels;
    std::vector<std::uint8_t> foliagePixels;
    grassPixels.reserve(static_cast<std::size_t>(size) * size * 4U);
    foliagePixels.reserve(static_cast<std::size_t>(size) * size * 4U);
    for (int texelZ = 0; texelZ < size; ++texelZ) {
        for (int texelX = 0; texelX < size; ++texelX) {
            const int worldX = texelX * span - halfBlocks;
            const int worldZ = texelZ * span - halfBlocks;
            const auto biome = biomes.sample(worldX >> 2, worldZ >> 2);
            const auto& definition = world::gen::biomeDefinition(biome);
            std::uint32_t grassColor =
                colorAt(grassColormap, definition.temperature, definition.downfall);
            if (biome == world::gen::Biome::DarkForest) {
                grassColor = ((grassColor & 0xFEFEFEU) + 0x28340AU) >> 1U;
            }
            std::uint32_t foliageColor =
                colorAt(foliageColormap, definition.temperature, definition.downfall);
            if (biome == world::gen::Biome::Swamp) {
                foliageColor = 0x6A7039U;
            }
            for (int channel = 2; channel >= 0; --channel) {
                const auto shift = static_cast<std::uint32_t>(channel) * 8U;
                grassPixels.push_back(static_cast<std::uint8_t>((grassColor >> shift) & 0xFFU));
                foliagePixels.push_back(static_cast<std::uint8_t>((foliageColor >> shift) & 0xFFU));
            }
            grassPixels.push_back(255U);
            foliagePixels.push_back(255U);
        }
    }
    const auto upload = [&](AllocatedImage& image, const std::vector<std::uint8_t>& pixels) {
        const VkDeviceSize byteSize = pixels.size();
        auto staging = resources_->createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, pixels.data(), static_cast<std::size_t>(byteSize));
        checkVk(vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(biome texture)");
        resources_->transitionTextureImage(
            image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size),
                              1};
        const auto commandBuffer = resources_->beginSingleUseCommands();
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        resources_->endSingleUseCommands(commandBuffer);
        resources_->transitionTextureImage(image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        resources_->destroyBuffer(staging);
    };
    upload(biomeGrassImage, grassPixels);
    upload(biomeFoliageImage, foliagePixels);
}

// Loads every shipped species' model + skin into a dedicated 2D-array texture
// (binding 4), one layer per species in speciesModels order. A new creature only
// has to register an EntityType with a render descriptor; missing .png files
// fall back to a procedural skin painted through the same box-UV mapping the
// shader samples with.
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
    const std::uint32_t layerCount = static_cast<std::uint32_t>(speciesModels.size());
    std::vector<std::uint8_t> atlas(
        static_cast<std::size_t>(atlasWidth) * atlasHeight * 4U * layerCount, 0U);
    for (std::size_t index = 0; index < speciesModels.size(); ++index) {
        const auto& species = speciesModels[index];
        if (!species.loaded) {
            continue;
        }
        const auto skin = gameplay::entities::buildSpeciesSkin(
            *resourceProvider_, species.model.model, species.type->render().texturePath,
            {static_cast<float>(atlasWidth), static_cast<float>(atlasHeight)});
        const glm::vec2 declared = declaredSize(species.model.model);
        const std::uint32_t skinWidth = static_cast<std::uint32_t>(declared.x);
        const std::uint32_t skinHeight = static_cast<std::uint32_t>(declared.y);
        for (std::uint32_t layerY = 0; layerY < atlasHeight; ++layerY) {
            const std::uint32_t srcY = std::min(skinHeight - 1U, layerY * skinHeight / atlasHeight);
            for (std::uint32_t layerX = 0; layerX < atlasWidth; ++layerX) {
                const std::uint32_t srcX =
                    std::min(skinWidth - 1U, layerX * skinWidth / atlasWidth);
                const std::size_t src = (static_cast<std::size_t>(srcY) * skinWidth + srcX) * 4U;
                const std::size_t dst =
                    (index * atlasWidth * atlasHeight + layerY * atlasWidth + layerX) * 4U;
                std::memcpy(&atlas[dst], &skin[src], 4U);
            }
        }
    }

    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(atlas.size());
    auto staging = resources_->createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, atlas.data(), atlas.size());
    checkVk(vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(entity staging)");
    entityTextureImage =
        resources_->createImage(atlasWidth, atlasHeight, layerCount, VK_FORMAT_R8G8B8A8_SRGB,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->transitionTextureImage(
        entityTextureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    std::vector<VkBufferImageCopy> regions(layerCount);
    for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
        regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[layer].imageSubresource.baseArrayLayer = layer;
        regions[layer].imageSubresource.layerCount = 1U;
        regions[layer].imageExtent = {atlasWidth, atlasHeight, 1U};
        regions[layer].bufferOffset =
            static_cast<VkDeviceSize>(layer) * atlasWidth * atlasHeight * 4U;
    }
    const auto commandBuffer = resources_->beginSingleUseCommands();
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, entityTextureImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount, regions.data());
    resources_->endSingleUseCommands(commandBuffer);
    resources_->transitionTextureImage(
        entityTextureImage, layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    resources_->destroyBuffer(staging);

    auto viewInfo = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    viewInfo.image = entityTextureImage.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = layerCount;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &entityTextureView),
            "vkCreateImageView(entity)");
    std::cout << "Loaded entity texture atlas: " << atlasWidth << 'x' << atlasHeight << " x "
              << layerCount << '\n';
}

// The font lives in one 256x256 R8 texture array. Layer 0 is the bitmap ASCII
// provider, followed by any additional bitmap providers and then pages
// rasterized from 26.1's unihex zip.
void TextureManager::createFontTexture(ui::BitmapFontMetrics& fontMetrics, ui::TextFont& textFont,
                                       const std::set<int>& requiredPages, bool forceUnicode) {
    // Font pages resolve per file so an incomplete pack falls back to the
    // bundled ascii sheet and unicode pages rather than losing them to a
    // shadowed font/ folder.
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

    constexpr std::uint32_t kFontPageSize = 256U;
    constexpr std::size_t kFontLayerBytes = static_cast<std::size_t>(kFontPageSize) * kFontPageSize;
    std::vector<std::uint8_t> pixels;
    pixels.resize(kFontLayerBytes);
    // Nearest-neighbour upscale of the 128x128 sheet keeps its normalized UVs
    // and its on-screen pixels identical.
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
    auto staging = resources_->createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, pixels.data(), pixels.size());
    checkVk(vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(font staging)");
    fontTextureImage =
        resources_->createImage(kFontPageSize, kFontPageSize, layerCount, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->transitionTextureImage(
        fontTextureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    std::vector<VkBufferImageCopy> regions(layerCount);
    for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
        regions[layer].bufferOffset = static_cast<VkDeviceSize>(kFontLayerBytes) * layer;
        regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[layer].imageSubresource.baseArrayLayer = layer;
        regions[layer].imageSubresource.layerCount = 1;
        regions[layer].imageExtent = {kFontPageSize, kFontPageSize, 1};
    }
    const auto commandBuffer = resources_->beginSingleUseCommands();
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, fontTextureImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions.size()), regions.data());
    resources_->endSingleUseCommands(commandBuffer);
    resources_->transitionTextureImage(
        fontTextureImage, layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    resources_->destroyBuffer(staging);

    auto viewInfo = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    viewInfo.image = fontTextureImage.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = layerCount;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &fontTextureView),
            "vkCreateImageView(font)");
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
    if (biomeGrassView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, biomeGrassView, nullptr);
        biomeGrassView = VK_NULL_HANDLE;
    }
    if (biomeFoliageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, biomeFoliageView, nullptr);
        biomeFoliageView = VK_NULL_HANDLE;
    }
    if (biomeSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, biomeSampler, nullptr);
        biomeSampler = VK_NULL_HANDLE;
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
        resources_->destroyImage(biomeGrassImage);
        resources_->destroyImage(biomeFoliageImage);
        resources_->destroyImage(fontTextureImage);
        resources_->destroyImage(entityTextureImage);
        resources_->destroyImage(textureImage);
    }
}

} // namespace mc::render
