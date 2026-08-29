#pragma once

// STRUCT-2: where a structure may start — the `random_spread` placement from a
// vanilla `structure_set`. This is the deterministic grid that decides, for a
// world seed, which chunk in each spacing×spacing region is a structure's origin.
//
// Pure and headless: `originChunk` is a function of (region, worldSeed, salt), so
// the structure step can ask "does a structure start here?" without any world
// state, and two runs of the same seed place structures identically (the STRUCT
// determinism rule — `mc::rng` only, never wall-clock). Mirrors vanilla's
// RandomSpreadStructurePlacement + setLargeFeatureWithSalt bit-for-bit so a
// structure lands where a JE player of the same seed expects.
//
// ConcentricRings (strongholds) is a different placement and lands in STRUCT-4.

#include "gameplay/Random.hpp"

#include <cstdint>

namespace mc::world::gen {

enum class SpreadType : std::uint8_t { Linear, Triangular };

struct StructureChunk final {
    int x = 0;
    int z = 0;
    [[nodiscard]] bool operator==(const StructureChunk&) const = default;
};

struct StructurePlacement final {
    int spacing = 32;    // region size in chunks
    int separation = 8;  // minimum gap, so range = spacing - separation
    std::int32_t salt = 0;
    SpreadType spread = SpreadType::Linear;

    // Floor division that rounds toward negative infinity, so the region grid is
    // continuous across the origin (std `/` truncates toward zero and would double
    // the region straddling x=0).
    [[nodiscard]] static constexpr int floorDiv(int value, int divisor) {
        const int quotient = value / divisor;
        return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
    }

    // The origin chunk of the region (gridX, gridZ). Vanilla
    // setLargeFeatureWithSalt seeds a LegacyRandom from the region and salt, then
    // draws the in-region offset.
    [[nodiscard]] StructureChunk originChunk(int gridX, int gridZ, std::uint64_t worldSeed) const {
        const auto seed = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(gridX) * 341873128712LL +
            static_cast<std::int64_t>(gridZ) * 132897987541LL +
            static_cast<std::int64_t>(worldSeed) + salt);
        std::uint64_t state = mc::rng::seedFromValue(seed);
        const auto range = static_cast<std::uint32_t>(spacing - separation);
        const int offsetX = offset(state, range);
        const int offsetZ = offset(state, range);
        return {gridX * spacing + offsetX, gridZ * spacing + offsetZ};
    }

    // True when (chunkX, chunkZ) is its region's chosen origin — i.e. a structure
    // of this set starts here.
    [[nodiscard]] bool isStructureChunk(int chunkX, int chunkZ, std::uint64_t worldSeed) const {
        const int gridX = floorDiv(chunkX, spacing);
        const int gridZ = floorDiv(chunkZ, spacing);
        const StructureChunk origin = originChunk(gridX, gridZ, worldSeed);
        return origin.x == chunkX && origin.z == chunkZ;
    }

  private:
    [[nodiscard]] int offset(std::uint64_t& state, std::uint32_t range) const {
        if (range == 0U) {
            return 0;
        }
        if (spread == SpreadType::Triangular) {
            const int first = static_cast<int>(mc::rng::nextInt(state, range));
            const int second = static_cast<int>(mc::rng::nextInt(state, range));
            return (first + second) / 2;
        }
        return static_cast<int>(mc::rng::nextInt(state, range));
    }
};

} // namespace mc::world::gen
