#pragma once

#include "gameplay/BlockEventQueue.hpp"
#include "gameplay/ChunkTickScheduler.hpp"
#include "gameplay/EnvironmentSnapshot.hpp"
#include "gameplay/RedstoneIslandPlanner.hpp"
#include "gameplay/RedstoneTorch.hpp"
#include "gameplay/RedstoneWireEvaluator.hpp"
#include "gameplay/SimulationPosition.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/WorldMutationService.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <array>
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


struct BlockChange final {
    SimulationPosition position;
    // The state the cell should end up in. One value rather than the block,
    // fluid level and optional orientation this used to carry: that trio could
    // not name a crop's new age except by smuggling it through the direction
    // enum, which is the overloading the state schema removed.
    world::BlockState state{};
    // The state that should drop as an item, air for none. Usually the state
    // removed by the edit; a failed falling-block landing carries the entity's
    // block here while worldChanged remains false. A popped crop keeps the age
    // it was broken at, so its loot table rolls against the right stage.
    world::BlockState dropped{};
    // Gravity entities hand geometry from a chunk mesh to a moving draw and
    // back again. Those two boundary edits must rebuild the render mesh in the
    // same frame or the old cube overlaps the entity (and the landing leaves a
    // temporary hole) while the background streamer catches up.
    bool immediateRenderUpdate = false;
    // False for an entity-only drop event. A falling block that cannot occupy
    // its landing cell must turn into an item without submitting a fictitious
    // edit for the block already stored in that cell.
    bool worldChanged = true;
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
    [[nodiscard]] friend bool operator==(const FallingBlockEntity&, const FallingBlockEntity&) =
        default;
};

[[nodiscard]] bool isCollectableWaterSource(
    const world::World& world,
    SimulationPosition position);

class WorldSimulation final {
  public:
    // Everything one randomly-ticked block needs. Passed by reference so the
    // dispatch stays a single indirect call with one argument, whatever the
    // behaviour ends up needing.
    struct RandomTickContext final {
        world::World& world;
        SimulationPosition position;
        world::Block block;
        std::vector<BlockChange>& changes;
        WorldSimulation& simulation;
    };
    using RandomTickFn = void (*)(const RandomTickContext&);

    // The table's entries. Plain function pointers, but members of
    // WorldSimulation so they can still reach its private state through the
    // context.
    static void randomTickGrassEntry(const RandomTickContext& context);
    static void randomTickSaplingEntry(const RandomTickContext& context);
    static void randomTickCropEntry(const RandomTickContext& context);
    static void randomTickFarmlandEntry(const RandomTickContext& context);
    static void randomTickSugarCaneEntry(const RandomTickContext& context);
    static void randomTickFireEntry(const RandomTickContext& context);

    // The behaviour table, indexed by block: a null entry means the block has no
    // random tick. This replaces the switch this used to be — 26.1 dispatches
    // through BlockBehaviour rather than a case list.
    //
    // It is constexpr and lives in the header on purpose. At randomTickSpeed 100
    // the draw loop runs tens of thousands of times per tick and virtually every
    // draw lands on air or stone, so the reject has to inline to one indexed
    // load. An out-of-line table costs a call plus a static-init guard on that
    // path, which measured ~4% slower than the switch — inside the plan's 5%
    // gate, but paying anything here for no reason is the wrong trade.
    static constexpr std::array<RandomTickFn, world::kBuiltinBlockCount> kRandomTickTable = [] {
        std::array<RandomTickFn, world::kBuiltinBlockCount> entries{};
        entries[world::blockId(world::Block::Grass).index()] = &randomTickGrassEntry;
        for (const auto sapling :
             {world::Block::OakSapling, world::Block::SpruceSapling, world::Block::BirchSapling,
              world::Block::JungleSapling, world::Block::AcaciaSapling,
              world::Block::DarkOakSapling}) {
            entries[world::blockId(sapling).index()] = &randomTickSaplingEntry;
        }
        for (const auto crop :
             {world::Block::WheatCrops, world::Block::Carrots, world::Block::Potatoes}) {
            entries[world::blockId(crop).index()] = &randomTickCropEntry;
        }
        entries[world::blockId(world::Block::Farmland).index()] = &randomTickFarmlandEntry;
        entries[world::blockId(world::Block::SugarCane).index()] = &randomTickSugarCaneEntry;
        entries[world::blockId(world::Block::Fire).index()] = &randomTickFireEntry;
        return entries;
    }();

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
    // longer still. Slow, but that is what vanilla does; this constant is the one
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

