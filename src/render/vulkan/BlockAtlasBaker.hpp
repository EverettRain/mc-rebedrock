#pragma once

// The CPU side of the block/entity/effect texture array: it resolves every
// source image by name from the block/item/entity registries and the biome
// definitions, bakes them onto their fixed atlas layers (see BlockAtlasLayout),
// and returns the packed RGBA layers. Kept separate from TextureManager so the
// renderer's texture layer stays a leaf that only depends on VulkanResources —
// all the gameplay/world content coupling lives here instead.

#include "assets/ResourceProvider.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace mc::render {

// One block-array layer is `width * height * 4` bytes; `rgba` holds every layer
// back to back, so the layer count is `rgba.size() / (width * height * 4)`.
struct TextureArrayPixels final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] TextureArrayPixels bakeBlockAtlas(const assets::ResourceProvider& resources);

} // namespace mc::render
