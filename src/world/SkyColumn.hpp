#pragma once

#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/WorldConstants.hpp"

#include <cstdint>

namespace mc::world {

// 26.1's sky light is not a per-cell "remaining direct value" that decays on the
// way down — it is a *binary source column* plus ordinary propagation. Every
// cell at or above a column's `lowestSourceY` is written 15 outright
// (`SkyLightEngine.addSourcesAbove`, :106-136); every cell below it can only
// earn light by propagating, losing `max(1, opacity)` per step in every
// direction, down included. `ChunkSkyLightSources.findLowestSourceY` (:47-75)
// is the scan that finds where a column stops being a source, and the two
// predicates below are its criterion — this header is their single source, so
// the light engine's column scan and ChunkStreamer's edit gate cannot drift
// apart the way they would if each spelled the test out.

// `LightEngine.getOcclusionShape` (:62-68): the occlusion shape light sees is
// the block's face shape only when the block both occludes at all and is one of
// the blocks that answers light with its shape — `isEmptyShape` is
// `!canOcclude() || !useShapeForLightOcclusion()`, and an empty shape seals
// nothing. RN-8a's `faceOcclusionMask` already is that predicate (it returns
// zero for a block whose `canOcclude` is false and asks the shape otherwise), so
// this reuses it rather than spelling the rule a second time: the mesher's face
// culling and the light engine's column scan cannot answer the same geometric
// question differently.
//
// The gate is not optional and not a formality. Ungated, a full-cube shape with
// no dampening — glass, ice, stained glass — would end the column, and light
// under a glass roof would fade with depth; vanilla lets it through untouched
// because glass is `noOcclusion()`. Ungated, so would a crop, a pressure plate,
// a repeater and redstone dust, all of which are Column shapes flush with the
// cell floor and therefore seal their own underside. On today's roster the gate
// leaves exactly the stairs, which is exactly the set vanilla's
// `useShapeForLightOcclusion` list (StairBlock, SlabBlock, SnowLayerBlock,
// FarmlandBlock, ...) leaves once the blocks this project already dampens by
// identity are removed. Vanilla's second gate has no axis here yet; a block that
// declares `canOcclude` but must not answer light with its shape is where one
// would have to be added.
[[nodiscard]] constexpr bool occludesLightFace(BlockState state, Face face) {
    const auto bit = static_cast<std::uint8_t>(1U << static_cast<unsigned>(face));
    return (faceOcclusionMask(state) & bit) != 0U;
}

// `ChunkSkyLightSources.isEdgeOccluded` (:140-148): does the edge between
// `bottomState` and the `topState` directly above it end the source column?
//
// Clause (2) is deliberately a *conservative approximation* of vanilla's:
// `Shapes.faceShapeOccludes` asks whether the union of the two shapes seals the
// shared face, while `faceOccludesFully` is a single-shape test (BlockShape.hpp
// documents why it does not port `Shapes.join`). So the case vanilla catches and
// this does not is "two half-faces that together seal the edge" — we answer *not
// occluded*, the column runs one edge longer, and the cells below stay brighter.
// The direction is the safe one: it can only leave light where today's engine
// already leaks it, never introduce a new dark region.
[[nodiscard]] constexpr bool skyColumnEdgeOccluded(BlockState topState, BlockState bottomState) {
    // (1) Any dampening at all ends the column — state-aware, so a submerged
    // stair ends it the way the water filling it would.
    if (skyLightOpacity(bottomState) != 0U) {
        return true;
    }
    // (2) ... and so does a shape that seals the edge, which is how a stair
    // (`lightFilter == 0`, and not opaque) ends one.
    return occludesLightFace(topState, Face::NegativeY) ||
           occludesLightFace(bottomState, Face::PositiveY);
}

// Everything about a state that `skyColumnEdgeOccluded` can read. An edit that
// leaves this unchanged provably cannot move any column's `lowestSourceY`, which
// is what lets ChunkStreamer skip the light update for the thousands of
// random-tick edits that only swap one plant stage for another. Comparing block
// identity — or opacity alone — is not enough: a fence and a bottom slab are
// both `lightFilter == 0` and both unlit, yet only one of them seals the edge.
[[nodiscard]] constexpr std::uint8_t skyColumnSignature(BlockState state) {
    return static_cast<std::uint8_t>(
        skyLightOpacity(state) |
        (occludesLightFace(state, Face::PositiveY) ? 0x10U : 0x00U) |
        (occludesLightFace(state, Face::NegativeY) ? 0x20U : 0x00U));
}

// `ChunkSkyLightSources.findLowestSourceY`: the lowest Y in this column that is
// still a sky source. Scans down from the top of the highest non-empty section
// with air as the initial `topState`, and returns the cell *above* the first
// occluded edge — so a bottom slab (which seals only its own underside) is
// itself the lowest source, while a top slab (which seals the edge above it)
// leaves the cell above it as the lowest source. An all-air chunk, and a column
// that reaches the bottom of the world unobstructed, are sources all the way
// down (vanilla's `fill(minY)`).
[[nodiscard]] inline int lowestSourceYIn(const Chunk& chunk, int localX, int localZ) {
    int topSectionIndex = -1;
    for (int sectionIndex = kSectionCount - 1; sectionIndex >= 0; --sectionIndex) {
        if (!chunk.section(sectionIndex).empty()) {
            topSectionIndex = sectionIndex;
            break;
        }
    }
    if (topSectionIndex < 0) {
        return kMinY;
    }
    BlockState topState{}; // air
    for (int sectionIndex = topSectionIndex; sectionIndex >= 0; --sectionIndex) {
        const ChunkSection& section = chunk.section(sectionIndex);
        if (section.empty()) {
            // An all-air section neither occludes nor carries a state forward:
            // resetting `topState` is what keeps the cell below an empty section
            // from being compared against the block above that section
            // (ChunkSkyLightSources.java:56-60).
            topState = BlockState{};
            continue;
        }
        for (int localY = kSectionSize - 1; localY >= 0; --localY) {
            const BlockState bottomState = section.state(localX, localY, localZ);
            if (skyColumnEdgeOccluded(topState, bottomState)) {
                return sectionOriginY(sectionIndex) + localY + 1;
            }
            topState = bottomState;
        }
    }
    return kMinY;
}

} // namespace mc::world
