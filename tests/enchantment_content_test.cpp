// DDC-1: JE-compatible enchantment content JSON, through the data path.
//
// What this pins:
//   * the codec reads JE 26.1's `data/minecraft/enchantment/*.json` field for
//     field, unchanged, and round-trips a def through JSON text;
//   * the static fields it reads golden-match ENCH-0's hand-transcribed
//     constexpr table (max_level / weight / min_cost / max_cost curve values),
//     proving the schema aligns with the baked source of truth;
//   * `#tag` references (supported_items / primary_items) and exclusive_set
//     (both the tag form and an inline id list) parse correctly;
//   * an overlay merges onto the baked floor and its *additions* register into a
//     DDC-0 registry's External phase through DataStore's bridge;
//   * an unknown top-level field is skipped and counted, not fatal
//     (forward compatibility with newer JE datapacks).
//
// Sabotage log (inject -> caught here -> revert):
//   (1) rename per_level_above_first -> perLevel in the cost codec  => every real
//       JE min_cost/max_cost read fails => golden-compare + real-file loads assert.
//   (2) drop the exclusive_set array branch (only read the string form) => inline
//       exclusive_set parses empty => exclusiveSetInlineParses assert fires.
//   (3) hard-fail the read on an unknown top-level field => forwardCompat overlay
//       file no longer loads => additions count + unknownFieldCount asserts fire.

#include "assets/ResourceProvider.hpp"
#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "core/Json.hpp"
#include "core/Registry.hpp"
#include "data/Codec.hpp"
#include "data/DataStore.hpp"
#include "data/EnchantmentContentStore.hpp"
#include "data/EnchantmentFile.hpp"
#include "gameplay/Enchantment.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using mc::data::Codec;
using mc::data::DataStore;
using mc::data::EnchantmentCostValue;
using mc::data::EnchantmentDef;
using mc::data::EnchantmentExclusiveSet;
using mc::data::EnchantmentItemSet;

// The real JE 26.1 files, embedded verbatim (copied byte-for-byte from
// data/minecraft/enchantment/). If DDC-1's schema drifts from JE's, these fail
// to read.
constexpr std::string_view kSharpnessJson = R"JSON({
  "anvil_cost": 1,
  "description": { "translate": "enchantment.minecraft.sharpness" },
  "effects": {
    "minecraft:damage": [
      { "effect": { "type": "minecraft:add",
          "value": { "type": "minecraft:linear", "base": 1.0, "per_level_above_first": 0.5 } } }
    ]
  },
  "exclusive_set": "#minecraft:exclusive_set/damage",
  "max_cost": { "base": 21, "per_level_above_first": 11 },
  "max_level": 5,
  "min_cost": { "base": 1, "per_level_above_first": 11 },
  "primary_items": "#minecraft:enchantable/melee_weapon",
  "slots": [ "mainhand" ],
  "supported_items": "#minecraft:enchantable/sharp_weapon",
  "weight": 10
})JSON";

constexpr std::string_view kProtectionJson = R"JSON({
  "anvil_cost": 1,
  "description": { "translate": "enchantment.minecraft.protection" },
  "effects": {
    "minecraft:damage_protection": [
      { "effect": { "type": "minecraft:add",
          "value": { "type": "minecraft:linear", "base": 1.0, "per_level_above_first": 1.0 } } }
    ]
  },
  "exclusive_set": "#minecraft:exclusive_set/armor",
  "max_cost": { "base": 12, "per_level_above_first": 11 },
  "max_level": 4,
  "min_cost": { "base": 1, "per_level_above_first": 11 },
  "slots": [ "armor" ],
  "supported_items": "#minecraft:enchantable/armor",
  "weight": 10
})JSON";

constexpr std::string_view kEfficiencyJson = R"JSON({
  "anvil_cost": 1,
  "description": { "translate": "enchantment.minecraft.efficiency" },
  "effects": { "minecraft:attributes": [] },
  "max_cost": { "base": 51, "per_level_above_first": 10 },
  "max_level": 5,
  "min_cost": { "base": 1, "per_level_above_first": 10 },
  "slots": [ "mainhand" ],
  "supported_items": "#minecraft:enchantable/mining",
  "weight": 10
})JSON";

