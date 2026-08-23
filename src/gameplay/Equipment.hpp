#pragma once

// EQ-0: the equipment slot system's storage — the four armor slots plus the
// offhand slot, as fixed ItemStack fields on the player. No armor items exist
// yet (EQ-1 in the docs' own numbering; this node's prompt calls the whole
// scope EQ-0), no damage-reduction formula (a future EQ-2), no equip
// interaction, no HUD/rendering — this header is pure storage + query, the
// seam those later nodes read from.
//
// Mainhand is deliberately NOT one of these slots: it already exists as
// Inventory::selectedHotbarSlot()/selectedStack(), the vanilla
// PlayerInventory#getSelected equivalent. Duplicating it here would give the
// mainhand two sources of truth.

#include "gameplay/Inventory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mc::gameplay {

// Java 1.16.1's net.minecraft.entity.EquipmentSlot enum, minus MAINHAND
// (which stays Inventory's selected hotbar slot — see the file banner). The
// declaration order below mirrors vanilla's own enum order exactly
// (MAINHAND, OFFHAND, FEET, LEGS, CHEST, HEAD) with MAINHAND dropped, so a
// JC reader lining this up against the vanilla source sees the same
// relative ordering rather than a reshuffled one.
//
// Dense and zero-based (DOD): a fixed-size array indexes directly by this
// enum, no map, no heap.
enum class EquipmentSlot : std::uint8_t {
    Offhand = 0U,
    Feet = 1U,
    Legs = 2U,
    Chest = 3U,
    Head = 4U,
};

inline constexpr std::size_t kEquipmentSlotCount = 5U;

// The four armor slots, in the same head-to-feet order the future armor
// renderer and EQ-2's damage-reduction formula will want to walk (matching
// vanilla's own iteration order in LivingEntity#getArmorSlots, which walks
// FEET..HEAD — see armorSlotsInWearOrder below for that exact order; this
// array is simply "the slots that are armor" for iteration/validation).
inline constexpr std::array<EquipmentSlot, 4U> kArmorSlots{
    EquipmentSlot::Feet,
    EquipmentSlot::Legs,
    EquipmentSlot::Chest,
    EquipmentSlot::Head,
};

[[nodiscard]] constexpr bool isArmorSlot(EquipmentSlot slot) {
    return slot != EquipmentSlot::Offhand;
}

// The player's five equipment slots (armor x4 + offhand), owned as a small
// companion to Inventory rather than folded into Inventory's own slot array:
// Inventory's kSlotCount (hotbar + main) is the "carried items" container
// vanilla's PlayerInventory#items/#offhand split already keeps as parallel
// arrays, not one combined list, and armor/offhand have a completely
// different access pattern (five stable named slots, never scrolled,
// selected, or clicked-and-shifted through the hotbar machinery) — so a
// dedicated fixed-size holder is the cleaner fit than growing Inventory's
// slot count and teaching every hotbar-indexed path to skip five new slots.
class EquipmentSlots final {
  public:
    [[nodiscard]] const ItemStack& get(EquipmentSlot slot) const {
        return slots_[static_cast<std::size_t>(slot)];
    }

    void set(EquipmentSlot slot, ItemStack stack) {
        slots_[static_cast<std::size_t>(slot)] = stack;
    }

    // The seam EQ-1 (armor items existing at all) and the future EQ-2
    // (armor damage-reduction formula, plugging into Damage.hpp's still-empty
    // "--- armor / toughness ---" stage) both read through this: "whatever is
    // currently worn in `slot`". Storage-identical to get() today — the
    // separate name is what lets EQ-2 grep for "the armor read" without
    // caring that, mechanically, it is just the slot accessor.
    [[nodiscard]] const ItemStack& equippedArmor(EquipmentSlot slot) const {
        return get(slot);
    }

    [[nodiscard]] const std::array<ItemStack, kEquipmentSlotCount>& slots() const {
        return slots_;
    }

    // Save load / snapshot decode: restores every slot verbatim (values, not
    // deltas), the same "trust the persisted/wire state outright" contract
    // Inventory::restore and PlayerExperience::restore follow.
    void restore(const std::array<ItemStack, kEquipmentSlotCount>& slots) { slots_ = slots; }

  private:
    std::array<ItemStack, kEquipmentSlotCount> slots_{};
};

static_assert(std::is_trivially_copyable_v<EquipmentSlots>,
              "EquipmentSlots must stay trivially copyable — no heap, mirrors ItemStack");

} // namespace mc::gameplay
