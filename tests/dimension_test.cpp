// DIM-0: dimension identity + static type table (headless).
#include "gameplay/command/CommandSource.hpp"
#include "world/Dimension.hpp"
#include "world/WorldClock.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

using mc::world::ClockId;
using mc::world::DimensionId;
using mc::world::DimensionType;
using mc::world::clockOf;
using mc::world::dimensionIdFromKey;
using mc::world::dimensionKey;
using mc::world::dimensionType;
using mc::world::hasFixedTime;
using mc::world::kDimensionCount;
using mc::world::kDimensionTypes;

int main() {
    // --- Identity: three dimensions in a dense enum ---------------------------
    static_assert(kDimensionCount == 3U, "DIM0 ships Overworld/Nether/End");
    static_assert(static_cast<std::uint8_t>(DimensionId::Overworld) == 0U);
    static_assert(static_cast<std::uint8_t>(DimensionId::Nether) == 1U);
    static_assert(static_cast<std::uint8_t>(DimensionId::End) == 2U);

    // The type table is dereferenced by subscript, not by a runtime map: the
    // accessor returns a reference straight into the constexpr rodata array. This
    // pins the DOD shape — a string-keyed map would not satisfy these.
    static_assert(std::is_same_v<decltype(dimensionType(DimensionId::Nether)),
                                 const DimensionType&>,
                  "dimensionType() derefs by subscript, returns a table reference");
    assert(&dimensionType(DimensionId::Overworld) == &kDimensionTypes[0]);
    assert(&dimensionType(DimensionId::Nether) == &kDimensionTypes[1]);
    assert(&dimensionType(DimensionId::End) == &kDimensionTypes[2]);

    // --- vanilla key aliases resolve both bare and minecraft:-qualified -------
    assert(dimensionIdFromKey("overworld") == DimensionId::Overworld);
    assert(dimensionIdFromKey("minecraft:overworld") == DimensionId::Overworld);
    assert(dimensionIdFromKey("the_nether") == DimensionId::Nether);
    assert(dimensionIdFromKey("minecraft:the_nether") == DimensionId::Nether);
    assert(dimensionIdFromKey("the_end") == DimensionId::End);
    assert(dimensionIdFromKey("minecraft:the_end") == DimensionId::End);
    // Unknown key is an expected miss, not a crash.
    assert(!dimensionIdFromKey("minecraft:aether").has_value());
    assert(!dimensionIdFromKey("").has_value());
    // Canonical serialised key mirrors vanilla (JC/DIM4 on-disk layout).
    assert(dimensionKey(DimensionId::Overworld) == "minecraft:overworld");
    assert(dimensionKey(DimensionId::Nether) == "minecraft:the_nether");
    assert(dimensionKey(DimensionId::End) == "minecraft:the_end");
    // Round trip: canonical key resolves back to its own id.
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(DimensionId::Count); ++i) {
        const auto dim = static_cast<DimensionId>(i);
        assert(dimensionIdFromKey(dimensionKey(dim)) == dim);
    }

    // --- Type table: the sabotage-guarded properties --------------------------
    const DimensionType& overworld = dimensionType(DimensionId::Overworld);
    const DimensionType& nether = dimensionType(DimensionId::Nether);
    const DimensionType& end = dimensionType(DimensionId::End);

    // Overworld: ordinary — sky, natural, 1:1, no ceiling, day cycle.
    assert(overworld.hasSkylight);
    assert(overworld.natural);
    assert(!overworld.hasCeiling);
    assert(!overworld.ultrawarm);
    assert(overworld.coordinateScale == 1.0);
    assert(!overworld.fixedTime.has_value());

    // Nether: ceiling, no sky, 8:1 scale, ultrawarm — the sabotage targets.
    assert(nether.hasCeiling);
    assert(!nether.hasSkylight);
    assert(nether.coordinateScale == 8.0);  // Sabotage ①: 1.0 here fails this (DIM5 scale)
    assert(nether.ultrawarm);
    assert(nether.fixedTime.has_value());

    // "Overworld has sky, Nether does not" — the EM/spawning input (Sabotage ②).
    assert(overworld.hasSkylight && !nether.hasSkylight);

    // End: no sky, not natural.
    assert(!end.hasSkylight);
    assert(!end.natural);
    assert(end.coordinateScale == 1.0);
    assert(end.fixedTime.has_value());

    // --- Clock extension: three dimensions, Nether/End fixed-time -------------
    static_assert(mc::world::kClockCount == kDimensionCount,
                  "one clock per dimension");
    assert(clockOf(DimensionId::Overworld) == ClockId::Overworld);
    assert(clockOf(DimensionId::Nether) == ClockId::Nether);
    assert(clockOf(DimensionId::End) == ClockId::End);
    // The Overworld runs the day; the Nether and End are pinned.
    assert(!hasFixedTime(DimensionId::Overworld));
    assert(hasFixedTime(DimensionId::Nether));
    assert(hasFixedTime(DimensionId::End));

    // --- CommandSource wiring: the source now carries a real DimensionId ------
    // The placeholder command::Dimension enum is gone; it aliases DimensionId.
    static_assert(std::is_same_v<mc::gameplay::command::Dimension, DimensionId>,
                  "CommandSource dimension is the real DimensionId, not a placeholder");
    {
        mc::gameplay::command::CommandSource source;
        // Regression: a fresh source defaults to the Overworld (behaviour
        // unchanged from the placeholder days).
        assert(source.dimension == DimensionId::Overworld);
        // withDimension carries a real id through a copy.
        const auto inNether = source.withDimension(DimensionId::Nether);
        assert(inNether.dimension == DimensionId::Nether);
        assert(source.dimension == DimensionId::Overworld);  // copy, not mutate
    }

    return 0;
}
