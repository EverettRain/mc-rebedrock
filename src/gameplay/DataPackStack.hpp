#pragma once

// PACK-1: the per-save half of the data-pack stack. PACK-0's PackManager gave
// every process one data stack and one resource stack, built once at app
// startup from <gameroot>/resourcepacks. That is the right shape for the
// resource half (client, global, options-persisted — PACK-3), but wrong for
// the data half: vanilla's data packs live at `<world>/datapacks/`, one stack
// per *save*, rebuilt every time a world opens.
//
// PerSaveDataStack is the management layer that makes the data half
// per-save: it scans a save directory's `datapacks/` folder, builds a
// PackManager data stack over the same built-in base Application already
// passes to loadDataPacks, and owns the "rebuild the data-driven gameplay
// tables from this stack" call — the same call GameRuntime::loadWorld makes
// on every world open and the dedicated server makes with no render
// dependency at all.
//
// Reset, not "unfreeze": the five gameplay tables this rebuilds
// (BlockTagTable, RecipeTable, LootTable, EntityAttributeOverlay,
// BiomeSpawnTables) are not wired through core::Registry's three-phase
// Bootstrap/External/Freeze machine — that machine exists for *identity*
// registries (block/item/entity-type ids), which datapacks in this project do
// not yet add to. Each of these five tables' own load() already clears its
// storage back to the built-in floor before applying the pack stack's
// overlay (see BlockTagTable::load, RecipeTable::load, etc. — every one
// calls loadBuiltinDefaults() first). That existing idempotent-reset
// discipline *is* this project's "return to a re-registerable state" — a
// second call to rebuild() with a different stack produces that stack's
// tables with no residue from the previous one, which is exactly what
// unloadWorld/loadWorld needs and why this class adds no reset logic of its
// own, only the orchestration that calls load() with the right provider at
// the right time.
//
// No IO reinvented: registration probes pack.mcmeta and a data/ directory the
// same way Application.cpp's resourcepacks scan already does; the actual
// bytes still flow through StandardPackResourceProvider/LayeredResourceProvider
// (PACK-0). Zip datapacks are not read here — ZipResourcePackProvider lives
// outside mc_rebedrock_runtime (it is a GUI/Vulkan-target-only dependency, see
// CMakeLists.txt), so per-save data packs are directories only for now; a save
// with a `datapacks/foo.zip` simply does not discover `foo` (noted as a
// deferred gap, not a silent partial-read).

#include "assets/PackManager.hpp"
#include "assets/PackMetadata.hpp"
#include "assets/ResourceProvider.hpp"

#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::gameplay {

// One discovered pack under a save's datapacks/ folder.
struct DiscoveredDataPack final {
    std::string id;             // the directory name — the stable key enable/disable use
    assets::PackMetadata metadata;
    bool enabled = false;       // whether it is currently in the save's data stack
};

class PerSaveDataStack final {
  public:
    // Drops every discovered pack and its providers, returning to "nothing
    // scanned" — the state a fresh PerSaveDataStack starts in. Call before
    // scan() when reusing one instance across world switches (GameRuntime
    // keeps one for its whole process lifetime rather than reconstructing it,
    // so a stale provider from a previous save's now-deleted deque entry is
    // never dereferenced).
    void reset();

    // Scans `<save>/datapacks/*`: a subdirectory holding pack.mcmeta at its
    // root is one pack, keyed by its directory name. Missing or unreadable
    // datapacks/ is not an error — a save with none simply discovers zero
    // packs and rebuild() falls back to the built-in floor, the sparse
    // "old/plain save" case PACK REGULAR #2 requires. Newly discovered packs
    // start disabled; a caller applies persisted enable/order afterwards.
    void scan(const std::filesystem::path& saveDirectory);

    // Enables `id` (a directory name scan() found) at the top of the data
    // stack, appended after whatever is already enabled — call in persisted
    // order to reproduce a save's exact stack. An id scan() did not find is
    // skipped rather than aborting: a save whose datapacks/ lost a folder
    // between sessions must still open (vanilla drops the missing pack from
    // the active list too), not crash on a dangling reference.
    void enable(const std::string& id);
    void disable(const std::string& id);

    // Every discovered pack, in discovery order, each flagged with whether it
    // is currently enabled — what `/datapack list` reports.
    [[nodiscard]] std::vector<DiscoveredDataPack> list() const;

    // The enabled ids, bottom (lowest priority) to top (highest) — what a
    // caller persists to the save's DPKS block.
    [[nodiscard]] const std::vector<std::string>& enabledOrder() const;

    [[nodiscard]] bool contains(const std::string& id) const {
        return manager_.find(id) != nullptr;
    }

    // Rebuilds every data-driven gameplay table (block tags, recipes, loot,
    // entity attributes, biome spawn tables) from `base` (the built-in
    // resources Application/DedicatedServer already own) plus this stack's
    // currently-enabled packs, in that priority order. This is the call
    // GameRuntime::loadWorld makes on every world open, and the same call
    // `/datapack enable|disable` makes afterward (PACK-2's `/reload` reuses
    // it too) — the single reusable "apply the current stack" entry point the
    // card asks for. Idempotent: calling it twice with the same stack
    // reproduces the same tables (each table's own load() clears first).
    void rebuild(const assets::ResourceProvider& base) const;

    // Rebuilds with no packs at all — the built-in floor. Used on world
    // unload (so a load() failure or a caller reading the tables between
    // unloadWorld and the next loadWorld sees the floor, never the outgoing
    // save's residue) and by Application's app-startup preparation (PACK-1
    // moves the *per-save* rebuild to world load; app startup only prepares
    // this floor, per the card's load-timing-migration rule).
    static void rebuildBuiltinOnly(const assets::ResourceProvider& base);

  private:
    assets::PackManager manager_;
    // Owns each discovered pack's provider; PackManager stores only a
    // non-owning pointer into this (mirrors Application.cpp's directoryPacks
    // deque — a deque so registering more packs never invalidates a pointer
    // already handed to PackManager).
    std::deque<assets::StandardPackResourceProvider> providers_;
    std::vector<std::string> discoveredIds_; // discovery order, for list()
};

} // namespace mc::gameplay
