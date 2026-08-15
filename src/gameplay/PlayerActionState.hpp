#pragma once

// The player's action timeline — the swing arc and the ongoing item use — as
// gameplay state rather than animation-internal state. The simulation advances
// it once per tick, so the same operation consumes the same number of ticks at
// any frame rate, and every consumer (first person, third person, a future
// remote player) reads the same snapshot instead of each animator guessing.
//
// Mirrors 26.1's Entity.actionSequence + LivingEntity#swing / useItem: the swing
// only restarts once the current arc is past halfway (the vanilla cadence for a
// held dig), the use countdown is a whole number of ticks, and `sequence`
// distinguishes two consecutive actions so a renderer never has to guess from a
// progress value going backwards.

#include <cstdint>

namespace mc::gameplay {

// A hand slot, independent of which physical arm renders it. ReBedrock only has
// the main hand today, but the type exists so a future off-hand does not
// require renaming every call site.
enum class InteractionHand : std::uint8_t {
    Main,
    Off,
};

// The physical arm that swings. Kept separate from InteractionHand for the day
// a left-handed option exists.
enum class HumanoidArm : std::uint8_t {
    Left,
    Right,
};

// The ongoing item use, 26.1's UseAnim. Eating is just one value here, not a
// special lifetime in the first-person animator.
enum class UseAnimation : std::uint8_t {
    None,
    Eat,
    Drink,
    Block,
    Bow,
    Trident,
    Crossbow,
    Spyglass,
    Horn,
    Brush,
    Spear,
};

// The swing's shape, carrying the arc distinction the renderer's two held-item
// clips express: a mining/attack swing is the larger Break arc, a placement/
// bucket swing the smaller Use arc. A future thrust can extend this.
enum class SwingAnimation : std::uint8_t {
    None,
    Break,
    Use,
};

// One arm swing. `progress` is the arm's position along the arc, in [0, 1],
// advanced once per tick — never per frame. The mining progress of the block
// being dug is a separate quantity (MiningSystem) and must not be confused with
// this (unified-player-animation-pipeline-analysis.md §12.3).
struct SwingState final {
    bool active = false;
    InteractionHand hand = InteractionHand::Main;
    SwingAnimation animation = SwingAnimation::Break;
    // Distinguishes two consecutive actions: a renderer compares this instead
    // of guessing from progress going backwards.
    std::uint64_t sequence = 0U;
    std::uint64_t startedTick = 0U;
    std::uint32_t durationTicks = 6U;
    std::uint32_t elapsedTicks = 0U;
    float previousProgress = 0.0F;
    float progress = 0.0F;
};

// The ongoing item use (eating, a charged bow...). Counts down in whole ticks.
struct ItemUseState final {
    bool active = false;
    InteractionHand hand = InteractionHand::Main;
    UseAnimation animation = UseAnimation::None;
    std::uint64_t startedTick = 0U;
    std::uint32_t durationTicks = 0U;
    std::uint32_t remainingTicks = 0U;
    std::uint32_t previousRemainingTicks = 0U;
};

// Owns the swing and use timelines. The interaction controller calls tick()
// once per server tick and the semantic actions (swingHand / startUsing /
// stopUsing) from the interaction branches; nothing else mutates it.
struct PlayerActionState final {
    SwingState swing;
    ItemUseState use;

    // Starts an arm swing. Restarts only if the current arc is past halfway,
    // so a held dig keeps the vanilla cadence instead of snapping back on
    // every call. Bumps `sequence`.
    void swingHand(InteractionHand hand, SwingAnimation animation, std::uint32_t durationTicks);
    // Starts an item use. Returns false if one is already active (no restart).
    bool startUsing(InteractionHand hand, UseAnimation animation, std::uint32_t durationTicks);
    // Cancels any ongoing use (attack interrupts eating, the meal finishes...).
    void stopUsing();
    // Advances the server tick and the active timelines. Call once per tick.
    void tick();

    [[nodiscard]] std::uint64_t serverTick() const { return serverTick_; }

  private:
    std::uint64_t serverTick_ = 0U;
    std::uint64_t swingSequence_ = 0U;
};

} // namespace mc::gameplay
