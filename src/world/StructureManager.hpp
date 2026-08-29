#pragma once

// STRUCT-2 (wiring, first half): the template registry.
//
// Loads every `structures/**/*.nbt` through the provider stack into a map keyed by
// the identifier a structure/pool references it under (`structures/igloo/top.nbt`
// -> `minecraft:igloo/top`), parsing each with STRUCT-0's parseStructureTemplate.
// This is what the structure step (and, later, the jigsaw engine) resolves a
// template id against. Empty until `load` runs; a build with no structure assets
// simply places nothing, never crashes.
//
// Templates are held by value: parsed once at load into the flat POD, then read by
// id — no per-lookup parse, no NBT at the placement site (the DOD lowering STRUCT
// keeps end to end).

#include "assets/ResourceProvider.hpp"
#include "data/StructurePoolFile.hpp"
#include "world/StructureTemplate.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/StructurePlacement.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::world {

// One structure set: where it may start (random_spread placement) and the biomes
// its origin is allowed in (empty = any). A set is either a single template
// (igloo — `templateId`) or a jigsaw graph (village — `startPool` + `size` +
// `maxDistance`, expanded by STRUCT-3b). The structure step iterates these, asks
// each placement whether a structure starts in the chunk, gates on biome, and
// either stamps the template or expands and stamps the jigsaw pieces.
enum class StructureKind : std::uint8_t { SingleTemplate, Jigsaw };

struct StructureSet final {
    gen::StructurePlacement placement;
    std::vector<gen::Biome> biomes;  // allowed origin biomes; empty = any
    StructureKind kind = StructureKind::SingleTemplate;
    // SingleTemplate:
    std::string templateId;          // a template in this manager
    // Jigsaw:
    std::string startPool;           // e.g. "minecraft:village/plains/town_centers"
    int size = 0;                    // max connection depth (structure `size`)
    int maxDistance = 128;           // block radius pieces stay within
};

class StructureManager final {
  public:
    // Parses every `structures/**/*.nbt` under the provider into the registry. A
    // file that fails to parse (truncated, wrong DataVersion, not a template) is
    // skipped, never fatal. Returns the number of templates loaded.
    std::size_t load(const assets::ResourceProvider& resources);

    // The template for `identifier` (e.g. "minecraft:igloo/top"), or nullptr.
    [[nodiscard]] const StructureTemplateDef* find(std::string_view identifier) const;

    [[nodiscard]] std::size_t size() const { return templates_.size(); }
    [[nodiscard]] std::size_t templateCount() const { return templates_.size(); }

    // Installs a template directly, for a baked structure or a test. Keeps the
    // registry usable without a resource pack.
    void add(std::string identifier, StructureTemplateDef def);

    // STRUCT-3a: loads every `worldgen/template_pool/**/*.json` into the pool
    // registry, keyed by pool id (`worldgen/template_pool/village/plains/town_centers.json`
    // -> `minecraft:village/plains/town_centers`). Returns how many pools loaded.
    std::size_t loadPools(const assets::ResourceProvider& resources);

    // The pool for `identifier`, or nullptr. The jigsaw engine (STRUCT-3b) resolves
    // a structure's `start_pool` and each jigsaw block's `pool` through this.
    [[nodiscard]] const data::StructurePoolDef* findPool(std::string_view identifier) const;

    [[nodiscard]] std::size_t poolCount() const { return pools_.size(); }
    void addPool(data::StructurePoolDef pool);

    // The structure sets the generation step iterates. Registered separately from
    // templates (a set references a template by id).
    void addSet(StructureSet set) { sets_.push_back(std::move(set)); }
    [[nodiscard]] const std::vector<StructureSet>& sets() const { return sets_; }
    void clearSets() { sets_.clear(); }

  private:
    std::unordered_map<std::string, StructureTemplateDef> templates_;
    std::unordered_map<std::string, data::StructurePoolDef> pools_;
    std::vector<StructureSet> sets_;
};

// The process-wide structure registry the generation step reads. Empty until
// loaded, so a build with no structure assets generates exactly as before — the
// structure step is a no-op with no templates/sets (the main-world-equivalent
// guard: STRUCT changes generation only once content is installed). Mirrors the
// lootTable()/chestLootTable() singletons.
[[nodiscard]] StructureManager& structureManager();

// Registers the built-in structure sets (their placement + biome gate, from the
// vanilla structure_set files) for whichever templates `manager` actually loaded.
// Clears any previously registered sets first, so a datapack rebuild re-registers
// cleanly. A set whose template is absent is skipped, so this is a no-op on a
// build with no structure assets. STRUCT-2 registers igloo; STRUCT-3/4 grow this.
void registerBuiltinStructureSets(StructureManager& manager);

} // namespace mc::world
