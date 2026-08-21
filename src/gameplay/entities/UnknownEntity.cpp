#include "gameplay/entities/UnknownEntity.hpp"

#include "core/ContentId.hpp"

namespace mc::gameplay::entities {
namespace {

// The one inert AI every placeholder shares; static so its address outlives any
// EntityType that points at it, exactly like a species' own AI singleton.
const UnknownEntityAi kUnknownEntityAi;

} // namespace

const EntityType& UnknownEntityTable::intern(std::string_view name) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (std::size_t index = 0; index < names_.size(); ++index) {
        if (names_[index] == name) {
            return types_[index];
        }
    }
    // Own the name string first; the placeholder's id views into it, so it must
    // sit in stable storage before build() captures the view.
    names_.emplace_back(name);
    const std::string_view stored = names_.back();
    EntityType placeholder = EntityType::Builder::create(MobCategory::Misc, kUnknownEntityAi)
                                 .sized(0.0F, 0.0F)
                                 .build(stored);
    // Not a real network type: keep it out of any per-networkId render/cache
    // slot so it never aliases onto a resolved species (byNetworkId still misses
    // it — it lives here, not in the registry's dense table).
    placeholder.networkId_ = core::EntityTypeId::kInvalidValue;
    types_.push_back(placeholder);
    return types_.back();
}

std::size_t UnknownEntityTable::size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return names_.size();
}

UnknownEntityTable& unknownEntityTable() {
    static UnknownEntityTable table;
    return table;
}

} // namespace mc::gameplay::entities
