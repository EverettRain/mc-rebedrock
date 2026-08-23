#include "world/WorldLighting.hpp"

#include "world/WorldConstants.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <stdexcept>

namespace mc::world {
namespace {

constexpr int kPropagationRadius = ChunkLightSampler::kMaximumLightLevel;
constexpr int kVertexSamplePadding = 1;
constexpr std::array<std::array<int, 3>, 6> kNeighbors{{
    {{1, 0, 0}},
    {{-1, 0, 0}},
    {{0, 1, 0}},
    {{0, -1, 0}},
    {{0, 0, 1}},
    {{0, 0, -1}},
}};

} // namespace

ChunkLightSampler::ChunkLightSampler(const World& world) : world_(world), stored_(true) {}

ChunkLightSampler::ChunkLightSampler(const World& world, ChunkPosition chunkPosition,
                                     const std::atomic<bool>* cancellation)
    : ChunkLightSampler(world, std::span<const ChunkPosition>{&chunkPosition, 1U}, cancellation) {}

ChunkLightSampler::ChunkLightSampler(const World& world,
                                     std::span<const ChunkPosition> chunkPositions,
                                     const std::atomic<bool>* cancellation)
    : world_(world), cancellation_(cancellation) {
    if (chunkPositions.empty()) {
        throw std::invalid_argument("Chunk light sampler requires at least one chunk");
    }
    int minimumChunkX = chunkPositions.front().x;
    int maximumChunkX = minimumChunkX;
    int minimumChunkZ = chunkPositions.front().z;
    int maximumChunkZ = minimumChunkZ;
    for (const auto position : chunkPositions.subspan(1U)) {
        minimumChunkX = std::min(minimumChunkX, position.x);
        maximumChunkX = std::max(maximumChunkX, position.x);
        minimumChunkZ = std::min(minimumChunkZ, position.z);
        maximumChunkZ = std::max(maximumChunkZ, position.z);
    }
    minimumX_ = minimumChunkX * kChunkWidth - kVertexSamplePadding - kPropagationRadius;
    minimumY_ = kMinY;
    minimumZ_ = minimumChunkZ * kChunkDepth - kVertexSamplePadding - kPropagationRadius;
    const int maximumX = (maximumChunkX + 1) * kChunkWidth + kPropagationRadius;
    const int maximumY = kMaxY - 1;
    const int maximumZ = (maximumChunkZ + 1) * kChunkDepth + kPropagationRadius;
    width_ = maximumX - minimumX_ + 1;
    height_ = maximumY - minimumY_ + 1;
    depth_ = maximumZ - minimumZ_ + 1;
    const std::size_t cellCount = static_cast<std::size_t>(width_) *
                                  static_cast<std::size_t>(height_) *
                                  static_cast<std::size_t>(depth_);
    skyLevels_.assign(cellCount, 0U);
    blockLevels_.assign(cellCount, 0U);
    opaque_.assign(cellCount, 0U);

    for (int z = minimumZ_; z <= maximumZ; ++z) {
        if (cancelled())
            return;
        for (int x = minimumX_; x <= maximumX; ++x) {
            std::uint8_t directSky = kMaximumLightLevel;
            for (int y = maximumY; y >= minimumY_; --y) {
                const std::size_t cell = index(x, y, z);
                const auto state = world_.state(x, y, z);
                const Block value = state.block();
                opaque_[cell] = mc::world::isOpaque(value) ? 1U : 0U;
                // State-aware: a submerged slab dims like water even though its
                // own identity (a dry slab) does not (F2).
                const std::uint8_t opacity = skyLightOpacity(state);
                if (opacity >= directSky) {
                    directSky = 0U;
                } else {
                    directSky = static_cast<std::uint8_t>(directSky - opacity);
                }
                skyLevels_[cell] = opaque_[cell] == 0U ? directSky : 0U;
                blockLevels_[cell] = state.emittedLight();
            }
        }
    }
    propagate(skyLevels_);
    propagate(blockLevels_);
}

std::size_t ChunkLightSampler::index(int x, int y, int z) const {
    return (static_cast<std::size_t>(y - minimumY_) * static_cast<std::size_t>(depth_) +
            static_cast<std::size_t>(z - minimumZ_)) *
               static_cast<std::size_t>(width_) +
           static_cast<std::size_t>(x - minimumX_);
}

bool ChunkLightSampler::contains(int x, int y, int z) const {
    return x >= minimumX_ && x < minimumX_ + width_ && y >= minimumY_ && y < minimumY_ + height_ &&
           z >= minimumZ_ && z < minimumZ_ + depth_;
}

void ChunkLightSampler::propagate(std::vector<std::uint8_t>& levels) const {
    std::deque<std::array<int, 3>> queue;
    for (int y = minimumY_; y < minimumY_ + height_; ++y) {
        if (cancelled())
            return;
        for (int z = minimumZ_; z < minimumZ_ + depth_; ++z) {
            for (int x = minimumX_; x < minimumX_ + width_; ++x) {
                const std::uint8_t source = levels[index(x, y, z)];
                if (source <= 1U)
                    continue;
                bool canSpread = false;
                for (const auto& offset : kNeighbors) {
                    const int neighborX = x + offset[0];
                    const int neighborY = y + offset[1];
                    const int neighborZ = z + offset[2];
                    if (!contains(neighborX, neighborY, neighborZ))
                        continue;
                    const std::size_t neighbor = index(neighborX, neighborY, neighborZ);
                    if (opaque_[neighbor] == 0U && levels[neighbor] + 1U < source) {
                        canSpread = true;
                        break;
                    }
                }
                if (canSpread)
                    queue.push_back({x, y, z});
            }
        }
    }
    while (!queue.empty()) {
        if (cancelled())
            return;
        const auto position = queue.front();
        queue.pop_front();
        const std::uint8_t source = levels[index(position[0], position[1], position[2])];
        if (source <= 1U)
            continue;
        const std::uint8_t propagated = static_cast<std::uint8_t>(source - 1U);
        for (const auto& offset : kNeighbors) {
            const int x = position[0] + offset[0];
            const int y = position[1] + offset[1];
            const int z = position[2] + offset[2];
            if (!contains(x, y, z))
                continue;
            const std::size_t target = index(x, y, z);
            if (opaque_[target] != 0U || levels[target] >= propagated)
                continue;
            levels[target] = propagated;
            queue.push_back({x, y, z});
        }
    }
}

bool ChunkLightSampler::cancelled() const {
    return cancellation_ != nullptr && cancellation_->load(std::memory_order_relaxed);
}

VoxelLightLevel ChunkLightSampler::level(int x, int y, int z) const {
    if (stored_) return {world_.skyLight(x, y, z), world_.blockLight(x, y, z)};
    if (!contains(x, y, z)) {
        if (y < kMinY)
            return {};
        if (y >= kMaxY)
            return {kMaximumLightLevel, 0U};
        return {
            static_cast<std::uint8_t>(!mc::world::isOpaque(world_.block(x, y, z)) ? kMaximumLightLevel : 0),
            world_.state(x, y, z).emittedLight(),
        };
    }
    const std::size_t cell = index(x, y, z);
    return {skyLevels_[cell], blockLevels_[cell]};
}

float ChunkLightSampler::sky(int x, int y, int z) const {
    return static_cast<float>(level(x, y, z).sky) / static_cast<float>(kMaximumLightLevel);
}

float ChunkLightSampler::block(int x, int y, int z) const {
    return static_cast<float>(level(x, y, z).block) / static_cast<float>(kMaximumLightLevel);
}

bool ChunkLightSampler::isOpaque(int x, int y, int z) const {
    if (contains(x, y, z)) return opaque_[index(x, y, z)] != 0U;
    if (!isWorldYInRange(y)) return false;
    return mc::world::isOpaque(world_.block(x, y, z));
}

bool ChunkLightSampler::aoOccludes(int x, int y, int z) const {
    if (!isWorldYInRange(y)) return false;
    return mc::world::aoOccludes(world_.block(x, y, z));
}

int ChunkLightSampler::opacity(int x, int y, int z) const {
    if (!isWorldYInRange(y)) return 0;
    return mc::world::opacity(world_.block(x, y, z));
}

Block ChunkLightSampler::blockType(int x, int y, int z) const {
    if (!isWorldYInRange(y)) return Block::Air;
    return world_.block(x, y, z);
}

} // namespace mc::world
