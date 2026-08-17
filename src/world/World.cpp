#include "world/World.hpp"

#include "world/WorldConstants.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace mc::world {
namespace {

[[nodiscard]] int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    const int remainder = value % divisor;
    if (remainder < 0) {
        --quotient;
    }
    return quotient;
}

} // namespace

std::size_t ChunkPositionHash::operator()(const ChunkPosition& position) const noexcept {
    const auto x = static_cast<std::uint32_t>(position.x);
    const auto z = static_cast<std::uint32_t>(position.z);
    const std::uint64_t packed = (static_cast<std::uint64_t>(x) << 32U) | z;
    return std::hash<std::uint64_t>{}(packed);
}

void World::setChunk(ChunkPosition position, Chunk chunk) {
    chunks_.insert_or_assign(position, std::make_shared<Chunk>(std::move(chunk)));
}

void World::setChunk(ChunkPosition position, std::shared_ptr<const Chunk> chunk) {
    if (chunk == nullptr) {
        chunks_.erase(position);
        return;
    }
    // sharedChunk() only exposes const access. Internally World keeps a mutable
    // handle and chunk() clones before returning a writable pointer whenever
    // another owner exists.
    chunks_.insert_or_assign(position, std::const_pointer_cast<Chunk>(std::move(chunk)));
}

bool World::removeChunk(ChunkPosition position) {
    return chunks_.erase(position) != 0U;
}

bool World::hasChunk(ChunkPosition position) const {
    return chunks_.contains(position);
}

const Chunk* World::chunk(ChunkPosition position) const {
    const auto found = chunks_.find(position);
    return found == chunks_.end() ? nullptr : found->second.get();
}

Chunk* World::chunk(ChunkPosition position) {
    const auto found = chunks_.find(position);
    if (found == chunks_.end()) return nullptr;
    if (found->second.use_count() != 1) {
        found->second = std::make_shared<Chunk>(*found->second);
    }
    return found->second.get();
}

std::shared_ptr<const Chunk> World::sharedChunk(ChunkPosition position) const {
    const auto found = chunks_.find(position);
    return found == chunks_.end() ? nullptr : found->second;
}

Block World::block(int worldX, int y, int worldZ) const {
    if (!isWorldYInRange(y)) {
        return Block::Air;
    }
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    const Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) {
        return Block::Air;
    }
    const int localX = worldX - chunkX * kChunkWidth;
    const int localZ = worldZ - chunkZ * kChunkDepth;
    return owner->block(localX, y, localZ);
}

gen::Biome World::biomeAt(int worldX, int worldZ) const {
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    const Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) {
        return gen::Biome::Plains;
    }
    return owner->columnBiome(worldX - chunkX * kChunkWidth, worldZ - chunkZ * kChunkDepth);
}

bool World::setBlock(int worldX, int y, int worldZ, Block value) {
    if (!isWorldYInRange(y)) {
        return false;
    }
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) {
        return false;
    }
    owner->setBlock(
        worldX - chunkX * kChunkWidth,
        y,
        worldZ - chunkZ * kChunkDepth,
        value);
    return true;
}

BlockState World::state(int worldX, int y, int worldZ) const {
    // One read of the interned state, not three per-axis decodes recomposed:
    // block()/orientation()/fluidLevel() each drop the axes they do not carry,
    // and none of them carries LIT, so composing them here silently unlit every
    // furnace. Out-of-world and unloaded columns read as air, as block() does.
    if (!isWorldYInRange(y)) {
        return BlockState{};
    }
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    const Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) {
        return BlockState{};
    }
    return owner->state(worldX - chunkX * kChunkWidth, y, worldZ - chunkZ * kChunkDepth);
}

bool World::setState(int worldX, int y, int worldZ, BlockState value) {
    // The whole state in one write. Going through setBlock() first would reset
    // the cell to the block's default state and lose LIT, which setOrientation/
    // setFluidLevel cannot write back — a lit furnace would land unlit.
    if (!isWorldYInRange(y)) {
        return false;
    }
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) {
        return false;
    }
    owner->setState(worldX - chunkX * kChunkWidth, y, worldZ - chunkZ * kChunkDepth, value);
    return true;
}

BlockOrientation World::orientation(int worldX, int y, int worldZ) const {
    if (!isWorldYInRange(y)) return BlockOrientation::North;
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    const Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) return BlockOrientation::North;
    return owner->orientation(worldX - chunkX * kChunkWidth, y,
                              worldZ - chunkZ * kChunkDepth);
}

