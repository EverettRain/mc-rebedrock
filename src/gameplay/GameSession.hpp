#pragma once

#include "gameplay/ChestSystem.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/Damage.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "world/Block.hpp"

#include <glm/vec3.hpp>

#include <cstdint>

namespace mc::world {
class World;
}

namespace mc::gameplay {

// The render-side reactions the simulation drives but does not own. The Vulkan
// renderer implements it; a headless test provides a recording stub. Kept
// deliberately small: only audio/particle playbacks, the world-edit pipeline
// and a few UI notifications — never gameplay state.
struct SimulationHost {
    virtual ~SimulationHost() = default;

    // A block the simulation changed (fluid, leaf decay, crop growth...). The
    // simulation has already written it to the gameplay world; the host queues
    // the mesh/light rebuild and persists the edit.
    virtual void submitWorldEdit(int x, int y, int z, world::Block block,
                                 std::uint8_t fluidLevel,
                                 std::optional<world::BlockOrientation> orientation) = 0;
    // Rebuilds the light preview for one changed block, so the edit renders
    // with correct light instead of stale stored values.
    virtual void previewBlockEdit(int worldX, int y, int worldZ) = 0;

    // Audio playbacks the fixed-tick loop drives.
    virtual void playBlockBreak(world::Block block, glm::vec3 position) = 0;
    virtual void playItemPickup(glm::vec3 position) = 0;
    // The chewing loop of an ongoing meal: LivingEntity#spawnConsumptionEffects
    // plays the generic.eat sound every fourth tick of the eat.
    virtual void playEat(glm::vec3 position) = 0;
    virtual void playPlayerHurt(glm::vec3 position) = 0;
    virtual void playPlayerFall(glm::vec3 position, float damage) = 0;
    virtual void playBurp(glm::vec3 position) = 0;
    virtual void playCreatureHurt(glm::vec3 position) = 0;
    virtual void playCreatureDeath(glm::vec3 position) = 0;
    virtual void playFootstep(world::Block ground, glm::vec3 position, float volume) = 0;
    virtual void playSplash(glm::vec3 position, float volume) = 0;

    // Particles the fixed-tick loop throws.
    virtual void spawnBlockBreakParticles(glm::ivec3 position, world::Block block) = 0;

    // The player died this tick. The host raises the death screen and closes
    // the inventory, then the session scatters the drops (onPlayerDeath).
    virtual void onPlayerDied() = 0;
    // A furnace the player has open started/stopped burning; the host swaps its
    // block state and light.
    virtual void onFurnaceStateChanged() = 0;
    // The held-item Eat animation and its cancels.
    virtual void onEatingStarted() = 0;
    virtual void onEatingCancelled() = 0;
};

// Owns every gameplay system the 20 TPS simulation drives and the fixed-tick
// orchestration between them. This is the piece of the old VulkanRenderer::Impl
// that was pure game logic; the renderer keeps Vulkan, streaming, camera and UI
// and calls tick() once per frame.
class GameSession final {
  public:
    static constexpr int kEatTicks = 32;

    GameSession();
    // GameSession is heavy (inventory, entities, world simulation); no copies.
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;
    GameSession(GameSession&&) = delete;
    GameSession& operator=(GameSession&&) = delete;

    // Advances the simulation one fixed 20 TPS tick. `world` is the gameplay
    // world the systems simulate into; `host` receives the render-side
    // reactions. Caller repeats it as many times as its accumulator allows.
    void tick(world::World& world, SimulationHost& host);

