#include "assets/GuiSpriteScaling.hpp"

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"


namespace mc::assets {
namespace {

// A border side is a non-negative pixel count; anything else (a negative, a
// string) reads as 0 so a malformed pack degrades to "no frame" rather than
// producing inverted slice rectangles downstream.
[[nodiscard]] int borderSide(const core::Json& value, int fallback) {
    if (!value.isNumber()) {
        return fallback;
    }
    const int pixels = static_cast<int>(value.asNumber());
    return pixels > 0 ? pixels : 0;
}

[[nodiscard]] GuiSpriteBorder parseBorder(const core::Json& border) {
    // Scalar form: one inset for all four sides.
    if (border.isNumber()) {
        const int uniform = borderSide(border, 0);
        return GuiSpriteBorder{uniform, uniform, uniform, uniform};
    }
    if (!border.isObject()) {
        return GuiSpriteBorder{};
    }
    return GuiSpriteBorder{
        borderSide(border["left"], 0),
        borderSide(border["top"], 0),
        borderSide(border["right"], 0),
        borderSide(border["bottom"], 0),
    };
}

[[nodiscard]] int referenceSize(const core::Json& value) {
    const int size = static_cast<int>(value.asNumber(0.0));
    return size > 0 ? size : 0;
}

} // namespace

GuiSpriteScaling GuiSpriteScaling::parse(std::string_view mcmeta) {
    core::Json root;
    try {
        root = core::Json::parse(mcmeta);
    } catch (const std::exception&) {
        return GuiSpriteScaling{};
    }
    const core::Json& scaling = root["gui"]["scaling"];
    if (!scaling.isObject()) {
        return GuiSpriteScaling{};
    }

    GuiSpriteScaling result;
    const std::string& type = scaling["type"].asString();
    if (type == "nine_slice") {
        result.type = GuiSpriteScalingType::NineSlice;
    } else if (type == "tile") {
        result.type = GuiSpriteScalingType::Tile;
    } else {
        // "stretch" and any unrecognised type both keep the default, which
        // carries no border and needs no reference size.
        return GuiSpriteScaling{};
    }

    result.width = referenceSize(scaling["width"]);
    result.height = referenceSize(scaling["height"]);
    if (result.type == GuiSpriteScalingType::NineSlice) {
        result.border = parseBorder(scaling["border"]);
        result.stretchInner = scaling["stretch_inner"].asBool(false);
    }
    return result;
}

GuiSpriteScaling GuiSpriteScaling::load(const ResourceProvider& resources,
                                        const ResourceLocation& pngLocation) {
    const ResourceLocation mcmetaLocation{pngLocation.space, pngLocation.path + ".mcmeta"};
    if (!resources.exists(mcmetaLocation)) {
        return GuiSpriteScaling{};
    }
    // Read as bytes, not through a path: a zipped pack serves the sidecar
    // straight out of the archive instead of extracting it to disk first.
    const auto bytes = resources.readBytes(mcmetaLocation);
    if (bytes.empty()) {
        return GuiSpriteScaling{};
    }
    return parse(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

} // namespace mc::assets
