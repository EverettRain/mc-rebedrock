#include "world/WorldLightEngine.hpp"

#include "world/Block.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <iterator>

namespace mc::world {
namespace {

constexpr std::array<std::array<int, 3>, 6> kNeighbors{{
    {{1, 0, 0}}, {{-1, 0, 0}}, {{0, 1, 0}},
    {{0, -1, 0}}, {{0, 0, 1}}, {{0, 0, -1}},
}};

[[nodiscard]] int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if (value % divisor < 0) --quotient;
    return quotient;
}

[[nodiscard]] int floorMod(int value, int divisor) {
    const int result = value % divisor;
    return result < 0 ? result + divisor : result;
}

[[nodiscard]] std::size_t mix(std::size_t seed, int value) {
    seed ^= std::hash<int>{}(value) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    return seed;
}

} // namespace

std::size_t LightSectionPositionHash::operator()(
    const LightSectionPosition& position) const noexcept {
    std::size_t seed = std::hash<int>{}(position.chunkX);
    seed = mix(seed, position.sectionY);
    return mix(seed, position.chunkZ);
}

std::size_t WorldLightEngine::NodeHash::operator()(const Node& node) const noexcept {
    std::size_t seed = std::hash<int>{}(node.x);
    seed = mix(seed, node.y);
    return mix(seed, node.z);
}

namespace {

[[nodiscard]] std::uint64_t mixPacked(std::uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

} // namespace

std::size_t WorldLightEngine::QueuedNodeSet::findSlot(PackedNode node) const {
    const std::size_t mask = slots_.size() - 1U;
    std::size_t slot = static_cast<std::size_t>(mixPacked(node)) & mask;
    while (states_[slot] != 0U && !(states_[slot] == 1U && slots_[slot] == node)) {
        slot = (slot + 1U) & mask;
    }
    return slot;
}

void WorldLightEngine::QueuedNodeSet::rehash(std::size_t capacity) {
    capacity = std::max<std::size_t>(1024U, std::bit_ceil(capacity));
    std::vector<PackedNode> oldSlots = std::move(slots_);
    std::vector<std::uint8_t> oldStates = std::move(states_);
    slots_.assign(capacity, 0U);
    states_.assign(capacity, 0U);
    touchedSlots_.clear();
    touchedSlots_.reserve(capacity / 2U);
    size_ = 0U;
    tombstones_ = 0U;
    for (std::size_t index = 0; index < oldSlots.size(); ++index) {
        if (oldStates[index] == 1U) {
            static_cast<void>(insert(oldSlots[index]));
        }
    }
}

bool WorldLightEngine::QueuedNodeSet::insert(PackedNode node) {
    if (slots_.empty() || (size_ + tombstones_ + 1U) * 10U >= slots_.size() * 7U) {
        rehash(slots_.empty() ? 1024U : slots_.size() * 2U);
    }
    const std::size_t mask = slots_.size() - 1U;
    std::size_t slot = static_cast<std::size_t>(mixPacked(node)) & mask;
    std::size_t firstTombstone = slots_.size();
    while (states_[slot] != 0U) {
        if (states_[slot] == 1U && slots_[slot] == node) return false;
        if (states_[slot] == 2U && firstTombstone == slots_.size()) firstTombstone = slot;
        slot = (slot + 1U) & mask;
    }
    if (firstTombstone != slots_.size()) {
        slot = firstTombstone;
        --tombstones_;
    }
    if (states_[slot] == 0U) touchedSlots_.push_back(slot);
    slots_[slot] = node;
    states_[slot] = 1U;
    ++size_;
    return true;
}

void WorldLightEngine::QueuedNodeSet::erase(PackedNode node) {
    if (slots_.empty()) return;
    const std::size_t slot = findSlot(node);
    if (states_[slot] != 1U) return;
    states_[slot] = 2U;
    --size_;
    ++tombstones_;
}

void WorldLightEngine::QueuedNodeSet::clear() {
    for (const std::size_t slot : touchedSlots_) states_[slot] = 0U;
    touchedSlots_.clear();
    size_ = 0U;
    tombstones_ = 0U;
}

WorldLightEngine::PackedNode WorldLightEngine::packNode(const Node& node) {
    constexpr std::uint64_t coordinateMask = (1ULL << 27U) - 1ULL;
    return ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(node.x)) & coordinateMask)
            << 36U) |
           ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(node.z)) & coordinateMask)
            << 9U) |
           // World Y is offset by kMinY so the negative rows (−64..−1) pack into
           // the 9-bit field instead of sign-extending into the z coordinate.
           static_cast<std::uint64_t>((node.y - kMinY) & 0x1FF);
}

