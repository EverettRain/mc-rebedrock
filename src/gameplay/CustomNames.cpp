#include "gameplay/CustomNames.hpp"

#include <limits>

namespace mc::gameplay {

CustomNameId CustomNameTable::intern(std::string_view name) {
    if (name.empty() || name.size() > kMaximumCustomNameBytes) {
        return kNoCustomName;
    }
    if (const auto existing = ids_.find(name); existing != ids_.end()) {
        return existing->second;
    }
    // The id is the index, and index 0 is the sentinel, so the table is full one
    // entry before the type's ceiling.
    if (names_.size() > std::numeric_limits<CustomNameId>::max()) {
        return kNoCustomName;
    }
    const auto id = static_cast<CustomNameId>(names_.size());
    names_.emplace_back(name);
    ids_.emplace(std::string{name}, id);
    return id;
}

std::string_view CustomNameTable::nameOf(CustomNameId id) const {
    if (id == kNoCustomName || static_cast<std::size_t>(id) >= names_.size()) {
        return {};
    }
    return names_[static_cast<std::size_t>(id)];
}

void CustomNameTable::clear() {
    names_.clear();
    names_.emplace_back();  // reinstate the sentinel slot
    ids_.clear();
}

CustomNameTable& customNames() {
    static CustomNameTable table;
    return table;
}

} // namespace mc::gameplay