    // A redstone component at `position` learned a neighbour changed: read its
    // input and, if it needs to flip, schedule its toggle tick. This is the
    // block-update half of the redstone drive (RedstoneTorchBlock.neighborChanged)
    // — the write half is the scheduled tick drained inside tick(). A no-op for a
    // cell that is not a redstone component, so the neighbour fan-out can call it
    // for every neighbour without a type check at the call site.
    //
    // Non-const: most components only read `world` here and schedule a later
    // tick, but a trapdoor's neighborChanged (W-signal) writes its OPEN/POWERED
    // state synchronously, in this same block-update pass, exactly as vanilla's
    // TrapDoorBlock.neighborChanged calls level.setBlock inline rather than
    // scheduling anything — a trapdoor has no delay, unlike a torch's 2gt toggle.
    void notifyRedstoneComponent(world::World& world, SimulationPosition position);

    // AR-B4-6: where a comparator's container reading comes from.
    //
    // ComparatorBlock#getInputSignal asks the block behind it for an analog
    // output — a chest's fill level, a furnace's. Those live in the session's
    // block-entity store, which the simulation deliberately does not own, so the
    // session hands this in. A plain function pointer plus an opaque context
    // rather than a std::function: this is read on the redstone path, and the
    // simulation should not learn what a chest is to call it.
    //
    // Returns < 0 for "the block there has no analog output at all", which is
    // not the same as an empty container returning 0 — an empty chest still
    // overrides whatever signal is reaching the comparator, a stone block does
    // not. Unset means every block answers "no analog output", which is what the
    // headless redstone fixtures see.
    using AnalogOutputFn = int (*)(const void* context, world::BlockPos pos);
    void setAnalogOutputSource(AnalogOutputFn function, const void* context) {
        analogOutput_ = function;
        analogOutputContext_ = context;
    }
    [[nodiscard]] int analogOutputAt(world::BlockPos pos) const {
        return analogOutput_ == nullptr ? -1 : analogOutput_(analogOutputContext_, pos);
    }

    // W-x-1: DiodeBlock#updateNeighborsInFront (DiodeBlock.java:177-183), shared
    // by the repeater and the comparator — it is base-class behaviour in vanilla,
    // not something one of them does specially.
    //
    // A diode's own write goes out with flags 2, so it fans out nothing; without
    // this, *nothing downstream of a diode is ever woken*. Not just the sinks —
    // a wire two cells past a repeater stayed dark, so every multi-stage circuit
    // with a diode in the middle was broken.
    //
    // Two steps, and both are load-bearing:
    //   1. the output cell itself, so a trapdoor or a door hung directly on the
    //      diode's face reacts at all;
    //   2. that cell's own neighbours (minus the diode, Java's
    //      `ExceptFromFacing`), so a wire there re-solves and carries the change
    //      onward.
    //
    // `front` is `opposite(FACING)`: in this build a diode's FACING names its
    // *input* side (see RedstoneSignal's diodeInputSignal), so its output is the
    // cell behind that.
    void updateNeighborsInFront(world::World& world, world::BlockPos pos,
                                world::BlockState state, world::MutationSink& sink);

    // ComparatorBlock#refreshOutputState (ComparatorBlock.java:157-176) — the
    // whole body of a comparator's scheduled tick, and the second half of its
    // useWithoutItem.
    //
    // It exists as one named thing because vanilla calls it from both places and
    // the second caller is not optional: switching MODE changes the OUTPUT at
    // once (compare 15 against a 15 side input, subtract 0), so a mode click that
    // only wrote MODE would leave everything downstream on the old signal until
    // some unrelated edit happened to wake it. Re-deriving the analog output, the
    // POWERED flip and the front wake at the interaction site would be a second
    // copy of the same three steps — AR-B4-7 calls this instead.
    //
    // Writes flags 2 (clients only) like every other diode write, then wakes the
    // output cell through updateNeighborsInFront.
    struct ComparatorRefresh final {
        bool changed = false;
        world::BlockState newState{};
    };
    ComparatorRefresh refreshComparatorOutputState(world::World& world, world::BlockPos pos,
                                                   world::BlockState state,
                                                   world::MutationSink& sink);
    // The three entry points JE reaches updateNeighborsInFront through: its tick
    // (via LevelChunk.setBlockState -> onPlace, which runs even for a flags-2
    // write), onPlace proper, and affectNeighborsAfterRemoval. This is the
    // place/remove pair, called by the gameplay mutation sink once per edit;
    // `previous` is what the cell held before, so a diode that was just broken
    // still wakes what it used to feed.
    void notifyDiodePlacedOrRemoved(world::World& world, world::BlockPos pos,
                                    world::BlockState previous, world::BlockState current,
                                    world::MutationSink& sink);

