#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::audio {

// SoundSource (1.16.1 net/minecraft/sound/SoundCategory): the fixed set of
// volume buses a sound routes through, mirroring the vanilla enum one-for-one so
// the options screen exposes the same sliders. Master is the global bus; every
// other value is a sub-category whose slider multiplies on top of Master. The
// order is the vanilla declaration order and doubles as a dense index into the
// per-category gain table — deref is one array subscript, never a map lookup.
//
// Kept in its own tiny header (no engine/miniaudio includes) so both the audio
// system and the options serialiser share one source of truth for the enum,
// the count and the vanilla names without either dragging in the other.
enum class SoundCategory : std::uint8_t {
    Master,
    Music,
    Record,  // jukebox / note block
    Weather, // rain, thunder
    Block,   // block break/place/step, redstone
    Hostile, // monster sounds
    Neutral, // passive/neutral creature sounds
    Player,  // the local player: hurt, eat, splash, footsteps
    Ambient, // cave / environment ambience
    Voice,   // spoken lines (unused in 1.16.1 content, present for parity)
    Count,
};

inline constexpr std::size_t kSoundCategoryCount = static_cast<std::size_t>(SoundCategory::Count);

// The vanilla lowercase name of each category, indexed by the enum. Matches the
// `soundCategory.<name>` translation keys and the options-file token. `master`
// is included so the whole table (used by the options serialiser and the
// sliders) round-trips.
inline constexpr std::array<std::string_view, kSoundCategoryCount> kSoundCategoryNames{
    "master", "music",  "record", "weather", "block",
    "hostile", "neutral", "player", "ambient", "voice",
};

[[nodiscard]] constexpr std::string_view soundCategoryName(SoundCategory category) {
    const auto index = static_cast<std::size_t>(category);
    return index < kSoundCategoryCount ? kSoundCategoryNames[index] : std::string_view{"master"};
}

// Parse a vanilla category name back to the enum; returns Count on no match, so
// callers can reject an unknown token rather than silently pick Master.
[[nodiscard]] constexpr SoundCategory soundCategoryFromName(std::string_view name) {
    for (std::size_t index = 0; index < kSoundCategoryCount; ++index) {
        if (name == kSoundCategoryNames[index]) {
            return static_cast<SoundCategory>(index);
        }
    }
    return SoundCategory::Count;
}

// Per-category linear gains in [0, 1], indexed by SoundCategory. Master lives at
// index 0 and is applied globally (the engine's master volume); the sub-category
// gains multiply on top of it at play time. Default is 1 for every category so a
// fresh install / an old options file with no category lines behaves exactly as
// before (everything at full volume under Master).
using SoundCategoryVolumes = std::array<float, kSoundCategoryCount>;

[[nodiscard]] constexpr SoundCategoryVolumes defaultSoundCategoryVolumes() {
    SoundCategoryVolumes volumes{};
    volumes.fill(1.0F);
    return volumes;
}

} // namespace mc::audio
