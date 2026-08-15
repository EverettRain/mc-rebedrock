#pragma once

// The interactive player's intents, one value per enqueued input. The render
// thread produces them (from GLFW callbacks, container clicks, the chat line)
// and GameSession drains them at the start of its tick, so the authoritative
// interaction runs at 20 TPS instead of at the frame rate. Named and shaped
// after 26.1's ServerboundPlayerActionPacket / ServerboundUseItemOnPacket
// family, but these are PODs in a queue — not packets. The real protocol grows
// from them at N4/C, and until then the queue is the only boundary.

#include "gameplay/PlayerActionState.hpp"
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
};

// Right-click on a block: use the held item on it. The ray's exact hit is
// carried so the server decides without re-raycasting. `adjacent` is the cell
// a placement would land in (BlockPlaceContext#getClickedPos).
struct UseItemOn final {
    glm::ivec3 block{};
    glm::ivec3 adjacent{};
    world::BlockOrientation face = world::BlockOrientation::North;
    glm::vec3 hitPosition{0.0F};
    // The player's view direction, for the placement's horizontal FACING.
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
};

// Right-click in air (an empty swing, or the held item's on-air use).
struct UseItem final {
    InteractionHand hand = InteractionHand::Main;
};

// Right-click released. Ends a held use (a meal, a repeated placement), the
// analogue of 26.1's ServerboundPlayerCommandPacket ReleaseUseItem.
struct UseItemStop final {};


// A hotbar selection change. `index` is the slot the player switched to.
struct SwapSlot final {
    std::size_t index = 0U;
};

// A container/inventory slot click, the same payload ScreenHandler routes.
struct ClickSlot final {
    std::size_t slotIndex = 0U;
    int button = 0;
    bool shiftHeld = false;
};

// A submitted chat line (a slash command or plain chat).
struct ChatCommand final {
    std::string line;
};

// One queued input, discriminated. All payloads are trivially copyable except
// ChatCommand's string.
using GameCommand =
    std::variant<PlayerAction, UseItemOn, UseItem, UseItemStop, SwapSlot, ClickSlot, ChatCommand>;

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
