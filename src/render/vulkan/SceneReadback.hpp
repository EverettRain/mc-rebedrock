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
[[nodiscard]] bool writeSceneImagePng(const VulkanResources& resources, VkDevice device,
                                      VkImage sceneImage, VkFormat sceneFormat,
                                      VkImageLayout currentLayout, std::uint32_t width,
                                      std::uint32_t height,
                                      const std::filesystem::path& file);

} // namespace mc::render
