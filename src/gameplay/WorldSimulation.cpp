#include "gameplay/WorldSimulation.hpp"

#include "gameplay/RedstoneDiode.hpp"
#include "gameplay/RedstoneSignal.hpp"
#include "world/BlockPlacement.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/TreeGrower.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>

namespace mc::gameplay {
namespace {

constexpr std::array<SimulationPosition, 4> kHorizontalWaterNeighbors{{
    {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1},
}};

// A cell water may flow into: empty, replaceable, or a decoration block that
// flowing water destroys.
[[nodiscard]] bool waterCanOccupy(world::Block block) {
    return world::isReplaceable(block) || world::isDestroyedByFluid(block);
}

[[nodiscard]] SimulationPosition offsetPosition(
    SimulationPosition position,
    SimulationPosition offset) {
    return {
        position.x + offset.x,
        position.y + offset.y,
        position.z + offset.z,
    };
}

constexpr std::array<SimulationPosition, 6> kSixNeighbors{{
    {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0},
}};

[[nodiscard]] constexpr std::array<SimulationPosition, 26> makeLeafFloodNeighbors() {
    std::array<SimulationPosition, 26> offsets{};
    std::size_t count = 0U;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -1; z <= 1; ++z) {
                if (x != 0 || y != 0 || z != 0) {
                    offsets[count++] = {x, y, z};
                }
            }
        }
    }
    return offsets;
}

// Deciding whether a leaf survives counts the six orthogonal steps that
// LeavesBlock.DISTANCE counts, and nothing else. Deciding which leaves get
// *asked* the question is a different job, and it has to reach further: a
// canopy is regularly only diagonally connected — the blob foliage placer trims
// layer corners at random, and a crown sliced by a chunk border leaves stragglers
// behind — and a cluster the flood never reaches is a cluster nothing ever
// schedules, so it hangs in the air forever. Walking all 26 neighbours here
// costs a wider flood and nothing else: a leaf that turns out to be supported is
// simply dropped again on its first check.
constexpr std::array<SimulationPosition, 26> kLeafFloodNeighbors = makeLeafFloodNeighbors();

[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

// Uniform integer in [0, bound) drawn from the same xorshift stream. The
// random-tick pass only bounds by 3, 5, 7 and 16, so the modulo bias of a
// plain remainder is far below anything observable.
[[nodiscard]] std::uint32_t nextBounded(std::uint32_t& state, std::uint32_t bound) {
    return nextRandom(state) % bound;
}

[[nodiscard]] int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if (value % divisor < 0) {
        --quotient;
    }
    return quotient;
}

// How long a block waits for a random tick that fires with the given per-tick
// probability. Drawing the wait up front costs one sample per leaf instead of
// one coin flip per leaf per tick, and produces the same distribution.
[[nodiscard]] std::uint64_t randomTickDelay(std::uint32_t& state, double chancePerTick) {
    const double uniform =
        (static_cast<double>(nextRandom(state) >> 8U) + 0.5) / 16777216.0;
    const double delay = std::log(uniform) / std::log(1.0 - chancePerTick);
    return static_cast<std::uint64_t>(delay) + 1U;
}

// The simulation's own sink. Its writes report their consequences through the
// BlockChange stream the host already drains, so nothing is dispatched here —
// what the service is used for is the single, shared decision about *whether*
// a cell changed, and the block-entity rule that rides on it.
class RecordingMutationSink final : public world::MutationSink {};

// The sink a redstone component's own tick writes through. Unlike the recording
// sink above, this one *does* react to the neighbour fan-out: a torch going out
// (flags 3) must wake the components it feeds so they schedule their own ticks —
// the block-update propagation that carries a signal change through a circuit.
// The mesh/save side still travels as BlockChange, recorded by the caller.
class RedstoneReactionSink final : public world::MutationSink {
  public:
    RedstoneReactionSink(const world::World& world, WorldSimulation& simulation)
        : world_(world), simulation_(simulation) {}

    void onNeighborChanged(world::BlockPos neighbor, world::BlockPos /*source*/) override {
        simulation_.notifyRedstoneComponent(world_, {neighbor.x, neighbor.y, neighbor.z});
    }

  private:
    const world::World& world_;
    WorldSimulation& simulation_;
};

} // namespace

bool isCollectableWaterSource(
    const world::World& world,
    SimulationPosition position) {
    return world.block(position.x, position.y, position.z) ==
               world::Block::Water &&
        world.fluidLevel(position.x, position.y, position.z) == 0U;
}

void WorldSimulation::queueSand(SimulationPosition position) {
    if (position.y > world::kMinY && position.y < world::kMaxY) {
        // Two properties of the flat deque this replaced, both expressed as the
        // due tick rather than as a flag:
        //   * No de-duplication — the same cell could be queued twice and each
        //     entry was re-checked against the world when it fired.
        //   * Due *next* tick, so a block that starts falling does not cascade
        //     its whole column within one tick. The old loop got this from
        //     computing its batch size before iterating; support checks, which
        //     are due immediately, deliberately do cascade.
        static_cast<void>(
            ticks_.schedule(TickTask::FallingBlock, position, tickCount_ + 1U, true));
    }
}

void WorldSimulation::queueWater(SimulationPosition position, std::uint8_t level) {
    static_cast<void>(level);
    if (!world::isWorldYInRange(position.y)) {
        return;
    }
    // WaterFluid#getTickRate is five game ticks in Java 1.16.1. Store the
    // deadline per position instead of releasing the whole queue on one
    // global modulo tick.
    static_cast<void>(ticks_.schedule(TickTask::Fluid, position, tickCount_ + 5U));
}

void WorldSimulation::queueSupportCheck(SimulationPosition position) {
    if (!world::isWorldYInRange(position.y)) {
        return;
    }
    static_cast<void>(ticks_.schedule(TickTask::SupportCheck, position, tickCount_));
}

void WorldSimulation::queueNeighborSupportChecks(SimulationPosition position) {
    for (const auto offset : kSixNeighbors) {
        queueSupportCheck(offsetPosition(position, offset));
    }
}

void WorldSimulation::queueLeafDecay(SimulationPosition position) {
    if (!world::isWorldYInRange(position.y) || randomTickSpeed_ <= 0 ||
        ticks_.contains(TickTask::LeafDecay, position)) {
        return;
    }
    // LeavesBlock decays on a random tick, so the wait scales with the
    // /gamerule randomTickSpeed exactly as vanilla's per-section draw count
    // does (a speed of 3 keeps the original 3/4096 pacing).
    const double chancePerTick =
        kLeafDecayChancePerTick * static_cast<double>(randomTickSpeed_) / 3.0;
    static_cast<void>(ticks_.schedule(
        TickTask::LeafDecay, position,
        tickCount_ + randomTickDelay(leafRandomState_, chancePerTick)));
}

