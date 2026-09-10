#pragma once

// RN-15b: the rendered frame, off the GPU and onto disk as a PNG.
//
// It reads the SCENE image, never the swapchain image. The scene image is the
// canvas both the world pass and the GUI pass draw on, and the last thing a frame
// does is `vkCmdCopyImage` it into the swapchain image — a byte-for-byte move
// with no colour conversion (VulkanRenderer::copySceneToSwapchain says so, and
// sceneUnormFormat exists to keep the channel order matching). So the scene
// image already holds exactly the bytes that reach the screen, and reading it
// avoids two things a swapchain readback would drag in: the format the surface
// happened to negotiate, and the timing of presentation.

#include "render/vulkan/VulkanResources.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace mc::render {

// Copies `sceneImage` into host memory and writes `file` as a 4-channel PNG.
//
// `currentLayout` is the layout the image is left in by the frame that drew it
// (the GUI pass's finalLayout, TRANSFER_SRC_OPTIMAL). The caller must have
// waited for that frame — this issues its own one-shot submit and does not
// synchronise against in-flight work.
//
// Returns false rather than throwing: the exporter turns one failed image into a
// non-zero exit code for the whole run, which is more useful than an exception
// halfway through eight files.
// UI-11 / A6：同一次回读，但**留在内存里**。
//
// ★ 它是从 `writeSceneImagePng` 里剖出来的那一半，不是第二条回读路径：
//   通道交换与不透明 alpha 那两步（`normalizePreviewPixels`）两边共用，
//   否则"存档缩略图的红蓝反了、而八角预览是对的"这种事迟早会发生。
//
// 返回 `width * height * 4` 字节的 RGBA8；失败返回空 vector。
[[nodiscard]] std::vector<std::uint8_t> readSceneImageRgba(const VulkanResources& resources,
                                                           VkImage sceneImage,
                                                           VkFormat sceneFormat,
                                                           VkImageLayout currentLayout,
                                                           std::uint32_t width,
                                                           std::uint32_t height);

[[nodiscard]] bool writeSceneImagePng(const VulkanResources& resources, VkDevice device,
                                      VkImage sceneImage, VkFormat sceneFormat,
                                      VkImageLayout currentLayout, std::uint32_t width,
                                      std::uint32_t height,
                                      const std::filesystem::path& file);

} // namespace mc::render
