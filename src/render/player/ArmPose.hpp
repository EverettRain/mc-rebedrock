#pragma once

// The pose family an arm holds this frame — 26.1's HumanoidModel.ArmPose. Which
// pose an arm shows is a pure function of the held stack and the active use (see
// deriveArmPose), so both the third-person solver and the first-person hand
// solver read the same value instead of each guessing from the item.
//
// Vulkan-free: this is render-side presentation vocabulary, but it depends only
// on gameplay item/use identity, so it lives in the runtime library and is
// headless-testable.

#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerActionState.hpp"

#include <cstdint>

namespace mc::render::player {

enum class ArmPose : std::uint8_t {
    Empty,          // no item: idle bob + walk swing
    Item,           // a flat/tool item held out
    Block,          // a placeable block, held slightly differently
    Eat,            // raising food/drink to the mouth (use in progress)
    Bow,            // drawing a bow (reserved; content not yet present)
    Spear,          // charging a trident (reserved)
    Crossbow,       // holding/charging a crossbow (reserved)
    Spyglass,       // spyglass to the eye (reserved)
    Horn,           // goat horn (reserved)
    Brush,          // brushing (reserved)
};

// Derives the arm pose for the hand holding `stack`, given the active use (if
// this hand is the one using an item). Pure and total: an empty stack is Empty,
// a block is Block, an eaten/drunk item during a use is Eat, everything else is
// Item. Reserved poses are mapped once their content lands (Phase 6); until then
// the derivation never emits them, so the solvers need no dead branches.
[[nodiscard]] inline ArmPose deriveArmPose(const gameplay::ItemStack& stack, bool usingThisHand,
                                           gameplay::UseAnimation use) noexcept {
    if (usingThisHand && (use == gameplay::UseAnimation::Eat ||
                          use == gameplay::UseAnimation::Drink)) {
        return ArmPose::Eat;
    }
    if (stack.empty()) {
        return ArmPose::Empty;
    }
    if (gameplay::isBlockStack(stack)) {
        return ArmPose::Block;
    }
    return ArmPose::Item;
}

}  // namespace mc::render::player
