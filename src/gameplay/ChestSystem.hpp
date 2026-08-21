#pragma once

#include "gameplay/BlockEntityStore.hpp"
#include "gameplay/Inventory.hpp"
#include "world/BlockPos.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace mc::gameplay {

// BE1 unified every block entity's position on one BlockPos (int x/y/z), so the
// store keys on a single type rather than each system minting its own. This is
// kept as an alias so the chest business code — and every caller that spells a
// cell as `ChestPosition{x,y,z}` — is untouched.
using ChestPosition = ::mc::world::BlockPos;

struct ChestBlockEntity final {
    static constexpr std::size_t kSlotCount = 27U;

    ChestPosition position;
    std::array<ItemStack, kSlotCount> items{};
    float previousLidAngle = 0.0F;
    float lidAngle = 0.0F;
    bool open = false;
};

class ChestSystem final {
  public:
    [[nodiscard]] bool place(ChestPosition position);
    [[nodiscard]] std::optional<ChestBlockEntity> remove(ChestPosition position);
    [[nodiscard]] ChestBlockEntity* find(ChestPosition position);
    [[nodiscard]] const ChestBlockEntity* find(ChestPosition position) const;
    [[nodiscard]] bool open(ChestPosition position);
    void close(ChestPosition position);
    void closeAll();
    void tick();
    void clickSlot(
        ChestPosition position,
        std::size_t index,
        Inventory& inventory,
        InventoryMouseButton button,
        bool shiftHeld = false);
    // QUICK_MOVE's inventory direction: move as much of `stack` into the open
    // chest as fits, leaving the remainder behind. Returns true when it all fit.
    bool moveInto(ChestPosition position, ItemStack& stack);
    void restore(std::vector<ChestBlockEntity> entities);

    [[nodiscard]] std::span<const ChestBlockEntity> entities() const {
        return entities_.entities();
    }

  private:
    BlockEntityStore<ChestPosition, ChestBlockEntity> entities_;
};

} // namespace mc::gameplay
