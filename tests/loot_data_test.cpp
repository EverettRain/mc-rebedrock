// D-4: block loot through the data path.
//
// The deterministic block drops moved out of MiningSystem's hand-written
// handlers into a baked loot floor plus a datapack overlay. What is pinned here:
// the loot codec round-trips through JSON text, the baked floor resolves to the
// former drops (stone -> cobblestone, an ore -> its item, glass -> nothing, a
// plain block -> no entry so it drops itself), an overlay changes/adds a block's
// drops, a build with no `data/` still drops on the floor, and an overlay naming
// content this build lacks is skipped. The random-loot blocks (leaves, crops)
// are deliberately not modelled here — that is the evaluator D-4 keeps minimal.

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "data/Codec.hpp"
#include "data/LootFile.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/LootTable.hpp"
#include "world/Block.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using mc::data::Codec;
using mc::gameplay::LootEntry;
using mc::gameplay::LootTable;
using mc::world::Block;

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

// 1. Loot defs round-trip through JSON text.
void testCodecRoundTrip() {
    using namespace mc::data;
    assert(roundTripsThroughText(LootTableDef{
        "minecraft:stone", {{"minecraft:cobblestone", 1}, {"minecraft:coal", 3}}}));
    assert(roundTripsThroughText(LootTableDef{"minecraft:glass", {}})); // empty = nothing

    // count defaults to 1 when a drop omits it.
    LootDropDef drop;
    assert(Codec<LootDropDef>::read(mc::core::Json::parse(R"({"id":"minecraft:stick"})"), drop));
    assert(drop.count == 1U && drop.id == "minecraft:stick");
    // a drop without an id is a clean failure.
    assert(!Codec<LootDropDef>::read(mc::core::Json::parse(R"({"count":2})"), drop));
}

// 2. The baked floor resolves to the former deterministic drops.
void testBuiltinFloorResolves() {
    LootTable table;
    table.loadBuiltinDefaults();

    // stone -> a cobblestone block stack.
    const LootEntry* stone = table.find(Block::Stone);
    assert(stone != nullptr && stone->stacks.size() == 1U);
    assert((stone->stacks[0] ==
            mc::gameplay::ItemStack{Block::Cobblestone, 1U,
                                    mc::gameplay::blockItemFor(Block::Cobblestone)}));
    // ore -> its item (block field Air).
    const LootEntry* coal = table.find(Block::CoalOre);
    assert(coal != nullptr && coal->stacks[0].block == Block::Air &&
           coal->stacks[0].item == &mc::gameplay::items::Coal);
    // bookshelf -> three books.
    const LootEntry* shelf = table.find(Block::Bookshelf);
    assert(shelf != nullptr && shelf->stacks[0].item == &mc::gameplay::items::Book &&
           shelf->stacks[0].count == 3U);
    // wall torch -> the standing torch block.
    assert(table.find(Block::WallTorch)->stacks[0].block == Block::Torch);
    // glass has an entry, but it drops nothing.
    const LootEntry* glass = table.find(Block::Glass);
    assert(glass != nullptr && glass->stacks.empty());
    // a plain self-dropping block has no entry at all.
    assert(table.find(Block::Cobblestone) == nullptr);
    assert(table.find(Block::OakPlanks) == nullptr);
}

// 3. A datapack overlay changes a built-in drop and adds a new one.
void testOverlayMerges() {
    LootTable table;
    MemoryProvider pack;
    // Change stone to drop a diamond.
    pack.add("loot_tables/blocks/stone.json",
             R"({"drops":[{"id":"minecraft:diamond","count":1}]})");
    // Give a self-dropping block (dirt) an explicit table.
    pack.add("loot_tables/blocks/dirt.json",
             R"({"drops":[{"id":"minecraft:coal","count":2}]})");
    // Make bookshelf drop nothing.
    pack.add("loot_tables/blocks/bookshelf.json", R"({"drops":[]})");

    table.load(pack);
    assert(table.find(Block::Stone)->stacks[0].item == &mc::gameplay::items::Diamond);
    const LootEntry* dirt = table.find(Block::Dirt);
    assert(dirt != nullptr && dirt->stacks[0].item == &mc::gameplay::items::Coal &&
           dirt->stacks[0].count == 2U);
    const LootEntry* shelf = table.find(Block::Bookshelf);
    assert(shelf != nullptr && shelf->stacks.empty());
}

// 4. No `data/` at all: the floor stands and blocks still drop.
void testNoDataFallback() {
    LootTable table;
    MemoryProvider empty;
    table.load(empty);
    // The floor must survive an empty overlay — dropping it here is what breaks
    // an ordinary install whose pack ships no `data/`.
    assert(table.find(Block::Stone) != nullptr);
    assert(table.find(Block::Stone)->stacks[0].block == Block::Cobblestone);
    assert(table.find(Block::CoalOre) != nullptr);
    assert(table.find(Block::CoalOre)->stacks[0].item == &mc::gameplay::items::Coal);
    assert(table.find(Block::Cobblestone) == nullptr); // still self-drops
}

// 5. An overlay naming content this build lacks is skipped, leaving the floor.
void testUnknownSkipped() {
    LootTable table;
    MemoryProvider pack;
    // A drop item that does not exist: the whole table is skipped, floor kept.
    pack.add("loot_tables/blocks/stone.json",
             R"({"drops":[{"id":"minecraft:no_such_item"}]})");
    // A table for a block that does not exist: skipped entirely.
    pack.add("loot_tables/blocks/no_such_block.json",
             R"({"drops":[{"id":"minecraft:stone"}]})");
    table.load(pack);
    assert(table.find(Block::Stone)->stacks[0].block == Block::Cobblestone); // floor kept
}

} // namespace

int main() {
    testCodecRoundTrip();
    testBuiltinFloorResolves();
    testOverlayMerges();
    testNoDataFallback();
    testUnknownSkipped();
    return 0;
}
