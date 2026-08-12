#pragma once

#include "gameplay/Inventory.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mc::gameplay {

struct FurnacePosition final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const FurnacePosition&) const = default;
};

// One furnace in the world, the way vanilla's FurnaceBlockEntity is: its own
// three slots and its own burn/cook counters, tied to a block position. This is
// what makes a furnace keep its contents when the screen closes and across a
// save — the old global single furnace on CraftingSystem could do neither, and
// every furnace shared one inventory.
struct FurnaceBlockEntity final {
    FurnacePosition position;
    ItemStack input{};
    ItemStack fuel{};
    ItemStack output{};
    int burnTicks = 0;        // fuel remaining for the current burn
    int initialBurnTicks = 0; // what the current fuel item started at, for the flame gauge
    int cookTicks = 0;        // progress on the current smelt
    int cookDurationTicks = 200;
    // The recipe the input currently smelts into, cached so a change of input
    // resets progress. It points into the static recipe table, so it outlives
    // any furnace; restore() re-points it after a load so progress resumes.
    std::string_view activeRecipe{};

    [[nodiscard]] bool burning() const { return burnTicks > 0; }
};

class FurnaceSystem final {
  public:
    [[nodiscard]] bool place(FurnacePosition position);
    [[nodiscard]] std::optional<FurnaceBlockEntity> remove(FurnacePosition position);
    [[nodiscard]] FurnaceBlockEntity* find(FurnacePosition position);
    [[nodiscard]] const FurnaceBlockEntity* find(FurnacePosition position) const;
    // Returns the furnace at the position, creating an empty one if none exists.
    // A furnace block loaded from a pre-block-entity save has no entity yet, and
    // opening its screen is where we discover it needs one.
    [[nodiscard]] FurnaceBlockEntity& findOrCreate(FurnacePosition position);

    // Advances every furnace one tick: consumes fuel, smelts input into output.
    void tick();

    void clickInput(FurnacePosition position, Inventory& inventory,
                    InventoryMouseButton button, bool shiftHeld = false);
    void clickFuel(FurnacePosition position, Inventory& inventory,
                   InventoryMouseButton button, bool shiftHeld = false);
    void clickOutput(FurnacePosition position, Inventory& inventory, bool shiftHeld = false);
    // QUICK_MOVE's inventory direction: a smeltable stack routes to the input
    // slot, a burnable one to the fuel slot. Returns true when it all fit.
    bool moveInto(FurnacePosition position, ItemStack& stack);

    // The two gauges the screen draws, for the furnace at a position. A missing
    // furnace reads as idle rather than throwing, so the UI never has to guard.
    [[nodiscard]] float cookProgress(FurnacePosition position) const;
    [[nodiscard]] float fuelProgress(FurnacePosition position) const;

    void restore(std::vector<FurnaceBlockEntity> entities);
    [[nodiscard]] std::span<const FurnaceBlockEntity> entities() const { return entities_; }

  private:
    static void tickOne(FurnaceBlockEntity& furnace);

    std::vector<FurnaceBlockEntity> entities_;
};

} // namespace mc::gameplay
