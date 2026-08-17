#include "world/Chunk.hpp"

#include <stdexcept>

namespace mc::world {

Chunk::Chunk() {
    columnBiomes_.fill(gen::Biome::Plains);
}

gen::Biome Chunk::columnBiome(int localX, int localZ) const {
    return columnBiomes_[static_cast<std::size_t>(localZ * kChunkWidth + localX)];
}

void Chunk::setColumnBiome(int localX, int localZ, gen::Biome biome) {
    columnBiomes_[static_cast<std::size_t>(localZ * kChunkWidth + localX)] = biome;
}

Block Chunk::block(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth) {
        return Block::Air;
    }
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .block(x, yInSectionFromWorldY(y), z);
}

void Chunk::setBlock(int x, int y, int z, Block value) {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth) {
        throw std::out_of_range("Chunk block coordinate is outside 16x384x16 bounds");
    }
    sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .setBlock(x, yInSectionFromWorldY(y), z, value);
}

BlockState Chunk::state(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth) {
        return BlockState{};
    }
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .state(x, yInSectionFromWorldY(y), z);
}

void Chunk::setState(int x, int y, int z, BlockState value) {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth) {
        throw std::out_of_range("Chunk state coordinate is outside 16x384x16 bounds");
    }
    sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .setState(x, yInSectionFromWorldY(y), z, value);
}

BlockOrientation Chunk::orientation(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth) {
        return BlockOrientation::North;
    }
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .orientation(x, yInSectionFromWorldY(y), z);
}

void Chunk::setOrientation(int x, int y, int z, BlockOrientation value) {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth) {
        throw std::out_of_range("Chunk orientation coordinate is outside 16x384x16 bounds");
    }
    sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .setOrientation(x, yInSectionFromWorldY(y), z, value);
}

std::uint8_t Chunk::fluidLevel(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth) {
        return 0U;
    }
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .fluidLevel(x, yInSectionFromWorldY(y), z);
}

void Chunk::setFluidLevel(int x, int y, int z, std::uint8_t value) {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth) {
        throw std::out_of_range("Chunk fluid coordinate is outside 16x384x16 bounds");
    }
    sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .setFluidLevel(x, yInSectionFromWorldY(y), z, value);
}

std::uint8_t Chunk::skyLight(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth)
        return 0U;
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .skyLight(x, yInSectionFromWorldY(y), z);
}

std::uint8_t Chunk::blockLight(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth)
        return 0U;
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .blockLight(x, yInSectionFromWorldY(y), z);
}

std::uint8_t Chunk::directSkyLight(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || !isWorldYInRange(y) || z < 0 || z >= kChunkDepth)
        return 0U;
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .directSkyLight(x, yInSectionFromWorldY(y), z);
}

bool Chunk::setSkyLight(int x, int y, int z, std::uint8_t value) {
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .setSkyLight(x, yInSectionFromWorldY(y), z, value);
}

bool Chunk::setBlockLight(int x, int y, int z, std::uint8_t value) {
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .setBlockLight(x, yInSectionFromWorldY(y), z, value);
}

bool Chunk::setDirectSkyLight(int x, int y, int z, std::uint8_t value) {
    return sections_[static_cast<std::size_t>(sectionIndexFromWorldY(y))]
        .setDirectSkyLight(x, yInSectionFromWorldY(y), z, value);
}

const ChunkSection& Chunk::section(int sectionY) const {
    if (sectionY < 0 || sectionY >= kSectionCount) {
        throw std::out_of_range("Chunk section index is outside 0..23");
    }
    return sections_[static_cast<std::size_t>(sectionY)];
}

ChunkSection& Chunk::section(int sectionY) {
    if (sectionY < 0 || sectionY >= kSectionCount) {
        throw std::out_of_range("Chunk section index is outside 0..23");
    }
    return sections_[static_cast<std::size_t>(sectionY)];
}

std::size_t Chunk::residentBytes() const {
    std::size_t bytes = sizeof(*this);
    for (const auto& section : sections_) {
        bytes += section.stateHeapBytes() + section.lightHeapBytes();
    }
    return bytes;
}

} // namespace mc::world
