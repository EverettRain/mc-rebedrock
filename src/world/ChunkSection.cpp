#include "world/ChunkSection.hpp"

#include <bit>
#include <stdexcept>
#include <utility>

namespace mc::world {
namespace {

// Palette index 0 is always the air state, so a freshly expanded section reads
// as air everywhere until something is written over it.
inline constexpr std::uint16_t kAirStateId = 0U;

static_assert(BlockState{}.rawId() == kAirStateId,
              "an all-zero section must read as air, or lazy allocation lies");

} // namespace

ChunkSection::ChunkSection() = default;

std::size_t ChunkSection::index(int x, int y, int z) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(kSectionSize) +
            static_cast<std::size_t>(z)) * static_cast<std::size_t>(kSectionSize) +
           static_cast<std::size_t>(x);
}

bool ChunkSection::inBounds(int x, int y, int z) {
    return x >= 0 && x < kSectionSize && y >= 0 && y < kSectionSize &&
           z >= 0 && z < kSectionSize;
}

std::uint8_t ChunkSection::bitsFor(std::size_t paletteSize) {
    // A multi-state section names at least two palette entries and never fewer
    // than one bit; ceil(log2(size)) counts the rest.
    if (paletteSize <= 2U) {
        return 1U;
    }
    return static_cast<std::uint8_t>(std::bit_width(paletteSize - 1U));
}

std::size_t ChunkSection::longsFor(std::uint8_t bits) {
    const std::size_t valuesPerLong = 64U / bits;
    return (kBlockCount + valuesPerLong - 1U) / valuesPerLong;
}

std::uint16_t ChunkSection::readIndex(std::size_t cell) const {
    const std::size_t valuesPerLong = 64U / bitsPerEntry_;
    const std::size_t word = cell / valuesPerLong;
    const std::size_t offset = (cell % valuesPerLong) * bitsPerEntry_;
    const std::uint64_t mask = (std::uint64_t{1} << bitsPerEntry_) - 1U;
    return static_cast<std::uint16_t>((data_[word] >> offset) & mask);
}

void ChunkSection::writeIndex(std::size_t cell, std::uint16_t paletteIndex) {
    const std::size_t valuesPerLong = 64U / bitsPerEntry_;
    const std::size_t word = cell / valuesPerLong;
    const std::size_t offset = (cell % valuesPerLong) * bitsPerEntry_;
    const std::uint64_t mask = (std::uint64_t{1} << bitsPerEntry_) - 1U;
    data_[word] = (data_[word] & ~(mask << offset)) |
                  (static_cast<std::uint64_t>(paletteIndex) << offset);
}

void ChunkSection::growBits(std::uint8_t newBits) {
    // Re-pack every cell's palette index into wider slots. The indices are
    // unchanged — only their packing widens — so the section's contents and its
    // non-air count are untouched.
    const std::vector<std::uint64_t> old = std::move(data_);
    const std::uint8_t oldBits = bitsPerEntry_;
    const std::size_t oldValuesPerLong = 64U / oldBits;
    const std::uint64_t oldMask = (std::uint64_t{1} << oldBits) - 1U;
    data_.assign(longsFor(newBits), 0ULL);
    bitsPerEntry_ = newBits;
    for (std::size_t cell = 0; cell < kBlockCount; ++cell) {
        const std::size_t word = cell / oldValuesPerLong;
        const std::size_t offset = (cell % oldValuesPerLong) * oldBits;
        const auto oldIndex = static_cast<std::uint16_t>((old[word] >> offset) & oldMask);
        writeIndex(cell, oldIndex);
    }
}

std::uint16_t ChunkSection::internState(std::uint16_t rawId) {
    for (std::size_t i = 0; i < palette_.size(); ++i) {
        if (palette_[i] == rawId) {
            return static_cast<std::uint16_t>(i);
        }
    }
    const auto newIndex = static_cast<std::uint16_t>(palette_.size());
    palette_.push_back(rawId);
    const std::uint8_t needed = bitsFor(palette_.size());
    if (needed > bitsPerEntry_) {
        growBits(needed);
    }
    return newIndex;
}

BlockState ChunkSection::state(int x, int y, int z) const {
    if (!inBounds(x, y, z)) {
        return BlockState{};
    }
    if (bitsPerEntry_ == 0U) {
        return BlockState{}; // uniform air section, nothing allocated
    }
    return BlockState::fromRawId(palette_[readIndex(index(x, y, z))]);
}

