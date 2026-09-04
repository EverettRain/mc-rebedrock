#pragma once

// RN-15b: the one part of the readback that is not a Vulkan call.
//
// It lives here, away from SceneReadback.hpp, precisely so it can be tested: the
// export path itself needs a GPU and cannot run in this project's CI container,
// but the byte transform between "what the scene image holds" and "what a PNG
// wants" is pure arithmetic, and it is also the part most likely to be silently
// wrong — a red/blue swap produces a perfectly plausible-looking picture that is
// simply the wrong colour, and nobody notices until it is a stored baseline.

#include <cstdint>
#include <span>

namespace mc::render {

// Whether a scene image of this Vulkan format stores blue in the first byte.
//
// The scene image's format follows the swapchain's (VulkanRenderer::
// sceneUnormFormat) so that the final copy stays a byte-for-byte move, which
// means it is BGRA on most surfaces and RGBA on the rest. PNG is always RGBA.
//
// Takes the format as a plain integer rather than a VkFormat so this header
// stays free of the Vulkan headers; the two enumerators are ABI-stable values in
// the Vulkan core spec.
inline constexpr std::uint32_t kFormatB8G8R8A8Unorm = 44U; // VK_FORMAT_B8G8R8A8_UNORM
inline constexpr std::uint32_t kFormatB8G8R8A8Srgb = 50U;  // VK_FORMAT_B8G8R8A8_SRGB

[[nodiscard]] constexpr bool readbackNeedsRedBlueSwap(std::uint32_t format) {
    return format == kFormatB8G8R8A8Unorm || format == kFormatB8G8R8A8Srgb;
}

// Turns the raw readback into the bytes a PNG is written from, in place.
//
// Two transforms, both mandatory:
//   * the channel swap above, when the source is BGRA;
//   * alpha forced opaque. The preview is a picture: the sky fills every pixel
//     the block does not, and whatever the GUI pass's blending left in the alpha
//     channel is not a transparency the viewer should inherit. A PNG that came
//     out with zero alpha looks like an empty file.
//
// A trailing partial pixel is left alone rather than read past.
constexpr void normalizePreviewPixels(std::span<std::uint8_t> pixels, bool swapRedAndBlue) {
    for (std::size_t index = 0; index + 4U <= pixels.size(); index += 4U) {
        if (swapRedAndBlue) {
            const std::uint8_t blue = pixels[index];
            pixels[index] = pixels[index + 2U];
            pixels[index + 2U] = blue;
        }
        pixels[index + 3U] = 255U;
    }
}

} // namespace mc::render
