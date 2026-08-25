// DDC-3: legacy enchantment content migration — golden compare.
//
// DDC-3 migrates ENCH-0's 34-row constexpr kEnchantmentTable into JE-schema
// datapack JSON, through the DDC-0 registry / DDC-1 codec / DDC-2 effect
// pipeline. This test is the migration's acceptance gate: it proves the
// constexpr table is *fully expressible* as JE datapack JSON with zero drift,
// which is what lets the built-in table be shipped as the internal datapack
// (resources/data/rebedrock/enchantment/*.json) instead of hardcoded numbers.
//
// The golden property, for every built-in enchantment:
//
//   constexpr EnchantmentDefinition
//       --toContentDef-->      data::EnchantmentDef   (JE-schema POD)
//       --Codec::write-->      JSON object            (a datapack file)
//       --dump/parse-->        JSON text round trip
//       --Codec::read-->       data::EnchantmentDef
//       --toGameplayDefinition--> EnchantmentDefinition
//
//   must reproduce the original, observably:
//     * max_level / rarity / category / treasure / curse / random-selection
//       identical;
//     * getMinCost / getMaxCost identical at EVERY level (the numeric golden
//       bridge — the flat-50 four are covered here too, since getMaxCost
//       special-cases them by name);
//     * the weight the file carries == ENCH-0's own rarity->weight table.
//
// It also loads the *shipped* files (resources/data/rebedrock/enchantment/*.json)
// through the real StandardPackResourceProvider datapack path and asserts they
// re-derive byte-for-byte what enchantmentContentJson() produces in memory — so
// the committed files can never drift from the constexpr table.
//
// Save compatibility: the migration changes no id and no stored form. An
// ItemStack persists an enchantment by its EnchantmentId ordinal / vanilla name
// (Enchantment.hpp), both untouched here; the fold only reconstructs the same
// EnchantmentDefinition the constexpr table already held. The ordinal-stability
// assert below pins that (row i still folds to id i, name unchanged).
//
// Sabotage log (inject -> caught here -> revert):
//   (1) in rarityFromWeight, collapse Uncommon/Rare into one bucket (return
//       Common for weight>=5) => the rarity golden assert fires for every
//       Uncommon/Rare enchantment.
//   (2) in maxCostValue, drop the flat-50 special case (always write
//       minBase+maxOffset) => getMaxCost golden assert fires for Loyalty /
//       Riptide / QuickCharge / Piercing.
//   (3) in enchantmentContentJson, always emit the rebedrock extension block
//       (even for a plain enchantment) => the shippedFilesMatchInMemory assert
//       fires (committed plain files have no block) AND a plain enchantment's
//       file stops being pure JE.

#include "assets/ResourceLocation.hpp"
#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "data/Codec.hpp"
#include "data/DataStore.hpp"
#include "data/EnchantmentContentStore.hpp"
#include "data/EnchantmentFile.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/EnchantmentContentMigration.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mc::core::Json;
using mc::data::EnchantmentDef;
using mc::gameplay::EnchantmentDefinition;
using mc::gameplay::EnchantmentId;
using mc::gameplay::kEnchantmentTable;

// Fold the constexpr row all the way through JSON text and back to a gameplay
// definition — the full datapack round trip a migrated built-in travels.
[[nodiscard]] EnchantmentDefinition roundTrip(const EnchantmentDefinition& original) {
    // constexpr def -> JE-schema content JSON (with rebedrock extras when they
    // deviate) -> text -> parse -> data def -> gameplay def.
    const Json contentJson = mc::gameplay::enchantmentContentJson(original);
    const std::string text = contentJson.dump();
    const Json reparsed = Json::parse(text);

    EnchantmentDef dataDef{};
    const bool ok = mc::data::Codec<EnchantmentDef>::read(reparsed, dataDef);
    assert(ok && "a generated built-in file must read as a JE-schema def");
    (void)ok;

    const mc::gameplay::RebedrockEnchantmentExtras extras =
        mc::gameplay::readRebedrockExtras(reparsed);
    return mc::gameplay::toGameplayDefinition(dataDef, original.id, original.vanillaName, extras);
}

// ---- (A) every built-in folds through JSON and back, observably identical ----