void WorldSimulation::queueLeafDecayChecks(
    const world::World& world,
    SimulationPosition origin) {
    // Almost every block change happens nowhere near a tree, so check the
    // neighbours before setting up a flood fill for them.
    const bool touchesLeaves = std::ranges::any_of(
        kLeafFloodNeighbors, [&](SimulationPosition offset) {
            const auto neighbor = offsetPosition(origin, offset);
            return world::isLeaves(world.block(neighbor.x, neighbor.y, neighbor.z));
        });
    if (!touchesLeaves) {
        return;
    }
    std::vector<SimulationPosition> frontier;
    std::unordered_set<SimulationPosition, SimulationPositionHash> visited;
    frontier.push_back(origin);
    visited.insert(origin);
    // One extra ring beyond the property's range, because the flood starts at
    // the block that changed rather than at the first leaf.
    for (int step = 0; step <= world::kMaximumLeafSupportDistance; ++step) {
        std::vector<SimulationPosition> next;
        for (const auto cell : frontier) {
            for (const auto offset : kLeafFloodNeighbors) {
                const auto neighbor = offsetPosition(cell, offset);
                if (!world::isLeaves(world.block(neighbor.x, neighbor.y, neighbor.z)) ||
                    !visited.insert(neighbor).second) {
                    continue;
                }
                queueLeafDecay(neighbor);
                next.push_back(neighbor);
            }
        }
        frontier = std::move(next);
    }
}

LeafSupport WorldSimulation::leafSupport(
    const world::World& world,
    SimulationPosition origin) const {
    // LeavesBlock#getDistanceFromLog counts a log as 0 and adds one per leaf
    // step, so leaves survive while any log sits within six steps of them
    // through other leaves.
    std::vector<SimulationPosition> frontier{origin};
    std::unordered_set<SimulationPosition, SimulationPositionHash> visited{origin};
    bool sawUnloadedChunk = false;
    for (int step = 0; step < world::kMaximumLeafSupportDistance; ++step) {
        std::vector<SimulationPosition> next;
        for (const auto cell : frontier) {
            for (const auto offset : kSixNeighbors) {
                const auto neighbor = offsetPosition(cell, offset);
                if (!world.hasChunk({
                        static_cast<int>(std::floor(
                            static_cast<float>(neighbor.x) / world::kChunkWidth)),
                        static_cast<int>(std::floor(
                            static_cast<float>(neighbor.z) / world::kChunkDepth)),
                    })) {
                    // The trunk may be sitting in a chunk that has not streamed
                    // in yet. Finding a log anywhere else still settles the
                    // question, so note the gap and keep looking rather than
                    // calling the whole canopy supported on the spot.
                    sawUnloadedChunk = true;
                    continue;
                }
                const auto block = world.block(neighbor.x, neighbor.y, neighbor.z);
                if (world::isLog(block)) {
                    return LeafSupport::Supported;
                }
                if (!world::isLeaves(block) || !visited.insert(neighbor).second) {
                    continue;
                }
                next.push_back(neighbor);
            }
        }
        frontier = std::move(next);
    }
    return sawUnloadedChunk ? LeafSupport::Undecided : LeafSupport::Unsupported;
}

void WorldSimulation::decayLeaves(world::World& world, std::vector<BlockChange>& changes) {
    // A high randomTickSpeed shortens every leaf's decay delay, so a big canopy
    // can have hundreds of due checks in one tick — each a flood fill. The
    // budget is the drain cap; the rest stay queued for later ticks.
    const std::size_t budget =
        kMaximumLeafDecayChecksPerTick > leafDecayChecksThisTick_
            ? kMaximumLeafDecayChecksPerTick - leafDecayChecksThisTick_
            : 0U;
    leafDecayChecksThisTick_ += ticks_.drainDue(
        TickTask::LeafDecay, tickCount_, budget, [&](SimulationPosition position) {
        const auto state = world.state(position.x, position.y, position.z);
        const auto block = state.block();
        if (!world::isLeaves(block) || state.persistent()) {
            return;
        }
        const auto support = leafSupport(world, position);
        if (support == LeafSupport::Supported) {
            return;
        }
        if (support == LeafSupport::Undecided) {
            // The search ran out of loaded world before it could find a trunk.
            // Vanilla does not tick unloaded chunks either, but it never forgets
            // the leaf: it is still there to be random-ticked once the chunk is
            // back. Dropping the entry here instead is what left canopies near
            // the streaming frontier hanging for good, so put it back with a
            // fresh draw and ask again later.
            queueLeafDecay(position);
            return;
        }
        const std::size_t changeCount = changes.size();
        setSimulatedBlock(world, position, world::Block::Air, changes);
        if (changes.size() > changeCount) {
            // Decayed leaves roll their loot table exactly like a mined block,
            // which is what dropStacks does before removeBlock in vanilla.
            changes.back().dropped = state;
        }
        wakeWaterNeighbors(world, position);
        queueNeighborSupportChecks(position);
        for (const auto offset : kLeafFloodNeighbors) {
            const auto neighbor = offsetPosition(position, offset);
            if (world::isLeaves(world.block(neighbor.x, neighbor.y, neighbor.z))) {
                queueLeafDecay(neighbor);
            }
        }
    });
}

int WorldSimulation::rawBrightnessAt(const world::World& world, SimulationPosition position) {
    // getRawBrightness(pos, 0): the sky channel as stored, no darkening. Crops
    // grow by this reading, so a field keeps filling out after dusk.
    return environment::rawBrightness(world, position.x, position.y, position.z);
}

int WorldSimulation::localBrightnessAt(
    const world::World& world,
    SimulationPosition position) const {
    // getMaxLocalRawBrightness(pos): the same reading minus the tick's ambient
    // darkness. Spreading and sapling growth use this one and therefore stop at
    // night, which is the behaviour the single shared reading used to lose.
    return environment::maxLocalRawBrightness(world, position.x, position.y, position.z,
                                              environment_);
}

