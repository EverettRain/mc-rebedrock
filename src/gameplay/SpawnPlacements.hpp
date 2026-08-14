#pragma once

// SpawnPlacements (26.1 `world/entity/SpawnPlacements.java` +
// `SpawnPlacementTypes.java`): given a cell and a species' SpawnPlacement, may
// a creature of that species be born there?
//
// Vanilla writes the four placements as lambdas behind a SpawnPlacementType
// interface. Here they are a switch over `entities::SpawnPlacement` and the
// whole thing is one pure function of (world, position, placement): the
// spawner calls it a few hundred times a second and every call answers a
// single bool, so an indirect call per answer buys nothing. The enum lives on
// EntityType (next to MobCategory, the other per-species spawn classification);
// only this header needs the World, which is why the predicate is not on the
// type itself.

#include "gameplay/SimulationPosition.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/World.hpp"

namespace mc::gameplay {

// BlockState#isRedstoneConductor: a full cube that is also solid to light.
// isFullCube alone would take glass with it, which vanilla does not treat as a
// conductor — hence the opacity term.
[[nodiscard]] inline bool isRedstoneConductorLike(world::Block block) {
    return world::isFullCube(block) && world::hasCollision(block) && world::isOpaque(block);
}

// BlockState#isValidSpawn, whose default is
// `isFaceSturdy(level, pos, UP) && getLightEmission() < 14`.
//
// The sturdiness half is spelled `isLandEntitySupport` here rather than
// `isFaceSturdy` on purpose: Block.hpp defines the former as this game's
// standing surface, which keeps partial collision blocks like farmland usable
// and rejects leaves. Rejecting leaves is what stops an entire population from
// being born on a forest canopy, and `natural_spawn_test` pins it.
[[nodiscard]] inline bool isValidSpawnSurface(world::Block block) {
    return world::isLandEntitySupport(block) && world::emittedLight(block) < 14U;
}

// NaturalSpawner.isValidEmptySpawnBlock: a cell a mob's body may occupy.
[[nodiscard]] inline bool isValidEmptySpawnBlock(const world::World& world,
                                                 SimulationPosition position) {
    const world::Block block = world.block(position.x, position.y, position.z);
    // isCollisionShapeFullBlock. Lava is a full collision cube in this game, so
    // this is also where vanilla's `EntityType#isBlockDangerous` lands.
    if (world::isFullCube(block) && world::hasCollision(block)) {
        return false;
    }
    // isSignalSource                          (no redstone in this game)
    // BlockTags.PREVENT_MOB_SPAWNING_INSIDE   (no rails, no bubble columns)
    return !world::isFluid(block);
}

// SpawnPlacements.isSpawnPositionOk. The world border term of every vanilla
// branch is dropped rather than faked: this game has no world border, and a
// bound that is always true is not a rule.
[[nodiscard]] inline bool isSpawnPositionOk(const world::World& world,
                                            SimulationPosition position,
                                            entities::SpawnPlacement placement) {
    switch (placement) {
    case entities::SpawnPlacement::NoRestrictions:
        return true;
    case entities::SpawnPlacement::InWater:
        // The cell is water and nothing conductive caps it.
        return world::isFluid(world.block(position.x, position.y, position.z)) &&
               !isRedstoneConductorLike(world.block(position.x, position.y + 1, position.z));
    case entities::SpawnPlacement::InLava:
        return world.block(position.x, position.y, position.z) == world::Block::Lava;
    case entities::SpawnPlacement::OnGround:
        // A floor that carries a mob, then two clear cells for the body. The
        // second cell is why a one-block cave crawlspace stays empty.
        return isValidSpawnSurface(world.block(position.x, position.y - 1, position.z)) &&
               isValidEmptySpawnBlock(world, position) &&
               isValidEmptySpawnBlock(world, {position.x, position.y + 1, position.z});
    }
    return false;
}

} // namespace mc::gameplay
