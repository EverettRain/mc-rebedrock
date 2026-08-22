#include "audio/AmbientMusicScheduler.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <optional>

namespace {

using mc::audio::MoodAccumulator;
using mc::audio::MoodSample;
using mc::audio::MoodSettings;
using mc::audio::MusicAction;
using mc::audio::MusicScheduler;
using mc::audio::MusicSituation;
using mc::audio::MusicTrack;

MusicScheduler::Table makeTable() {
    MusicScheduler::Table table{};
    // Game: does NOT replace, short delays so the test reaches a start quickly.
    table[static_cast<std::size_t>(MusicSituation::Game)] =
        MusicTrack{"music.game", 5, 10, /*replace=*/false};
    // Menu: replaces on entry.
    table[static_cast<std::size_t>(MusicSituation::Menu)] =
        MusicTrack{"music.menu", 5, 10, /*replace=*/true};
    // Creative: replaces on entry.
    table[static_cast<std::size_t>(MusicSituation::Creative)] =
        MusicTrack{"music.creative", 5, 10, /*replace=*/true};
    // None left empty (no music).
    return table;
}

// Run the scheduler until it emits a Start/Replace, returning the command; caps
// iterations so a broken scheduler that never starts fails the assert below
// rather than looping forever.
mc::audio::MusicCommand runUntilStart(MusicScheduler& scheduler, MusicSituation situation,
                                      int maxTicks = 1000) {
    for (int i = 0; i < maxTicks; ++i) {
        const auto command = scheduler.tick(situation, scheduler.isPlaying());
        if (command.action != MusicAction::Nothing) {
            return command;
        }
    }
    return {MusicAction::Nothing, {}};
}

} // namespace

