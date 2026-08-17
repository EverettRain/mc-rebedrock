#include "gameplay/GameCommandCodec.hpp"

#include "gameplay/StreamCodec.hpp"

#include <cstdint>
#include <stdexcept>

namespace mc::gameplay {
namespace {

void appendSlotRef(std::vector<std::uint8_t>& bytes, const SlotRef& ref) {
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(ref.kind));
    persistence::appendInteger(bytes, ref.index);
}

[[nodiscard]] SlotRef readSlotRef(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    SlotRef ref;
    ref.kind = static_cast<SlotKind>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    ref.index = persistence::readInteger<std::uint16_t>(bytes, cursor);
    return ref;
}

}  // namespace

std::vector<std::uint8_t> encodeGameCommand(const GameCommand& command) {
    std::vector<std::uint8_t> bytes;
    const std::uint8_t tag = static_cast<std::uint8_t>(command.index());
    codec::appendFrame(bytes, tag, [&] {
        std::visit(
            [&](const auto& specific) {
                using T = std::decay_t<decltype(specific)>;
                if constexpr (std::is_same_v<T, PlayerAction>) {
                    persistence::appendInteger(bytes, static_cast<std::uint8_t>(specific.kind));
                    persistence::appendInteger(
                        bytes, static_cast<std::uint8_t>(specific.block.has_value() ? 1 : 0));
                    if (specific.block.has_value()) {
                        codec::appendIvec3(bytes, *specific.block);
                    }
                    persistence::appendInteger(bytes,
                                               static_cast<std::uint8_t>(specific.entity ? 1 : 0));
                    persistence::appendInteger(bytes, specific.entityId);
                } else if constexpr (std::is_same_v<T, UseItemOn>) {
                    codec::appendIvec3(bytes, specific.block);
                    codec::appendIvec3(bytes, specific.adjacent);
                    persistence::appendInteger(bytes,
                                               static_cast<std::uint8_t>(specific.face));
                    codec::appendVec3(bytes, specific.hitPosition);
                    codec::appendVec3(bytes, specific.lookDirection);
                } else if constexpr (std::is_same_v<T, UseItem>) {
                    persistence::appendInteger(bytes,
                                               static_cast<std::uint8_t>(specific.hand));
                } else if constexpr (std::is_same_v<T, UseItemStop>) {
                } else if constexpr (std::is_same_v<T, SwapSlot>) {
                    persistence::appendInteger(bytes,
                                               static_cast<std::uint64_t>(specific.index));
                } else if constexpr (std::is_same_v<T, ClickSlot>) {
                    persistence::appendInteger(bytes,
                                               static_cast<std::uint8_t>(specific.kind));
                    persistence::appendInteger(bytes, specific.slotIndex);
                    persistence::appendInteger(bytes, static_cast<std::int32_t>(specific.button));
                    persistence::appendInteger(
                        bytes, static_cast<std::uint8_t>(specific.shiftHeld ? 1 : 0));
                } else if constexpr (std::is_same_v<T, ChatCommand>) {
                    codec::appendString32(bytes, specific.line);
                } else if constexpr (std::is_same_v<T, ClickCreativeItem>) {
                    codec::appendItemStack(bytes, specific.catalogStack);
                    persistence::appendInteger(bytes,
                                               static_cast<std::uint8_t>(specific.button));
                    persistence::appendInteger(
                        bytes, static_cast<std::uint8_t>(specific.shiftHeld ? 1 : 0));
                } else if constexpr (std::is_same_v<T, ClearCursor>) {
                } else if constexpr (std::is_same_v<T, DropCursor>) {
                    codec::appendVec3(bytes, specific.lookDirection);
                } else if constexpr (std::is_same_v<T, DropSelected>) {
                    persistence::appendInteger(
                        bytes, static_cast<std::uint8_t>(specific.wholeStack ? 1 : 0));
                    codec::appendVec3(bytes, specific.lookDirection);
                } else if constexpr (std::is_same_v<T, DragDistribute>) {
                    persistence::appendInteger(bytes,
                                               static_cast<std::uint8_t>(specific.button));
                    persistence::appendInteger(
                        bytes, static_cast<std::uint32_t>(specific.targets.size()));
                    for (const auto& ref : specific.targets) {
                        appendSlotRef(bytes, ref);
                    }
                } else if constexpr (std::is_same_v<T, PickupAll>) {
                    persistence::appendInteger(
                        bytes, static_cast<std::uint32_t>(specific.targets.size()));
                    for (const auto& ref : specific.targets) {
                        appendSlotRef(bytes, ref);
                    }
                }
            },
            command);
    });
    return bytes;
}

