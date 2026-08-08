#pragma once

#include "gameplay/Inventory.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace mc::gameplay {

struct ChestPosition final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const ChestPosition&) const = default;
};

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
        return entities_;
    }

  private:
    std::vector<ChestBlockEntity> entities_;
};

} // namespace mc::gameplay
