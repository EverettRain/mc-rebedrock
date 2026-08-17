#include "gameplay/GameEventCodec.hpp"

#include "gameplay/StreamCodec.hpp"

#include <cstdint>
#include <stdexcept>

namespace mc::gameplay {
namespace {

// Event tags, kept after the snapshot tags (13/14) so a mixed stream never
// routes an event to the wrong decoder.
constexpr std::uint8_t kWorldEditTag = 15U;
constexpr std::uint8_t kSoundTag = 16U;
constexpr std::uint8_t kParticleTag = 17U;
constexpr std::uint8_t kPlayerDiedTag = 18U;
constexpr std::uint8_t kClientActionTag = 19U;

void appendBool(std::vector<std::uint8_t>& bytes, bool value) {
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(value ? 1 : 0));
}
[[nodiscard]] bool readBool(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    return persistence::readInteger<std::uint8_t>(bytes, cursor) != 0;
}

void appendWorldEdit(std::vector<std::uint8_t>& bytes, const WorldEditEvent& event) {
    persistence::appendInteger(bytes, static_cast<std::int32_t>(event.x));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(event.y));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(event.z));
    codec::appendBlockState(bytes, event.state);
    appendBool(bytes, event.immediate);
}

[[nodiscard]] std::optional<WorldEditEvent> readWorldEdit(std::span<const std::uint8_t> bytes,
                                                          std::size_t& cursor) {
    WorldEditEvent event;
    event.x = persistence::readInteger<std::int32_t>(bytes, cursor);
    event.y = persistence::readInteger<std::int32_t>(bytes, cursor);
    event.z = persistence::readInteger<std::int32_t>(bytes, cursor);
    const auto state = codec::readBlockState(bytes, cursor);
    if (!state.has_value()) {
        return std::nullopt;
    }
    event.state = *state;
    event.immediate = readBool(bytes, cursor);
    return event;
}

void appendSound(std::vector<std::uint8_t>& bytes, const SoundEvent& event) {
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(event.kind));
    codec::appendVec3(bytes, event.position);
    codec::appendBlock(bytes, event.block);
    codec::appendEntityType(bytes, event.species);
    persistence::appendFloat(bytes, event.volume);
    appendBool(bytes, event.heavy);
}

[[nodiscard]] std::optional<SoundEvent> readSound(std::span<const std::uint8_t> bytes,
                                                  std::size_t& cursor) {
    SoundEvent event;
    event.kind = static_cast<SoundEventKind>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    event.position = codec::readVec3(bytes, cursor);
    const auto block = codec::readBlock(bytes, cursor);
    if (!block.has_value()) {
        return std::nullopt;
    }
    event.block = *block;
    event.species = codec::readEntityType(bytes, cursor);
    event.volume = persistence::readFloat(bytes, cursor);
    event.heavy = readBool(bytes, cursor);
    return event;
}

void appendParticle(std::vector<std::uint8_t>& bytes, const ParticleEvent& event) {
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(event.kind));
    codec::appendVec3(bytes, event.position);
    codec::appendBlock(bytes, event.block);
}

[[nodiscard]] std::optional<ParticleEvent> readParticle(std::span<const std::uint8_t> bytes,
                                                        std::size_t& cursor) {
    ParticleEvent event;
    event.kind =
        static_cast<ParticleEventKind>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    event.position = codec::readVec3(bytes, cursor);
    const auto block = codec::readBlock(bytes, cursor);
    if (!block.has_value()) {
        return std::nullopt;
    }
    event.block = *block;
    return event;
}

void appendClientAction(std::vector<std::uint8_t>& bytes, const ClientActionEvent& event) {
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(event.kind));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(event.screen));
    codec::appendIvec3(bytes, event.position);
    appendBool(bytes, event.hasPosition);
}

[[nodiscard]] ClientActionEvent readClientAction(std::span<const std::uint8_t> bytes,
                                                 std::size_t& cursor) {
    ClientActionEvent event;
    event.kind = static_cast<ClientActionEventKind>(
        persistence::readInteger<std::uint8_t>(bytes, cursor));
    event.screen =
        static_cast<ContainerScreen>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    event.position = codec::readIvec3(bytes, cursor);
    event.hasPosition = readBool(bytes, cursor);
    return event;
}

}  // namespace

std::vector<std::uint8_t> encodeGameEvent(const GameEvent& event) {
    std::vector<std::uint8_t> bytes;
    std::visit(
        [&](const auto& specific) {
            using T = std::decay_t<decltype(specific)>;
            if constexpr (std::is_same_v<T, WorldEditEvent>) {
                codec::appendFrame(bytes, kWorldEditTag,
                                   [&] { appendWorldEdit(bytes, specific); });
            } else if constexpr (std::is_same_v<T, SoundEvent>) {
                codec::appendFrame(bytes, kSoundTag, [&] { appendSound(bytes, specific); });
            } else if constexpr (std::is_same_v<T, ParticleEvent>) {
                codec::appendFrame(bytes, kParticleTag,
                                   [&] { appendParticle(bytes, specific); });
            } else if constexpr (std::is_same_v<T, PlayerDiedEvent>) {
                codec::appendFrame(bytes, kPlayerDiedTag, [] {});
            } else if constexpr (std::is_same_v<T, ClientActionEvent>) {
                codec::appendFrame(bytes, kClientActionTag,
                                   [&] { appendClientAction(bytes, specific); });
            }
        },
        event);
    return bytes;
}

std::size_t encodedGameEventSize(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < codec::kFrameHeaderBytes) {
        return 0;
    }
    std::size_t cursor = 1;  // skip the tag
    const auto size = persistence::readInteger<std::uint32_t>(bytes, cursor);
    if (bytes.size() < cursor + size) {
        return 0;  // truncated frame: wait for more bytes
    }
    return cursor + size;
}

std::optional<GameEvent> decodeGameEvent(std::span<const std::uint8_t> bytes) {
    try {
        std::size_t cursor = 0;
        const auto frame = codec::readFrame(bytes, cursor);
        if (!frame.has_value()) {
            return std::nullopt;
        }
        const auto [tag, payloadEnd] = *frame;
        std::optional<GameEvent> decoded;
        if (tag == kWorldEditTag) {
            decoded = readWorldEdit(bytes, cursor);
        } else if (tag == kSoundTag) {
            decoded = readSound(bytes, cursor);
        } else if (tag == kParticleTag) {
            decoded = readParticle(bytes, cursor);
        } else if (tag == kPlayerDiedTag) {
            decoded = PlayerDiedEvent{};
        } else if (tag == kClientActionTag) {
            decoded = readClientAction(bytes, cursor);
        } else {
            // An unknown event tag (a newer build's event): skippable.
            return std::nullopt;
        }
        if (!decoded.has_value() || cursor > payloadEnd) {
            return std::nullopt;
        }
        return decoded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace mc::gameplay
