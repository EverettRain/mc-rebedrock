// N2's multi-player acceptance: two ServerPlayer slots in GameSession are fully
// independent — hurting or respawning one never touches the other's vitals,
// inventory or spawn point.

#include "gameplay/GameSession.hpp"
#include "world/Block.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>

using namespace mc;

namespace {

struct TestHost final : gameplay::SimulationHost {
    int playerHurts = 0;
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override { ++playerHurts; }
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

} // namespace

int main() {
    constexpr gameplay::PlayerId second = 2U;

    gameplay::GameSession session;
    // A second connected player, independent of the primary.
    session.players().emplace(second, gameplay::ServerPlayer{glm::vec3{8.0F, 1.0F, 8.0F}});
    auto& p1 = session.players().at(gameplay::kPrimaryPlayerId);
    auto& p2 = session.players().at(second);

    // Inventories are separate: an item in one player's slot is absent from the
    // other's.
    p1.inventory.mutableSlot(0) = {world::Block::Stone, 5U, nullptr};
    assert(p2.inventory.mutableSlot(0).empty());
    assert(!p1.inventory.mutableSlot(0).empty());

    // Vitals are separate: hurting the second player leaves the primary's
    // health untouched.
    TestHost host;
    const float p1Health = p1.vitals.health();
    const float p2Health = p2.vitals.health();
    assert(session.hurtPlayer(second, gameplay::DamageType::Fall, 5.0F, host));
    assert(p1.vitals.health() == p1Health);
    assert(p2.vitals.health() < p2Health);  // the second player took the hit
    session.drainEvents();
    assert(host.playerHurts == 1);

    // Respawn uses the player's own spawn point, not the other's.
    p1.hasSpawn = true;
    p1.spawnPosition = {10.0F, 1.0F, 10.0F};
    p2.hasSpawn = true;
    p2.spawnPosition = {-3.0F, 1.0F, -3.0F};
    session.respawn(gameplay::kPrimaryPlayerId);
    const auto p1Feet = p1.controller.position();
    assert(std::abs(p1Feet.x - 10.0F) < 0.01F);
    assert(std::abs(p1Feet.z - 10.0F) < 0.01F);
    const auto p2Feet = p2.controller.position();
    assert(std::abs(p2Feet.x - 8.0F) < 0.01F);  // untouched by p1's respawn
    assert(std::abs(p2Feet.z - 8.0F) < 0.01F);

    // Respawn of the SECOND player uses ITS spawn point, leaving the primary
    // exactly where it was (this catches respawn hardcoding the primary).
    session.respawn(second);
    assert(std::abs(p2.controller.position().x - -3.0F) < 0.01F);
    assert(std::abs(p2.controller.position().z - -3.0F) < 0.01F);
    assert(std::abs(p1.controller.position().x - 10.0F) < 0.01F);
    assert(std::abs(p1.controller.position().z - 10.0F) < 0.01F);

    return 0;
}
