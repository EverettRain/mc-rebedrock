#pragma once

// The byte encoding of the side-effect events the simulation raises (N4 slice
// 3): world edits, sounds, particles and the player's death. Same framing as
// the command and snapshot codecs, so a future transport carries intents,
// mirrors and events in one stream with one forward-compatibility rule.

#include "gameplay/GameEvents.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace mc::gameplay {

// The four event classes, discriminated the way the command queue discriminates
// its intents.
using GameEvent =
    std::variant<WorldEditEvent, SoundEvent, ParticleEvent, PlayerDiedEvent, ClientActionEvent>;

// Encodes one event into its framed byte form.
[[nodiscard]] std::vector<std::uint8_t> encodeGameEvent(const GameEvent& event);

// Decodes one event from the start of `bytes`, or nullopt when the frame is
// truncated, holds an unknown event tag, or references an unknown block.
[[nodiscard]] std::optional<GameEvent> decodeGameEvent(std::span<const std::uint8_t> bytes);

// The total bytes the event at the start of `bytes` occupies, including its
// frame header — how a stream is split back into events. 0 when truncated.
[[nodiscard]] std::size_t encodedGameEventSize(std::span<const std::uint8_t> bytes);

}  // namespace mc::gameplay
