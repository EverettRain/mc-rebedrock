#pragma once

// The "block entities held by position" half of a block-entity system.
//
// ChestSystem and FurnaceSystem each carried their own copy of the same four
// operations — find, find-const, place-if-absent, remove-and-return — over the
// same flat vector, differing only in the entity type. That duplication is
// what A3 keeps running into: the mutation service now decides *when* a block
// entity is created or destroyed, so the storage those decisions act on should
// be stated once. What is left in each system is the part that is actually
// about chests or furnaces (lids, burns, slot clicks).
//
// A flat vector is deliberate at this size: a world holds tens of these, and a
// linear scan beats a hash map for that. The container is behind this type, so
// swapping it when chunk-level persistence lands is a change here rather than
// in every system.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace mc::gameplay {

// `Entity` must expose a `position` member of type `Position`, which must be
// equality-comparable — the same shape ChestBlockEntity and FurnaceBlockEntity
// already had.
template <typename Position, typename Entity>
class BlockEntityStore final {
  public:
    [[nodiscard]] Entity* find(Position position) {
        const auto found = std::ranges::find(entities_, position, &Entity::position);
        return found == entities_.end() ? nullptr : &*found;
    }

    [[nodiscard]] const Entity* find(Position position) const {
        const auto found = std::ranges::find(entities_, position, &Entity::position);
        return found == entities_.end() ? nullptr : &*found;
    }

    // Creates an entity for a newly placed block. Returns false when one is
    // already there, so re-placing over an existing entity never orphans it.
    bool place(Position position) {
        if (find(position) != nullptr) {
            return false;
        }
        entities_.push_back(Entity{position});
        return true;
    }

    // Removes the entity at a position and hands it back so the caller can
    // spill its contents. Nullopt when the cell held none.
    std::optional<Entity> remove(Position position) {
        const auto found = std::ranges::find(entities_, position, &Entity::position);
        if (found == entities_.end()) {
            return std::nullopt;
        }
        Entity removed = *found;
        entities_.erase(found);
        return removed;
    }

    // The entity at a position, creating an empty one if none exists. A block
    // loaded from a pre-block-entity save has no entity yet, and the first
    // interaction with it is where we discover it needs one.
    [[nodiscard]] Entity& findOrCreate(Position position) {
        if (auto* existing = find(position); existing != nullptr) {
            return *existing;
        }
        entities_.push_back(Entity{position});
        return entities_.back();
    }

    void restore(std::vector<Entity> entities) { entities_ = std::move(entities); }

    [[nodiscard]] std::span<const Entity> entities() const { return entities_; }
    [[nodiscard]] std::vector<Entity>& mutableEntities() { return entities_; }

  private:
    std::vector<Entity> entities_;
};

} // namespace mc::gameplay