WorldLightEngine::Node WorldLightEngine::unpackNode(PackedNode node) {
    constexpr std::uint32_t coordinateMask = (1U << 27U) - 1U;
    const auto signExtend = [](std::uint32_t value) {
        constexpr std::uint32_t sign = 1U << 26U;
        return static_cast<int>((value ^ sign) - sign);
    };
    return {signExtend(static_cast<std::uint32_t>(node >> 36U) & coordinateMask),
            static_cast<int>(node & 0x1FFU) + kMinY,
            signExtend(static_cast<std::uint32_t>(node >> 9U) & coordinateMask)};
}

bool WorldLightEngine::cancelled() const {
    return cancellation_ != nullptr && cancellation_->load(std::memory_order_relaxed);
}

bool WorldLightEngine::loaded(const World& world, int x, int y, int z) {
    if (!isWorldYInRange(y)) return false;
    return world.hasChunk({floorDiv(x, kChunkWidth), floorDiv(z, kChunkDepth)});
}

std::uint8_t WorldLightEngine::level(const World& world, Channel channel,
                                     int x, int y, int z) {
    if (!loaded(world, x, y, z)) return 0U;
    return channel == Channel::Sky ? world.skyLight(x, y, z)
                                   : world.blockLight(x, y, z);
}

std::uint8_t WorldLightEngine::desiredLevel(const World& world, Channel channel,
                                            const Node& node) {
    const auto state = world.state(node.x, node.y, node.z);
    const Block value = state.block();
    const bool opaque = isOpaque(value);
    // Emission is a property of the state, not the block: a lit furnace is the
    // same block as a cold one and only the lit state glows.
    std::uint8_t desired = channel == Channel::Sky
                               ? (opaque ? 0U : world.directSkyLight(node.x, node.y, node.z))
                               : state.emittedLight();
    if (opaque) return desired;
    for (const auto& offset : kNeighbors) {
        const int neighborX = node.x + offset[0];
        const int neighborY = node.y + offset[1];
        const int neighborZ = node.z + offset[2];
        const std::uint8_t neighbor = level(world, channel, neighborX, neighborY, neighborZ);
        if (neighbor > 1U) {
            desired = std::max(desired, static_cast<std::uint8_t>(neighbor - 1U));
        }
    }
    return desired;
}

bool WorldLightEngine::setLevel(World& world, Channel channel, const Node& node,
                                std::uint8_t value) {
    return channel == Channel::Sky
               ? world.setSkyLight(node.x, node.y, node.z, value)
               : world.setBlockLight(node.x, node.y, node.z, value);
}

