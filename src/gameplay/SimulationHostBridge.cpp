#include "gameplay/SimulationHostBridge.hpp"

#include "gameplay/GameSession.hpp"

#include <utility>

namespace mc::gameplay {

SimulationHostBridge::SimulationHostBridge(GameEventBus& bus) {
    // Queue on publish; the host is touched only from drain().
    bus.subscribeWorldEdit([this](const WorldEditEvent& event) { queued_.emplace_back(event); });
    bus.subscribeSound([this](const SoundEvent& event) { queued_.emplace_back(event); });
    bus.subscribeParticle([this](const ParticleEvent& event) { queued_.emplace_back(event); });
    bus.subscribePlayerDied([this](const PlayerDiedEvent& event) { queued_.emplace_back(event); });
}

std::size_t SimulationHostBridge::drain() {
    if (queued_.empty()) {
        return 0U;
    }
    // Swapped out first: a handler may publish (a world edit's mesh rebuild can
    // raise a sound), and that has to land in the *next* drain rather than
    // extend the range being walked.
    std::vector<QueuedEvent> batch;
    batch.swap(queued_);
    for (const auto& event : batch) {
        std::visit([this](const auto& payload) { run(payload); }, event);
    }
    return batch.size();
}

void SimulationHostBridge::run(const WorldEditEvent& event) const {
    if (host_ == nullptr) {
        return;
    }
    // The state-carrying submission plus the light preview, the pair every
    // world edit has always needed together. Keeping them adjacent here is what
    // stops a future call site from remembering one and forgetting the other.
    host_->submitWorldStateEdit(event.x, event.y, event.z, event.state);
    if (event.immediate) {
        host_->previewBlockEdit(event.x, event.y, event.z);
    }
}

void SimulationHostBridge::run(const SoundEvent& event) const {
    if (host_ == nullptr) {
        return;
    }
    switch (event.kind) {
    case SoundEventKind::BlockBreak:
        host_->playBlockBreak(event.block, event.position);
        break;
    case SoundEventKind::ItemPickup:
        host_->playItemPickup(event.position);
        break;
    case SoundEventKind::Eat:
        host_->playEat(event.position);
        break;
    case SoundEventKind::PlayerHurt:
        host_->playPlayerHurt(event.position);
        break;
    case SoundEventKind::PlayerFall:
        host_->playPlayerFall(event.position, event.heavy);
        break;
    case SoundEventKind::Burp:
        host_->playBurp(event.position);
        break;
    case SoundEventKind::CreatureHurt:
        if (event.species != nullptr) {
            host_->playCreatureHurt(*event.species, event.position);
        }
        break;
    case SoundEventKind::CreatureDeath:
        if (event.species != nullptr) {
            host_->playCreatureDeath(*event.species, event.position);
        }
        break;
    case SoundEventKind::CreatureAmbient:
        if (event.species != nullptr) {
            host_->playCreatureAmbient(*event.species, event.position);
        }
        break;
    case SoundEventKind::CreatureStep:
        if (event.species != nullptr) {
            host_->playCreatureStep(*event.species, event.position);
        }
        break;
    case SoundEventKind::Footstep:
        host_->playFootstep(event.block, event.position, event.volume);
        break;
    case SoundEventKind::Splash:
        host_->playSplash(event.position, event.volume);
        break;
    }
}

void SimulationHostBridge::run(const ParticleEvent& event) const {
    if (host_ == nullptr) {
        return;
    }
    switch (event.kind) {
    case ParticleEventKind::BlockBreak:
        host_->spawnBlockBreakParticles(event.position, event.block);
        break;
    }
}

void SimulationHostBridge::run(const PlayerDiedEvent&) const {
    if (host_ != nullptr) {
        host_->onPlayerDied();
    }
}

} // namespace mc::gameplay
