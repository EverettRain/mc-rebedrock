#include "world/VoxelRaycast.hpp"

#include <cassert>

int main() {
    mc::world::World world;
    mc::world::Chunk chunk;
    chunk.setBlock(4, 4, 4, mc::world::Block::Stone);
    chunk.setBlock(1, 2, 1, mc::world::Block::Dirt);
    chunk.setBlock(7, 2, 1, mc::world::Block::Glass);
    chunk.setBlock(9, 3, 1, mc::world::Block::Water);
    chunk.setBlock(10, 3, 1, mc::world::Block::Dandelion);
    chunk.setBlock(4, 2, 6, mc::world::Block::Torch);
    chunk.setBlock(7, 2, 6, mc::world::Block::Stone);
    // A young wheat crop (raycast backdrop behind it) and a farmland strip.
    chunk.setBlock(14, 5, 14, mc::world::Block::WheatCrops);
    chunk.setBlock(15, 5, 14, mc::world::Block::Stone);
    chunk.setBlock(14, 3, 8, mc::world::Block::Farmland);
    chunk.setBlock(15, 3, 8, mc::world::Block::Stone);
    world.setChunk({0, 0}, std::move(chunk));
    world.setOrientation(14, 5, 14, mc::world::cropOrientation(0));
    world.setOrientation(14, 3, 8, mc::world::farmlandOrientation(0));

    const auto positiveX = mc::world::raycastVoxels(
        world, {0.5F, 4.5F, 4.5F}, {1.0F, 0.0F, 0.0F}, 8.0F);
    assert(positiveX.has_value());
    assert(positiveX->block == (glm::ivec3{4, 4, 4}));
    assert(positiveX->adjacent == (glm::ivec3{3, 4, 4}));
    assert(positiveX->normal == (glm::ivec3{-1, 0, 0}));

    const auto negativeZ = mc::world::raycastVoxels(
        world, {4.5F, 4.5F, 8.5F}, {0.0F, 0.0F, -1.0F}, 8.0F);
    assert(negativeZ.has_value());
    assert(negativeZ->block == (glm::ivec3{4, 4, 4}));
    assert(negativeZ->adjacent == (glm::ivec3{4, 4, 5}));

    const auto tooShort = mc::world::raycastVoxels(
        world, {0.5F, 4.5F, 4.5F}, {1.0F, 0.0F, 0.0F}, 2.0F);
    assert(!tooShort.has_value());

    const auto startsInside = mc::world::raycastVoxels(
        world, {1.25F, 2.25F, 1.25F}, {0.0F, 1.0F, 0.0F}, 1.0F);
    assert(startsInside.has_value());
    assert(startsInside->distance == 0.0F);

    const auto hitsGlass = mc::world::raycastVoxels(
        world, {3.5F, 2.5F, 1.5F}, {1.0F, 0.0F, 0.0F}, 6.0F);
    assert(hitsGlass.has_value());
    assert(hitsGlass->block == (glm::ivec3{7, 2, 1}));

    const auto ignoresWaterButHitsPlant = mc::world::raycastVoxels(
        world, {8.5F, 3.5F, 1.5F}, {1.0F, 0.0F, 0.0F}, 4.0F);
    assert(ignoresWaterButHitsPlant.has_value());
    assert(ignoresWaterButHitsPlant->block == (glm::ivec3{10, 3, 1}));
    const auto bucketHitsWater = mc::world::raycastVoxels(
        world, {8.5F, 3.5F, 1.5F}, {1.0F, 0.0F, 0.0F}, 4.0F, true);
    assert(bucketHitsWater.has_value());
    assert(bucketHitsWater->block == (glm::ivec3{9, 3, 1}));

    const auto hitsTorchMesh = mc::world::raycastVoxels(
        world, {0.5F, 2.4F, 6.5F}, {1.0F, 0.0F, 0.0F}, 8.0F);
    assert(hitsTorchMesh.has_value());
    assert(hitsTorchMesh->block == (glm::ivec3{4, 2, 6}));
    assert(hitsTorchMesh->normal == (glm::ivec3{-1, 0, 0}));
    assert(hitsTorchMesh->distance > 3.9F && hitsTorchMesh->distance < 4.0F);

    // Rays through the empty part of a torch voxel must continue to blocks
    // behind it instead of selecting a full one-block collision volume.
    const auto missesTorchWidth = mc::world::raycastVoxels(
        world, {0.5F, 2.4F, 6.75F}, {1.0F, 0.0F, 0.0F}, 8.0F);
    assert(missesTorchWidth.has_value());
    assert(missesTorchWidth->block == (glm::ivec3{7, 2, 6}));
    const auto missesTorchHeight = mc::world::raycastVoxels(
        world, {0.5F, 2.75F, 6.5F}, {1.0F, 0.0F, 0.0F}, 8.0F);
    assert(missesTorchHeight.has_value());
    assert(missesTorchHeight->block == (glm::ivec3{7, 2, 6}));

    // Crops select only up to their stage height. Age 0 wheat is 2/16 tall, so
    // a ray through the empty part of its cell passes to the stone behind it,
    // while a ray at the crop's base selects the crop itself.
    const auto overYoungWheat = mc::world::raycastVoxels(
        world, {12.5F, 5.2F, 14.5F}, {1.0F, 0.0F, 0.0F}, 6.0F);
    assert(overYoungWheat.has_value());
    assert(overYoungWheat->block == (glm::ivec3{15, 5, 14}));
    const auto hitsYoungWheat = mc::world::raycastVoxels(
        world, {12.5F, 5.05F, 14.5F}, {1.0F, 0.0F, 0.0F}, 6.0F);
    assert(hitsYoungWheat.has_value());
    assert(hitsYoungWheat->block == (glm::ivec3{14, 5, 14}));
    // Mature wheat is a full block: the same high ray now hits it.
    world.setOrientation(14, 5, 14, mc::world::cropOrientation(7));
    const auto hitsMatureWheat = mc::world::raycastVoxels(
        world, {12.5F, 5.2F, 14.5F}, {1.0F, 0.0F, 0.0F}, 6.0F);
    assert(hitsMatureWheat.has_value());
    assert(hitsMatureWheat->block == (glm::ivec3{14, 5, 14}));

    // Farmland is a 15/16 block: a ray through the top 1/16 of its cell passes
    // over it, one through the solid part selects it.
    const auto overFarmland = mc::world::raycastVoxels(
        world, {12.5F, 3.96F, 8.5F}, {1.0F, 0.0F, 0.0F}, 6.0F);
    assert(overFarmland.has_value());
    assert(overFarmland->block == (glm::ivec3{15, 3, 8}));
    const auto hitsFarmland = mc::world::raycastVoxels(
        world, {12.5F, 3.8F, 8.5F}, {1.0F, 0.0F, 0.0F}, 6.0F);
    assert(hitsFarmland.has_value());
    assert(hitsFarmland->block == (glm::ivec3{14, 3, 8}));
    return 0;
}
