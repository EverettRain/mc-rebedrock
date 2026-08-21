#pragma once

// The runtime overlay layer for entity attributes: the datapack half of E2's
// two-layer model. The built-in floor is compiled into each EntityType (the
// Builder literals, baked into `.rodata` the way BiomeSpawnTables compiles its
// spawn numbers); this table holds only the *overrides* a datapack supplied on
// top, indexed by dense EntityTypeId so a lookup is one subscript.
//
// Merge policy is per attribute: an overlay file for a species is read onto a
// copy of that species' floor, so a file that lists only `max_health` changes
// max health and leaves movement speed, follow range and the rest at their
// built-in values. A species a datapack does not mention keeps its floor whole —
// which is also exactly the no-`data/` case (an ordinary resource pack ships no
// `data/` at all), so `EntityType::attributes()` still answers with the compiled
// defaults and the build runs.
//
// Like biomeSpawnTables()/blockTags(), it is a process-wide singleton read once
// at startup: EntityType::attributes() resolves through it on every read, so the
// override a world loaded is what the simulation and renderer see.

#include "core/ContentId.hpp"
#include "gameplay/entities/EntityAttributes.hpp"

#include <cstddef>
#include <vector>

namespace mc::assets {
class ResourceProvider; // load() takes one; full definition only in the .cpp.
} // namespace mc::assets

namespace mc::gameplay::entities {

class EntityAttributeOverlay final {
  public:
    // Reads `data/<space>/entity_attributes/<species>.json` for every registered
    // species through the provider stack, each onto a copy of that species'
    // built-in floor (per-attribute fallback), and files the result by the
    // species' EntityTypeId. A file for a species this build lacks, or one that
    // fails to parse/decode, is skipped — never fatal. Rebuilds the table from
    // scratch, so calling it again re-reads the current pack stack.
    void load(const assets::ResourceProvider& resources);

    // The effective attributes for `id`: the datapack override when one was
    // filed, otherwise `floor` (the species' compiled-in default). Out-of-range
    // or never-overlaid ids (an UnknownEntity placeholder, a species registered
    // after load) fall back to `floor`, so this never reaches past the table.
    [[nodiscard]] const EntityAttributes& effectiveOr(core::EntityTypeId id,
                                                      const EntityAttributes& floor) const {
        const std::size_t index = id.index();
        return (index < overridden_.size() && overridden_[index]) ? effective_[index] : floor;
    }

    // How many species a pack actually overrode; zero is the no-`data/` case.
    [[nodiscard]] std::size_t overrideCount() const;

  private:
    std::vector<EntityAttributes> effective_; // EntityTypeId::index() -> merged attributes
    std::vector<bool> overridden_;            // EntityTypeId::index() -> a pack overrode it
};

// The process-wide overlay table every attribute read resolves through.
[[nodiscard]] EntityAttributeOverlay& entityAttributeTable();

} // namespace mc::gameplay::entities
