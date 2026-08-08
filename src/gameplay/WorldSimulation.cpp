#include "gameplay/WorldSimulation.hpp"

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

// How long a block waits for a random tick that fires with the given per-tick
// probability. Drawing the wait up front costs one sample per leaf instead of
// one coin flip per leaf per tick, and produces the same distribution.
[[nodiscard]] std::uint64_t randomTickDelay(std::uint32_t& state, double chancePerTick) {
    const double uniform =
        (static_cast<double>(nextRandom(state) >> 8U) + 0.5) / 16777216.0;
    const double delay = std::log(uniform) / std::log(1.0 - chancePerTick);
    return static_cast<std::uint64_t>(delay) + 1U;
}

} // namespace

bool isCollectableWaterSource(
    const world::World& world,
    SimulationPosition position) {
    return world.block(position.x, position.y, position.z) ==
               world::Block::Water &&
        world.fluidLevel(position.x, position.y, position.z) == 0U;
}

std::size_t SimulationPositionHash::operator()(const SimulationPosition& position) const noexcept {
    std::size_t seed = std::hash<int>{}(position.x);
    seed ^= std::hash<int>{}(position.y) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<int>{}(position.z) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    return seed;
}

void WorldSimulation::queueSand(SimulationPosition position) {
    if (position.y > 0 && position.y < world::kWorldHeight) {
        activeSand_.push_back(position);
    }
}

void WorldSimulation::queueWater(SimulationPosition position, std::uint8_t level) {
    static_cast<void>(level);
    if (position.y < 0 || position.y >= world::kWorldHeight ||
        !queuedWater_.insert(position).second) {
        return;
    }
    // WaterFluid#getTickRate is five game ticks in Java 1.16.1. Store the
    // deadline per position instead of releasing the whole queue on one
    // global modulo tick.
    activeWater_.push_back({position, tickCount_ + 5U});
}

void WorldSimulation::queueSupportCheck(SimulationPosition position) {
    if (position.y < 0 || position.y >= world::kWorldHeight ||
        !queuedSupportChecks_.insert(position).second) {
        return;
    }
    pendingSupportChecks_.push_back(position);
}

void WorldSimulation::queueNeighborSupportChecks(SimulationPosition position) {
    for (const auto offset : kSixNeighbors) {
        queueSupportCheck(offsetPosition(position, offset));
    }
}

void WorldSimulation::queueLeafDecay(SimulationPosition position) {
    if (position.y < 0 || position.y >= world::kWorldHeight ||
        !queuedLeafDecays_.insert(position).second || randomTickSpeed_ <= 0) {
        return;
    }
    // LeavesBlock decays on a random tick, so the wait scales with the
    // /gamerule randomTickSpeed exactly as vanilla's per-section draw count
    // does (a speed of 3 keeps the original 3/4096 pacing).
    const double chancePerTick =
        kLeafDecayChancePerTick * static_cast<double>(randomTickSpeed_) / 3.0;
    pendingLeafDecays_.push_back(
        {position, tickCount_ + randomTickDelay(leafRandomState_, chancePerTick)});
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
    for (std::size_t index = 0; index < pendingLeafDecays_.size();) {
        const auto scheduled = pendingLeafDecays_[index];
        if (scheduled.dueTick > tickCount_) {
            ++index;
            continue;
        }
        // A high randomTickSpeed shortens every leaf's decay delay, so a big
        // canopy can have hundreds of due checks in one tick — each a flood
        // fill. Stop once this tick's budget is spent; the rest stay queued and
        // are processed over the next ticks.
        if (leafDecayChecksThisTick_ >= kMaximumLeafDecayChecksPerTick) {
            break;
        }
        ++leafDecayChecksThisTick_;
        pendingLeafDecays_[index] = pendingLeafDecays_.back();
        pendingLeafDecays_.pop_back();
        queuedLeafDecays_.erase(scheduled.position);

        const auto position = scheduled.position;
        const auto block = world.block(position.x, position.y, position.z);
        if (!world::isLeaves(block) ||
            world::leavesArePersistent(world.orientation(position.x, position.y, position.z))) {
            continue;
        }
        const auto support = leafSupport(world, position);
        if (support == LeafSupport::Supported) {
            continue;
        }
        if (support == LeafSupport::Undecided) {
            // The search ran out of loaded world before it could find a trunk.
            // Vanilla does not tick unloaded chunks either, but it never forgets
            // the leaf: it is still there to be random-ticked once the chunk is
            // back. Dropping the entry here instead is what left canopies near
            // the streaming frontier hanging for good, so put it back with a
            // fresh draw and ask again later.
            queueLeafDecay(position);
            continue;
        }
        const std::size_t changeCount = changes.size();
        setSimulatedBlock(world, position, world::Block::Air, changes);
        if (changes.size() > changeCount) {
            // Decayed leaves roll their loot table exactly like a mined block,
            // which is what dropStacks does before removeBlock in vanilla.
            changes.back().dropped = block;
        }
        wakeWaterNeighbors(world, position);
        queueNeighborSupportChecks(position);
        for (const auto offset : kLeafFloodNeighbors) {
            const auto neighbor = offsetPosition(position, offset);
            if (world::isLeaves(world.block(neighbor.x, neighbor.y, neighbor.z))) {
                queueLeafDecay(neighbor);
            }
        }
    }
}

