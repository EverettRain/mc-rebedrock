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
// The Y an entity (dropped item, experience orb, falling block) falls to before
// the void removes it — vanilla's `minBuildHeight - 64`. Everything above kMinY is
// solid world (bedrock sits at kMinY), so the despawn line must be *below* the
// world, not at 0: with kMinY at -64 a hard-coded `y < -8` would delete every drop
// mined below y=-8, which is most of the deepslate layer. Any drop/orb/entity Y
// below this is genuinely in the void and cleared.
inline constexpr float kVoidDespawnY = static_cast<float>(kMinY) - 64.0F;

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

// World coordinate -> the chunk index containing it. C++'s `/` truncates toward
// zero, so a plain `x / kChunkWidth` puts x = -1 in chunk 0 instead of chunk -1
// and the whole negative half of the world reads one chunk off. `>> 4` happens
// to be correct for a power-of-two width but silently hard-codes it; this stays
// honest about the divisor. Several call sites carry a private copy of this —
// the collision walk's is what pulled it up here.
[[nodiscard]] inline constexpr int floorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor < 0) ? quotient - 1 : quotient;
}

// Whether smooth lighting (26.1's ambient occlusion) is applied. 26.1 makes this
// a boolean — `OptionInstance.createBoolean("options.ao", true)`, and
// `OptionsAmbientOcclusionFix` is the datafixer that migrated the old three-tier
// setting to it — and so does this now (RN-19b). It used to carry a third,
// self-invented "Standard" tier that averaged a 0.35 floor instead of vanilla's
// 0.2 and then remapped it into [0.72, 1.0] in the shader; it was the default,
// so the AO nobody could see was nobody's fault but the tier's. Measurement
// killed the one argument for keeping it: it was 0.6%-2.6% *slower* than the
// vanilla algorithm, not cheaper (RN-19 §9.1).
//
// The mesh no longer depends on this value — one algorithm bakes it, and Off
// only tells the shader to ignore the AO channel and read the flat light — so
// toggling it no longer remeshes the world.
enum class SmoothLightingQuality : std::uint8_t { Off, On };

// What the terrain shaders read as `lightingSettings.y`: whether to apply the
// baked AO channel and the smooth light at all, or fall back to the flat light
// with no occlusion. Off is a shader-side switch and nothing more — the mesh is
// byte-identical either way — so this is the whole of what the option does at
// runtime, and it lives here rather than inline in the frame code so a headless
// test can hold it to that.
[[nodiscard]] constexpr float smoothLightingShaderSwitch(SmoothLightingQuality quality) {
    return quality == SmoothLightingQuality::Off ? 0.0F : 1.0F;
}

static_assert(kSectionCount == 24);
static_assert(kWorldHeight % kSectionSize == 0);
static_assert(kMaxY - kMinY == kWorldHeight);

} // namespace mc::world
