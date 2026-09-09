#pragma once

// SLP-2: whether the player may lie down in this bed, as a pure decision.
//
// 26.1 spreads the answer over three files — `ServerPlayer#startSleepInBed` does
// the reachability, obstruction and monster checks, `Player#startSleepInBed`
// does the lying down, and `BedRule` (an EnvironmentAttribute since 26.1) says
// whether this dimension allows sleeping at all and whether the bed sets a spawn
// point. The ORDER matters and is easy to get wrong: the spawn point is set
// BEFORE the "can you actually sleep" gate, so in vanilla a bed you cannot sleep
// in may still become your respawn point.
//
// Everything that needs the world is passed in rather than reached for: the
// caller (GameSession) knows the darkness, the game mode and what creatures are
// nearby, and keeping those out of here is what makes the eight-step chain
// testable one step at a time.

#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockPos.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"
#include "world/attribute/EnvironmentAttribute.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>

namespace mc::gameplay {

// `Player.BedSleepingProblem`, plus None for "you are now asleep". The names are
// vanilla's, and each maps to one of its translation keys.
enum class BedSleepProblem : std::uint8_t {
    None,
    // block.minecraft.bed.no_sleep — "you can only sleep at night" (and the
    // dimension rule's own message, which for an exploding bed vanilla leaves
    // empty because the bed answers with a bang instead).
    NotPossibleNow,
    NotPossibleHere,
    TooFarAway,
    Obstructed,
    NotSafe,
    OtherProblem,
};

// What the world knows that the decision needs. Everything here is cheap for
// the caller to have on hand at the moment of the right-click.
struct SleepConditions final {
    world::attribute::BedRule rule = world::attribute::BedRule::CanSleepWhenDark;
    // Level#isDarkOutside: `skyDarken >= 4`, which this build already computes
    // as EnvironmentSnapshot::ambientDarkness. Not "is it night" — a thunder
    // storm darkens the sky enough to sleep through, exactly as in vanilla.
    bool darkOutside = false;
    bool creative = false;
    bool alreadySleeping = false;
    bool alive = true;
    // A Monster within 8 blocks horizontally and 5 vertically of the bed, per
    // ServerPlayer's own AABB. The caller runs the query because it owns the
    // entity list; this only asks for the answer.
    bool monstersNearby = false;
    glm::vec3 playerPosition{0.0F};
};

// The outcome. `setsSpawn` is deliberately independent of `problem`: vanilla
// sets the spawn point before the can-sleep gate, so a bed can refuse the sleep
// and still become your respawn point.
struct SleepDecision final {
    BedSleepProblem problem = BedSleepProblem::None;
    bool setsSpawn = false;
};

namespace detail {

// BedRule.Rule::test — ALWAYS / WHEN_DARK / NEVER against the sky.
[[nodiscard]] constexpr bool bedRuleAllows(bool always, bool never, bool whenDark,
                                           bool darkOutside) {
    if (never) return false;
    if (always) return true;
    return whenDark && darkOutside;
}

// ServerPlayer#isReachableBedBlock: |dx| <= 3, |dy| <= 2, |dz| <= 3 from the
// bottom centre of the cell.
[[nodiscard]] inline bool reachableBedCell(glm::vec3 player, world::BlockPos cell) {
    const glm::vec3 centre{static_cast<float>(cell.x) + 0.5F, static_cast<float>(cell.y),
                           static_cast<float>(cell.z) + 0.5F};
    return std::fabs(player.x - centre.x) <= 3.0F && std::fabs(player.y - centre.y) <= 2.0F &&
           std::fabs(player.z - centre.z) <= 3.0F;
}

// ServerPlayer#bedBlocked: the cell above the bed, and the one above the bed's
// other half, must both be free of collision.
[[nodiscard]] inline bool cellFree(const world::World& world, world::BlockPos cell) {
    const auto state = world.state(cell.x, cell.y, cell.z);
    return world::collisionShape(state).kind == world::ShapeKind::Empty;
}

} // namespace detail

// The reach test, published for the tick that has to notice a player walking
// away from the bed they are in.
namespace detail_sleep = detail;

// The head cell of the bed `pos` belongs to: the head is toward FACING from the
// foot, and is itself the head.
[[nodiscard]] inline world::BlockPos bedHeadCell(world::BlockPos pos, world::BlockState state) {
    if (state.isBedHead()) {
        return pos;
    }
    const auto offset = world::orientationOffset(state.orientation());
    return {pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
}

// The eight-step chain, in vanilla's order.
[[nodiscard]] inline SleepDecision evaluateSleep(const world::World& world, world::BlockPos bedPos,
                                                 world::BlockState bedState,
                                                 const SleepConditions& conditions) {
    using world::attribute::BedRule;
    SleepDecision decision;

    // 1. Already asleep, or dead.
    if (conditions.alreadySleeping || !conditions.alive) {
        decision.problem = BedSleepProblem::OtherProblem;
        return decision;
    }

    // 2. The dimension's BedRule. `Explodes` says NEVER to both halves, which is
    //    how the nether refuses a bed; this build has no explosion mechanic yet,
    //    so the bed simply refuses (registered deviation).
    const bool explodes = conditions.rule == BedRule::Explodes;
    const bool canSleep = detail::bedRuleAllows(
        /*always=*/conditions.rule == BedRule::CanSleepAlways, /*never=*/explodes,
        /*whenDark=*/conditions.rule == BedRule::CanSleepWhenDark, conditions.darkOutside);
    const bool canSetSpawn = !explodes;
    if (!canSleep && !canSetSpawn) {
        decision.problem = BedSleepProblem::NotPossibleHere;
        return decision;
    }

    // 3. Reach: either cell of the bed counts.
    const auto head = bedHeadCell(bedPos, bedState);
    const auto facing = bedState.orientation();
    const auto backward = world::orientationOffset(world::oppositeOrientation(facing));
    const world::BlockPos foot{head.x + backward.x, head.y + backward.y, head.z + backward.z};
    if (!detail::reachableBedCell(conditions.playerPosition, head) &&
        !detail::reachableBedCell(conditions.playerPosition, foot)) {
        decision.problem = BedSleepProblem::TooFarAway;
        return decision;
    }

    // 4. Obstruction above either half.
    if (!detail::cellFree(world, {head.x, head.y + 1, head.z}) ||
        !detail::cellFree(world, {foot.x, foot.y + 1, foot.z})) {
        decision.problem = BedSleepProblem::Obstructed;
        return decision;
    }

    // 5. The spawn point is set HERE — before the can-sleep gate, exactly as in
    //    vanilla, so a daytime click on a bed still moves your respawn.
    decision.setsSpawn = canSetSpawn;

    // 6. Now the sleep itself.
    if (!canSleep) {
        decision.problem = BedSleepProblem::NotPossibleNow;
        return decision;
    }

    // 7. Monsters, unless creative.
    if (!conditions.creative && conditions.monstersNearby) {
        decision.problem = BedSleepProblem::NotSafe;
        return decision;
    }

    // 8. And the bed must not already have someone in it.
    if (bedState.occupied()) {
        decision.problem = BedSleepProblem::OtherProblem;
        return decision;
    }
    return decision;
}

// ServerPlayer's monster AABB: 8 horizontal, 5 vertical, from the bed's centre.
[[nodiscard]] inline bool withinMonsterWakeBox(glm::vec3 bedCentre, glm::vec3 position) {
    return std::fabs(position.x - bedCentre.x) <= 8.0F &&
           std::fabs(position.y - bedCentre.y) <= 5.0F &&
           std::fabs(position.z - bedCentre.z) <= 8.0F;
}

// Player#isSleepingLongEnough: a hundred ticks in bed is what lets the night be
// skipped. Both halves of that — the counter and the threshold — are vanilla's.
inline constexpr int kSleepTicksBeforeSkip = 100;

} // namespace mc::gameplay
