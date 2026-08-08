#include "gameplay/entities/MobAi.hpp"

#include "gameplay/EntitySystem.hpp"

namespace mc::gameplay::entities {

void AnimalAi::chooseWanderIntent(SimpleEntity& self, std::uint32_t& rng) const {
    // Schedule the next decision two to five seconds out, then either graze in
    // place (one time in four) or amble off on a fresh random heading.
    self.wanderTimer = 40U + (nextRandom(rng) % 60U);
    if (randomUnit(rng) < 0.25F) {
        self.moving = false;
    } else {
        self.moving = true;
        self.yaw = randomUnit(rng) * kTwoPi;
    }
}

void MonsterAi::chooseWanderIntent(SimpleEntity& self, std::uint32_t& rng) const {
    // Only a rare pause; a monster keeps drifting toward wherever it faces.
    self.wanderTimer = 30U + (nextRandom(rng) % 40U);
    if (randomUnit(rng) < 0.10F) {
        self.moving = false;
    } else {
        self.moving = true;
        self.yaw = randomUnit(rng) * kTwoPi;
    }
}

} // namespace mc::gameplay::entities
