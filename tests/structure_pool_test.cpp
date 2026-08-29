// STRUCT-3a: the jigsaw data layer — template pools and the jigsaw blocks lifted
// from a template. What is pinned: a vanilla template pool reduces to its weighted
// single-template elements (projection + processor ref kept, an unsupported element
// kind dropped, fallback recorded); a `minecraft:jigsaw` block in a template is
// lifted into `jigsaws` with its connection front (from the FrontAndTop
// orientation), name/target/pool, joint (rollable vs aligned) and final_state,
// while the marker block itself stays unresolved (not placed); and a pool overlay
// loads keyed by pool id.

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "data/StructurePoolFile.hpp"
#include "world/Block.hpp"
#include "world/StructureManager.hpp"
#include "world/StructureTemplate.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using mc::data::jeStructurePool;
using mc::data::PoolProjection;
using mc::world::BlockOrientation;
using mc::world::parseStructureTemplate;

// --- pool reduction -----------------------------------------------------------

void testPoolReduction() {
    constexpr std::string_view kJson = R"({
      "fallback": "minecraft:empty",
      "elements": [
        { "element": { "element_type": "minecraft:legacy_single_pool_element",
            "location": "minecraft:village/plains/houses/plains_small_house_1",
            "processors": "minecraft:mossify_20_percent", "projection": "rigid" },
          "weight": 50 },
        { "element": { "element_type": "minecraft:single_pool_element",
            "location": "minecraft:village/plains/streets/corner_01",
            "processors": { "processors": [] }, "projection": "terrain_matching" },
          "weight": 4 },
        { "element": { "element_type": "minecraft:feature_pool_element" }, "weight": 1 }
      ]
    })";
    const auto json = mc::core::Json::parse(kJson);
    const auto pool = jeStructurePool(json, "minecraft:village/plains/town_centers");
    assert(pool.has_value());
    assert(pool->id == "minecraft:village/plains/town_centers");
    assert(pool->fallback == "minecraft:empty");
    // legacy_single + single kept; feature dropped.
    assert(pool->elements.size() == 2);
    assert(pool->elements[0].location == "minecraft:village/plains/houses/plains_small_house_1");
    assert(pool->elements[0].weight == 50);
    assert(pool->elements[0].projection == PoolProjection::Rigid);
    assert(pool->elements[0].processors == "minecraft:mossify_20_percent");
    assert(pool->elements[1].projection == PoolProjection::TerrainMatching);
    assert(pool->elements[1].processors.empty()); // inline list -> no named ref

    assert(!jeStructurePool(mc::core::Json::parse("[1,2]"), "x").has_value());
}

// --- jigsaw block lifting -----------------------------------------------------

struct NbtWriter final {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v >> 8)); u8(static_cast<std::uint8_t>(v)); }
    void i32(std::int32_t v) {
        const auto r = static_cast<std::uint32_t>(v);
        u8(static_cast<std::uint8_t>(r >> 24)); u8(static_cast<std::uint8_t>(r >> 16));
        u8(static_cast<std::uint8_t>(r >> 8)); u8(static_cast<std::uint8_t>(r));
    }
    void str(std::string_view s) { u16(static_cast<std::uint16_t>(s.size())); bytes.insert(bytes.end(), s.begin(), s.end()); }
    void named(std::uint8_t tag, std::string_view name) { u8(tag); str(name); }
    void end() { u8(0); }
};
constexpr std::uint8_t kInt = 3, kString = 8, kList = 9, kCompound = 10;

