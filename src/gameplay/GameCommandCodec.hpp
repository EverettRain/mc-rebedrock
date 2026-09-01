#pragma once

// The byte encoding of a GameCommand — the client→server intent boundary. Today
// the command queue is the only boundary and it shares memory; this makes a
// command a byte stream so a loopback/TCP transport (stage C) can carry it the
// same way 26.1's ServerboundPlayerActionPacket travels.
//
// Every command is framed the way save blocks are: [tag: u8][payload size: u32]
// followed by the payload. A reader that does not know a tag skips its payload
// by size instead of failing — the same forward-compatibility the save format
// already proves (unknown blocks, unknown properties, missing defaults). The
// tag is the variant index, stable because it is protocol, not an enum ordinal.

#include "gameplay/GameCommand.hpp"
#include "gameplay/SessionCommand.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mc::gameplay {

// Encodes one command into its framed byte form.
[[nodiscard]] std::vector<std::uint8_t> encodeGameCommand(const GameCommand& command);

// Decodes one command from the start of `bytes`, or nullopt when the frame does
// not describe a known command (an unknown tag is skipped, a truncated frame
// fails). The transport uses encodedGameCommandSize to find the next command.
[[nodiscard]] std::optional<GameCommand> decodeGameCommand(std::span<const std::uint8_t> bytes);

// The total bytes the command at the start of `bytes` occupies, including its
// frame header — how a stream is split back into commands. Returns 0 when the
// frame is truncated.
[[nodiscard]] std::size_t encodedGameCommandSize(std::span<const std::uint8_t> bytes);

// ---- The mixed stream's frame-tag layout ----
//
// Every category on the wire gets a contiguous tag range: commands, then the
// two snapshots, then the five events, then the entity snapshot, the movement
// input and the session commands. The bases live HERE, in one place, and are
// DERIVED from the command variant's own size rather than written out as
// literals.
//
// They used to be literals spread across three files (13/14 in the snapshot
// codec, 15..19 in the event codec, 20/21/22 here), and adding one command
// variant silently pushed a command's tag onto the snapshot codec's first tag —
// only the one static_assert in NetMessage.hpp noticed, and only for the last
// of the four boundaries. Deriving them makes that class of collision
// impossible; NetMessage.hpp cross-checks each boundary against the real
// variant sizes.
inline constexpr std::uint8_t kCommandTagCount =
    static_cast<std::uint8_t>(std::variant_size_v<GameCommand>);
// PublishedSnapshot's two alternatives: the player tick and the world snapshot.
inline constexpr std::uint8_t kSnapshotTagBase = kCommandTagCount;
inline constexpr std::uint8_t kSnapshotTagCount = 2U;
// GameEvent's five alternatives: world edit, sound, particle, player died,
// client action.
inline constexpr std::uint8_t kEventTagBase =
    static_cast<std::uint8_t>(kSnapshotTagBase + kSnapshotTagCount);
inline constexpr std::uint8_t kEventTagCount = 5U;
// The entity render snapshot is a single type, not a variant, so it is one tag.
inline constexpr std::uint8_t kEntitySnapshotTag =
    static_cast<std::uint8_t>(kEventTagBase + kEventTagCount);

// The frame tag the MovementInput continuous intent occupies — one past the
// entity render snapshot, so it never collides with a command, snapshot,
// event or entity frame in the mixed stream.
inline constexpr std::uint8_t kMovementInputTag =
    static_cast<std::uint8_t>(kEntitySnapshotTag + 1);

// Encodes / decodes the per-tick MovementInput. It is not a GameCommand (the
// server applies it before the tick rather than in the late command drain), so
// it has its own frame on the same wire, tagged kMovementInputTag.
[[nodiscard]] std::vector<std::uint8_t> encodeMovementInput(const MovementInput& input);
[[nodiscard]] std::optional<MovementInput> decodeMovementInput(
    std::span<const std::uint8_t> bytes);

// The base frame tag for SessionCommand — one past the movement input, so a
// session command's tag is this plus its variant index, clear of the command,
// snapshot, event, entity and movement tags. net::kMovementTagEnd must equal this
// (a static_assert there guards the two staying aligned).
inline constexpr std::uint8_t kSessionCommandTagBase =
    static_cast<std::uint8_t>(kMovementInputTag + 1);

// Encodes / decodes a SessionCommand (respawn, game-mode switch). Like
// MovementInput it is not a GameCommand and rides its own tag range.
[[nodiscard]] std::vector<std::uint8_t> encodeSessionCommand(const SessionCommand& command);
[[nodiscard]] std::optional<SessionCommand> decodeSessionCommand(
    std::span<const std::uint8_t> bytes);

}  // namespace mc::gameplay
