#pragma once

#include <cstdint>

namespace mc::world {

// A block position in world space. World-layer callers (the mutation service,
// block behaviour, the neighbour updater) use this rather than gameplay's
// SimulationPosition so world/ keeps no dependency on gameplay/.
struct BlockPos final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const BlockPos&) const = default;
};

// Packed block coordinate, bit-for-bit identical to Java Edition's
// BlockPos.asLong(): X in bits [38,63] (26 bits), Z in bits [12,37] (26 bits),
// Y in bits [0,11] (12 bits), each a two's-complement signed field. This is the
// one encoding the neighbour updater keys its records on and the one a JE save
// converter reads back, so the two must never drift — a lossless map to
// `block_ticks`/`fluid_ticks` and to any long-keyed position set depends on it.
inline constexpr int kPackedXBits = 26;
inline constexpr int kPackedYBits = 12; // 64 - 26 - 26
inline constexpr int kPackedZBits = 26;
inline constexpr int kPackedYOffset = 0;
inline constexpr int kPackedZOffset = kPackedYBits;                 // 12
inline constexpr int kPackedXOffset = kPackedYBits + kPackedZBits;  // 38
inline constexpr std::int64_t kPackedXMask = (std::int64_t{1} << kPackedXBits) - 1;
inline constexpr std::int64_t kPackedYMask = (std::int64_t{1} << kPackedYBits) - 1;
inline constexpr std::int64_t kPackedZMask = (std::int64_t{1} << kPackedZBits) - 1;

[[nodiscard]] constexpr std::int64_t packBlockPos(int x, int y, int z) {
    std::int64_t packed = 0;
    packed |= (static_cast<std::int64_t>(x) & kPackedXMask) << kPackedXOffset;
    packed |= (static_cast<std::int64_t>(y) & kPackedYMask) << kPackedYOffset;
    packed |= (static_cast<std::int64_t>(z) & kPackedZMask) << kPackedZOffset;
    return packed;
}

[[nodiscard]] constexpr std::int64_t packBlockPos(BlockPos pos) {
    return packBlockPos(pos.x, pos.y, pos.z);
}

namespace detail {
// Slide the field to the top of the word (as unsigned, so the shift stays
// well-defined) then arithmetic-shift it back down, sign-extending the field —
// exactly how JE's BlockPos.getX/Y/Z recover a signed coordinate.
[[nodiscard]] constexpr int unpackSignedField(std::int64_t packed, int offset, int bits) {
    const int toTop = 64 - offset - bits;
    const auto atTop = static_cast<std::int64_t>(static_cast<std::uint64_t>(packed) << toTop);
    return static_cast<int>(atTop >> (64 - bits));
}
} // namespace detail

[[nodiscard]] constexpr int unpackBlockPosX(std::int64_t packed) {
    return detail::unpackSignedField(packed, kPackedXOffset, kPackedXBits);
}
[[nodiscard]] constexpr int unpackBlockPosY(std::int64_t packed) {
    return detail::unpackSignedField(packed, kPackedYOffset, kPackedYBits);
}
[[nodiscard]] constexpr int unpackBlockPosZ(std::int64_t packed) {
    return detail::unpackSignedField(packed, kPackedZOffset, kPackedZBits);
}

[[nodiscard]] constexpr BlockPos unpackBlockPos(std::int64_t packed) {
    return {unpackBlockPosX(packed), unpackBlockPosY(packed), unpackBlockPosZ(packed)};
}

} // namespace mc::world