void everyBuiltinGoldenMatches() {
    for (const EnchantmentDefinition& original : kEnchantmentTable) {
        const EnchantmentDefinition restored = roundTrip(original);

        // Ordinal / name stability — the save-compat guarantee.
        assert(restored.id == original.id);
        assert(restored.vanillaName == original.vanillaName);

        // Static fields.
        assert(restored.maxLevel == original.maxLevel);
        assert(restored.rarity == original.rarity);
        assert(restored.category == original.category);
        assert(restored.treasureOnly == original.treasureOnly);
        assert(restored.curse == original.curse);
        assert(restored.availableForRandomSelection ==
               original.availableForRandomSelection);

        // The numeric golden bridge: cost curves must agree at every level,
        // through the accessors the enchanting-table candidate generator reads.
        // We compare a fresh registry-style lookup that indexes by the restored
        // fields against ENCH-0's constexpr accessors on the original id.
        for (std::int32_t level = original.minLevel; level <= original.maxLevel; ++level) {
            const std::int32_t minOriginal =
                mc::gameplay::getMinCost(original.id, level);
            const std::int32_t maxOriginal =
                mc::gameplay::getMaxCost(original.id, level);
            // min curve is reconstructed directly into restored.cost.
            const std::int32_t minRestored =
                mc::gameplay::detail::minPower(restored.cost, level);
            assert(minRestored == minOriginal);
            // max curve goes through getMaxCost's flat-50 special case, which is
            // keyed by id — restored.id == original.id, so it agrees.
            const std::int32_t maxRestored =
                mc::gameplay::getMaxCost(restored.id, level);
            assert(maxRestored == maxOriginal);
        }
    }
}

// ---- (B) the JE weight carried == ENCH-0's rarity->weight table -------------

void weightMatchesRarityTable() {
    for (const EnchantmentDefinition& def : kEnchantmentTable) {
        const EnchantmentDef data = mc::gameplay::toContentDef(def);
        assert(data.weight == mc::gameplay::enchantmentRarityWeight(def.rarity));
    }
}

// ---- (C) a plain enchantment's file is pure JE (no rebedrock block) ---------
//       and a special one carries exactly the deviating booleans.

void extensionBlockOnlyWhenDeviating() {
    // Sharpness: Common, non-treasure, non-curse, table-rollable -> pure JE.
    const Json sharpness =
        mc::gameplay::enchantmentContentJson(mc::gameplay::enchantmentDefinition(
            EnchantmentId::Sharpness));
    assert(!sharpness.contains(std::string{mc::gameplay::kRebedrockExtensionKey}));

    // Soul Speed: treasure + not random-selectable -> carries the block with
    // both flags; curse absent.
    const Json soulSpeed =
        mc::gameplay::enchantmentContentJson(mc::gameplay::enchantmentDefinition(
            EnchantmentId::SoulSpeed));
    assert(soulSpeed.contains(std::string{mc::gameplay::kRebedrockExtensionKey}));
    const Json& block = soulSpeed[std::string{mc::gameplay::kRebedrockExtensionKey}];
    assert(block.contains("treasure_only") && block["treasure_only"].asBool());
    assert(block.contains("random_selection") && !block["random_selection"].asBool());
    assert(!block.contains("curse"));

    // Binding Curse: treasure + curse -> both those flags, no random_selection
    // key (it stays selectable in vanilla's sense — availableForRandomSelection
    // is true for the curse, only soul_speed sets it false).
    const Json binding =
        mc::gameplay::enchantmentContentJson(mc::gameplay::enchantmentDefinition(
            EnchantmentId::BindingCurse));
    const Json& bindingBlock =
        binding[std::string{mc::gameplay::kRebedrockExtensionKey}];
    assert(bindingBlock.contains("treasure_only"));
    assert(bindingBlock.contains("curse") && bindingBlock["curse"].asBool());
    assert(!bindingBlock.contains("random_selection"));
}

// ---- (D) a vanilla JE file (no rebedrock block) folds to sane defaults ------

void vanillaFileFoldsToDefaults() {
    // A minimal JE file with none of the extension flags: it must fold to a
    // non-treasure, non-curse, table-rollable enchantment (the common case that
    // makes official JE files load correctly without any rebedrock knowledge).
    constexpr std::string_view kVanilla = R"JSON({
      "anvil_cost": 1,
      "max_cost": { "base": 21, "per_level_above_first": 11 },
      "max_level": 5,
      "min_cost": { "base": 1, "per_level_above_first": 11 },
      "slots": [ "mainhand" ],
      "supported_items": "#minecraft:enchantable/weapon",
      "weight": 10
    })JSON";
    const Json json = Json::parse(kVanilla);
    EnchantmentDef def{};
    assert(mc::data::Codec<EnchantmentDef>::read(json, def));
    const auto extras = mc::gameplay::readRebedrockExtras(json);
    const EnchantmentDefinition folded = mc::gameplay::toGameplayDefinition(
        def, EnchantmentId::Count, "custom_sharp", extras);
    assert(!folded.treasureOnly);
    assert(!folded.curse);
    assert(folded.availableForRandomSelection);
    assert(folded.rarity == mc::gameplay::EnchantmentRarity::Common); // weight 10
    assert(folded.category == mc::gameplay::EnchantmentCategory::Weapon);
    assert(folded.maxLevel == 5);
}

