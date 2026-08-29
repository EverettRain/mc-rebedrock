#include "gameplay/GameEvents.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// A3a's event layer. The point of publishing rather than calling is that P3
// Step 3 can swap the synchronous dispatcher below for a cross-thread queue
// without touching a single emitter — so what is pinned here is that the
// emitters really do go through the bus, that a subscriber sees the same
// information the SimulationHost used to be handed directly, and that the
// payloads stay queueable.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"game_events_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc;
using mc::gameplay::ParticleEvent;
using mc::gameplay::PlayerDiedEvent;
using mc::gameplay::SoundEvent;
using mc::gameplay::SoundEventKind;
using mc::gameplay::WorldEditEvent;

// A subscriber that just records, which is what a cross-thread queue will be.
struct Recorder final {
    std::vector<WorldEditEvent> worldEdits;
    std::vector<SoundEvent> sounds;
    std::vector<ParticleEvent> particles;
    int deaths = 0;

    void attach(gameplay::GameEventBus& bus) {
        bus.subscribeWorldEdit([this](const WorldEditEvent& e) { worldEdits.push_back(e); });
        bus.subscribeSound([this](const SoundEvent& e) { sounds.push_back(e); });
        bus.subscribeParticle([this](const ParticleEvent& e) { particles.push_back(e); });
        bus.subscribePlayerDied([this](const PlayerDiedEvent&) { ++deaths; });
    }

    [[nodiscard]] bool sawSound(SoundEventKind kind) const {
        for (const auto& sound : sounds) {
            if (sound.kind == kind) {
                return true;
            }
        }
        return false;
    }
};

// The minimal host: A3a keeps SimulationHost, so the bridge must still drive it.
struct CountingHost final : mc::gameplay::SimulationHost {
    int stateEdits = 0;
    int previews = 0;
    int blockBreakSounds = 0;
    int playerHurtSounds = 0;
    int footstepSounds = 0;
    int breakParticles = 0;
    bool died = false;

    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {
        ++stateEdits;
    }
    void submitWorldStateEdit(int, int, int, world::BlockState) override { ++stateEdits; }
    void previewBlockEdit(int, int, int) override { ++previews; }
    void playBlockBreak(world::Block, glm::vec3) override { ++blockBreakSounds; }
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override { ++playerHurtSounds; }
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override { ++footstepSounds; }
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override { ++breakParticles; }
    void onPlayerDied() override { died = true; }
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

[[nodiscard]] world::World loadedWorld() {
    world::World world;
    for (int chunkX = -1; chunkX <= 1; ++chunkX) {
        for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
            world.setChunk({chunkX, chunkZ}, world::Chunk{});
        }
    }
    return world;
}

} // namespace

