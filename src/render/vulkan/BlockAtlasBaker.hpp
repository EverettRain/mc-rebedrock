#pragma once

// The CPU side of the block/entity/effect texture array: it resolves every
// source image by name from the block/item/entity registries and the biome
// definitions, bakes them onto their fixed atlas layers (see BlockAtlasLayout),
// and returns the packed RGBA layers. Kept separate from TextureManager so the
// renderer's texture layer stays a leaf that only depends on VulkanResources —
// all the gameplay/world content coupling lives here instead.

#include "assets/ResourceProvider.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace mc::render {

// RN-4b: an animated non-fluid block texture baked as a contiguous run of frames
// (magma, and — once added to the roster — prismarine/sea lantern/…). `baseLayer`
// is the first frame's atlas layer; the terrain shader advances from it by
// `floor(tick/frameTime mod frameCount)`, the same cycle the fluids use. This
// generalises the old "fluids only" animation section so any `.mcmeta` block
// strip animates instead of baking frame 0.
struct BlockTextureAnimation final {
    float baseLayer = 0.0F;
    std::uint32_t frameCount = 0;
    float frameTime = 1.0F;
};

// One block-array layer is `width * height * 4` bytes; `rgba` holds every layer
// back to back, so the layer count is `rgba.size() / (width * height * 4)`.
struct TextureArrayPixels final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    // water still/flow, lava still/flow: ticks spent on each frame, read from
    // each texture's .mcmeta and forwarded to the terrain shader.
    std::array<float, 4> fluidAnimationFrameTimes{1.0F, 1.0F, 1.0F, 1.0F};
    // RN-4b: every animated non-fluid block texture, in the order baked.
    std::vector<BlockTextureAnimation> blockAnimations;
};

[[nodiscard]] TextureArrayPixels bakeBlockAtlas(const assets::ResourceProvider& resources);

} // namespace mc::render
