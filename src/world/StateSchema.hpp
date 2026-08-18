#pragma once

// StateDefinition, as the general property list it is in vanilla.
//
// What this replaces: three hand-rolled axes. `blockStateSlotCount()` used to
// branch on BlockDefinition's flags — crops and farmland 8, leaves 2, pillars
// 6, horizontal facing 4, everything else 1 — and the axis it produced *was*
// `BlockOrientation`. Two things fell out of that:
//
//   1. `BlockOrientation` has six enumerators and crops stored 0-7 in it, read
//      back with `& 0x7`. The enum's name was already false for wheat.
//   2. There was nowhere to put a fourth axis. One stair needs
//      facing(4) x half(2) x shape(5) x waterlogged(2) = 80 states, and no
//      amount of Block enum members buys that — spending one member per
//      combination is exactly what the interned table took back from the lit
//      furnace and the four wall torches.
//
// A block now declares which properties it has and how many values each takes.
// The state id inside the block's range is a mixed-radix number over that list,
// which is the cartesian product vanilla's StateDefinition builds. Adding a
// fourth property is adding an enumerator here and one call on the block's
// builder chain; nothing in the encoder, the section storage or the save layer
// has to learn about it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::world {

// The properties a block state can carry. Names mirror vanilla's
// `BlockStateProperties` so a data-driven definition can be recognised later.
enum class StateProperty : std::uint8_t {
    Facing,     // DirectionProperty FACING / AXIS, stored as a BlockOrientation
    Age,        // CropBlock.AGE, 0-7
    Moisture,   // FarmlandBlock.MOISTURE, 0-7
    Persistent, // LeavesBlock.PERSISTENT
    Lit,        // AbstractFurnaceBlock.LIT
    FluidLevel, // LiquidBlock.LEVEL, 0-8
    SlabType,   // SlabBlock.TYPE, bottom/top/double
    Count,
};

inline constexpr std::size_t kStatePropertyCount = static_cast<std::size_t>(StateProperty::Count);

// The name a property serialises under. This is the whole reason a save can
// gain a property without gaining a format: a reader matches on the name, skips
// what it does not know, and defaults what it was not told.
[[nodiscard]] constexpr std::string_view statePropertyName(StateProperty property) {
    switch (property) {
    case StateProperty::Facing:
        return "facing";
    case StateProperty::Age:
        return "age";
    case StateProperty::Moisture:
        return "moisture";
    case StateProperty::Persistent:
        return "persistent";
    case StateProperty::Lit:
        return "lit";
    case StateProperty::FluidLevel:
        return "level";
    case StateProperty::SlabType:
        return "type";
    case StateProperty::Count:
        break;
    }
    return {};
}

// The inverse, for the save reader. Returns Count for a name this build has no
// property for — an older or newer save naming something unknown, which is
// skipped rather than refused.
[[nodiscard]] constexpr StateProperty statePropertyFromName(std::string_view name) {
    for (std::size_t index = 0; index < kStatePropertyCount; ++index) {
        const auto property = static_cast<StateProperty>(index);
        if (statePropertyName(property) == name) {
            return property;
        }
    }
    return StateProperty::Count;
}

// How many independent properties one block may declare. Six covers the widest
// vanilla shapes this project is heading for: a stair is four (facing, half,
// shape, waterlogged) and a door is five or six.
inline constexpr std::size_t kMaximumStateProperties = 6U;

struct StateAxis final {
    StateProperty property = StateProperty::Facing;
    std::uint8_t valueCount = 1U;
};

// One block's declared property list, and the mixed-radix arithmetic over it.
//
// Declaration order is the digit order, most significant first. It is part of
// the *runtime* numbering only: the save layer writes property names, never
// digits, so reordering a block's properties cannot corrupt a world.
class StateSchema final {
  public:
    constexpr void add(StateProperty property, std::uint8_t valueCount) {
        if (count_ >= kMaximumStateProperties || valueCount == 0U || has(property)) {
            return;
        }
        axes_[count_++] = {property, valueCount};
    }

    [[nodiscard]] constexpr std::size_t size() const { return count_; }
    [[nodiscard]] constexpr StateAxis axis(std::size_t index) const { return axes_[index]; }

    [[nodiscard]] constexpr bool has(StateProperty property) const {
        for (std::size_t index = 0; index < count_; ++index) {
            if (axes_[index].property == property) {
                return true;
            }
        }
        return false;
    }

    // How many values this block's property takes, or 1 for a property it does
    // not declare — so an absent property is a single-valued axis that
    // contributes nothing to the product and always reads back as 0.
    [[nodiscard]] constexpr std::uint8_t valueCount(StateProperty property) const {
        for (std::size_t index = 0; index < count_; ++index) {
            if (axes_[index].property == property) {
                return axes_[index].valueCount;
            }
        }
        return 1U;
    }

    // The product: how many states this block has.
    [[nodiscard]] constexpr std::uint16_t stateCount() const {
        std::uint32_t total = 1U;
        for (std::size_t index = 0; index < count_; ++index) {
            total *= axes_[index].valueCount;
        }
        return static_cast<std::uint16_t>(total);
    }

    // The place value of one digit: the product of every axis after it.
    [[nodiscard]] constexpr std::uint16_t stride(std::size_t index) const {
        std::uint32_t total = 1U;
        for (std::size_t after = index + 1U; after < count_; ++after) {
            total *= axes_[after].valueCount;
        }
        return static_cast<std::uint16_t>(total);
    }

    [[nodiscard]] constexpr std::uint16_t strideOf(StateProperty property) const {
        for (std::size_t index = 0; index < count_; ++index) {
            if (axes_[index].property == property) {
                return stride(index);
            }
        }
        return 0U;
    }

  private:
    std::array<StateAxis, kMaximumStateProperties> axes_{};
    std::size_t count_ = 0U;
};

} // namespace mc::world