std::size_t encodedGameCommandSize(std::span<const std::uint8_t> bytes) {
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

std::optional<GameCommand> decodeGameCommand(std::span<const std::uint8_t> bytes) {
    try {
        std::size_t cursor = 0;
        const auto frame = codec::readFrame(bytes, cursor);
        if (!frame.has_value()) {
            return std::nullopt;
        }
        const auto [tag, payloadEnd] = *frame;
        GameCommand decoded;
        switch (tag) {
        case 0: {  // PlayerAction
            const auto kind = static_cast<PlayerAction::Kind>(
                persistence::readInteger<std::uint8_t>(bytes, cursor));
            std::optional<glm::ivec3> block;
            if (persistence::readInteger<std::uint8_t>(bytes, cursor) != 0) {
                block = codec::readIvec3(bytes, cursor);
            }
            const bool entity = persistence::readInteger<std::uint8_t>(bytes, cursor) != 0;
            const auto entityId = persistence::readInteger<std::uint64_t>(bytes, cursor);
            decoded = PlayerAction{kind, block, entity, entityId};
            break;
        }
        case 1: {  // UseItemOn
            UseItemOn use;
            use.block = codec::readIvec3(bytes, cursor);
            use.adjacent = codec::readIvec3(bytes, cursor);
            use.face = static_cast<world::BlockOrientation>(
                persistence::readInteger<std::uint8_t>(bytes, cursor));
            use.hitPosition = codec::readVec3(bytes, cursor);
            use.lookDirection = codec::readVec3(bytes, cursor);
            decoded = use;
            break;
        }
        case 2: {  // UseItem
            decoded = UseItem{static_cast<InteractionHand>(
                persistence::readInteger<std::uint8_t>(bytes, cursor))};
            break;
        }
        case 3:  // UseItemStop
            decoded = UseItemStop{};
            break;
        case 4: {  // SwapSlot
            decoded = SwapSlot{
                static_cast<std::size_t>(persistence::readInteger<std::uint64_t>(bytes, cursor))};
            break;
        }
        case 5: {  // ClickSlot
            ClickSlot click;
            click.kind =
                static_cast<SlotKind>(persistence::readInteger<std::uint8_t>(bytes, cursor));
            click.slotIndex = persistence::readInteger<std::uint16_t>(bytes, cursor);
            click.button = persistence::readInteger<std::int32_t>(bytes, cursor);
            click.shiftHeld = persistence::readInteger<std::uint8_t>(bytes, cursor) != 0;
            decoded = click;
            break;
        }
        case 6:  // ChatCommand
            decoded = ChatCommand{codec::readString32(bytes, cursor)};
            break;
        case 7: {  // ClickCreativeItem
            ClickCreativeItem click;
            const auto stack = codec::readItemStack(bytes, cursor);
            if (!stack.has_value()) {
                return std::nullopt;  // an item this build does not know: skip
            }
            click.catalogStack = *stack;
            click.button = static_cast<InventoryMouseButton>(
                persistence::readInteger<std::uint8_t>(bytes, cursor));
            click.shiftHeld = persistence::readInteger<std::uint8_t>(bytes, cursor) != 0;
            decoded = click;
            break;
        }
        case 8:  // ClearCursor
            decoded = ClearCursor{};
            break;
        case 9:  // DropCursor
            decoded = DropCursor{codec::readVec3(bytes, cursor)};
            break;
        case 10: {  // DropSelected
            DropSelected drop;
            drop.wholeStack = persistence::readInteger<std::uint8_t>(bytes, cursor) != 0;
            drop.lookDirection = codec::readVec3(bytes, cursor);
            decoded = drop;
            break;
        }
        case 11: {  // DragDistribute
            DragDistribute drag;
            drag.button = static_cast<InventoryMouseButton>(
                persistence::readInteger<std::uint8_t>(bytes, cursor));
            const auto count = persistence::readInteger<std::uint32_t>(bytes, cursor);
            drag.targets.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                drag.targets.push_back(readSlotRef(bytes, cursor));
            }
            decoded = std::move(drag);
            break;
        }
        case 12: {  // PickupAll
            PickupAll pickup;
            const auto count = persistence::readInteger<std::uint32_t>(bytes, cursor);
            pickup.targets.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                pickup.targets.push_back(readSlotRef(bytes, cursor));
            }
            decoded = std::move(pickup);
            break;
        }
        default:
            // An unknown tag: a newer build's command. The frame is well-formed
            // (the transport skips it by encodedGameCommandSize); we just cannot
            // decode it.
            return std::nullopt;
        }
        if (cursor > payloadEnd) {
            return std::nullopt;  // the payload overran its declared size
        }
        return decoded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace mc::gameplay
