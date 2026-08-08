#include "world/ChunkSection.hpp"

#include <stdexcept>

namespace mc::world {

ChunkSection::ChunkSection() = default;

std::size_t ChunkSection::index(int x, int y, int z) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(kSectionSize) +
            static_cast<std::size_t>(z)) * static_cast<std::size_t>(kSectionSize) +
           static_cast<std::size_t>(x);
}

Block ChunkSection::block(int x, int y, int z) const {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) {
        return Block::Air;
    }
    return blocks_.empty() ? Block::Air : blocks_[index(x, y, z)];
}

void ChunkSection::setBlock(int x, int y, int z, Block value) {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) {
        throw std::out_of_range("ChunkSection coordinate is outside 16x16x16 bounds");
    }
    const std::size_t blockIndex = index(x, y, z);
    if (blocks_.empty()) {
        if (value == Block::Air) {
            return;
        }
        blocks_.assign(kBlockCount, Block::Air);
    }
    const Block previous = blocks_[blockIndex];
    if (previous == Block::Air && value != Block::Air) {
        ++nonAirBlockCount_;
    } else if (previous != Block::Air && value == Block::Air) {
        --nonAirBlockCount_;
    }
    blocks_[blockIndex] = value;
    if (!orientations_.empty()) {
        orientations_[blockIndex] = static_cast<std::uint8_t>(defaultOrientation(value));
    }
    if (!fluidLevels_.empty()) {
        fluidLevels_[blockIndex] = 0U;
    }
}

BlockOrientation ChunkSection::orientation(int x, int y, int z) const {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) {
        return BlockOrientation::North;
    }
    const auto blockValue = block(x, y, z);
    return orientations_.empty()
        ? defaultOrientation(blockValue)
        : static_cast<BlockOrientation>(orientations_[index(x, y, z)]);
}

void ChunkSection::setOrientation(int x, int y, int z, BlockOrientation value) {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) {
        throw std::out_of_range("ChunkSection coordinate is outside 16x16x16 bounds");
    }
    const auto blockIndex = index(x, y, z);
    if (orientations_.empty()) {
        const auto defaultValue = defaultOrientation(block(x, y, z));
        if (value == defaultValue) {
            return;
        }
        orientations_.assign(kBlockCount, static_cast<std::uint8_t>(BlockOrientation::North));
        if (!blocks_.empty()) {
            for (std::size_t index = 0; index < kBlockCount; ++index) {
                orientations_[index] = static_cast<std::uint8_t>(defaultOrientation(blocks_[index]));
            }
        }
    }
    orientations_[blockIndex] = static_cast<std::uint8_t>(value);
}

std::uint8_t ChunkSection::fluidLevel(int x, int y, int z) const {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) {
        return 0U;
    }
    return fluidLevels_.empty() ? 0U : fluidLevels_[index(x, y, z)];
}

void ChunkSection::setFluidLevel(int x, int y, int z, std::uint8_t value) {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) {
        throw std::out_of_range("ChunkSection coordinate is outside 16x16x16 bounds");
    }
    if (fluidLevels_.empty()) {
        if (value == 0U) {
            return;
        }
        fluidLevels_.assign(kBlockCount, 0U);
    }
    fluidLevels_[index(x, y, z)] = value;
}

std::uint8_t ChunkSection::skyLight(int x, int y, int z) const {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) return 0U;
    return skyLight_.get(index(x, y, z));
}

std::uint8_t ChunkSection::blockLight(int x, int y, int z) const {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) return 0U;
    return blockLight_.get(index(x, y, z));
}

std::uint8_t ChunkSection::directSkyLight(int x, int y, int z) const {
    if (x < 0 || x >= kSectionSize || y < 0 || y >= kSectionSize ||
        z < 0 || z >= kSectionSize) return 0U;
    return directSkyLight_.get(index(x, y, z));
}

bool ChunkSection::setSkyLight(int x, int y, int z, std::uint8_t value) {
    return skyLight_.set(index(x, y, z), value);
}

bool ChunkSection::setBlockLight(int x, int y, int z, std::uint8_t value) {
    return blockLight_.set(index(x, y, z), value);
}

bool ChunkSection::setDirectSkyLight(int x, int y, int z, std::uint8_t value) {
    return directSkyLight_.set(index(x, y, z), value);
}

} // namespace mc::world