void WorldSimulation::randomTicks(world::World& world, std::vector<BlockChange>& changes) {
    if (randomTickSpeed_ <= 0) {
        return;
    }
    // ServerWorld#tickChunk: randomTickSpeed draws per non-empty 16x16x16
    // section. World::positions returns a copy, so a streamed batch swapping a
    // chunk in mid-pass never invalidates the snapshot.
    for (const auto chunkPosition : world.positions()) {
        const world::Chunk* chunk = world.chunk(chunkPosition);
        if (chunk == nullptr) {
            continue;
        }
        for (int sectionY = 0; sectionY < world::kSectionCount; ++sectionY) {
            const world::ChunkSection& section = chunk->section(sectionY);
            if (section.empty()) {
                continue;
            }
            // Read the drawn cell straight out of the held section. Going back
            // through world.block() would re-resolve the chunk hash map for
            // every one of the tens of thousands of draws a high randomTickSpeed
            // performs per tick, which alone ate most of a frame at speed 100.
            for (int pick = 0; pick < randomTickSpeed_; ++pick) {
                const int localX =
                    static_cast<int>(nextBounded(randomTickState_, world::kChunkWidth));
                const int localY =
                    static_cast<int>(nextBounded(randomTickState_, world::kSectionSize));
                const int localZ =
                    static_cast<int>(nextBounded(randomTickState_, world::kChunkDepth));
                // isRandomlyTicking first: at a high randomTickSpeed almost
                // every draw lands on air or stone, and rejecting those with one
                // array read rather than a call is what keeps the table at least
                // as fast as the switch it replaces.
                const auto drawn = section.block(localX, localY, localZ);
                if (!isRandomlyTicking(drawn)) {
                    continue;
                }
                const SimulationPosition position{
                    chunkPosition.x * world::kChunkWidth + localX,
                    world::sectionOriginY(sectionY) + localY,
                    chunkPosition.z * world::kChunkDepth + localZ,
                };
                randomTickBlock(world, position, drawn, changes);
            }
        }
    }
}

void WorldSimulation::randomTickGrassEntry(const RandomTickContext& context) {
    context.simulation.randomTickGrass(context.world, context.position, context.changes);
}

void WorldSimulation::randomTickSaplingEntry(const RandomTickContext& context) {
    context.simulation.randomTickSapling(context.world, context.position, context.changes);
}

void WorldSimulation::randomTickCropEntry(const RandomTickContext& context) {
    context.simulation.randomTickCrop(context.world, context.position,
                                      context.changes);
}

void WorldSimulation::randomTickFarmlandEntry(const RandomTickContext& context) {
    context.simulation.randomTickFarmland(context.world, context.position, context.changes);
}

void WorldSimulation::randomTickBlock(
    world::World& world,
    SimulationPosition position,
    world::Block block,
    std::vector<BlockChange>& changes) {
    const auto index = static_cast<std::size_t>(block);
    if (index >= kRandomTickTable.size() || kRandomTickTable[index] == nullptr) {
        return;
    }
    kRandomTickTable[index](RandomTickContext{world, position, block, changes, *this});
}

void WorldSimulation::randomTickGrass(
    world::World& world,
    SimulationPosition position,
    std::vector<BlockChange>& changes) {
    // SpreadableBlock#canSurvive is independent of the time of day: it asks how
    // much the block above physically shields this face (water reports 3 and a
    // full block 15), not how dark the sky currently looks. Using the local
    // brightness here made leaves-filtered sky light fall from 14 to 3 at
    // midnight and turned perfectly healthy grass into dirt until sunrise.
    const SimulationPosition above{position.x, position.y + 1, position.z};
    const auto aboveBlock = world.block(above.x, above.y, above.z);
    if (world::opacity(aboveBlock) > 2) {
        reserveConversionAndApply(world, position, world::Block::Dirt, changes);
        return;
    }
    // Only propagation follows the sun. Open ground reads 4 at midnight (and
    // leaves-filtered ground can read 3), both below the vanilla spread gate of
    // 9 without affecting the source grass's survival.
    if (localBrightnessAt(world, above) < 9) {
        return;
    }
    // SpreadableBlock#randomTick: up to four probes over a 3x3x5 volume reach
    // the surrounding soil. Only plain dirt converts, and only where the grass
    // state could survive (soil below).
    for (int attempt = 0; attempt < 4; ++attempt) {
        const SimulationPosition target{
            position.x + static_cast<int>(nextBounded(randomTickState_, 3U)) - 1,
            position.y + static_cast<int>(nextBounded(randomTickState_, 5U)) - 3,
            position.z + static_cast<int>(nextBounded(randomTickState_, 3U)) - 1,
        };
        if (!world::isWorldYInRange(target.y)) {
            continue;
        }
        if (world.block(target.x, target.y, target.z) != world::Block::Dirt) {
            continue;
        }
        // The probe range reaches one below the surface, so without a light
        // check the spread would flip the dirt layer under the grass to grass,
        // which then dies for being covered — a constant churn of conversions
        // that drowns real spread and eats the per-tick budget. The target must
        // be able to hold grass itself, but that survival check is physical
        // opacity/fluid state rather than the current day/night brightness.
        const SimulationPosition targetAbove{target.x, target.y + 1, target.z};
        if (targetAbove.y >= world::kMaxY ||
            world::opacity(world.block(targetAbove.x, targetAbove.y, targetAbove.z)) > 2) {
            continue;
        }
        if (!world::canBlockSurvive(world, {target.x, target.y, target.z},
                                    world::Block::Grass, world::BlockOrientation::North)) {
            continue;
        }
        reserveConversionAndApply(world, target, world::Block::Grass, changes);
    }
}

void WorldSimulation::reserveConversionAndApply(
    world::World& world,
    SimulationPosition position,
    world::Block block,
    std::vector<BlockChange>& changes) {
    if (randomTickConversionsThisTick_ >= kMaximumRandomTickConversionsPerTick) {
        return;
    }
    ++randomTickConversionsThisTick_;
    setSimulatedBlock(world, position, block, changes);
}

void WorldSimulation::randomTickSapling(
    world::World& world,
    SimulationPosition position,
    std::vector<BlockChange>& changes) {
    static_cast<void>(changes);
    // SaplingBlock#randomTick: needs getMaxLocalRawBrightness 9 above the
    // sapling and rolls 1/7 before growing. The reading follows the sun, so
    // saplings sit through the night instead of sprouting in the dark.
    const SimulationPosition above{position.x, position.y + 1, position.z};
    if (localBrightnessAt(world, above) < 9) {
        return;
    }
    if (nextBounded(randomTickState_, 7U) != 0U) {
        return;
    }
    queueTreeGrowth(position);
}

float WorldSimulation::availableMoisture(
    const world::World& world,
    SimulationPosition position) {
    // CropsBlock#getAvailableMoisture: each farmland cell in the 3x3 ring below
    // the crop contributes to a growth multiplier — 1.0 for the cell directly
    // under the crop (3.0 when that farmland is moist), a quarter of that for
    // the eight neighbours. Nine moist farmland blocks total about 10.
    float moisture = 1.0F;
    const SimulationPosition below{position.x, position.y - 1, position.z};
    if (below.y < world::kMinY) {
        return moisture;
    }
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int sampleX = below.x + dx;
            const int sampleZ = below.z + dz;
            float contribution = 0.0F;
            if (world::isFarmland(world.block(sampleX, below.y, sampleZ))) {
                contribution = 1.0F;
                if (world.state(sampleX, below.y, sampleZ).moisture() > 0) {
                    contribution = 3.0F;
                }
            }
            if (dx != 0 || dz != 0) {
                contribution /= 4.0F;
            }
            moisture += contribution;
        }
    }
    return moisture;
}

