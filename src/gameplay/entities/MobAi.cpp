#include "gameplay/entities/MobAi.hpp"

#include "gameplay/entities/MobBrain.hpp"

#include <memory>

namespace mc::gameplay::entities {

void AnimalAi::configureBrain(MobBrain& brain) const {
    // CowEntity/PigEntity 1.16.1 common land-animal core. Mating, temptation
    // and following parents can be inserted at priorities 2..4 later without
    // changing the selector or navigation contract.
    brain.goals().add(0, std::make_unique<SwimGoal>());
    brain.goals().add(1, std::make_unique<EscapeDangerGoal>(escapeSpeedMultiplier_));
    brain.goals().add(5 + passiveGoalPriorityOffset_,
                      std::make_unique<WanderAroundFarGoal>(wanderSpeedMultiplier_));
    brain.goals().add(6 + passiveGoalPriorityOffset_, std::make_unique<LookAtPlayerGoal>(6.0F));
    brain.goals().add(7 + passiveGoalPriorityOffset_, std::make_unique<LookAroundGoal>());
}

void MonsterAi::configureBrain(MobBrain& brain) const {
    // Shared idle fallback for hostile species. Concrete monsters install their
    // own target and combat goals above these lower-priority actions.
    brain.goals().add(7, std::make_unique<WanderAroundFarGoal>(1.0F));
    brain.goals().add(8, std::make_unique<LookAtPlayerGoal>(8.0F));
    brain.goals().add(8, std::make_unique<LookAroundGoal>());
}

} // namespace mc::gameplay::entities
