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

    // ---- AU-3 ①: a streamed voice (music / biome loop) carries ONLY the
    // sub-category gain, NEVER master. Master is the engine's global endpoint
    // gain applied once; folding it into the per-voice volume too gave master²
    // (music barely audible at the default master 0.8). This must stay decoupled
    // from master at every master level. ----
    {
        using mc::audio::ambientSoundCategory;
        using mc::audio::musicSoundCategory;
        // master 0.5 (set above), Music gain 1.0 → streamed voice volume 1.0, not
        // 0.5. The engine multiplies master in globally at runtime.
        assert(nearlyEqual(audio.streamedVoiceVolume(musicSoundCategory()), 1.0F));
        audio.setCategoryVolume(musicSoundCategory(), 0.6F);
        // Now the per-voice volume is the Music gain alone (0.6), independent of
        // master: sweep master and the streamed-voice volume must not move.
        for (const float master : {0.0F, 0.2F, 0.5F, 0.8F, 1.0F}) {
            audio.setMasterVolume(master);
            assert(nearlyEqual(audio.streamedVoiceVolume(musicSoundCategory()), 0.6F));
        }
        // An ambient loop on its own bus behaves the same, isolated from Music.
        audio.setCategoryVolume(ambientSoundCategory(), 0.3F);
        assert(nearlyEqual(audio.streamedVoiceVolume(ambientSoundCategory()), 0.3F));
        assert(nearlyEqual(audio.streamedVoiceVolume(musicSoundCategory()), 0.6F));
        // Restore the state the following assertions rely on: master 0.5 (as it
        // was before this block) and the streamed buses back to unity gain.
        audio.setMasterVolume(0.5F);
        audio.setCategoryVolume(musicSoundCategory(), 1.0F);
        audio.setCategoryVolume(ambientSoundCategory(), 1.0F);
    }

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

    // ---- AU-3 ⑥: distance attenuation reaches ZERO at the ceiling (no穿地听声).
    // The old miniaudio default (inverse) clamped distance to max and left a
    // ~0.108 non-zero plateau, so a mob hundreds of blocks away / deep underground
    // stayed audible. play() now sets the LINEAR model with rolloff = 1.0, whose
    // gain = 1 − (clamp(d,min,max)−min)/(max−min): full up close, converging
    // smoothly to exactly 0 at maxDistance from the formula itself (not a boundary
    // guard). The helper mirrors that curve so it is device-free verifiable. ----
    {
        using mc::audio::cullByDistance;
        using mc::audio::linearAttenuationGain;
        constexpr float minD = 4.0F;  // play()'s min_distance
        constexpr float maxD = 48.0F; // ordinary max_distance
        // Near field: full volume at and inside the reference distance.
        assert(nearlyEqual(linearAttenuationGain(0.0F, minD, maxD), 1.0F));
        assert(nearlyEqual(linearAttenuationGain(minD, minD, maxD), 1.0F));
        // 4.5-block mining stays near full — with rolloff 1.0 the gain there is
        // 1 − 0.5/44 ≈ 0.989, well within reach (inverse's near field was worse,
        // which is exactly why min_distance = 4 was chosen).
        assert(linearAttenuationGain(4.5F, minD, maxD) > 0.98F);
        // Exact expected values at two interior points (formula, rolloff 1.0).
        assert(nearlyEqual(linearAttenuationGain(26.0F, minD, maxD), 1.0F - 22.0F / 44.0F)); // 0.5
        assert(nearlyEqual(linearAttenuationGain(4.5F, minD, maxD), 1.0F - 0.5F / 44.0F));
        // At the ceiling the gain is EXACTLY zero — and it gets there from the
        // formula (clamp(max)==max → 1 − 1 = 0), so approaching max is continuous,
        // not a 0.25→0 jump. Beyond max the clamp keeps it at 0. Inverse left 0.108.
        assert(nearlyEqual(linearAttenuationGain(maxD, minD, maxD), 0.0F));
        assert(linearAttenuationGain(47.9F, minD, maxD) < 0.01F); // continuous → ~0 just inside
        assert(nearlyEqual(linearAttenuationGain(60.0F, minD, maxD), 0.0F));
        assert(nearlyEqual(linearAttenuationGain(500.0F, minD, maxD), 0.0F));
        // Strictly monotonically decreasing between min and max.
        float previous = 2.0F;
        for (float dist = minD; dist <= maxD; dist += 1.0F) {
            const float g = linearAttenuationGain(dist, minD, maxD);
            assert(g <= previous + 1e-6F);
            previous = g;
        }
        // Records carry further (64) but still reach zero at their own ceiling.
        assert(linearAttenuationGain(50.0F, minD, 64.0F) > 0.0F);
        assert(nearlyEqual(linearAttenuationGain(64.0F, minD, 64.0F), 0.0F));

        // The cull predicate play() uses: a source past the ceiling is skipped
        // entirely (never decoded) as a pure optimisation/backstop; inside it is
        // kept and the linear curve above does the audible attenuation.
        assert(cullByDistance(48.01F, maxD));
        assert(cullByDistance(1000.0F, maxD));
        assert(!cullByDistance(47.9F, maxD));
        assert(!cullByDistance(0.0F, maxD));
    }

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

    // ---- AU-3 ②: tickAmbientMusic advances by `ticks`, linearly, and ticks=0
    // advances nothing. The cave-mood accumulator is the device-free witness: a
    // dark sample fills it by 1/tickDelay per game-tick, so after T processed
    // ticks the level is the same regardless of how the T were split across
    // calls/frames. This is the FPS-decoupling: the scheduler steps per tick, not
    // per call. moodLevelForTest() exposes that internal level for the test. ----
    {
        auto darkCtx = []() {
            AudioSystem::AmbientMusicContext c;
            c.situation = MusicSituation::Game;
            c.listenerPosition = {0.0F, 64.0F, 0.0F};
            mc::audio::MoodSample dark{};
            dark.offsetX = 4.0; // off the player so the trigger has a direction
            dark.skyBrightness = 0;
            dark.blockBrightness = 0;
            c.moodSample = dark;
            return c;
        };

        // ticks=0 never advances: a system fed only zero-tick frames keeps the
        // mood at exactly zero forever (a frame with no tick boundary does
        // nothing — this is what decouples the scheduler from the frame rate).
        {
            AudioSystem zero(provider, 1.0F);
            auto ctx = darkCtx();
            ctx.ticks = 0;
            for (int i = 0; i < 10000; ++i) {
                zero.tickAmbientMusic(ctx);
            }
            assert(nearlyEqual(zero.moodLevelForTest(), 0.0F));
        }

        // Linear: one call of T ticks == N calls summing to T ticks. Keep T well
        // under the ~6000-tick trigger threshold so no reset intervenes.
        const int total = 900;
        float bulkLevel = 0.0F;
        {
            AudioSystem bulk(provider, 1.0F);
            auto ctx = darkCtx();
            ctx.ticks = total;
            bulk.tickAmbientMusic(ctx);
            bulkLevel = bulk.moodLevelForTest();
            assert(bulkLevel > 0.0F); // it actually moved
        }
        // Split as 900 single-tick calls.
        {
            AudioSystem split1(provider, 1.0F);
            auto ctx = darkCtx();
            ctx.ticks = 1;
            for (int i = 0; i < total; ++i) {
                split1.tickAmbientMusic(ctx);
            }
            assert(nearlyEqual(split1.moodLevelForTest(), bulkLevel));
        }
        // Split as 300 three-tick calls, and one lopsided 300+600 split. All must
        // land on the same level as the single 900-tick call.
        {
            AudioSystem split3(provider, 1.0F);
            auto ctx = darkCtx();
            ctx.ticks = 3;
            for (int i = 0; i < total / 3; ++i) {
                split3.tickAmbientMusic(ctx);
            }
            assert(nearlyEqual(split3.moodLevelForTest(), bulkLevel));
        }
        {
            AudioSystem lop(provider, 1.0F);
            auto a = darkCtx();
            a.ticks = 300;
            auto b = darkCtx();
            b.ticks = 600;
            lop.tickAmbientMusic(a);
            lop.tickAmbientMusic(b);
            assert(nearlyEqual(lop.moodLevelForTest(), bulkLevel));
        }
    }

    return 0;
}
