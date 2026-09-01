#pragma once

#include "gameplay/Item.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace mc::gameplay {

struct ItemStack;

// Defined after ItemStack (it reads the stack's fields); both forms of a block
// stack — the legacy null item pointer or the block's own BlockItem — count.
[[nodiscard]] constexpr bool isBlockStack(const ItemStack& stack);

// ENCH-0: one enchantment carried by a stack. `EnchantmentId` is declared in
// gameplay/Enchantment.hpp, which is deliberately NOT included here — this
// header only needs a POD-storable id, and Enchantment.hpp itself does not
// depend on ItemStack, so Inventory.hpp would otherwise be the one taking on
// a (currently) one-way include for no reason. The id is stored as its raw
// underlying type to avoid the include; every reader casts it back.
using EnchantmentIdStorage = std::uint8_t;

struct EnchantmentInstance final {
    EnchantmentIdStorage id = 0U;
    std::uint8_t level = 0U;

    [[nodiscard]] constexpr bool operator==(const EnchantmentInstance&) const = default;
};

// A deliberate vanilla deviation (JC debt, see Enchantment.hpp's banner): a
// fixed inline capacity instead of an unbounded list, so ItemStack stays
// trivially-copyable/heap-free on the hot inventory paths (DOD: no
// allocation on a slot click, a stack split, a save-buffer append). 34
// vanilla enchantments exist today and only a handful can ever be
// simultaneously compatible on one item (the exclusivity table in
// Enchantment.hpp guarantees at most one member of each mutually-exclusive
// family survives a real application), so 12 is generous headroom, not a
// realistic ceiling — a legitimately over-full "give"-command or a corrupt
// save simply drops enchantments past the cap rather than growing the
// struct's footprint for a case that cannot occur through play.
inline constexpr std::size_t kMaxEnchantmentsPerStack = 12U;

struct ItemStack final {
    world::Block block = world::Block::Air;
    std::uint8_t count = 0;
    // The stack's identity. nullptr means "this stack is its block" (the old
    // ItemKind::Block sentinel); otherwise it points at the registered Item.
    const Item* item = nullptr;
    // ItemStack#getDamage: how much of the tool's durability has been spent.
    // Zero for everything that has no durability at all.
    std::uint16_t damage = 0;
    // ENCH-0: the stack's enchantments, inline (no heap) and order-independent
    // (two stacks carrying the same set in a different order are the same
    // stack — see enchantmentsEqual below, used by operator== and sameItem).
    std::array<EnchantmentInstance, kMaxEnchantmentsPerStack> enchantments{};
    std::uint8_t enchantmentCount = 0U;
    // ENCH-3: DataComponents.REPAIR_COST — the anvil's "prior work penalty".
    // Every anvil operation doubles it (2n+1), and it is added on top of the
    // next operation's price, which is what makes repeatedly anvilling one item
    // get exponentially more expensive until the 40-level wall refuses it
    // outright. Without it an item could be combined without limit.
    //
    // A byte, saturating at 255, is exact rather than a compromise: the anvil
    // refuses any operation costing 40 or more levels, so once this passes 40
    // the item is permanently too expensive and no larger value is ever
    // observable. The doubling sequence reaches that wall on the sixth
    // operation (0,1,3,7,15,31,63) — long before the byte does.
    std::uint8_t repairCost = 0U;
    // I-3: the anvil's rename (and, later, a name tag's). A u16 index into the
    // session's CustomNameTable rather than the string itself — ItemStack had
    // exactly four bytes of tail padding, so this costs NOTHING: sizeof stays
    // 48. A 32-byte inline array would have made it 80 (+67% on every stack in
    // every inventory and chest) and still could not hold vanilla's 50-character
    // limit. See docs/content-dev/I-item/I-3-custom-name-storage-decision.md.
    //
    // Deliberately NOT resolved here: Inventory.hpp is a value type and must not
    // depend on a session service. gameplay/CustomNames.hpp turns the id back
    // into a string.
    std::uint16_t customNameId = 0U;

    [[nodiscard]] constexpr bool empty() const {
        return count == 0 || (item == nullptr && block == world::Block::Air);
    }

    // Looks up this stack's level for `id` (0 = not enchanted with it). Takes
    // the raw storage id so this header need not include Enchantment.hpp;
    // gameplay/Enchantment.hpp provides the typed convenience wrapper.
    [[nodiscard]] constexpr std::uint8_t enchantmentLevelRaw(EnchantmentIdStorage id) const {
        for (std::uint8_t index = 0; index < enchantmentCount; ++index) {
            if (enchantments[index].id == id) return enchantments[index].level;
        }
        return 0U;
    }

