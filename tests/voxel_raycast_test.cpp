#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/VoxelRaycast.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>

int main() {
    mc::world::World world;
    mc::world::Chunk chunk;
    chunk.setBlock(4, 4, 4, mc::world::Block::Stone);
    chunk.setBlock(1, 2, 1, mc::world::Block::Dirt);
    chunk.setBlock(7, 2, 1, mc::world::Block::Glass);
    chunk.setBlock(9, 3, 1, mc::world::Block::Water);
    chunk.setBlock(10, 3, 1, mc::world::Block::Dandelion);
    // A flowing-water cell (level 3) behind which a stone block waits.
    chunk.setBlock(12, 3, 1, mc::world::Block::Water);
    chunk.setBlock(13, 3, 1, mc::world::Block::Stone);
    chunk.setBlock(4, 2, 6, mc::world::Block::Torch);
    chunk.setBlock(7, 2, 6, mc::world::Block::Stone);
    // A young wheat crop (raycast backdrop behind it) and a farmland strip.
    chunk.setBlock(14, 5, 14, mc::world::Block::WheatCrops);
    chunk.setBlock(15, 5, 14, mc::world::Block::Stone);
    chunk.setBlock(14, 3, 8, mc::world::Block::Farmland);
    chunk.setBlock(15, 3, 8, mc::world::Block::Stone);
    // A bottom slab and a top slab, each with a stone backdrop, to prove the pick
    // ray hits the half box that is drawn rather than the whole cell.
    chunk.setBlock(4, 7, 1, mc::world::Block::OakSlab);            // bottom (default)
    chunk.setBlock(7, 7, 1, mc::world::Block::Stone);
    chunk.setBlock(4, 8, 3, mc::world::Block::OakSlab);            // set to top below
    chunk.setBlock(7, 8, 3, mc::world::Block::Stone);
    world.setChunk({0, 0}, std::move(chunk));
    world.setState(4, 8, 3,
                   mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                       mc::world::SlabPortion::Top));
    world.setFluidLevel(12, 3, 1, 3U);
    // Age 0 and moisture 0 are each block's default state, so both cells are
    // already right; naming them keeps the fixture explicit about the stage the
    // raycast shapes below are measured against.
    world.setState(14, 5, 14, mc::world::BlockState{mc::world::Block::WheatCrops}.withAge(0));
    world.setState(14, 3, 8, mc::world::BlockState{mc::world::Block::Farmland}.withMoisture(0));

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
    // A bucket ray stops only at a still source: the flowing cell at (12,3,1)
    // is walked past, so the stone behind it is reachable.
    const auto bucketWalksFlowingWater = mc::world::raycastVoxels(
        world, {11.5F, 3.5F, 1.5F}, {1.0F, 0.0F, 0.0F}, 3.0F, true);
    assert(bucketWalksFlowingWater.has_value());
    assert(bucketWalksFlowingWater->block == (glm::ivec3{13, 3, 1}));

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
    world.setState(14, 5, 14, mc::world::BlockState{mc::world::Block::WheatCrops}.withAge(7));
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

    // The reported bug: a bottom slab fills y[0,0.5]. A ray through the empty
    // upper half must pass to the block behind, not select the slab as a full
    // cube; a ray through the solid lower half selects the slab.
    const auto overBottomSlab = mc::world::raycastVoxels(
        world, {0.5F, 7.75F, 1.5F}, {1.0F, 0.0F, 0.0F}, 8.0F);
    assert(overBottomSlab.has_value());
    assert(overBottomSlab->block == (glm::ivec3{7, 7, 1}));
    const auto hitsBottomSlab = mc::world::raycastVoxels(
        world, {0.5F, 7.25F, 1.5F}, {1.0F, 0.0F, 0.0F}, 8.0F);
    assert(hitsBottomSlab.has_value());
    assert(hitsBottomSlab->block == (glm::ivec3{4, 7, 1}));

    // Looking straight down onto a bottom slab selects it, hitting the top face
    // at y=7.5 (its half height), not the cell top at y=8.
    const auto ontoSlabTop = mc::world::raycastVoxels(
        world, {4.5F, 9.0F, 1.5F}, {0.0F, -1.0F, 0.0F}, 8.0F);
    assert(ontoSlabTop.has_value());
    assert(ontoSlabTop->block == (glm::ivec3{4, 7, 1}));
    assert(ontoSlabTop->normal == (glm::ivec3{0, 1, 0}));
    assert(ontoSlabTop->distance > 1.49F && ontoSlabTop->distance < 1.51F);

    // A top slab fills y[0.5,1]. The empty lower half passes through; the solid
    // upper half selects it.
    const auto underTopSlab = mc::world::raycastVoxels(
        world, {0.5F, 8.25F, 3.5F}, {1.0F, 0.0F, 0.0F}, 8.0F);
    assert(underTopSlab.has_value());
    assert(underTopSlab->block == (glm::ivec3{7, 8, 3}));
    const auto hitsTopSlab = mc::world::raycastVoxels(
        world, {0.5F, 8.75F, 3.5F}, {1.0F, 0.0F, 0.0F}, 8.0F);
    assert(hitsTopSlab.has_value());
    assert(hitsTopSlab->block == (glm::ivec3{4, 8, 3}));

    // --- RN-10f / audit R17: the selection outline is drawn BOX BY BOX. ---
    //
    // It used to be the shape's bounding box, so a stair, a wall and a fence gate
    // were all outlined by a full cube — a marker around air the ray does not hit
    // and the player cannot stand on. The counts below are the shapes themselves:
    // a straight stair is two boxes and an inner-corner one three, a wall is its
    // post plus one arm per connection, a fence gate is one, and a full cube is
    // one. Read off blockShape, which is the same source the ray tests.
    {
        using mc::world::Block;
        using mc::world::BlockState;
        using mc::world::blockShape;
        mc::world::World outlineWorld;
        mc::world::Chunk outlineChunk;
        const auto boxesAt = [&](int x, int y, int z, BlockState state) {
            outlineChunk.setState(x, y, z, state);
            outlineWorld.setChunk({0, 0}, outlineChunk);
            return mc::world::blockSelectionBoxes(outlineWorld, {x, y, z});
        };

        // Cube: one box, the whole cell.
        {
            const auto boxes = boxesAt(2, 20, 2, BlockState{Block::Stone});
            assert(boxes.count == 1U);
            assert(boxes.boxes[0].minimum == glm::vec3{0.0F});
            assert(boxes.boxes[0].maximum == glm::vec3{1.0F});
        }
        // Straight stair: two. Inner corner: three. Both equal the shape.
        {
            const BlockState straight{Block::OakStairs};
            const auto boxes = boxesAt(3, 20, 2, straight);
            assert(blockShape(straight).boxes.size() == 2U);
            assert(boxes.count == 2U);
            const auto inner = BlockState{Block::OakStairs}.withStairShape(
                mc::world::StairShape::InnerRight);
            const auto innerBoxes = boxesAt(4, 20, 2, inner);
            assert(blockShape(inner).boxes.size() == 3U);
            assert(innerBoxes.count == 3U);
            // Not the bounding box: no single outline may span the whole cell,
            // which is exactly what the old code drew.
            bool anySpansCell = false;
            for (std::size_t i = 0; i < innerBoxes.count; ++i) {
                anySpansCell = anySpansCell || (innerBoxes.boxes[i].minimum == glm::vec3{0.0F} &&
                                                innerBoxes.boxes[i].maximum == glm::vec3{1.0F});
            }
            assert(!anySpansCell);
        }
        // Fence gate: one box, and it is the gate's slab, not the cell.
        {
            const auto boxes = boxesAt(5, 20, 2, BlockState{Block::OakFenceGate});
            assert(boxes.count == 1U);
            assert(boxes.boxes[0].maximum.z < 0.7F);
        }
        // A pressure plate is a Column: one synthesised box at its height.
        {
            const auto boxes = boxesAt(6, 20, 2, BlockState{Block::StonePressurePlate});
            assert(boxes.count == 1U);
            assert(boxes.boxes[0].maximum.y < 0.2F);
        }
        // Air outlines nothing at all.
        {
            const auto boxes = boxesAt(7, 20, 2, BlockState{Block::Air});
            assert(boxes.count == 0U);
        }
    }

    // The cap is not a guess: no state of any block in the roster has more boxes
    // than kMaxSelectionBoxes, so nothing is ever silently half-outlined. A wall
    // with four arms is the widest, at five.
    {
        std::size_t widest = 0;
        for (std::uint32_t id = 0; id < mc::world::kBlockStateCount; ++id) {
            const auto shape = mc::world::blockShape(mc::world::BlockState::fromRawId(id));
            if (shape.kind == mc::world::ShapeKind::Boxes) {
                widest = std::max(widest, shape.boxes.size());
            }
        }
        assert(widest > 1U); // the sweep actually saw multi-box shapes
        assert(widest <= mc::world::kMaxSelectionBoxes);
    }
    return 0;
}
