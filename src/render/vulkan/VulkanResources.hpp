#pragma once

// 基于 VMA 的无状态资源助手，供渲染器及其离屏/GPU 模块共用
// 有了它，GpuSceneBuffer、OffscreenTarget 这类设施能自行分配缓冲与图像，不必反向依赖渲染器的实现

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace mc::render {

// 零初始化 Vulkan 结构体的最小助手，替掉 `VkXxxCreateInfo info{}; info.sType = ...;` 这两行样板
template <typename Structure> [[nodiscard]] Structure vkStructure(VkStructureType type) {
    Structure structure{};
    structure.sType = type;
    return structure;
}

// Vulkan 调用失败时带可读信息抛出
// 渲染器里所有需要检查的调用都走这里，失败以异常暴露，而不是揣着错误码继续跑
inline void checkVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                                 std::to_string(result));
    }
}

// 一个缓冲连同它的 VMA 分配
// 主机可见的缓冲（createBuffer 里走 VMA_MAPPED_BIT 的那条）其 `mapped` 非空
struct AllocatedBuffer final {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    // 0 = 不来自池，释放即销毁
    // 否则是 1 + kStreamBufferClassSizes 的下标，表示它属于流式网格池的哪一档
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

// 持有每次分配都要用到的三个句柄，调用方因此一个都不用传
// 在 VMA 分配器就绪后创建一次（分配器依赖逻辑设备，逻辑设备依赖物理设备）
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

    // 图形队列上的一次性命令提交，供下面的暂存上传和布局转换使用
    // 需要命令池和图形队列，所以只有带着它们构造出来的实例才提供这个能力
    [[nodiscard]] VkCommandBuffer beginSingleUseCommands() const;
    void endSingleUseCommands(VkCommandBuffer commandBuffer) const;

    // 整个子资源范围的图像布局转换，封在一次性命令缓冲里；访问掩码与管线阶段由调用方给
    void transitionTextureImage(const AllocatedImage& image, std::uint32_t layerCount,
                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                VkPipelineStageFlags sourceStage,
                                VkPipelineStageFlags destinationStage) const;

    [[nodiscard]] VkFormat chooseDepthFormat() const;

    // 阴影贴图要用可采样的深度格式，优先取 D32_SFLOAT
    // 其次取 MoltenVK 声明既能当深度附件又能采样的那几个深度模板格式
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