// A 1×1×1 template whose single block is a west-facing jigsaw connection.
std::vector<std::uint8_t> jigsawTemplate() {
    NbtWriter w;
    w.u8(kCompound); w.str("");
    w.named(kList, "size"); w.u8(kInt); w.i32(3); w.i32(1); w.i32(1); w.i32(1);
    // palette: one jigsaw entry with orientation west_up
    w.named(kList, "palette"); w.u8(kCompound); w.i32(1);
    w.named(kCompound, "Properties"); w.named(kString, "orientation"); w.str("west_up"); w.end();
    w.named(kString, "Name"); w.str("minecraft:jigsaw");
    w.end();
    // blocks: one block at (0,0,0) -> palette 0, with jigsaw nbt
    w.named(kList, "blocks"); w.u8(kCompound); w.i32(1);
    w.named(kList, "pos"); w.u8(kInt); w.i32(3); w.i32(0); w.i32(0); w.i32(0);
    w.named(kInt, "state"); w.i32(0);
    w.named(kCompound, "nbt");
    w.named(kString, "id"); w.str("minecraft:jigsaw");
    w.named(kString, "name"); w.str("minecraft:building_entrance");
    w.named(kString, "target"); w.str("minecraft:street_side");
    w.named(kString, "pool"); w.str("minecraft:village/plains/streets");
    w.named(kString, "joint"); w.str("rollable");
    w.named(kString, "final_state"); w.str("minecraft:oak_planks");
    w.end(); // nbt
    w.end(); // block
    w.named(kInt, "DataVersion"); w.i32(mc::world::kStructureDataVersion);
    w.end();
    return w.bytes;
}

void testJigsawLift() {
    const auto bytes = jigsawTemplate();
    const auto def = parseStructureTemplate(std::span<const std::uint8_t>{bytes.data(), bytes.size()});
    assert(def.has_value());
    // The jigsaw marker palette entry is unresolved (not a placeable block).
    assert(def->palette.size() == 1);
    assert(!def->palette[0].resolved);
    assert(def->palette[0].isJigsaw);
    assert(def->palette[0].jigsawFront == BlockOrientation::West); // "west_up" -> West
    // The connection was lifted, not stored as a block entity.
    assert(def->blockEntities.empty());
    assert(def->jigsaws.size() == 1);
    const auto& j = def->jigsaws[0];
    assert(j.x == 0 && j.y == 0 && j.z == 0);
    assert(j.front == BlockOrientation::West);
    assert(j.name == "minecraft:building_entrance");
    assert(j.target == "minecraft:street_side");
    assert(j.pool == "minecraft:village/plains/streets");
    assert(j.rollable);
    assert(j.finalState == "minecraft:oak_planks");
}

// --- pool overlay load --------------------------------------------------------

class MemoryProvider final : public mc::assets::ResourceProvider {
  public:
    void add(std::string path, std::string body) {
        const mc::assets::ResourceLocation location{"minecraft", std::move(path),
                                                    mc::assets::PackType::ServerData};
        files_[location.toString()] = std::move(body);
    }
    [[nodiscard]] std::filesystem::path locate(const mc::assets::ResourceLocation&) const override { return {}; }
    [[nodiscard]] bool exists(const mc::assets::ResourceLocation& l) const override {
        return files_.count(l.toString()) != 0;
    }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
    [[nodiscard]] std::vector<std::byte> readBytes(const mc::assets::ResourceLocation& l) const override {
        const auto slot = files_.find(l.toString());
        if (slot == files_.end()) return {};
        std::vector<std::byte> b(slot->second.size());
        std::memcpy(b.data(), slot->second.data(), slot->second.size());
        return b;
    }
    [[nodiscard]] std::vector<mc::assets::ResourceLocation>
    list(std::string_view space, std::string_view prefix, mc::assets::PackType = mc::assets::PackType::ClientResources) const override {
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

void testPoolOverlayLoad() {
    MemoryProvider provider;
    provider.add("worldgen/template_pool/village/plains/town_centers.json",
                 R"({"fallback":"minecraft:empty","elements":[{"element":{"element_type":"minecraft:single_pool_element","location":"minecraft:village/plains/houses/h1","projection":"rigid"},"weight":1}]})");
    mc::world::StructureManager manager;
    const std::size_t loaded = manager.loadPools(provider);
    assert(loaded == 1);
    const auto* pool = manager.findPool("minecraft:village/plains/town_centers");
    assert(pool != nullptr);
    assert(pool->elements.size() == 1);
    assert(pool->elements[0].location == "minecraft:village/plains/houses/h1");
    assert(manager.findPool("minecraft:village/plains/missing") == nullptr);
}

} // namespace

int main() {
    testPoolReduction();
    testJigsawLift();
    testPoolOverlayLoad();
    return 0;
}
