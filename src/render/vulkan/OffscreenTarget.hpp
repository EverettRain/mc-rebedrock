#pragma once

#include "render/vulkan/VulkanResources.hpp"

#include <cstdint>

namespace mc::render {

// 单采样的离屏深度目标（阴影贴图等纯深度通道用），连同往里画的纯深度渲染通道和帧缓冲
// 尺寸固定、与交换链无关，因此交换链重建时它不受影响
// 另外提供帧内布局转换：画完之后必须屏障到 SHADER_READ_ONLY，主通道才能采样它
class OffscreenTarget final {
  public:
    struct Config final {
        const VulkanResources* resources = nullptr;
        VkDevice device = VK_NULL_HANDLE;
        std::uint32_t width = 2048;
        std::uint32_t height = 2048;
    };

    void init(const Config& config);
    void destroy();

    [[nodiscard]] VkRenderPass renderPass() const { return renderPass_; }
    [[nodiscard]] VkFramebuffer framebuffer() const { return framebuffer_; }
    [[nodiscard]] VkImageView view() const { return view_; }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }

    // 帧内屏障把布局从 DEPTH_STENCIL_ATTACHMENT_OPTIMAL 转成 SHADER_READ_ONLY_OPTIMAL
    // 主通道因此能在阴影通道之后采样这张深度图
    void transitionToShaderRead(VkCommandBuffer commandBuffer) const;

    // 用一次性提交把刚创建的目标从 UNDEFINED 直接转成 SHADER_READ_ONLY_OPTIMAL
    //
    // 每一帧的描述符集都指向这张图像并声明该布局
    // 在 Vulkan 看来采样它的着色器是无条件采样的
    // 运行期的一个 if 并不能让静态使用的描述符变成可选
    // 所以即使从没有东西画进这张图，布局也必须成立
    // 关掉太阳阴影时正是如此：预通道直接返回，它本该做的转换从未发生
    void initializeAsShaderRead() const;

  private:
    const VulkanResources* resources_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    AllocatedImage image_;
    VkImageView view_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect_ = VK_IMAGE_ASPECT_DEPTH_BIT;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace mc::render
