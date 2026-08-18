#pragma once

// The byte encoding of the server→client mirror boundary (N4 slice 2): the
// per-tick snapshots the renderer reads. Same framing as the command codec —
// [tag u8][size u32][payload] — so a future transport carries both intents and
// mirrors in one stream, with the same forward-compatibility (an unknown tag
// is skipped by size).

#include "gameplay/EntityRenderSnapshot.hpp"
#include "gameplay/PlayerTickSnapshot.hpp"
#include "gameplay/WorldSnapshot.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace mc::gameplay {

// The two snapshots a transport can carry, discriminated the same way the
// command queue discriminates its intents.
using PublishedSnapshot = std::variant<PlayerTickSnapshot, WorldSnapshot>;

// Encodes one snapshot into its framed byte form.
[[nodiscard]] std::vector<std::uint8_t> encodeSnapshot(const PublishedSnapshot& snapshot);

// Decodes one snapshot from the start of `bytes`, or nullopt when the frame is
// truncated, holds an unknown snapshot tag, or carries an unknown item.
[[nodiscard]] std::optional<PublishedSnapshot> decodeSnapshot(std::span<const std::uint8_t> bytes);

// The total bytes the snapshot at the start of `bytes` occupies, including its
// frame header — how a stream is split back into snapshots. 0 when truncated.
[[nodiscard]] std::size_t encodedSnapshotSize(std::span<const std::uint8_t> bytes);

// The entity render snapshot (creatures, drops, falling blocks) the renderer
// draws. It carries its own tag, after the event tags, so it rides the same
// stream as the other messages without colliding. Unknown species/items/blocks
// (a newer build's content) are skipped, the same forward-compatibility the
// other codecs apply.
[[nodiscard]] std::vector<std::uint8_t> encodeEntitySnapshot(const EntityRenderSnapshot& snapshot);
[[nodiscard]] std::optional<EntityRenderSnapshot> decodeEntitySnapshot(
    std::span<const std::uint8_t> bytes);

}  // namespace mc::gameplay
