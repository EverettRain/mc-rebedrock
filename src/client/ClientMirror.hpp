#pragma once

// The client-side mirror of the server's per-tick player/world snapshots
// (stage C-1b-2). The renderer reads its player and world state from here after
// decoding the loopback/network channel, instead of reaching straight into the
// authoritative GameSession — so single-player runs the same server→client
// mirror path a networked client will. This is the ClientLevel-equivalent.
//
// Drops, creatures and falling blocks ride the same channel as an entity render
// snapshot, so every render-facing gameplay view is owned by this client mirror.
//
// The interpolation alpha comes from the *client's* receive time, not the
// server's tick timestamp. Across a real connection the two machines' clocks are
// not comparable, so a client always interpolates from when it received a
// snapshot; in loopback this is equally coherent, and because pump() stamps the
// receive time in the same call that replaces the endpoints, the alpha and the
// endpoints stay in step (the same property that fixed the render-side jitter,
// now on the client side of the channel).

#include "gameplay/GameCommand.hpp"
#include "gameplay/GameEventCodec.hpp"
#include "gameplay/GameSnapshotCodec.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerTickSnapshot.hpp"
#include "gameplay/SimulationHostBridge.hpp"
#include "gameplay/WorldSnapshot.hpp"
#include "net/Transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <variant>

namespace mc::client {

class ClientMirror final {
  public:
    // Applies every frame waiting on the channel, in receive order. Snapshot
    // frames update the mirror (newest wins; a player snapshot also stamps the
    // receive time the interpolation alpha is measured from). Event frames are
    // applied to `host` with applyGameEvent — the world edits, sounds, particles
    // and container/eating reactions the server raised this tick, now carried
    // over the channel instead of the renderer draining the bridge directly.
    // Command frames travel the other direction and are ignored. Returns how many
    // frames were applied.
    std::size_t pump(net::MessageChannel& channel, gameplay::SimulationHost& host) {
        std::size_t applied = 0;
        std::optional<net::NetMessage> message;
        while (net::receiveMessage(channel, message)) {
            if (!message.has_value()) {
                continue;  // an unknown tag from a newer peer, already consumed
            }
            if (const auto* snapshot = std::get_if<gameplay::PublishedSnapshot>(&*message)) {
                if (const auto* player = std::get_if<gameplay::PlayerTickSnapshot>(snapshot)) {
                    player_ = *player;
                    receiveRep_ = std::chrono::steady_clock::now().time_since_epoch().count();
                    ++applied;
                } else if (const auto* world = std::get_if<gameplay::WorldSnapshot>(snapshot)) {
                    world_ = *world;
                    ++applied;
                }
            } else if (const auto* event = std::get_if<gameplay::GameEvent>(&*message)) {
                gameplay::applyGameEvent(*event, host);
                ++applied;
            } else if (const auto* entities =
                           std::get_if<gameplay::EntityRenderSnapshot>(&*message)) {
                entities_ = *entities;
                ++applied;
            }
        }
        return applied;
    }

    [[nodiscard]] const gameplay::PlayerTickSnapshot& player() const { return player_; }
    [[nodiscard]] const gameplay::WorldSnapshot& world() const { return world_; }
    [[nodiscard]] const gameplay::EntityRenderSnapshot& entities() const { return entities_; }

    // The entity snapshot paired with the interpolation alpha, both from this
    // mirror — the client analogue of GameSession::entityRenderFrame. Drops and
    // creatures now interpolate against the client's receive-time alpha, the same
    // basis the camera and HUD use, so every mirrored body shares one clock.
    struct InterpolatedEntities final {
        const gameplay::EntityRenderSnapshot& snapshot;
        float alpha = 0.0F;
    };
    [[nodiscard]] InterpolatedEntities entityRenderFrame() const {
        return {entities_, interpolationAlpha()};
    }

    // How far the current frame sits past the tick whose snapshot the client last
    // received, in [0,1]. Measured from the client's own receive time, so it
    // needs no clock shared with the server. 0 before the first snapshot arrives.
    [[nodiscard]] float interpolationAlpha() const {
        if (receiveRep_ == 0) {
            return 0.0F;
        }
        const std::chrono::steady_clock::time_point received{
            std::chrono::steady_clock::duration{receiveRep_}};
        const float elapsed =
            std::chrono::duration<float>{std::chrono::steady_clock::now() - received}.count();
        return std::clamp(elapsed / gameplay::PlayerController::kTickSeconds, 0.0F, 1.0F);
    }

    // A world switch drops the mirror so the next world does not briefly show the
    // previous one's player/world state.
    void clear() {
        player_ = {};
        world_ = {};
        entities_.clear();
        receiveRep_ = 0;
    }

  private:
    gameplay::PlayerTickSnapshot player_;
    gameplay::WorldSnapshot world_;
    gameplay::EntityRenderSnapshot entities_;
    // steady_clock rep of when the last player snapshot arrived; 0 before any.
    std::int64_t receiveRep_ = 0;
};

}  // namespace mc::client
