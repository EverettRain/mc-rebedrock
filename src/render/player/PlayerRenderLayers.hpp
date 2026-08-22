#pragma once

// The third-person player render layers (analysis §10.3). The world player is
// drawn as a stack of layers, each consuming the SAME PlayerRenderState and the
// SAME solved PlayerPoseFrame — no layer re-solves the pose (§18 [low]). This
// header is the Vulkan-free contract: the layer identity, its draw order, and
// which pose socket an item layer attaches to. The concrete draw calls live in
// the renderer (WorldRenderer), which walks these in order.
//
// Keeping this list here (rather than as ad-hoc branches in the renderer) means
// a new layer — armor, cape, elytra (§10.3) — is added to the enum and the
// ordered table, not by threading another bool through the world draw.

#include "animation/HumanoidPoseSolver.hpp"
#include "render/player/PlayerRenderState.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::render::player {

enum class PlayerRenderLayerId : std::uint8_t {
    Body = 0,       // the base skin cuboids
    SkinOuter,      // hat / jacket / sleeves / pants overlay
    ItemInHand,     // the held item, attached to a hand socket
    // Reserved for Phase 6 content; declared so their order is fixed now.
    Armor,
    Cape,
};

// Which hand socket an item-in-hand layer draws at. Resolved from the render
// state's main arm; the off arm is reserved.
enum class HandSocket : std::uint8_t { Right, Left };

// One layer's fixed metadata. `attachesToSocket` is only meaningful for the
// item-in-hand layer; the others draw against the model root + pose directly.
struct PlayerRenderLayerInfo final {
    PlayerRenderLayerId id;
    bool attachesToSocket;
};

// The draw order. Body first, overlays and attachments after, so an item never
// z-fights the base skin. This is the single source of layer ordering.
inline constexpr std::array<PlayerRenderLayerInfo, 5> kPlayerRenderLayerOrder{{
    {PlayerRenderLayerId::Body, false},
    {PlayerRenderLayerId::SkinOuter, false},
    {PlayerRenderLayerId::Armor, false},
    {PlayerRenderLayerId::ItemInHand, true},
    {PlayerRenderLayerId::Cape, false},
}};

// The socket the item-in-hand layer uses for a given render state: the main arm.
[[nodiscard]] inline HandSocket itemInHandSocket(const PlayerRenderState& state) {
    // The main hand renders on the right arm unless a left-handed main arm is
    // ever configured; today the main hand is always the right arm.
    static_cast<void>(state);
    return HandSocket::Right;
}

// The model-space socket matrix for a hand, from the solved pose frame.
[[nodiscard]] inline const glm::mat4& handSocketMatrix(const animation::PlayerPoseFrame& pose,
                                                       HandSocket socket) {
    return socket == HandSocket::Right ? pose.rightHandSocket : pose.leftHandSocket;
}

}  // namespace mc::render::player
