#include "gameplay/FurnaceSystem.hpp"

#include "gameplay/CraftingSystem.hpp"

#include <algorithm>
#include <stdexcept>

namespace mc::gameplay {

FurnaceBlockEntity* FurnaceSystem::find(FurnacePosition position) {
    return entities_.find(position);
}

const FurnaceBlockEntity* FurnaceSystem::find(FurnacePosition position) const {
    return entities_.find(position);
}

bool FurnaceSystem::place(FurnacePosition position) { return entities_.place(position); }

FurnaceBlockEntity& FurnaceSystem::findOrCreate(FurnacePosition position) {
    return entities_.findOrCreate(position);
}

std::optional<FurnaceBlockEntity> FurnaceSystem::remove(FurnacePosition position) {
    return entities_.remove(position);
}

void FurnaceSystem::tickOne(FurnaceBlockEntity& furnace) {
    // AbstractFurnaceBlockEntity#tick, per furnace. A change of input resets the
    // cook progress; a lit furnace with a valid recipe and room in the output
    // advances it; fuel is consumed only to start a fresh burn.
    const auto* recipe = matchedFurnaceRecipe(furnace.input);
    if (recipe == nullptr || recipe->identifier != furnace.activeRecipe) {
        furnace.cookTicks = 0;
        furnace.activeRecipe = recipe != nullptr ? recipe->identifier : std::string_view{};
        furnace.cookDurationTicks = recipe != nullptr ? recipe->cookTicks : 200;
    }
    const ItemStack result = recipe != nullptr ? recipe->output : ItemStack{};
    const bool outputAccepts = recipe != nullptr &&
        (furnace.output.empty() ||
         (sameItem(furnace.output, result) &&
          furnace.output.count < itemMaximumStackSize(furnace.output)));
    const int availableFuelTicks = fuelBurnTicks(furnace.fuel);
    if (furnace.burnTicks <= 0 && outputAccepts && availableFuelTicks > 0) {
        --furnace.fuel.count;
        if (furnace.fuel.count == 0U) furnace.fuel = {};
        furnace.burnTicks = availableFuelTicks;
        furnace.initialBurnTicks = availableFuelTicks;
    }
    const bool burning = furnace.burnTicks > 0;
    if (burning && outputAccepts) {
        ++furnace.cookTicks;
        if (furnace.cookTicks >= furnace.cookDurationTicks) {
            furnace.cookTicks = 0;
            --furnace.input.count;
            if (furnace.input.count == 0U) furnace.input = {};
            if (furnace.output.empty()) furnace.output = result;
            else ++furnace.output.count;
        }
    } else {
        furnace.cookTicks = 0;
    }
    if (furnace.burnTicks > 0) --furnace.burnTicks;
}

void FurnaceSystem::tick() {
    for (auto& furnace : entities_.mutableEntities()) {
        tickOne(furnace);
    }
}

void FurnaceSystem::clickInput(FurnacePosition position, Inventory& inventory,
                               InventoryMouseButton button, bool shiftHeld) {
    auto* furnace = find(position);
    if (furnace == nullptr) return;
    if (shiftHeld) {
        inventory.quickMoveInto(furnace->input);
        return;
    }
    inventory.clickExternalSlot(furnace->input, button);
}

void FurnaceSystem::clickFuel(FurnacePosition position, Inventory& inventory,
                              InventoryMouseButton button, bool shiftHeld) {
    auto* furnace = find(position);
    if (furnace == nullptr) return;
    if (shiftHeld) {
        inventory.quickMoveInto(furnace->fuel);
        return;
    }
    inventory.clickExternalSlot(furnace->fuel, button);
}

void FurnaceSystem::clickOutput(FurnacePosition position, Inventory& inventory, bool shiftHeld) {
    auto* furnace = find(position);
    if (furnace == nullptr || furnace->output.empty()) return;
    // Plain click takes the result onto the cursor; Shift-click QUICK_MOVEs it
    // into the player inventory, matching vanilla's furnace result slot.
    if (shiftHeld) {
        inventory.quickMoveInto(furnace->output);
        return;
    }
    if (inventory.mergeIntoCursor(furnace->output)) {
        furnace->output = {};
    }
}

bool FurnaceSystem::moveInto(FurnacePosition position, ItemStack& stack) {
    auto* furnace = find(position);
    if (furnace == nullptr || stack.empty()) return true;
    // QUICK_MOVE routes a smeltable item to the input slot and a burnable one to
    // the fuel slot, merging into a partial stack just like a click would.
    ItemStack* target = nullptr;
    if (matchedFurnaceRecipe(stack) != nullptr) {
        target = &furnace->input;
    } else if (fuelBurnTicks(stack) > 0) {
        target = &furnace->fuel;
    }
    if (target == nullptr) return false;
    if (target->empty()) {
        *target = stack;
        stack = {};
        return true;
    }
    if (sameItem(*target, stack) && target->count < itemMaximumStackSize(stack)) {
        const auto moved = std::min(
            stack.count, static_cast<std::uint8_t>(itemMaximumStackSize(stack) - target->count));
        target->count = static_cast<std::uint8_t>(target->count + moved);
        stack.count = static_cast<std::uint8_t>(stack.count - moved);
        if (stack.count == 0U) {
            stack = {};
            return true;
        }
    }
    return false;
}

float FurnaceSystem::cookProgress(FurnacePosition position) const {
    const auto* furnace = find(position);
    if (furnace == nullptr || furnace->cookDurationTicks <= 0) return 0.0F;
    return std::clamp(static_cast<float>(furnace->cookTicks) /
                          static_cast<float>(furnace->cookDurationTicks),
                      0.0F, 1.0F);
}

float FurnaceSystem::fuelProgress(FurnacePosition position) const {
    const auto* furnace = find(position);
    if (furnace == nullptr || furnace->initialBurnTicks <= 0) return 0.0F;
    return static_cast<float>(furnace->burnTicks) /
           static_cast<float>(furnace->initialBurnTicks);
}

void FurnaceSystem::restore(std::vector<FurnaceBlockEntity> entities) {
    entities_.restore(std::move(entities));
    // A saved furnace stored its cook progress but not the string_view recipe
    // cache (which points into static data). Re-point it from the input so the
    // first tick after load sees the same recipe and resumes rather than resets.
    for (auto& furnace : entities_.mutableEntities()) {
        const auto* recipe = matchedFurnaceRecipe(furnace.input);
        furnace.activeRecipe = recipe != nullptr ? recipe->identifier : std::string_view{};
    }
}

} // namespace mc::gameplay
