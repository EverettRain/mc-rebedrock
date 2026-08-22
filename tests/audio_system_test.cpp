#include "assets/ResourceProvider.hpp"
#include "audio/AudioSystem.hpp"
#include "audio/SoundCategory.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <vector>

namespace {

// A provider that owns no assets: locate/exists/readBytes all come back empty.
// The audio system builds cleanly on it (an empty SoundRegistry, no device in a
// headless container), which is all the per-category gain state under test
// needs — no clip is ever decoded.
class EmptyResourceProvider final : public mc::assets::ResourceProvider {
  public:
    [[nodiscard]] std::filesystem::path
    locate(const mc::assets::ResourceLocation&) const override {
        return {};
    }
    [[nodiscard]] bool exists(const mc::assets::ResourceLocation&) const override { return false; }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
};

[[nodiscard]] bool nearlyEqual(float lhs, float rhs) { return std::fabs(lhs - rhs) < 1e-5F; }

} // namespace

int main() {
    using mc::audio::AudioSystem;
    using mc::audio::BlockSoundFamily;
    using mc::audio::blockSoundFamily;
    using mc::audio::SoundCategory;
    using mc::world::Block;

    // ---- Block sound families (unchanged behaviour) ----
    assert(blockSoundFamily(Block::Stone) == BlockSoundFamily::Stone);
    assert(blockSoundFamily(Block::OakLog) == BlockSoundFamily::Wood);
    assert(blockSoundFamily(Block::OakLeaves) == BlockSoundFamily::Grass);
    assert(blockSoundFamily(Block::RedSand) == BlockSoundFamily::Sand);
    assert(blockSoundFamily(Block::Gravel) == BlockSoundFamily::Gravel);
    assert(blockSoundFamily(Block::RedWool) == BlockSoundFamily::Cloth);
    assert(blockSoundFamily(Block::Glass) == BlockSoundFamily::Glass);
    assert(std::string_view{mc::audio::blockSoundFamilyName(BlockSoundFamily::Glass)} == "glass");
    assert(std::string_view{mc::audio::blockSoundFamilyName(BlockSoundFamily::Wood)} == "wood");
    assert(std::string_view{mc::audio::blockSoundFamilyName(BlockSoundFamily::Cloth)} == "wool");

    // ---- SoundCategory: 11 slots (10 vanilla categories + the Count sentinel),
    // names round-trip, unknown names reject ----
    static_assert(mc::audio::kSoundCategoryCount == 10U,
                  "the ten vanilla SoundSource categories, Master included");
    assert(mc::audio::soundCategoryName(SoundCategory::Master) == "master");
    assert(mc::audio::soundCategoryName(SoundCategory::Hostile) == "hostile");
    assert(mc::audio::soundCategoryName(SoundCategory::Voice) == "voice");
    for (std::size_t index = 0; index < mc::audio::kSoundCategoryCount; ++index) {
        const auto category = static_cast<SoundCategory>(index);
        assert(mc::audio::soundCategoryFromName(mc::audio::soundCategoryName(category)) == category);
    }
    assert(mc::audio::soundCategoryFromName("nonsense") == SoundCategory::Count);
    assert(mc::audio::soundCategoryFromName("") == SoundCategory::Count);

    // Defaults are all 1.
    const auto defaults = mc::audio::defaultSoundCategoryVolumes();
    for (const float volume : defaults) {
        assert(nearlyEqual(volume, 1.0F));
    }

    // ---- Per-category gain state (no audio device required) ----
    const EmptyResourceProvider provider;
    AudioSystem audio(provider, 1.0F);

    // Master defaults through the constructor argument; sub-categories default 1.
    assert(nearlyEqual(audio.categoryVolume(SoundCategory::Master), 1.0F));
    assert(nearlyEqual(audio.categoryVolume(SoundCategory::Block), 1.0F));

    // Effective volume = master × category × event. All 1 → passthrough.
    assert(nearlyEqual(audio.effectiveVolume(SoundCategory::Block, 0.8F), 0.8F));

    // Muting one category silences only itself: Block=0 leaves Hostile untouched.
    audio.setCategoryVolume(SoundCategory::Block, 0.0F);
    assert(nearlyEqual(audio.effectiveVolume(SoundCategory::Block, 1.0F), 0.0F));
    assert(nearlyEqual(audio.effectiveVolume(SoundCategory::Hostile, 1.0F), 1.0F));
    assert(nearlyEqual(audio.effectiveVolume(SoundCategory::Weather, 0.5F), 0.5F));

    // The layers multiply: master 0.5, Hostile 0.4, event 0.5 → 0.1.
    audio.setMasterVolume(0.5F);
    audio.setCategoryVolume(SoundCategory::Hostile, 0.4F);
    assert(nearlyEqual(audio.effectiveVolume(SoundCategory::Hostile, 0.5F), 0.5F * 0.4F * 0.5F));
    // Block is still muted, independent of the master change.
    assert(nearlyEqual(audio.effectiveVolume(SoundCategory::Block, 1.0F), 0.0F));
    // A Master-routed play folds master's own gain in: master × event, no double
    // attenuation.
    assert(nearlyEqual(audio.effectiveVolume(SoundCategory::Master, 0.8F), 0.5F * 0.8F));

    // setMasterVolume keeps the Master slot mirrored.
    assert(nearlyEqual(audio.categoryVolume(SoundCategory::Master), 0.5F));

    // Clamping: out-of-range gains saturate to [0, 1].
    audio.setCategoryVolume(SoundCategory::Player, 2.0F);
    assert(nearlyEqual(audio.categoryVolume(SoundCategory::Player), 1.0F));
    audio.setCategoryVolume(SoundCategory::Player, -1.0F);
    assert(nearlyEqual(audio.categoryVolume(SoundCategory::Player), 0.0F));

    // Bulk apply from an options table.
    mc::audio::SoundCategoryVolumes table = mc::audio::defaultSoundCategoryVolumes();
    table[static_cast<std::size_t>(SoundCategory::Weather)] = 0.25F;
    table[static_cast<std::size_t>(SoundCategory::Master)] = 0.6F;
    audio.setCategoryVolumes(table);
    assert(nearlyEqual(audio.categoryVolume(SoundCategory::Weather), 0.25F));
    assert(nearlyEqual(audio.categoryVolume(SoundCategory::Master), 0.6F));

    // ---- Directional Audio toggle ----
    assert(audio.directionalAudio()); // on by default (vanilla parity)
    audio.setDirectionalAudio(false);
    assert(!audio.directionalAudio());
    audio.setDirectionalAudio(true);
    assert(audio.directionalAudio());

    // ---- AU-2: ambient/music integration (no assets on this provider, so the
    // scheduler runs but no voice actually sounds) ----
    using mc::audio::MusicSituation;
    // Nothing playing before the first tick.
    assert(!audio.musicPlaying());
    assert(audio.musicSituation() == MusicSituation::None);

    // Driving the scheduler with a large tick budget reaches a StartTrack
    // decision; with no music asset the voice never becomes audible, so
    // musicPlaying() (which also requires the voice to be sounding) stays false —
    // this is the "缺资产则记账不硬造" behaviour, not a crash.
    AudioSystem::AmbientMusicContext context;
    context.situation = MusicSituation::Game;
    context.ticks = 2000; // enough ticks to clear the start delay
    context.listenerPosition = {0.0F, 64.0F, 0.0F};
    audio.tickAmbientMusic(context); // must not crash without assets

    // A mood sample in the dark drives the accumulator through the audio system;
    // over enough ticks it would emit a cave sound (again silent without assets).
    // Just prove the call path is safe.
    mc::audio::MoodSample dark{};
    dark.offsetX = 4.0;
    dark.skyBrightness = 0;
    dark.blockBrightness = 0;
    context.moodSample = dark;
    context.ticks = 1;
    for (int i = 0; i < 10; ++i) {
        audio.tickAmbientMusic(context);
    }

    // Records and their stop are safe to call with no jukebox asset.
    audio.playRecord("music_disc.cat", {1.0F, 64.0F, 1.0F});
    audio.stopRecord();

    return 0;
}
