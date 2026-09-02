#include "world/StructureManager.hpp"

#include "core/Json.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::world {
namespace {

// `structure/igloo/top.nbt` -> `igloo/top`, the id half a structure references
// (namespaced by the caller to `minecraft:igloo/top`).
[[nodiscard]] std::string_view templateIdFromPath(std::string_view path) {
    constexpr std::string_view kPrefix = "structure/";
    if (path.size() >= kPrefix.size() && path.substr(0, kPrefix.size()) == kPrefix) {
        path.remove_prefix(kPrefix.size());
    }
    if (path.size() >= 4U && path.substr(path.size() - 4U) == ".nbt") {
        path.remove_suffix(4U);
    }
    return path;
}

// `worldgen/template_pool/village/plains/town_centers.json` ->
// `village/plains/town_centers`, the pool id half.
[[nodiscard]] std::string_view poolIdFromPath(std::string_view path) {
    constexpr std::string_view kPrefix = "worldgen/template_pool/";
    if (path.size() >= kPrefix.size() && path.substr(0, kPrefix.size()) == kPrefix) {
        path.remove_prefix(kPrefix.size());
    }
    if (path.size() >= 5U && path.substr(path.size() - 5U) == ".json") {
        path.remove_suffix(5U);
    }
    return path;
}

} // namespace

std::size_t StructureManager::load(const assets::ResourceProvider& resources) {
    templates_.clear();
    for (const auto& location :
         resources.list("minecraft", "structure", assets::PackType::ServerData)) {
        const auto bytes = resources.readBytes(location);
        if (bytes.empty()) {
            continue;
        }
        const std::span<const std::uint8_t> view{
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
        auto def = parseStructureTemplate(view);
        if (!def.has_value()) {
            continue; // truncated / wrong DataVersion / not a template: skip
        }
        std::string identifier =
            location.space + ":" + std::string{templateIdFromPath(location.path)};
        templates_.insert_or_assign(std::move(identifier), std::move(*def));
    }
    return templates_.size();
}

const StructureTemplateDef* StructureManager::find(std::string_view identifier) const {
    const auto found = templates_.find(std::string{identifier});
    return found == templates_.end() ? nullptr : &found->second;
}

void StructureManager::add(std::string identifier, StructureTemplateDef def) {
    templates_.insert_or_assign(std::move(identifier), std::move(def));
}

std::size_t StructureManager::loadPools(const assets::ResourceProvider& resources) {
    pools_.clear();
    for (const auto& location :
         resources.list("minecraft", "worldgen/template_pool", assets::PackType::ServerData)) {
        const auto bytes = resources.readBytes(location);
        if (bytes.empty()) {
            continue;
        }
        std::string identifier =
            location.space + ":" + std::string{poolIdFromPath(location.path)};
        core::Json root;
        try {
            root = core::Json::parse(std::string_view{
                reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception&) {
            continue; // a malformed pool must not take the rest of the pack down
        }
        if (auto def = data::jeStructurePool(root, std::move(identifier)); def.has_value()) {
            pools_.insert_or_assign(def->id, std::move(*def));
        }
    }
    return pools_.size();
}

const data::StructurePoolDef* StructureManager::findPool(std::string_view identifier) const {
    const auto found = pools_.find(std::string{identifier});
    return found == pools_.end() ? nullptr : &found->second;
}

void StructureManager::addPool(data::StructurePoolDef pool) {
    pools_.insert_or_assign(pool.id, std::move(pool));
}

StructureManager& structureManager() {
    static StructureManager manager;
    return manager;
}

void registerBuiltinStructureSets(StructureManager& manager) {
    manager.clearSets();

    // Master off switch: MC_REBEDROCK_NO_STRUCTURES leaves the set list empty, so
    // placeStructures/createStructureChests iterate nothing and cost is zero — no
    // per-chunk neighbourhood scan, no stamping, no meshing of structure geometry
    // (the candidate scan lives inside the per-set loop, so an empty set list skips
    // it outright). The world is otherwise generated identically. Doubles as an A/B
    // probe for whether structure generation is a frame-rate sink near a village.
    if (std::getenv("MC_REBEDROCK_NO_STRUCTURES") != nullptr) {
        return;
    }

    // Debug find-aid: MC_REBEDROCK_STRUCTURE_DEBUG makes structures dense and
    // biome-agnostic so one is always near the player in any world — for quickly
    // locating and inspecting them. Off by default; the shipped placement is vanilla.
    const bool debug = std::getenv("MC_REBEDROCK_STRUCTURE_DEBUG") != nullptr;
    if (debug) {
        // Diagnostic: if templates/pools are 0 the resource pack has no structure
        // data (no `data/minecraft/structure` or `.../worldgen/template_pool`), so
        // nothing will generate however dense the placement — check the pack.
        std::cerr << "[STRUCT] MC_REBEDROCK_STRUCTURE_DEBUG: " << manager.templateCount()
                  << " templates, " << manager.poolCount()
                  << " pools loaded; villages (spacing 24), igloos rare (spacing 48), "
                     "any biome. igloo/top="
                  << (manager.find("minecraft:igloo/top") != nullptr)
                  << " villagePool="
                  << (manager.findPool("minecraft:village/plains/town_centers") != nullptr) << "\n";
    }

    // Igloo: worldgen/structure_set/igloos.json (spacing 32, separation 8, salt
    // 14357618). The visible dome is the `top` template; the buried basement is
    // STRUCT-4. Snowy-gated in vanilla; in debug kept *rare* (wide spacing) so it
    // does not clutter the world while verifying villages.
    if (manager.find("minecraft:igloo/top") != nullptr) {
        StructureSet igloo;
        igloo.placement = debug ? gen::StructurePlacement{48, 0, 14357618, gen::SpreadType::Linear}
                                : gen::StructurePlacement{32, 8, 14357618, gen::SpreadType::Linear};
        igloo.templateId = "minecraft:igloo/top";
        igloo.biomes = debug ? std::vector<gen::Biome>{} : std::vector<gen::Biome>{gen::Biome::SnowyPlains};
        manager.addSet(igloo);
    }

    // Plains village: worldgen/structure_set/villages.json (spacing 34, separation
    // 8, salt 10387312) + village_plains.json (start_pool town_centers, size 6,
    // max_distance 80). A jigsaw structure — STRUCT-3b expands it. Plains-gated in
    // vanilla; in debug made denser + any-biome for visual verification. But a
    // village spans ~10 chunks at max_distance, so the spacing must stay well above
    // that or villages tile into a *continuous carpet* of building geometry — which
    // makes every chunk crossed while moving generate+mesh+upload a full village
    // slice, a sustained frame-rate sink (measured: dense placeStructures ~0.4 ms/
    // chunk + 5.7 ms spikes vs ~0.005 ms/chunk at vanilla spacing). spacing 24 keeps
    // a village within ~250 blocks (findable within a short walk) while leaving real
    // gaps between them, so the streaming/mesh load stays bounded.
    if (manager.findPool("minecraft:village/plains/town_centers") != nullptr) {
        StructureSet village;
        village.placement = debug
                                ? gen::StructurePlacement{24, 8, 10387312, gen::SpreadType::Linear}
                                : gen::StructurePlacement{34, 8, 10387312, gen::SpreadType::Linear};
        village.biomes = debug ? std::vector<gen::Biome>{} : std::vector<gen::Biome>{gen::Biome::Plains};
        village.kind = StructureKind::Jigsaw;
        village.startPool = "minecraft:village/plains/town_centers";
        village.size = 6;
        village.maxDistance = 80;
        manager.addSet(village);
    }
}

} // namespace mc::world
