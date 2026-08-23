#pragma once

// The authoritative interaction controller: consumes the render thread's queued
// GameCommands and applies the dig/use/attack decisions inside the server tick,
// so the same operation consumes the same ticks at any frame rate. This is the
// orchestration half of the old renderer-side updateBlockInteraction — the
// decision functions (decideBlockInteraction, itemUseOn, MiningSystem) were
// already gameplay-owned; only the frame-driven arrangement moved here.
//
// The render thread keeps the raycast (camera -> target, a pure read) and
// enqueues commands carrying that target; gameplay never re-raycasts.

#include "gameplay/GameCommand.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mc::gameplay {
class GameSession;
struct SimulationHost;
} // namespace mc::gameplay

namespace mc::world {
class World;
} // namespace mc::world

namespace mc::gameplay {

// AR-B3: BasePressurePlateBlock's per-tick check (BasePressurePlateBlock#tick /
// #entityInside collapsed into one call): tests whether `playerFeet`/
// `creatureFeet` stand over a pressure plate and flips its Powered bit
// accordingly. `pressedPlates` is caller-owned per-session state (the
// previous tick's covered-plate set, diffed against this tick's) — GameSession
// holds it and passes it by reference so the function itself stays free of
// hidden static state. Free rather than a PlayerInteraction member since it
// is not gated by a queued command; GameSession calls it once per tick
// alongside the other per-tick world checks (farmland trample is the nearest
// precedent — see GameSession::tick).
void tickPressurePlates(GameSession& session, world::World& world, glm::vec3 playerFeet,
                        std::span<const glm::vec3> creatureFeet,
                        std::vector<glm::ivec3>& pressedPlates);

class PlayerInteraction final {
  public:
    // One tick of the authoritative interaction. `commands` is the batch drained
    // from the input queue since the last tick.
    void tick(GameSession& session, world::World& world, SimulationHost& host,
              std::vector<GameCommand> commands);

    [[nodiscard]] bool destroying() const { return destroying_; }
    [[nodiscard]] const std::optional<glm::ivec3>& destroyTarget() const {
        return destroyTarget_;
    }
    // What the renderer draws a crack over: the cell being dug and when it
    // started, so the overlay can interpolate the dig progress per frame.
    struct DigSnapshot final {
        bool active = false;
        glm::ivec3 target{};
        std::uint64_t startedTick = 0U;
    };
    [[nodiscard]] DigSnapshot digSnapshot() const {
        return {destroying_ && destroyTarget_.has_value(),
                destroyTarget_.value_or(glm::ivec3{}), miningStartedTick_};
    }

  private:
    void handleDestroyCommand(GameSession& session, world::World& world, SimulationHost& host,
                              const PlayerAction& action);
    // The continuous dig the destroy command started, run once per tick.
    void continueDig(GameSession& session, world::World& world);
    void applyBreak(GameSession& session, world::World& world, const glm::ivec3& block);
    // The use decision and the held-item action switch, run for one target.
    void performUse(GameSession& session, world::World& world, const UseItemOn& use);
    // AR-A2: right-clicking a creature with the use button — shears (shear a
    // wooled sheep) and a species' tempt item (feed toward love). Split out of
    // performUse because it never touches a block cell at all; AR-A3/AR-A4 (cow/
    // chicken) extend this same switch rather than performUse's block ladder.
    void performUseOnEntity(GameSession& session, world::World& world, const UseItemOn& use);

    bool destroying_ = false;
    std::optional<glm::ivec3> destroyTarget_;
    std::uint64_t miningStartedTick_ = 0U;
    std::int64_t lastMiningSoundTick_ = -1;
    std::uint64_t nextCreativeBreakTick_ = 0U;

    bool using_ = false;
    std::optional<UseItemOn> latestUse_;
    std::uint64_t nextUseTick_ = 0U;
};

} // namespace mc::gameplay
