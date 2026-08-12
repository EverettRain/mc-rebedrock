#pragma once

#include <string_view>

namespace mc::audio {

// The sound events one species owns, mirroring the overridable sound hooks on a
// living entity. Resource packs choose the physical clips through sounds.json;
// gameplay only names events and never encodes OGG paths or variation counts.
struct MobSoundProfile final {
    std::string_view ambientEvent{};
    std::string_view hurtEvent{};
    std::string_view deathEvent{};
    std::string_view stepEvent{};
    // MobEntity#getSoundVolume: cows 0.4, most mobs 1.0. Hurt/death/ambient
    // clips all play at this volume with the vanilla ±0.2 randomised pitch.
    float volume = 1.0F;
    // MobEntity#playStepSound: every 1.16.1 mob steps at 0.15, pitch 1.0.
    float stepVolume = 0.15F;
};

} // namespace mc::audio