bool WorldSimulation::farmlandNearWater(
    const world::World& world,
    SimulationPosition position) {
    // FarmlandBlock#isNearWater: any water within four blocks horizontally at
    // the farmland's level. The scan radius is fixed rather than animated so
    // the hydration check never depends on a distance value being readable.
    for (int dz = -4; dz <= 4; ++dz) {
        for (int dx = -4; dx <= 4; ++dx) {
            const SimulationPosition probe{position.x + dx, position.y, position.z + dz};
            if (world::isFluid(world.block(probe.x, probe.y, probe.z))) {
                return true;
            }
        }
    }
    return false;
}

bool WorldSimulation::cropOnTop(
    const world::World& world,
    SimulationPosition position) {
    const SimulationPosition above{position.x, position.y + 1, position.z};
    if (above.y >= world::kMaxY) {
        return false;
    }
    return world::isCrop(world.block(above.x, above.y, above.z));
}

void WorldSimulation::randomTickCrop(
    world::World& world,
    SimulationPosition position,
    std::vector<BlockChange>& changes) {
    // CropBlock#randomTick: `getRawBrightness(pos, 0) >= 9` — the crop's own
    // cell, and deliberately without the day's darkening, which is why a wheat
    // field keeps growing overnight. This used to read the cell above with the
    // shared light helper; a crop is transparent so the two readings usually
    // agree, but under an overhang they do not.
    if (rawBrightnessAt(world, position) < 9) {
        return;
    }
    const auto cropState = world.state(position.x, position.y, position.z);
    const int age = cropState.age();
    if (age >= 7) {
        return;
    }
    // getAvailableMoisture + nextInt(floor(25 / f) + 1): a lone crop grows on
    // ~1/26 of its ticks, one ringed by nine moist farmland blocks on ~1/3.
    const float moisture = availableMoisture(world, position);
    const std::uint32_t growthBound = static_cast<std::uint32_t>(25.0F / moisture) + 1U;
    if (nextBounded(randomTickState_, growthBound) != 0U) {
        return;
    }
    if (!reserveCropStateWrite()) {
        return;
    }
    const auto grown = cropState.withAge(age + 1);
    world.setState(position.x, position.y, position.z, grown);
    changes.push_back({position, grown});
}

void WorldSimulation::randomTickFarmland(
    world::World& world,
    SimulationPosition position,
    std::vector<BlockChange>& changes) {
    // FarmlandBlock#randomTick: water within four blocks *or rain falling on
    // it* hydrates the soil (the moisture jumps straight to 7, vanilla never
    // raises it gradually); without either it dries one level, and at moisture 0
    // with nothing planted on it the farmland reverts to plain dirt.
    const auto farmlandState = world.state(position.x, position.y, position.z);
    const int moisture = farmlandState.moisture();
    const SimulationPosition aboveFarmland{position.x, position.y + 1, position.z};
    const bool hydrated = farmlandNearWater(world, position) ||
                          isRainingAt(world, aboveFarmland.x, aboveFarmland.y, aboveFarmland.z,
                                      environment_);
    if (!hydrated) {
        if (moisture > 0) {
            if (!reserveCropStateWrite()) {
                return;
            }
            const auto dried = farmlandState.withMoisture(moisture - 1);
            world.setState(position.x, position.y, position.z, dried);
            changes.push_back({position, dried});
        } else if (!cropOnTop(world, position)) {
            // FarmlandBlock reverts only when nothing stands on it, exactly like
            // the vanilla hasCrops guard.
            if (!reserveCropStateWrite()) {
                return;
            }
            setSimulatedBlock(world, position, world::Block::Dirt, changes);
        }
        return;
    }
    if (moisture < 7) {
        if (!reserveCropStateWrite()) {
            return;
        }
        const auto hydratedState = farmlandState.withMoisture(7);
        world.setState(position.x, position.y, position.z, hydratedState);
        changes.push_back({position, hydratedState});
    }
}

void WorldSimulation::queueTreeGrowth(SimulationPosition position) {
    if (!world::isWorldYInRange(position.y) ||
        ticks_.contains(TickTask::TreeGrowth, position)) {
        return;
    }
    // Due next tick; growTrees drains the queue at the per-tick cap from there.
    static_cast<void>(ticks_.schedule(TickTask::TreeGrowth, position, tickCount_ + 1U));
}

void WorldSimulation::growTrees(world::World& world, std::vector<BlockChange>& changes) {
    // The per-tick cap is the drain budget now; the rest waits for a later tick
    // instead of growing a whole forest in one frame.
    ticks_.drainDue(TickTask::TreeGrowth, tickCount_, kMaximumTreeGrowthsPerTick,
                    [&](SimulationPosition position) {
        const auto block = world.block(position.x, position.y, position.z);
        if (!world::gen::isSapling(block)) {
            return;
        }
        const SimulationPosition above{position.x, position.y + 1, position.z};
        // Re-checked against the sapling's own gate: a growth queued before dusk
        // and drained after it is dropped, and the sapling — still a sapling —
        // gets queued again by a later random tick once it is light enough.
        if (localBrightnessAt(world, above) < 9) {
            return;
        }
        ++lastTreeGrowthsProcessed_;
        growTreeAt(world, position, block, changes);
    });
}

