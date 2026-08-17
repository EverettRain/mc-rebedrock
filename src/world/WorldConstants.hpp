#pragma once

#include <cstdint>

namespace mc::world {

inline constexpr int kChunkWidth = 16;
inline constexpr int kChunkDepth = 16;
// The world's bottommost row (inclusive), matching 1.18's −64..319 column.
// World Y is an absolute coordinate throughout: the height span below is the
// number of rows, not an upper bound.
inline constexpr int kMinY = -64;
inline constexpr int kWorldHeight = 384;
inline constexpr int kSectionSize = 16;
inline constexpr int kSectionCount = kWorldHeight / kSectionSize;
inline constexpr int kSeaLevel = 63;
// The first Y above the world (exclusive top).
inline constexpr int kMaxY = kMinY + kWorldHeight;

// World Y → the index of the section it falls in (0 = the chunk's bottom
// section), and the local Y within that section. Both subtract kMinY first:
// C++'s truncating `/` and `%` would give the wrong section for the negative
// rows otherwise.
[[nodiscard]] inline constexpr int sectionIndexFromWorldY(int y) {
    return (y - kMinY) / kSectionSize;
}
[[nodiscard]] inline constexpr int yInSectionFromWorldY(int y) {
    return (y - kMinY) % kSectionSize;
}
// The world Y a section's base row sits at (section 0 = the bottom).
[[nodiscard]] inline constexpr int sectionOriginY(int sectionY) {
    return kMinY + sectionY * kSectionSize;
}
[[nodiscard]] inline constexpr bool isWorldYInRange(int y) {
    return y >= kMinY && y < kMaxY;
}

// The smooth-lighting algorithm the mesh was baked with. Off keeps the flat
// light values; Standard is the current binary-AO algorithm; High is the
// vanilla 1.16.1 per-block AO. Because VoxelVertex has no room for two AO
// sets, the quality is baked into the mesh and a change remeshes the world.
enum class SmoothLightingQuality : std::uint8_t { Off, Standard, High };

static_assert(kSectionCount == 24);
static_assert(kWorldHeight % kSectionSize == 0);
static_assert(kMaxY - kMinY == kWorldHeight);

} // namespace mc::world
