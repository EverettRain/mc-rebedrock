#pragma once

#include "gameplay/Item.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mc::gameplay {

struct ItemStack;

// Defined after ItemStack (it reads the stack's fields); both forms of a block
// stack — the legacy null item pointer or the block's own BlockItem — count.
[[nodiscard]] constexpr bool isBlockStack(const ItemStack& stack);

struct ItemStack final {
    world::Block block = world::Block::Air;
    std::uint8_t count = 0;
    // The stack's identity. nullptr means "this stack is its block" (the old
    // ItemKind::Block sentinel); otherwise it points at the registered Item.
    const Item* item = nullptr;
    // ItemStack#getDamage: how much of the tool's durability has been spent.
    // Zero for everything that has no durability at all.
    std::uint16_t damage = 0;

    [[nodiscard]] constexpr bool empty() const {
        return count == 0 || (item == nullptr && block == world::Block::Air);
    }

    // ItemStack#equals. Two block stacks compare by their block alone: a stack
    // that still carries the legacy null item pointer is the same stone stack as
    // one that points at stone's BlockItem. Item stacks compare by item.
    [[nodiscard]] constexpr bool operator==(const ItemStack& other) const {
        if (count != other.count || damage != other.damage) return false;
        if (isBlockStack(*this) && isBlockStack(other)) return block == other.block;
        return item == other.item && block == other.block;
    }
};

// A stack is a block stack when it names a real block and its item is either the
// legacy null pointer or the block's own BlockItem. The block field stays the
// single source of truth for a block stack's identity.
[[nodiscard]] constexpr bool isBlockStack(const ItemStack& stack) {
    return stack.block != world::Block::Air && stack.count > 0 &&
        (stack.item == nullptr || asBlockItem(stack.item) != nullptr);
}

// ItemStack#getMaxDamage: the tool material's durability, or zero for anything
// that never wears out.
[[nodiscard]] constexpr std::uint16_t itemMaximumDamage(const ItemStack& stack) {
    if (stack.item == nullptr) return 0U;
    return toolAttributes(stack.item->toolType, stack.item->toolTier).durability;
}

[[nodiscard]] constexpr bool isDamageable(const ItemStack& stack) {
    return itemMaximumDamage(stack) > 0U;
}

// ItemStack#canCombine, which compares the damage too: a half-worn pickaxe is
// not the same item as a fresh one. Block stacks combine by block, whatever form
// their item pointer takes; every other stack needs a matching item (or a
// matching block for the legacy null-item sentinel, which keeps an empty stack
// distinct from a block stack).
[[nodiscard]] constexpr bool sameItem(const ItemStack& first, const ItemStack& second) {
    if (first.damage != second.damage) return false;
    if (isBlockStack(first) && isBlockStack(second)) return first.block == second.block;
    return first.item == second.item &&
        (first.item != nullptr || first.block == second.block);
}

[[nodiscard]] constexpr std::uint8_t itemMaximumStackSize(const ItemStack& stack) {
    return stack.item == nullptr
        ? world::blockDefinition(stack.block).maximumStackSize
        : stack.item->maximumStackSize;
}

// The English display name, used for the HUD and command echoes. The renderer
// picks Chinese vs English by the active language (see stackDisplayName).
[[nodiscard]] constexpr const char* itemName(const ItemStack& stack) {
    if (isBlockStack(stack)) return world::blockName(stack.block);
    return stack.item == nullptr ? "" : stack.item->en;
}

[[nodiscard]] inline float itemTextureLayer(const ItemStack& stack) {
    // A block stack samples its block's own textures, never the item icon atlas
    // (block items are not appended to it), whatever form its item pointer takes.
    if (isBlockStack(stack)) return world::textureLayers(stack.block).top;
    return itemTextureLayer(stack.item);
}

[[nodiscard]] constexpr bool emitsHeldLight(const ItemStack& stack) {
    return isBlockStack(stack) && world::isTorch(stack.block);
}