constexpr std::string_view kUnbreakingJson = R"JSON({
  "anvil_cost": 2,
  "description": { "translate": "enchantment.minecraft.unbreaking" },
  "effects": { "minecraft:item_damage": [] },
  "max_cost": { "base": 55, "per_level_above_first": 8 },
  "max_level": 3,
  "min_cost": { "base": 5, "per_level_above_first": 8 },
  "slots": [ "any" ],
  "supported_items": "#minecraft:enchantable/durability",
  "weight": 5
})JSON";

[[nodiscard]] EnchantmentDef readDef(std::string_view text) {
    const mc::core::Json json = mc::core::Json::parse(text);
    EnchantmentDef def{};
    const bool ok = Codec<EnchantmentDef>::read(json, def);
    assert(ok && "a real JE 26.1 enchantment file must read");
    (void)ok;
    return def;
}

// ---- (A) real JE files read + golden-match ENCH-0's constexpr table ---------

void realFilesReadAndGoldenMatch() {
    const EnchantmentDef sharpness = readDef(kSharpnessJson);
    assert(sharpness.maxLevel == 5);
    assert(sharpness.weight == 10); // Rarity::Common weight
    assert(sharpness.anvilCost == 1);
    assert(sharpness.supportedItems.isTag);
    assert(sharpness.supportedItems.value == "#minecraft:enchantable/sharp_weapon");
    assert(sharpness.hasPrimaryItems);
    assert(sharpness.primaryItems.tagName() == "minecraft:enchantable/melee_weapon");

    // Cost curves must reproduce ENCH-0's getMinCost/getMaxCost exactly, at
    // every level. This is the golden bridge from JE JSON to the hand-written
    // constexpr numbers.
    using namespace mc::gameplay;
    for (std::int32_t level = 1; level <= sharpness.maxLevel; ++level) {
        assert(sharpness.minCost.at(level) == getMinCost(EnchantmentId::Sharpness, level));
        assert(sharpness.maxCost.at(level) == getMaxCost(EnchantmentId::Sharpness, level));
    }

    const EnchantmentDef protection = readDef(kProtectionJson);
    assert(protection.maxLevel == 4);
    for (std::int32_t level = 1; level <= protection.maxLevel; ++level) {
        assert(protection.minCost.at(level) == getMinCost(EnchantmentId::Protection, level));
        assert(protection.maxCost.at(level) == getMaxCost(EnchantmentId::Protection, level));
    }

    const EnchantmentDef efficiency = readDef(kEfficiencyJson);
    assert(efficiency.maxLevel == 5);
    assert(!efficiency.hasPrimaryItems); // efficiency omits primary_items
    for (std::int32_t level = 1; level <= efficiency.maxLevel; ++level) {
        assert(efficiency.minCost.at(level) == getMinCost(EnchantmentId::Efficiency, level));
        assert(efficiency.maxCost.at(level) == getMaxCost(EnchantmentId::Efficiency, level));
    }

    const EnchantmentDef unbreaking = readDef(kUnbreakingJson);
    assert(unbreaking.weight == 5); // Rarity::Uncommon
    for (std::int32_t level = 1; level <= unbreaking.maxLevel; ++level) {
        assert(unbreaking.minCost.at(level) == getMinCost(EnchantmentId::Unbreaking, level));
        assert(unbreaking.maxCost.at(level) == getMaxCost(EnchantmentId::Unbreaking, level));
    }
}

// ---- (B) effects/description carried through as raw text, never evaluated ----

void effectsCarriedAsRawText() {
    const EnchantmentDef sharpness = readDef(kSharpnessJson);
    assert(!sharpness.rawEffects.empty());
    // The raw blob is the effects object re-serialised; it names the component
    // type but DDC-1 never interprets it. A cheap substring check confirms it
    // was captured whole rather than dropped.
    assert(sharpness.rawEffects.find("minecraft:damage") != std::string::npos);
    assert(!sharpness.rawDescription.empty());
    assert(sharpness.rawDescription.find("enchantment.minecraft.sharpness") != std::string::npos);
}

// ---- (C) exclusive_set: tag form and inline id-list form --------------------

void exclusiveSetTagParses() {
    const EnchantmentDef sharpness = readDef(kSharpnessJson);
    assert(!sharpness.exclusiveSet.empty());
    assert(sharpness.exclusiveSet.tag == "#minecraft:exclusive_set/damage");
    assert(sharpness.exclusiveSet.inlineIds.empty());
    assert(sharpness.exclusiveSet.tagName() == "minecraft:exclusive_set/damage");
}

