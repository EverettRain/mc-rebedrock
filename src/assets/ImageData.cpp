#include "assets/ImageData.hpp"

#include <stb_image.h>

#include <stdexcept>
#include <string>

namespace mc::assets {

ImageData ImageData::loadRgba(const std::filesystem::path& path) {
    int sourceChannels = 0;
    int imageWidth = 0;
    int imageHeight = 0;
    auto* pixels = stbi_load(
        path.string().c_str(), &imageWidth, &imageHeight, &sourceChannels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error(
            "Unable to decode image " + path.string() + ": " +
            (reason != nullptr ? reason : "unknown stb_image error"));
    }

    const auto byteCount = static_cast<std::size_t>(imageWidth) *
                           static_cast<std::size_t>(imageHeight) * 4U;
    ImageData image;
    image.width = imageWidth;
    image.height = imageHeight;
    image.rgba.assign(pixels, pixels + byteCount);
    stbi_image_free(pixels);
    return image;
}

} // namespace mc::assets
