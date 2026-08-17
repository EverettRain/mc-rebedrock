#pragma once

// The four event classes A3a defines, and the synchronous dispatcher that
// delivers them.
//
// Scope is deliberate, and narrower than the audit asked for. 26.1 has **no**
// general semantic event bus: it has GameEvent (sculk vibrations only),
// CriteriaTriggers (advancements), and otherwise direct sound/particle calls.
// So "every semantic action publishes an event" is not a vanilla-backed design,
// and on a tick hot path it would cost a great deal for nothing. What is here is
// exactly the set P3 Step 3 has to move across a thread boundary — world edits,
// sounds, particles, the player's death — and nothing else.
//
// **Every payload is trivially copyable, and must stay that way.** C4' (= P3
// Step 3) replaces the synchronous dispatch below with a lock-protected queue
// that the main thread drains each frame; that swap must not require touching a
// single emitter. A payload holding a reference, an owning handle, or anything
// whose lifetime is shorter than the queue would break that. Species pointers
// are fine: EntityType instances are static singletons.

#include "gameplay/entities/EntityType.hpp"
#include "gameplay/ScreenHandler.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <type_traits>
#include <vector>

namespace mc::gameplay {

// A cell changed. The state is carried whole rather than as the
// block/fluid/orientation triple, because that triple cannot express a
// furnace's LIT and an edit that only lights one would arrive unlit.
struct WorldEditEvent final {
    int x = 0;
    int y = 0;
    int z = 0;
    world::BlockState state{};
    // Whether this edit has to be visible in the same frame rather than waiting
    // for the streaming worker's round trip. It is a property of the edit, not
    // of the renderer: a player's break must show at once, and a gravity block
    // handing geometry between a chunk mesh and a moving draw leaves a hole for
    // a frame if it does not. Bulk fluid spread deliberately does not ask for
    // it — a thousand cells would each pay for an immediate relight.
    bool immediate = false;
    [[nodiscard]] friend bool operator==(const WorldEditEvent&, const WorldEditEvent&) = default;
};

enum class SoundEventKind : std::uint8_t {
    BlockBreak,
    BlockHit,
    BlockPlace,
    ItemBreak,
    ItemPickup,
    Eat,
    PlayerHurt,
    PlayerFall,
    Burp,
    CreatureHurt,
    CreatureDeath,
    CreatureAmbient,
    CreatureStep,
    Footstep,
    Splash,
};

// One payload for every sound the simulation raises. A variant per kind would
// be tidier to read and worse to queue; the unused fields cost a few bytes on a
// path that fires a handful of times per tick.
struct SoundEvent final {
    SoundEventKind kind = SoundEventKind::BlockBreak;
    glm::vec3 position{0.0F};
    // BlockBreak: the block broken. Footstep: the ground walked on.
    world::Block block = world::Block::Air;
    // Creature*: the species whose clip plays. Null for the rest.
    const entities::EntityType* species = nullptr;
    // Footstep / Splash.
    float volume = 1.0F;
    // PlayerFall: the heavy landing variant.
    bool heavy = false;
    [[nodiscard]] friend bool operator==(const SoundEvent&, const SoundEvent&) = default;
};

enum class ParticleEventKind : std::uint8_t {
    BlockBreak,
    WaterSplash,
};

struct ParticleEvent final {
    ParticleEventKind kind = ParticleEventKind::BlockBreak;
    glm::vec3 position{0.0F};
    world::Block block = world::Block::Air;
    [[nodiscard]] friend bool operator==(const ParticleEvent&, const ParticleEvent&) = default;
};

// The player died this tick. The fact is the whole payload: who and how are
// already in PlayerVitals, and duplicating them here would be state, not event.
struct PlayerDiedEvent final {
    [[nodiscard]] friend bool operator==(const PlayerDiedEvent&, const PlayerDiedEvent&) = default;
};

// Main-thread-only presentation reactions that are neither sounds nor
// particles. The simulation updates the authoritative container/eating/furnace
// state first; this event merely tells the client presentation to react.
enum class ClientActionEventKind : std::uint8_t {
    OpenContainer,
    EatingStarted,
    EatingCancelled,
};

struct ClientActionEvent final {
    ClientActionEventKind kind = ClientActionEventKind::OpenContainer;
    ContainerScreen screen = ContainerScreen::PlayerInventory;
    glm::ivec3 position{0};
    bool hasPosition = false;
    [[nodiscard]] friend bool operator==(const ClientActionEvent&, const ClientActionEvent&) =
        default;
};

static_assert(std::is_trivially_copyable_v<WorldEditEvent>);
static_assert(std::is_trivially_copyable_v<SoundEvent>);
static_assert(std::is_trivially_copyable_v<ParticleEvent>);
static_assert(std::is_trivially_copyable_v<PlayerDiedEvent>);
static_assert(std::is_trivially_copyable_v<ClientActionEvent>);

// Synchronous, single-threaded, in-order delivery. Publishing calls every
// subscriber before returning, so behaviour is identical to the direct host
// calls this replaces — the difference is that the emitter no longer names its
// consumer. The queue lands in P3 Step 3, behind this same interface.
//
// Subscriptions are permanent: the subscribers are the audio system, the
// particle system and the render/persistence pipeline, all of which outlive the
// session. There is deliberately no unsubscribe to get wrong.
class GameEventBus final {
  public:
    void subscribeWorldEdit(std::function<void(const WorldEditEvent&)> listener) {
        worldEdit_.push_back(std::move(listener));
    }
    void subscribeSound(std::function<void(const SoundEvent&)> listener) {
        sound_.push_back(std::move(listener));
    }
    void subscribeParticle(std::function<void(const ParticleEvent&)> listener) {
        particle_.push_back(std::move(listener));
    }
    void subscribePlayerDied(std::function<void(const PlayerDiedEvent&)> listener) {
        playerDied_.push_back(std::move(listener));
    }
    void subscribeClientAction(std::function<void(const ClientActionEvent&)> listener) {
        clientAction_.push_back(std::move(listener));
    }

    void publish(const WorldEditEvent& event) const { dispatch(worldEdit_, event); }
    void publish(const SoundEvent& event) const { dispatch(sound_, event); }
    void publish(const ParticleEvent& event) const { dispatch(particle_, event); }
    void publish(const PlayerDiedEvent& event) const { dispatch(playerDied_, event); }
    void publish(const ClientActionEvent& event) const { dispatch(clientAction_, event); }

  private:
    template <typename Listeners, typename Event>
    static void dispatch(const Listeners& listeners, const Event& event) {
        for (const auto& listener : listeners) {
            listener(event);
        }
    }

    std::vector<std::function<void(const WorldEditEvent&)>> worldEdit_;
    std::vector<std::function<void(const SoundEvent&)>> sound_;
    std::vector<std::function<void(const ParticleEvent&)>> particle_;
    std::vector<std::function<void(const PlayerDiedEvent&)>> playerDied_;
    std::vector<std::function<void(const ClientActionEvent&)>> clientAction_;
};

} // namespace mc::gameplay
