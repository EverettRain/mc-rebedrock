#pragma once

#include "assets/ResourceProvider.hpp"
#include "audio/AmbientMusicScheduler.hpp"
#include "audio/MobSoundProfile.hpp"
#include "audio/SoundCategory.hpp"
#include "world/Block.hpp"

#include <glm/vec3.hpp>
#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>

namespace mc::audio {

// A block's sound group is now a block property (world::blockSoundType), the
// single source both this system and any future JE-datapack sound mapping read.
// The old ad-hoc whitelist here defaulted the great majority of blocks to Stone
// (dirt to grass, only three wool colours, nothing nether); it has been retired
// in favour of the complete, per-block SoundType transcribed from Blocks.java.
using BlockSoundFamily = world::SoundType;

[[nodiscard]] constexpr BlockSoundFamily blockSoundFamily(world::Block block) {
    return world::blockSoundType(block);
}

// The event-name prefix for a group ("block.<name>.<action>"), or "" for a group
// that plays nothing (SoundType::Empty). Defined in the .cpp.
[[nodiscard]] const char* blockSoundFamilyName(BlockSoundFamily family);

// ⑥ The linear distance-attenuation gain a positioned voice receives, matching
// miniaudio's linear model exactly (ma_attenuation_model_linear):
//   gain = 1 − rolloff · (clamp(d, min, max) − min) / (max − min), clamped [0,1].
// play() sets rolloff = 1.0, so the gain converges SMOOTHLY to exactly 0 as the
// distance approaches maxDistance — no non-zero plateau (miniaudio's default
// inverse model flattened at ≈0.108, which is what leaked distant/underground
// mobs through) and no discontinuous jump. This is the faithful curve, not a
// guard that fakes zero at the boundary: with rolloff = 1.0, clamp(d,min,max)==max
// makes the formula itself return 0. Pure/device-free so the near-field-full,
// silent-past-max, monotonic guarantees are verifiable without an audio device.
[[nodiscard]] constexpr float linearAttenuationGain(float distance, float minDistance,
                                                    float maxDistance, float rolloff = 1.0F) {
    if (maxDistance <= minDistance) {
        return distance <= minDistance ? 1.0F : 0.0F;
    }
    // Clamp the distance into [min, max] first, exactly as miniaudio does; beyond
    // max the clamped distance is max, so the formula yields 1 − rolloff (0 when
    // rolloff == 1.0) with no separate early-out needed.
    const float clamped = distance < minDistance ? minDistance
                          : distance > maxDistance ? maxDistance
                                                   : distance;
    const float gain = 1.0F - rolloff * (clamped - minDistance) / (maxDistance - minDistance);
    return gain < 0.0F ? 0.0F : (gain > 1.0F ? 1.0F : gain);
}

// ⑦ Vanilla's audible ceiling for a positioned sound, derived from its emission
// volume: ServerWorld.playSound broadcasts (and the client attenuates) out to
// 16 · max(volume, 1) blocks. An ordinary mob ambient/hurt clip has volume ≈ 1.0
// → 16 blocks; a record has volume 4.0 → 64 blocks (the same formula, not a
// special case). The max(volume, 1) floor means quieter-than-1 sounds (footsteps
// at 0.5, item pickup) still carry the full 16 rather than shrinking. Pure/
// device-free so both play() and the regression share one source of truth.
[[nodiscard]] constexpr float vanillaBroadcastRadius(float emissionVolume) {
    return 16.0F * std::max(emissionVolume, 1.0F);
}

// ⑥ Whether play() should skip a positioned voice entirely: its source sits past
// the attenuation ceiling, where the linear model is already silent. Culling it
// saves the decode and is the second guarantee that nothing beyond `maxDistance`
// is ever heard. Shared by play() and the regression so a dropped cull is caught.
[[nodiscard]] constexpr bool cullByDistance(float distance, float maxDistance) {
    return distance > (maxDistance < 0.0F ? 0.0F : maxDistance);
}

class AudioSystem final {
  public:
    explicit AudioSystem(const assets::ResourceProvider& provider, float masterVolume = 1.0F);
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;

    [[nodiscard]] bool available() const;
    // PX-6 Bug3: the accessibility subtitle of the most recently played sound
    // event (empty when the event has no caption). The renderer reads this right
    // after a play call to feed the subtitle overlay when subtitles are enabled.
    [[nodiscard]] std::string_view lastSubtitle() const;
    void setMasterVolume(float volume);
    // Per-category (non-master) gain. Master is still driven by setMasterVolume
    // (the engine's global bus); setting SoundCategory::Master here folds into the
    // same value. Every subsequent play of that category multiplies by this gain,
    // so lowering one category never touches another.
    void setCategoryVolume(SoundCategory category, float volume);
    [[nodiscard]] float categoryVolume(SoundCategory category) const;
    // Apply a whole options-loaded table at once (Master routed to the engine,
    // the rest to the per-category gains).
    void setCategoryVolumes(const SoundCategoryVolumes& volumes);
    // Vanilla's "Directional Audio" (HRTF) toggle. On enables miniaudio's HRTF
    // spatializer for positioned voices; off falls back to plain stereo panning.
    // Parity only — no occlusion or reverb is added (vanilla has none).
    void setDirectionalAudio(bool enabled);
    [[nodiscard]] bool directionalAudio() const;
    // The pre-distance effective volume a play of `eventVolume` on `category`
    // would use: master × category-gain × eventVolume. This is exactly the value
    // handed to miniaudio's set_volume (miniaudio then applies distance on top),
    // exposed so the layered-multiply and category isolation are verifiable
    // without an audio device. Master's own gain folds into master, so a Master
    // play is master × eventVolume.
    [[nodiscard]] float effectiveVolume(SoundCategory category, float eventVolume) const;
    // The per-voice volume a streamed voice (situational music / biome ambient
    // loop) hands to miniaudio: the sub-category gain alone, with NO master
    // factor. Master is the engine's global endpoint gain applied once to every
    // voice, so a streamed voice must not fold it in again (the master² bug).
    // Exposed so that invariant is verifiable without an audio device.
    [[nodiscard]] float streamedVoiceVolume(SoundCategory category) const;
    void updateListener(const glm::vec3& position, const glm::vec3& direction, const glm::vec3& up);
    void update();

