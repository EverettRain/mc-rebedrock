#include "gameplay/GameSnapshotCodec.hpp"

#include "gameplay/StreamCodec.hpp"

#include <cstdint>
#include <stdexcept>

namespace mc::gameplay {
namespace {

// Snapshot tags, kept separate from the command tags (0..12) so a mixed stream
// never routes a snapshot to the command decoder or vice versa.
constexpr std::uint8_t kPlayerTickTag = 13U;
constexpr std::uint8_t kWorldTag = 14U;

void appendBool(std::vector<std::uint8_t>& bytes, bool value) {
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(value ? 1 : 0));
}
[[nodiscard]] bool readBool(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    return persistence::readInteger<std::uint8_t>(bytes, cursor) != 0;
}

void appendSwing(std::vector<std::uint8_t>& bytes, const SwingState& swing) {
    appendBool(bytes, swing.active);
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(swing.hand));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(swing.animation));
    persistence::appendInteger(bytes, swing.sequence);
    persistence::appendInteger(bytes, swing.startedTick);
    persistence::appendInteger(bytes, swing.durationTicks);
    persistence::appendInteger(bytes, swing.elapsedTicks);
    persistence::appendFloat(bytes, swing.previousProgress);
    persistence::appendFloat(bytes, swing.progress);
}
[[nodiscard]] SwingState readSwing(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    SwingState swing;
    swing.active = readBool(bytes, cursor);
    swing.hand = static_cast<InteractionHand>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    swing.animation = static_cast<SwingAnimation>(
        persistence::readInteger<std::uint8_t>(bytes, cursor));
    swing.sequence = persistence::readInteger<std::uint64_t>(bytes, cursor);
    swing.startedTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    swing.durationTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    swing.elapsedTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    swing.previousProgress = persistence::readFloat(bytes, cursor);
    swing.progress = persistence::readFloat(bytes, cursor);
    return swing;
}

void appendUse(std::vector<std::uint8_t>& bytes, const ItemUseState& use) {
    appendBool(bytes, use.active);
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(use.hand));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(use.animation));
    persistence::appendInteger(bytes, use.startedTick);
    persistence::appendInteger(bytes, use.durationTicks);
    persistence::appendInteger(bytes, use.remainingTicks);
    persistence::appendInteger(bytes, use.previousRemainingTicks);
}
[[nodiscard]] ItemUseState readUse(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    ItemUseState use;
    use.active = readBool(bytes, cursor);
    use.hand = static_cast<InteractionHand>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    use.animation =
        static_cast<UseAnimation>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    use.startedTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    use.durationTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    use.remainingTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    use.previousRemainingTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    return use;
}

void appendPlayerTick(std::vector<std::uint8_t>& bytes, const PlayerTickSnapshot& snap) {
    persistence::appendInteger(bytes, snap.serverTick);
    appendSwing(bytes, snap.swing);
    appendUse(bytes, snap.use);
    codec::appendVec3(bytes, snap.physicsPrevious);
    codec::appendVec3(bytes, snap.physicsCurrent);
    persistence::appendFloat(bytes, snap.previousStride);
    persistence::appendFloat(bytes, snap.stride);
    persistence::appendFloat(bytes, snap.previousSpeed);
    persistence::appendFloat(bytes, snap.speed);
    appendBool(bytes, snap.sneaking);
    appendBool(bytes, snap.flying);
    appendBool(bytes, snap.sprinting);
    appendBool(bytes, snap.inWater);
    appendBool(bytes, snap.onGround);
    persistence::appendFloat(bytes, snap.previousFieldOfViewMultiplier);
    persistence::appendFloat(bytes, snap.fieldOfViewMultiplier);
    codec::appendItemStack(bytes, snap.heldStack);
    persistence::appendFloat(bytes, snap.health);
    persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.foodLevel));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.airTicks));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.ticksSinceDamage));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(snap.gameMode));
    appendBool(bytes, snap.eating);
    persistence::appendInteger(bytes, static_cast<std::uint64_t>(snap.selectedHotbarSlot));
}

