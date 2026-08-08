#pragma once

#include <cstdint>

namespace mc::world {

inline constexpr int kChunkWidth = 16;
inline constexpr int kChunkDepth = 16;
inline constexpr int kWorldHeight = 256;
inline constexpr int kSectionSize = 16;
inline constexpr int kSectionCount = kWorldHeight / kSectionSize;
inline constexpr int kSeaLevel = 63;

// The smooth-lighting algorithm the mesh was baked with. Off keeps the flat
// light values; Standard is the current binary-AO algorithm; High is the
// vanilla 1.16.1 per-block AO. Because VoxelVertex has no room for two AO
// sets, the quality is baked into the mesh and a change remeshes the world.
enum class SmoothLightingQuality : std::uint8_t { Off, Standard, High };

static_assert(kSectionCount == 16);
static_assert(kWorldHeight % kSectionSize == 0);

} // namespace mc::world
