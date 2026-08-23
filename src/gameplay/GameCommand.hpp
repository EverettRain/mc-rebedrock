#pragma once

// The interactive player's intents, one value per enqueued input. The render
// thread produces them (from GLFW callbacks, container clicks, the chat line)
// and GameSession drains them at the start of its tick, so the authoritative
// interaction runs at 20 TPS instead of at the frame rate. Named and shaped
// after 26.1's ServerboundPlayerActionPacket / ServerboundUseItemOnPacket
// family, but these are PODs in a queue — not packets. The real protocol grows
// from them at N4/C, and until then the queue is the only boundary.

#include "gameplay/PlayerActionState.hpp"
#include "gameplay/ScreenHandler.hpp"
#include "world/Block.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mc::gameplay {

// Attack/destroy lifecycle. StartDestroy fires on the press (with the target the
// ray reached — a block cell, a creature, or nothing for an empty swing);
// AbortDestroy on release; StopDestroy when the block broke or the target
// changed. Between Start and Abort the server continues the dig each tick.
struct PlayerAction final {
    enum class Kind : std::uint8_t {
        StartDestroy,
        AbortDestroy,
        StopDestroy,
    };
    Kind kind = Kind::StartDestroy;
    // The aimed-at cell, when the ray hit a block within reach.
    std::optional<glm::ivec3> block;
    // The creature hit, when the ray reached it before any block.
    bool entity = false;
    std::uint64_t entityId = 0U;
    [[nodiscard]] friend bool operator==(const PlayerAction&, const PlayerAction&) = default;
};

// Right-click on a block: use the held item on it. The ray's exact hit is
// carried so the server decides without re-raycasting. `adjacent` is the cell
// a placement would land in (BlockPlaceContext#getClickedPos).
//
// AR-A2: also carries a right-click-on-creature hit (Minecraft#doAttack's
// entity branch, but for the *use* button rather than attack — vanilla's
// ServerboundInteractPacket InteractionHand/EntityInteraction). `entity` mirrors
// PlayerAction's own (entity, entityId) pair: the render thread already prefers
// a creature hit over a block hit for the attack button (see updateInteractionTarget
// resetting targetedBlock once creatureHit is set), so the use button follows the
// same precedence. When `entity` is true the block fields are unused/zeroed.
struct UseItemOn final {
    glm::ivec3 block{};
    glm::ivec3 adjacent{};
    world::BlockOrientation face = world::BlockOrientation::North;
    glm::vec3 hitPosition{0.0F};
    // The player's view direction, for the placement's horizontal FACING.
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
    bool entity = false;
    std::uint64_t entityId = 0U;
    [[nodiscard]] friend bool operator==(const UseItemOn&, const UseItemOn&) = default;
};

// Right-click in air (an empty swing, or the held item's on-air use).
struct UseItem final {
    InteractionHand hand = InteractionHand::Main;
    [[nodiscard]] friend bool operator==(const UseItem&, const UseItem&) = default;
};

// Right-click released. Ends a held use (a meal, a repeated placement), the
// analogue of 26.1's ServerboundPlayerCommandPacket ReleaseUseItem.
struct UseItemStop final {
    [[nodiscard]] friend bool operator==(const UseItemStop&, const UseItemStop&) = default;
};


// A hotbar selection change. `index` is the slot the player switched to.
struct SwapSlot final {
    std::size_t index = 0U;
    [[nodiscard]] friend bool operator==(const SwapSlot&, const SwapSlot&) = default;
};

// A container/inventory slot click. `kind` is the slot's role (what
// ScreenHandler routes by); the interaction resolves the storage from the
// session's open container and the slot index, then calls ScreenHandler::click.
struct ClickSlot final {
    SlotKind kind = SlotKind::PlayerInventory;
    std::uint16_t slotIndex = 0U;
    int button = 0;
    bool shiftHeld = false;
    [[nodiscard]] friend bool operator==(const ClickSlot&, const ClickSlot&) = default;
};

// A submitted chat line (a slash command or plain chat).
struct ChatCommand final {
    std::string line;
    [[nodiscard]] friend bool operator==(const ChatCommand&, const ChatCommand&) = default;
};

// The identity of a slot a drag sweeps: its role plus its index. Values, not
// pointers, so the renderer's drag state holds no reference into gameplay —
// the drag set travels to the server as (kind, index) and the interaction
// resolves each to its storage against the open container.
struct SlotRef final {
    SlotKind kind = SlotKind::PlayerInventory;
    std::uint16_t index = 0U;
    [[nodiscard]] friend bool operator==(const SlotRef&, const SlotRef&) = default;
};

