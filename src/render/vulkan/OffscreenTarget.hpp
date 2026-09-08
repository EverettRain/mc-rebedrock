#pragma once

#include "render/vulkan/VulkanResources.hpp"

#include <cstdint>
#include <vector>

namespace mc::render {

// 单采样的离屏深度目标（阴影贴图等纯深度通道用），连同往里画的纯深度渲染通道和帧缓冲
// 尺寸固定、与交换链无关，因此交换链重建时它不受影响
// 另外提供帧内布局转换：画完之后必须屏障到 SHADER_READ_ONLY，主通道才能采样它
//
// RN-35：可以有多层（级联阴影图的每一级一层）。一张数组图像而不是 N 张独立图像，
// 是为了让绑定点数量与级数无关——binding 8 / 10 各绑一个 2D_ARRAY 视图就够了，
// 级数将来涨到 3、4 时那一侧一个字都不用改。
// 采样视图是**整张数组**（2D_ARRAY）；每一层另有一个单层视图，只用来当帧缓冲的附件——
// 一个 renderpass 一次只画一层，附件必须是单层视图。
class OffscreenTarget final {
  public:
    struct Config final {
        const VulkanResources* resources = nullptr;
        VkDevice device = VK_NULL_HANDLE;
        std::uint32_t width = 2048;
        std::uint32_t height = 2048;
        // 层数 = 级联数。每层一个帧缓冲，采样时按层号索引
        std::uint32_t layers = 1;
    };

    // 这张图像与它那趟 renderpass 的**实际**创建参数。RN-20c 的资源计划对
    // shadow_depth 只校验、不接管创建（理由：它的生命周期不在交换链里，且 binding 8
    // 与 shadowDebugSet 的描述符在初始化期写一次、之后从不重写，重建 image 会连带
    // 悬垂），所以必须有一份可比对的东西，否则就成了「计划说 A、创建写 B 而没人比对」
    struct Parameters final {
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t layers = 1;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageUsageFlags usage = 0;
        VkImageAspectFlags aspect = 0;
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    [[nodiscard]] Parameters parameters() const;

    void init(const Config& config);
    void destroy();

    [[nodiscard]] VkRenderPass renderPass() const { return renderPass_; }
    // 第 `layer` 层的帧缓冲。一个 renderpass 一次画一层
    [[nodiscard]] VkFramebuffer framebuffer(std::uint32_t layer = 0U) const {
        return framebuffers_.at(layer);
    }
    // 采样用的**整张数组**视图（binding 8 / 10 绑的就是它）
    [[nodiscard]] VkImageView view() const { return view_; }
    [[nodiscard]] std::uint32_t layers() const { return layers_; }
    // 图像句柄与 aspect：RN-20a 之后「画完转 SHADER_READ_ONLY」那条屏障由 frame graph
    // 在世界那步的边界上下（renderpass 之内做附件的 layout 转换是非法的），
    // 它需要按 transitionToShaderRead 的字面内容自己拼一份 VkImageMemoryBarrier
    [[nodiscard]] VkImage image() const { return image_.image; }
    [[nodiscard]] VkImageAspectFlags aspect() const { return aspect_; }
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
    // 逐层的单层视图，只作帧缓冲附件用
    std::vector<VkImageView> layerViews_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect_ = VK_IMAGE_ASPECT_DEPTH_BIT;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t layers_ = 1;
};

} // namespace mc::render
