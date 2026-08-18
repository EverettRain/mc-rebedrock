#pragma once

// The dedicated server's SimulationHost (stage C, §8.2 / D7). A SimulationHost is
// the runtime's outward edge — the tick calls it to persist world edits and to
// raise side effects (sounds, particles, screen opens). In the render process
// those side effects drive audio and visuals; on a headless server there is no
// audio or screen, and every player-visible effect already travels to the
// clients as a GameEvent over the channel (GameRuntime publishes them each tick).
// So the server host keeps only the one responsibility the server actually owns:
// persisting block edits into the open save, exactly as the render host does, so
// a change survives a save and reload. Everything else is a no-op.
//
// This is the same shape as the headless test's RecordingHost — which is no
// accident: game_runtime_test is this server's functional ancestor.

#include "gameplay/GameSession.hpp"
#include "persistence/SaveRepository.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/PersistentBlockEdit.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>

namespace mc::server {

class ServerHost final : public gameplay::SimulationHost {
  public:
    // The open save the runtime owns; edits are appended/updated here so they
    // persist. Set from GameRuntime::currentSaveSlot() after a world is open.
    std::optional<persistence::SaveGame>* save = nullptr;

    void submitWorldEdit(int x, int y, int z, world::Block block, std::uint8_t fluidLevel,
                         std::optional<world::BlockOrientation> orientation) override {
        const auto resolved = orientation.value_or(world::defaultOrientation(block));
        remember({x, y, z, world::BlockState{block, resolved, fluidLevel}});
    }
    void submitWorldStateEdit(int x, int y, int z, world::BlockState state) override {
        remember({x, y, z, state});
    }

    // Everything below is presentation the headless server does not do; the
    // player-visible ones already reach clients as GameEvents on the channel.
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
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

  private:
    // Append the edit, or replace an existing edit at the same cell — the same
    // last-writer-wins the render host keeps, so the save holds one edit per cell.
    void remember(world::PersistentBlockEdit edit) {
        if (save == nullptr || !save->has_value()) {
            return;
        }
        auto& edits = (*save)->edits;
        for (auto& existing : edits) {
            if (existing.x == edit.x && existing.y == edit.y && existing.z == edit.z) {
                existing = edit;
                return;
            }
        }
        edits.push_back(edit);
    }
};

}  // namespace mc::server
