#include "gameplay/entities/EntityAttributeOverlay.hpp"

#include "assets/ResourceLocation.hpp"
#include "assets/ResourceProvider.hpp"
#include "core/Identifier.hpp"
#include "core/Json.hpp"
#include "gameplay/entities/EntityAttributesCodec.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace mc::gameplay::entities {
namespace {

// A species' override file, e.g. `data/minecraft/entity_attributes/pig.json`.
// Built per name rather than list()'d because list() only scans the `assets/`
// half of a pack; server data is read by explicit path, the way the biome
// spawn loader reads each `data/.../<biome>.json` it knows the name of.
[[nodiscard]] assets::ResourceLocation attributeFile(const core::Identifier& name) {
    return assets::data("entity_attributes/" + std::string{name.path} + ".json",
                        std::string{name.space});
}

// Reads one override file onto a copy of `floor` (per-attribute fallback) and
// returns it, or nothing when the file is absent, unreadable, malformed, or the
// wrong shape — none of which is fatal.
[[nodiscard]] std::optional<EntityAttributes> readOverride(
    const assets::ResourceProvider& resources, const assets::ResourceLocation& location,
    const EntityAttributes& floor) {
    if (!resources.exists(location)) {
        return std::nullopt;
    }
    core::Json root;
    try {
        const auto bytes = resources.readBytes(location);
        root = core::Json::parse(
            std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    } catch (const std::exception&) {
        return std::nullopt; // a malformed file must not take the rest of the pack down.
    }
    EntityAttributes merged = floor;
    if (!data::Codec<EntityAttributes>::read(root, merged)) {
        return std::nullopt; // wrong shape for an attribute file: skip, stay tolerant.
    }
    return merged;
}

} // namespace

void EntityAttributeOverlay::load(const assets::ResourceProvider& resources) {
    const auto& registry = entityTypeRegistry();
    // One slot per registered species; every id derefs in range even if no pack
    // overrode it (overridden_ stays false and attributes() answers the floor).
    effective_.assign(registry.size(), EntityAttributes{});
    overridden_.assign(registry.size(), false);

    for (const EntityType* type : registry.all()) {
        if (type == nullptr) {
            continue;
        }
        const EntityAttributes& floor = type->attributesFloor();
        // The vanilla-named file first, then this project's own namespace on top,
        // so a pack that ships both has the native `rebedrock:` file win — a fixed
        // order, never a map iteration, so the result is deterministic.
        std::optional<EntityAttributes> merged = readOverride(resources, attributeFile(type->vanillaId()), floor);
        if (auto native = readOverride(resources, attributeFile(type->id()),
                                       merged.value_or(floor))) {
            merged = native;
        }
        if (!merged.has_value()) {
            continue;
        }
        const std::size_t index = type->typeId().index();
        if (index < effective_.size()) {
            effective_[index] = *merged;
            overridden_[index] = true;
        }
    }
}

std::size_t EntityAttributeOverlay::overrideCount() const {
    return static_cast<std::size_t>(std::count(overridden_.begin(), overridden_.end(), true));
}

EntityAttributeOverlay& entityAttributeTable() {
    static EntityAttributeOverlay table;
    return table;
}

} // namespace mc::gameplay::entities