// A creative-catalogue cell click. The renderer resolves the catalogue stack (a
// presentation read of the catalogue) and carries the value; gameplay decides
// how the cursor changes.
struct ClickCreativeItem final {
    ItemStack catalogStack{};
    InventoryMouseButton button = InventoryMouseButton::Left;
    bool shiftHeld = false;
    [[nodiscard]] friend bool operator==(const ClickCreativeItem&, const ClickCreativeItem&) =
        default;
};

// The creative delete slot or an empty catalogue cell: clear the cursor.
struct ClearCursor final {
    [[nodiscard]] friend bool operator==(const ClearCursor&, const ClearCursor&) = default;
};

// Drop the whole cursor stack as an item entity in front of the player.
struct DropCursor final {
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
    [[nodiscard]] friend bool operator==(const DropCursor&, const DropCursor&) = default;
};

// The Q drop: throw the selected hotbar stack (or one item of it) in front of
// the player.
struct DropSelected final {
    bool wholeStack = true;
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
    [[nodiscard]] friend bool operator==(const DropSelected&, const DropSelected&) = default;
};

// SlotActionType.QUICK_CRAFT end: distribute the cursor stack across the swept
// slots (left shares evenly, right places one per slot), matching vanilla's
// two-phase press/drag/release. The renderer accumulates the swept slots as
// values between the press and the release, then ships them here.
struct DragDistribute final {
    InventoryMouseButton button = InventoryMouseButton::Left;
    std::vector<SlotRef> targets;
    [[nodiscard]] friend bool operator==(const DragDistribute&, const DragDistribute&) = default;
};

// SlotActionType.PICKUP_ALL: gather every stack in `targets` that matches the
// cursor's item into it, stopping at the stack limit.
struct PickupAll final {
    std::vector<SlotRef> targets;
    [[nodiscard]] friend bool operator==(const PickupAll&, const PickupAll&) = default;
};

// The player's continuous movement intent for one tick — the per-frame keyboard
// and look sample the renderer wrote straight into GameSession::input() before
// the client/server split. Deliberately NOT a GameCommand: those are discrete
// actions the tick drains late (after physics), whereas movement must be staged
// before the tick reads it, exactly as commitInput() published it. Cross-process
// the client has no session to write, so it ships this each tick and the server
// stages it on the authoritative player. It carries only raw intent — the server
// derives the gated fields (flightAllowed from the game mode, sprintAllowed from
// the food level) itself rather than trusting the client's copy of them.
struct MovementInput final {
    float forward = 0.0F;
    float strafe = 0.0F;
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
    bool jumpHeld = false;
    bool descendHeld = false;
    bool sneakHeld = false;
    bool sprintHeld = false;
    // The jump edge (a fresh press this interval), which toggles creative flight;
    // ORed across the inputs that arrive between two ticks so a press is never
    // dropped. Distinct from jumpHeld, which is level-sampled for ordinary jumps.
    bool jumpPressed = false;
    // The sprint double-tap edge the client detects.
    bool forwardPressed = false;
    // The client's auto-jump option.
    bool autoJump = false;
    [[nodiscard]] friend bool operator==(const MovementInput&, const MovementInput&) = default;
};

// One queued input, discriminated. All payloads are trivially copyable except
// the chat string and the two drag vectors.
using GameCommand = std::variant<PlayerAction, UseItemOn, UseItem, UseItemStop, SwapSlot, ClickSlot,
                                 ChatCommand, ClickCreativeItem, ClearCursor, DropCursor,
                                 DropSelected, DragDistribute, PickupAll>;

// The input queue between the render thread and the simulation tick.
class GameCommandQueue final {
  public:
    // Produced by the render thread between ticks.
    void enqueue(GameCommand command) {
        const std::lock_guard<std::mutex> guard{mutex_};
        pending_.push_back(std::move(command));
    }
    // Consumed by GameSession at the start of its tick. Drains the whole batch
    // so a burst of inputs between ticks all land in order.
    [[nodiscard]] std::vector<GameCommand> drain() {
        const std::lock_guard<std::mutex> guard{mutex_};
        std::vector<GameCommand> batch;
        batch.swap(pending_);
        return batch;
    }
    [[nodiscard]] bool empty() const {
        const std::lock_guard<std::mutex> guard{mutex_};
        return pending_.empty();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<GameCommand> pending_;
};

} // namespace mc::gameplay
