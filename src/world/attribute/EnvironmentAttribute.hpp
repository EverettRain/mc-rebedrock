#pragma once

// 26.1's environment attributes (`world/attribute/EnvironmentAttributes.java`).
//
// Up to 1.21 a biome's visuals and its atmosphere lived in BiomeSpecialEffects,
// and the rules that vary by dimension were hard-coded checks against the
// dimension type. 26.1 pulled both into one layered attribute system:
// BiomeSpecialEffects is down to five colour fields (BM-1 has those), and fog,
// sky, clouds, lighting tints, music, ambient sounds and a couple of dozen
// gameplay rules are attributes that a dimension sets and a biome may override.
//
// The Java side is a HashMap of EnvironmentAttribute<?> to a boxed entry, read
// through a generic getter. That shape costs a hash and a pointer chase per
// query, on a path that runs per frame (fog colour) and per tick (rules). Here
// each layer is a 64-bit presence bitmap plus a dense array indexed by the
// attribute's own id, so a query is one bit test and one array subscript, with
// no allocation and no hashing anywhere. There are 47 attributes, so one word of
// bitmap covers all of them.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::world::attribute {

// The attribute ids, in the order EnvironmentAttributes declares them. The
// namespace prefixes (`visual/`, `audio/`, `gameplay/`) are vanilla's own and
// are kept in the id table below, because a data pack names attributes by path.
enum class EnvAttr : std::uint8_t {
    // visual/
    FogColor,
    FogStartDistance,
    FogEndDistance,
    SkyFogEndDistance,
    CloudFogEndDistance,
    WaterFogColor,
    WaterFogStartDistance,
    WaterFogEndDistance,
    SkyColor,
    SunriseSunsetColor,
    CloudColor,
    CloudHeight,
    SunAngle,
    MoonAngle,
    StarAngle,
    MoonPhase,
    StarBrightness,
    BlockLightTint,
    SkyLightColor,
    SkyLightFactor,
    NightVisionColor,
    AmbientLightColor,
    DefaultDripstoneParticle,
    // audio/
    BackgroundMusic,
    MusicVolume,
    AmbientSounds,
    FireflyBushSounds,
    // gameplay/
    SkyLightLevel,
    CanStartRaid,
    WaterEvaporates,
    BedRule,
    RespawnAnchorWorks,
    NetherPortalSpawnsPiglin,
    FastLava,
    IncreasedFireBurnout,
    EyeblossomOpen,
    TurtleEggHatchChance,
    PiglinsZombify,
    SnowGolemMelts,
    CreakingActive,
    SurfaceSlimeSpawnChance,
    CatWakingUpGiftChance,
    BeesStayInHive,
    MonstersBurn,
    CanPillagerPatrolSpawn,
    VillagerActivity,
    BabyVillagerActivity,
    Count,
};

inline constexpr std::size_t kAttrCount = static_cast<std::size_t>(EnvAttr::Count);
// One machine word of bitmap covers every attribute. If vanilla grows past 64,
// the bitmap becomes an array and `has`/`set` index into it — the rest is
// unchanged, which is why this is an assert rather than a design constraint.
static_assert(kAttrCount <= 64U, "the presence bitmap is a single uint64");

// What kind of value an attribute carries. Vanilla's AttributeTypes, reduced to
// what the storage needs to know: everything is 32 bits, and the type only says
// how to read those bits back.
enum class AttrType : std::uint8_t {
    // RGB_COLOR / ARGB_COLOR, stored as vanilla stores them (a signed int).
    Color,
    // FLOAT / ANGLE_DEGREES.
    Float,
    Boolean,
    // A small enum: MOON_PHASE, BED_RULE, TRI_STATE, ACTIVITY.
    Enum,
    // PARTICLE / BACKGROUND_MUSIC / AMBIENT_SOUNDS — values too big for 32 bits.
    // They are stored as an index into a side table the consumer owns, so the
    // value layer stays one uniform width. Index 0 always means "none".
    Reference,
};

// One attribute's value: 32 bits, whatever the type. Keeping every value the
// same width is what lets a layer be a flat array rather than a variant.
struct AttrValue final {
    std::int32_t bits = 0;