void WorldSimulation::growTreeAt(
    world::World& world,
    SimulationPosition position,
    world::Block sapling,
    std::vector<BlockChange>& changes) {
    // The soil a sapling already survives on is what the trunk needs.
    if (!world::isSoil(world.block(position.x, position.y - 1, position.z))) {
        return;
    }
    // The shape placers only overwrite treeReplaceable cells, and a sapling is
    // not one, so the sapling's own cell has to be cleared before the trunk can
    // fill it. The clear is done in-world only: growTree writes nothing before
    // every one of its failure checks (height clamps and the dark-oak soil
    // probe all precede the first write), so if growth fails the sapling is put
    // back and the worker never sees it move — no drop, no break, exactly like
    // SaplingBlock#generate keeps the sapling when the tree cannot fit.
    // Deliberately not through setSimulatedBlock: this clear must stay
    // invisible outside the world, so a failed growth can put the sapling back
    // without the worker ever seeing it move (no flicker, no drop). The write
    // still goes through the service, so it is the same write everything else
    // makes — it just emits no BlockChange.
    RecordingMutationSink saplingSink;
    static_cast<void>(mutations_.setBlock(world, {position.x, position.y, position.z},
                                          world::BlockState{}, world::MutationFlags::KnownShape,
                                          world::MutationCause::RandomTick, saplingSink));

    const auto choice = world::gen::treeChoiceForSapling(sapling);
    // Deterministic per position: the same sapling always grows the same tree.
    // Vanilla uses the world's transient Random here; a fixed base seed only
    // picks which of the shapes the placers can roll, never whether one grows.
    world::gen::JavaRandom rng;
    rng.setPopulationSeed(0x9E3779B97F4A7C15ULL, position.x, position.z);

    class GrowthTreeWriter final : public world::gen::TreeWriter {
      public:
        GrowthTreeWriter(world::World& world, std::vector<BlockChange>& changes,
                         world::WorldMutationService& mutations)
            : world_(world), changes_(changes), mutations_(mutations) {}

        [[nodiscard]] world::Block block(int x, int y, int z) const override {
            return world_.block(x, y, z);
        }
        bool setState(int x, int y, int z, world::BlockState value) override {
            if (!world::isWorldYInRange(y)) {
                return false;
            }
            // A grown tree is a world edit like any other, so its trunk and
            // canopy go through the service too — that is what destroys the
            // block entity of anything the crown grows over.
            RecordingMutationSink sink;
            if (!mutations_.setBlock(world_, {x, y, z}, value,
                                     world::MutationFlags::KnownShape,
                                     world::MutationCause::RandomTick, sink)
                     .changed) {
                return false;
            }
            changes_.push_back(
                {SimulationPosition{x, y, z}, value, world::BlockState{}, false});
            return true;
        }

      private:
        world::World& world_;
        std::vector<BlockChange>& changes_;
        world::WorldMutationService& mutations_;
    };

    GrowthTreeWriter writer{world, changes, mutations_};
    const bool grew =
        world::gen::growTree(writer, rng, choice, position.x, position.y - 1, position.z);
    if (!grew) {
        static_cast<void>(mutations_.setBlock(
            world, {position.x, position.y, position.z}, world::BlockState{sapling},
            world::MutationFlags::KnownShape, world::MutationCause::RandomTick, saplingSink));
        return;
    }
    // Tree generation writes cells without neighbour notifications, so the
    // canopy is not yet queued for leaf decay. Notifying the trunk base floods
    // the surrounding leaves into the decay queue, exactly like a placed log.
    notifyNeighborChanged(world, position);
}

void WorldSimulation::notifyPlaced(SimulationPosition position, world::Block block) {
    if (world::isAffectedByGravity(block)) {
        queueSand(position);
    }
    if (world::isFluid(block)) {
        queueWater(position, 0);
    }
    queueNeighborSupportChecks(position);
}

void WorldSimulation::wakeWaterNeighbors(
    const world::World& world,
    SimulationPosition position) {
    constexpr std::array<SimulationPosition, 6> neighbors{{
        {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0},
    }};
    for (const auto offset : neighbors) {
        const auto neighbor = offsetPosition(position, offset);
        if (world::isFluid(world.block(neighbor.x, neighbor.y, neighbor.z))) {
            queueWater(
                neighbor,
                world.fluidLevel(neighbor.x, neighbor.y, neighbor.z));
        }
    }
}

void WorldSimulation::notifyNeighborChanged(
    const world::World& world,
    SimulationPosition position) {
    const SimulationPosition above{position.x, position.y + 1, position.z};
    if (world::isAffectedByGravity(world.block(above.x, above.y, above.z))) {
        queueSand(above);
    }
    wakeWaterNeighbors(world, position);
    queueNeighborSupportChecks(position);
    queueLeafDecayChecks(world, position);
}

void WorldSimulation::breakUnsupportedBlocks(
    world::World& world,
    std::vector<BlockChange>& changes) {
    // Java's neighbourChanged pass: an attached block that lost its support pops
    // off immediately, and popping it off can strand the next one along.
    constexpr std::size_t kMaximumSupportChecks = 512;
    ticks_.drainDue(TickTask::SupportCheck, tickCount_, kMaximumSupportChecks,
                    [&](SimulationPosition position) {
        const auto block = world.block(position.x, position.y, position.z);
        // A wall torch hangs off the wall behind its FACING, so the support
        // check needs the cell's state, not just its block.
        if (world::blockSupport(block) == world::BlockSupport::None ||
            world::canBlockSurvive(world, {position.x, position.y, position.z}, block,
                                   world.orientation(position.x, position.y, position.z))) {
            return;
        }
        // Crops do not drop themselves (blockDefinition.dropsItem is false), so
        // their loot has to be requested explicitly, like decayLeaves does for a
        // canopy. The whole state is captured before the cell is cleared so the
        // rolled table reflects the stage the crop had reached.
        const auto previousState = world.state(position.x, position.y, position.z);
        const std::size_t changeCount = changes.size();
        setSimulatedBlock(world, position, world::Block::Air, changes);
        if (changes.size() > changeCount && world::isCrop(block)) {
            changes.back().dropped = previousState;
        }
        wakeWaterNeighbors(world, position);
        queueNeighborSupportChecks(position);
    });
}

std::optional<std::uint8_t> WorldSimulation::updatedWaterLevel(
    const world::World& world,
    SimulationPosition position) const {
    std::size_t adjacentSources = 0U;
    std::optional<std::uint8_t> closestUpstream;
    for (const auto offset : kHorizontalWaterNeighbors) {
        const auto neighbor = offsetPosition(position, offset);
        if (!world::isFluid(world.block(neighbor.x, neighbor.y, neighbor.z))) {
            continue;
        }
        const std::uint8_t level =
            world.fluidLevel(neighbor.x, neighbor.y, neighbor.z);
        if (level == 0U) {
            ++adjacentSources;
        }
        const std::uint8_t effectiveLevel = level == kFallingWaterLevel ? 0U : level;
        closestUpstream = closestUpstream.has_value()
            ? std::min(*closestUpstream, effectiveLevel)
            : effectiveLevel;
    }

    const SimulationPosition below{position.x, position.y - 1, position.z};
    const auto belowBlock = world.block(below.x, below.y, below.z);
    const bool supportedBySource = world::isFluid(belowBlock) &&
        world.fluidLevel(below.x, below.y, below.z) == 0U;
    if (adjacentSources >= 2U &&
        (world::hasCollision(belowBlock) || supportedBySource)) {
        return 0U;
    }

    const SimulationPosition above{position.x, position.y + 1, position.z};
    if (world::isFluid(world.block(above.x, above.y, above.z))) {
        return kFallingWaterLevel;
    }
    if (!closestUpstream.has_value() || *closestUpstream >= kMaximumHorizontalWaterLevel) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(*closestUpstream + 1U);
}

