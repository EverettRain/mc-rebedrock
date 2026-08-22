#include "audio/AmbientMusicScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mc::audio {

std::int32_t MusicScheduler::nextInt(std::int32_t min, std::int32_t max) {
    if (max <= min) {
        return min;
    }
    // Numerical Recipes LCG, the same generator the rest of the audio system
    // uses; the top bits are the strong ones.
    randomState_ = randomState_ * 1664525U + 1013904223U;
    const std::uint32_t span = static_cast<std::uint32_t>(max - min) + 1U;
    return min + static_cast<std::int32_t>((randomState_ >> 8) % span);
}

MusicCommand MusicScheduler::tick(MusicSituation situation, bool currentMusicActive) {
    // MusicManager#tick: if a song is playing, first see whether the situation
    // now wants a different, replace-capable track. When it does, vanilla stops
    // the current song and shortens the delay to [0, minDelay/2].
    if (playing_ != MusicSituation::None) {
        const MusicTrack& wanted = track(situation);
        const bool differentSituation = situation != playing_;
        if (differentSituation && wanted.hasMusic() && wanted.replaceCurrentMusic) {
            playing_ = situation;
            nextSongDelay_ = std::numeric_limits<std::int32_t>::max();
            return {MusicAction::ReplaceTrack, wanted.event};
        }
        // The current song ended on its own: drop it and roll the delay to the
        // next one for whatever situation we are now in.
        if (!currentMusicActive) {
            playing_ = MusicSituation::None;
            const MusicTrack& current = track(situation);
            nextSongDelay_ = current.hasMusic() ? nextInt(current.minDelay, current.maxDelay) : 100;
        } else {
            // Still playing and no replace wanted — nothing to do.
            return {MusicAction::Nothing, {}};
        }
    }

    const MusicTrack& current = track(situation);
    if (!current.hasMusic()) {
        // No music in this situation: hold the countdown at its floor (100) so a
        // song does not fire the instant we re-enter a musical situation.
        nextSongDelay_ = std::max(nextSongDelay_, 100);
        return {MusicAction::Nothing, {}};
    }

    // Count down to the next song; start it when the timer expires.
    nextSongDelay_ = std::min(nextSongDelay_, nextInt(current.minDelay, current.maxDelay));
    if (nextSongDelay_-- <= 0) {
        playing_ = situation;
        nextSongDelay_ = std::numeric_limits<std::int32_t>::max();
        return {MusicAction::StartTrack, current.event};
    }
    return {MusicAction::Nothing, {}};
}

std::optional<MoodTrigger> MoodAccumulator::tick(const MoodSample& sample) {
    // BiomeAmbientSoundsHandler#tick mood branch, verbatim arithmetic.
    if (sample.skyBrightness > 0) {
        moodiness_ -= static_cast<float>(sample.skyBrightness) / 15.0F * kSkyMoodRecoveryRate;
    } else {
        moodiness_ -=
            static_cast<float>(sample.blockBrightness - 1) / static_cast<float>(settings_.tickDelay);
    }

    if (moodiness_ >= 1.0F) {
        // The sound plays soundPositionOffset blocks beyond the sampled block,
        // along the player→block direction. When the sample sits on the player
        // (distance 0) vanilla would divide by zero; guard it and place the sound
        // straight up, which is what a zero-length direction degenerates to.
        const double distance = std::sqrt(sample.offsetX * sample.offsetX +
                                          sample.offsetY * sample.offsetY +
                                          sample.offsetZ * sample.offsetZ);
        MoodTrigger trigger;
        trigger.event = settings_.event;
        if (distance > 1e-6) {
            const double sourceDistance = distance + settings_.soundPositionOffset;
            trigger.positionX = sample.offsetX / distance * sourceDistance;
            trigger.positionY = sample.offsetY / distance * sourceDistance;
            trigger.positionZ = sample.offsetZ / distance * sourceDistance;
        } else {
            trigger.positionY = settings_.soundPositionOffset;
        }
        moodiness_ = 0.0F;
        return trigger;
    }

    moodiness_ = std::max(moodiness_, 0.0F);
    return std::nullopt;
}

const MusicScheduler::Table& defaultMusicTable() {
    // Built-in situational music. The event ids are vanilla's music.* events; a
    // pack that ships those OGGs plays them, one that does not stays silent
    // (记账不硬造). Delays mirror Music entries in vanilla's biome/menu configs:
    // the menu/creative sets replace on entry, the overworld GAME set lets the
    // current song finish (replaceCurrentMusic = false) and spaces songs out.
    static const MusicScheduler::Table table = [] {
        MusicScheduler::Table entries{};
        // MENU: MENU music, 20..600 s → replace on entry (title screen).
        entries[static_cast<std::size_t>(MusicSituation::Menu)] =
            MusicTrack{"music.menu", 20 * 20, 600 * 20, true};
        // CREATIVE: creative-mode music set, replace on entry.
        entries[static_cast<std::size_t>(MusicSituation::Creative)] =
            MusicTrack{"music.creative", 12000, 24000, true};
        // GAME: overworld day music — the current song finishes before the next
        // (replaceCurrentMusic = false), 12000..24000 ticks apart.
        entries[static_cast<std::size_t>(MusicSituation::Game)] =
            MusicTrack{"music.game", 12000, 24000, false};
        // UNDER_WATER: submerged ambience music, replace on entry.
        entries[static_cast<std::size_t>(MusicSituation::Underwater)] =
            MusicTrack{"music.under_water", 12000, 24000, true};
        // None keeps its default empty track (no music).
        return entries;
    }();
    return table;
}

} // namespace mc::audio
