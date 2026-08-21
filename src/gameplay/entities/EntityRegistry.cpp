#include "gameplay/entities/EntityRegistry.hpp"

#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/UnknownEntity.hpp"
#include "gameplay/entities/ZombieEntity.hpp"

namespace mc::gameplay::entities {

const EntityType& EntityTypeRegistry::registerBuiltin(EntityType& type) {
    const core::EntityTypeId id = store_.registerBuiltin(type.id(), &type);
    // File the `minecraft:` name as an alias onto the same id (skip when a
    // species left it equal to the `rebedrock:` key, which would collide).
    if (!type.vanillaId().empty() && type.vanillaId() != type.id()) {
        store_.alias(type.vanillaId(), id);
    }
    type.networkId_ = id.value();
    return type;
}

const EntityType& EntityTypeRegistry::registerExternal(EntityType& type) {
    const core::EntityTypeId id = store_.registerExternal(type.id(), &type);
    if (!type.vanillaId().empty() && type.vanillaId() != type.id()) {
        store_.alias(type.vanillaId(), id);
    }
    type.networkId_ = id.value();
    return type;
}

const EntityType* EntityTypeRegistry::byId(std::string_view identifier) const {
    const core::EntityTypeId id = store_.byName(identifier);
    return id.valid() ? store_.get(id) : nullptr;
}

const EntityType* EntityTypeRegistry::byNetworkId(std::uint16_t id) const {
    // Range-check against the live table before deref: an id past it (a peer or
    // save that knows more types than this build) is a miss, not a get() abort.
    if (id >= store_.size()) {
        return nullptr;
    }
    return store_.get(core::EntityTypeId::of(id));
}

EntityTypeRegistry& entityTypeRegistry() {
    static EntityTypeRegistry registry;
    return registry;
}

void registerBuiltinEntities() {
    // Each creature's type() accessor builds the type on first call and files it
    // in the registry (registerBuiltin, Bootstrap phase). Touch every built-in
    // species here in a fixed order so their dense ids stay stable across runs;
    // re-running is a no-op because the per-species static is built only once.
    static_cast<void>(PigEntity::type());
    static_cast<void>(CowEntity::type());
    static_cast<void>(ZombieEntity::type());
}

const EntityType& resolveEntityTypeForRestore(std::string_view name) {
    if (const EntityType* type = entityTypeRegistry().byId(name); type != nullptr) {
        return *type;
    }
    // A species this build cannot resolve: keep it as an interned placeholder so
    // the creature round-trips by name instead of being dropped from the world.
    return unknownEntityTable().intern(name);
}

} // namespace mc::gameplay::entities