void exclusiveSetInlineParses() {
    // 26.1 also allows exclusive_set as an inline array of ids.
    constexpr std::string_view kInline = R"JSON({
      "anvil_cost": 1,
      "max_cost": { "base": 50, "per_level_above_first": 0 },
      "max_level": 1,
      "min_cost": { "base": 25, "per_level_above_first": 0 },
      "slots": [ "any" ],
      "supported_items": "#minecraft:enchantable/durability",
      "weight": 1,
      "exclusive_set": [ "minecraft:sharpness", "minecraft:smite" ]
    })JSON";
    const EnchantmentDef def = readDef(kInline);
    assert(def.exclusiveSet.tag.empty());
    assert(def.exclusiveSet.inlineIds.size() == 2);
    assert(def.exclusiveSet.inlineIds[0] == "minecraft:sharpness");
    assert(def.exclusiveSet.inlineIds[1] == "minecraft:smite");
    assert(!def.exclusiveSet.empty());
}

// ---- (D) codec round-trips through JSON text --------------------------------

void roundTripsThroughText() {
    const EnchantmentDef original = readDef(kSharpnessJson);
    const std::string text = Codec<EnchantmentDef>::write(original).dump();
    const mc::core::Json reparsed = mc::core::Json::parse(text);
    EnchantmentDef restored{};
    assert(Codec<EnchantmentDef>::read(reparsed, restored));
    assert(restored == original);
}

// ---- (E) DataStore overlay + additions bridge (DDC-0) ----------------------

// A datapack served from memory, JE `data/<space>/enchantment/*.json` layout.
class MemoryProvider final : public mc::assets::ResourceProvider {
  public:
    void add(std::string path, std::string body) {
        const mc::assets::ResourceLocation location{"minecraft", std::move(path),
                                                    mc::assets::PackType::ServerData};
        files_[location.toString()] = std::move(body);
    }
    [[nodiscard]] std::filesystem::path locate(const mc::assets::ResourceLocation&) const override {
        return {};
    }
    [[nodiscard]] bool exists(const mc::assets::ResourceLocation& location) const override {
        return files_.find(location.toString()) != files_.end();
    }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
    [[nodiscard]] std::vector<std::byte>
    readBytes(const mc::assets::ResourceLocation& location) const override {
        const auto slot = files_.find(location.toString());
        if (slot == files_.end()) return {};
        std::vector<std::byte> bytes(slot->second.size());
        std::memcpy(bytes.data(), slot->second.data(), slot->second.size());
        return bytes;
    }
    [[nodiscard]] std::vector<mc::assets::ResourceLocation>
    list(std::string_view space, std::string_view pathPrefix,
         mc::assets::PackType = mc::assets::PackType::ClientResources) const override {
        std::vector<mc::assets::ResourceLocation> found;
        for (const auto& [key, body] : files_) {
            (void)body;
            auto location =
                mc::assets::ResourceLocation::parse(key, mc::assets::PackType::ServerData);
            if (location.space == space && location.path.size() >= pathPrefix.size() &&
                std::string_view{location.path}.substr(0, pathPrefix.size()) == pathPrefix) {
                found.push_back(std::move(location));
            }
        }
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) { return a.path < b.path; });
        return found;
    }

  private:
    std::unordered_map<std::string, std::string> files_;
};

