#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace mc::assets {

struct ImageData final {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    // Decodes an image, throwing std::runtime_error if the file is missing or
    // undecodable.
    [[nodiscard]] static ImageData loadRgba(const std::filesystem::path& path);

    // Decodes an image, or returns the missing-texture placeholder at the given
    // size when the file is absent or fails to decode. This is what makes an
    // incomplete resource pack degrade to a visible magenta/black marker instead
    // of crashing the atlas bake, exactly as vanilla substitutes its
    // MissingTextureAtlasSprite.
    [[nodiscard]] static ImageData loadRgbaOrMissing(const std::filesystem::path& path,
                                                     int fallbackWidth = 16,
                                                     int fallbackHeight = 16);

    // The vanilla "missing texture": a two-colour checkerboard of black and
    // magenta split into quadrants. Generated at an arbitrary size so it can
    // stand in for whatever the atlas layer expects.
    [[nodiscard]] static ImageData missingTexture(int width, int height);
};

} // namespace mc::assets