int main() {
    const auto table = makeTable();

    // ---- Single-track: at most one music plays; a start marks it playing ----
    {
        MusicScheduler scheduler{table};
        assert(!scheduler.isPlaying());
        const auto command = runUntilStart(scheduler, MusicSituation::Game);
        assert(command.action == MusicAction::StartTrack);
        assert(command.event == "music.game");
        assert(scheduler.isPlaying());
        assert(scheduler.playingSituation() == MusicSituation::Game);

        // While a song is active and the situation is unchanged, no second song
        // starts — "at most one music at a time".
        for (int i = 0; i < 100; ++i) {
            const auto again = scheduler.tick(MusicSituation::Game, /*active=*/true);
            assert(again.action == MusicAction::Nothing);
        }
        assert(scheduler.playingSituation() == MusicSituation::Game);
    }

    // ---- A non-replacing song is allowed to finish before the next one ----
    {
        MusicScheduler scheduler{table};
        runUntilStart(scheduler, MusicSituation::Game);
        assert(scheduler.isPlaying());
        // Game does not replace: entering another Game tick with the song still
        // active never restarts.
        const auto held = scheduler.tick(MusicSituation::Game, /*active=*/true);
        assert(held.action == MusicAction::Nothing);
        // Once the voice ends (active=false), the scheduler drops it and later
        // schedules the next Game song.
        const auto ended = scheduler.tick(MusicSituation::Game, /*active=*/false);
        assert(ended.action == MusicAction::Nothing);
        assert(!scheduler.isPlaying());
        const auto next = runUntilStart(scheduler, MusicSituation::Game);
        assert(next.action == MusicAction::StartTrack);
    }

    // ---- Replace: a replace-capable situation change stops the current song
    // and starts immediately ----
    {
        MusicScheduler scheduler{table};
        runUntilStart(scheduler, MusicSituation::Game);
        assert(scheduler.playingSituation() == MusicSituation::Game);
        // Switch to Menu (replaceCurrentMusic = true) while the game song is
        // still active: the very next tick replaces it.
        const auto replace = scheduler.tick(MusicSituation::Menu, /*active=*/true);
        assert(replace.action == MusicAction::ReplaceTrack);
        assert(replace.event == "music.menu");
        assert(scheduler.playingSituation() == MusicSituation::Menu);
    }

    // ---- No music in a situation: never starts anything ----
    {
        MusicScheduler scheduler{table};
        for (int i = 0; i < 5000; ++i) {
            const auto command = scheduler.tick(MusicSituation::None, /*active=*/false);
            assert(command.action == MusicAction::Nothing);
        }
        assert(!scheduler.isPlaying());
    }

    // ---- Mood: darkness fills, light drains; only fills in the dark ----
    {
        MoodAccumulator accumulator{MoodSettings{"ambient.cave", 6000, 8, 2.0}};
        // Pitch black (sky 0, block 0): moodiness += (0 - 1) / -6000... actually
        // -= (block-1)/tickDelay = -(-1)/6000 = +1/6000 per tick. So ~6000 ticks
        // to fill. Feed enough and expect a trigger, then a reset to 0.
        std::optional<mc::audio::MoodTrigger> trigger;
        MoodSample dark{};
        dark.offsetX = 3.0;
        dark.offsetY = -1.0;
        dark.offsetZ = 0.0;
        dark.skyBrightness = 0;
        dark.blockBrightness = 0;
        int ticksToFill = 0;
        for (int i = 0; i < 20000 && !trigger; ++i) {
            trigger = accumulator.tick(dark);
            ++ticksToFill;
        }
        assert(trigger.has_value());
        assert(trigger->event == "ambient.cave");
        // Reset after trigger.
        assert(std::fabs(accumulator.moodiness()) < 1e-6F);
        // ~6000 ticks (allow slack for float accumulation).
        assert(ticksToFill > 5000 && ticksToFill < 7000);
        // The sound is placed beyond the sampled block: |offset| = block distance
        // + 2.0. Sample distance is sqrt(3^2+1^2)=~3.162, so source ~5.162.
        const double px = trigger->positionX;
        const double py = trigger->positionY;
        const double pz = trigger->positionZ;
        const double sourceDistance = std::sqrt(px * px + py * py + pz * pz);
        assert(std::fabs(sourceDistance - (std::sqrt(10.0) + 2.0)) < 1e-3);
    }

    // ---- Mood: bright open sky never triggers, and drains any built-up mood ----
    {
        MoodAccumulator accumulator{MoodSettings{"ambient.cave", 6000, 8, 2.0}};
        MoodSample bright{};
        bright.skyBrightness = 15; // full daylight
        bright.blockBrightness = 15;
        for (int i = 0; i < 100000; ++i) {
            const auto trigger = accumulator.tick(bright);
            assert(!trigger.has_value()); // sky > 0 only ever drains, never fills
        }
        assert(accumulator.moodiness() <= 0.0F + 1e-6F);

        // Even lit caves (sky 0 but block light present) do not fill: block>=1
        // makes the term (block-1)/tickDelay >= 0, so moodiness never rises.
        MoodAccumulator litCave{MoodSettings{"ambient.cave", 6000, 8, 2.0}};
        MoodSample lit{};
        lit.skyBrightness = 0;
        lit.blockBrightness = 8; // a torch-lit cave
        for (int i = 0; i < 100000; ++i) {
            const auto trigger = litCave.tick(lit);
            assert(!trigger.has_value());
        }
        assert(litCave.moodiness() <= 0.0F + 1e-6F);
    }

    // ---- AU-1 bus routing: music/ambient/record go to their own sliders, NOT
    // Master (otherwise the per-category sliders could not control them) ----
    {
        assert(mc::audio::musicSoundCategory() == mc::audio::SoundCategory::Music);
        assert(mc::audio::ambientSoundCategory() == mc::audio::SoundCategory::Ambient);
        assert(mc::audio::recordSoundCategory() == mc::audio::SoundCategory::Record);
        // None of them is Master — the "Music=0 mutes music" guarantee depends on
        // this: a sound on Master would ignore its own category slider.
        assert(mc::audio::musicSoundCategory() != mc::audio::SoundCategory::Master);
        assert(mc::audio::ambientSoundCategory() != mc::audio::SoundCategory::Master);
        assert(mc::audio::recordSoundCategory() != mc::audio::SoundCategory::Master);
    }

    // ---- The built-in default table is well-formed: Game exists and does not
    // replace; Menu replaces. ----
    {
        const auto& defaults = mc::audio::defaultMusicTable();
        const auto& game = defaults[static_cast<std::size_t>(MusicSituation::Game)];
        const auto& menu = defaults[static_cast<std::size_t>(MusicSituation::Menu)];
        assert(game.hasMusic());
        assert(!game.replaceCurrentMusic);
        assert(menu.hasMusic());
        assert(menu.replaceCurrentMusic);
        assert(!defaults[static_cast<std::size_t>(MusicSituation::None)].hasMusic());
    }

    return 0;
}