[[nodiscard]] PlayerTickSnapshot readPlayerTick(std::span<const std::uint8_t> bytes,
                                                std::size_t& cursor) {
    PlayerTickSnapshot snap;
    snap.serverTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    snap.swing = readSwing(bytes, cursor);
    snap.use = readUse(bytes, cursor);
    snap.physicsPrevious = codec::readVec3(bytes, cursor);
    snap.physicsCurrent = codec::readVec3(bytes, cursor);
    snap.previousStride = persistence::readFloat(bytes, cursor);
    snap.stride = persistence::readFloat(bytes, cursor);
    snap.previousSpeed = persistence::readFloat(bytes, cursor);
    snap.speed = persistence::readFloat(bytes, cursor);
    snap.sneaking = readBool(bytes, cursor);
    snap.flying = readBool(bytes, cursor);
    snap.sprinting = readBool(bytes, cursor);
    snap.inWater = readBool(bytes, cursor);
    snap.onGround = readBool(bytes, cursor);
    snap.previousFieldOfViewMultiplier = persistence::readFloat(bytes, cursor);
    snap.fieldOfViewMultiplier = persistence::readFloat(bytes, cursor);
    const auto held = codec::readItemStack(bytes, cursor);
    if (!held.has_value()) {
        throw std::runtime_error("Snapshot holds an unknown item");
    }
    snap.heldStack = *held;
    snap.health = persistence::readFloat(bytes, cursor);
    snap.foodLevel = persistence::readInteger<std::int32_t>(bytes, cursor);
    snap.airTicks = persistence::readInteger<std::int32_t>(bytes, cursor);
    snap.ticksSinceDamage = persistence::readInteger<std::int32_t>(bytes, cursor);
    snap.gameMode = static_cast<GameMode>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    snap.eating = readBool(bytes, cursor);
    snap.selectedHotbarSlot =
        static_cast<std::size_t>(persistence::readInteger<std::uint64_t>(bytes, cursor));
    return snap;
}

void appendWorld(std::vector<std::uint8_t>& bytes, const WorldSnapshot& snap) {
    persistence::appendInteger(bytes, snap.serverTick);
    persistence::appendFloat(bytes, snap.previousRainGradient);
    persistence::appendFloat(bytes, snap.rainGradient);
    persistence::appendFloat(bytes, snap.previousThunderGradient);
    persistence::appendFloat(bytes, snap.thunderGradient);
    appendBool(bytes, snap.raining);
    appendBool(bytes, snap.thundering);
    persistence::appendDouble(bytes, snap.dayTimeTicks);
    for (const auto& clock : snap.clocks) {
        persistence::appendInteger(bytes, clock.totalTicks);
        persistence::appendFloat(bytes, clock.partialTick);
        persistence::appendFloat(bytes, clock.rate);
        appendBool(bytes, clock.paused);
    }
    appendBool(bytes, snap.doDaylightCycle);
    appendBool(bytes, snap.doWeatherCycle);
    codec::appendVec3(bytes, snap.worldSpawnPosition);
    codec::appendVec3(bytes, snap.playerSpawnPosition);
    persistence::appendFloat(bytes, snap.playerSpawnYaw);
    appendBool(bytes, snap.hasPlayerSpawn);
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(snap.chests.size()));
    for (const auto& chest : snap.chests) {
        persistence::appendInteger(bytes, static_cast<std::int32_t>(chest.position.x));
        persistence::appendInteger(bytes, static_cast<std::int32_t>(chest.position.y));
        persistence::appendInteger(bytes, static_cast<std::int32_t>(chest.position.z));
        persistence::appendFloat(bytes, chest.previousLidAngle);
        persistence::appendFloat(bytes, chest.lidAngle);
    }
    for (const auto& stack : snap.inventorySlots) {
        codec::appendItemStack(bytes, stack);
    }
    codec::appendItemStack(bytes, snap.cursorStack);
    for (const auto& stack : snap.chestItems) {
        codec::appendItemStack(bytes, stack);
    }
    for (const auto& stack : snap.tableCraftingGrid) {
        codec::appendItemStack(bytes, stack);
    }
    codec::appendItemStack(bytes, snap.tableCraftingOutput);
    for (const auto& stack : snap.playerCraftingGrid) {
        codec::appendItemStack(bytes, stack);
    }
    codec::appendItemStack(bytes, snap.playerCraftingOutput);
    codec::appendItemStack(bytes, snap.furnaceInput);
    codec::appendItemStack(bytes, snap.furnaceFuel);
    codec::appendItemStack(bytes, snap.furnaceOutput);
    persistence::appendFloat(bytes, snap.furnaceFuelProgress);
    persistence::appendFloat(bytes, snap.furnaceCookProgress);
}

