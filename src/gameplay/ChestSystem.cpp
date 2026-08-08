#include "gameplay/ChestSystem.hpp"

#include <algorithm>
#include <stdexcept>

namespace mc::gameplay {

ChestBlockEntity* ChestSystem::find(ChestPosition position) {
    const auto found = std::ranges::find(entities_, position, &ChestBlockEntity::position);
    return found == entities_.end() ? nullptr : &*found;
}

const ChestBlockEntity* ChestSystem::find(ChestPosition position) const {
    const auto found = std::ranges::find(entities_, position, &ChestBlockEntity::position);
    return found == entities_.end() ? nullptr : &*found;
}

bool ChestSystem::place(ChestPosition position) {
    if (find(position) != nullptr) return false;
    entities_.push_back({position});
    return true;
}

std::optional<ChestBlockEntity> ChestSystem::remove(ChestPosition position) {
    const auto found = std::ranges::find(entities_, position, &ChestBlockEntity::position);
    if (found == entities_.end()) return std::nullopt;
    ChestBlockEntity removed = *found;
    entities_.erase(found);
    return removed;
}

bool ChestSystem::open(ChestPosition position) {
    auto* chest = find(position);
    if (chest == nullptr) return false;
    chest->open = true;
    return true;
}

void ChestSystem::close(ChestPosition position) {
    if (auto* chest = find(position); chest != nullptr) chest->open = false;
}

void ChestSystem::closeAll() {
    for (auto& chest : entities_) chest.open = false;
}

void ChestSystem::tick() {
    constexpr float kLidStep = 0.1F;
    for (auto& chest : entities_) {
        chest.previousLidAngle = chest.lidAngle;
        const float target = chest.open ? 1.0F : 0.0F;
        if (chest.lidAngle < target) {
            chest.lidAngle = std::min(target, chest.lidAngle + kLidStep);
        } else if (chest.lidAngle > target) {
            chest.lidAngle = std::max(target, chest.lidAngle - kLidStep);
        }
    }
}

void ChestSystem::clickSlot(
    ChestPosition position,
    std::size_t index,
    Inventory& inventory,
    InventoryMouseButton button,
    bool shiftHeld) {
    auto* chest = find(position);
    if (chest == nullptr) return;
    if (index >= ChestBlockEntity::kSlotCount) {
        throw std::out_of_range("chest slot index is outside 0..26");
    }
    if (shiftHeld) {
        inventory.quickMoveInto(chest->items[index]);
        return;
    }
    inventory.clickExternalSlot(chest->items[index], button);
}

bool ChestSystem::moveInto(ChestPosition position, ItemStack& stack) {
    auto* chest = find(position);
    if (chest == nullptr) {
        return true;
    }
    if (stack.empty()) {
        return true;
    }
    const auto maximum = itemMaximumStackSize(stack);
    for (auto& target : chest->items) {
        if (!sameItem(target, stack) || target.count >= maximum) continue;
        const auto moved = std::min(
            stack.count, static_cast<std::uint8_t>(maximum - target.count));
        target.count = static_cast<std::uint8_t>(target.count + moved);
        stack.count = static_cast<std::uint8_t>(stack.count - moved);
        if (stack.count == 0U) {
            stack = {};
            return true;
        }
    }
    for (auto& target : chest->items) {
        if (!target.empty()) continue;
        target = stack;
        stack = {};
        return true;
    }
    return false;
}

void ChestSystem::restore(std::vector<ChestBlockEntity> entities) {
    for (auto& chest : entities) {
        chest.previousLidAngle = 0.0F;
        chest.lidAngle = 0.0F;
        chest.open = false;
    }
    entities_ = std::move(entities);
}

} // namespace mc::gameplay
