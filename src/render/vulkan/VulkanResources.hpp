#pragma once

// The stateless VMA-backed resource helpers shared by the Vulkan renderer and
// its offscreen/GPU modules. Extracted from VulkanRenderer.cpp so the new
// infrastructure (GpuSceneBuffer, OffscreenTarget) can allocate buffers and
// images without reaching into the renderer's implementation.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace mc::render {

// Minimal zero-initialized Vulkan structure helper: replaces the
// `VkXxxCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_XXX;` boilerplate.
template <typename Structure> [[nodiscard]] Structure vkStructure(VkStructureType type) {
    Structure structure{};
    structure.sType = type;
    return structure;
}

// Throw on a failed Vulkan call with a readable message. Every checked call in
// the renderer funnels through this so failures surface as exceptions instead
// of silently continuing with a bad result code.
inline void checkVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                                 std::to_string(result));
    }
}

// A buffer plus its VMA allocation. `mapped` is non-null for host-visible
// buffers (the VMA_MAPPED_BIT path in VulkanResources::createBuffer).
struct AllocatedBuffer final {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    // 0 = not pooled (destroy on release); otherwise 1 + index into
    // kStreamBufferClassSizes for the stream-mesh pools.
    std::uint8_t pooledSizeClass = 0;
};

struct AllocatedImage final {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

struct DepthTarget final {
    AllocatedImage image;
    VkImageView view = VK_NULL_HANDLE;
};

using ColorTarget = DepthTarget;

// Owns the three handles every allocation needs so call sites pass none of
// them. Created once after the VMA allocator exists (allocator depends on the
// device, which depends on the physical device).
class VulkanResources final {
  public:
    VulkanResources() = default;
    VulkanResources(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator,
                    VkCommandPool commandPool = VK_NULL_HANDLE,
                    VkQueue graphicsQueue = VK_NULL_HANDLE)
        : physicalDevice_(physicalDevice), device_(device), allocator_(allocator),
          commandPool_(commandPool), graphicsQueue_(graphicsQueue) {}

    [[nodiscard]] AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                               bool hostVisible) const;
    [[nodiscard]] AllocatedImage createImage(std::uint32_t width, std::uint32_t height,
                                             std::uint32_t layers, VkFormat format,
                                             VkImageUsageFlags usage,
                                             VkSampleCountFlagBits samples =
                                                 VK_SAMPLE_COUNT_1_BIT) const;
    void destroyBuffer(AllocatedBuffer& buffer) const noexcept;
    void destroyImage(AllocatedImage& image) const noexcept;

    [[nodiscard]] VkImageView createImageView(VkImage image, VkFormat format,
                                              VkImageAspectFlags aspect) const;

    // One-shot command submission on the graphics queue, used by the staging
    // uploads and layout transitions below. Needs the command pool and graphics
    // queue, so it is only available when the resources were built with them.
    [[nodiscard]] VkCommandBuffer beginSingleUseCommands() const;
    void endSingleUseCommands(VkCommandBuffer commandBuffer) const;

    // Full-subresource image layout transition wrapped in a single-use command
    // buffer; the caller supplies the access masks and pipeline stages.
    void transitionTextureImage(const AllocatedImage& image, std::uint32_t layerCount,
                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                VkPipelineStageFlags sourceStage,
                                VkPipelineStageFlags destinationStage) const;

    [[nodiscard]] VkFormat chooseDepthFormat() const;

    // A depth format that can also be sampled (for shadow maps): prefers
    // D32_SFLOAT, then the depth-stencil formats MoltenVK reports as both depth
    // attachments and samplable.
    [[nodiscard]] VkFormat chooseShadowDepthFormat() const;

    [[nodiscard]] static bool depthFormatHasStencil(VkFormat format) {
        return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
    }

  private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
};

} // namespace mc::render
