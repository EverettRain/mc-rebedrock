#pragma once

#include "world/Block.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_set>
#include <vector>

#include <glm/vec3.hpp>

namespace mc::world {
class World;
}

namespace mc::gameplay {

inline constexpr std::uint8_t kMaximumHorizontalWaterLevel = 7U;
inline constexpr std::uint8_t kFallingWaterLevel = 8U;

struct SimulationPosition final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const SimulationPosition&) const = default;
};

struct SimulationPositionHash final {
    [[nodiscard]] std::size_t operator()(const SimulationPosition& position) const noexcept;
};

struct BlockChange final {
    SimulationPosition position;
    world::Block block = world::Block::Air;
    std::uint8_t fluidLevel = 0U;
    // The block that was removed and should drop as an item, if any. Set when
    // an attached block loses its support or a fluid washes it away.
    world::Block dropped = world::Block::Air;
    // The orientation a state-carrying block should land on: a crop's new age,
    // farmland's new moisture. Nullopt for an ordinary block swap, which takes
    // the block's default orientation.
    std::optional<world::BlockOrientation> orientation;
    // The orientation of the dropped block, so a crop that pops keeps the age
    // it was broken at when its loot table rolls.
    world::BlockOrientation droppedOrientation = world::BlockOrientation::North;
};

// What a leaf's support search concluded. A search that walked into a chunk the
// streamer has not delivered yet cannot answer either way — the trunk may be
// standing in it — so that case is kept apart from a definite "no log in range".
enum class LeafSupport : std::uint8_t {
    Supported,
    Unsupported,
    Undecided,
};

struct FallingBlockEntity final {
    glm::vec3 position{0.0F};
    glm::vec3 previousPosition{0.0F};
    float verticalVelocity = 0.0F;
    world::Block block = world::Block::Sand;
    bool removed = false;
};

[[nodiscard]] bool isCollectableWaterSource(
    const world::World& world,
    SimulationPosition position);

class WorldSimulation final {
  public:
    static constexpr std::size_t kMaximumWaterUpdatesPerPhase = 16U;
    // How many saplings may grow into trees in one game tick. Growing a tree
    // writes a couple of hundred blocks through the change pipeline on the
    // render thread, so a batch of saplings that mature in the same tick is
    // drained at this rate instead of stalling a single frame.
    static constexpr std::size_t kMaximumTreeGrowthsPerTick = 2U;
    // How many spreadable-block conversions one random-tick pass may apply in a
    // single game tick. Every conversion is a full block edit through the
    // streaming worker plus a section mesh rebuild, so an unbounded spread at a
    // high randomTickSpeed can outrun that pipeline — stalling the frame,
    // burying player edits behind the flood, and piling GPU mesh allocations on
    // until the device runs out of memory. The cap keeps the pipeline in
    // equilibrium; leftover conversions wait for later ticks.
    static constexpr std::size_t kMaximumRandomTickConversionsPerTick = 32U;
    // How many queued leaf-decay entries one game tick may process. Each check
    // walks a flood fill through the canopy, and at a high randomTickSpeed the
    // short decay delays make those checks frequent, so the cap bounds the
    // render thread's worst-case leaf cost. Unchecked leaves stay queued.
    static constexpr std::size_t kMaximumLeafDecayChecksPerTick = 128U;
    // LeavesBlock decays on a random tick, and vanilla rolls three random blocks
    // per 16x16x16 section per game tick, so an individual leaf gets its chance
    // 3/4096 of the time. That is a mean wait of 1365 ticks — 68 seconds — per
    // leaf: a canopy starts thinning within a second, but the last leaf of a
    // fifty-odd block crown takes about five minutes, and a chopped forest takes
    // longer still. Slow, but that is what 1.16.1 does; this constant is the one
    // knob if the pacing needs to change.
    static constexpr double kLeafDecayChancePerTick = 3.0 / 4096.0;
    // How many crop-growth / farmland-moisture state writes one random-tick pass
    // may apply per game tick. Each is a full block edit through the streaming
    // worker plus a section mesh rebuild (the crop's stage texture changes), so
    // a whole mature farm can outrun that pipeline at a high randomTickSpeed;
    // the cap keeps the pipeline in equilibrium like the grass-conversion one.
    static constexpr std::size_t kMaximumCropStateWritesPerTick = 64U;

