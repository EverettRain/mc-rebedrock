#pragma once

// UI-5：菜单背景的整帧模糊（26.1 `post_effect/blur.json`）。
//
// 26.1 的模糊**不是**画在某一层上的效果，而是一趟真正的后处理：`Screen` 在界面元素之间
// 插一个 `blurBeforeThisStratum()` 标记（`Screen.java:435-438`），`GuiRenderer` 因此把
// 界面拆成 BEFORE_BLUR / AFTER_BLUR 两段（`GuiRenderer.java:182-184`），中间跑
// `GameRenderer.processBlurEffect()`。那条后处理链是**六趟** box_blur，横竖横竖横竖
// （blur.json 里三对 H/V），每趟在 main 与 swap 两张靶之间来回。
//
// 本作从前的做法是在 panorama.frag 里做一个 5x5 盒式近似。它只能糊全景自己——
// 有世界的界面（暂停、从游戏里打开的选项）背后是世界画面，那条路径根本碰不到，
// 于是暂停菜单背后的世界一直是清晰的。这个类是那件事的收口。
//
// 它自带全部东西：一张与交换链同尺寸的 ping-pong 靶、一趟只有颜色附件的渲染通道、
// screenquad 管线、以及一个 **LINEAR** 采样器——box_blur.fsh 靠双线性采样把采样次数
// 减半（步长 2，在像素之间取样），换成最近邻会漏掉一半像素，画面变成竖条纹。
//
// 全景那一趟也在这里 begin/end：它必须跑在模糊**之前**（否则糊不到它），而界面其余
// 部分跑在模糊之后。两件事的先后就是这个类存在的理由，拆开放两处就会有人调换。

#include "render/vulkan/VulkanResources.hpp"
#include "ui/ScreenBackground.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace mc::render {

class MenuBlur final {
  public:
    struct Config final {
        const VulkanResources* resources = nullptr;
        VkDevice device = VK_NULL_HANDLE;
        VkExtent2D extent{};
        // scene_color 的格式与它的逐交换链图像视图/图像句柄
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        std::span<const VkImageView> sceneColorViews{};
        std::span<const VkImage> sceneColorImages{};
        // 界面那趟期望 scene_color 处于的布局。取自 frame graph 的推导结果，
        // 不是写死的常量——模糊完必须**原样**把它还回去，否则界面那趟的
        // initialLayout 对不上，而那是校验层才看得见的错误。
        VkImageLayout sceneColorPassLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // 全景那一趟用的渲染通道与帧缓冲。渲染通道与 guiRenderPass **兼容**
        //（同样的附件数、格式与采样数），所以 hud / panorama 两条管线不必重建，
        // guiFramebuffers 也能直接拿来用。
        VkRenderPass backgroundRenderPass = VK_NULL_HANDLE;
        std::span<const VkFramebuffer> backgroundFramebuffers{};
        std::filesystem::path shaderRoot;
    };

    void init(const Config& config);
    void destroy();

    [[nodiscard]] bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

    // 全景那一趟：begin → 调用方画 → end。
    void beginBackgroundPass(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) const;
    void endBackgroundPass(VkCommandBuffer commandBuffer) const;

    // 六趟 box_blur。`radius` 是 26.1 的 menuBackgroundBlurriness（整数档，0 = 不模糊）；
    // 传 0 或更小时这里一条命令都不下。
    void record(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, int radius) const;

  private:
    struct BlurPush final {
        float dirX = 0.0F;
        float dirY = 0.0F;
        float radius = 0.0F;
        float reserved = 0.0F;
    };

    void createTargets();
    void createRenderPass();
    void createDescriptors();
    void createPipeline(const std::filesystem::path& shaderRoot);
    void transition(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout from,
                    VkImageLayout to, VkPipelineStageFlags sourceStage,
                    VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess,
                    VkAccessFlags destinationAccess) const;
    void runPass(VkCommandBuffer commandBuffer, VkFramebuffer target, VkDescriptorSet source,
                 float dirX, float dirY, float radius) const;

    const VulkanResources* resources_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkImageLayout sceneColorPassLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    std::vector<VkImageView> sceneColorViews_;
    std::vector<VkImage> sceneColorImages_;

    VkRenderPass backgroundRenderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> backgroundFramebuffers_;

    // ping-pong 的另一张靶（blur.json 里那个名叫 `swap` 的 target）
    std::vector<AllocatedImage> swapImages_;
    std::vector<VkImageView> swapViews_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> sceneFramebuffers_;
    std::vector<VkFramebuffer> swapFramebuffers_;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sceneSets_;
    std::vector<VkDescriptorSet> swapSets_;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace mc::render