    // Sets (or clears, for level 0) `id`'s level. A no-op past the fixed
    // capacity (see kMaxEnchantmentsPerStack's comment) rather than UB.
    constexpr void setEnchantmentRaw(EnchantmentIdStorage id, std::uint8_t level) {
        for (std::uint8_t index = 0; index < enchantmentCount; ++index) {
            if (enchantments[index].id == id) {
                if (level == 0U) {
                    // Swap-erase: order does not matter (see enchantmentsEqual).
                    enchantments[index] = enchantments[enchantmentCount - 1U];
                    enchantments[enchantmentCount - 1U] = EnchantmentInstance{};
                    --enchantmentCount;
                } else {
                    enchantments[index].level = level;
                }
                return;
            }
        }
        if (level == 0U || enchantmentCount >= enchantments.size()) {
            return;
        }
        enchantments[enchantmentCount] = EnchantmentInstance{id, level};
        ++enchantmentCount;
    }
};

// Two stacks' enchantment sets match when they carry the same (id, level)
// pairs, independent of storage order (setEnchantmentRaw's swap-erase does
// not preserve insertion order, and neither does a save/net round trip that
// writes them in a different sequence than they were applied in).
[[nodiscard]] constexpr bool enchantmentsEqual(const ItemStack& first, const ItemStack& second) {
    if (first.enchantmentCount != second.enchantmentCount) return false;
    for (std::uint8_t index = 0; index < first.enchantmentCount; ++index) {
        const auto& entry = first.enchantments[index];
        if (second.enchantmentLevelRaw(entry.id) != entry.level) return false;
    }
    return true;
}

// ItemStack#equals. Two block stacks compare by their block alone: a stack
// that still carries the legacy null item pointer is the same stone stack as
// one that points at stone's BlockItem. Item stacks compare by item.
// Enchantments participate in equality for every stack shape: an enchanted
// item is never the "same" stack as its unenchanted (or differently
// enchanted) twin, matching vanilla's NBT-inclusive ItemStack#equals (and,
// separately, matching the vanilla RULE that enchanted items do not stack —
// canCombine below is what actually enforces non-merging; this operator is
// the general-purpose value comparison callers such as tests reach for).
[[nodiscard]] constexpr bool operator==(const ItemStack& first, const ItemStack& second) {
    if (first.count != second.count || first.damage != second.damage) return false;
    if (first.repairCost != second.repairCost) return false;
    // The name is part of the stack's value: a renamed sword is not the same
    // stack as its unnamed twin (and, through canCombine, does not merge with
    // it — matching vanilla's component-inclusive ItemStack#matches).
    if (first.customNameId != second.customNameId) return false;
    if (!enchantmentsEqual(first, second)) return false;
    if (isBlockStack(first) && isBlockStack(second)) return first.block == second.block;
    return first.item == second.item && first.block == second.block;
}

static_assert(std::is_trivially_copyable_v<ItemStack>,
              "ItemStack must stay trivially copyable — no heap on the hot inventory paths");

// A stack is a block stack when it names a real block and its item is either the
// legacy null pointer or the block's own BlockItem. The block field stays the
// single source of truth for a block stack's identity.
[[nodiscard]] constexpr bool isBlockStack(const ItemStack& stack) {
    return stack.block != world::Block::Air && stack.count > 0 &&
        (stack.item == nullptr || asBlockItem(stack.item) != nullptr);
}

// ItemStack#getMaxDamage: the tool material's durability, or the armor
// material's per-slot durability for armor (EQ-0), or zero for anything that
// never wears out. A stack is never both a tool and armor (toolType and
// armorMaterial are set by mutually distinct builder calls — tool() vs
// armor() — on disjoint Item constants), so checking armor first when
// toolType is None is unambiguous.
[[nodiscard]] constexpr std::uint16_t itemMaximumDamage(const ItemStack& stack) {
    if (stack.item == nullptr) return 0U;
    if (stack.item->armorMaterial != ArmorMaterialId::None) {
        return armorAttributes(stack.item->armorMaterial, stack.item->armorSlot).durability;
    }
    return toolAttributes(stack.item->toolType, stack.item->toolTier).durability;
}

[[nodiscard]] constexpr bool isDamageable(const ItemStack& stack) {
    return itemMaximumDamage(stack) > 0U;
}

// ItemStack#isEnchanted: 栈上带着至少一条**已生效**的附魔。
// 附魔书是 vanilla 里唯一的例外，见 itemRarity 的注释。
[[nodiscard]] constexpr bool isEnchanted(const ItemStack& stack) {
    return stack.enchantmentCount > 0U;
}

