#pragma once

#include "core/ContentId.hpp"
#include "core/Registry.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace mc::gameplay::entities {

// Registry.ENTITY_TYPE (1.16.1): the ordered list of registered species, hosted
// on the shared R0 core::Registry so entity types walk the same identity machine
// as blocks and items — the three-phase Bootstrap/External/Freeze lifecycle, the
// `minecraft:` alias beside the `rebedrock:` key, the freeze-then-abort guard,
// and (later) the network registry-sync remap.
//
// It does not own the EntityType objects: each creature class holds its type in
// static storage (like a `static final EntityType<T>` field) and the registry
// stores a pointer to it as its Def (approach A — deref is one subscript to a
// pointer, and the EntityType's static address stays stable so SimpleEntity can
// keep holding `const EntityType*`). Behaviour is never dispatched by name — it
// is read straight off the EntityType a SimpleEntity already points at.
class EntityTypeRegistry final {
  public:
    // The underlying identity store: id -> the type's stable pointer.
    using Store = core::Registry<const EntityType*, core::EntityTypeId>;

    // Registry#register in the Bootstrap phase: files `type` under its
    // `rebedrock:` id, aliases its `minecraft:` name, stamps the assigned dense
    // id back into the type's networkId, and returns it. The type is passed
    // mutable because the registry owns the id it hands out.
    const EntityType& registerBuiltin(EntityType& type);

    // The datapack / mod registration door (opened after every built-in has
    // claimed its id). E2 摄取 external species through here; wired now so the
    // phase guard is real and tested.
    const EntityType& registerExternal(EntityType& type);

    // Advance Bootstrap -> External, then lock the table. Both abort if replayed,
    // exactly like the block registry.
    void beginExternal() { store_.beginExternal(); }
    void freeze() { store_.freeze(); }
    [[nodiscard]] core::RegistryPhase phase() const { return store_.phase(); }

    // Registry#get by name (strict): accepts the `rebedrock:` id or the
    // `minecraft:` alias, in full `space:path` or bare `path` form. A name no
    // species owns is a miss (nullptr), never a placeholder — /summon and the
    // command validators depend on rejecting a name nobody registered. The
    // save-restore path that must instead preserve an unknown creature resolves
    // through resolveEntityTypeForRestore(), not here.
    [[nodiscard]] const EntityType* byId(std::string_view identifier) const;

    // Registry#get(int): reverse of EntityType#networkId. An id past the table
    // is a miss (nullptr), not an abort — the value may come off a peer or a
    // save this build does not fully share.
    [[nodiscard]] const EntityType* byNetworkId(std::uint16_t id) const;

    // The dense id a name resolves to, invalid when nothing owns it.
    [[nodiscard]] core::EntityTypeId idOf(std::string_view identifier) const {
        return store_.byName(identifier);
    }

    [[nodiscard]] std::span<const EntityType* const> all() const { return store_.definitions(); }
    [[nodiscard]] std::size_t size() const { return store_.size(); }

  private:
    Store store_;
};

// The process-wide entity-type registry, populated by registerBuiltinEntities().
[[nodiscard]] EntityTypeRegistry& entityTypeRegistry();

// The single unified registration entry point. It touches every built-in
// creature's type() accessor, which builds the type through the Builder and
// files it in the registry in this fixed order (so the Bootstrap ids are stable
// across runs). Adding a new creature is: write its class, then add one line
// here — no switch to extend, no global table to edit.
void registerBuiltinEntities();

// Resolve a saved species name to a live EntityType for the restore path: the
// real type when this build knows the name, or an interned UnknownEntity
// placeholder (see UnknownEntity.hpp) when it does not, so a creature a removed
// datapack/mod once placed round-trips by name instead of vanishing. Never null.
[[nodiscard]] const EntityType& resolveEntityTypeForRestore(std::string_view name);

} // namespace mc::gameplay::entities
