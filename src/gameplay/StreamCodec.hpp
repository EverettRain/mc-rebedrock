#pragma once

// The shared byte primitives for the message boundary (N4). Both the GameCommand
// and the snapshot codecs frame their values the same way — [tag u8][size u32]
// then payload — and encode the same low-level shapes (u32-length strings,
// vec3, ItemStack by identifier palette), so one framing and one identifier
// rule serve every message type and the forward-compatibility is the same
// everywhere. The item identifiers come from the same registry the creative
// catalogue and the save format use; an unknown identifier on read is skipped,
// not fatal.

#include "gameplay/BlockIdRemap.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "persistence/SaveStream.hpp"
#include "world/Block.hpp"
#include "world/BlockRegistry.hpp"
#include "world/StateSchema.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::gameplay::codec {

inline constexpr std::size_t kFrameHeaderBytes = 1U + 4U;
// Chat lines and identifiers use a u32-length string rather than the save
// format's u16 length capped at 256, so a message's text is not limited by an
// identifier bound.
inline constexpr std::uint32_t kMaximumCodecString = 1024U * 1024U;

inline void appendString32(std::vector<std::uint8_t>& bytes, std::string_view text) {
    if (text.size() > kMaximumCodecString) {
        throw std::runtime_error("Codec string exceeds the limit");
    }
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(text.size()));
    bytes.insert(bytes.end(), text.begin(), text.end());
}

[[nodiscard]] inline std::string readString32(std::span<const std::uint8_t> bytes,
                                              std::size_t& cursor) {
    const auto length = persistence::readInteger<std::uint32_t>(bytes, cursor);
    if (length > kMaximumCodecString || cursor + length > bytes.size()) {
        throw std::runtime_error("Codec string is truncated or oversized");
    }
    std::string text(reinterpret_cast<const char*>(bytes.data() + cursor), length);
    cursor += length;
    return text;
}

inline void appendVec3(std::vector<std::uint8_t>& bytes, glm::vec3 value) {
    persistence::appendFloat(bytes, value.x);
    persistence::appendFloat(bytes, value.y);
    persistence::appendFloat(bytes, value.z);
}
[[nodiscard]] inline glm::vec3 readVec3(std::span<const std::uint8_t> bytes,
                                        std::size_t& cursor) {
    return {persistence::readFloat(bytes, cursor), persistence::readFloat(bytes, cursor),
            persistence::readFloat(bytes, cursor)};
}

inline void appendIvec3(std::vector<std::uint8_t>& bytes, glm::ivec3 value) {
    persistence::appendInteger(bytes, static_cast<std::int32_t>(value.x));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(value.y));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(value.z));
}
[[nodiscard]] inline glm::ivec3 readIvec3(std::span<const std::uint8_t> bytes,
                                          std::size_t& cursor) {
    return {persistence::readInteger<std::int32_t>(bytes, cursor),
            persistence::readInteger<std::int32_t>(bytes, cursor),
            persistence::readInteger<std::int32_t>(bytes, cursor)};
}

// An item stack is its identity plus its count and damage. A block stack is
// identified by its block, an item stack by its registered item. ENCH-0: the
// enchantment list rides after damage, sparse (count then (id,level) pairs) —
// this is a live per-message wire format (never persisted across builds the
// way the save format is), so there is no version gate to thread here: every
// build that can decode this message at all decodes this exact shape.
inline void appendItemStack(std::vector<std::uint8_t>& bytes, const ItemStack& stack) {
    if (isBlockStack(stack)) {
        const auto* blockItem = blockItemFor(stack.block);
        appendString32(bytes, blockItem != nullptr ? blockItem->identifier.toString()
                                                   : std::string_view{"rebedrock:air"});
    } else {
        appendString32(bytes, stack.item != nullptr ? stack.item->identifier.toString()
                                                    : std::string_view{"rebedrock:air"});
    }
    persistence::appendInteger(bytes, stack.count);
    persistence::appendInteger(bytes, stack.damage);
    persistence::appendInteger(bytes, stack.enchantmentCount);
    for (std::uint8_t index = 0; index < stack.enchantmentCount; ++index) {
        persistence::appendInteger(bytes, stack.enchantments[index].id);
        persistence::appendInteger(bytes, stack.enchantments[index].level);
    }
}

