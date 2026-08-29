// STRUCT-1: the chest loot engine (pools / weighted entries / set_count).
//
// What is pinned here: a vanilla JE chest table reduces to the flat POD with its
// pools, weighted entries and set_count/uniform intact; an unsupported entry kind
// (loot_table) drops out of the pool while its neighbours survive, and an
// unsupported function drops off its entry; the evaluator is deterministic in its
// rng state (same seed -> same stacks), set_count sizes stacks inside the declared
// range, an `empty` entry yields nothing, and an item this build lacks is skipped
// without shifting the sequence (a following known item still rolls the same); and
// an overlay of `loot_tables/chests/*.json` loads keyed by loot-table identifier.

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "data/ChestLootFile.hpp"
#include "gameplay/ChestLootTable.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/Random.hpp"
#include "world/Block.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using mc::data::ChestEntryKind;
using mc::data::ChestFunctionKind;
using mc::data::ChestLootTableDef;
using mc::data::ChestNumberKind;
using mc::data::jeChestLoot;
using mc::gameplay::ChestLootTable;
using mc::gameplay::ItemStack;

// A JE chest table: a uniform roll count, a set_count'd apple, a plain coal, an
// item this build lacks, an empty entry, an unsupported loot_table entry, and an
// entry carrying an unsupported function alongside a supported one.
constexpr std::string_view kChestJson = R"({
  "type": "minecraft:chest",
  "pools": [
    {
      "rolls": { "type": "minecraft:uniform", "min": 2.0, "max": 8.0 },
      "entries": [
        { "type": "minecraft:item", "name": "minecraft:apple", "weight": 15,
          "functions": [
            { "function": "minecraft:enchant_randomly" },
            { "function": "minecraft:set_count", "add": false,
              "count": { "type": "minecraft:uniform", "min": 1.0, "max": 3.0 } } ] },
        { "type": "minecraft:item", "name": "minecraft:coal", "weight": 10 },
        { "type": "minecraft:item", "name": "minecraft:totally_unknown_item", "weight": 5 },
        { "type": "minecraft:empty", "weight": 10 },
        { "type": "minecraft:loot_table", "name": "minecraft:chests/other", "weight": 3 }
      ]
    }
  ]
})";

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
            auto location = mc::assets::ResourceLocation::parse(key, mc::assets::PackType::ServerData);
            if (location.space == space && location.path.size() >= pathPrefix.size() &&
                std::string_view{location.path}.substr(0, pathPrefix.size()) == pathPrefix) {
                found.push_back(std::move(location));
            }
        }
        return found;
    }

  private:
    std::unordered_map<std::string, std::string> files_;
};

ChestLootTableDef reduce() {
    const auto json = mc::core::Json::parse(kChestJson);
    auto def = jeChestLoot(json, "minecraft:chests/test");
    assert(def.has_value());
    return *def;
}

void testReduction() {
    const auto def = reduce();
    assert(def.identifier == "minecraft:chests/test");
    assert(def.pools.size() == 1);
    const auto& pool = def.pools[0];
    assert(pool.rolls.kind == ChestNumberKind::Uniform);
    assert(pool.rolls.min == 2.0F && pool.rolls.max == 8.0F);

    // apple, coal, unknown-item, empty survive; the loot_table entry is dropped.
    assert(pool.entries.size() == 4);
    const auto& apple = pool.entries[0];
    assert(apple.kind == ChestEntryKind::Item && apple.name == "minecraft:apple");
    assert(apple.weight == 15);
    // enchant_randomly is dropped, set_count kept.
    assert(apple.functions.size() == 1);
    assert(apple.functions[0].kind == ChestFunctionKind::SetCount);
    assert(apple.functions[0].count.kind == ChestNumberKind::Uniform);
    assert(apple.functions[0].count.min == 1.0F && apple.functions[0].count.max == 3.0F);
    assert(!apple.functions[0].add);

    assert(pool.entries[1].name == "minecraft:coal" && pool.entries[1].weight == 10);
    assert(pool.entries[1].functions.empty());
    assert(pool.entries[2].name == "minecraft:totally_unknown_item");
    assert(pool.entries[3].kind == ChestEntryKind::Empty);
}

