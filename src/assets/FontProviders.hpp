#pragma once

#include "assets/ResourceLocation.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace mc::assets {

class ResourceProvider;

enum class FontProviderKind {
    Bitmap,
    Space,
    Unihex,
};

struct UnihexSizeOverride final {
    char32_t from = 0;
    char32_t to = 0;
    int left = 0;
    int right = 15;
};

struct FontProviderDefinition final {
    FontProviderKind kind = FontProviderKind::Bitmap;
    ResourceLocation file;
    int height = 8;
    int ascent = 7;
    std::vector<std::vector<char32_t>> chars;
    std::vector<std::pair<char32_t, float>> advances;
    std::vector<UnihexSizeOverride> sizeOverrides;
};

// Resolves font/<id>.json references recursively and returns the concrete
// providers in author order. The two filter flags cover the conditions used by
// vanilla 26.1's default/uniform stacks.
[[nodiscard]] std::vector<FontProviderDefinition>
loadFontProviders(const ResourceProvider& resources, std::string_view id, bool uniform = false,
                  bool japanese = false);

} // namespace mc::assets
