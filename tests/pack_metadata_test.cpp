#include "assets/PackMetadata.hpp"

#include <cassert>
#include <string_view>

// pack.mcmeta drives which packs the client offers and how it lists their
// languages. These pin the two format spellings (modern min/max and the legacy
// single pack_format) and the language block, against the shape the real
// vanilla-26.1 pack uses.
int main() {
    using namespace mc::assets;

    // --- Modern min_format/max_format range, as 26.1 writes it. ---
    {
        constexpr std::string_view mcmeta = R"({
            "pack": {
                "min_format": 84,
                "max_format": 84,
                "description": "Minecraft Java Edition 26.1 Vanilla Resources"
            },
            "language": {
                "en_us": {"name": "English", "region": "US", "bidirectional": false},
                "ar_sa": {"name": "العربية", "region": "SA", "bidirectional": true}
            }
        })";
        const auto meta = PackMetadata::parse(mcmeta);
        assert(meta.minFormat == 84 && meta.maxFormat == 84);
        assert(meta.supportsFormat(84));
        assert(!meta.supportsFormat(83));
        assert(meta.description == "Minecraft Java Edition 26.1 Vanilla Resources");
        assert(meta.languages.size() == 2U);
        // Member order is preserved, so the menu lists languages as authored.
        assert(meta.languages[0].code == "en_us" && meta.languages[0].name == "English");
        assert(!meta.languages[0].bidirectional);
        assert(meta.languages[1].code == "ar_sa" && meta.languages[1].bidirectional);
    }

    // --- Legacy single pack_format collapses to a one-value range. ---
    {
        constexpr std::string_view mcmeta = R"({"pack": {"pack_format": 6, "description": "old"}})";
        const auto meta = PackMetadata::parse(mcmeta);
        assert(meta.minFormat == 6 && meta.maxFormat == 6);
        assert(meta.supportsFormat(6) && !meta.supportsFormat(7));
        assert(meta.description == "old");
        assert(meta.languages.empty());
    }

    // --- A well-formed file that is not a pack yields a zeroed format rather
    //     than throwing, so "not a pack" is diagnosable without a try/catch. ---
    {
        const auto meta = PackMetadata::parse("{}");
        assert(meta.minFormat == 0 && meta.maxFormat == 0);
        assert(meta.description.empty());
        assert(meta.languages.empty());
    }

    // --- Version-gated overlays parse, as ProgrammerArtFix's drop26_1 does. ---
    {
        constexpr std::string_view mcmeta = R"({
            "pack": {"min_format": 75, "max_format": 84, "description": "patch"},
            "overlays": {"entries": [
                {"directory": "drop26_1", "min_format": 77, "max_format": 84},
                {"directory": "legacy", "format": 70}
            ]}
        })";
        const auto meta = PackMetadata::parse(mcmeta);
        assert(meta.overlays.size() == 2U);
        assert(meta.overlays[0].directory == "drop26_1");
        assert(meta.overlays[0].appliesTo(84) && meta.overlays[0].appliesTo(77));
        assert(!meta.overlays[0].appliesTo(76)); // below range
        // A bare `format` is a single-value range.
        assert(meta.overlays[1].directory == "legacy");
        assert(meta.overlays[1].appliesTo(70) && !meta.overlays[1].appliesTo(84));
    }

    return 0;
}
