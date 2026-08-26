#include "gameplay/SimulationHostBridge.hpp"

#include "gameplay/GameSession.hpp"

#include <utility>

namespace mc::gameplay {

SimulationHostBridge::SimulationHostBridge(GameEventBus& bus) {
    // Queue on publish; the host is touched only from drain().
    bus.subscribeWorldEdit([this](const WorldEditEvent& event) { enqueue(event); });
    bus.subscribeSound([this](const SoundEvent& event) { enqueue(event); });
    bus.subscribeParticle([this](const ParticleEvent& event) { enqueue(event); });
    bus.subscribePlayerDied([this](const PlayerDiedEvent& event) { enqueue(event); });
    bus.subscribeClientAction([this](const ClientActionEvent& event) { enqueue(event); });
}

namespace {

void applyTo(SimulationHost& target, const WorldEditEvent& event) {
    // The state-carrying submission plus the light preview, the pair every
    // world edit has always needed together. Keeping them adjacent here is what
    // stops a future call site from remembering one and forgetting the other.
    target.submitWorldStateEdit(event.x, event.y, event.z, event.state);
    if (event.immediate) {
        target.previewBlockEdit(event.x, event.y, event.z);
    }
}

void applyTo(SimulationHost& target, const SoundEvent& event) {
    switch (event.kind) {
    case SoundEventKind::BlockBreak:
        target.playBlockBreak(event.block, event.position);
        break;
    case SoundEventKind::BlockHit:
        target.playBlockHit(event.block, event.position);
        break;
    case SoundEventKind::BlockPlace:
        target.playBlockPlace(event.block, event.position);
        break;
    case SoundEventKind::ItemBreak:
        target.playItemBreak(event.position);
        break;
    case SoundEventKind::ItemPickup:
        target.playItemPickup(event.position);
        break;
    case SoundEventKind::Eat:
        target.playEat(event.position);
        break;
    case SoundEventKind::PlayerHurt:
        target.playPlayerHurt(event.position);
        break;
    case SoundEventKind::PlayerFall:
        target.playPlayerFall(event.position, event.heavy);
        break;
    case SoundEventKind::Burp:
        target.playBurp(event.position);
        break;
    case SoundEventKind::CreatureHurt:
        if (event.species != nullptr) {
            target.playCreatureHurt(*event.species, event.position);
        }
        break;
    case SoundEventKind::CreatureDeath:
        if (event.species != nullptr) {
            target.playCreatureDeath(*event.species, event.position);
        }
        break;
    case SoundEventKind::CreatureAmbient:
        if (event.species != nullptr) {
            target.playCreatureAmbient(*event.species, event.position);
        }
        break;
    case SoundEventKind::CreatureStep:
        if (event.species != nullptr) {
            target.playCreatureStep(*event.species, event.position);
        }
        break;
    case SoundEventKind::Footstep:
        target.playFootstep(event.block, event.position, event.volume);
        break;
    case SoundEventKind::Splash:
        target.playSplash(event.position, event.volume);
        break;
    case SoundEventKind::BlockOpen:
        target.playBlockOpen(event.block, event.position);
        break;
    case SoundEventKind::BlockClose:
        target.playBlockClose(event.block, event.position);
        break;
    case SoundEventKind::BlockClick:
        target.playBlockClick(event.block, event.position, event.heavy);
        break;
    case SoundEventKind::FlintAndSteelUse:
        target.playFlintAndSteelUse(event.position);
        break;
    case SoundEventKind::Shear:
        target.playShear(event.position);
        break;
    }
}

void applyTo(SimulationHost& target, const ParticleEvent& event) {
    switch (event.kind) {
    case ParticleEventKind::BlockBreak:
        target.spawnBlockBreakParticles(glm::ivec3{event.position}, event.block);
        break;
    case ParticleEventKind::WaterSplash:
        target.spawnWaterSplash(event.position);
        break;
    }
}

void applyTo(SimulationHost& target, const PlayerDiedEvent&) { target.onPlayerDied(); }

void applyTo(SimulationHost& target, const ClientActionEvent& event) {
    switch (event.kind) {
    case ClientActionEventKind::OpenContainer:
        target.onOpenContainer(event.screen,
                               event.hasPosition ? std::optional<glm::ivec3>{event.position}
                                                 : std::nullopt);
        break;
    case ClientActionEventKind::EatingStarted:
        target.onEatingStarted();
        break;
    case ClientActionEventKind::EatingCancelled:
        target.onEatingCancelled();
        break;
    }
}

}  // namespace

void applyGameEvent(const GameEvent& event, SimulationHost& host) {
    std::visit([&host](const auto& payload) { applyTo(host, payload); }, event);
}

void SimulationHostBridge::enqueue(GameEvent event) {
    const std::lock_guard<std::mutex> guard{queueMutex_};
    queued_.push_back(std::move(event));
}

std::size_t SimulationHostBridge::drain() {
    // Swapped out first: a handler may publish (a world edit's mesh rebuild can
    // raise a sound), and that has to land in the *next* drain rather than
    // extend the range being walked.
    std::vector<GameEvent> batch;
    {
        const std::lock_guard<std::mutex> guard{queueMutex_};
        batch.swap(queued_);
    }
    if (auto* target = host(); target != nullptr) {
        for (const auto& event : batch) {
            applyGameEvent(event, *target);
        }
    }
    return batch.size();
}

std::vector<GameEvent> SimulationHostBridge::takeQueued() {
    std::vector<GameEvent> batch;
    const std::lock_guard<std::mutex> guard{queueMutex_};
    batch.swap(queued_);
    return batch;
}

void SimulationHostBridge::clear() {
    const std::lock_guard<std::mutex> guard{queueMutex_};
    queued_.clear();
}

std::size_t SimulationHostBridge::pending() const {
    const std::lock_guard<std::mutex> guard{queueMutex_};
    return queued_.size();
}

} // namespace mc::gameplay
