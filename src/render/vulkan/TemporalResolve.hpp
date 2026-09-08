#pragma once

// TAA-1：时间性 resolve 的那一趟 GPU 对象。
//
// 位置：世界那趟之后、界面那趟之前。这个先后不是偏好——界面画在同一张 scene_color 上，
// 让它参与时间累积就是让每一个字、每一条边框都吃八帧历史，光标闪一下都会拖尾。
//
// 形状与 MenuBlur 同源（自带靶、渲染通道、描述符、管线、一个全屏三角形），
// 三处不同，每一处都是一条决定：
//
//  1. **两个颜色附件**：0 号是 8 位的 scene_color（帧末 vkCmdCopyImage 与 writeSceneImagePng
//     都按这个格式读它，一个字节都不能变），1 号是 16F 的历史。一趟写两份而不是
//     "写完再拷一份"：拷贝会先把结果量化成 8 位再进历史，那样历史是 16F 就毫无意义。
//  2. **历史是两张真正的 ping-pong 靶**，逐帧翻转，与交换链图像下标无关。
//     曾经试过蹭交换链那一维（写 history[imageIndex]、读 history[上一帧的 imageIndex]），
//     它能让帧图直接按 imageIndex 索引帧缓冲，一行特殊逻辑都不用写——
//     ★ 但 vkAcquireNextImageKHR **不保证**每帧给出不同的下标。隐藏窗口的离屏导出上
//     它实测恒为同一个值，于是同一张图像在同一趟里既是颜色附件又被采样：
//     校验层报 imageLayout-00344，真机上是未定义行为。这里要的是一个**结构上**
//     不可能重合的下标，不是一个"实测没重复过"的下标。
//     代价是帧缓冲变成 N × 2 张，而帧图的 BakedStep 只能按 imageIndex 索引——
//     于是有了第 3 条。
//  3. **这一趟在帧图里是非渲染步，renderpass 由它自己 begin/end**（与 menu_background
//     同一种形态）。图仍然通过附件表知道谁读谁写，推导照常给出 scene_color 的 loadOp
//     与布局，这个类照抄；图不必替它保管一张两维的帧缓冲表。
//     前置屏障也顺势留在同一个 body 的开头：那三条屏障都带逐交换链图像的 VkImage 句柄，
//     而 PassDesc::barriers 是编译期烘死的一段常量区间，装得下 shadow_depth 那种单份
//     图像，装不下"按 imageIndex 取句柄"。非渲染步的身份保证它跑在任何 renderpass 之外。

#include "render/vulkan/VulkanResources.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace mc::render {

class TemporalResolve final {
  public:
    // 历史缓冲的格式。★ 不能是 8 位：TAA 每帧只掺 10% 的新样本，8 位的累积会在
    // 平缓的渐变（天空、雾）上攒出可见的 banding。scene_color 保持 8 位、只让历史
    // 是 16F，是 TAA-1 最重要的那个折中（见 TAA README 分解 1）。
    static constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    struct Config final {
        const VulkanResources* resources = nullptr;
        VkDevice device = VK_NULL_HANDLE;
        VkExtent2D extent{};
        // resolve 的输出附件 0：scene_color 本身
        VkFormat sceneColorFormat = VK_FORMAT_UNDEFINED;
        std::span<const VkImageView> sceneColorViews{};
        // resolve 的输入：世界那趟这一轮画到的那张图（scene_taa_input）
        std::span<const VkImageView> inputViews{};
        std::span<const VkImage> inputImages{};
        // 深度。重投影要靠它把像素送回上一帧，所以世界那趟的深度这一档必须 STORE
        // （推导会自己得出这一点：resolve 对它声明了 Sample）
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        std::span<const VkImage> depthImages{};
        // 深度图像**创建时**的 aspect。带 stencil 的深度格式在这里是两位，而采样用的
        // 视图只能有一个 aspect——所以视图是这个类自己建的，屏障用的仍是这一份
        VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        // scene_color 在 resolve 这一步的附件操作，取自帧图的推导结果而不是写死：
        // 写死就回到了"计划说 A、创建写 B 而没有任何东西比对这两者"
        VkAttachmentLoadOp sceneLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkImageLayout sceneInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout sceneFinalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        std::filesystem::path shaderRoot;
    };

    void init(const Config& config);
    void destroy();

    [[nodiscard]] bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

    // 整趟：三条前置屏障（世界那趟的输出与深度转成可采样，加上一帧那张历史的
    // **跨提交可见性**——两帧在队列上是可以重叠的，layout 相同不代表数据已经可读），
    // 然后 begin/draw/end。`writeSlot` 是本帧写哪一张历史，读的是另一张。
    void record(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                std::uint32_t writeSlot, const glm::mat4& reprojection,
                float historyWeight) const;

  private:
    // 80 字节，push constant 的 128 字节上限还剩一半。矩阵单独一个字段而不是拆成
    // 行：着色器那边就是 mat4，拆开只会多一处要手工对齐的布局
    struct TemporalPush final {
        glm::mat4 reprojection{1.0F};
        float historyWeight = 0.0F;
        float reserved0 = 0.0F;
        float reserved1 = 0.0F;
        float reserved2 = 0.0F;
    };

    // ping-pong 的两张靶。两张就够：TAA 读一帧写一帧，第三张永远没有读者
    static constexpr std::size_t kHistorySlotCount = 2U;

    void createHistoryTargets();
    void createRenderPass(const Config& config);
    void createFramebuffers(const Config& config);
    void createDescriptors(const Config& config);
    void createPipeline(const std::filesystem::path& shaderRoot);
    void recordBarriers(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                        std::uint32_t readSlot) const;
    // 一张刚分配的图里是垃圾，而第一帧的描述符照样指着它。清成黑并留在
    // SHADER_READ_ONLY——权重是 0，值不会被采用，但采样一张 UNDEFINED 布局的图像
    // 本身就是未定义行为
    void initializeHistory() const;

    const VulkanResources* resources_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkImageAspectFlags depthAspect_ = VK_IMAGE_ASPECT_DEPTH_BIT;

    std::vector<VkImage> inputImages_;
    std::vector<VkImage> depthImages_;
    std::vector<VkImageView> depthViews_;

    std::vector<AllocatedImage> historyImages_;
    std::vector<VkImageView> historyViews_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    // N × 2：[imageIndex * kHistorySlotCount + writeSlot]。scene_color 那一维跟着
    // 交换链图像走，历史那一维跟着帧的奇偶走，两者互不相干
    std::vector<VkFramebuffer> framebuffers_;

    VkSampler nearestSampler_ = VK_NULL_HANDLE;
    VkSampler linearSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout currentLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout historyLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> currentSets_;
    std::vector<VkDescriptorSet> historySets_;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace mc::render
