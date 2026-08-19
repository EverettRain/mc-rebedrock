#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"

#include <array>
#include <cassert>
#include <cmath>

// BlockShape is the single shape source the mechanism-fix plan calls for: one
// `blockShape(state)` — the block's base geometry — that the pick ray, the
// selection outline and (filtered by hasCollision) the collision box all derive
// from, so a slab can never again be a half box to the outline and a full cube
// to the ray. This checks the base shapes and that `collisionSpan` filters them
// to exactly what collision saw before (bar the chest, whose box is now honest).
namespace {

[[nodiscard]] bool near(float a, float b) { return std::fabs(a - b) < 1.0e-6F; }

} // namespace

int main() {
    using namespace mc::world;

    // --- Empty: air has no base shape and no collision. ---
    {
        const auto shape = blockShape(BlockState{Block::Air});
        assert(shape.kind == ShapeKind::Empty);
        const auto span = collisionSpan(BlockState{Block::Air});
        assert(span.top <= span.bottom);
    }

    // --- Column: a full cube fills its whole cell, and collides as such. ---
    {
        const auto shape = blockShape(BlockState{Block::Stone});
        assert(shape.kind == ShapeKind::Column);
        assert(near(shape.bottom, 0.0F) && near(shape.top, 1.0F));
        const auto span = collisionSpan(BlockState{Block::Stone});
        assert(near(span.bottom, 0.0F) && near(span.top, 1.0F));
    }

    // --- Column: a slab is the half its SlabType names, a double a full cube;
    // collision follows because a slab collides. ---
    {
        const BlockState bottom = BlockState{Block::OakSlab}.withSlabPortion(SlabPortion::Bottom);
        const BlockState top = BlockState{Block::OakSlab}.withSlabPortion(SlabPortion::Top);
        const BlockState twin = BlockState{Block::OakSlab}.withSlabPortion(SlabPortion::Double);

        assert(near(blockShape(bottom).bottom, 0.0F) && near(blockShape(bottom).top, 0.5F));
        assert(near(blockShape(top).bottom, 0.5F) && near(blockShape(top).top, 1.0F));
        assert(near(blockShape(twin).bottom, 0.0F) && near(blockShape(twin).top, 1.0F));
        assert(near(collisionSpan(bottom).bottom, 0.0F) && near(collisionSpan(bottom).top, 0.5F));
    }

    // --- Column: farmland keeps the vanilla 15/16 box, and collides to it. ---
    {
        const auto shape = blockShape(BlockState{Block::Farmland});
        assert(shape.kind == ShapeKind::Column);
        assert(near(shape.top, kFarmlandModelHeight));
        assert(near(collisionSpan(BlockState{Block::Farmland}).top, kFarmlandModelHeight));
    }

    // --- Boxes, no collision: a torch and a flower have a slim base shape that
    // the ray and outline see, but they never block movement. ---
    {
        const auto torch = blockShape(BlockState{Block::Torch});
        assert(torch.kind == ShapeKind::Boxes && torch.boxes.size() == 1);
        assert(near(torch.boxes.front().minX, 0.4375F) && near(torch.boxes.front().maxY, 0.625F));
        assert(collisionSpan(BlockState{Block::Torch}).top <= 0.0F); // no collision

        const auto flower = blockShape(BlockState{Block::Dandelion});
        assert(flower.kind == ShapeKind::Boxes && flower.boxes.size() == 1);
        assert(near(flower.boxes.front().minX, 0.1F) && near(flower.boxes.front().maxY, 0.8F));
        assert(collisionSpan(BlockState{Block::Dandelion}).top <= 0.0F);
    }

    // --- Boxes, wall torch: the box leans by FACING, and pokes into the wall
    // neighbour exactly as the old torchSelectionBox produced. ---
    {
        const auto north = blockShape(BlockState{Block::WallTorch, BlockOrientation::North});
        const auto west = blockShape(BlockState{Block::WallTorch, BlockOrientation::West});
        assert(north.kind == ShapeKind::Boxes && north.boxes.size() == 1);
        assert(near(north.boxes.front().maxZ, 1.056284F)); // leans out past the cell
        assert(near(west.boxes.front().maxX, 1.056284F));
    }

    // --- Boxes with collision: the chest is its 14/16 box now, so collision is
    // its real height (0.875) rather than a phantom full cube. ---
    {
        const auto chest = blockShape(BlockState{Block::Chest});
        assert(chest.kind == ShapeKind::Boxes && chest.boxes.size() == 1);
        const auto span = collisionSpan(BlockState{Block::Chest});
        assert(near(span.bottom, 0.0F) && near(span.top, 0.875F));
    }

    // --- collisionSpan is the shape's vertical extent, filtered by collision. ---
    {
        for (const Block block : {Block::Stone, Block::Farmland, Block::OakSlab}) {
            const auto span = collisionSpan(BlockState{block});
            const auto direct = verticalSpanOf(blockShape(BlockState{block}));
            assert(near(span.bottom, direct.bottom) && near(span.top, direct.top));
        }
    }

    // --- verticalSpanOf unions a Boxes shape's y ranges. The first box is
    // neither lowest nor tallest, so both ends must come from the loop. ---
    {
        static constexpr std::array<ShapeBox, 2> kBoxes{
            ShapeBox{0.375F, 0.3F, 0.375F, 0.625F, 0.6F, 0.625F}, // a mid post
            ShapeBox{0.0F, 0.0F, 0.0F, 1.0F, 0.9F, 1.0F},         // a low, tall floor
        };
        const BlockShape shape{ShapeKind::Boxes, 0.0F, 0.0F, kBoxes};
        const auto span = verticalSpanOf(shape);
        assert(near(span.bottom, 0.0F) && near(span.top, 0.9F));

        const BlockShape none{ShapeKind::Boxes, 0.0F, 0.0F, {}};
        assert(verticalSpanOf(none).top <= verticalSpanOf(none).bottom);
    }

    // --- shapeOverlaps: the box-set collision the fence/stair walk needs. A
    // Column ignores XZ (its footprint is full, the caller iterates the cells);
    // a Boxes post only blocks where its box actually is. Cell origin (10,4,10). ---
    {
        const float ox = 10.0F, oy = 4.0F, oz = 10.0F;
        const BlockShape column{ShapeKind::Column, 0.0F, 0.5F, {}};
        // Column: overlaps on Y span regardless of XZ, empty above its top.
        assert(shapeOverlaps(column, ox, oy, oz, ox, oy + 0.2F, oz, ox + 1, oy + 0.3F, oz + 1));
        assert(!shapeOverlaps(column, ox, oy, oz, ox, oy + 0.6F, oz, ox + 1, oy + 0.9F, oz + 1));

        // A centred fence-post box: 4/16..12/16 in X and Z, full height.
        static constexpr std::array<ShapeBox, 1> kPost{
            ShapeBox{0.25F, 0.0F, 0.25F, 0.75F, 1.0F, 0.75F}};
        const BlockShape post{ShapeKind::Boxes, 0.0F, 0.0F, kPost};
        // A box over the cell centre hits the post.
        assert(shapeOverlaps(post, ox, oy, oz, ox + 0.4F, oy + 0.2F, oz + 0.4F, ox + 0.6F,
                             oy + 0.8F, oz + 0.6F));
        // A box tucked in the cell corner misses it (the gap a fence leaves).
        assert(!shapeOverlaps(post, ox, oy, oz, ox + 0.02F, oy + 0.2F, oz + 0.02F, ox + 0.2F,
                              oy + 0.8F, oz + 0.2F));
        // Empty never collides.
        assert(!shapeOverlaps(BlockShape{}, ox, oy, oz, ox, oy, oz, ox + 1, oy + 1, oz + 1));
    }

    return 0;
}
