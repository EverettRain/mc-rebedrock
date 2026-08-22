#pragma once

#include "world/Dimension.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>

namespace mc::gameplay {

// DIM-5: the dimension-transfer mechanism's pure pieces — the 1:8 coordinate
// scaling and the deterministic portal search — kept free of any block/frame
// content (that is AR/B) and of the Level plumbing, so they can be reasoned about
// and tested on their own.

// The horizontal coordinate scaling a cross-dimension move applies, mirroring
// JE Entity.changeDimension: the target position is the source's X/Z times
// (sourceScale / targetScale), Y unchanged. Both scales come from the
// DimensionType (DIM-0), never a hardcoded 8 — so Overworld -> Nether divides by
// 8 (1.0 / 8.0), Nether -> Overworld multiplies by 8 (8.0 / 1.0), and a same-
// scale pair (Overworld <-> End) leaves the coordinate untouched.
[[nodiscard]] inline glm::vec3 scaleCoordinatesBetweenDimensions(
    glm::vec3 sourcePosition, world::DimensionId from, world::DimensionId to) {
    const double sourceScale = world::dimensionType(from).coordinateScale;
    const double targetScale = world::dimensionType(to).coordinateScale;
    const double factor = sourceScale / targetScale;
    return glm::vec3{
        static_cast<float>(static_cast<double>(sourcePosition.x) * factor),
        sourcePosition.y,  // vertical is never scaled
        static_cast<float>(static_cast<double>(sourcePosition.z) * factor),
    };
}

// The outcome of asking the target dimension for a landing spot at a scaled
// position. A cross-dimension query must never force-load or generate a chunk in
// the tick ([[lowframe-chunk-unload-io]]): if the destination chunk is resident
// the transfer can land now; otherwise the caller queues the transfer and asks
// the streamer to bring the chunk in asynchronously.
enum class PortalDestinationStatus : std::uint8_t {
    Ready,          // the destination chunk is loaded; the transfer can land now
    AwaitingChunk,  // the destination chunk is not loaded; queue + async request
    NoWorld,        // the target dimension has no world bound at all
};

struct PortalDestination final {
    PortalDestinationStatus status = PortalDestinationStatus::NoWorld;
    glm::vec3 position{0.0F};        // the scaled landing position (valid when Ready)
    world::ChunkPosition chunk{};   // the destination chunk (for the async request)
};

// Resolves where a transfer to `to` at the scaled `scaledPosition` would land,
// WITHOUT loading or generating anything. Deterministic: same inputs, same
// answer, no random azimuth. The real portal-frame search (finding/creating an
// exit portal within a radius) needs portal *blocks*, which are AR/B content not
// yet on disk; until then the destination is the scaled column itself, and the
// only decision here is whether its chunk is resident (land now) or not (queue).
[[nodiscard]] inline PortalDestination resolvePortalDestination(
    const world::World* targetWorld, glm::vec3 scaledPosition) {
    PortalDestination destination;
    destination.position = scaledPosition;
    const auto floorDiv = [](int value, int divisor) {
        return value >= 0 ? value / divisor : -(((-value) + divisor - 1) / divisor);
    };
    destination.chunk = world::ChunkPosition{
        floorDiv(static_cast<int>(std::floor(scaledPosition.x)), 16),
        floorDiv(static_cast<int>(std::floor(scaledPosition.z)), 16)};
    if (targetWorld == nullptr) {
        destination.status = PortalDestinationStatus::NoWorld;
        return destination;
    }
    destination.status = targetWorld->hasChunk(destination.chunk)
                             ? PortalDestinationStatus::Ready
                             : PortalDestinationStatus::AwaitingChunk;
    return destination;
}

} // namespace mc::gameplay
