#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mc::assets {

// One language a pack declares in its `pack.mcmeta` `language` block. The client
// reads these to populate the language menu; `bidirectional` selects the RTL
// shaping path for Arabic/Hebrew scripts.
struct PackLanguage final {
    std::string code;   // e.g. "zh_cn"
    std::string name;   // display name in that language
    std::string region; // display region
    bool bidirectional = false;

    [[nodiscard]] bool operator==(const PackLanguage&) const = default;
};

// One overlay a pack declares in `pack.mcmeta` `overlays.entries`. An overlay is
// a subdirectory (`directory`) holding its own `assets/` tree that overrides the
// pack's main assets when the loading format falls in the overlay's range. 26.1
// packs use this to ship version-specific art (e.g. ProgrammerArtFix's
// `drop26_1`) without forking the whole pack.
struct PackOverlay final {
    std::string directory;
    int minFormat = 0;
    int maxFormat = 0;

    [[nodiscard]] bool appliesTo(int format) const {
        return format >= minFormat && format <= maxFormat;
    }
    [[nodiscard]] bool operator==(const PackOverlay&) const = default;
};

// A parsed `pack.mcmeta`. The resource-pack format is a range: modern packs
// carry `min_format`/`max_format`, older ones a single `pack_format`, which is
// normalised here to `minFormat == maxFormat`. A reader compares its own
// supported format against this range and warns (rather than refusing) on a
// mismatch, the way vanilla still loads an out-of-range pack behind a caution.
struct PackMetadata final {
    int minFormat = 0;
    int maxFormat = 0;
    std::string description;
    std::vector<PackLanguage> languages;
    // In declaration order; the last applicable overlay wins over earlier ones
    // and over the pack's main assets.
    std::vector<PackOverlay> overlays;

    [[nodiscard]] bool supportsFormat(int format) const {
        return format >= minFormat && format <= maxFormat;
    }

    // Parses the text of a pack.mcmeta. Throws std::runtime_error (with a
    // line/column from the JSON reader) on malformed JSON; a well-formed file
    // missing the `pack` object yields a zeroed format rather than throwing, so
    // a caller can diagnose "not a pack" without a try/catch.
    [[nodiscard]] static PackMetadata parse(std::string_view mcmeta);
};

} // namespace mc::assets