[[nodiscard]] std::optional<WorldSnapshot> readWorld(std::span<const std::uint8_t> bytes,
                                                     std::size_t& cursor) {
    WorldSnapshot snap;
    snap.serverTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    snap.previousRainGradient = persistence::readFloat(bytes, cursor);
    snap.rainGradient = persistence::readFloat(bytes, cursor);
    snap.previousThunderGradient = persistence::readFloat(bytes, cursor);
    snap.thunderGradient = persistence::readFloat(bytes, cursor);
    snap.raining = readBool(bytes, cursor);
    snap.thundering = readBool(bytes, cursor);
    snap.dayTimeTicks = persistence::readDouble(bytes, cursor);
    for (auto& clock : snap.clocks) {
        clock.totalTicks = persistence::readInteger<std::uint64_t>(bytes, cursor);
        clock.partialTick = persistence::readFloat(bytes, cursor);
        clock.rate = persistence::readFloat(bytes, cursor);
        clock.paused = readBool(bytes, cursor);
    }
    snap.doDaylightCycle = readBool(bytes, cursor);
    snap.doWeatherCycle = readBool(bytes, cursor);
    snap.worldSpawnPosition = codec::readVec3(bytes, cursor);
    snap.playerSpawnPosition = codec::readVec3(bytes, cursor);
    snap.playerSpawnYaw = persistence::readFloat(bytes, cursor);
    snap.hasPlayerSpawn = readBool(bytes, cursor);
    const auto chestCount = persistence::readInteger<std::uint32_t>(bytes, cursor);
    snap.chests.reserve(chestCount);
    for (std::uint32_t index = 0; index < chestCount; ++index) {
        WorldSnapshot::ChestRenderState chest;
        chest.position.x = persistence::readInteger<std::int32_t>(bytes, cursor);
        chest.position.y = persistence::readInteger<std::int32_t>(bytes, cursor);
        chest.position.z = persistence::readInteger<std::int32_t>(bytes, cursor);
        chest.previousLidAngle = persistence::readFloat(bytes, cursor);
        chest.lidAngle = persistence::readFloat(bytes, cursor);
        snap.chests.push_back(chest);
    }
    const auto readStack = [&](ItemStack& out) {
        const auto stack = codec::readItemStack(bytes, cursor);
        if (!stack.has_value()) {
            return false;
        }
        out = *stack;
        return true;
    };
    for (auto& stack : snap.inventorySlots) {
        if (!readStack(stack)) return std::nullopt;
    }
    if (!readStack(snap.cursorStack)) return std::nullopt;
    for (auto& stack : snap.chestItems) {
        if (!readStack(stack)) return std::nullopt;
    }
    for (auto& stack : snap.tableCraftingGrid) {
        if (!readStack(stack)) return std::nullopt;
    }
    if (!readStack(snap.tableCraftingOutput)) return std::nullopt;
    for (auto& stack : snap.playerCraftingGrid) {
        if (!readStack(stack)) return std::nullopt;
    }
    if (!readStack(snap.playerCraftingOutput)) return std::nullopt;
    if (!readStack(snap.furnaceInput)) return std::nullopt;
    if (!readStack(snap.furnaceFuel)) return std::nullopt;
    if (!readStack(snap.furnaceOutput)) return std::nullopt;
    snap.furnaceFuelProgress = persistence::readFloat(bytes, cursor);
    snap.furnaceCookProgress = persistence::readFloat(bytes, cursor);
    return snap;
}

}  // namespace

std::vector<std::uint8_t> encodeSnapshot(const PublishedSnapshot& snapshot) {
    std::vector<std::uint8_t> bytes;
    std::visit(
        [&](const auto& specific) {
            using T = std::decay_t<decltype(specific)>;
            if constexpr (std::is_same_v<T, PlayerTickSnapshot>) {
                codec::appendFrame(bytes, kPlayerTickTag,
                                   [&] { appendPlayerTick(bytes, specific); });
            } else if constexpr (std::is_same_v<T, WorldSnapshot>) {
                codec::appendFrame(bytes, kWorldTag, [&] { appendWorld(bytes, specific); });
            }
        },
        snapshot);
    return bytes;
}

std::size_t encodedSnapshotSize(std::span<const std::uint8_t> bytes) {
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

std::optional<PublishedSnapshot> decodeSnapshot(std::span<const std::uint8_t> bytes) {
    try {
        std::size_t cursor = 0;
        const auto frame = codec::readFrame(bytes, cursor);
        if (!frame.has_value()) {
            return std::nullopt;
        }
        const auto [tag, payloadEnd] = *frame;
        std::optional<PublishedSnapshot> decoded;
        if (tag == kPlayerTickTag) {
            decoded = readPlayerTick(bytes, cursor);
        } else if (tag == kWorldTag) {
            decoded = readWorld(bytes, cursor);
        } else {
            // An unknown snapshot tag (a newer build's mirror): skippable.
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
