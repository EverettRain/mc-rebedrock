#include "render/vulkan/OffscreenTarget.hpp"

namespace mc::render {
namespace {

// init() 与 parameters() 的**同一份**取值。分成两处手抄就等于把 RN-20c 的比对
// 变成一场自证：计划要比的是这张图像真的怎么建的，不是另一处照抄出来的字面量。
constexpr VkImageUsageFlags kShadowUsage =
    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkSampleCountFlagBits kShadowSamples = VK_SAMPLE_COUNT_1_BIT;
constexpr VkAttachmentLoadOp kShadowLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
constexpr VkAttachmentStoreOp kShadowStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
constexpr VkImageLayout kShadowInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
constexpr VkImageLayout kShadowFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

} // namespace

OffscreenTarget::Parameters OffscreenTarget::parameters() const {
    return {.format = format_,
            .width = width_,
            .height = height_,
            .layers = layers_,
            .samples = kShadowSamples,
            .usage = kShadowUsage,
            .aspect = aspect_,
            .loadOp = kShadowLoadOp,
            .storeOp = kShadowStoreOp,
            .initialLayout = kShadowInitialLayout,
            .finalLayout = kShadowFinalLayout};
}

void OffscreenTarget::init(const Config& config) {
    destroy();
    resources_ = config.resources;
    device_ = config.device;
    width_ = config.width;
    height_ = config.height;
    layers_ = config.layers;
    format_ = resources_->chooseShadowDepthFormat();
    image_ =
        resources_->createImage(width_, height_, layers_, format_, kShadowUsage, kShadowSamples);
    aspect_ = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (VulkanResources::depthFormatHasStencil(format_)) {
        aspect_ |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    // ★ 采样视图**永远**是 2D_ARRAY，哪怕只有一层：着色器那边声明的是
    // sampler2DArrayShadow，而视图类型与采样器类型对不上是未定义行为
    // （createImageView 的注释里写过同一条，实体图集与字体图集踩过）
    view_ = resources_->createImageView(image_.image, format_, aspect_, layers_,
                                        VK_IMAGE_VIEW_TYPE_2D_ARRAY);

    VkAttachmentDescription depth{};
    depth.format = format_;
    depth.samples = kShadowSamples;
    depth.loadOp = kShadowLoadOp;
    depth.storeOp = kShadowStoreOp;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = kShadowInitialLayout;
    depth.finalLayout = kShadowFinalLayout;
    VkAttachmentReference depthReference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthReference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    auto passInfo =
        vkStructure<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &depth;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    checkVk(vkCreateRenderPass(device_, &passInfo, nullptr, &renderPass_), "vkCreateRenderPass(offscreen)");

    // 逐层：一个单层视图 + 一个帧缓冲。渲染通道只有一个，两级共用——
    // 它描述的是「往一张单层深度附件里画」，与层号无关
    layerViews_.resize(layers_);
    framebuffers_.resize(layers_);
    for (std::uint32_t layer = 0; layer < layers_; ++layer) {
        auto viewInfo =
            vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        viewInfo.image = image_.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange.aspectMask = aspect_;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = layer;
        viewInfo.subresourceRange.layerCount = 1;
        checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &layerViews_[layer]),
                "vkCreateImageView(offscreen layer)");
        auto framebufferInfo =
            vkStructure<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &layerViews_[layer];
        framebufferInfo.width = width_;
        framebufferInfo.height = height_;
        framebufferInfo.layers = 1;
        checkVk(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffers_[layer]),
                "vkCreateFramebuffer(offscreen)");
    }
}

void OffscreenTarget::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        for (const auto framebuffer : framebuffers_) {
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
        framebuffers_.clear();
        for (const auto layerView : layerViews_) {
            vkDestroyImageView(device_, layerView, nullptr);
        }
        layerViews_.clear();
        if (renderPass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }
        if (view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view_, nullptr);
            view_ = VK_NULL_HANDLE;
        }
    }
    if (resources_ != nullptr) {
        resources_->destroyImage(image_);
    }
    width_ = 0;
    height_ = 0;
    layers_ = 1;
    format_ = VK_FORMAT_UNDEFINED;
}

void OffscreenTarget::initializeAsShaderRead() const {
    if (image_.image == VK_NULL_HANDLE || resources_ == nullptr) {
        return;
    }
    const auto commandBuffer = resources_->beginSingleUseCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_.image;
    barrier.subresourceRange.aspectMask = aspect_;
    barrier.subresourceRange.levelCount = 1;
    // 所有层一起转：级联的每一级都是这张图的一层，采样端拿到的是整张数组
    barrier.subresourceRange.layerCount = layers_;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    resources_->endSingleUseCommands(commandBuffer);
}

void OffscreenTarget::transitionToShaderRead(VkCommandBuffer commandBuffer) const {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_.image;
    barrier.subresourceRange.aspectMask = aspect_;
    barrier.subresourceRange.levelCount = 1;
    // 所有层一起转：级联的每一级都是这张图的一层，采样端拿到的是整张数组
    barrier.subresourceRange.layerCount = layers_;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

} // namespace mc::render
