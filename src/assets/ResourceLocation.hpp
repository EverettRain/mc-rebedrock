#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mc::assets {

// A namespaced content path, the way a resource pack names things:
// `minecraft:textures/block/stone.png`. `path` is always the standard,
// pack-relative content path (category first: textures/…, sounds/…, lang/…),
// never a physical on-disk path. A ResourceProvider is what turns one of these
// into bytes or a file — the standard pack layout and this project's current
// `resources/vanilla/1.16.1/…` layout are two providers over the same names.
// Which half of a pack a resource lives in. 26.1 packs carry client assets
// under `assets/` and server data (tags, loot tables, recipes) under `data/`,
// and they are two different roots for the same namespaced path. Defaulting to
// the client half keeps every existing location literal working unchanged.
enum class PackType : std::uint8_t {
    ClientResources,
    ServerData,
};

struct ResourceLocation final {
    std::string space{"minecraft"};
    std::string path;
    PackType type = PackType::ClientResources;

    [[nodiscard]] bool operator==(const ResourceLocation&) const = default;

    [[nodiscard]] std::string toString() const {
        std::string result{space};
        result.push_back(':');
        result.append(path);
        return result;
    }

    // Parses `space:path`; a bare `path` defaults to the `minecraft` namespace,
    // matching how vanilla resolves an unqualified reference.
    [[nodiscard]] static ResourceLocation parse(std::string_view text,
                                                PackType type = PackType::ClientResources) {
        const auto separator = text.find(':');
        if (separator == std::string_view::npos) {
            return ResourceLocation{"minecraft", std::string{text}, type};
        }
        return ResourceLocation{std::string{text.substr(0, separator)},
                                std::string{text.substr(separator + 1U)}, type};
    }
};

// Convenience builders for the categories this project actually reads, so call
// sites read as `assets::textures("block/stone.png")` instead of spelling the
// category prefix every time.
[[nodiscard]] inline ResourceLocation textures(std::string_view subpath,
                                               std::string_view space = "minecraft") {
    return ResourceLocation{std::string{space}, "textures/" + std::string{subpath}};
}

[[nodiscard]] inline ResourceLocation sounds(std::string_view subpath,
                                             std::string_view space = "minecraft") {
    return ResourceLocation{std::string{space}, "sounds/" + std::string{subpath}};
}

[[nodiscard]] inline ResourceLocation lang(std::string_view subpath,
                                           std::string_view space = "minecraft") {
    return ResourceLocation{std::string{space}, "lang/" + std::string{subpath}};
}

[[nodiscard]] inline ResourceLocation font(std::string_view subpath,
                                           std::string_view space = "minecraft") {
    return ResourceLocation{std::string{space}, "font/" + std::string{subpath}};
}

// Server data: tags, loot tables, recipes. `subpath` is the content path below
// `data/<namespace>/`, e.g. `tags/block/mineable/pickaxe.json`. Data resources
// layer per file through the same provider stack the client half uses, so a
// pack can override one tag without shadowing the rest.
[[nodiscard]] inline ResourceLocation data(std::string_view subpath,
                                           std::string_view space = "minecraft") {
    return ResourceLocation{std::string{space}, std::string{subpath}, PackType::ServerData};
}

} // namespace mc::assets
