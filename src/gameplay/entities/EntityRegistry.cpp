#include "gameplay/entities/EntityRegistry.hpp"

#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/ZombieEntity.hpp"

#include <algorithm>

namespace mc::gameplay::entities {

const EntityType& EntityTypeRegistry::add(EntityType& type) {
    const auto existing = std::find(entries_.begin(), entries_.end(), &type);
    if (existing != entries_.end()) {
        return type;
    }
    type.networkId_ = static_cast<std::uint16_t>(entries_.size());
    entries_.push_back(&type);
    return type;
}

const EntityType* EntityTypeRegistry::byId(std::string_view identifier) const {
    for (const EntityType* type : entries_) {
        if (type->id().matches(identifier) || type->vanillaId().matches(identifier)) {
            return type;
        }
    }
    return nullptr;
}

const EntityType* EntityTypeRegistry::byNetworkId(std::uint16_t id) const {
    return id < entries_.size() ? entries_[id] : nullptr;
}

EntityTypeRegistry& entityTypeRegistry() {
    static EntityTypeRegistry registry;
    return registry;
}

void registerBuiltinEntities() {
    // Each creature's type() accessor builds the type on first call and files it
    // in the registry. List every built-in species here; the order fixes their
    // network ids.
    static_cast<void>(PigEntity::type());
    static_cast<void>(CowEntity::type());
    static_cast<void>(ZombieEntity::type());
}

} // namespace mc::gameplay::entities
