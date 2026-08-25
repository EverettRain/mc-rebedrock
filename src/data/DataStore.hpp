#pragma once

// A generic two-layer data table: baked built-in defaults underneath, a datapack
// overlay on top. This is the shape recipes (D-3) and loot (D-4) load through,
// and it is built around one deployment fact the tag work already ran into: an
// ordinary *resource* pack ships only `assets/`, no `data/` at all, so a build
// that read its content solely from the pack would come up with no recipes and no
// loot. The built-in floor is therefore mandatory and is what an installation
// with no `data/` runs on — BlockTags calls the same thing loadBuiltinDefaults().
//
// The floor is *baked*: it comes from constexpr definitions in `.rodata` (see the
// generator under tools/, and the demo baked table the test drives), so bringing
// the store up performs zero JSON parsing. Only the overlay — the handful of
// files a player actually supplied — is parsed at runtime, through the codec.
//
// Merge policy is whole-entry-by-name, the way a Java datapack replaces a recipe
// file: an overlay entry whose key matches a built-in swaps that entry's
// definition in place (keeping its slot), and an overlay entry with a new key is
// appended as an addition. This is the recipe/loot shape; tags merge additively
// per block and keep their own bitset policy (D-2), not this store.

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "core/Registry.hpp"
#include "data/Codec.hpp"

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::data {

template <typename Def>
class DataStore final {
  public:
    struct Entry final {
        std::string name;  // full identifier, e.g. "minecraft:oak_planks"
        Def def;
        // True only for an entry an overlay *added* (a new key). An overlay that
        // replaced a built-in keeps this false: it is still the built-in slot,
        // now carrying different data. This is what tells the R0 hook which
        // entries are external content to register in the External phase.
        bool fromOverlay = false;
    };

    // Files a baked built-in default. Called once per constexpr record while the
    // floor is laid; performs no parsing. A repeated name overwrites (a baked
    // table should not contain duplicates, but defining wins over aborting here).
    void bakeBuiltin(std::string name, Def def) {
        const auto [slot, inserted] = index_.try_emplace(name, entries_.size());
        if (!inserted) {
            entries_[slot->second].def = std::move(def);
            return;
        }
        entries_.push_back(Entry{std::move(name), std::move(def), false});
    }

    // Merges a datapack overlay on top of the baked floor. Enumerates every file
    // under `data/<space>/<prefix>/` through the provider stack, parses each with
    // the codec, and merges by name: a key that already exists (built-in or an
    // earlier overlay file) has its definition replaced; a new key is appended.
    // A file that fails to parse or decode is skipped, never fatal. Returns how
    // many files were applied — zero is the no-`data/` case, where the floor is
    // the whole table and the build still runs.
    std::size_t applyOverlay(const assets::ResourceProvider& resources, std::string_view space,
                             std::string_view prefix) {
        std::size_t applied = 0;
        // Datapack content lives under `data/` — PackType::ServerData. The
        // default list() type is ClientResources (assets/), so a real
        // directory/standard-pack provider would enumerate the wrong tree and
        // find nothing; the MemoryProvider tests use ignore the type. Ask for the
        // server-data half explicitly so the shipped internal datapack (and any
        // player datapack) is actually enumerated.
        for (const auto& location :
             resources.list(space, prefix, assets::PackType::ServerData)) {
            const auto bytes = resources.readBytes(location);
            if (bytes.empty()) {
                continue;
            }
            core::Json root;
            try {
                root = core::Json::parse(std::string_view{
                    reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            } catch (const std::exception&) {
                continue; // a malformed file must not take the rest of the pack down
            }
            Def def{};
            if (!Codec<Def>::read(root, def)) {
                continue; // wrong shape for this data kind: skip, stay forward-compatible
            }
            mergeOverlay(keyFor(location, prefix), std::move(def));
            ++applied;
        }
        return applied;
    }

    // Registers the overlay *additions* into an R0 registry in the External
    // phase, so external content parsed from a datapack claims a dense id beside
    // the built-ins that registered in Bootstrap. Replacements are not
    // re-registered: they kept a built-in's slot, and the definition swap already
    // lives in this table, which is what consumers read. The registry must be in
    // its External phase; registering elsewhere aborts, by R0-1's contract.
    template <typename Id>
    void registerAdditionsInto(core::Registry<Def, Id>& registry) const {
        for (const auto& entry : entries_) {
            if (entry.fromOverlay) {
                registry.registerExternal(core::Identifier::parse(entry.name), entry.def);
            }
        }
    }

    [[nodiscard]] const Def* find(std::string_view name) const {
        const auto slot = index_.find(std::string{name});
        return slot == index_.end() ? nullptr : &entries_[slot->second].def;
    }
    [[nodiscard]] bool contains(std::string_view name) const {
        return index_.find(std::string{name}) != index_.end();
    }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

  private:
    // Turns a resource location into the store key: strips the `<prefix>/` folder
    // and the `.json` suffix, keeping the namespace, so `recipes/oak_planks.json`
    // under `minecraft` becomes `minecraft:oak_planks` — the same identity the
    // built-in floor was baked under.
    [[nodiscard]] static std::string keyFor(const assets::ResourceLocation& location,
                                            std::string_view prefix) {
        std::string_view path = location.path;
        if (path.size() >= prefix.size() && path.substr(0, prefix.size()) == prefix) {
            path.remove_prefix(prefix.size());
            if (!path.empty() && path.front() == '/') {
                path.remove_prefix(1U);
            }
        }
        if (path.size() >= 5U && path.substr(path.size() - 5U) == ".json") {
            path.remove_suffix(5U);
        }
        return location.space + ":" + std::string{path};
    }

    void mergeOverlay(std::string name, Def def) {
        const auto [slot, inserted] = index_.try_emplace(name, entries_.size());
        if (!inserted) {
            // Replace an existing entry's definition, keeping its slot and its
            // origin: a swapped built-in stays a built-in, not a new addition.
            entries_[slot->second].def = std::move(def);
            return;
        }
        entries_.push_back(Entry{std::move(name), std::move(def), true});
    }

    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_; // name -> entries_ index
};

} // namespace mc::data
