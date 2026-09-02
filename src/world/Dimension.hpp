#pragma once

#include "world/attribute/EnvironmentAttribute.hpp"

#include "world/WorldClock.hpp"  // ClockId — one clock per dimension, index-aligned

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mc::world {

// The dimensions a world holds, mirroring 26.1's built-in Level keys
// (minecraft:overworld / the_nether / the_end). A dense enum rather than a
// ResourceKey<Level> string hash for the same reason ClockId and BlockId are:
// dereferencing a dimension is an array subscript, and a reference to one is the
// enum value, never a heap Holder. The registry-shaped path (data/<space>/
// dimension_type/*.json) waits for DIM data-isation; while the set is the three
// built-ins it is a fixed enum, exactly as WorldClocks is in vanilla.
enum class DimensionId : std::uint8_t {
    Overworld,
    Nether,
    End,
    Count,
};

inline constexpr std::size_t kDimensionCount = static_cast<std::size_t>(DimensionId::Count);

// Resolves a vanilla dimension key to its DimensionId, so a JC import or a
// `execute in <dimension>` string lands on the dense id without a runtime map.
// Both the bare path and the fully-qualified `minecraft:` form are accepted, and
// so is ReBedrock's own the-nether/the-end spelling; anything else is nullopt
// (an unknown key is an expected answer here, not a fatal one). The vanilla keys
// are aliases of the same id — DIM4's save layout and JC import align on them
// without introducing a new deviation.
[[nodiscard]] constexpr std::optional<DimensionId> dimensionIdFromKey(std::string_view key) {
    if (key == "overworld" || key == "minecraft:overworld") {
        return DimensionId::Overworld;
    }
    if (key == "the_nether" || key == "minecraft:the_nether" || key == "nether"
        || key == "minecraft:nether") {
        return DimensionId::Nether;
    }
    if (key == "the_end" || key == "minecraft:the_end" || key == "end"
        || key == "minecraft:end") {
        return DimensionId::End;
    }
    return std::nullopt;
}

// The canonical vanilla key an id serialises to (never a bare id): DIM4 region
// subdirectories and JC export write this, so the on-disk name matches vanilla.
[[nodiscard]] constexpr std::string_view dimensionKey(DimensionId dimension) {
    switch (dimension) {
    case DimensionId::Overworld:
        return "minecraft:overworld";
    case DimensionId::Nether:
        return "minecraft:the_nether";
    case DimensionId::End:
        return "minecraft:the_end";
    case DimensionId::Count:
        break;
    }
    return {};
}

// The static, per-dimension properties, mirroring 26.1's DimensionType record.
// A value POD in a constexpr rodata table, not an object with a vtable: it is
// pure data every subtree reads (the whole point is that "is there a sky here?"
// is one field lookup, never an `if (dim == Overworld)` scattered across EM,
// spawning, fluids and the clock). `fixedTime` is optional because only the
// Nether and the End freeze their clock; a present value is the tick the sun is
// pinned to.
struct DimensionType final {
    // Vertical bounds, aligned with the world-height milestone (26.1 build
    // limits). minY is the lowest buildable layer; height is the number of
    // layers above it.
    std::int32_t minY = 0;
    std::int32_t height = 256;

    bool hasSkylight = true;   // EM daytime burning / mob spawning read this
    bool hasCeiling = false;   // the Nether's bedrock roof
    bool natural = true;       // beds/compasses behave; the End/Nether do not
    bool ultrawarm = true;     // water evaporates, lava flows faster (Nether); F3 reads this
    bool bedWorks = true;      // sleeping vs. the Nether/End explosion
    bool respawnAnchorWorks = false;  // Nether-only respawn anchor
    bool piglinSafe = false;   // piglins do not zombify (Nether)

    double coordinateScale = 1.0;  // DIM5 reads this: the Nether is 8:1
    float ambientLight = 0.0F;     // baseline light added everywhere (Nether 0.1)
    std::uint8_t monsterSpawnLightLevel = 0U;  // max block light that still spawns monsters

    std::optional<std::uint64_t> fixedTime;  // Nether/End: the clock does not advance the day

    [[nodiscard]] bool operator==(const DimensionType&) const = default;
};

// The built-in dimension types, baked into rodata (26.1's default_overworld /
// the_nether / the_end). Indexed straight by DimensionId — deref is a subscript,
// not a hash. Overworld figures are 26.1 build limits; the Nether/End mirror
// vanilla's fixed-time, no-sky, ceiling/scale flags.
inline constexpr std::array<DimensionType, kDimensionCount> kDimensionTypes = {{
    // Overworld: full daylight cycle, sky, natural, 1:1 scale.
    DimensionType{
        .minY = -64,
        .height = 384,
        .hasSkylight = true,
        .hasCeiling = false,
        .natural = true,
        .ultrawarm = false,
        .bedWorks = true,
        .respawnAnchorWorks = false,
        .piglinSafe = false,
        .coordinateScale = 1.0,
        .ambientLight = 0.0F,
        .monsterSpawnLightLevel = 0U,
        .fixedTime = std::nullopt,
    },
    // Nether: no sky, bedrock ceiling, ultrawarm, 8:1 scale, fixed noon-ish
    // light, beds explode, respawn anchor works, piglins safe.
    DimensionType{
        .minY = 0,
        .height = 256,
        .hasSkylight = false,
        .hasCeiling = true,
        .natural = false,
        .ultrawarm = true,
        .bedWorks = false,
        .respawnAnchorWorks = true,
        .piglinSafe = true,
        .coordinateScale = 8.0,
        .ambientLight = 0.1F,
        .monsterSpawnLightLevel = 15U,
        .fixedTime = std::uint64_t{18000},
    },
    // End: no sky, no ceiling, not natural, 1:1 scale, fixed dim daylight,
    // beds explode.
    DimensionType{
        .minY = 0,
        .height = 256,
        .hasSkylight = false,
        .hasCeiling = false,
        .natural = false,
        .ultrawarm = false,
        .bedWorks = false,
        .respawnAnchorWorks = false,
        .piglinSafe = false,
        .coordinateScale = 1.0,
        .ambientLight = 0.0F,
        .monsterSpawnLightLevel = 0U,
        .fixedTime = std::uint64_t{6000},
    },
}};

// Dereferences a dimension to its static type. A subscript into rodata; the id
// is a caller's own enum value, so an out-of-range one is a programming bug the
// bounds assert catches, never a lookup miss (DimensionId::Count is a sentinel,
// not a real dimension).
// The per-dimension attribute layer (26.1's DimensionType#attributes).
//
// These overlap DimensionType's own booleans on purpose and for now: the fields
// above have callers all over the codebase, so they stay, and this layer is the
// 26.1-shaped statement of the same facts plus the ones the fields never
// carried (fog, sky, the light colours). A test asserts the two agree, which is
// the guard while both exist; folding the duplicated booleans into the layer is
// a follow-up, not something to do halfway.
//
// Values are vanilla's own, from DimensionTypes.java. Attributes carrying a
// reference (music, ambient sounds, the dripstone particle) are deliberately
// absent until BM-3's audio wiring gives them a side table to point into.
[[nodiscard]] constexpr attribute::EnvAttrLayer overworldAttributes() {
    using attribute::AttrValue;
    using attribute::EnvAttr;
    attribute::EnvAttrLayer layer{};
    layer.set(EnvAttr::FogColor, AttrValue::ofColor(-4138753))
        .set(EnvAttr::SkyColor, AttrValue::ofColor(attribute::calculateSkyColor(0.8F)))
        .set(EnvAttr::AmbientLightColor, AttrValue::ofColor(-16119286))
        .set(EnvAttr::CloudColor,
             AttrValue::ofColor(attribute::packArgbFromFloat(0.8F, 1.0F, 1.0F, 1.0F)))
        .set(EnvAttr::CloudHeight, AttrValue::ofFloat(192.33F))
        .set(EnvAttr::BedRule,
             AttrValue::ofEnum(static_cast<std::int32_t>(attribute::BedRule::CanSleepWhenDark)))
        .set(EnvAttr::RespawnAnchorWorks, AttrValue::ofBool(false))
        .set(EnvAttr::NetherPortalSpawnsPiglin, AttrValue::ofBool(true));
    return layer;
}

[[nodiscard]] constexpr attribute::EnvAttrLayer netherAttributes() {
    using attribute::AttrValue;
    using attribute::EnvAttr;
    attribute::EnvAttrLayer layer{};
    layer.set(EnvAttr::FogStartDistance, AttrValue::ofFloat(10.0F))
        .set(EnvAttr::FogEndDistance, AttrValue::ofFloat(96.0F))
        // Timelines.NIGHT_SKY_LIGHT_COLOR
        .set(EnvAttr::SkyLightColor,
             AttrValue::ofColor(attribute::packArgbFromFloat(1.0F, 0.48F, 0.48F, 1.0F)))
        .set(EnvAttr::SkyLightLevel, AttrValue::ofFloat(4.0F))
        .set(EnvAttr::SkyLightFactor, AttrValue::ofFloat(0.0F))
        .set(EnvAttr::AmbientLightColor, AttrValue::ofColor(-13621215))
        .set(EnvAttr::BedRule,
             AttrValue::ofEnum(static_cast<std::int32_t>(attribute::BedRule::Explodes)))
        .set(EnvAttr::RespawnAnchorWorks, AttrValue::ofBool(true))
        .set(EnvAttr::WaterEvaporates, AttrValue::ofBool(true))
        .set(EnvAttr::FastLava, AttrValue::ofBool(true))
        .set(EnvAttr::PiglinsZombify, AttrValue::ofBool(false))
        .set(EnvAttr::CanStartRaid, AttrValue::ofBool(false))
        .set(EnvAttr::SnowGolemMelts, AttrValue::ofBool(true));
    return layer;
}

[[nodiscard]] constexpr attribute::EnvAttrLayer endAttributes() {
    using attribute::AttrValue;
    using attribute::EnvAttr;
    attribute::EnvAttrLayer layer{};
    layer.set(EnvAttr::FogColor, AttrValue::ofColor(-15199464))
        .set(EnvAttr::SkyLightColor, AttrValue::ofColor(-5480243))
        .set(EnvAttr::SkyColor, AttrValue::ofColor(-16777216))
        .set(EnvAttr::SkyLightFactor, AttrValue::ofFloat(0.0F))
        .set(EnvAttr::AmbientLightColor, AttrValue::ofColor(-12630209))
        .set(EnvAttr::BedRule,
             AttrValue::ofEnum(static_cast<std::int32_t>(attribute::BedRule::Explodes)))
        .set(EnvAttr::RespawnAnchorWorks, AttrValue::ofBool(false));
    return layer;
}

inline constexpr std::array<attribute::EnvAttrLayer, kDimensionCount> kDimensionAttributes{{
    overworldAttributes(),
    netherAttributes(),
    endAttributes(),
}};

[[nodiscard]] constexpr const attribute::EnvAttrLayer& dimensionAttributes(DimensionId id) {
    const auto index = static_cast<std::size_t>(id);
    return kDimensionAttributes[index < kDimensionAttributes.size() ? index : 0U];
}

[[nodiscard]] constexpr const DimensionType& dimensionType(DimensionId dimension) {
    assert(static_cast<std::size_t>(dimension) < kDimensionCount);
    return kDimensionTypes[static_cast<std::size_t>(dimension)];
}

// DimensionId and ClockId are index-aligned, so a dimension's clock is the same
// subscript. Named for readers, and pinned to the enums so a reorder of either
// is a compile error rather than a silently mismatched sun.
static_assert(kDimensionCount == kClockCount,
              "one clock per dimension: keep DimensionId and ClockId in lockstep");
static_assert(static_cast<std::uint8_t>(DimensionId::Overworld)
              == static_cast<std::uint8_t>(ClockId::Overworld));
static_assert(static_cast<std::uint8_t>(DimensionId::Nether)
              == static_cast<std::uint8_t>(ClockId::Nether));
static_assert(static_cast<std::uint8_t>(DimensionId::End)
              == static_cast<std::uint8_t>(ClockId::End));

[[nodiscard]] constexpr ClockId clockOf(DimensionId dimension) {
    return static_cast<ClockId>(static_cast<std::uint8_t>(dimension));
}

// True when the dimension's clock is pinned (the Nether and the End): DIM2 holds
// such a clock at DimensionType::fixedTime rather than advancing the day.
[[nodiscard]] constexpr bool hasFixedTime(DimensionId dimension) {
    return dimensionType(dimension).fixedTime.has_value();
}

} // namespace mc::world