// I-2 / ItemStack#getRarity(:968)：名称行的着色档 = 物品的基础档，附魔再升一档
// （COMMON/UNCOMMON → RARE，RARE → EPIC，EPIC 封顶）。
//
// 附魔书不升档：vanilla 的附魔书把附魔存在 STORED_ENCHANTMENTS 而不是
// ENCHANTMENTS 上，`isEnchanted()` 因此对它为假，它停在自己的 RARE（青色）而
// 不会变成 EPIC。本项目只有一个附魔字段，所以这条区别落在这里的身份判断上——
// 这是"不为对齐凭空造组件系统"的代价，也是它唯一的落点。
[[nodiscard]] inline Rarity itemRarity(const ItemStack& stack) {
    const Rarity base = stack.item == nullptr ? Rarity::Common : stack.item->itemRarity;
    if (!isEnchanted(stack) || stack.item == &items::EnchantedBook) {
        return base;
    }
    switch (base) {
    case Rarity::Common:
    case Rarity::Uncommon:
        return Rarity::Rare;
    case Rarity::Rare:
        return Rarity::Epic;
    case Rarity::Epic:
        return Rarity::Epic;
    }
    return base;
}

// EQ-1: EquipmentSlot#canEquip's filter (its actual home in vanilla is
// per-slot: LivingEntity#getEquipmentSlotForItem consults
// EquipmentSlot.MAINHAND/OFFHAND/ARMOR's own canEquip predicate, and armor's
// slot-matching test is ArmorItem#getSlotType == the target slot). The
// offhand takes anything (vanilla's OFFHAND slot has no canEquip restriction
// at all — it is the one slot every item type may occupy); an armor slot
// takes only the armor whose own body slot matches, and only armor (a
// pickaxe can never be "worn" in the head slot even though nothing else
// would want it there). This is the single source both the click-slot filter
// (ScreenHandler) and the auto-equip right-click path (PlayerInteraction)
// read, so they can never disagree about what a given slot accepts.
[[nodiscard]] constexpr bool canEquip(EquipmentSlot slot, const ItemStack& stack) {
    if (slot == EquipmentSlot::Offhand) {
        return true;
    }
    return isArmor(stack.item) && armorSlotOf(stack.item) == slot;
}

// ItemStack#canCombine, which compares the damage too: a half-worn pickaxe is
// not the same item as a fresh one. Block stacks combine by block, whatever form
// their item pointer takes; every other stack needs a matching item (or a
// matching block for the legacy null-item sentinel, which keeps an empty stack
// distinct from a block stack). ENCH-0: also compares enchantments, so a
// Sharpness sword never merges with a plain one (vanilla's enchanted items are
// unstackable in the first place via maximumStackSize==1, but the comparison
// itself must still refuse two DIFFERENTLY-enchanted stacks of something that
// could otherwise stack, such as two enchanted books).
[[nodiscard]] constexpr bool sameItem(const ItemStack& first, const ItemStack& second) {
    if (first.damage != second.damage) return false;
    if (!enchantmentsEqual(first, second)) return false;
    if (isBlockStack(first) && isBlockStack(second)) return first.block == second.block;
    return first.item == second.item &&
        (first.item != nullptr || first.block == second.block);
}

[[nodiscard]] constexpr std::uint8_t itemMaximumStackSize(const ItemStack& stack) {
    return stack.item == nullptr
        ? world::blockDefinition(stack.block).maximumStackSize
        : stack.item->maximumStackSize;
}

// The stable language-resource key for a stack. Legacy block stacks without an
// Item pointer are normalized through the registered BlockItem first.
[[nodiscard]] inline DescriptionId itemDescriptionId(const ItemStack& stack) {
    if (stack.item != nullptr) return stack.item->descriptionId();
    if (isBlockStack(stack)) {
        if (const Item* item = blockItemFor(stack.block); item != nullptr) {
            return item->descriptionId();
        }
    }
    return DescriptionId{};
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
    // RW-1: PlayerEntity#getArrowType's inventory scan — a bow does not need
    // an arrow in the SELECTED slot, only somewhere in the 36-slot inventory
    // (hotbar first, since that mirrors the scan order vanilla's own
    // `for (i = 0; i < this.inventory.size(); i++)` walks: hotbar slots occupy
    // Java's low indices too). Returns the slot index of the first matching
    // stack, or nullopt if none carries one.
    [[nodiscard]] std::optional<std::size_t> findFirstArrowSlot() const;
    // Spends one item from the given slot (the arrow slot findFirstArrowSlot
    // located), the same shrink-to-empty rule consumeSelected applies to the
    // selected slot.
    bool consumeSlot(std::size_t index, std::uint8_t count = 1U);
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
