#include "world/Dimension.hpp"
#include "world/attribute/EnvironmentAttribute.hpp"
#include "world/gen/Biome.hpp"

#include <stdexcept>
#include <string>

// BM-3a/b: 26.1's environment attributes, and the two built-in layers that set
// them.
//
// 26.1 moved fog, sky, the light colours, music, ambient sounds and about twenty
// gameplay rules out of BiomeSpecialEffects and the hard-coded dimension checks
// into one layered attribute system. This build had none of that layer: the
// visual half did not exist and the gameplay half was a handful of booleans on
// DimensionType, read through `if (dimension == Nether)` at each use.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"environment_attribute_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::world::DimensionId;
using mc::world::dimensionAttributes;
using mc::world::dimensionType;
using mc::world::attribute::AttrType;
using mc::world::attribute::AttrValue;
using mc::world::attribute::BedRule;
using mc::world::attribute::EnvAttr;
using mc::world::attribute::EnvAttrLayer;
using mc::world::attribute::EnvAttrStack;
using mc::world::gen::Biome;
using mc::world::gen::biomeAttributes;

// Innermost layer that sets an attribute wins; the vanilla default answers when
// no layer does.
void checkLayerOrder() {
    EnvAttrLayer dimension{};
    EnvAttrLayer biome{};
    EnvAttrLayer local{};
    dimension.set(EnvAttr::FogColor, AttrValue::ofColor(0x111111));
    biome.set(EnvAttr::FogColor, AttrValue::ofColor(0x222222));
    local.set(EnvAttr::FogColor, AttrValue::ofColor(0x333333));

    REQUIRE(resolveColor(EnvAttr::FogColor, EnvAttrStack{}) == 0);  // the default
    REQUIRE(resolveColor(EnvAttr::FogColor, EnvAttrStack{nullptr, nullptr, &dimension}) ==
            0x111111);
    REQUIRE(resolveColor(EnvAttr::FogColor, EnvAttrStack{nullptr, &biome, &dimension}) == 0x222222);
    REQUIRE(resolveColor(EnvAttr::FogColor, EnvAttrStack{&local, &biome, &dimension}) == 0x333333);

    // An attribute a layer does not set falls through it, rather than being
    // read as a zero the layer happens to hold.
    REQUIRE(!biome.has(EnvAttr::CloudHeight));
    REQUIRE(resolveFloat(EnvAttr::CloudHeight, EnvAttrStack{nullptr, &biome, &dimension}) ==
            192.33F);
}

// Every attribute round-trips through the width its type declares.
void checkValueRoundTrip() {
    for (std::size_t index = 0; index < mc::world::attribute::kAttrCount; ++index) {
        const auto attribute = static_cast<EnvAttr>(index);
        const auto& spec = mc::world::attribute::attrSpec(attribute);
        REQUIRE(!spec.id.empty());
        REQUIRE(mc::world::attribute::attrFromId(spec.id) == attribute);
        EnvAttrLayer layer{};
        switch (spec.type) {
        case AttrType::Color:
            layer.set(attribute, AttrValue::ofColor(-12345));
            REQUIRE(resolveColor(attribute, EnvAttrStack{&layer, nullptr, nullptr}) == -12345);
            break;
        case AttrType::Float:
            layer.set(attribute, AttrValue::ofFloat(-3.5F));
            REQUIRE(resolveFloat(attribute, EnvAttrStack{&layer, nullptr, nullptr}) == -3.5F);
            break;
        case AttrType::Boolean:
            layer.set(attribute, AttrValue::ofBool(true));
            REQUIRE(resolveBool(attribute, EnvAttrStack{&layer, nullptr, nullptr}));
            break;
        case AttrType::Enum:
            layer.set(attribute, AttrValue::ofEnum(3));
            REQUIRE(resolveEnum(attribute, EnvAttrStack{&layer, nullptr, nullptr}) == 3);
            break;
        case AttrType::Reference:
            layer.set(attribute, AttrValue::ofReference(7U));
            REQUIRE(resolveReference(attribute, EnvAttrStack{&layer, nullptr, nullptr}) == 7U);
            break;
        }
    }
    REQUIRE(mc::world::attribute::attrFromId("visual/not_an_attribute") == EnvAttr::Count);
}

// The dimension layer and DimensionType's own booleans state the same facts
// while both exist. This is the guard against them drifting apart — the layer is
// the 26.1 shape, the fields are what the rest of the codebase still reads.
void checkDimensionLayerAgreesWithFields() {
    for (const auto id : {DimensionId::Overworld, DimensionId::Nether, DimensionId::End}) {
        const auto& type = dimensionType(id);
        const EnvAttrStack stack{nullptr, nullptr, &dimensionAttributes(id)};
        const auto bedRule = static_cast<BedRule>(resolveEnum(EnvAttr::BedRule, stack));
        REQUIRE(type.bedWorks == (bedRule == BedRule::CanSleepWhenDark));
        REQUIRE(type.respawnAnchorWorks == resolveBool(EnvAttr::RespawnAnchorWorks, stack));
        // piglinSafe is the negation: a safe dimension is one where they do not
        // zombify.
        REQUIRE(type.piglinSafe == !resolveBool(EnvAttr::PiglinsZombify, stack));
        // ultrawarm is two attributes in 26.1.
        REQUIRE(type.ultrawarm == resolveBool(EnvAttr::WaterEvaporates, stack));
        REQUIRE(type.ultrawarm == resolveBool(EnvAttr::FastLava, stack));
    }
}