[[nodiscard]] std::span<const ItemStack> creativeCatalog();
[[nodiscard]] std::span<const ItemStack> creativeBlockCatalog();
[[nodiscard]] std::span<const ItemStack> creativeItemCatalog();
[[nodiscard]] std::span<const ItemStack> creativeCatalog(CreativeCategory category);

enum class InventoryMouseButton {
    Left,
    Right,
};

class Inventory final {
  public:
    static constexpr std::size_t kHotbarSize = 9;
    static constexpr std::size_t kMainSize = 27;
    static constexpr std::size_t kSlotCount = kHotbarSize + kMainSize;

    Inventory();

    [[nodiscard]] const ItemStack& slot(std::size_t index) const;
    [[nodiscard]] const ItemStack& cursorStack() const { return cursorStack_; }
    [[nodiscard]] std::size_t selectedHotbarSlot() const { return selectedHotbarSlot_; }
    [[nodiscard]] const ItemStack& selectedStack() const {
        return slots_[selectedHotbarSlot_];
    }
    [[nodiscard]] const std::array<ItemStack, kSlotCount>& slots() const { return slots_; }
    [[nodiscard]] world::Block selectedBlock() const;

    void selectHotbar(std::size_t index);
    void scrollHotbar(int steps);
    void clickSlot(std::size_t index, InventoryMouseButton button, bool shiftHeld);
    void clickExternalSlot(ItemStack& slot, InventoryMouseButton button);
    // SlotActionType.QUICK_MOVE, the container direction: move as much of
    // `source` (a chest slot, a crafting grid cell, a furnace slot) into the
    // player inventory as fits, leaving whatever does not fit in place.
    void quickMoveInto(ItemStack& source);
    // The mutable reference behind a player-inventory slot index, for the
    // container-shift-move and drag paths that hand a slot to another system.
    [[nodiscard]] ItemStack& mutableSlot(std::size_t index);
    // SlotActionType.QUICK_CRAFT, the drag: `targets` are the slots the cursor
    // swept over, in drag order. Left distributes the cursor stack as evenly as
    // possible, right places a single item in each slot. Slots that cannot take
    // the item are skipped.
    void dragDistribute(std::span<ItemStack*> targets, InventoryMouseButton button);
    // SlotActionType.PICKUP_ALL: pull every stack in `sources` that matches the
    // cursor's item into the cursor, stopping at the stack limit. Does nothing
    // with an empty cursor, exactly like vanilla.
    void gatherAllIntoCursor(std::span<ItemStack*> sources);
    [[nodiscard]] bool mergeIntoCursor(ItemStack stack);
    void clickCreativeItem(
        std::size_t catalogIndex,
        InventoryMouseButton button,
        bool shiftHeld);
    void clickCreativeItem(
        ItemStack catalogStack,
        InventoryMouseButton button,
        bool shiftHeld);
    void stowCursorStack();
    void clearCursorStack() { cursorStack_ = {}; }
    [[nodiscard]] ItemStack takeCursorStack(bool wholeStack = true);
    [[nodiscard]] ItemStack takeSelected(bool wholeStack);
    // ItemUsage#method_30012's stack swap: replaces the selected hotbar slot with
    // a new stack in place — an empty bucket becomes a water bucket, a water
    // bucket reverts to an empty one. The cursor and every other slot are
    // untouched.
    void replaceSelected(ItemStack stack);
    bool consumeSelected(std::uint8_t count = 1U);
    // ItemStack#damage: spends durability on the selected stack and reports
    // whether the tool broke, which is when the caller plays the break sound.
    // Undamageable stacks and empty hands are left alone and report false.
    bool damageSelected(std::uint16_t amount);
    bool add(ItemStack& stack);
    void restore(
        const std::array<ItemStack, kSlotCount>& slots,
        std::size_t selectedHotbarSlot);

  private:
    std::array<ItemStack, kSlotCount> slots_{};
    ItemStack cursorStack_{};
    std::size_t selectedHotbarSlot_ = 0;

    void quickMove(std::size_t index, bool singleItem);
};

} // namespace mc::gameplay
