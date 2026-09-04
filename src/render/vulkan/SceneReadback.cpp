#include "render/vulkan/SceneReadback.hpp"

#include "render/PreviewImageBytes.hpp"

#include <stb_image_write.h>

#include <cstring>
#include <iostream>
#include <vector>

namespace mc::render {
namespace {

void recordCopyToBuffer(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout currentLayout,
                        VkBuffer destination, std::uint32_t width, std::uint32_t height) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = currentLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                           1, &region);
}

} // namespace

bool writeSceneImagePng(const VulkanResources& resources, VkDevice device, VkImage sceneImage,
                        VkFormat sceneFormat, VkImageLayout currentLayout, std::uint32_t width,
                        std::uint32_t height, const std::filesystem::path& file) {
    static_cast<void>(device);
    if (width == 0U || height == 0U) {
        std::cerr << "Scene readback: the frame has no size\n";
        return false;
    }
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4U;
    AllocatedBuffer staging =
        resources.createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, /*hostVisible=*/true);
    if (staging.buffer == VK_NULL_HANDLE || staging.mapped == nullptr) {
        std::cerr << "Scene readback: could not allocate a host-visible staging buffer\n";
        return false;
    }

    const auto commandBuffer = resources.beginSingleUseCommands();
    recordCopyToBuffer(commandBuffer, sceneImage, currentLayout, staging.buffer, width, height);
    // endSingleUseCommands waits on the queue, so the bytes are readable when it
    // returns. That is heavy-handed for a per-frame path and exactly right for an
    // offline one that writes eight files and exits.
    resources.endSingleUseCommands(commandBuffer);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(byteSize));
    std::memcpy(pixels.data(), staging.mapped, pixels.size());
    resources.destroyBuffer(staging);

    // The channel swap and the opaque alpha both live in PreviewImageBytes.hpp,
    // where they can be tested without a GPU — see the note at the top of that
    // header for why that particular piece is worth separating.
    static_assert(kFormatB8G8R8A8Unorm == static_cast<std::uint32_t>(VK_FORMAT_B8G8R8A8_UNORM));
    static_assert(kFormatB8G8R8A8Srgb == static_cast<std::uint32_t>(VK_FORMAT_B8G8R8A8_SRGB));
    normalizePreviewPixels(pixels,
                           readbackNeedsRedBlueSwap(static_cast<std::uint32_t>(sceneFormat)));

    std::error_code error;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            std::cerr << "Scene readback: cannot create " << file.parent_path().string() << ": "
                      << error.message() << "\n";
            return false;
        }
    }
    const int written =
        stbi_write_png(file.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                       pixels.data(), static_cast<int>(width) * 4);
    if (written == 0) {
        std::cerr << "Scene readback: stbi_write_png failed for " << file.string() << "\n";
        return false;
    }
    return true;
}

} // namespace mc::render