    [[nodiscard]] static constexpr AttrValue ofColor(std::int32_t color) { return {color}; }
    [[nodiscard]] static constexpr AttrValue ofFloat(float value) {
        return {std::bit_cast<std::int32_t>(value)};
    }
    [[nodiscard]] static constexpr AttrValue ofBool(bool value) { return {value ? 1 : 0}; }
    [[nodiscard]] static constexpr AttrValue ofEnum(std::int32_t value) { return {value}; }
    [[nodiscard]] static constexpr AttrValue ofReference(std::uint32_t index) {
        return {static_cast<std::int32_t>(index)};
    }

    [[nodiscard]] constexpr std::int32_t asColor() const { return bits; }
    [[nodiscard]] constexpr float asFloat() const { return std::bit_cast<float>(bits); }
    [[nodiscard]] constexpr bool asBool() const { return bits != 0; }
    [[nodiscard]] constexpr std::int32_t asEnum() const { return bits; }
    [[nodiscard]] constexpr std::uint32_t asReference() const {
        return static_cast<std::uint32_t>(bits);
    }

    [[nodiscard]] constexpr bool operator==(const AttrValue&) const = default;
};

// The small enums attributes carry, with vanilla's own ordinals.
enum class MoonPhase : std::int32_t {
    FullMoon = 0,
    WaningGibbous,
    ThirdQuarter,
    WaningCrescent,
    NewMoon,
    WaxingCrescent,
    FirstQuarter,
    WaxingGibbous,
};
enum class BedRule : std::int32_t { CanSleepWhenDark = 0, CanSleepAlways, Explodes };
enum class TriState : std::int32_t { Default = 0, True, False };

// One layer of overrides: which attributes it sets, and their values.
//
// A layer is a POD so the built-in dimension and biome layers are constexpr and
// land in .rodata. A data pack overlay builds one at load time the same way.
struct EnvAttrLayer final {
    std::uint64_t present = 0U;
    std::array<AttrValue, kAttrCount> values{};

    [[nodiscard]] constexpr bool has(EnvAttr attribute) const {
        return (present & (1ULL << static_cast<std::uint64_t>(attribute))) != 0ULL;
    }
    [[nodiscard]] constexpr AttrValue at(EnvAttr attribute) const {
        return values[static_cast<std::size_t>(attribute)];
    }
    constexpr EnvAttrLayer& set(EnvAttr attribute, AttrValue value) {
        present |= 1ULL << static_cast<std::uint64_t>(attribute);
        values[static_cast<std::size_t>(attribute)] = value;
        return *this;
    }
};

// The layers in effect at a position, innermost first. A null layer is simply
// absent — the caller does not build an empty one to fill a slot.
//
// `local` is the slot vanilla fills from a local effect (a dripstone cave's
// particle, a potion's fog). Nothing produces one yet; the slot exists so adding
// one later is a value change rather than a signature change.
struct EnvAttrStack final {
    const EnvAttrLayer* local = nullptr;
    const EnvAttrLayer* biome = nullptr;
    const EnvAttrLayer* dimension = nullptr;
};

// Every attribute's type and its vanilla default, in enum order.
struct AttrSpec final {
    std::string_view id;
    AttrType type;
    AttrValue defaultValue;
};

