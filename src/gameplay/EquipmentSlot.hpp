#pragma once

// The EquipmentSlot enum, split out of Equipment.hpp so gameplay/Item.hpp can
// see it too: EQ-0's armor items need to declare which slot they occupy, but
// Item.hpp sits BELOW Equipment.hpp in the include graph (Equipment.hpp
// already includes Inventory.hpp, which includes Item.hpp) — so Item.hpp
// including Equipment.hpp back would be circular. This header holds exactly
// the acyclic part (the enum + the small constexpr tables that only depend on
// it), and Equipment.hpp includes it and keeps re-exporting the same names
// from the same namespace, so every existing `gameplay::EquipmentSlot` call
// site is untouched — this is a location split, not a redefinition (the EQ-0
// brief's "reuse the existing enum" rule holds: the enum's values, ordering
// and semantics are exactly what EQ-1's storage lane declared).

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::gameplay {

// Java 1.16.1's net.minecraft.entity.EquipmentSlot enum, minus MAINHAND
// (which stays Inventory's selected hotbar slot — see Equipment.hpp's file
// banner for why). The declaration order below mirrors vanilla's own enum
// order exactly (MAINHAND, OFFHAND, FEET, LEGS, CHEST, HEAD) with MAINHAND
// dropped, so a JC reader lining this up against the vanilla source sees the
// same relative ordering rather than a reshuffled one.
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

// The four armor slots, in the same head-to-feet order the armor renderer and
// EQ-2's damage-reduction formula will want to walk (matching vanilla's own
// iteration order in LivingEntity#getArmorSlots, which walks FEET..HEAD).
inline constexpr std::array<EquipmentSlot, 4U> kArmorSlots{
    EquipmentSlot::Feet,
    EquipmentSlot::Legs,
    EquipmentSlot::Chest,
    EquipmentSlot::Head,
};

[[nodiscard]] constexpr bool isArmorSlot(EquipmentSlot slot) {
    return slot != EquipmentSlot::Offhand;
}

} // namespace mc::gameplay
