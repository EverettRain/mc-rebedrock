#include "render/vulkan/TextureManager.hpp"

#include "render/vulkan/BlockAtlasBaker.hpp"

#include "animation/SkeletalModel.hpp"
#include "assets/ImageData.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/LayeredBiomeSource.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
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
    const auto pixels = bakeBlockAtlas(blockTextureRoot_);
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
    resources_->transitionTextureImage(textureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
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
    samplerInfo.anisotropyEnable =
        anisotropySupported_ && anisotropy > 1 ? VK_TRUE : VK_FALSE;
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
    const auto pixels = assets::ImageData::loadRgba(blockTextureRoot_.parent_path() /
                                                    "environment" / "rain.png");
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
    resources_->transitionTextureImage(rainTextureImage, 1U, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
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
    const auto guiRoot = blockTextureRoot_.parent_path() / "gui";
    const auto underwater = repeatTileToAtlas(
        assets::ImageData::loadRgba(guiRoot.parent_path() / "misc" / "underwater.png"), 256, 256, 4);
    const auto loadingDirt = repeatTileToAtlas(
        assets::ImageData::loadRgba(guiRoot / "options_background.png"), 256, 256, 8);
    const auto chestGui =
        singleChestGui(assets::ImageData::loadRgba(guiRoot / "container" / "generic_54.png"));
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
        assets::ImageData::loadRgba(guiRoot / "widgets.png"),
        assets::ImageData::loadRgba(guiRoot / "icons.png"),
        assets::ImageData::loadRgba(guiRoot / "container" / "inventory.png"),
        assets::ImageData::loadRgba(guiRoot / "container" / "creative_inventory" / "tab_items.png"),
        assets::ImageData::loadRgba(guiRoot / "container" / "creative_inventory" / "tabs.png"),
        assets::ImageData::loadRgba(guiRoot / "container" / "creative_inventory" /
                                    "tab_inventory.png"),
        underwater,
        assets::ImageData::loadRgba(guiRoot / "container" / "crafting_table.png"),
        assets::ImageData::loadRgba(guiRoot / "container" / "furnace.png"),
        loadingDirt,
        chestGui,
        assets::ImageData::loadRgba(guiRoot.parent_path() / "misc" / "vignette.png"),
        screenDimGradient,
    };
    constexpr std::uint32_t kGuiLayerCount = 13U;
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
    resources_->transitionTextureImage(guiTextureImage, kGuiLayerCount, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT);

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
    const auto guiRoot = blockTextureRoot_.parent_path() / "gui";
    const auto backgroundRoot = guiRoot / "title" / "background";
    std::array<assets::ImageData, kPanoramaFaces> faces{};
    for (std::size_t index = 0; index < kPanoramaFaces; ++index) {
        faces[index] = assets::ImageData::loadRgba(backgroundRoot /
                                                   ("panorama_" + std::to_string(index) + ".png"));
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
    const auto grassColormap =
        assets::ImageData::loadRgba(blockTextureRoot_.parent_path() / "colormap" / "grass.png");
    const auto foliageColormap =
        assets::ImageData::loadRgba(blockTextureRoot_.parent_path() / "colormap" / "foliage.png");
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
                grassPixels.push_back(
                    static_cast<std::uint8_t>((grassColor >> (channel * 8U)) & 0xFFU));
                foliagePixels.push_back(
                    static_cast<std::uint8_t>((foliageColor >> (channel * 8U)) & 0xFFU));
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
        resources_->transitionTextureImage(image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                           VK_ACCESS_TRANSFER_WRITE_BIT,
                                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
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
    const auto resourceRoot =
        blockTextureRoot_.parent_path().parent_path().parent_path().parent_path().parent_path();
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
            resourceRoot, blockTextureRoot_.parent_path(), species.model.model,
            species.type->render().texturePath,
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
    resources_->transitionTextureImage(entityTextureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT);
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

std::vector<std::uint8_t> TextureManager::loadGlyphSizes() const {
    const auto path = blockTextureRoot_.parent_path().parent_path().parent_path() / "fonts" /
                      "minecraft" / "glyph_sizes.bin";
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        std::cout << "No glyph_sizes.bin at " << path << "; unicode text is unavailable\n";
        return {};
    }
    std::vector<std::uint8_t> sizes(0x10000U);
    input.read(reinterpret_cast<char*>(sizes.data()), static_cast<std::streamsize>(sizes.size()));
    if (input.gcount() != static_cast<std::streamsize>(sizes.size())) {
        std::cout << "glyph_sizes.bin is truncated; unicode text is unavailable\n";
        return {};
    }
    return sizes;
}

// The font lives in one 256x256 R8 texture array: layer 0 is ascii.png upscaled
// so every layer shares a size, and each following layer is one legacy unicode
// page the active language needs (requiredPages, computed by the renderer).
void TextureManager::createFontTexture(ui::BitmapFontMetrics& fontMetrics, ui::TextFont& textFont,
                                       const std::set<int>& requiredPages, bool forceUnicode) {
    const auto fontRoot = blockTextureRoot_.parent_path() / "font";
    const auto ascii = assets::ImageData::loadRgba(fontRoot / "ascii.png");
    fontMetrics = ui::BitmapFontMetrics::fromRgba(ascii.rgba, ascii.width, ascii.height);
    textFont.setAsciiMetrics(fontMetrics);
    textFont.clearUnicodePages();
    textFont.setForceUnicode(forceUnicode);

    constexpr std::uint32_t kFontPageSize = 256U;
    constexpr std::size_t kFontLayerBytes = static_cast<std::size_t>(kFontPageSize) * kFontPageSize;
    std::vector<std::uint8_t> pixels;
    pixels.resize(kFontLayerBytes);
    // Nearest-neighbour upscale of the 128x128 sheet keeps its normalized UVs
    // and its on-screen pixels identical.
    for (std::uint32_t y = 0; y < kFontPageSize; ++y) {
        for (std::uint32_t x = 0; x < kFontPageSize; ++x) {
            const auto sourceX = std::min(x * static_cast<std::uint32_t>(ascii.width) / kFontPageSize,
                                          static_cast<std::uint32_t>(ascii.width) - 1U);
            const auto sourceY =
                std::min(y * static_cast<std::uint32_t>(ascii.height) / kFontPageSize,
                         static_cast<std::uint32_t>(ascii.height) - 1U);
            pixels[y * kFontPageSize + x] =
                ascii.rgba[(static_cast<std::size_t>(sourceY) *
                                static_cast<std::size_t>(ascii.width) +
                            sourceX) *
                               4U +
                           3U];
        }
    }

    // glyph_sizes.bin gives the used column range of every BMP codepoint;
    // without it the unicode pages are skipped entirely.
    textFont.setUnicodeSizes(loadGlyphSizes());
    std::uint32_t layerCount = 1U;
    for (const int page : textFont.hasUnicodePages() ? requiredPages : std::set<int>{}) {
        std::ostringstream name;
        name << "unicode_page_" << std::hex << std::setfill('0') << std::setw(2) << page << ".png";
        const auto path = fontRoot / name.str();
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            continue;
        }
        try {
            const auto image = assets::ImageData::loadRgba(path);
            if (image.width != static_cast<int>(kFontPageSize) ||
                image.height != static_cast<int>(kFontPageSize)) {
                continue;
            }
            const std::size_t offset = pixels.size();
            pixels.resize(offset + kFontLayerBytes);
            for (std::size_t index = 0; index < kFontLayerBytes; ++index) {
                pixels[offset + index] = image.rgba[index * 4U + 3U];
            }
            textFont.setUnicodePageLayer(page, static_cast<int>(layerCount));
            ++layerCount;
        } catch (const std::exception&) {
            // A missing or damaged page just falls back to the ASCII sheet.
        }
    }

    const auto byteSize = static_cast<VkDeviceSize>(pixels.size());
    auto staging = resources_->createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, pixels.data(), pixels.size());
    checkVk(vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(font staging)");
    fontTextureImage =
        resources_->createImage(kFontPageSize, kFontPageSize, layerCount, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    resources_->transitionTextureImage(fontTextureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT);

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
    resources_->transitionTextureImage(fontTextureImage, layerCount,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
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
              << layerCount << " (ascii + " << (layerCount - 1U) << " unicode pages)\n";
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
