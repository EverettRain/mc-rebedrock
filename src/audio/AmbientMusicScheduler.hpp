#pragma once

#include "audio/SoundCategory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mc::audio {

// The AU-1 buses the AU-2 sounds route through, stated once so the audio system
// and the tests share a single source of truth (and a slider-controllability
// regression — "music must be on the Music bus, not Master" — is a one-line
// guard). Vanilla: MusicManager → MUSIC, BiomeAmbientSoundsHandler loop/mood →
// AMBIENT, jukebox → RECORD.
[[nodiscard]] constexpr SoundCategory musicSoundCategory() { return SoundCategory::Music; }
[[nodiscard]] constexpr SoundCategory ambientSoundCategory() { return SoundCategory::Ambient; }
[[nodiscard]] constexpr SoundCategory recordSoundCategory() { return SoundCategory::Record; }

// The situational-music inputs, mirroring Minecraft#getSituationalMusic: which
// context the player is in decides which Music entry (if any) the MusicManager
// should be playing. Overworld is the only generated dimension today; Nether/End
// land with WG, and the enum leaves their slots so wiring them later touches no
// scheduler code. `None` means "no music here" (e.g. the credits screen), which
// the scheduler treats as "let the current song finish, then stay silent".
enum class MusicSituation : std::uint8_t {
    None,
    Menu,      // MENU: the title/menu screen loop
    Creative,  // CREATIVE: the creative-mode music set
    Game,      // GAME: the ordinary overworld day music (a biome may override)
    Underwater, // UNDER_WATER: submerged ambience music
    Count,
};

inline constexpr std::size_t kMusicSituationCount = static_cast<std::size_t>(MusicSituation::Count);

// One music track's scheduling parameters, mirroring net.minecraft.sounds.Music
// (sound + minDelay + maxDelay + replaceCurrentMusic). `event` is the
// SoundRegistry event id; empty means the situation has no music (the interface
// is still present so a resource pack can fill it —缺资产则记账不硬造).
struct MusicTrack final {
    std::string_view event{};
    // Mth.nextInt(random, minDelay, maxDelay) ticks between songs. Vanilla's
    // overworld GAME music uses 12000..24000 (10..20 minutes at 20 tps).
    std::int32_t minDelay = 12000;
    std::int32_t maxDelay = 24000;
    // Music#replaceCurrentMusic: when true, a situation change stops whatever is
    // playing and starts this immediately (menu/creative/nether do this); when
    // false, the current song is allowed to finish first (overworld GAME).
    bool replaceCurrentMusic = false;

    [[nodiscard]] bool hasMusic() const { return !event.empty(); }
};

// What the scheduler decides to do this tick with the music voice. The audio
// system is the one that actually starts/stops/fades a streamed voice; the
// scheduler only owns the *decision* so it can be unit-tested with no device.
enum class MusicAction : std::uint8_t {
    Nothing,      // keep doing what we're doing
    StartTrack,   // begin `event` (a new song)
    ReplaceTrack, // stop the current song immediately and begin `event`
};

struct MusicCommand final {
    MusicAction action = MusicAction::Nothing;
    std::string_view event{}; // the event to start, for Start/Replace
};

// The single-track situational-music state machine, a port of MusicManager#tick
// reduced to the decision it makes: it never plays audio itself. It holds the
// countdown to the next song and which track is playing, enforces "at most one
// music at a time", honours replaceCurrentMusic, and re-rolls the delay when a
// song ends. Deterministic: it draws from an injected LCG so a test can pin the
// exact delays.
class MusicScheduler final {
  public:
    // The music table, indexed by MusicSituation. Filled once by the integration
    // (built-in defaults + any resource-pack override); a situation whose track
    // has no event simply schedules nothing.
    using Table = std::array<MusicTrack, kMusicSituationCount>;

    explicit MusicScheduler(const Table& table, std::uint32_t seed = 0x9E3779B9U)
        : table_(table), randomState_(seed) {}

    // Advance one client tick. `situation` is this tick's context;
    // `currentMusicActive` is whether the voice the audio system last started is
    // still playing (false once it has ended). Returns the action to take.
    [[nodiscard]] MusicCommand tick(MusicSituation situation, bool currentMusicActive);