void ChunkSection::setState(int x, int y, int z, BlockState value) {
    if (!inBounds(x, y, z)) {
        throw std::out_of_range("ChunkSection coordinate is outside 16x16x16 bounds");
    }
    const std::uint16_t rawId = value.rawId();
    if (bitsPerEntry_ == 0U) {
        // The section is uniform air. Writing air leaves it uniform (and free);
        // the first real block expands it to { air, <state> } at one bit.
        if (rawId == kAirStateId) {
            return;
        }
        palette_.assign(1U, kAirStateId);
        bitsPerEntry_ = 1U;
        data_.assign(longsFor(1U), 0ULL);
    }
    const std::size_t cell = index(x, y, z);
    const Block previous = BlockState::fromRawId(palette_[readIndex(cell)]).block();
    const std::uint16_t paletteIndex = internState(rawId); // may widen bitsPerEntry_
    writeIndex(cell, paletteIndex);
    const Block next = value.block();
    if (previous == Block::Air && next != Block::Air) {
        ++nonAirBlockCount_;
    } else if (previous != Block::Air && next == Block::Air) {
        --nonAirBlockCount_;
    }
}

std::size_t ChunkSection::stateHeapBytes() const {
    return palette_.capacity() * sizeof(std::uint16_t) +
           data_.capacity() * sizeof(std::uint64_t);
}

std::size_t ChunkSection::lightHeapBytes() const {
    const auto nibbleBytes = [](const NibbleArray& array) {
        return array.uniform() ? std::size_t{0U} : NibbleArray::kByteCount;
    };
    return nibbleBytes(skyLight_) + nibbleBytes(blockLight_) + nibbleBytes(directSkyLight_);
}

Block ChunkSection::block(int x, int y, int z) const {
    return state(x, y, z).block();
}

void ChunkSection::setBlock(int x, int y, int z, Block value) {
    // Changing a cell's block resets its state slot and fluid level, exactly as
    // the three separate arrays used to: a furnace replaced by stone does not
    // keep the furnace's facing. Building the state from the block alone is
    // what does it now.
    setState(x, y, z, BlockState{value, defaultOrientation(value), 0U});
}

BlockOrientation ChunkSection::orientation(int x, int y, int z) const {
    // state() already resolves out-of-bounds and a uniform-air section to the
    // air default, whose orientation is North.
    return state(x, y, z).orientation();
}

void ChunkSection::setOrientation(int x, int y, int z, BlockOrientation value) {
    if (!inBounds(x, y, z)) {
        throw std::out_of_range("ChunkSection coordinate is outside 16x16x16 bounds");
    }
    // BlockState numbers only the states a block declares, so an orientation
    // outside that block's range lands on the block's default rather than being
    // stored. No caller writes one today — crop age, farmland moisture, the log
    // axis and the horizontal facings all stay inside their block's range, and
    // chunk_section_test walks the registry to keep it that way. The clamp is
    // deliberately silent rather than a throw: the old three-array storage
    // accepted the byte and every reader ignored it, so refusing loudly here
    // would be a new crash path rather than the representation swap this is.
    setState(x, y, z, state(x, y, z).with(value));
}

std::uint8_t ChunkSection::fluidLevel(int x, int y, int z) const {
    return state(x, y, z).fluidLevel();
}

void ChunkSection::setFluidLevel(int x, int y, int z, std::uint8_t value) {
    if (!inBounds(x, y, z)) {
        throw std::out_of_range("ChunkSection coordinate is outside 16x16x16 bounds");
    }
    setState(x, y, z, state(x, y, z).withFluidLevel(value));
}

std::uint8_t ChunkSection::skyLight(int x, int y, int z) const {
    if (!inBounds(x, y, z)) return 0U;
    return skyLight_.get(index(x, y, z));
}

std::uint8_t ChunkSection::blockLight(int x, int y, int z) const {
    if (!inBounds(x, y, z)) return 0U;
    return blockLight_.get(index(x, y, z));
}

std::uint8_t ChunkSection::directSkyLight(int x, int y, int z) const {
    if (!inBounds(x, y, z)) return 0U;
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