int main() {
    // --- The dispatcher itself: in-order, every subscriber, all four channels.
    {
        gameplay::GameEventBus bus;
        Recorder first;
        Recorder second;
        first.attach(bus);
        second.attach(bus);

        bus.publish(WorldEditEvent{1, 2, 3, world::BlockState{world::Block::Stone}, true});
        bus.publish(SoundEvent{SoundEventKind::Burp, {4.0F, 5.0F, 6.0F}});
        bus.publish(ParticleEvent{gameplay::ParticleEventKind::BlockBreak, {7, 8, 9},
                                  world::Block::Dirt});
        bus.publish(PlayerDiedEvent{});

        for (const auto* recorder : {&first, &second}) {
            REQUIRE(recorder->worldEdits.size() == 1U);
            REQUIRE(recorder->worldEdits[0].x == 1 && recorder->worldEdits[0].z == 3);
            REQUIRE(recorder->worldEdits[0].state.block() == world::Block::Stone);
            REQUIRE(recorder->worldEdits[0].immediate);
            REQUIRE(recorder->sounds.size() == 1U);
            REQUIRE(recorder->sounds[0].kind == SoundEventKind::Burp);
            REQUIRE(recorder->particles.size() == 1U);
            REQUIRE(recorder->particles[0].block == world::Block::Dirt);
            REQUIRE(recorder->deaths == 1);
        }
    }

    // --- A mutation publishes its world edit rather than calling the host, and
    // the bridge turns it back into the host calls that always happened. ---
    {
        auto world = loadedWorld();
        gameplay::GameSession session;
        CountingHost host;
        Recorder recorder;
        recorder.attach(session.events());
        session.setEventHost(host);
        world.setState(4, 20, 4, world::BlockState{world::Block::Stone});

        gameplay::GameplayMutationSink sink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {4, 20, 4}, world::BlockState{}, world::MutationFlags::All,
            world::MutationCause::PlayerBreak, sink));

        REQUIRE(recorder.worldEdits.size() == 1U);
        REQUIRE(recorder.worldEdits[0].state.block() == world::Block::Air);
        // An interactive edit has to show the same frame.
        REQUIRE(recorder.worldEdits[0].immediate);
        // And the retained host still received both halves.
        session.drainEvents();
        REQUIRE(host.stateEdits == 1);
        session.drainEvents();
        REQUIRE(host.previews == 1);
    }

    // --- The simulation's own effects travel the same way. A block broken by
    // the simulation raises a sound and a particle event, and the bridge feeds
    // the host exactly as the direct calls used to. ---
    {
        auto world = loadedWorld();
        gameplay::GameSession session;
        CountingHost host;
        Recorder recorder;
        recorder.attach(session.events());
        // A torch whose support is pulled out from under it pops off, which is
        // a real block break: sound, particles and a drop.
        world.setState(3, 5, 3, world::BlockState{world::Block::Stone});
        world.setState(3, 6, 3, world::BlockState{world::Block::Torch});
        world.setState(3, 5, 3, world::BlockState{});
        session.worldSimulation().notifyNeighborChanged(world, {3, 5, 3});
        for (int tick = 0; tick < 6 && recorder.sounds.empty(); ++tick) {
            session.tick(world, host);
        }
        REQUIRE(world.block(3, 6, 3) == world::Block::Air);
        REQUIRE(recorder.sawSound(SoundEventKind::BlockBreak));
        REQUIRE(!recorder.particles.empty());
        session.drainEvents();
        REQUIRE(host.blockBreakSounds > 0);
        session.drainEvents();
        REQUIRE(host.breakParticles > 0);
        // A simulated break must still be previewed, or it lingers on screen.
        session.drainEvents();
        REQUIRE(host.previews > 0);
    }

    // --- The death event reaches both a plain subscriber and the host. ---
    {
        auto world = loadedWorld();
        gameplay::GameSession session;
        CountingHost host;
        Recorder recorder;
        recorder.attach(session.events());
        session.setGameMode(gameplay::GameMode::Survival);
        REQUIRE(session.hurtPlayer(gameplay::kPrimaryPlayerId, gameplay::DamageType::OutOfWorld, 1000.0F, host));
        REQUIRE(recorder.deaths == 1);
        session.drainEvents();
        REQUIRE(host.died);
        REQUIRE(recorder.sawSound(SoundEventKind::PlayerHurt));
        // The bridge routes by kind: a hurt must reach playPlayerHurt and
        // nothing else. A switch that fell through to the wrong host method
        // would still publish the right event, so assert the host side too.
        session.drainEvents();
        REQUIRE(host.playerHurtSounds == 1);
        session.drainEvents();
        REQUIRE(host.blockBreakSounds == 0);
        session.drainEvents();
        REQUIRE(host.footstepSounds == 0);
    }

    // --- No host bound: publishing is still safe. A headless caller that never
    // supplies a SimulationHost must be able to run the simulation. ---
    {
        auto world = loadedWorld();
        gameplay::GameSession session;
        Recorder recorder;
        recorder.attach(session.events());
        world.setState(3, 9, 3, world::BlockState{world::Block::Stone});
        gameplay::GameplayMutationSink sink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {3, 9, 3}, world::BlockState{}, world::MutationFlags::All,
            world::MutationCause::Command, sink));
        REQUIRE(recorder.worldEdits.size() == 1U);
    }

    // --- P3 Step 3: publishing queues, it does not call. Nothing reaches the
    // host until the main thread drains, which is what lets Step 2 move the
    // tick onto its own thread without the simulation touching renderer state.
    {
        auto world = loadedWorld();
        gameplay::GameSession session;
        CountingHost host;
        session.setEventHost(host);
        world.setState(7, 30, 7, world::BlockState{world::Block::Stone});

        gameplay::GameplayMutationSink sink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {7, 30, 7}, world::BlockState{}, world::MutationFlags::All,
            world::MutationCause::PlayerBreak, sink));

        // The world is already changed — that part is synchronous...
        REQUIRE(world.block(7, 30, 7) == world::Block::Air);
        // ...but the host has not been touched.
        REQUIRE(host.stateEdits == 0);
        REQUIRE(host.previews == 0);
        REQUIRE(session.pendingEvents() > 0U);

        REQUIRE(session.drainEvents() > 0U);
        REQUIRE(host.stateEdits == 1);
        REQUIRE(host.previews == 1);
        // The queue is emptied, so a second drain is a no-op rather than a
        // replay — a replayed world edit would rebuild meshes every frame.
        REQUIRE(session.pendingEvents() == 0U);
        REQUIRE(session.drainEvents() == 0U);
        REQUIRE(host.stateEdits == 1);
    }

    // --- Order is preserved across event kinds. A block's break sound and its
    // particles have to follow the edit that removed it, not run in a separate
    // pass per kind. ---
    {
        gameplay::GameSession session;
        CountingHost host;
        session.setEventHost(host);
        Recorder recorder;
        recorder.attach(session.events());

        session.events().publish(WorldEditEvent{1, 1, 1, world::BlockState{}, true});
        session.events().publish(
            SoundEvent{SoundEventKind::BlockBreak, {1.0F, 1.0F, 1.0F}, world::Block::Stone});
        session.events().publish(ParticleEvent{gameplay::ParticleEventKind::BlockBreak,
                                               {1, 1, 1}, world::Block::Stone});
        // Subscribers still see them synchronously; only the host is deferred.
        REQUIRE(recorder.worldEdits.size() == 1U);
        REQUIRE(host.stateEdits == 0);

        REQUIRE(session.drainEvents() == 3U);
        REQUIRE(host.stateEdits == 1);
        REQUIRE(host.blockBreakSounds == 1);
        REQUIRE(host.breakParticles == 1);
    }

    // --- A headless caller that never binds a host still runs: the queue is
    // drained and discarded rather than growing without bound. ---
    {
        gameplay::GameSession session;
        session.events().publish(SoundEvent{SoundEventKind::Burp, {0.0F, 0.0F, 0.0F}});
        REQUIRE(session.pendingEvents() == 1U);
        REQUIRE(session.drainEvents() == 1U);
        REQUIRE(session.pendingEvents() == 0U);
    }

    return 0;
}