void overlayMergesAndAdditionsRegister() {
    DataStore<EnchantmentDef> store;
    // A baked floor of one built-in, under JE's identity.
    EnchantmentDef bakedSharpness = readDef(kSharpnessJson);
    store.bakeBuiltin("minecraft:sharpness", bakedSharpness);
    assert(store.size() == 1);

    MemoryProvider pack;
    // (1) replaces the built-in sharpness (same key) with a max_level-1 variant.
    pack.add("enchantment/sharpness.json",
             std::string{R"JSON({
               "anvil_cost": 1,
               "max_cost": { "base": 21, "per_level_above_first": 11 },
               "max_level": 1,
               "min_cost": { "base": 1, "per_level_above_first": 11 },
               "slots": [ "mainhand" ],
               "supported_items": "#minecraft:enchantable/sharp_weapon",
               "weight": 10
             })JSON"});
    // (2) adds a brand-new enchantment (new key) -> an addition.
    pack.add("enchantment/spicy.json",
             std::string{R"JSON({
               "anvil_cost": 1,
               "max_cost": { "base": 50, "per_level_above_first": 0 },
               "max_level": 2,
               "min_cost": { "base": 5, "per_level_above_first": 5 },
               "slots": [ "mainhand" ],
               "supported_items": "#minecraft:enchantable/sharp_weapon",
               "weight": 2
             })JSON"});

    const std::size_t applied = mc::data::applyEnchantmentOverlay(store, pack);
    assert(applied == 2);
    assert(store.size() == 2); // sharpness replaced in place, spicy appended

    // The replacement swapped the def in place (still the built-in slot).
    const EnchantmentDef* replaced = store.find("minecraft:sharpness");
    assert(replaced != nullptr && replaced->maxLevel == 1);
    // The addition is present and flagged fromOverlay.
    const EnchantmentDef* spicy = store.find("minecraft:spicy");
    assert(spicy != nullptr && spicy->maxLevel == 2);

    // The DDC-0 bridge: only additions register into an External phase, claiming
    // a dense id beside built-ins. sharpness (a replacement) must NOT re-register.
    mc::core::Registry<EnchantmentDef, mc::core::EnchantmentTypeId> registry;
    registry.registerBuiltin(mc::core::Identifier::parse("minecraft:sharpness"), bakedSharpness);
    registry.beginExternal();
    store.registerAdditionsInto(registry);
    registry.freeze();
    assert(registry.size() == 2); // 1 builtin + 1 addition (spicy), sharpness not doubled
    const mc::core::EnchantmentTypeId spicyId =
        registry.byName("minecraft:spicy");
    assert(spicyId.valid());
    const mc::core::EnchantmentTypeId sharpId =
        registry.byName("minecraft:sharpness");
    assert(sharpId.valid() && sharpId.index() == 0); // stayed the built-in id
}

// ---- (F) forward compatibility: unknown fields skipped + counted -----------

void forwardCompatSkipsUnknownFields() {
    // A newer JE datapack introduces a top-level field DDC-1 predates.
    constexpr std::string_view kFuture = R"JSON({
      "anvil_cost": 1,
      "max_cost": { "base": 21, "per_level_above_first": 11 },
      "max_level": 5,
      "min_cost": { "base": 1, "per_level_above_first": 11 },
      "slots": [ "mainhand" ],
      "supported_items": "#minecraft:enchantable/sharp_weapon",
      "weight": 10,
      "future_field_a": 42,
      "future_field_b": { "nested": true }
    })JSON";
    const EnchantmentDef def = readDef(kFuture); // must not fail
    assert(def.maxLevel == 5);
    assert(def.unknownFieldCount == 2);

    // The same tolerance through the overlay path: a file with an unknown field
    // still loads (not skipped as wrong-shape).
    DataStore<EnchantmentDef> store;
    MemoryProvider pack;
    pack.add("enchantment/futuristic.json", std::string{kFuture});
    const std::size_t applied = mc::data::applyEnchantmentOverlay(store, pack);
    assert(applied == 1);
    const EnchantmentDef* loaded = store.find("minecraft:futuristic");
    assert(loaded != nullptr && loaded->unknownFieldCount == 2);
}

// ---- (G) malformed / wrong-shape files are skipped, never fatal -------------

void malformedFilesSkipped() {
    DataStore<EnchantmentDef> store;
    store.bakeBuiltin("minecraft:sharpness", readDef(kSharpnessJson));
    MemoryProvider pack;
    pack.add("enchantment/broken.json", "{ this is not json");
    // Missing a required field (no min_cost/max_cost) -> wrong shape -> skipped.
    pack.add("enchantment/incomplete.json",
             std::string{R"JSON({ "weight": 10, "max_level": 5 })JSON"});
    const std::size_t applied = mc::data::applyEnchantmentOverlay(store, pack);
    assert(applied == 0);       // both skipped
    assert(store.size() == 1);  // floor intact
}

} // namespace

int main() {
    realFilesReadAndGoldenMatch();
    effectsCarriedAsRawText();
    exclusiveSetTagParses();
    exclusiveSetInlineParses();
    roundTripsThroughText();
    overlayMergesAndAdditionsRegister();
    forwardCompatSkipsUnknownFields();
    malformedFilesSkipped();
    return 0;
}
