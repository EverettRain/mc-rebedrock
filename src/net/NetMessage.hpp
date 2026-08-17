#pragma once

// The single message stream the transport carries (stage C, slice 1).
//
// N4 gave us three byte codecs — GameCommand (client→server intents),
// PublishedSnapshot (server→client mirror) and GameEvent (server→client side
// effects) — that already share one framing ([tag u8][size u32][payload]) and
// one identifier palette, with the tag ranges deliberately partitioned so the
// three never collide in a mixed stream:
//
//   commands  tags 0 .. variant_size(GameCommand)-1          (0..12 today)
//   snapshots tags variant_size(GameCommand) ..              (13..14 today)
//   events    tags variant_size(GameCommand)+size(Snapshot).. (15..19 today)
//
// This layer is the "one stream carries intents, mirrors and events" the codec
// comments anticipated: a NetMessage is any of the three, encodeMessage frames
// it with the matching codec, and decodeMessage peeks the frame tag to route it
// back. It invents no wire format — every byte is produced by an existing N4
// codec — so the transport (loopback now, TCP next) never learns the payloads.

#include "gameplay/GameCommand.hpp"
#include "gameplay/GameCommandCodec.hpp"
#include "gameplay/GameEventCodec.hpp"
#include "gameplay/GameSnapshotCodec.hpp"
#include "gameplay/StreamCodec.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace mc::net {

// One message on the wire, discriminated by which server/client boundary it
// crosses. The three alternatives are themselves variants; because they are
// distinct types, std::visit over a NetMessage picks the boundary unambiguously.
using NetMessage =
    std::variant<gameplay::GameCommand, gameplay::PublishedSnapshot, gameplay::GameEvent>;

// The tag ranges, derived from the variant sizes so they stay consistent with
// the codecs' own scheme (each codec's base offset is the count of the
// categories before it: snapshots start where commands end, events start where
// snapshots end). If a category grows a variant, every boundary shifts together.
inline constexpr std::uint8_t kCommandTagEnd =
    static_cast<std::uint8_t>(std::variant_size_v<gameplay::GameCommand>);
inline constexpr std::uint8_t kSnapshotTagEnd = static_cast<std::uint8_t>(
    kCommandTagEnd + std::variant_size_v<gameplay::PublishedSnapshot>);
inline constexpr std::uint8_t kEventTagEnd =
    static_cast<std::uint8_t>(kSnapshotTagEnd + std::variant_size_v<gameplay::GameEvent>);

// Frames one message with the codec that matches its boundary. The result is a
// self-delimiting frame: its header carries the payload size, so a byte stream
// of many frames splits back with encodedMessageSize.
[[nodiscard]] inline std::vector<std::uint8_t> encodeMessage(const NetMessage& message) {
    return std::visit(
        [](const auto& specific) -> std::vector<std::uint8_t> {
            using T = std::decay_t<decltype(specific)>;
            if constexpr (std::is_same_v<T, gameplay::GameCommand>) {
                return gameplay::encodeGameCommand(specific);
            } else if constexpr (std::is_same_v<T, gameplay::PublishedSnapshot>) {
                return gameplay::encodeSnapshot(specific);
            } else {
                return gameplay::encodeGameEvent(specific);
            }
        },
        message);
}

// The total bytes the frame at the start of `bytes` occupies, including its
// header — how a concatenated stream is split back into messages. 0 when the
// frame is truncated. It reads only the shared header, so it is the same for
// every category and needs no routing.
[[nodiscard]] inline std::size_t encodedMessageSize(std::span<const std::uint8_t> bytes) {
    std::size_t cursor = 0;
    const auto frame = gameplay::codec::readFrame(bytes, cursor);
    if (!frame.has_value()) {
        return 0;
    }
    return frame->second;
}

// Decodes one message from the start of `bytes`, routing by the frame tag to
// the codec that owns that tag range. nullopt when the frame is truncated, its
// tag is unknown (a newer build's message), or its payload references content
// this build does not have — the same forward-compatibility every codec already
// applies.
[[nodiscard]] inline std::optional<NetMessage> decodeMessage(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < gameplay::codec::kFrameHeaderBytes) {
        return std::nullopt;
    }
    const std::uint8_t tag = bytes[0];
    if (tag < kCommandTagEnd) {
        if (auto command = gameplay::decodeGameCommand(bytes); command.has_value()) {
            return NetMessage{std::move(*command)};
        }
        return std::nullopt;
    }
    if (tag < kSnapshotTagEnd) {
        if (auto snapshot = gameplay::decodeSnapshot(bytes); snapshot.has_value()) {
            return NetMessage{std::move(*snapshot)};
        }
        return std::nullopt;
    }
    if (tag < kEventTagEnd) {
        if (auto event = gameplay::decodeGameEvent(bytes); event.has_value()) {
            return NetMessage{std::move(*event)};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace mc::net