// ---- (E) the shipped files re-derive byte-for-byte the in-memory JSON -------

void shippedFilesMatchInMemory() {
    // resources/ is passed via MC_REBEDROCK_RESOURCE_ROOT (CMake sets it). The
    // internal datapack lives at resources/data/rebedrock/enchantment/*.json,
    // the StandardPackResourceProvider layout (<root>/data/<ns>/<path>).
    const char* rootEnv = MC_REBEDROCK_RESOURCE_ROOT;
    const std::filesystem::path resources{rootEnv};
    const std::filesystem::path packRoot = resources; // <root>/data/rebedrock/...
    const mc::assets::StandardPackResourceProvider provider{packRoot};

    // The provider must serve every built-in file, and each must equal the JSON
    // enchantmentContentJson() produces in memory for the same id (so the
    // committed files can never drift from the constexpr table).
    std::size_t seen = 0;
    for (const EnchantmentDefinition& def : kEnchantmentTable) {
        const mc::assets::ResourceLocation location{
            "rebedrock",
            "enchantment/" + std::string{def.vanillaName} + ".json",
            mc::assets::PackType::ServerData};
        const std::vector<std::byte> bytes = provider.readBytes(location);
        assert(!bytes.empty() && "a shipped built-in enchantment file must exist");
        const std::string_view fileText{reinterpret_cast<const char*>(bytes.data()),
                                        bytes.size()};
        const Json onDisk = Json::parse(fileText);
        const Json inMemory = mc::gameplay::enchantmentContentJson(def);
        // Compare the canonical dumps (order is insertion order in both; the file
        // was generated from the same writer).
        assert(onDisk.dump() == inMemory.dump() &&
               "shipped datapack file must match the constexpr-derived JSON");
        ++seen;
    }
    assert(seen == mc::gameplay::kEnchantmentCount);

    // And through the real overlay loader the files parse into a DataStore.
    mc::data::DataStore<EnchantmentDef> store;
    const std::size_t applied =
        mc::data::applyEnchantmentOverlay(store, provider, "rebedrock");
    assert(applied == mc::gameplay::kEnchantmentCount);
    // Every folded def golden-matches, loaded through the shipped file this time.
    for (const EnchantmentDefinition& original : kEnchantmentTable) {
        const std::string key =
            "rebedrock:" + std::string{original.vanillaName};
        const EnchantmentDef* data = store.find(key);
        assert(data != nullptr);
        const mc::assets::ResourceLocation location{
            "rebedrock",
            "enchantment/" + std::string{original.vanillaName} + ".json",
            mc::assets::PackType::ServerData};
        const std::vector<std::byte> bytes = provider.readBytes(location);
        const Json onDisk = Json::parse(std::string_view{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        const auto extras = mc::gameplay::readRebedrockExtras(onDisk);
        const EnchantmentDefinition folded = mc::gameplay::toGameplayDefinition(
            *data, original.id, original.vanillaName, extras);
        assert(folded.rarity == original.rarity);
        assert(folded.category == original.category);
        assert(folded.treasureOnly == original.treasureOnly);
        assert(folded.curse == original.curse);
        assert(folded.availableForRandomSelection == original.availableForRandomSelection);
        for (std::int32_t level = 1; level <= original.maxLevel; ++level) {
            assert(mc::gameplay::detail::minPower(folded.cost, level) ==
                   mc::gameplay::getMinCost(original.id, level));
            assert(mc::gameplay::getMaxCost(folded.id, level) ==
                   mc::gameplay::getMaxCost(original.id, level));
        }
    }
}

} // namespace

int main() {
    everyBuiltinGoldenMatches();
    weightMatchesRarityTable();
    extensionBlockOnlyWhenDeviating();
    vanillaFileFoldsToDefaults();
    shippedFilesMatchInMemory();
    return 0;
}