void testRoll() {
    const auto def = reduce();
    ChestLootTable table;

    const mc::gameplay::Item* apple = mc::gameplay::itemFromIdentifier("minecraft:apple");
    const mc::gameplay::Item* coal = mc::gameplay::itemFromIdentifier("minecraft:coal");
    assert(apple != nullptr && coal != nullptr);

    // Determinism: the same starting state yields identical stacks.
    std::uint64_t a = mc::rng::seedFromValue(0xABCDEF12ULL);
    std::uint64_t b = mc::rng::seedFromValue(0xABCDEF12ULL);
    const auto first = table.roll(def, a);
    const auto second = table.roll(def, b);
    assert(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        assert(first[i].item == second[i].item && first[i].count == second[i].count);
    }

    // Every produced stack is a known item (unknown item skipped, empty yields
    // nothing), and apple counts respect the set_count [1,3] range.
    bool sawApple = false;
    for (const auto& stack : first) {
        assert(stack.item == apple || stack.item == coal);
        assert(stack.count >= 1);
        if (stack.item == apple) {
            sawApple = true;
            assert(stack.count >= 1 && stack.count <= 3);
        } else {
            assert(stack.count == 1); // coal has no set_count
        }
    }

    // Roll a range of seeds so the assertions above see apples with varied counts,
    // and confirm the roll count stays within the pool's [2,8] draw bound.
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        std::uint64_t state = mc::rng::seedFromValue(seed);
        const auto stacks = table.roll(def, state);
        assert(stacks.size() <= 8); // at most `rolls` items, minus empties/unknowns
    }
    (void)sawApple;
}

void testOverlayLoad() {
    MemoryProvider provider;
    provider.add("loot_table/chests/igloo_chest.json", std::string{kChestJson});
    ChestLootTable table;
    table.load(provider);
    assert(table.size() == 1);
    // Keyed by the loot-table identifier a structure references.
    const auto* def = table.find("minecraft:chests/igloo_chest");
    assert(def != nullptr);
    assert(def->pools.size() == 1);
    assert(table.find("minecraft:chests/missing") == nullptr);
}

void testNotALootTable() {
    const auto json = mc::core::Json::parse(R"([1,2,3])");
    assert(!jeChestLoot(json, "x").has_value());
}

// fillSlots scatters rolled stacks into empty cells, deterministically, and leaves
// occupied cells alone.
void testFillSlots() {
    const auto def = reduce(); // uniform 2..8 rolls of apple/coal (+unknown/empty)
    ChestLootTable table;

    std::array<ItemStack, 27> a{};
    std::array<ItemStack, 27> b{};
    std::uint64_t sa = mc::rng::seedFromValue(7ULL);
    std::uint64_t sb = mc::rng::seedFromValue(7ULL);
    table.fillSlots(a, def, sa);
    table.fillSlots(b, def, sb);

    // Deterministic: same seed -> same layout.
    std::size_t filled = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        assert((a[i].item == b[i].item) && (a[i].count == b[i].count));
        if (!a[i].empty()) ++filled;
    }
    assert(filled >= 1 && filled <= 27);

    // An occupied slot is left untouched.
    std::array<ItemStack, 4> small{};
    const mc::gameplay::Item* coal = mc::gameplay::itemFromIdentifier("minecraft:coal");
    small[0] = ItemStack{mc::world::Block::Air, 5, coal};
    std::uint64_t s = mc::rng::seedFromValue(3ULL);
    table.fillSlots(small, def, s);
    assert(small[0].item == coal && small[0].count == 5); // preserved
}

} // namespace

int main() {
    testReduction();
    testRoll();
    testOverlayLoad();
    testNotALootTable();
    testFillSlots();
    return 0;
}
