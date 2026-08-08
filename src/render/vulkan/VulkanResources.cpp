#include "render/vulkan/VulkanResources.hpp"

#include <array>
#include <cstdint>

namespace mc::render {

AllocatedBuffer VulkanResources::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                              bool hostVisible) const {
    auto bufferInfo = vkStructure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage =
        hostVisible ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (hostVisible) {
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    AllocatedBuffer result;
    VmaAllocationInfo resultInfo{};
    checkVk(vmaCreateBuffer(allocator_, &bufferInfo, &allocationInfo, &result.buffer,
                            &result.allocation, &resultInfo),
            "vmaCreateBuffer");
    result.mapped = resultInfo.pMappedData;
    return result;
}

AllocatedImage VulkanResources::createImage(std::uint32_t width, std::uint32_t height,
                                            std::uint32_t layers, VkFormat format,
                                            VkImageUsageFlags usage,
                                            VkSampleCountFlagBits samples) const {
    auto imageInfo = vkStructure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layers;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = samples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    AllocatedImage result;
    checkVk(vmaCreateImage(allocator_, &imageInfo, &allocationInfo, &result.image,
                           &result.allocation, nullptr),
            "vmaCreateImage");
    return result;
}

void VulkanResources::destroyBuffer(AllocatedBuffer& buffer) const noexcept {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
        buffer = {};
    }
}

void VulkanResources::destroyImage(AllocatedImage& image) const noexcept {
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image.image, image.allocation);
        image = {};
    }
}

VkImageView VulkanResources::createImageView(VkImage image, VkFormat format,
                                             VkImageAspectFlags aspect) const {
    auto info = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    checkVk(vkCreateImageView(device_, &info, nullptr, &view), "vkCreateImageView");
    return view;
}

VkFormat VulkanResources::chooseShadowDepthFormat() const {
    constexpr std::array candidates{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                                    VK_FORMAT_D32_SFLOAT_S8_UINT};
    constexpr VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (const auto format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
        if ((properties.optimalTilingFeatures & needed) == needed) {
            return format;
        }
    }
    throw std::runtime_error("No supported samplable Vulkan depth format was found");
}

VkFormat VulkanResources::chooseDepthFormat() const {
    constexpr std::array candidates{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                    VK_FORMAT_D24_UNORM_S8_UINT};
    for (const auto format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
            return format;
        }
    }
    throw std::runtime_error("No supported Vulkan depth format was found");
}

} // namespace mc::render
