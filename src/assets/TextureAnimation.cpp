#include "assets/TextureAnimation.hpp"

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"

#include <fstream>
#include <sstream>

namespace mc::assets {

std::optional<TextureAnimation> TextureAnimation::parse(std::string_view mcmeta) {
    core::Json root;
    try {
        root = core::Json::parse(mcmeta);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!root.contains("animation")) {
        return std::nullopt;
    }
    const core::Json& animation = root["animation"];
    TextureAnimation result;
    result.frametime = static_cast<int>(animation["frametime"].asNumber(1.0));
    if (result.frametime < 1) {
        result.frametime = 1;
    }
    result.interpolate = animation["interpolate"].asBool(false);

    const core::Json& frames = animation["frames"];
    if (frames.isArray()) {
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const core::Json& entry = frames[i];
            if (entry.isNumber()) {
                // Bare index: default frametime.
                result.frames.push_back(TextureAnimationFrame{static_cast<int>(entry.asNumber()), -1});
            } else if (entry.isObject()) {
                result.frames.push_back(TextureAnimationFrame{
                    static_cast<int>(entry["index"].asNumber()),
                    static_cast<int>(entry["time"].asNumber(-1.0)),
                });
            }
        }
    }
    return result;
}

std::optional<TextureAnimation> TextureAnimation::load(const ResourceProvider& resources,
                                                       const ResourceLocation& pngLocation) {
    const ResourceLocation mcmetaLocation{pngLocation.space, pngLocation.path + ".mcmeta"};
    if (!resources.exists(mcmetaLocation)) {
        return std::nullopt;
    }
    std::ifstream input{resources.locate(mcmetaLocation), std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return parse(contents.str());
}

} // namespace mc::assets
