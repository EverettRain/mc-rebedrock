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

}  // namespace mc::gameplay
