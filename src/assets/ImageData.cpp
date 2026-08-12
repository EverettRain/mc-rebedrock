#include "assets/ImageData.hpp"

#include <stb_image.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace mc::assets {

ImageData ImageData::loadRgba(const std::filesystem::path& path) {
    int sourceChannels = 0;
    int imageWidth = 0;
    int imageHeight = 0;
    auto* pixels = stbi_load(path.string().c_str(), &imageWidth, &imageHeight, &sourceChannels,
                             STBI_rgb_alpha);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error("Unable to decode image " + path.string() + ": " +
                                 (reason != nullptr ? reason : "unknown stb_image error"));
    }

    const auto byteCount =
        static_cast<std::size_t>(imageWidth) * static_cast<std::size_t>(imageHeight) * 4U;
    ImageData image;
    image.width = imageWidth;
    image.height = imageHeight;
    image.rgba.assign(pixels, pixels + byteCount);
    stbi_image_free(pixels);
    return image;
}

ImageData ImageData::missingTexture(int width, int height) {
    // MissingTextureAtlasSprite.generateMissingImage: magenta where
    // (x < w/2) XOR (y < h/2), black elsewhere — a diagonal two-quadrant split.
    const int safeWidth = width > 0 ? width : 16;
    const int safeHeight = height > 0 ? height : 16;
    ImageData image;
    image.width = safeWidth;
    image.height = safeHeight;
    image.rgba.resize(static_cast<std::size_t>(safeWidth) * static_cast<std::size_t>(safeHeight) *
                      4U);
    for (int y = 0; y < safeHeight; ++y) {
        for (int x = 0; x < safeWidth; ++x) {
            const bool magenta = (x < safeWidth / 2) != (y < safeHeight / 2);
            const std::size_t index =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(safeWidth) +
                 static_cast<std::size_t>(x)) *
                4U;
            image.rgba[index + 0U] = magenta ? 248U : 0U;
            image.rgba[index + 1U] = 0U;
            image.rgba[index + 2U] = magenta ? 248U : 0U;
            image.rgba[index + 3U] = 255U;
        }
    }
    return image;
}

ImageData ImageData::loadRgbaOrMissing(const std::filesystem::path& path, int fallbackWidth,
                                       int fallbackHeight) {
    int sourceChannels = 0;
    int imageWidth = 0;
    int imageHeight = 0;
    auto* pixels = stbi_load(path.string().c_str(), &imageWidth, &imageHeight, &sourceChannels,
                             STBI_rgb_alpha);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        std::cerr << "[missing-texture] " << path.string() << ": "
                  << (reason != nullptr ? reason : "unknown stb_image error") << '\n';
        return missingTexture(fallbackWidth, fallbackHeight);
    }
    const auto byteCount =
        static_cast<std::size_t>(imageWidth) * static_cast<std::size_t>(imageHeight) * 4U;
    ImageData image;
    image.width = imageWidth;
    image.height = imageHeight;
    image.rgba.assign(pixels, pixels + byteCount);
    stbi_image_free(pixels);
    return image;
}

} // namespace mc::assets
