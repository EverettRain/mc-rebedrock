// STRUCT-2 (wiring, first half): the template registry + the non-streamer
// orchestration — load a template by id, decide a structure chunk by placement,
// place the template into a chunk, and roll its chest. This is the full flow the
// structure step will run, exercised headless before it touches ChunkStreamer.

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "data/ChestLootFile.hpp"
#include "gameplay/ChestLootTable.hpp"
#include "gameplay/Random.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/StructureManager.hpp"
#include "world/StructurePlacer.hpp"
#include "world/StructureRotation.hpp"
#include "world/StructureTemplate.hpp"
#include "world/gen/StructurePlacement.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::StructureManager;
using mc::world::StructureRotation;
using mc::world::StructureTemplateDef;
using mc::world::gen::StructurePlacement;

// --- a minimal raw (uncompressed) NBT structure, for the load path ----------

struct NbtWriter final {
    std::string bytes;
    void u8(std::uint8_t v) { bytes.push_back(static_cast<char>(v)); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v >> 8)); u8(static_cast<std::uint8_t>(v)); }
    void i32(std::int32_t v) {
        const auto r = static_cast<std::uint32_t>(v);
        u8(static_cast<std::uint8_t>(r >> 24)); u8(static_cast<std::uint8_t>(r >> 16));
        u8(static_cast<std::uint8_t>(r >> 8)); u8(static_cast<std::uint8_t>(r));
    }
    void str(std::string_view s) { u16(static_cast<std::uint16_t>(s.size())); bytes += s; }
    void named(std::uint8_t tag, std::string_view name) { u8(tag); str(name); }
    void end() { u8(0); }
};

// size[1,1,1], palette[stone], blocks[{0,0,0->0}], DataVersion 4786.
std::string minimalTemplate() {
    NbtWriter w;
    w.u8(10); w.str(""); // root compound
    w.named(9, "size"); w.u8(3); w.i32(3); w.i32(1); w.i32(1); w.i32(1);
    w.named(9, "palette"); w.u8(10); w.i32(1);
    w.named(8, "Name"); w.str("minecraft:stone"); w.end();
    w.named(9, "blocks"); w.u8(10); w.i32(1);
    w.named(9, "pos"); w.u8(3); w.i32(3); w.i32(0); w.i32(0); w.i32(0);
    w.named(3, "state"); w.i32(0); w.end();
    w.named(3, "DataVersion"); w.i32(mc::world::kStructureDataVersion);
    w.end();
    return w.bytes;
}

class MemoryProvider final : public mc::assets::ResourceProvider {
  public:
    void add(std::string path, std::string body) {
        const mc::assets::ResourceLocation location{"minecraft", std::move(path),
                                                    mc::assets::PackType::ServerData};
        files_[location.toString()] = std::move(body);
    }
    [[nodiscard]] std::filesystem::path locate(const mc::assets::ResourceLocation&) const override { return {}; }
    [[nodiscard]] bool exists(const mc::assets::ResourceLocation& l) const override {
        return files_.find(l.toString()) != files_.end();
    }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
    [[nodiscard]] std::vector<std::byte> readBytes(const mc::assets::ResourceLocation& l) const override {
        const auto slot = files_.find(l.toString());
        if (slot == files_.end()) return {};
        std::vector<std::byte> bytes(slot->second.size());
        std::memcpy(bytes.data(), slot->second.data(), slot->second.size());
        return bytes;
    }
    [[nodiscard]] std::vector<mc::assets::ResourceLocation>
    list(std::string_view space, std::string_view prefix,
         mc::assets::PackType = mc::assets::PackType::ClientResources) const override {
        std::vector<mc::assets::ResourceLocation> found;
        for (const auto& [key, body] : files_) {
            (void)body;
            auto loc = mc::assets::ResourceLocation::parse(key, mc::assets::PackType::ServerData);
            if (loc.space == space && loc.path.size() >= prefix.size() &&
                std::string_view{loc.path}.substr(0, prefix.size()) == prefix) {
                found.push_back(std::move(loc));
            }
        }
        return found;
    }

  private:
    std::unordered_map<std::string, std::string> files_;
};

void testLoad() {
    MemoryProvider provider;
    provider.add("structure/igloo/top.nbt", minimalTemplate());
    StructureManager manager;
    const std::size_t loaded = manager.load(provider);
    assert(loaded == 1);
    const auto* def = manager.find("minecraft:igloo/top");
    assert(def != nullptr);
    assert(def->sizeX == 1 && def->sizeY == 1 && def->sizeZ == 1);
    assert(def->palette.size() == 1 && def->palette[0].block == Block::Stone);
    assert(def->blocks.size() == 1);
    assert(manager.find("minecraft:igloo/missing") == nullptr);
}

StructureTemplateDef houseWithChest() {
    using namespace mc::world;
    StructureTemplateDef t;
    t.sizeX = 2; t.sizeY = 1; t.sizeZ = 1;
    t.palette.push_back({Block::Stone, BlockState{Block::Stone}.rawId(), true});
    t.palette.push_back({Block::Chest, BlockState{Block::Chest}.rawId(), true});
    t.blockEntities.push_back({"minecraft:chest", "minecraft:chests/test", ""});
    t.blocks.push_back({0, 0, 0, 0, kNoBlockEntity});
    t.blocks.push_back({1, 0, 0, 1, 0});
    return t;
}

// The flow the structure step runs: placement decides a chunk, the manager
// resolves the template, the placer stamps it and emits the chest, the chest rolls.
void testOrchestration() {
    using namespace mc::world;
    StructureManager manager;
    manager.add("minecraft:test/house", houseWithChest());

    const StructurePlacement placement{16, 4, 555, gen::SpreadType::Linear};
    const std::uint64_t seed = 0xC0FFEEULL;

    // Find a structure chunk deterministically.
    gen::StructureChunk origin = placement.originChunk(0, 0, seed);
    assert(placement.isStructureChunk(origin.x, origin.z, seed));

    const StructureTemplateDef* def = manager.find("minecraft:test/house");
    assert(def != nullptr);

    Chunk chunk;
    std::vector<gen::TreeBorderBlock> border;
    std::vector<StructureLootPlacement> loot;
    // Place at the origin chunk's local corner, some ground height.
    const int originX = origin.x * 16;
    const int originZ = origin.z * 16;
    placeStructure(chunk, origin.x, origin.z, *def, originX, 70, originZ,
                   StructureRotation::None, border, loot);

    assert(chunk.state(0, 70, 0).block() == Block::Stone);
    assert(chunk.state(1, 70, 0).block() == Block::Chest);
    assert(loot.size() == 1 && loot[0].lootTable == "minecraft:chests/test");

    // Roll the chest.
    constexpr std::string_view kJson = R"({"type":"minecraft:chest","pools":[
      {"rolls":2.0,"entries":[{"type":"minecraft:item","name":"minecraft:coal","weight":1}]}]})";
    const auto json = mc::core::Json::parse(kJson);
    const auto table = mc::data::jeChestLoot(json, loot[0].lootTable);
    assert(table.has_value());
    mc::gameplay::ChestLootTable chests;
    std::uint64_t state = mc::rng::seedFromValue(origin.x * 31 + origin.z);
    const auto stacks = chests.roll(*table, state);
    assert(stacks.size() == 2); // two guaranteed coal rolls
}

} // namespace

int main() {
    testLoad();
    testOrchestration();
    return 0;
}