bool World::setOrientation(int worldX, int y, int worldZ, BlockOrientation value) {
    if (!isWorldYInRange(y)) return false;
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) return false;
    owner->setOrientation(worldX - chunkX * kChunkWidth, y,
                          worldZ - chunkZ * kChunkDepth, value);
    return true;
}

std::uint8_t World::fluidLevel(int worldX, int y, int worldZ) const {
    if (!isWorldYInRange(y)) {
        return 0U;
    }
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    const Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) {
        return 0U;
    }
    return owner->fluidLevel(
        worldX - chunkX * kChunkWidth, y, worldZ - chunkZ * kChunkDepth);
}

bool World::setFluidLevel(int worldX, int y, int worldZ, std::uint8_t value) {
    if (!isWorldYInRange(y)) {
        return false;
    }
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) {
        return false;
    }
    owner->setFluidLevel(
        worldX - chunkX * kChunkWidth, y, worldZ - chunkZ * kChunkDepth, value);
    return true;
}

std::uint8_t World::skyLight(int worldX, int y, int worldZ) const {
    if (y >= kMaxY) return 15U;
    if (y < kMinY) return 0U;
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    const Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) return 15U;
    return owner->skyLight(worldX - chunkX * kChunkWidth, y,
                           worldZ - chunkZ * kChunkDepth);
}

std::uint8_t World::blockLight(int worldX, int y, int worldZ) const {
    if (!isWorldYInRange(y)) return 0U;
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    const Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) return 0U;
    return owner->blockLight(worldX - chunkX * kChunkWidth, y,
                             worldZ - chunkZ * kChunkDepth);
}

std::uint8_t World::directSkyLight(int worldX, int y, int worldZ) const {
    if (y >= kMaxY) return 15U;
    if (y < kMinY) return 0U;
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    const Chunk* owner = chunk({chunkX, chunkZ});
    if (owner == nullptr) return 15U;
    return owner->directSkyLight(worldX - chunkX * kChunkWidth, y,
                                 worldZ - chunkZ * kChunkDepth);
}

namespace {
template <typename Setter>
bool setWorldLight(World& world, int worldX, int y, int worldZ, std::uint8_t value,
                   Setter setter) {
    if (!isWorldYInRange(y)) return false;
    const int chunkX = floorDiv(worldX, kChunkWidth);
    const int chunkZ = floorDiv(worldZ, kChunkDepth);
    Chunk* owner = world.chunk({chunkX, chunkZ});
    if (owner == nullptr) return false;
    return (owner->*setter)(worldX - chunkX * kChunkWidth, y,
                            worldZ - chunkZ * kChunkDepth, value);
}
} // namespace

bool World::setSkyLight(int worldX, int y, int worldZ, std::uint8_t value) {
    return setWorldLight(*this, worldX, y, worldZ, value, &Chunk::setSkyLight);
}

bool World::setBlockLight(int worldX, int y, int worldZ, std::uint8_t value) {
    return setWorldLight(*this, worldX, y, worldZ, value, &Chunk::setBlockLight);
}

bool World::setDirectSkyLight(int worldX, int y, int worldZ, std::uint8_t value) {
    return setWorldLight(*this, worldX, y, worldZ, value, &Chunk::setDirectSkyLight);
}

std::vector<ChunkPosition> World::positions() const {
    std::vector<ChunkPosition> result;
    result.reserve(chunks_.size());
    for (const auto& [position, chunkValue] : chunks_) {
        static_cast<void>(chunkValue);
        result.push_back(position);
    }
    std::ranges::sort(result, {}, [](const ChunkPosition& position) {
        return std::pair{position.z, position.x};
    });
    return result;
}

std::size_t World::residentBytes() const {
    std::size_t bytes = sizeof(*this);
    for (const auto& [position, chunkValue] : chunks_) {
        static_cast<void>(position);
        bytes += chunkValue->residentBytes() + sizeof(ChunkPosition);
    }
    return bytes;
}

std::size_t World::uniqueResidentBytes() const {
    std::size_t bytes = sizeof(*this);
    for (const auto& [position, chunkValue] : chunks_) {
        static_cast<void>(position);
        // use_count()==1 means no other World (server/client/worker) still shares
        // this chunk — it is a real exclusive copy. Shared chunks are physically
        // one copy and are excluded here so the number is not triple-counted.
        if (chunkValue.use_count() == 1) {
            bytes += chunkValue->residentBytes() + sizeof(ChunkPosition);
        }
    }
    return bytes;
}

} // namespace mc::world
