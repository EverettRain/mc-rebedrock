#pragma once

#include "assets/ResourceLocation.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace mc::assets {

class ResourceProvider;

// One entry of an animation's frame order. `index` selects a source frame;
// `time` overrides the animation-wide frametime for this frame, or is -1 to use
// the default.
struct TextureAnimationFrame final {
    int index = 0;
    int time = -1;
};

// A parsed `<texture>.png.mcmeta` `animation` block, the way vanilla drives
// animated block textures. `frametime` is ticks per frame; `frames` is the
// optional explicit order (empty means "every source frame in order").
struct TextureAnimation final {
    int frametime = 1;
    bool interpolate = false;
    std::vector<TextureAnimationFrame> frames;

    // Parses the text of a .mcmeta. Returns nullopt when the file has no
    // `animation` object (a still texture) or the JSON is malformed — a caller
    // treats either as "not animated".
    [[nodiscard]] static std::optional<TextureAnimation> parse(std::string_view mcmeta);

    // Reads `<pngLocation>.mcmeta` through the provider, or nullopt when the
    // sidecar is absent.
    [[nodiscard]] static std::optional<TextureAnimation>
    load(const ResourceProvider& resources, const ResourceLocation& pngLocation);
};

} // namespace mc::assets