bool WorldSimulation::waterCanReplace(
    const world::World& world,
    SimulationPosition position) const {
    const auto block = world.block(position.x, position.y, position.z);
    // FlowingFluid#canSpreadTo also destroys decoration blocks that do not
    // block motion, which is how water washes torches and flowers away.
    return !world::isFluid(block) &&
        (world::isReplaceable(block) || world::isDestroyedByFluid(block));
}

bool WorldSimulation::hasDownwardFlowPath(
    const world::World& world,
    SimulationPosition position) const {
    if (position.y <= 0) {
        return false;
    }
    const SimulationPosition below{position.x, position.y - 1, position.z};
    const auto block = world.block(below.x, below.y, below.z);
    return world::isFluid(block) || waterCanOccupy(block);
}

int WorldSimulation::distanceToDownwardFlow(
    const world::World& world,
    SimulationPosition origin,
    SimulationPosition previous,
    int depth) const {
    constexpr int kNoPath = 1000;
    constexpr int kFlowSearchDepth = 4;
    int best = kNoPath;
    for (const auto offset : kHorizontalWaterNeighbors) {
        const auto neighbor = offsetPosition(origin, offset);
        if (neighbor == previous) {
            continue;
        }
        const auto neighborBlock = world.block(neighbor.x, neighbor.y, neighbor.z);
        if ((!waterCanOccupy(neighborBlock) && !world::isFluid(neighborBlock)) ||
            (world::isFluid(neighborBlock) &&
             world.fluidLevel(neighbor.x, neighbor.y, neighbor.z) == 0U)) {
            continue;
        }
        if (hasDownwardFlowPath(world, neighbor)) {
            return depth;
        }
        if (depth < kFlowSearchDepth) {
            best = std::min(
                best,
                distanceToDownwardFlow(world, neighbor, origin, depth + 1));
        }
    }
    return best;
}

bool WorldSimulation::setSimulatedBlock(
    world::World& world,
    SimulationPosition position,
    world::Block block,
    std::vector<BlockChange>& changes,
    std::uint8_t fluidLevel,
    bool immediateRenderUpdate) {
    if (world.block(position.x, position.y, position.z) == block &&
        (!world::isFluid(block) ||
         world.fluidLevel(position.x, position.y, position.z) == fluidLevel)) {
        return false;
    }
    const auto previousState = world.state(position.x, position.y, position.z);
    const auto previous = previousState.block();
    // The write itself goes through the mutation service, so a simulated edit
    // decides "did this actually change?" by exactly the rule a player edit
    // does. The consequences still travel as BlockChange rather than through
    // the service's sink: the simulation hands its changes to the host, which
    // is what queues the mesh, the light and the drops for the whole tick's
    // batch. Neighbour reactions are the simulation's own queues, already
    // driven by the callers of this function, so the service is told not to
    // fire them a second time.
    RecordingMutationSink sink;
    const auto result = mutations_.setBlock(
        world, {position.x, position.y, position.z},
        world::BlockState{block, world::defaultOrientation(block), fluidLevel},
        world::MutationFlags::KnownShape, world::MutationCause::ScheduledTick, sink);
    if (result.changed) {
        // Decoration blocks that a fluid washes away, or that lost their
        // support, leave an item behind exactly like a mined block does. The
        // whole previous state drops, so a crop that pops rolls its loot from
        // the age it had grown to.
        const bool dropsPrevious = previous != block &&
            world::isDestroyedByFluid(previous) &&
            world::blockDefinition(previous).dropsItem;
        changes.push_back({position,
                           world::BlockState{block, world::defaultOrientation(block), fluidLevel},
                           dropsPrevious ? previousState : world::BlockState{},
                           immediateRenderUpdate});
    }
    return result.changed;
}

void WorldSimulation::notifyRedstoneComponent(const world::World& world,
                                              SimulationPosition position) {
    const auto block = world.block(position.x, position.y, position.z);
    const world::BlockPos pos{position.x, position.y, position.z};
    const auto state = world.state(position.x, position.y, position.z);
    // The dedup guard (UNIQUE_TICK_HASH / willTickThisTick): a component with a
    // toggle already pending must not queue a second one, so a flurry of input
    // updates before the tick fires still collapses to one flip.
    const bool alreadyScheduled = ticks_.contains(TickTask::RedstoneComponent, position);

    if (block == world::Block::RedstoneTorch || block == world::Block::RedstoneWallTorch) {
        const bool hasNeighborSignal = redstone::torchHasNeighborSignal(world, pos);
        if (redstone::torchShouldScheduleToggle(state, hasNeighborSignal, alreadyScheduled)) {
            static_cast<void>(ticks_.schedule(
                TickTask::RedstoneComponent, position,
                tickCount_ + static_cast<std::uint64_t>(redstone::kTorchToggleDelay), false,
                TickPriority::Normal));
        }
        return;
    }

    if (block == world::Block::Repeater) {
        const auto schedule = redstone::diodeCheckTick(
            state, redstone::repeaterShouldTurnOn(world, pos, state),
            redstone::repeaterIsLocked(world, pos, state), alreadyScheduled,
            redstone::diodeShouldPrioritize(world, pos, state),
            redstone::repeaterDelayGameticks(state));
        if (schedule.has_value()) {
            static_cast<void>(ticks_.schedule(
                TickTask::RedstoneComponent, position,
                tickCount_ + static_cast<std::uint64_t>(schedule->delayGameticks), false,
                schedule->priority));
        }
        return;
    }
}