void WorldLightEngine::markDirty(const Node& node) {
    if (!isWorldYInRange(node.y)) return;
    std::array<int, 2> chunkXs{floorDiv(node.x, kChunkWidth), 0};
    std::array<int, 2> chunkZs{floorDiv(node.z, kChunkDepth), 0};
    std::array<int, 2> sectionYs{sectionIndexFromWorldY(node.y), 0};
    std::size_t xCount = 1U;
    std::size_t zCount = 1U;
    std::size_t yCount = 1U;
    const int localX = floorMod(node.x, kChunkWidth);
    const int localZ = floorMod(node.z, kChunkDepth);
    const int localY = yInSectionFromWorldY(node.y);
    if (localX == 0) chunkXs[xCount++] = chunkXs[0] - 1;
    else if (localX == kChunkWidth - 1) chunkXs[xCount++] = chunkXs[0] + 1;
    if (localZ == 0) chunkZs[zCount++] = chunkZs[0] - 1;
    else if (localZ == kChunkDepth - 1) chunkZs[zCount++] = chunkZs[0] + 1;
    if (localY == 0 && sectionYs[0] > 0) sectionYs[yCount++] = sectionYs[0] - 1;
    else if (localY == kSectionSize - 1 && sectionYs[0] + 1 < kSectionCount)
        sectionYs[yCount++] = sectionYs[0] + 1;
    for (std::size_t xi = 0U; xi < xCount; ++xi) {
        for (std::size_t zi = 0U; zi < zCount; ++zi) {
            for (std::size_t yi = 0U; yi < yCount; ++yi) {
                dirtySections_.insert({chunkXs[xi], sectionYs[yi], chunkZs[zi]});
            }
        }
    }
}

void WorldLightEngine::recomputeSkyColumn(World& world, int x, int z,
                                          std::vector<Node>& changedSources) {
    std::uint8_t direct = 15U;
    for (int y = kMaxY - 1; y >= kMinY; --y) {
        const BlockState state = world.state(x, y, z);
        // State-aware opacity: a submerged slab dims the column like water (F2).
        const std::uint8_t opacity = skyLightOpacity(state);
        direct = opacity >= direct ? 0U : static_cast<std::uint8_t>(direct - opacity);
        const std::uint8_t stored = isOpaque(state.block()) ? 0U : direct;
        if (world.setDirectSkyLight(x, y, z, stored)) {
            changedSources.push_back({x, y, z});
        }
    }
}

void WorldLightEngine::propagateIncreases(World& world, Channel channel,
                                          std::vector<Node>& queue) {
    std::size_t cursor = 0U;
    while (cursor < queue.size() && !cancelled()) {
        const Node source = queue[cursor++];
        ++lastPropagationVisitCount_;
        const std::uint8_t sourceLevel = level(world, channel, source.x, source.y, source.z);
        if (sourceLevel <= 1U) continue;
        const std::uint8_t propagated = static_cast<std::uint8_t>(sourceLevel - 1U);
        for (const auto& offset : kNeighbors) {
            const Node target{source.x + offset[0], source.y + offset[1],
                              source.z + offset[2]};
            if (!loaded(world, target.x, target.y, target.z) ||
                isOpaque(world.block(target.x, target.y, target.z)) ||
                level(world, channel, target.x, target.y, target.z) >= propagated) {
                continue;
            }
            setLevel(world, channel, target, propagated);
            queue.push_back(target);
        }
    }
}