    // The cells a synchronous neighborChanged sink actually wrote.
    //
    // `notifyRedstoneComponent` may write more cells than the one it was asked
    // about: a door's neighborChanged writes both of its halves. A caller that
    // has to tell the client what changed therefore cannot recover the set by
    // comparing the notified cell before and after — that was fine while the
    // trapdoor was the only synchronous sink, and it silently dropped the
    // door's far half the moment there were two (a door open in the world and
    // half-open on screen).
    //
    // The list is a mark/release stack so it survives re-entrancy: a sink write
    // fans out to more components, which may write again. Take a mark, run the
    // notification, publish everything appended past the mark, then truncate
    // back to it. The vector keeps its capacity, so a steady state allocates
    // nothing.
    [[nodiscard]] std::size_t synchronousWriteMark() const { return synchronousWrites_.size(); }
    [[nodiscard]] std::span<const world::BlockPos> synchronousWritesSince(std::size_t mark) const {
        return std::span<const world::BlockPos>{synchronousWrites_}.subspan(
            mark < synchronousWrites_.size() ? mark : synchronousWrites_.size());
    }
    void releaseSynchronousWrites(std::size_t mark) {
        if (mark < synchronousWrites_.size()) {
            synchronousWrites_.resize(mark);
        }
    }
    // Raised by RedstoneReactionSink for every real change the mutation service
    // reports (its onSectionDirty), which is the one callback that already fires
    // per changed cell regardless of the notify flags.
    void recordSynchronousWrite(world::BlockPos pos) { synchronousWrites_.push_back(pos); }
    // Appends every synchronous write since `mark` to `changes` (skipping cells
    // already listed there) and releases the mark.
    void collectSynchronousWrites(const world::World& world, std::size_t mark,
                                  std::vector<BlockChange>& changes);

    // An observer at `observerPos` saw a block-state change at `changedPos`
    // (the updateShape pass reports every neighbour a write touched). If that is
    // the block on its FACING side and it is not already pulsing, it schedules
    // its 2gt pulse. A no-op for anything that is not an observer watching that
    // cell, so the shape pass can call it for every neighbour.
    void notifyObserverShapeChange(const world::World& world, SimulationPosition observerPos,
                                   SimulationPosition changedPos);

    // A button was just pressed (POWERED set by the caller): schedule its
    // release tick the fixed number of gameticks later.
    void scheduleButtonRelease(SimulationPosition position);

    // W-8: DiodeBlock#setPlacedBy (DiodeBlock.java:159-163). A diode placed into
    // a line that is already live starts itself, scheduled one gametick out —
    // note `1`, not the diode's own delay, which is what vanilla writes here and
    // what makes a freshly placed repeater catch up quickly.
    //
    // It hangs on *placement* and nothing else. Java's onPlace, which runs on
    // every state write, would reschedule this on every POWERED flip and the
    // diode would never settle; MutationSink::onBlockPlaced is the callback that
    // keeps the two apart.
    void scheduleDiodeSelfStart(const world::World& world, world::BlockPos pos,
                                world::BlockState state);
    // Whether a block has a random tick at all — the draw loop's pre-filter, and
    // the cheapest possible statement of "does this block do anything on a
    // random tick". Public because it is a property of the block set, and a
    // block silently leaving it has no other symptom than grass quietly no
    // longer spreading.
    [[nodiscard]] static constexpr bool isRandomlyTicking(world::Block block) {
        return isRandomlyTicking(world::blockId(block));
    }
    [[nodiscard]] static constexpr bool isRandomlyTicking(world::BlockId block) {
        const auto index = block.index();
        return index < kRandomTickTable.size() && kRandomTickTable[index] != nullptr;
    }

