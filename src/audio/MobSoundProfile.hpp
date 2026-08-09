#pragma once

#include <string_view>

namespace mc::audio {

// The sound set one species owns, mirroring the overridable sound hooks on a
// 1.16.1 MobEntity: getAmbientSound / getHurtSound / getDeathSound /
// playStepSound plus getSoundVolume. The clips resolve under the sound root as
// `<root>/<base><variation>.ogg`, e.g. "mob/cow/say3.ogg". A single-clip event
// (variation count of one) drops the number — vanilla's `mob/zombie/death.ogg`
// has no `1` suffix. An empty base, or a count of zero, means the species has
// no sound for that event and it stays silent.
//
// The per-species values come straight from the decompiled 1.16.1 entity
// classes and this build's sounds.json mappings:
//   Cow:     ambient mob/cow/say1-4, hurt mob/cow/hurt1-3, death reuses hurt1-3,
//            step mob/cow/step1-4, getSoundVolume 0.4 (CowEntity overrides).
//   Pig:     ambient mob/pig/say1-3, hurt reuses say1-3 (PigEntity has no
//            distinct hurt clip), death mob/pig/death, step mob/pig/step1-5.
//   Zombie:  ambient mob/zombie/say1-3, hurt mob/zombie/hurt1-2,
//            death mob/zombie/death, step mob/zombie/step1-5.
struct MobSoundProfile final {
    // The asset directory under the sound root, e.g. "mob/cow".
    std::string_view root{};
    // Per-event clip base name + variation count. `variations == 1` resolves to
    // `<root>/<base>.ogg`; `variations > 1` rolls `<root>/<base>1..N.ogg`.
    std::string_view ambientBase{};
    int ambientVariations = 0;
    std::string_view hurtBase{};
    int hurtVariations = 0;
    std::string_view deathBase{};
    int deathVariations = 0;
    std::string_view stepBase{};
    int stepVariations = 0;
    // MobEntity#getSoundVolume: cows 0.4, most mobs 1.0. Hurt/death/ambient
    // clips all play at this volume with the vanilla ±0.2 randomised pitch.
    float volume = 1.0F;
    // MobEntity#playStepSound: every 1.16.1 mob steps at 0.15, pitch 1.0.
    float stepVolume = 0.15F;
};

} // namespace mc::audio