int WorldSimulation::lightAt(const world::World& world, SimulationPosition position) {
    // Java's getLightLevel(pos, 0): the brighter of the two channels. The
    // stored values are static full-sun sky light, which 1.16.1's growth and
    // spread checks read without any skylightSubtracted adjustment.
    return std::max(static_cast<int>(world.skyLight(position.x, position.y, position.z)),
                    static_cast<int>(world.blockLight(position.x, position.y, position.z)));
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
                const SimulationPosition position{
                    chunkPosition.x * world::kChunkWidth + localX,
                    sectionY * world::kSectionSize + localY,
                    chunkPosition.z * world::kChunkDepth + localZ,
                };
                randomTickBlock(world, position, section.block(localX, localY, localZ), changes);
            }
        }
    }
}

void WorldSimulation::randomTickBlock(
    world::World& world,
    SimulationPosition position,
    world::Block block,
    std::vector<BlockChange>& changes) {
    switch (block) {
    case world::Block::Grass:
        randomTickGrass(world, position, changes);
        break;
    case world::Block::OakSapling:
    case world::Block::SpruceSapling:
    case world::Block::BirchSapling:
    case world::Block::JungleSapling:
    case world::Block::AcaciaSapling:
    case world::Block::DarkOakSapling:
        randomTickSapling(world, position, changes);
        break;
    case world::Block::WheatCrops:
    case world::Block::Carrots:
    case world::Block::Potatoes:
        randomTickCrop(world, position, block, changes);
        break;
    case world::Block::Farmland:
        randomTickFarmland(world, position, changes);
        break;
    default:
        break;
    }
}

