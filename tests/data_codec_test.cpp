// D-1: codec + two-layer baking infrastructure.
//
// Covers the four moving parts the data layer stands on:
//   1. the codec round-trips a POD through JSON *text*, per value (sabotage ①);
//   2. the baked built-in floor comes up with zero JSON parses (sabotage ③);
//   3. a datapack overlay merges onto that floor by name, replacing and adding;
//   4. with no `data/` at all the floor is the whole table (sabotage ②);
// plus the R0 hook that files overlay additions into a registry's External phase.

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "core/Registry.hpp"
#include "data/Codec.hpp"
#include "data/DataStore.hpp"
#include "data/DemoData.hpp"
#include "data/DemoBakedData.inc"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using mc::data::Codec;
using mc::data::DataStore;
using mc::data::demo::BakedDemoRecord;
using mc::data::demo::DemoId;
using mc::data::demo::DemoRecord;
using mc::data::demo::kDemoBakedTable;
using mc::data::demo::kDemoPrefix;

// An in-memory datapack: named files handed straight back as bytes, so the
// overlay path can be exercised without touching disk. Only the pieces the
// DataStore uses (list + readBytes + exists) carry content.
class MemoryProvider final : public mc::assets::ResourceProvider {
  public:
    void add(std::string path, std::string body, std::string space = "minecraft") {
        const mc::assets::ResourceLocation location{std::move(space), std::move(path),
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
        if (slot == files_.end()) {
            return {};
        }
        std::vector<std::byte> bytes(slot->second.size());
        std::memcpy(bytes.data(), slot->second.data(), slot->second.size());
        return bytes;
    }

    [[nodiscard]] std::vector<mc::assets::ResourceLocation>
    list(std::string_view space, std::string_view pathPrefix) const override {
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

DemoRecord bakedFloor(std::string_view name) {
    DataStore<DemoRecord> store;
    mc::data::demo::bakeInto(store, kDemoBakedTable);
    const DemoRecord* found = store.find(name);
    assert(found != nullptr);
    return *found;
}

// 1. Round-trip: every DemoRecord survives write -> dump -> parse -> read intact.
void testCodecRoundTrip() {
    const DemoRecord cases[] = {
        {3, "alpha-label", {1, 2, 3}, true, 2.5},
        {7, "beta", {}, false, std::nullopt},            // empty list, absent optional
        {42, "gamma\ntext\t\"q\"", {9}, true, -0.5},     // escapes + present optional
        {-1, "", {0, -5, 2147483647}, false, 0.0},       // negatives, extremes, explicit 0.0
    };
    for (const auto& record : cases) {
        assert(mc::data::roundTripsThroughText(record));
        // The in-memory half of the round-trip is equally exact.
        DemoRecord restored;
        assert(Codec<DemoRecord>::read(Codec<DemoRecord>::write(record), restored));
        assert(restored == record);
    }

    // A wrong JSON shape is a clean read failure, not a crash — the tolerance a
    // malformed datapack file relies on.
    DemoRecord sink;
    assert(!Codec<DemoRecord>::read(mc::core::Json::parse("[1,2,3]"), sink));
    assert(!Codec<DemoRecord>::read(mc::core::Json::parse("{\"weight\":\"nope\"}"), sink));
}

// The dumped text is real JSON: integers dump without a decimal point, and the
// value re-parses to an equal structure.
void testDumpShape() {
    const DemoRecord record{4, "x", {1, 2}, true, 2.5};
    const std::string text = Codec<DemoRecord>::write(record).dump();
    assert(text.find("\"weight\":4") != std::string::npos);   // 4, not 4.0
    assert(text.find("\"steps\":[1,2]") != std::string::npos);
    assert(text.find("\"enabled\":true") != std::string::npos);
    assert(text.find("\"scale\":2.5") != std::string::npos);
    DemoRecord restored;
    assert(Codec<DemoRecord>::read(mc::core::Json::parse(text), restored));
    assert(restored == record);
}

// 2. The baked floor is constexpr `.rodata`: laying it down parses nothing.
void testBuiltinFloorIsBakedNotParsed() {
    const std::uint64_t before = mc::core::Json::parseCount();
    DataStore<DemoRecord> store;
    mc::data::demo::bakeInto(store, kDemoBakedTable);
    const std::uint64_t after = mc::core::Json::parseCount();
    assert(after == before); // zero parses to bring the built-in floor up

    assert(store.size() == 3U);
    const DemoRecord* alpha = store.find("minecraft:alpha");
    assert(alpha != nullptr);
    assert(alpha->weight == 3);
    assert(alpha->label == "alpha-label");
    assert((alpha->steps == std::vector<std::int32_t>{1, 2, 3}));
    assert(alpha->enabled);
    assert(alpha->scale.has_value() && *alpha->scale == 2.5);
    // beta carries an empty list and an absent optional, baked faithfully.
    const DemoRecord* beta = store.find("minecraft:beta");
    assert(beta != nullptr && beta->steps.empty() && !beta->scale.has_value());
    // gamma's newline escape survived the bake.
    const DemoRecord* gamma = store.find("minecraft:gamma");
    assert(gamma != nullptr && gamma->label == "gamma\ntext");
}

// 3. Overlay merge: a file whose id matches a built-in replaces it in place; a
// file with a new id is added. The floor entries a pack does not touch remain.
void testOverlayMergesOntoFloor() {
    DataStore<DemoRecord> store;
    mc::data::demo::bakeInto(store, kDemoBakedTable);

    MemoryProvider pack;
    // Replace the built-in alpha.
    pack.add("demo/alpha.json",
             R"({"weight":99,"label":"overridden","steps":[8],"enabled":false})");
    // Add a brand-new delta.
    pack.add("demo/delta.json",
             R"({"weight":5,"label":"new","steps":[],"enabled":true,"scale":1.25})");

    const std::size_t applied = store.applyOverlay(pack, "minecraft", kDemoPrefix);
    assert(applied == 2U);
    assert(store.size() == 4U); // 3 built-ins + delta

    const DemoRecord* alpha = store.find("minecraft:alpha");
    assert(alpha != nullptr && alpha->weight == 99 && alpha->label == "overridden");
    const DemoRecord* delta = store.find("minecraft:delta");
    assert(delta != nullptr && delta->weight == 5 && delta->scale.has_value());
    // An untouched built-in is exactly its baked self.
    assert(*store.find("minecraft:beta") == bakedFloor("minecraft:beta"));

    // Exactly one entry is an overlay *addition* (delta); the replaced alpha kept
    // its built-in origin.
    std::size_t additions = 0;
    for (const auto& entry : store.entries()) {
        additions += entry.fromOverlay ? 1U : 0U;
    }
    assert(additions == 1U);
}

// 4. No `data/` at all: the floor is the whole table and the build still runs.
void testNoDataFallback() {
    DataStore<DemoRecord> store;
    mc::data::demo::bakeInto(store, kDemoBakedTable);

    MemoryProvider empty; // a resource pack with assets only, nothing under demo/
    const std::size_t applied = store.applyOverlay(empty, "minecraft", kDemoPrefix);
    assert(applied == 0U);
    assert(store.size() == 3U);
    assert(*store.find("minecraft:alpha") == bakedFloor("minecraft:alpha"));
    for (const auto& entry : store.entries()) {
        assert(!entry.fromOverlay); // nothing came from an overlay
    }
}

// A layered pack stack overrides per file: the top layer's alpha wins, the base
// layer's own file still resolves.
void testLayeredOverlayPerFile() {
    DataStore<DemoRecord> store;
    mc::data::demo::bakeInto(store, kDemoBakedTable);

    MemoryProvider base;
    base.add("demo/alpha.json", R"({"weight":1,"label":"base","steps":[],"enabled":false})");
    base.add("demo/epsilon.json", R"({"weight":2,"label":"base-only","steps":[],"enabled":false})");
    MemoryProvider top;
    top.add("demo/alpha.json", R"({"weight":50,"label":"top","steps":[],"enabled":true})");

    const mc::assets::LayeredResourceProvider layered{base, {&top}};
    store.applyOverlay(layered, "minecraft", kDemoPrefix);

    assert(store.find("minecraft:alpha")->label == "top");        // top layer wins
    assert(store.find("minecraft:epsilon")->label == "base-only"); // base still resolves
}

// The R0 hook: built-ins register in Bootstrap, overlay additions in External.
void testRegistersAdditionsIntoRegistry() {
    DataStore<DemoRecord> store;
    mc::data::demo::bakeInto(store, kDemoBakedTable);
    MemoryProvider pack;
    pack.add("demo/delta.json", R"({"weight":5,"label":"new","steps":[1],"enabled":true})");
    store.applyOverlay(pack, "minecraft", kDemoPrefix);

    mc::core::Registry<DemoRecord, DemoId> registry;
    for (const auto& entry : store.entries()) {
        if (!entry.fromOverlay) {
            registry.registerBuiltin(mc::core::Identifier::parse(entry.name), entry.def);
        }
    }
    registry.beginExternal();
    store.registerAdditionsInto(registry);
    registry.freeze();

    assert(registry.size() == 4U);
    const DemoId delta = registry.byName(mc::core::Identifier::parse("minecraft:delta"));
    assert(delta.valid());
    assert(registry.get(delta).label == "new");
    // A built-in kept its Bootstrap id and definition.
    const DemoId alpha = registry.byName(mc::core::Identifier::parse("minecraft:alpha"));
    assert(alpha.valid() && registry.get(alpha).weight == 3);
}

} // namespace

int main() {
    testCodecRoundTrip();
    testDumpShape();
    testBuiltinFloorIsBakedNotParsed();
    testOverlayMergesOntoFloor();
    testNoDataFallback();
    testLayeredOverlayPerFile();
    testRegistersAdditionsIntoRegistry();
    return 0;
}