    // The situation whose track is currently playing (None when silent). Exposed
    // for the isolation invariant "only one music at a time" in tests.
    [[nodiscard]] MusicSituation playingSituation() const { return playing_; }
    [[nodiscard]] bool isPlaying() const { return playing_ != MusicSituation::None; }
    [[nodiscard]] std::int32_t nextSongDelay() const { return nextSongDelay_; }

  private:
    [[nodiscard]] const MusicTrack& track(MusicSituation situation) const {
        return table_[static_cast<std::size_t>(situation)];
    }
    // Mth.nextInt(random, min, max) inclusive.
    [[nodiscard]] std::int32_t nextInt(std::int32_t min, std::int32_t max);

    const Table& table_;
    std::uint32_t randomState_;
    MusicSituation playing_ = MusicSituation::None;
    // MusicManager#nextSongDelay, ticks until the next song may start. Seeded to
    // the vanilla STARTING_DELAY of 100 so the first song is not instant.
    std::int32_t nextSongDelay_ = 100;
};

// AmbientMoodSettings (net.minecraft.world.attribute.AmbientMoodSettings): the
// cave-mood parameters. LEGACY_CAVE_SETTINGS = {AMBIENT_CAVE, 6000, 8, 2.0}.
struct MoodSettings final {
    std::string_view event = "ambient.cave";
    // AmbientMoodSettings#tickDelay: the divisor that turns darkness into mood
    // accumulation. 6000 → full mood after ~6000 ticks of pitch black.
    std::int32_t tickDelay = 6000;
    // blockSearchExtent: the mood sample is taken from a random block within this
    // many blocks of the player's eye.
    std::int32_t blockSearchExtent = 8;
    // soundPositionOffset: the cave sound plays this far *beyond* the sampled
    // block along the player→block direction, so it feels like it comes from
    // deeper in the dark.
    double soundPositionOffset = 2.0;
};

// A brightness sample of one randomly-chosen block near the player, the input
// BiomeAmbientSoundsHandler#tick feeds its mood accumulator each tick. Both are
// 0..15 (Minecraft light levels). The integration samples the world and hands
// this in; the accumulator does only the vanilla arithmetic, so its behaviour
// is fully testable without a world.
struct MoodSample final {
    // The sampled block position relative to the player's eye, in blocks.
    double offsetX = 0.0;
    double offsetY = 0.0;
    double offsetZ = 0.0;
    // Level#getBrightness(SKY/BLOCK, pos) at that block, 0..15.
    int skyBrightness = 0;
    int blockBrightness = 0;
};

// Where and what the accumulator decided to play when the mood filled up.
struct MoodTrigger final {
    std::string_view event{};
    // The world-space offset from the player at which to place the cave sound
    // (player + this = play position), already including soundPositionOffset.
    double positionX = 0.0;
    double positionY = 0.0;
    double positionZ = 0.0;
};

// AmbientMoodSoundHandler's moodiness accumulator, ported exactly:
//   sky>0  : moodiness -= sky/15 * SKY_MOOD_RECOVERY_RATE (0.001)
//   sky==0 : moodiness -= (block - 1) / tickDelay   (dark => block low => adds)
//   >=1.0  : play cave sound at the offset, reset to 0
//   else   : clamp to >= 0
// Client-side, no determinism contract; kept faithful so the feel matches.
class MoodAccumulator final {
  public:
    static constexpr float kSkyMoodRecoveryRate = 0.001F;

    explicit MoodAccumulator(const MoodSettings& settings) : settings_(settings) {}

    // Feed one tick's brightness sample; returns a trigger when the mood fills.
    [[nodiscard]] std::optional<MoodTrigger> tick(const MoodSample& sample);

    [[nodiscard]] float moodiness() const { return moodiness_; }
    void reset() { moodiness_ = 0.0F; }

  private:
    MoodSettings settings_;
    float moodiness_ = 0.0F;
};

// Built-in music table: the overworld/menu/creative/underwater defaults. Events
// point at vanilla music event ids; if the pack ships no such clips the audio
// system logs one "missing asset" and stays silent (记账不硬造).
[[nodiscard]] const MusicScheduler::Table& defaultMusicTable();

} // namespace mc::audio
