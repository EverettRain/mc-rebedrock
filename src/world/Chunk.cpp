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
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight ||
        z < 0 || z >= kChunkDepth) {
        return Block::Air;
    }
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .block(x, y % kSectionSize, z);
}

void Chunk::setBlock(int x, int y, int z, Block value) {
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight ||
        z < 0 || z >= kChunkDepth) {
        throw std::out_of_range("Chunk block coordinate is outside 16x256x16 bounds");
    }
    sections_[static_cast<std::size_t>(y / kSectionSize)]
        .setBlock(x, y % kSectionSize, z, value);
}

BlockOrientation Chunk::orientation(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight ||
        z < 0 || z >= kChunkDepth) {
        return BlockOrientation::North;
    }
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .orientation(x, y % kSectionSize, z);
}

void Chunk::setOrientation(int x, int y, int z, BlockOrientation value) {
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight ||
        z < 0 || z >= kChunkDepth) {
        throw std::out_of_range("Chunk orientation coordinate is outside 16x256x16 bounds");
    }
    sections_[static_cast<std::size_t>(y / kSectionSize)]
        .setOrientation(x, y % kSectionSize, z, value);
}

std::uint8_t Chunk::fluidLevel(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight ||
        z < 0 || z >= kChunkDepth) {
        return 0U;
    }
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .fluidLevel(x, y % kSectionSize, z);
}

void Chunk::setFluidLevel(int x, int y, int z, std::uint8_t value) {
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight ||
        z < 0 || z >= kChunkDepth) {
        throw std::out_of_range("Chunk fluid coordinate is outside 16x256x16 bounds");
    }
    sections_[static_cast<std::size_t>(y / kSectionSize)]
        .setFluidLevel(x, y % kSectionSize, z, value);
}

std::uint8_t Chunk::skyLight(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight || z < 0 || z >= kChunkDepth)
        return 0U;
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .skyLight(x, y % kSectionSize, z);
}

std::uint8_t Chunk::blockLight(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight || z < 0 || z >= kChunkDepth)
        return 0U;
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .blockLight(x, y % kSectionSize, z);
}

std::uint8_t Chunk::directSkyLight(int x, int y, int z) const {
    if (x < 0 || x >= kChunkWidth || y < 0 || y >= kWorldHeight || z < 0 || z >= kChunkDepth)
        return 0U;
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .directSkyLight(x, y % kSectionSize, z);
}

bool Chunk::setSkyLight(int x, int y, int z, std::uint8_t value) {
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .setSkyLight(x, y % kSectionSize, z, value);
}

bool Chunk::setBlockLight(int x, int y, int z, std::uint8_t value) {
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .setBlockLight(x, y % kSectionSize, z, value);
}

bool Chunk::setDirectSkyLight(int x, int y, int z, std::uint8_t value) {
    return sections_[static_cast<std::size_t>(y / kSectionSize)]
        .setDirectSkyLight(x, y % kSectionSize, z, value);
}

const ChunkSection& Chunk::section(int sectionY) const {
    if (sectionY < 0 || sectionY >= kSectionCount) {
        throw std::out_of_range("Chunk section index is outside 0..15");
    }
    return sections_[static_cast<std::size_t>(sectionY)];
}

ChunkSection& Chunk::section(int sectionY) {
    if (sectionY < 0 || sectionY >= kSectionCount) {
        throw std::out_of_range("Chunk section index is outside 0..15");
    }
    return sections_[static_cast<std::size_t>(sectionY)];
}

} // namespace mc::world