    [[nodiscard]] std::vector<BlockChange> tick(
        world::World& world,
        bool processFluidUpdates = true);
    [[nodiscard]] const std::vector<FallingBlockEntity>& fallingBlocks() const {
        return fallingBlocks_;
    }
    // Puts a block back mid-fall from a save. Without it a world saved during a
    // collapse reloads with the column's blocks nowhere at all: they are in
    // neither the chunk nor the drop list while airborne.
    void restoreFallingBlock(glm::vec3 position, world::Block block, float verticalVelocity) {
        fallingBlocks_.push_back({position, position, verticalVelocity, block, false});
    }
    [[nodiscard]] std::size_t pendingWaterUpdateCount() const {
        return ticks_.pending(TickTask::Fluid);
    }
    [[nodiscard]] std::size_t lastWaterUpdatesProcessed() const {
        return lastWaterUpdatesProcessed_;
    }
    [[nodiscard]] std::size_t pendingLeafDecayCount() const {
        return ticks_.pending(TickTask::LeafDecay);
    }
    // The `/gamerule randomTickSpeed` value: how many random blocks per
    // 16x16x16 section are drawn every game tick. Vanilla's default is 3; 0
    // disables random ticks (and with them grass spread, sapling growth and
    // leaf decay).
    // Drops every tick scheduled inside a chunk the world no longer holds.
    // With the flat queues an entry outlived its chunk and later fired against
    // cells that were not loaded; per-chunk storage is what makes forgetting
    // them a single erase.
    void forgetChunk(int chunkX, int chunkZ) { ticks_.forgetChunk(chunkX, chunkZ); }
    [[nodiscard]] const ChunkTickScheduler& scheduledTicks() const { return ticks_; }

    void setRandomTickSpeed(int speed) { randomTickSpeed_ = speed; }
    [[nodiscard]] int randomTickSpeed() const { return randomTickSpeed_; }

    // Confine the random-tick pass to a `radiusChunks` window around the chunk
    // at (centerChunkX, centerChunkZ) — the simulation distance, set from the
    // player's chunk each tick (GameSession). A negative radius restores the
    // unbounded walk over every loaded chunk (the headless-test default).
    void setSimulationBounds(int centerChunkX, int centerChunkZ, int radiusChunks) {
        simCenterChunkX_ = centerChunkX;
        simCenterChunkZ_ = centerChunkZ;
        simRadiusChunks_ = radiusChunks;
    }

    // The block the simulation is centred on — the player's feet. The chunk
    // bounds above are the coarse window random ticks walk; this is the exact
    // point the `fire_spread_radius_around_player` rule measures from, which is
    // a block radius and so cannot be answered at chunk granularity.
    void setSimulationCenterBlock(int x, int y, int z) {
        simCenterBlockX_ = x;
        simCenterBlockY_ = y;
        simCenterBlockZ_ = z;
    }

    // The `/gamerule fire_spread_radius_around_player` value: -1 ticks fire
    // anywhere, 0 nowhere (26.1's replacement for the retired doFireTick=false),
    // and a positive value only within that many blocks of the simulation
    // centre. Gates the whole fire tick — spread, aging and burning out — the
    // way ServerLevel#canSpreadFireAround gates FireBlock#tick.
    void setFireSpreadRadius(int radius) { fireSpreadRadius_ = radius; }
    [[nodiscard]] int fireSpreadRadius() const { return fireSpreadRadius_; }
    [[nodiscard]] bool canSpreadFireAround(SimulationPosition position) const {
        if (fireSpreadRadius_ < 0) {
            return true;
        }
        const std::int64_t dx = position.x - simCenterBlockX_;
        const std::int64_t dy = position.y - simCenterBlockY_;
        const std::int64_t dz = position.z - simCenterBlockZ_;
        const std::int64_t radius = fireSpreadRadius_;
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }

    // How the redstone component ticks due on a gametick are ordered before they
    // drain (W-6). Serial — the default and ground truth — drains the whole due
    // set in the single (dueTick, priority, subTickOrder) order Java uses. Island
    // partitions the due set into components that cannot influence each other this
    // tick and drains island-major (islands ordered by min (chunkPos, packed pos),
    // ticks within an island still in drain order). Island is a *reordering* of
    // the same serial drain, single-threaded: it exists so the lockstep gate can
    // prove the partition is bit-for-bit identical to Serial, the analysis a
    // future threaded evaluator needs. It is never the default.
    enum class RedstoneDrainMode : std::uint8_t { Serial, Island };
    void setRedstoneDrainMode(RedstoneDrainMode mode) { redstoneDrainMode_ = mode; }
    [[nodiscard]] RedstoneDrainMode redstoneDrainMode() const { return redstoneDrainMode_; }
    // The number of islands the last Island-mode redstone drain partitioned the
    // due set into (0 after a Serial tick). The parallelism the partition exposes,
    // read by the benchmark and the multi-island test.
    [[nodiscard]] std::size_t lastRedstoneIslandCount() const { return lastRedstoneIslandCount_; }

    // The environment the current tick runs under, resolved once by the session
    // and handed down. Growth and spreading read fields off it rather than
    // asking a clock or the weather what is going on — 26.1 routes the same
    // facts through EnvironmentAttributes for the same reason.
    void setEnvironment(const EnvironmentSnapshot& environment) { environment_ = environment; }
    [[nodiscard]] const EnvironmentSnapshot& environment() const { return environment_; }
    [[nodiscard]] std::size_t lastTreeGrowthsProcessed() const {
        return lastTreeGrowthsProcessed_;
    }
    [[nodiscard]] std::size_t pendingTreeGrowthCount() const {
        return ticks_.pending(TickTask::TreeGrowth);
    }
    [[nodiscard]] std::size_t lastRandomTickConversions() const {
        return randomTickConversionsThisTick_;
    }

