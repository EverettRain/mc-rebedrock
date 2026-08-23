#pragma once

#include "gameplay/BlockEntityStore.hpp"
#include "gameplay/Inventory.hpp"
#include "world/BlockPos.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mc::gameplay {

// BE1 unified every block entity's position on one BlockPos (int x/y/z). Kept as
// an alias so the furnace business code and its callers stay untouched — see the
// note on ChestPosition in ChestSystem.hpp.
using FurnacePosition = ::mc::world::BlockPos;

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
    // AbstractFurnaceBlockEntity's recipesUsed map, reduced to a single running
    // total (this furnace only ever tracks its own activeRecipe, so there is
    // nothing to key a map by): every completed smelt adds that recipe's
    // `experience` here uncashed, exactly the way vanilla accumulates
    // recipesUsed counts rather than paying out per-craft. XP-2's popExperience
    // cashes it out the moment the player actually takes the result, the same
    // "smelt now, pay on withdrawal" rule FurnaceResultSlot#onTake enforces
    // (never earlier — vanilla lets a furnace queue dozens of smelts with the
    // screen closed and pays the lot only when a hand finally reaches in).
    float pendingExperience = 0.0F;

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
    // FurnaceResultSlot#remove/onTake: returns true when this click actually
    // moved any amount out of the output slot (a full take, a partial
    // quick-move that could not fit the whole stack, anything above zero) —
    // vanilla's onTake/checkTakeAchievements fires on exactly that condition,
    // which is XP-2's cue to cash in pendingExperience via popExperience.
    bool clickOutput(FurnacePosition position, Inventory& inventory, bool shiftHeld = false);
    // QUICK_MOVE's inventory direction: a smeltable stack routes to the input
    // slot, a burnable one to the fuel slot. Returns true when it all fit.
    bool moveInto(FurnacePosition position, ItemStack& stack);

    // The two gauges the screen draws, for the furnace at a position. A missing
    // furnace reads as idle rather than throwing, so the UI never has to guard.
    [[nodiscard]] float cookProgress(FurnacePosition position) const;
    [[nodiscard]] float fuelProgress(FurnacePosition position) const;

    // AbstractFurnaceBlockEntity#getRecipesToAwardAndPopExperience: hands back
    // the furnace's whole uncashed experience total and resets it to zero — a
    // missing furnace (should not happen right after clickOutput found one,
    // but a screen can outlive a broken block) simply pops nothing. The
    // fractional-remainder roll (createExperience's `Mth.frac` + a coin flip)
    // is the caller's job: it needs a JavaRandom stream, which this system
    // does not own (furnace math stays RNG-free, matching cookProgress/
    // fuelProgress being pure reads).
    [[nodiscard]] float popExperience(FurnacePosition position);

    void restore(std::vector<FurnaceBlockEntity> entities);
    [[nodiscard]] std::span<const FurnaceBlockEntity> entities() const {
        return entities_.entities();
    }

  private:
    static void tickOne(FurnaceBlockEntity& furnace);

    BlockEntityStore<FurnacePosition, FurnaceBlockEntity> entities_;
};

} // namespace mc::gameplay
