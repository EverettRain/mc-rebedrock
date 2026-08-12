#include "assets/PackMetadata.hpp"

#include "core/Json.hpp"

namespace mc::assets {

PackMetadata PackMetadata::parse(std::string_view mcmeta) {
    const core::Json root = core::Json::parse(mcmeta);
    const core::Json& pack = root["pack"];

    PackMetadata metadata;
    // `min_format`/`max_format` is the modern range; a lone `pack_format`
    // collapses to a single-value range. Description is optional and, in real
    // packs, is sometimes a rich-text array rather than a string — only the
    // plain string form is kept here.
    if (pack.contains("min_format") || pack.contains("max_format")) {
        metadata.minFormat = static_cast<int>(pack["min_format"].asNumber());
        metadata.maxFormat = static_cast<int>(pack["max_format"].asNumber());
    } else {
        const int format = static_cast<int>(pack["pack_format"].asNumber());
        metadata.minFormat = format;
        metadata.maxFormat = format;
    }
    if (pack["description"].isString()) {
        metadata.description = pack["description"].asString();
    }

    // The language block maps a code to its display metadata; member order is
    // preserved by the JSON reader, so the menu lists languages as authored.
    const core::Json& languages = root["language"];
    if (languages.isObject()) {
        for (const auto& [code, entry] : languages.asObject()) {
            metadata.languages.push_back(PackLanguage{
                code,
                entry["name"].asString(),
                entry["region"].asString(),
                entry["bidirectional"].asBool(false),
            });
        }
    }

    // Overlays: version-gated subdirectories that override the main assets. Each
    // carries its own format range; a bare `format` is a single-value range.
    const core::Json& overlays = root["overlays"]["entries"];
    if (overlays.isArray()) {
        for (std::size_t index = 0; index < overlays.size(); ++index) {
            const core::Json& entry = overlays[index];
            PackOverlay overlay;
            overlay.directory = entry["directory"].asString();
            if (entry.contains("min_format") || entry.contains("max_format")) {
                overlay.minFormat = static_cast<int>(entry["min_format"].asNumber());
                overlay.maxFormat = static_cast<int>(entry["max_format"].asNumber());
            } else {
                const int format = static_cast<int>(entry["format"].asNumber());
                overlay.minFormat = format;
                overlay.maxFormat = format;
            }
            if (!overlay.directory.empty()) {
                metadata.overlays.push_back(std::move(overlay));
            }
        }
    }
    return metadata;
}

} // namespace mc::assets