  private:
    // AR-B4-3: see synchronousWriteMark() above.
    std::vector<world::BlockPos> synchronousWrites_;
    AnalogOutputFn analogOutput_ = nullptr;
    const void* analogOutputContext_ = nullptr;
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
    // ServerLevel#tickPrecipitation, run once per chunk per tick at one draw in
    // sixteen — vanilla runs it in tickChunk ahead of the random ticks, and it is
    // not one of them, so randomTickSpeed does not gate it.
    void precipitationTick(world::World& world, const world::Chunk& chunk,
                           world::ChunkPosition chunkPosition,
                           std::vector<BlockChange>& changes);
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
    // CropBlock#randomTick: grows one age when the block above is lit enough
    // and the moisture-weighted growth roll passes. The grown state is written
    // to the world and emitted as a BlockChange.
    void randomTickCrop(world::World& world, SimulationPosition position,
                        std::vector<BlockChange>& changes);
    // FarmlandBlock#randomTick: near water the moisture jumps to 7, otherwise it
    // dries one level, reverting to dirt at 0 once no crop stands on it.
    void randomTickFarmland(world::World& world, SimulationPosition position,
                            std::vector<BlockChange>& changes);
    // SugarCaneBlock#randomTick: when the cell above is air and the stack is
    // shorter than three, an AGE counter climbs to 15 and then places a new
    // sugar cane above (resetting this cell's age). A crop-style state write,
    // so it shares the crop-write budget. Deterministic via mc::rng only.
    void randomTickSugarCane(world::World& world, SimulationPosition position,
                             std::vector<BlockChange>& changes);
    // AR-CX4-b: FireBlock#randomTick (simplified). The AGE counter climbs one per
    // random tick to 15; on each tick fire also tries to spread to a flammable
    // neighbour, and once it is old (or loses its footing) it burns out to air.
    // The burn budget is shared with the crop-write budget so a wildfire cannot
    // flood one tick's change pipeline. Spread targets are chosen with the
    // deterministic random-tick LCG (nextBounded), never a wall clock.
    void randomTickFire(world::World& world, SimulationPosition position,
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
    // getRawBrightness(pos, 0) and getMaxLocalRawBrightness(pos): the two
    // readings vanilla actually distinguishes. These used to be one shared
    // `lightAt` that never subtracted anything, which is why growth and
    // spreading behaved identically at noon and at midnight.
    [[nodiscard]] static int rawBrightnessAt(
        const world::World& world, SimulationPosition position);
    [[nodiscard]] int localBrightnessAt(
        const world::World& world, SimulationPosition position) const;
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
    bool setSimulatedBlock(
        world::World& world,
        SimulationPosition position,
        world::Block block,
        std::vector<BlockChange>& changes,
        std::uint8_t fluidLevel = 0U,
        bool immediateRenderUpdate = false);

    // Runs one redstone component's due tick: applies its state change through
    // the mutation service (flags 3, so the change fans out to neighbours and
    // wakes the components it feeds) and records the BlockChange for the mesher.
    void dispatchRedstoneTick(world::World& world, SimulationPosition position,
                              std::vector<BlockChange>& changes);

    // Settles one piston block event at tick end (phase two): flips EXTENDED.
    // The actual block movement is a separate task; this carries the state.
    void settlePistonEvent(world::World& world, const BlockEvent& event,
                           std::vector<BlockChange>& changes);

    // Every simulated block write goes through this, so the "did the cell
    // actually change?" rule is the one the player's edits use.
    world::WorldMutationService mutations_;

    // The five flat queues this replaced — falling sand, fluid, support checks,
    // leaf decay, tree growth — were the same `{position, dueTick}` + dedupe-set
    // shape five times over. Keyed by chunk they can be dropped when the chunk
    // unloads and saved with it; the drain order is unchanged (see the header).
    ChunkTickScheduler ticks_;
    // The recent off-toggles behind redstone-torch burnout, shared across the
    // world's torches (Java's RECENT_TOGGLES list, packed).
    redstone::TorchBurnoutTracker torchBurnout_;
    // The AC wire evaluator (W-5): one instance reused across every wire tick so
    // its wavefront/cell/map buffers are a live per-tick arena (zero allocation
    // once warm). Replaces RedstoneWire.hpp's naive relaxation on the hot path;
    // the serial evaluator survives only as the lockstep cross-check oracle.
    redstone::WireNetworkEvaluator wireEvaluator_;
    // The island partitioner (W-6, analysis scope): reused across ticks so its
    // union-find and cell arenas stay warm. Only touched in Island drain mode.
    redstone::RedstoneIslandPlanner islandPlanner_;
    std::vector<std::size_t> islandOffsets_;
    RedstoneDrainMode redstoneDrainMode_ = RedstoneDrainMode::Serial;
    std::size_t lastRedstoneIslandCount_ = 0U;
    // Piston/note block events, collected through a tick and settled at its end
    // (Java's Level.blockEvent). This is the W-2 queue's first live consumer.
    BlockEventQueue blockEvents_;
    EnvironmentSnapshot environment_{};
    std::uint32_t leafRandomState_ = 0x2545F491U;
    int randomTickSpeed_ = 3;
    // The simulation-distance window random ticks are confined to, in chunks
    // around simCenterChunk*. 26.1 only random-ticks chunks within the
    // simulation distance (default 10), decoupled from render distance; without
    // this, randomTicks walked *every* loaded chunk (World::positions()),
    // multiplying the per-tick cost by the whole render volume and — because the
    // pass runs under the world write lock — stalling the render thread at a high
    // randomTickSpeed. A negative radius means "unbounded" (walk positions()),
    // the default headless tests rely on so they need not place a player first.
    int simCenterChunkX_ = 0;
    int simCenterChunkZ_ = 0;
    int simRadiusChunks_ = -1;
    // The exact centre block (the player's feet), for the block-radius fire
    // rule. Headless tests never place a player, so the default sits at the
    // origin — harmless, because fireSpreadRadius_ defaults to 128 and every
    // such test builds its fire within a few blocks of it.
    int simCenterBlockX_ = 0;
    int simCenterBlockY_ = 0;
    int simCenterBlockZ_ = 0;
    int fireSpreadRadius_ = 128;
    std::uint32_t randomTickState_ = 0x2F6E2B1DU;
    // Its own stream, so changing randomTickSpeed does not shift where ice forms.
    std::uint32_t precipitationRandomState_ = 0x7F4A7C15U;
    std::size_t lastTreeGrowthsProcessed_ = 0U;
    std::size_t randomTickConversionsThisTick_ = 0U;
    std::size_t leafDecayChecksThisTick_ = 0U;
    std::size_t cropStateWritesThisTick_ = 0U;
    std::vector<FallingBlockEntity> fallingBlocks_;
    std::uint64_t tickCount_ = 0;
    std::size_t lastWaterUpdatesProcessed_ = 0U;
};

} // namespace mc::gameplay