// Values against 26.1's DimensionTypes.java.
void checkDimensionValues() {
    const EnvAttrStack overworld{nullptr, nullptr, &dimensionAttributes(DimensionId::Overworld)};
    REQUIRE(resolveColor(EnvAttr::FogColor, overworld) == -4138753);
    REQUIRE(resolveFloat(EnvAttr::CloudHeight, overworld) == 192.33F);
    // ARGB.white(0.8F)
    REQUIRE(resolveColor(EnvAttr::CloudColor, overworld) == -855638017);
    REQUIRE(resolveBool(EnvAttr::NetherPortalSpawnsPiglin, overworld));

    const EnvAttrStack nether{nullptr, nullptr, &dimensionAttributes(DimensionId::Nether)};
    REQUIRE(resolveFloat(EnvAttr::FogStartDistance, nether) == 10.0F);
    REQUIRE(resolveFloat(EnvAttr::FogEndDistance, nether) == 96.0F);
    REQUIRE(resolveFloat(EnvAttr::SkyLightLevel, nether) == 4.0F);
    REQUIRE(resolveFloat(EnvAttr::SkyLightFactor, nether) == 0.0F);
    REQUIRE(resolveBool(EnvAttr::SnowGolemMelts, nether));
    REQUIRE(!resolveBool(EnvAttr::CanStartRaid, nether));

    const EnvAttrStack end{nullptr, nullptr, &dimensionAttributes(DimensionId::End)};
    REQUIRE(resolveColor(EnvAttr::FogColor, end) == -15199464);
    REQUIRE(resolveColor(EnvAttr::SkyColor, end) == -16777216);
    REQUIRE(resolveColor(EnvAttr::AmbientLightColor, end) == -12630209);
}

// The overworld biomes carry the sky colour their temperature derives; the
// nether biomes carry their own fog; the end biomes take everything from the
// dimension, as in 26.1.
void checkBiomeLayer() {
    // OverworldBiomes.calculateSkyColor(0.8F) is vanilla's plains sky, 0x78A7FF.
    const EnvAttrStack plains{nullptr, &biomeAttributes(Biome::Plains),
                              &dimensionAttributes(DimensionId::Overworld)};
    REQUIRE(resolveColor(EnvAttr::SkyColor, plains) ==
            static_cast<std::int32_t>(0xFF78A7FFU));
    // A colder biome sits at a different hue, so the ramp is actually applied.
    const EnvAttrStack snowy{nullptr, &biomeAttributes(Biome::SnowyPlains),
                             &dimensionAttributes(DimensionId::Overworld)};
    REQUIRE(resolveColor(EnvAttr::SkyColor, snowy) != resolveColor(EnvAttr::SkyColor, plains));

    // The nether biomes override the fog; the dimension supplies its distances.
    const EnvAttrStack wastes{nullptr, &biomeAttributes(Biome::NetherWastes),
                              &dimensionAttributes(DimensionId::Nether)};
    REQUIRE(resolveColor(EnvAttr::FogColor, wastes) == -13432824);
    REQUIRE(resolveFloat(EnvAttr::FogEndDistance, wastes) == 96.0F);
    const EnvAttrStack soulSand{nullptr, &biomeAttributes(Biome::SoulSandValley),
                                &dimensionAttributes(DimensionId::Nether)};
    REQUIRE(resolveColor(EnvAttr::FogColor, soulSand) == -14989499);
    // A nether biome has no sky colour of its own: vanilla builds them without
    // baseBiome, so the dimension answers.
    REQUIRE(!biomeAttributes(Biome::NetherWastes).has(EnvAttr::SkyColor));

    // The end biomes set nothing at all.
    for (const auto biome : {Biome::TheEnd, Biome::EndHighlands, Biome::EndMidlands,
                             Biome::EndBarrens, Biome::SmallEndIslands}) {
        REQUIRE(biomeAttributes(biome).present == 0ULL);
    }
    const EnvAttrStack theEnd{nullptr, &biomeAttributes(Biome::TheEnd),
                              &dimensionAttributes(DimensionId::End)};
    REQUIRE(resolveColor(EnvAttr::SkyColor, theEnd) == -16777216);
}

} // namespace

int main() {
    checkLayerOrder();
    checkValueRoundTrip();
    checkDimensionLayerAgreesWithFields();
    checkDimensionValues();
    checkBiomeLayer();
    return 0;
}
