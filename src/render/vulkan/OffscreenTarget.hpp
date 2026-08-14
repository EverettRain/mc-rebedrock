#pragma once

#include "render/vulkan/VulkanResources.hpp"

#include <cstdint>

namespace mc::render {

// A single-sample offscreen depth target (for shadow maps / depth-only passes)
// plus the depth-only render pass and framebuffer that draw into it. Sized once
// at a fixed resolution independent of the swapchain, so it survives swapchain
// recreation. Also exposes the in-frame layout transition the renderer lacks: a
// pass finished drawing into it must be barrier'd to SHADER_READ_ONLY before the
// main pass can sample it.
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

    // In-frame barrier: DEPTH_STENCIL_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL,
    // so the main pass can sample the depth map after the shadow pass.
    void transitionToShaderRead(VkCommandBuffer commandBuffer) const;

    // Puts a freshly created target straight into SHADER_READ_ONLY_OPTIMAL,
    // from UNDEFINED, on its own one-shot submit.
    //
    // Every frame's descriptor set points at this image and declares that
    // layout, and the shaders that sample it do so unconditionally as far as
    // Vulkan is concerned — a runtime `if` does not make a statically used
    // descriptor optional. So the layout has to hold even when nothing ever
    // renders into the target, which is exactly what happens with the sun
    // shadows switched off: the pre-pass returns early and the transition it
    // would have done never runs.
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
