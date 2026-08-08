#include "gameplay/entities/NeutralMob.hpp"

#include "gameplay/EntitySystem.hpp"

#include <algorithm>

namespace mc::gameplay::entities {

int NeutralAi::chooseAngerTime(std::uint32_t& rng) const {
    // 400–799 ticks, the vanilla 20–39 second window.
    return 400 + static_cast<int>(nextRandom(rng) % 400U);
}

void NeutralAi::onAttacked(SimpleEntity& self, std::uint32_t& rng) const {
    // Turn hostile for a fresh spell; a longer-running anger is not cut short.
    self.angerTicks = std::max(self.angerTicks, chooseAngerTime(rng));
}

} // namespace mc::gameplay::entities