    void notifyPlaced(SimulationPosition position, world::Block block);
    void notifyNeighborChanged(const world::World& world, SimulationPosition position);
    [[nodiscard]] std::vector<BlockChange> tick(
        world::World& world,
        bool processFluidUpdates = true);
    [[nodiscard]] const std::vector<FallingBlockEntity>& fallingBlocks() const {
        return fallingBlocks_;
    }
    [[nodiscard]] std::size_t pendingWaterUpdateCount() const {
        return activeWater_.size();
    }
    [[nodiscard]] std::size_t lastWaterUpdatesProcessed() const {
        return lastWaterUpdatesProcessed_;
    }
    [[nodiscard]] std::size_t pendingLeafDecayCount() const {
        return queuedLeafDecays_.size();
    }
    // The `/gamerule randomTickSpeed` value: how many random blocks per
    // 16x16x16 section are drawn every game tick. Vanilla's default is 3; 0
    // disables random ticks (and with them grass spread, sapling growth and
    // leaf decay).
    void setRandomTickSpeed(int speed) { randomTickSpeed_ = speed; }
    [[nodiscard]] int randomTickSpeed() const { return randomTickSpeed_; }
    [[nodiscard]] std::size_t lastTreeGrowthsProcessed() const {
        return lastTreeGrowthsProcessed_;
    }
    [[nodiscard]] std::size_t pendingTreeGrowthCount() const {
        return queuedTreeGrowths_.size();
    }
    [[nodiscard]] std::size_t lastRandomTickConversions() const {
        return randomTickConversionsThisTick_;
    }

  private:
    void queueSand(SimulationPosition position);
    void queueWater(SimulationPosition position, std::uint8_t level);
    void queueSupportCheck(SimulationPosition position);
    void queueNeighborSupportChecks(SimulationPosition position);
    void breakUnsupportedBlocks(world::World& world, std::vector<BlockChange>& changes);
    // Java recomputes LeavesBlock.DISTANCE through a chain of neighbour updates
    // once a log disappears. Flooding out from the change through leaves, as far
    // as the property counts, reaches exactly the same blocks in one pass.
    void queueLeafDecayChecks(const world::World& world, SimulationPosition origin);
    void queueLeafDecay(SimulationPosition position);
    [[nodiscard]] LeafSupport leafSupport(
        const world::World& world,
        SimulationPosition origin) const;
    void decayLeaves(world::World& world, std::vector<BlockChange>& changes);
    // Java's random-tick pass: `randomTickSpeed` draws per non-empty section,
    // dispatching each drawn block to the blocks that have a random tick
    // (grass spread and sapling growth today, crops later).
    void randomTicks(world::World& world, std::vector<BlockChange>& changes);
    void randomTickBlock(world::World& world, SimulationPosition position,
                         world::Block block, std::vector<BlockChange>& changes);
    void randomTickGrass(world::World& world, SimulationPosition position,
                         std::vector<BlockChange>& changes);
    // Applies one grass/spreadable conversion, unless this tick's conversion
    // budget is spent (see kMaximumRandomTickConversionsPerTick).
    void reserveConversionAndApply(world::World& world, SimulationPosition position,
                                   world::Block block, std::vector<BlockChange>& changes);
    void randomTickSapling(world::World& world, SimulationPosition position,
                           std::vector<BlockChange>& changes);
    // CropsBlock#randomTick: grows one age when the block above is lit enough
    // and the moisture-weighted growth roll passes. The new age is written to
    // the crop's orientation state and emitted as an orientation BlockChange.
    void randomTickCrop(world::World& world, SimulationPosition position, world::Block block,
                        std::vector<BlockChange>& changes);
    // FarmlandBlock#randomTick: near water the moisture jumps to 7, otherwise it
    // dries one level, reverting to dirt at 0 once no crop stands on it.
    void randomTickFarmland(world::World& world, SimulationPosition position,
                            std::vector<BlockChange>& changes);
    // CropsBlock#getAvailableMoisture: how much the farmland under and around a
    // crop speeds its growth (1.0 alone, up to ~10 when nine moist blocks ring
    // the plant). The crop's growth chance divides by this.
    [[nodiscard]] static float availableMoisture(
        const world::World& world, SimulationPosition position);
    // FarmlandBlock#isNearWater: any water within four blocks horizontally at
    // the farmland's level keeps it hydrated.
    [[nodiscard]] static bool farmlandNearWater(
        const world::World& world, SimulationPosition position);
    [[nodiscard]] static bool cropOnTop(
        const world::World& world, SimulationPosition position);
    // Applies one crop/farmland state write unless this tick's budget is spent
    // (see kMaximumCropStateWritesPerTick).
    [[nodiscard]] bool reserveCropStateWrite() {
        if (cropStateWritesThisTick_ >= kMaximumCropStateWritesPerTick) return false;
        ++cropStateWritesThisTick_;
        return true;
    }
    void queueTreeGrowth(SimulationPosition position);
    void growTrees(world::World& world, std::vector<BlockChange>& changes);
    void growTreeAt(world::World& world, SimulationPosition position,
                    world::Block sapling, std::vector<BlockChange>& changes);
    // Combined light level, Java's getLightLevel(pos, 0): max of the sky and
    // block channels. The stored arrays hold static full-sun values, which is
    // exactly what 1.16.1's growth and spread checks read (skylightSubtracted
    // is never applied to them).
    [[nodiscard]] static int lightAt(const world::World& world, SimulationPosition position);
    void wakeWaterNeighbors(const world::World& world, SimulationPosition position);
    [[nodiscard]] std::optional<std::uint8_t> updatedWaterLevel(
        const world::World& world,
        SimulationPosition position) const;
    [[nodiscard]] bool waterCanReplace(
        const world::World& world,
        SimulationPosition position) const;
    [[nodiscard]] bool hasDownwardFlowPath(
        const world::World& world,
        SimulationPosition position) const;
    [[nodiscard]] int distanceToDownwardFlow(
        const world::World& world,
        SimulationPosition origin,
        SimulationPosition previous,
        int depth) const;
    void setSimulatedBlock(
        world::World& world,
        SimulationPosition position,
        world::Block block,
        std::vector<BlockChange>& changes,
        std::uint8_t fluidLevel = 0U);