[[nodiscard]] inline std::optional<ItemStack> readItemStack(std::span<const std::uint8_t> bytes,
                                                            std::size_t& cursor) {
    const auto identifier = readString32(bytes, cursor);
    const auto count = persistence::readInteger<std::uint8_t>(bytes, cursor);
    const auto damage = persistence::readInteger<std::uint16_t>(bytes, cursor);
    const auto enchantmentCount = persistence::readInteger<std::uint8_t>(bytes, cursor);
    std::array<EnchantmentInstance, kMaxEnchantmentsPerStack> enchantments{};
    std::uint8_t storedCount = 0U;
    for (std::uint8_t index = 0; index < enchantmentCount; ++index) {
        const auto id = persistence::readInteger<EnchantmentIdStorage>(bytes, cursor);
        const auto level = persistence::readInteger<std::uint8_t>(bytes, cursor);
        // Same forward-compatible skip as every other palette lookup here: an
        // id this build does not recognise (or a corrupt/zero level) is
        // dropped, the bytes having already been consumed either way.
        if (id < kEnchantmentCount && level > 0U && storedCount < kMaxEnchantmentsPerStack) {
            enchantments[storedCount] = EnchantmentInstance{id, level};
            ++storedCount;
        }
    }
    // A real registered item wins over a same-named block — the mirror of
    // /give's fix (GameRuntime) and the general "Items registry beats the block
    // bridge" rule. Resolving the block first decoded an identifier like "wheat"
    // (the crop block and the wheat item share the id) back into the crop
    // block-item — a 2D "wheat plant" that cannot feed animals or craft bread —
    // on *every* inventory sync that crosses the client/server channel, so the
    // command-side /give fix alone never reached the client. This checks the
    // Items registry directly (not itemFromIdentifier, whose block bridge would
    // turn every plain block into an item stack and break the block-stack
    // round-trip below): only a genuinely registered item takes the item path;
    // a bare block name falls through to the block branch and stays a
    // null-item block stack, exactly as before.
    if (const core::ItemId id = itemRegistry().byName(identifier); id.valid()) {
        const Item* item = itemFromId(id);
        if (const auto* blockItem = asBlockItem(item); blockItem != nullptr) {
            return ItemStack{blockItem->block(), count, item, damage, enchantments, storedCount};
        }
        return ItemStack{world::Block::Air, count, item, damage, enchantments, storedCount};
    }
    if (const auto block = world::blockFromIdentifier(identifier); block.has_value()) {
        return ItemStack{*block, count, nullptr, damage, enchantments, storedCount};
    }
    // A name only the block bridge knows (a block wielded as its BlockItem whose
    // block id resolved above already, so this is just a safety net for any item
    // that is neither a registry entry nor a block).
    if (const auto* item = itemFromIdentifier(identifier); item != nullptr) {
        if (const auto* blockItem = asBlockItem(item); blockItem != nullptr) {
            return ItemStack{blockItem->block(), count, item, damage, enchantments, storedCount};
        }
        return ItemStack{world::Block::Air, count, item, damage, enchantments, storedCount};
    }
    return std::nullopt;
}

// A block by its dense BlockId — two bytes, not a name string — the way 26.1
// sends block ids against a shared palette. The id is the sender's own; the
// receiver maps it back to its local BlockId through `remap` (null / identity on
// loopback, a name-aligned table on a cross-process connection, see
// BlockIdRemap.hpp). A peer block this build has no place for (an external id, or
// a name the remap could not resolve) reads back as nullopt and is skipped, the
// same forward-compatible skip the identifier form gave.
inline void appendBlock(std::vector<std::uint8_t>& bytes, world::Block block) {
    persistence::appendInteger(bytes, world::blockId(block).value());
}
[[nodiscard]] inline std::optional<world::Block> readBlock(std::span<const std::uint8_t> bytes,
                                                           std::size_t& cursor,
                                                           const BlockIdRemap* remap = nullptr) {
    const auto peerId = persistence::readInteger<std::uint16_t>(bytes, cursor);
    const world::BlockId local =
        remap != nullptr ? remap->toLocal(peerId) : world::BlockId::of(peerId);
    // Only built-in blocks have an enum handle and a place in this build's world;
    // an external id (or an unresolved name) is skipped rather than forced.
    if (!local.valid() || local.index() >= world::kBuiltinBlockCount) {
        return std::nullopt;
    }
    return world::blockFromId(local);
}