void WorldLightEngine::initializeChunks(World& world,
                                        std::span<const ChunkPosition> positions) {
    if (positions.empty()) return;
    lastPropagationVisitCount_ = 0U;
    // The per-column scan is chunk-local: each worker writes only the light
    // arrays of the chunks it claims, and reads neighbours through the world.
    // The world holds no insert/erase while this runs, so the concurrent map
    // lookups are safe. The queues each worker fills are merged afterwards and
    // the propagation phase stays serial, so the result is identical to a
    // single-threaded pass.
    const std::size_t workerCount = workerPool_ != nullptr
                                        ? workerPool_->workerCount()
                                        : std::size_t{1U};
    std::vector<std::vector<Node>> skyQueues(workerCount);
    std::vector<std::vector<Node>> blockQueues(workerCount);
    const auto scanPosition = [&](std::size_t index, std::size_t workerIndex) {
        auto& skyQueue = skyQueues[workerIndex];
        auto& blockQueue = blockQueues[workerIndex];
        if (!cancelled()) {
            const ChunkPosition position = positions[index];
            Chunk* chunk = world.chunk(position);
            if (chunk == nullptr) return;
            const int originX = position.x * kChunkWidth;
            const int originZ = position.z * kChunkDepth;
            // The topmost contiguous run of empty (all-air) sections is provably
            // full-15 open sky: nothing above them attenuates. Fill their sky
            // arrays uniformly (zero allocation) instead of writing 15 into every
            // cell — the taller 26.1 world stacks several such empty sky sections
            // above the terrain, and per-cell writes would allocate 2 KB per array.
            // The column scan then starts at the top of the highest non-empty
            // section (everything above is the just-filled uniform 15, entered with
            // direct == 15), so it neither reallocates nor re-scans the open sky.
            int scanTopY = kMinY - 1; // whole-air chunk: nothing left to scan
            for (int sectionY = kSectionCount - 1; sectionY >= 0; --sectionY) {
                if (!chunk->section(sectionY).empty()) {
                    scanTopY = sectionOriginY(sectionY) + kSectionSize - 1;
                    break;
                }
                chunk->section(sectionY).fillSkyLight(15U);
                chunk->section(sectionY).fillDirectSkyLight(15U);
            }
            for (int localZ = 0; localZ < kChunkDepth; ++localZ) {
                for (int localX = 0; localX < kChunkWidth; ++localX) {
                    std::uint8_t direct = 15U;
                    for (int y = scanTopY; y >= kMinY; --y) {
                        const BlockState value = chunk->state(localX, y, localZ);
                        // State-aware: a submerged slab dims like water (F2).
                        const std::uint8_t opacity = skyLightOpacity(value);
                        const std::uint8_t previousDirect = direct;
                        direct = opacity >= direct ? 0U
                                                   : static_cast<std::uint8_t>(direct - opacity);
                        const std::uint8_t sky = isOpaque(value.block()) ? 0U : direct;
                        chunk->setDirectSkyLight(localX, y, localZ, sky);
                        chunk->setSkyLight(localX, y, localZ, sky);
                        const std::uint8_t emitted =
                            chunk->section(sectionIndexFromWorldY(y))
                                .state(localX, yInSectionFromWorldY(y), localZ)
                                .emittedLight();
                        chunk->setBlockLight(localX, y, localZ, emitted);
                        // Do not enqueue the enormous uniform open-sky volume.
                        // Only light boundaries can improve another cell: the
                        // cell above an attenuation step, and horizontal sources
                        // around partially lit transparent cells.
                        if (previousDirect > direct && y + 1 < kMaxY) {
                            skyQueue.push_back({originX + localX, y + 1, originZ + localZ});
                        }
                        if (!isOpaque(value.block()) && sky < 15U) {
                            skyQueue.push_back({originX + localX - 1, y, originZ + localZ});
                            skyQueue.push_back({originX + localX + 1, y, originZ + localZ});
                            skyQueue.push_back({originX + localX, y, originZ + localZ - 1});
                            skyQueue.push_back({originX + localX, y, originZ + localZ + 1});
                        }
                        if (emitted > 1U)
                            blockQueue.push_back({originX + localX, y, originZ + localZ});
                    }
                }
            }
        }
    };
    if (workerPool_ != nullptr && positions.size() > 1U) {
        workerPool_->run(positions.size(), scanPosition);
    } else {
        for (std::size_t index = 0U; index < positions.size(); ++index)
            scanPosition(index, 0U);
    }
    std::vector<Node> skyQueue;
    std::vector<Node> blockQueue;
    for (auto& queue : skyQueues) {
        skyQueue.insert(skyQueue.end(), std::make_move_iterator(queue.begin()),
                        std::make_move_iterator(queue.end()));
    }
    for (auto& queue : blockQueues) {
        blockQueue.insert(blockQueue.end(), std::make_move_iterator(queue.begin()),
                          std::make_move_iterator(queue.end()));
    }
    // Existing light at the four shared borders is also a source for newly
    // loaded chunks. A one-cell ring is enough because propagation fans out.
    for (const ChunkPosition position : positions) {
        const int originX = position.x * kChunkWidth;
        const int originZ = position.z * kChunkDepth;
        for (int y = kMinY; y < kMaxY; ++y) {
            for (int offset = 0; offset < kChunkWidth; ++offset) {
                const std::array<Node, 4> ring{{
                    {originX - 1, y, originZ + offset},
                    {originX + kChunkWidth, y, originZ + offset},
                    {originX + offset, y, originZ - 1},
                    {originX + offset, y, originZ + kChunkDepth},
                }};
                for (const Node node : ring) {
                    if (!loaded(world, node.x, node.y, node.z)) continue;
                    if (world.blockLight(node.x, node.y, node.z) > 1U) blockQueue.push_back(node);
                }
            }
        }
    }
    propagateIncreases(world, Channel::Sky, skyQueue);
    propagateIncreases(world, Channel::Block, blockQueue);
}