void WorldSimulation::randomTickGrass(
    world::World& world,
    SimulationPosition position,
    std::vector<BlockChange>& changes) {
    // SpreadableBlock#canSpread: a snow layer keeps the grass alive; otherwise
    // the block above must neither shield the cell (opacity > 2 — which water
    // does through the material check, so grass reverts to dirt under water)
    // nor leave it darker than 4.
    const SimulationPosition above{position.x, position.y + 1, position.z};
    const auto aboveBlock = world.block(above.x, above.y, above.z);
    if (world::opacity(aboveBlock) > 2 || lightAt(world, above) < 4) {
        reserveConversionAndApply(world, position, world::Block::Dirt, changes);
        return;
    }
    if (lightAt(world, above) < 9) {
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
        if (target.y < 0 || target.y >= world::kWorldHeight) {
            continue;
        }
        if (world.block(target.x, target.y, target.z) != world::Block::Dirt) {
            continue;
        }
        // The probe range reaches one below the surface, so without a light
        // check the spread would flip the dirt layer under the grass to grass,
        // which then dies for being covered — a constant churn of conversions
        // that drowns real spread and eats the per-tick budget. The target must
        // be able to hold grass itself: an uncovered, lit cell above it, the
        // same canSpread condition that keeps grass alive (vanilla's
        // SpreadableBlock#canSurvive checks the target's own light).
        const SimulationPosition targetAbove{target.x, target.y + 1, target.z};
        if (targetAbove.y >= world::kWorldHeight ||
            world::opacity(world.block(targetAbove.x, targetAbove.y, targetAbove.z)) > 2 ||
            lightAt(world, targetAbove) < 4) {
            continue;
        }
        if (!world::canBlockSurvive(world, {target.x, target.y, target.z},
                                    world::Block::Grass)) {
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
    // SaplingBlock#randomTick: needs light 9 above the sapling and rolls 1/7
    // before growing.
    const SimulationPosition above{position.x, position.y + 1, position.z};
    if (lightAt(world, above) < 9) {
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
    if (below.y < 0) {
        return moisture;
    }
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int sampleX = below.x + dx;
            const int sampleZ = below.z + dz;
            float contribution = 0.0F;
            if (world::isFarmland(world.block(sampleX, below.y, sampleZ))) {
                contribution = 1.0F;
                if (world::farmlandMoisture(
                        world.orientation(sampleX, below.y, sampleZ)) > 0) {
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
    if (above.y >= world::kWorldHeight) {
        return false;
    }
    return world::isCrop(world.block(above.x, above.y, above.z));
}

void WorldSimulation::randomTickCrop(
    world::World& world,
    SimulationPosition position,
    world::Block block,
    std::vector<BlockChange>& changes) {
    // CropsBlock#randomTick: the crop needs light 9 in the block above it, then
    // rolls a growth whose odds the surrounding farmland improves.
    const SimulationPosition above{position.x, position.y + 1, position.z};
    if (above.y >= world::kWorldHeight || lightAt(world, above) < 9) {
        return;
    }
    const int age = world::cropAge(world.orientation(position.x, position.y, position.z));
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
    const int newAge = age + 1;
    world.setOrientation(position.x, position.y, position.z, world::cropOrientation(newAge));
    changes.push_back({position, block, 0U, world::Block::Air,
                       world::cropOrientation(newAge), world::BlockOrientation::North});
}

void WorldSimulation::randomTickFarmland(
    world::World& world,
    SimulationPosition position,
    std::vector<BlockChange>& changes) {
    // FarmlandBlock#randomTick: water within four blocks hydrates the soil (the
    // moisture jumps straight to 7, vanilla never raises it gradually); without
    // water it dries one level, and at moisture 0 with nothing planted on it the
    // farmland reverts to plain dirt.
    const int moisture = world::farmlandMoisture(world.orientation(position.x, position.y, position.z));
    if (!farmlandNearWater(world, position)) {
        if (moisture > 0) {
            if (!reserveCropStateWrite()) {
                return;
            }
            const int newMoisture = moisture - 1;
            world.setOrientation(
                position.x, position.y, position.z, world::farmlandOrientation(newMoisture));
            changes.push_back({position, world::Block::Farmland, 0U, world::Block::Air,
                               world::farmlandOrientation(newMoisture),
                               world::BlockOrientation::North});
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
        world.setOrientation(position.x, position.y, position.z, world::farmlandOrientation(7));
        changes.push_back({position, world::Block::Farmland, 0U, world::Block::Air,
                           world::farmlandOrientation(7), world::BlockOrientation::North});
    }
}

void WorldSimulation::queueTreeGrowth(SimulationPosition position) {
    if (position.y < 0 || position.y >= world::kWorldHeight ||
        !queuedTreeGrowths_.insert(position).second) {
        return;
    }
    // Due next tick; growTrees drains the queue at the per-tick cap from there.
    pendingTreeGrowths_.push_back({position, tickCount_ + 1U});
}

void WorldSimulation::growTrees(world::World& world, std::vector<BlockChange>& changes) {
    for (std::size_t index = 0; index < pendingTreeGrowths_.size();) {
        const auto scheduled = pendingTreeGrowths_[index];
        if (scheduled.dueTick > tickCount_) {
            ++index;
            continue;
        }
        if (lastTreeGrowthsProcessed_ >= kMaximumTreeGrowthsPerTick) {
            // Leave the rest for the next tick instead of growing a whole
            // forest in one frame.
            ++index;
            continue;
        }
        pendingTreeGrowths_[index] = pendingTreeGrowths_.back();
        pendingTreeGrowths_.pop_back();
        queuedTreeGrowths_.erase(scheduled.position);

        const auto position = scheduled.position;
        const auto block = world.block(position.x, position.y, position.z);
        if (!world::gen::isSapling(block)) {
            continue;
        }
        const SimulationPosition above{position.x, position.y + 1, position.z};
        if (lightAt(world, above) < 9) {
            continue;
        }
        ++lastTreeGrowthsProcessed_;
        growTreeAt(world, position, block, changes);
    }
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
    world.setBlock(position.x, position.y, position.z, world::Block::Air);

    const auto choice = world::gen::treeChoiceForSapling(sapling);
    // Deterministic per position: the same sapling always grows the same tree.
    // Vanilla uses the world's transient Random here; a fixed base seed only
    // picks which of the shapes the placers can roll, never whether one grows.
    world::gen::JavaRandom rng;
    rng.setPopulationSeed(0x9E3779B97F4A7C15ULL, position.x, position.z);

    class GrowthTreeWriter final : public world::gen::TreeWriter {
      public:
        GrowthTreeWriter(world::World& world, std::vector<BlockChange>& changes)
            : world_(world), changes_(changes) {}

        [[nodiscard]] world::Block block(int x, int y, int z) const override {
            return world_.block(x, y, z);
        }
        bool setBlock(int x, int y, int z, world::Block value) override {
            if (y < 0 || y >= world::kWorldHeight || world_.block(x, y, z) == value) {
                return false;
            }
            if (!world_.setBlock(x, y, z, value)) {
                return false;
            }
            changes_.push_back({SimulationPosition{x, y, z}, value, 0U, world::Block::Air});
            return true;
        }
        bool setOrientation(int x, int y, int z, world::BlockOrientation value) override {
            return world_.setOrientation(x, y, z, value);
        }

      private:
        world::World& world_;
        std::vector<BlockChange>& changes_;
    };

    GrowthTreeWriter writer{world, changes};
    const bool grew =
        world::gen::growTree(writer, rng, choice, position.x, position.y - 1, position.z);
    if (!grew) {
        world.setBlock(position.x, position.y, position.z, sapling);
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
    for (std::size_t processed = 0U;
         processed < kMaximumSupportChecks && !pendingSupportChecks_.empty();
         ++processed) {
        const auto position = pendingSupportChecks_.front();
        pendingSupportChecks_.pop_front();
        queuedSupportChecks_.erase(position);
        const auto block = world.block(position.x, position.y, position.z);
        if (world::blockSupport(block) == world::BlockSupport::None ||
            world::canBlockSurvive(world, {position.x, position.y, position.z}, block)) {
            continue;
        }
        // Crops do not drop themselves (blockDefinition.dropsItem is false), so
        // their loot has to be requested explicitly, like decayLeaves does for a
        // canopy. The age is captured before the cell is cleared so the rolled
        // table reflects the stage the crop had reached.
        const auto previousOrientation =
            world.orientation(position.x, position.y, position.z);
        const std::size_t changeCount = changes.size();
        setSimulatedBlock(world, position, world::Block::Air, changes);
        if (changes.size() > changeCount && world::isCrop(block)) {
            changes.back().dropped = block;
            changes.back().droppedOrientation = previousOrientation;
        }
        wakeWaterNeighbors(world, position);
        queueNeighborSupportChecks(position);
    }
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

void WorldSimulation::setSimulatedBlock(
    world::World& world,
    SimulationPosition position,
    world::Block block,
    std::vector<BlockChange>& changes,
    std::uint8_t fluidLevel) {
    if (world.block(position.x, position.y, position.z) == block &&
        (!world::isFluid(block) ||
         world.fluidLevel(position.x, position.y, position.z) == fluidLevel)) {
        return;
    }
    const auto previous = world.block(position.x, position.y, position.z);
    const auto previousOrientation =
        world.orientation(position.x, position.y, position.z);
    if (world.setBlock(position.x, position.y, position.z, block)) {
        if (world::isFluid(block)) {
            world.setFluidLevel(position.x, position.y, position.z, fluidLevel);
        }
        // Decoration blocks that a fluid washes away, or that lost their
        // support, leave an item behind exactly like a mined block does. The
        // dropped block keeps the orientation it was broken at, so a crop that
        // pops rolls its loot from the age it had grown to.
        const bool dropsPrevious = previous != block &&
            world::isDestroyedByFluid(previous) &&
            world::blockDefinition(previous).dropsItem;
        changes.push_back({position, block, fluidLevel,
                           dropsPrevious ? previous : world::Block::Air,
                           std::nullopt,
                           dropsPrevious ? previousOrientation
                                         : world::BlockOrientation::North});
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
    const std::size_t sandUpdates = std::min(activeSand_.size(), kMaximumSandUpdates);
    for (std::size_t index = 0; index < sandUpdates; ++index) {
        const auto position = activeSand_.front();
        activeSand_.pop_front();
        const auto fallingBlock = world.block(position.x, position.y, position.z);
        if (!world::isAffectedByGravity(fallingBlock) || position.y <= 0) {
            continue;
        }
        const SimulationPosition below{position.x, position.y - 1, position.z};
        const auto belowBlock = world.block(below.x, below.y, below.z);
        if (!world::isReplaceable(belowBlock)) {
            continue;
        }
        setSimulatedBlock(world, position, world::Block::Air, changes);
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
    }

    for (auto& entity : fallingBlocks_) {
        entity.previousPosition = entity.position;
        entity.verticalVelocity = std::max(entity.verticalVelocity - 0.04F, -3.92F);
        const float nextY = entity.position.y + entity.verticalVelocity;
        const int blockX = static_cast<int>(std::floor(entity.position.x));
        const int blockZ = static_cast<int>(std::floor(entity.position.z));
        const int belowY = static_cast<int>(std::floor(nextY - 0.5F));
        if (belowY >= 0 &&
            world::hasCollision(world.block(blockX, belowY, blockZ)) &&
            nextY - 0.5F <= static_cast<float>(belowY + 1)) {
            const SimulationPosition landing{blockX, belowY + 1, blockZ};
            if (world::isReplaceable(world.block(landing.x, landing.y, landing.z))) {
                setSimulatedBlock(world, landing, entity.block, changes);
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
            }
            entity.removed = true;
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

    if (!processFluidUpdates) {
        return changes;
    }
    while (lastWaterUpdatesProcessed_ < kMaximumWaterUpdatesPerPhase &&
           !activeWater_.empty() &&
           activeWater_.front().dueTick <= tickCount_) {
        const auto position = activeWater_.front().position;
        activeWater_.pop_front();
        queuedWater_.erase(position);
        ++lastWaterUpdatesProcessed_;
        if (world.block(position.x, position.y, position.z) !=
            world::Block::Water) {
            continue;
        }
        std::uint8_t currentLevel =
            world.fluidLevel(position.x, position.y, position.z);
        if (currentLevel != 0U) {
            const auto updated = updatedWaterLevel(world, position);
            if (!updated.has_value()) {
                setSimulatedBlock(world, position, world::Block::Air, changes);
                wakeWaterNeighbors(world, position);
                continue;
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
        if (below.y >= 0 && waterCanReplace(world, below)) {
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
            continue;
        }
        if (currentLevel == 0U && flowedDown && adjacentSources < 3U) {
            continue;
        }
        const std::uint8_t horizontalLevel = currentLevel == kFallingWaterLevel
            ? 1U
            : static_cast<std::uint8_t>(currentLevel + 1U);
        if (horizontalLevel > kMaximumHorizontalWaterLevel) {
            continue;
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
    }
    return changes;
}

} // namespace mc::gameplay