// Vanilla's own defaults, from EnvironmentAttributes' builders. A colour default
// like -16448205 is the signed int vanilla writes; it is stored, not reinterpreted.
inline constexpr std::array<AttrSpec, kAttrCount> kAttrSpecs{{
    {"visual/fog_color", AttrType::Color, AttrValue::ofColor(0)},
    {"visual/fog_start_distance", AttrType::Float, AttrValue::ofFloat(0.0F)},
    {"visual/fog_end_distance", AttrType::Float, AttrValue::ofFloat(1024.0F)},
    {"visual/sky_fog_end_distance", AttrType::Float, AttrValue::ofFloat(512.0F)},
    {"visual/cloud_fog_end_distance", AttrType::Float, AttrValue::ofFloat(2048.0F)},
    {"visual/water_fog_color", AttrType::Color, AttrValue::ofColor(-16448205)},
    {"visual/water_fog_start_distance", AttrType::Float, AttrValue::ofFloat(-8.0F)},
    {"visual/water_fog_end_distance", AttrType::Float, AttrValue::ofFloat(96.0F)},
    {"visual/sky_color", AttrType::Color, AttrValue::ofColor(0)},
    {"visual/sunrise_sunset_color", AttrType::Color, AttrValue::ofColor(0)},
    {"visual/cloud_color", AttrType::Color, AttrValue::ofColor(0)},
    {"visual/cloud_height", AttrType::Float, AttrValue::ofFloat(192.33F)},
    {"visual/sun_angle", AttrType::Float, AttrValue::ofFloat(0.0F)},
    {"visual/moon_angle", AttrType::Float, AttrValue::ofFloat(0.0F)},
    {"visual/star_angle", AttrType::Float, AttrValue::ofFloat(0.0F)},
    {"visual/moon_phase", AttrType::Enum,
     AttrValue::ofEnum(static_cast<std::int32_t>(MoonPhase::FullMoon))},
    {"visual/star_brightness", AttrType::Float, AttrValue::ofFloat(0.0F)},
    {"visual/block_light_tint", AttrType::Color, AttrValue::ofColor(-10100)},
    {"visual/sky_light_color", AttrType::Color, AttrValue::ofColor(-1)},
    {"visual/sky_light_factor", AttrType::Float, AttrValue::ofFloat(1.0F)},
    {"visual/night_vision_color", AttrType::Color, AttrValue::ofColor(-6710887)},
    {"visual/ambient_light_color", AttrType::Color, AttrValue::ofColor(-16777216)},
    {"visual/default_dripstone_particle", AttrType::Reference, AttrValue::ofReference(0U)},
    {"audio/background_music", AttrType::Reference, AttrValue::ofReference(0U)},
    {"audio/music_volume", AttrType::Float, AttrValue::ofFloat(1.0F)},
    {"audio/ambient_sounds", AttrType::Reference, AttrValue::ofReference(0U)},
    {"audio/firefly_bush_sounds", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/sky_light_level", AttrType::Float, AttrValue::ofFloat(15.0F)},
    {"gameplay/can_start_raid", AttrType::Boolean, AttrValue::ofBool(true)},
    {"gameplay/water_evaporates", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/bed_rule", AttrType::Enum,
     AttrValue::ofEnum(static_cast<std::int32_t>(BedRule::CanSleepWhenDark))},
    {"gameplay/respawn_anchor_works", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/nether_portal_spawns_piglin", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/fast_lava", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/increased_fire_burnout", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/eyeblossom_open", AttrType::Enum,
     AttrValue::ofEnum(static_cast<std::int32_t>(TriState::Default))},
    {"gameplay/turtle_egg_hatch_chance", AttrType::Float, AttrValue::ofFloat(0.002F)},
    {"gameplay/piglins_zombify", AttrType::Boolean, AttrValue::ofBool(true)},
    {"gameplay/snow_golem_melts", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/creaking_active", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/surface_slime_spawn_chance", AttrType::Float, AttrValue::ofFloat(0.0F)},
    {"gameplay/cat_waking_up_gift_chance", AttrType::Float, AttrValue::ofFloat(0.0F)},
    {"gameplay/bees_stay_in_hive", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/monsters_burn", AttrType::Boolean, AttrValue::ofBool(false)},
    {"gameplay/can_pillager_patrol_spawn", AttrType::Boolean, AttrValue::ofBool(true)},
    // ACTIVITY is a registry key in vanilla (Activity.IDLE); nothing here reads
    // it yet, so it is a reference with 0 meaning "the default activity".
    {"gameplay/villager_activity", AttrType::Reference, AttrValue::ofReference(0U)},
    {"gameplay/baby_villager_activity", AttrType::Reference, AttrValue::ofReference(0U)},
}};

[[nodiscard]] constexpr const AttrSpec& attrSpec(EnvAttr attribute) {
    return kAttrSpecs[static_cast<std::size_t>(attribute)];
}

// Resolves an attribute through the stack: innermost layer that sets it wins,
// and the vanilla default answers when none does.
//
// Three bit tests and one subscript in the worst case, no branch on the value's
// type and nothing to allocate — this runs per frame for fog and per tick for
// the rules.
[[nodiscard]] constexpr AttrValue resolve(EnvAttr attribute, const EnvAttrStack& stack) {
    if (stack.local != nullptr && stack.local->has(attribute)) {
        return stack.local->at(attribute);
    }
    if (stack.biome != nullptr && stack.biome->has(attribute)) {
        return stack.biome->at(attribute);
    }
    if (stack.dimension != nullptr && stack.dimension->has(attribute)) {
        return stack.dimension->at(attribute);
    }
    return attrSpec(attribute).defaultValue;
}

// Typed readers. They exist so a caller states which type it expects and a
// mismatch is caught in a test rather than read as garbage — the one thing the
// uniform-width storage gives up compared with vanilla's generic map.
[[nodiscard]] constexpr std::int32_t resolveColor(EnvAttr attribute, const EnvAttrStack& stack) {
    return resolve(attribute, stack).asColor();
}
[[nodiscard]] constexpr float resolveFloat(EnvAttr attribute, const EnvAttrStack& stack) {
    return resolve(attribute, stack).asFloat();
}
[[nodiscard]] constexpr bool resolveBool(EnvAttr attribute, const EnvAttrStack& stack) {
    return resolve(attribute, stack).asBool();
}
[[nodiscard]] constexpr std::int32_t resolveEnum(EnvAttr attribute, const EnvAttrStack& stack) {
    return resolve(attribute, stack).asEnum();
}
[[nodiscard]] constexpr std::uint32_t resolveReference(EnvAttr attribute,
                                                       const EnvAttrStack& stack) {
    return resolve(attribute, stack).asReference();
}

// ARGB.color: the packed form every colour attribute stores, as a signed int
// because that is what vanilla's tables hold.
[[nodiscard]] constexpr std::int32_t packArgb(int alpha, int red, int green, int blue) {
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(alpha & 0xFF) << 24U) |
                                     (static_cast<std::uint32_t>(red & 0xFF) << 16U) |
                                     (static_cast<std::uint32_t>(green & 0xFF) << 8U) |
                                     static_cast<std::uint32_t>(blue & 0xFF));
}