void WorldLightEngine::settle(World& world, Channel channel,
                              std::span<const Node> seeds) {
    settleQueue_.clear();
    queuedNodes_.clear();
    const auto enqueue = [this, &world](const Node& node) {
        const PackedNode packed = packNode(node);
        if (loaded(world, node.x, node.y, node.z) && queuedNodes_.insert(packed))
            settleQueue_.push_back(packed);
    };
    for (const Node seed : seeds) enqueue(seed);
    std::size_t cursor = 0U;
    while (cursor < settleQueue_.size() && !cancelled()) {
        const PackedNode packed = settleQueue_[cursor++];
        const Node node = unpackNode(packed);
        queuedNodes_.erase(packed);
        ++lastPropagationVisitCount_;
        const std::uint8_t previous = level(world, channel, node.x, node.y, node.z);
        const std::uint8_t desired = desiredLevel(world, channel, node);
        if (previous == desired) continue;
        setLevel(world, channel, node, desired);
        markDirty(node);
        for (const auto& offset : kNeighbors) {
            enqueue({node.x + offset[0], node.y + offset[1], node.z + offset[2]});
        }
    }
}

void WorldLightEngine::updateBlock(World& world, int worldX, int y, int worldZ) {
    if (!loaded(world, worldX, y, worldZ)) return;
    lastPropagationVisitCount_ = 0U;
    skySeeds_.clear();
    blockSeeds_.clear();
    recomputeSkyColumn(world, worldX, worldZ, skySeeds_);
    const Node edited{worldX, y, worldZ};
    skySeeds_.push_back(edited);
    blockSeeds_.push_back(edited);
    for (const auto& offset : kNeighbors) {
        const Node neighbor{worldX + offset[0], y + offset[1], worldZ + offset[2]};
        skySeeds_.push_back(neighbor);
        blockSeeds_.push_back(neighbor);
    }
    markDirty(edited);
    settle(world, Channel::Sky, skySeeds_);
    settle(world, Channel::Block, blockSeeds_);
}

void WorldLightEngine::updateAfterChunkRemoval(World& world, ChunkPosition removed) {
    lastPropagationVisitCount_ = 0U;
    skySeeds_.clear();
    const int originX = removed.x * kChunkWidth;
    const int originZ = removed.z * kChunkDepth;
    skySeeds_.reserve(static_cast<std::size_t>(kWorldHeight * kChunkWidth * 4));
    for (int y = kMinY; y < kMaxY; ++y) {
        for (int offset = 0; offset < kChunkWidth; ++offset) {
            skySeeds_.push_back({originX - 1, y, originZ + offset});
            skySeeds_.push_back({originX + kChunkWidth, y, originZ + offset});
            skySeeds_.push_back({originX + offset, y, originZ - 1});
            skySeeds_.push_back({originX + offset, y, originZ + kChunkDepth});
        }
    }
    settle(world, Channel::Sky, skySeeds_);
    settle(world, Channel::Block, skySeeds_);
}

std::vector<LightSectionPosition> WorldLightEngine::takeDirtySections() {
    std::vector<LightSectionPosition> result{dirtySections_.begin(), dirtySections_.end()};
    dirtySections_.clear();
    std::ranges::sort(result, {}, [](const LightSectionPosition& position) {
        return std::array{position.chunkZ, position.chunkX, position.sectionY};
    });
    return result;
}

} // namespace mc::world