    std::deque<SimulationPosition> activeSand_;
    struct ScheduledWaterUpdate final {
        SimulationPosition position;
        std::uint64_t dueTick = 0U;
    };

    std::deque<ScheduledWaterUpdate> activeWater_;
    std::unordered_set<SimulationPosition, SimulationPositionHash> queuedWater_;
    std::deque<SimulationPosition> pendingSupportChecks_;
    std::unordered_set<SimulationPosition, SimulationPositionHash> queuedSupportChecks_;
    // Each unsupported leaf waits out its own draw from the random-tick
    // distribution, so a canopy dissolves in scattered pieces rather than
    // vanishing in one frame.
    struct ScheduledLeafDecay final {
        SimulationPosition position;
        std::uint64_t dueTick = 0U;
    };

    std::vector<ScheduledLeafDecay> pendingLeafDecays_;
    std::unordered_set<SimulationPosition, SimulationPositionHash> queuedLeafDecays_;
    std::uint32_t leafRandomState_ = 0x2545F491U;
    int randomTickSpeed_ = 3;
    std::uint32_t randomTickState_ = 0x2F6E2B1DU;
    // Saplings that rolled a growth wait out the per-tick cap here, in the
    // same due-tick shape leaf decay uses, so a whole batch that matures in
    // one tick spreads over the following ones instead of stalling a frame.
    struct ScheduledTreeGrowth final {
        SimulationPosition position;
        std::uint64_t dueTick = 0U;
    };

    std::vector<ScheduledTreeGrowth> pendingTreeGrowths_;
    std::unordered_set<SimulationPosition, SimulationPositionHash> queuedTreeGrowths_;
    std::size_t lastTreeGrowthsProcessed_ = 0U;
    std::size_t randomTickConversionsThisTick_ = 0U;
    std::size_t leafDecayChecksThisTick_ = 0U;
    std::size_t cropStateWritesThisTick_ = 0U;
    std::vector<FallingBlockEntity> fallingBlocks_;
    std::uint64_t tickCount_ = 0;
    std::size_t lastWaterUpdatesProcessed_ = 0U;
};

} // namespace mc::gameplay