// ARGB.colorFromFloat / ARGB.white: channels given as 0..1.
[[nodiscard]] constexpr std::int32_t packArgbFromFloat(float alpha, float red, float green,
                                                       float blue) {
    const auto channel = [](float value) { return static_cast<int>(value * 255.0F); };
    return packArgb(channel(alpha), channel(red), channel(green), channel(blue));
}

// Mth.hsvToArgb, verbatim — the switch, the truncating casts and the clamp all
// matter, because the sky colour is derived from it and a rounding difference is
// a visibly different sky.
[[nodiscard]] constexpr std::int32_t hsvToArgb(float hue, float saturation, float value,
                                               int alpha) {
    const int sextant = static_cast<int>(hue * 6.0F) % 6;
    const float fraction = hue * 6.0F - static_cast<float>(sextant);
    const float p = value * (1.0F - saturation);
    const float q = value * (1.0F - fraction * saturation);
    const float t = value * (1.0F - (1.0F - fraction) * saturation);
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    switch (sextant) {
    case 0: red = value; green = t; blue = p; break;
    case 1: red = q; green = value; blue = p; break;
    case 2: red = p; green = value; blue = t; break;
    case 3: red = p; green = q; blue = value; break;
    case 4: red = t; green = p; blue = value; break;
    default: red = value; green = p; blue = q; break;
    }
    const auto channel = [](float component) {
        const int scaled = static_cast<int>(component * 255.0F);
        return scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled);
    };
    return packArgb(alpha, channel(red), channel(green), channel(blue));
}

// OverworldBiomes.calculateSkyColor: the overworld's sky is a hue ramp on the
// biome's temperature — colder biomes sit slightly bluer and less saturated.
[[nodiscard]] constexpr std::int32_t calculateSkyColor(float temperature) {
    float scaled = temperature / 3.0F;
    scaled = scaled < -1.0F ? -1.0F : (scaled > 1.0F ? 1.0F : scaled);
    return hsvToArgb(0.62222224F - scaled * 0.05F, 0.5F + scaled * 0.1F, 1.0F, 0) |
           static_cast<std::int32_t>(0xFF000000U);  // ARGB.opaque
}

// Resolves a data pack's attribute path (`visual/fog_color`) to its id, or Count
// when the pack names one this build does not carry.
[[nodiscard]] constexpr EnvAttr attrFromId(std::string_view id) {
    for (std::size_t index = 0; index < kAttrCount; ++index) {
        if (kAttrSpecs[index].id == id) {
            return static_cast<EnvAttr>(index);
        }
    }
    return EnvAttr::Count;
}

} // namespace mc::world::attribute