void WorldSimulation::dispatchRedstoneTick(world::World& world, SimulationPosition position,
                                           std::vector<BlockChange>& changes) {
    const auto block = world.block(position.x, position.y, position.z);
    const world::BlockPos pos{position.x, position.y, position.z};
    const auto state = world.state(position.x, position.y, position.z);
    RedstoneReactionSink sink{world, *this};

    if (block == world::Block::RedstoneTorch || block == world::Block::RedstoneWallTorch) {
        // Age out toggles older than the burnout window before this tick counts,
        // exactly as RedstoneTorchBlock.tick does at its top.
        torchBurnout_.prune(static_cast<std::int64_t>(tickCount_));
        const bool hasNeighborSignal = redstone::torchHasNeighborSignal(world, pos);
        const auto result = redstone::torchTick(state, hasNeighborSignal, pos,
                                                static_cast<std::int64_t>(tickCount_), torchBurnout_);
        if (!result.changed) {
            return;
        }
        // Flags 3 (neighbours + clients): a torch's flip propagates, so the
        // reaction sink wakes any component it feeds.
        const auto applied = mutations_.setBlock(
            world, pos, result.newState,
            world::MutationFlags::NotifyNeighbors | world::MutationFlags::NotifyClients,
            world::MutationCause::ScheduledTick, sink);
        if (applied.changed) {
            changes.push_back({position, result.newState});
        }
        if (result.burnedOut) {
            // Re-check after RESTART_DELAY, when the window has aged out. The
            // smoke levelEvent(1502) is presentation and is intentionally omitted.
            static_cast<void>(ticks_.schedule(
                TickTask::RedstoneComponent, position,
                tickCount_ + static_cast<std::uint64_t>(redstone::kTorchRestartDelay), false,
                TickPriority::Normal));
        }
        return;
    }

    if (block == world::Block::Repeater) {
        const auto result = redstone::diodeTick(
            state, redstone::repeaterShouldTurnOn(world, pos, state),
            redstone::repeaterIsLocked(world, pos, state), redstone::repeaterDelayGameticks(state));
        if (!result.changed) {
            return;
        }
        // Flags 2 (clients only): a diode's POWERED flip does NOT fan out
        // neighbour updates — DiodeBlock.tick writes flag 2 — so it re-shapes and
        // re-renders but does not itself wake the block in front. The BlockChange
        // still carries the flip to the mesher.
        const auto applied =
            mutations_.setBlock(world, pos, result.newState, world::MutationFlags::NotifyClients,
                                world::MutationCause::ScheduledTick, sink);
        if (applied.changed) {
            changes.push_back({position, result.newState});
        }
        if (result.pulseReschedule.has_value()) {
            static_cast<void>(ticks_.schedule(
                TickTask::RedstoneComponent, position,
                tickCount_ + static_cast<std::uint64_t>(result.pulseReschedule->delayGameticks),
                false, result.pulseReschedule->priority));
        }
        return;
    }
}

