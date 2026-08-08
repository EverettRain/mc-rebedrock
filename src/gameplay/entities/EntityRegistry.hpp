#pragma once

#include "gameplay/entities/EntityType.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mc::gameplay::entities {

// Registry.ENTITY_TYPE (1.16.1): the ordered list of registered species. It does
// not own the EntityType objects — each creature class holds its type in static
// storage (like a `static final EntityType<T>` field) and the registry only
// indexes a pointer to it, assigning a stable network id on the way in. There is
// deliberately no string-keyed dispatch map: name resolution is a linear scan
// over the handful of registered types, and behaviour is never dispatched by
// name — it is read straight off the EntityType a SimpleEntity already points at.
class EntityTypeRegistry final {
  public:
    // Registry#register: files `type`, gives it the next network id and returns
    // it for the caller to keep. Registering the same object twice is a no-op so
    // a creature's type() accessor stays idempotent.
    const EntityType& add(EntityType& type);

    // Registry#get by name. Accepts either the `rebedrock:` id or the
    // `minecraft:` alias, in full `space:path` or bare `path` form.
    [[nodiscard]] const EntityType* byId(std::string_view identifier) const;

    // Registry#get(int): reverse of EntityType#networkId.
    [[nodiscard]] const EntityType* byNetworkId(std::uint16_t id) const;

    [[nodiscard]] std::span<const EntityType* const> all() const { return entries_; }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

  private:
    std::vector<const EntityType*> entries_;
};

// The process-wide entity-type registry, populated by registerBuiltinEntities().
[[nodiscard]] EntityTypeRegistry& entityTypeRegistry();

// The single unified registration entry point. It touches every built-in
// creature's type() accessor, which builds the type through the Builder and
// files it in the registry. Adding a new creature is: write its class, then add
// one line here — no switch to extend, no global table to edit.
void registerBuiltinEntities();

} // namespace mc::gameplay::entities