    // ---- Actions (the interactive layer calls these) ----
    void setGameMode(GameMode mode);
    // Per-save difficulty, driven by the level.dat value the renderer loads.
    void setDifficulty(Difficulty difficulty) { difficulty_ = difficulty; }
    // 1.16.1 entity.kill(): OutOfWorld damage at infinite magnitude.
    void killPlayer(SimulationHost& host);
    [[nodiscard]] bool hurtPlayer(DamageSource source, float amount, SimulationHost& host);
    // Restores a respawning player to full health/food at the world spawn. The
    // renderer repositions the camera and re-centres streaming after this.
    void respawn();
    void beginEating(const Item* kind, SimulationHost& host);
    void cancelEating(SimulationHost& host);
    // ItemStack#damage on the selected stack; returns true when the tool broke,
    // which is when the renderer plays the break sound.
    [[nodiscard]] bool damageHeldTool(ToolUse use, float blockHardness);
    // Rolls and scatters the loot a broken block drops (mined or simulated).
    void spawnBlockDrops(glm::ivec3 position, world::Block block,
                         const ItemStack& tool,
                         world::BlockOrientation droppedOrientation =
                             world::BlockOrientation::North);
    // The gameplay half of death: scatter the inventory (unless keepInventory)
    // and reset the player's stacks. The host raises the death screen first.
    void onPlayerDeath();
    // The interactive layer's per-frame input edges.
    void setJumpPressed() { jumpPressed_ = true; }
    void setForwardPressed() { forwardPressed_ = true; }
    void clearInputEdges() { jumpPressed_ = forwardPressed_ = false; }

    // ---- Render accessors (inline so hot render paths pay nothing) ----
    // Mutable references: the interactive layer stays in the renderer for now
    // and reads/writes the session's systems through these. The simulation's
    // own code uses the private fields directly.
    [[nodiscard]] PlayerController& player() { return player_; }
    [[nodiscard]] const PlayerController& player() const { return player_; }
    [[nodiscard]] PlayerInput& input() { return playerInput_; }
    [[nodiscard]] PlayerVitals& vitals() { return vitals_; }
    [[nodiscard]] const PlayerVitals& vitals() const { return vitals_; }
    [[nodiscard]] Inventory& inventory() { return inventory_; }
    [[nodiscard]] const Inventory& inventory() const { return inventory_; }
    [[nodiscard]] CraftingSystem& craftingSystem() { return craftingSystem_; }
    [[nodiscard]] const CraftingSystem& craftingSystem() const { return craftingSystem_; }
    [[nodiscard]] GameMode& gameMode() { return gameMode_; }
    [[nodiscard]] GameMode gameMode() const { return gameMode_; }
    [[nodiscard]] GameRules& gameRules() { return gameRules_; }
    [[nodiscard]] const GameRules& gameRules() const { return gameRules_; }
    [[nodiscard]] WorldSimulation& worldSimulation() { return worldSimulation_; }
    [[nodiscard]] const WorldSimulation& worldSimulation() const { return worldSimulation_; }
    [[nodiscard]] ItemEntitySystem& itemEntities() { return itemEntities_; }
    [[nodiscard]] const ItemEntitySystem& itemEntities() const { return itemEntities_; }
    [[nodiscard]] EntitySystem& worldEntities() { return worldEntities_; }
    [[nodiscard]] const EntitySystem& worldEntities() const { return worldEntities_; }
    [[nodiscard]] ChestSystem& chestSystem() { return chestSystem_; }
    [[nodiscard]] const ChestSystem& chestSystem() const { return chestSystem_; }