    void playBlockBreak(world::Block block, const glm::vec3& position);
    void playBlockHit(world::Block block, const glm::vec3& position);
    void playBlockPlace(world::Block block, const glm::vec3& position);
    // Block interaction sounds: door/trapdoor/fence gate/chest open+close and
    // lever/button click. The block selects the vanilla event family; `on` picks
    // the lever pitch / button click_on|click_off.
    void playBlockOpen(world::Block block, const glm::vec3& position);
    void playBlockClose(world::Block block, const glm::vec3& position);
    void playBlockClick(world::Block block, const glm::vec3& position, bool on);
    // Tool-use sounds: flint and steel igniting, shears shearing a sheep.
    void playFlintAndSteelUse(const glm::vec3& position);
    void playShear(const glm::vec3& position);
    // `ui.button.click`, the master-category sound every vanilla button plays
    // when pressed. Positioned at the listener so attenuation cannot hide it.
    void playButtonClick(const glm::vec3& position);
    void playFootstep(world::Block block, const glm::vec3& position, float volume = 0.5F);
    void playItemPickup(const glm::vec3& position);
    void playSplash(const glm::vec3& position, float volume = 0.7F);
    void playPlayerHurt(const glm::vec3& position);
    void playPlayerFall(const glm::vec3& position, bool heavy);
    void playEat(const glm::vec3& position);
    void playBurp(const glm::vec3& position);
    // `entity.item.break`, played when a tool runs out of durability.
    void playItemBreak(const glm::vec3& position);

    // ---- Per-species mob sound events ----
    // Each resolves the species event through sounds.json and applies its
    // getSoundVolume with the vanilla pitch roll. A profile with no event
    // stays silent, so species without an ambient sound are not forced to bark.
    // `category` is the creature bus (Hostile for monsters, Neutral for the rest,
    // Ambient for bats) the caller derives from the species' MobCategory.
    void playCreatureHurt(const MobSoundProfile& profile, SoundCategory category,
                          const glm::vec3& position);
    void playCreatureDeath(const MobSoundProfile& profile, SoundCategory category,
                           const glm::vec3& position);
    void playCreatureAmbient(const MobSoundProfile& profile, SoundCategory category,
                             const glm::vec3& position);
    // Steps use profile.stepVolume and fixed pitch 1.0.
    void playCreatureStep(const MobSoundProfile& profile, SoundCategory category,
                          const glm::vec3& position);

    // ---- Weather ----
    // weather.rain / weather.rain.above: the per-frame rain clip played at the
    // surface the drops hit, and its muffled under-roof variant. `volume` is the
    // caller's gradient-scaled value — 0.2 base for rain, 0.1 for rain-above.
    void playWeatherRain(const glm::vec3& position, float volume);
    void playWeatherRainAbove(const glm::vec3& position, float volume);

    // ---- AU-2: ambient environment + situational music + jukebox ----

    // The per-tick context the ambient/music scheduler reads: which situational
    // music applies, the biome's ambient loop event (empty = none), and — when
    // the caller is in a mood-capable place — a brightness sample near the player
    // for the cave-mood accumulator. All of it is gathered by the renderer, so
    // the audio system stays free of world/biome types.
    struct AmbientMusicContext final {
        MusicSituation situation = MusicSituation::Game;
        // The biome's looping ambience (BiomeAmbientSoundsHandler loop sound),
        // e.g. a nether/cave loop; empty means the current biome has none.
        std::string_view ambientLoopEvent{};
        // When present, the mood accumulator advances by this sample. Absent when
        // the world is not loaded / no sampling happened this tick.
        std::optional<MoodSample> moodSample{};
        // The listener position, used to place the mood cave sound in the world.
        glm::vec3 listenerPosition{0.0F};
        // ticks elapsed since the last call; the scheduler counts in ticks.
        int ticks = 1;
    };

    // Drive one client-tick of the ambient/music scheduler: fade/replace the
    // situational music voice, advance the cave-mood accumulator and play its
    // trigger, and cross-fade the biome ambient loop. Pure client presentation;
    // routes music→Music, loop/mood→Ambient (AU-1 buses).
    void tickAmbientMusic(const AmbientMusicContext& context);

    // Jukebox: play a music-disc clip at `position` on the Record bus. Records
    // ignore the normal attenuation ceiling the way vanilla does (audible across
    // the jukebox's play radius), so this uses a wider max distance.
    void playRecord(std::string_view event, const glm::vec3& position);
    // Stop a currently-playing record (hopper removes the disc / another disc
    // inserted). No-op when nothing is playing.
    void stopRecord();

    // Whether a situational-music voice is currently sounding. Exposed for the
    // "at most one music at a time" invariant and for tests.
    [[nodiscard]] bool musicPlaying() const;
    // The situation whose music is currently playing (None when silent).
    [[nodiscard]] MusicSituation musicSituation() const;
    // The cave-mood accumulator's current level, 0..1. It advances once per
    // game-tick that tickAmbientMusic processes, so it is the device-free witness
    // that the scheduler steps by `ticks` (linear, zero when ticks==0) rather than
    // once per call/frame. Not part of the audio contract — for tests only.
    [[nodiscard]] float moodLevelForTest() const;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation;
};

} // namespace mc::audio