std::vector<BlockChange> WorldSimulation::tick(
    world::World& world,
    bool processFluidUpdates) {
    ++tickCount_;
    lastWaterUpdatesProcessed_ = 0U;
    lastTreeGrowthsProcessed_ = 0U;
    randomTickConversionsThisTick_ = 0U;
    leafDecayChecksThisTick_ = 0U;
    cropStateWritesThisTick_ = 0U;
    std::vector<BlockChange> changes;
    breakUnsupportedBlocks(world, changes);
    decayLeaves(world, changes);
    randomTicks(world, changes);
    growTrees(world, changes);
    constexpr std::size_t kMaximumSandUpdates = 64;
    ticks_.drainDue(TickTask::FallingBlock, tickCount_, kMaximumSandUpdates,
                    [&](SimulationPosition position) {
        const auto fallingBlock = world.block(position.x, position.y, position.z);
        if (!world::isAffectedByGravity(fallingBlock) || position.y <= 0) {
            return;
        }
        const SimulationPosition below{position.x, position.y - 1, position.z};
        const auto belowBlock = world.block(below.x, below.y, below.z);
        if (!world::isReplaceable(belowBlock)) {
            return;
        }
        setSimulatedBlock(world, position, world::Block::Air, changes, 0U, true);
        // FallingBlockEntity removes the block before the entity starts moving.
        // Fluid ticks are event-driven, so water above/alongside that newly empty
        // cell must be woken just like it is after a player mines a block.
        wakeWaterNeighbors(world, position);
        // FallingBlock#getStateForNeighborUpdate: emptying a sand cell notifies
        // all six neighbours. The block above loses its support and drops next
        // tick, and any other unsupported gravity block the change touches is
        // re-checked — a floating patch collapses as a whole instead of leaving
        // a hanging shelf behind. The same neighbour pass re-checks attached
        // blocks, so a torch standing on the sand pops instead of floating.
        queueNeighborSupportChecks(position);
        for (const auto offset : kSixNeighbors) {
            const auto neighbor = offsetPosition(position, offset);
            if (world::isAffectedByGravity(
                    world.block(neighbor.x, neighbor.y, neighbor.z))) {
                queueSand(neighbor);
            }
        }
        fallingBlocks_.push_back({
            {static_cast<float>(position.x) + 0.5F,
             static_cast<float>(position.y) + 0.5F,
             static_cast<float>(position.z) + 0.5F},
            {static_cast<float>(position.x) + 0.5F,
             static_cast<float>(position.y) + 0.5F,
             static_cast<float>(position.z) + 0.5F},
            0.0F,
            fallingBlock,
            false,
        });
    });

    for (auto& entity : fallingBlocks_) {
        entity.previousPosition = entity.position;
        const int blockX = static_cast<int>(std::floor(entity.position.x));
        const int blockZ = static_cast<int>(std::floor(entity.position.z));
        const world::ChunkPosition owner{
            floorDiv(blockX, world::kChunkWidth),
            floorDiv(blockZ, world::kChunkDepth),
        };
        // World::block deliberately reads an unloaded chunk as air. Treating
        // that sentinel as physics would let an in-flight entity fall out of
        // the world and be discarded while its column is merely streamed out.
        if (!world.hasChunk(owner)) {
            continue;
        }

        entity.verticalVelocity = std::max(entity.verticalVelocity - 0.04F, -3.92F);
        const float nextY = entity.position.y + entity.verticalVelocity;
        const float previousBottom = entity.position.y - 0.5F;
        const float nextBottom = nextY - 0.5F;

        // Sweep the whole vertical segment travelled this tick. Sampling only
        // floor(nextBottom) tunnels through a one-block surface once gravity
        // accelerates the entity beyond one block per tick. Starting one tiny
        // epsilon below the old bottom also treats an exact contact with a block
        // top as a collision instead of sampling the air cell above it.
        constexpr float kContactEpsilon = 1.0e-4F;
        const int highestCandidate = std::min(
            world::kMaxY - 1,
            static_cast<int>(std::floor(previousBottom - kContactEpsilon)));
        const int lowestCandidate = std::max(world::kMinY, static_cast<int>(std::floor(nextBottom)));
        std::optional<int> collisionY;
        for (int candidateY = highestCandidate; candidateY >= lowestCandidate; --candidateY) {
            if (!world::hasCollision(world.block(blockX, candidateY, blockZ))) {
                continue;
            }
            const float collisionTop = static_cast<float>(candidateY + 1);
            if (previousBottom + kContactEpsilon >= collisionTop &&
                nextBottom <= collisionTop + kContactEpsilon) {
                collisionY = candidateY;
                break;
            }
        }

        if (collisionY.has_value()) {
            const SimulationPosition landing{blockX, *collisionY + 1, blockZ};
            const auto landingBlock = world.block(landing.x, landing.y, landing.z);
            if (world::isReplaceable(landingBlock) &&
                setSimulatedBlock(world, landing, entity.block, changes, 0U, true)) {
                wakeWaterNeighbors(world, landing);
                // The placed block notifies its neighbours (World#setBlockState
                // updateNeighbors), scheduling the sand that now sits above it.
                for (const auto offset : kSixNeighbors) {
                    const auto neighbor = offsetPosition(landing, offset);
                    if (world::isAffectedByGravity(
                            world.block(neighbor.x, neighbor.y, neighbor.z))) {
                        queueSand(neighbor);
                    }
                }
                entity.removed = true;
            } else if (!world::isReplaceable(landingBlock)) {
                // Vanilla turns a falling block into an item when its landing
                // state cannot be placed. This is an entity event, not a world
                // edit: the occupied landing cell remains untouched.
                changes.push_back({landing, world::BlockState{landingBlock},
                                   world::BlockState{entity.block}, false, false});
                entity.removed = true;
            } else {
                // A replaceable target that failed to mutate can only be a
                // transient storage failure. Keep the entity resting on the
                // collision plane and retry instead of deleting it.
                entity.position.y = static_cast<float>(*collisionY) + 1.5F;
                entity.previousPosition = entity.position;
                entity.verticalVelocity = 0.0F;
            }
            continue;
        }
        entity.position.y = nextY;
        if (entity.position.y < -8.0F) {
            entity.removed = true;
        }
    }
    std::erase_if(fallingBlocks_, [](const FallingBlockEntity& entity) {
        return entity.removed;
    });

    // Redstone components' scheduled ticks. One drain, so torch/repeater/
    // comparator run in the single (dueTick, priority, subTickOrder) order — the
    // snapshot the drain takes means a flip that schedules a follow-up (delay>=1)
    // lands in a later gametick, never this one, which is what keeps a circuit's
    // stages cleanly separated the way Java's two-phase LevelTicks does.
    constexpr std::size_t kMaximumRedstoneTicks = 1024;
    ticks_.drainDue(
        TickTask::RedstoneComponent, tickCount_, kMaximumRedstoneTicks,
        [&](SimulationPosition position) { dispatchRedstoneTick(world, position, changes); });

    if (!processFluidUpdates) {
        return changes;
    }
    lastWaterUpdatesProcessed_ += ticks_.drainDue(
        TickTask::Fluid, tickCount_, kMaximumWaterUpdatesPerPhase - lastWaterUpdatesProcessed_,
        [&](SimulationPosition position) {
        if (world.block(position.x, position.y, position.z) !=
            world::Block::Water) {
            return;
        }
        std::uint8_t currentLevel =
            world.fluidLevel(position.x, position.y, position.z);
        if (currentLevel != 0U) {
            const auto updated = updatedWaterLevel(world, position);
            if (!updated.has_value()) {
                setSimulatedBlock(world, position, world::Block::Air, changes);
                wakeWaterNeighbors(world, position);
                return;
            }
            if (*updated != currentLevel) {
                setSimulatedBlock(
                    world, position, world::Block::Water, changes, *updated);
                currentLevel = *updated;
                wakeWaterNeighbors(world, position);
            }
        }

        const SimulationPosition below{
            position.x, position.y - 1, position.z};
        bool flowedDown = false;
        if (below.y >= world::kMinY && waterCanReplace(world, below)) {
            setSimulatedBlock(
                world, below, world::Block::Water, changes, kFallingWaterLevel);
            queueWater(below, kFallingWaterLevel);
            wakeWaterNeighbors(world, below);
            flowedDown = true;
        }

        std::size_t adjacentSources = 0U;
        for (const auto offset : kHorizontalWaterNeighbors) {
            const auto neighbor = offsetPosition(position, offset);
            if (world::isFluid(world.block(neighbor.x, neighbor.y, neighbor.z)) &&
                world.fluidLevel(neighbor.x, neighbor.y, neighbor.z) == 0U) {
                ++adjacentSources;
            }
        }
        const bool connectedToWaterBelow =
            world::isFluid(world.block(below.x, below.y, below.z));
        if (currentLevel != 0U && (flowedDown || connectedToWaterBelow)) {
            return;
        }
        if (currentLevel == 0U && flowedDown && adjacentSources < 3U) {
            return;
        }
        const std::uint8_t horizontalLevel = currentLevel == kFallingWaterLevel
            ? 1U
            : static_cast<std::uint8_t>(currentLevel + 1U);
        if (horizontalLevel > kMaximumHorizontalWaterLevel) {
            return;
        }
        struct SpreadCandidate final {
            SimulationPosition position;
            std::uint8_t level;
            int distance;
        };
        std::array<SpreadCandidate, 4> candidates{};
        std::size_t candidateCount = 0U;
        int closestDrop = 1000;
        for (const auto offset : kHorizontalWaterNeighbors) {
            const auto neighbor = offsetPosition(position, offset);
            const auto neighborBlock = world.block(neighbor.x, neighbor.y, neighbor.z);
            if ((!waterCanOccupy(neighborBlock) &&
                 !world::isFluid(neighborBlock)) ||
                (world::isFluid(neighborBlock) &&
                 world.fluidLevel(neighbor.x, neighbor.y, neighbor.z) == 0U)) {
                continue;
            }
            const auto neighborLevel = updatedWaterLevel(world, neighbor);
            if (!neighborLevel.has_value()) {
                continue;
            }
            if (*neighborLevel == 0U) {
                if (world::isFluid(neighborBlock) &&
                    world.fluidLevel(neighbor.x, neighbor.y, neighbor.z) != 0U) {
                    queueWater(
                        neighbor,
                        world.fluidLevel(neighbor.x, neighbor.y, neighbor.z));
                    continue;
                }
            }
            if (*neighborLevel > horizontalLevel) {
                continue;
            }
            const int distance = hasDownwardFlowPath(world, neighbor)
                ? 0
                : distanceToDownwardFlow(world, neighbor, position, 1);
            closestDrop = std::min(closestDrop, distance);
            candidates[candidateCount++] = {neighbor, *neighborLevel, distance};
        }
        for (std::size_t candidateIndex = 0U;
             candidateIndex < candidateCount;
             ++candidateIndex) {
            const auto& candidate = candidates[candidateIndex];
            if (candidate.distance != closestDrop) {
                continue;
            }
            if (world::isFluid(world.block(
                    candidate.position.x,
                    candidate.position.y,
                    candidate.position.z)) &&
                world.fluidLevel(
                    candidate.position.x,
                    candidate.position.y,
                    candidate.position.z) == candidate.level) {
                continue;
            }
            setSimulatedBlock(
                world,
                candidate.position,
                world::Block::Water,
                changes,
                candidate.level);
            queueWater(candidate.position, candidate.level);
            wakeWaterNeighbors(world, candidate.position);
        }
    });
    return changes;
}

} // namespace mc::gameplay