    [[nodiscard]] double& gameTimeSeconds() { return gameTimeSeconds_; }
    [[nodiscard]] double gameTimeSeconds() const { return gameTimeSeconds_; }
    [[nodiscard]] std::uint32_t& lootRandomState() { return lootRandomState_; }
    [[nodiscard]] std::uint32_t lootRandomState() const { return lootRandomState_; }
    [[nodiscard]] glm::vec3& worldSpawnPosition() { return worldSpawnPosition_; }
    [[nodiscard]] const glm::vec3& worldSpawnPosition() const { return worldSpawnPosition_; }
    // The player's personal spawn point (ServerPlayerEntity#spawnPointPosition):
    // /spawnpoint sets it, death respawns there before the world spawn, and it
    // is persisted with the save.
    [[nodiscard]] glm::vec3& playerSpawnPosition() { return playerSpawnPosition_; }
    [[nodiscard]] const glm::vec3& playerSpawnPosition() const { return playerSpawnPosition_; }
    [[nodiscard]] float& playerSpawnYaw() { return playerSpawnYaw_; }
    [[nodiscard]] float playerSpawnYaw() const { return playerSpawnYaw_; }
    [[nodiscard]] bool& hasPlayerSpawn() { return hasPlayerSpawn_; }
    [[nodiscard]] bool hasPlayerSpawn() const { return hasPlayerSpawn_; }
    [[nodiscard]] glm::vec3& physicsPreviousPosition() { return physicsPreviousPosition_; }
    [[nodiscard]] const glm::vec3& physicsPreviousPosition() const { return physicsPreviousPosition_; }
    [[nodiscard]] glm::vec3& physicsCurrentPosition() { return physicsCurrentPosition_; }
    [[nodiscard]] const glm::vec3& physicsCurrentPosition() const { return physicsCurrentPosition_; }
    [[nodiscard]] bool& eating() { return eating_; }
    [[nodiscard]] bool eating() const { return eating_; }
    [[nodiscard]] const Item*& eatingKind() { return eatingKind_; }
    [[nodiscard]] const Item* eatingKind() const { return eatingKind_; }
    [[nodiscard]] int& eatTicks() { return eatTicks_; }
    [[nodiscard]] int eatTicks() const { return eatTicks_; }
    [[nodiscard]] float& footstepDistance() { return footstepDistance_; }
    [[nodiscard]] float footstepDistance() const { return footstepDistance_; }
    [[nodiscard]] bool& previousInWater() { return previousInWater_; }
    [[nodiscard]] bool previousInWater() const { return previousInWater_; }
    // The input-edge flags, written by the interactive layer and consumed by
    // the fixed-tick simulation.
    [[nodiscard]] bool& jumpPressed() { return jumpPressed_; }
    [[nodiscard]] bool& forwardPressed() { return forwardPressed_; }

  private:
    void tickEating(SimulationHost& host);
    void tickPlayerVitals(SimulationHost& host, const world::World& world,
                          const glm::vec3& previousPosition, bool jumped);
    void updateMovementAudio(SimulationHost& host, const world::World& world,
                             const glm::vec3& previousPosition,
                             const glm::vec3& currentPosition);
    void consumeEntityEvents(SimulationHost& host);
    [[nodiscard]] bool submergedInWater(const world::World& world, glm::vec3 position) const;

    gameplay::PlayerController player_;
    gameplay::PlayerInput playerInput_;
    gameplay::PlayerVitals vitals_;
    gameplay::Inventory inventory_;
    gameplay::CraftingSystem craftingSystem_;
    gameplay::GameMode gameMode_ = gameplay::GameMode::Creative;
    gameplay::Difficulty difficulty_ = gameplay::Difficulty::Normal;
    gameplay::WorldSimulation worldSimulation_;
    gameplay::GameRules gameRules_;
    gameplay::ItemEntitySystem itemEntities_;
    gameplay::EntitySystem worldEntities_;
    gameplay::ChestSystem chestSystem_;

    glm::vec3 worldSpawnPosition_{24.0F, 76.38F, 24.0F};
    glm::vec3 playerSpawnPosition_{24.0F, 76.38F, 24.0F};
    float playerSpawnYaw_ = 0.0F;
    bool hasPlayerSpawn_ = false;
    glm::vec3 physicsPreviousPosition_;
    glm::vec3 physicsCurrentPosition_;
    float footstepDistance_ = 0.0F;
    bool previousInWater_ = false;
    bool jumpPressed_ = false;
    bool forwardPressed_ = false;
    double gameTimeSeconds_ = 0.0;
    std::uint32_t lootRandomState_ = 0x9E3779B9U;

    bool eating_ = false;
    const Item* eatingKind_ = nullptr;
    int eatTicks_ = 0;
};

} // namespace mc::gameplay