// A block state as its identifier plus the properties the block declares, the
// same "identifier + named properties" shape save format 18 uses. Only declared
// properties ride the wire, so an undeclared one can never corrupt a state.
// Block state on the wire: the block, then every property its schema declares as
// an (index, value) pair. Schema-driven on purpose — a hand-listed property set
// is what silently dropped SlabType here, so a placed top or double slab crossed
// the loopback channel as a default bottom slab and meshed as one while the
// authoritative world still knew the truth. Iterating the enum means the stair,
// fence and door axes to come travel without this codec being touched. The
// property index is written (not a fixed bitmask) so the format scales past the
// eight properties a single flags byte could name.
inline void appendBlockState(std::vector<std::uint8_t>& bytes, const world::BlockState& state) {
    appendBlock(bytes, state.block());
    std::uint8_t count = 0U;
    for (std::size_t index = 0; index < world::kStatePropertyCount; ++index) {
        if (state.has(static_cast<world::StateProperty>(index))) {
            ++count;
        }
    }
    persistence::appendInteger(bytes, count);
    for (std::size_t index = 0; index < world::kStatePropertyCount; ++index) {
        const auto property = static_cast<world::StateProperty>(index);
        if (state.has(property)) {
            persistence::appendInteger(bytes, static_cast<std::uint8_t>(index));
            persistence::appendInteger(bytes, state.value(property));
        }
    }
}
[[nodiscard]] inline std::optional<world::BlockState>
readBlockState(std::span<const std::uint8_t> bytes, std::size_t& cursor,
               const BlockIdRemap* remap = nullptr) {
    const auto block = readBlock(bytes, cursor, remap);
    if (!block.has_value()) {
        return std::nullopt;
    }
    auto state = world::BlockState{*block};
    const auto count = persistence::readInteger<std::uint8_t>(bytes, cursor);
    for (std::uint8_t written = 0U; written < count; ++written) {
        const auto index = persistence::readInteger<std::uint8_t>(bytes, cursor);
        const auto value = persistence::readInteger<std::uint8_t>(bytes, cursor);
        // A property this build does not know (a newer peer) is skipped, not
        // refused — the value was already consumed, so the stream stays aligned.
        if (index < world::kStatePropertyCount) {
            state = state.with(static_cast<world::StateProperty>(index), value);
        }
    }
    return state;
}

// A creature species by its registered identifier (EntityType instances are
// static singletons, so the pointer round-trips by id). Null encodes as empty.
inline void appendEntityType(std::vector<std::uint8_t>& bytes,
                             const entities::EntityType* type) {
    appendString32(bytes, type != nullptr ? type->id().toString() : std::string_view{""});
}
[[nodiscard]] inline const entities::EntityType* readEntityType(std::span<const std::uint8_t> bytes,
                                                                std::size_t& cursor) {
    const auto identifier = readString32(bytes, cursor);
    return identifier.empty() ? nullptr : entities::entityTypeRegistry().byId(identifier);
}

// Writes [tag][size placeholder], runs `appendPayload`, then fills the size.
inline void appendFrame(std::vector<std::uint8_t>& bytes, std::uint8_t tag,
                        const auto& appendPayload) {
    bytes.push_back(tag);
    const std::size_t sizeAt = bytes.size();
    persistence::appendInteger(bytes, std::uint32_t{0});
    const std::size_t payloadStart = bytes.size();
    appendPayload();
    const auto size = static_cast<std::uint32_t>(bytes.size() - payloadStart);
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[sizeAt + index] = static_cast<std::uint8_t>(size >> (index * 8U));
    }
}

// Reads a frame's tag and the payload end; nullopt when the frame is truncated.
[[nodiscard]] inline std::optional<std::pair<std::uint8_t, std::size_t>>
readFrame(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    if (cursor + kFrameHeaderBytes > bytes.size()) {
        return std::nullopt;
    }
    const auto tag = persistence::readInteger<std::uint8_t>(bytes, cursor);
    const auto size = persistence::readInteger<std::uint32_t>(bytes, cursor);
    const std::size_t payloadEnd = cursor + size;
    if (payloadEnd > bytes.size()) {
        return std::nullopt;
    }
    return std::pair{tag, payloadEnd};
}

// The total length one frame occupies (header + payload), read from the header
// alone. nullopt while fewer than the header's bytes are present. Unlike
// readFrame this does not require the payload to be there yet, so a byte-stream
// reassembler (the TCP channel) can decide from a partial buffer how many more
// bytes a whole frame still needs — the half-packet/coalesced-packet case a raw
// stream has and the loopback queue never does.
[[nodiscard]] inline std::optional<std::size_t> peekFrameLength(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kFrameHeaderBytes) {
        return std::nullopt;
    }
    std::size_t cursor = 1U;  // Skip the tag; the size follows it.
    const auto size = persistence::readInteger<std::uint32_t>(bytes, cursor);
    return kFrameHeaderBytes + static_cast<std::size_t>(size);
}

}  // namespace mc::gameplay::codec
