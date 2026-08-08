#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace mc::assets {

struct ImageData final {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] static ImageData loadRgba(const std::filesystem::path& path);
};

} // namespace mc::assets
